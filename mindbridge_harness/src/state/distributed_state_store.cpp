#include "mindbridge/state/distributed_state_store.hpp"

#include <sqlite3.h>
#include <cstring>

#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
extern "C" {
#include <mysql/mysql.h>
}
#endif

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mindbridge {
namespace state {
namespace {

using StmtPtr = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void exec(sqlite3* db, const std::string& sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : "sqlite exec failed";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

StmtPtr prepare(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
    return StmtPtr(raw, sqlite3_finalize);
}

void bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

std::string col_text(sqlite3_stmt* stmt, int index) {
    const unsigned char* text = sqlite3_column_text(stmt, index);
    return text ? reinterpret_cast<const char*>(text) : "";
}

void step_done(sqlite3* db, sqlite3_stmt* stmt) {
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

nlohmann::json parse_json_or_object(const std::string& raw) {
    if (raw.empty()) {
        return nlohmann::json::object();
    }
    try {
        return nlohmann::json::parse(raw);
    } catch (...) {
        return nlohmann::json{{"raw", raw}};
    }
}

std::vector<std::string> parse_targets(const std::string& raw) {
    std::vector<std::string> out;
    const auto json = parse_json_or_object(raw);
    if (!json.is_array()) {
        return out;
    }
    for (const auto& item : json) {
        if (item.is_string()) {
            out.push_back(item.get<std::string>());
        }
    }
    return out;
}

bool targets_contain(const std::vector<std::string>& targets, const std::string& node_id) {
    if (targets.empty()) {
        return true;
    }
    for (const auto& one : targets) {
        if (one == node_id) {
            return true;
        }
    }
    return false;
}

StateRecord record_from_stmt(sqlite3_stmt* stmt) {
    StateRecord record;
    record.user_id = col_text(stmt, 0);
    record.conversation_id = col_text(stmt, 1);
    record.namespace_name = col_text(stmt, 2);
    record.key = col_text(stmt, 3);
    record.version = sqlite3_column_int64(stmt, 4);
    record.risk_level = col_text(stmt, 5);
    record.payload = parse_json_or_object(col_text(stmt, 6));
    record.updated_at = sqlite3_column_int64(stmt, 7);
    return record;
}

StateChange change_from_stmt(sqlite3_stmt* stmt) {
    StateChange change;
    change.change_id = sqlite3_column_int64(stmt, 0);
    change.user_id = col_text(stmt, 1);
    change.conversation_id = col_text(stmt, 2);
    change.namespace_name = col_text(stmt, 3);
    change.key = col_text(stmt, 4);
    change.operation = col_text(stmt, 5);
    change.version = sqlite3_column_int64(stmt, 6);
    change.risk_level = col_text(stmt, 7);
    change.payload = parse_json_or_object(col_text(stmt, 8));
    change.source_node = col_text(stmt, 9);
    change.target_nodes = parse_targets(col_text(stmt, 10));
    change.created_at = sqlite3_column_int64(stmt, 11);
    return change;
}

std::string conversation_key(std::int64_t timestamp, const std::string& role) {
    static std::int64_t local_counter = 0;
    std::ostringstream out;
    out << "turn_" << timestamp << "_" << ++local_counter << "_" << role;
    return out.str();
}

std::string env_or(const char* key, const std::string& fallback = "") {
    const char* value = std::getenv(key);
    if (!value || std::string(value).empty()) {
        return fallback;
    }
    return value;
}

bool is_conversation_namespace(const std::string& namespace_name) {
    return namespace_name == "chat_memory" ||
           namespace_name == "consult_memory" ||
           namespace_name == "risk_memory";
}

nlohmann::json turn_from_record(const StateRecord& record) {
    const nlohmann::json payload = record.payload.is_object() ? record.payload : nlohmann::json::object();
    return {{"role", payload.value("role", "")},
            {"content", payload.value("content", "")},
            {"run_id", payload.value("run_id", "")},
            {"created_at", payload.value("created_at", record.updated_at)},
            {"risk_level", record.risk_level},
            {"namespace_name", record.namespace_name},
            {"state_key", record.key}};
}

}  // namespace

struct DistributedStateStore::Impl {
    explicit Impl(const std::string& path) {
        backend = env_or("MINDBRIDGE_STATE_BACKEND", "sqlite");
        if (backend == "mysql") {
#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
            mysql_host = env_or("MINDBRIDGE_MYSQL_HOST", "127.0.0.1");
            mysql_port = std::stoi(env_or("MINDBRIDGE_MYSQL_PORT", "3307"));
            mysql_user = env_or("MINDBRIDGE_MYSQL_USER", "root");
            mysql_password = env_or("MINDBRIDGE_MYSQL_PASSWORD");
            mysql_database = env_or("MINDBRIDGE_MYSQL_DATABASE", "mindbridge");
            return;
#else
            throw std::runtime_error("MINDBRIDGE_STATE_BACKEND=mysql requires mysqlclient support");
#endif
        }
        const auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }
        if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
            std::string error = db ? sqlite3_errmsg(db) : "sqlite open failed";
            if (db) {
                sqlite3_close(db);
                db = nullptr;
            }
            throw std::runtime_error(error);
        }
    }

    ~Impl() {
        if (db) {
            sqlite3_close(db);
        }
    }

    sqlite3* db{nullptr};
    std::string backend{"sqlite"};

#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    std::string mysql_host{"127.0.0.1"};
    int mysql_port{3306};
    std::string mysql_user{"root"};
    std::string mysql_password;
    std::string mysql_database{"mindbridge"};

    struct MysqlHandle {
        MYSQL* conn{nullptr};
        ~MysqlHandle() {
            if (conn) {
                mysql_close(conn);
            }
        }
    };

    bool mysql_mode() const { return backend == "mysql"; }

    MysqlHandle mysql_connect() const {
        MysqlHandle handle;
        handle.conn = mysql_init(nullptr);
        if (!handle.conn) {
            throw std::runtime_error("mysql_init failed");
        }
        mysql_options(handle.conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
        if (!mysql_real_connect(handle.conn,
                                mysql_host.c_str(),
                                mysql_user.c_str(),
                                mysql_password.c_str(),
                                mysql_database.c_str(),
                                mysql_port,
                                nullptr,
                                0)) {
            throw std::runtime_error(std::string("mysql connect failed: ") + mysql_error(handle.conn));
        }
        return handle;
    }

    std::string escape(MYSQL* conn, const std::string& value) const {
        std::string out(value.size() * 2 + 1, '\0');
        const unsigned long n = mysql_real_escape_string(
            conn, out.data(), value.c_str(), static_cast<unsigned long>(value.size()));
        out.resize(n);
        return out;
    }

    void mysql_exec(MYSQL* conn, const std::string& sql) const {
        if (mysql_query(conn, sql.c_str()) != 0) {
            throw std::runtime_error(std::string("mysql query failed: ") + mysql_error(conn));
        }
    }
#else
    bool mysql_mode() const { return false; }
#endif
};

DistributedStateStore::DistributedStateStore(std::string db_path, std::string node_id)
    : db_path_(std::move(db_path)), node_id_(std::move(node_id)), impl_(new Impl(db_path_)) {
    // Initialize L1 caches with env-var-configurable settings
    auto record_cfg = agent_rpc::mcp::rag::CaffeineCacheConfig::from_env();
    // Override with state-store-specific env vars if set
    if (const char* v = std::getenv("MINDBRIDGE_STATE_CACHE_MAX_SIZE")) {
        record_cfg.max_size = static_cast<size_t>(std::atol(v));
    }
    if (const char* v = std::getenv("MINDBRIDGE_STATE_CACHE_ENABLED")) {
        record_cfg.enabled = (std::strcmp(v, "true") == 0 || std::strcmp(v, "1") == 0);
    }
    record_cfg.write_ttl_seconds = 60;
    record_cfg.refresh_seconds = 0;  // no refresh for individual records

    auto history_cfg = record_cfg;
    history_cfg.refresh_seconds = 10;  // refresh conversation history after 10s

    record_cache_ = std::make_unique<RecordCache>(record_cfg);
    history_cache_ = std::make_unique<HistoryCache>(history_cfg);
}

DistributedStateStore::~DistributedStateStore() {
    delete impl_;
}

void DistributedStateStore::initialize() {
#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    if (impl_->mysql_mode()) {
        auto mysql = impl_->mysql_connect();
        impl_->mysql_exec(mysql.conn,
                          "CREATE TABLE IF NOT EXISTS mb_state_records ("
                          "user_id VARCHAR(128) NOT NULL,"
                          "conversation_id VARCHAR(256) NOT NULL,"
                          "namespace_name VARCHAR(128) NOT NULL,"
                          "state_key VARCHAR(256) NOT NULL,"
                          "version BIGINT NOT NULL,"
                          "risk_level VARCHAR(32) NOT NULL DEFAULT 'low',"
                          "payload JSON NOT NULL,"
                          "updated_at BIGINT NOT NULL,"
                          "PRIMARY KEY(user_id, conversation_id, namespace_name, state_key),"
                          "KEY idx_mb_state_namespace(user_id, conversation_id, namespace_name)) "
                          "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");
        impl_->mysql_exec(mysql.conn,
                          "CREATE TABLE IF NOT EXISTS mb_state_changes ("
                          "change_id BIGINT NOT NULL AUTO_INCREMENT,"
                          "user_id VARCHAR(128) NOT NULL,"
                          "conversation_id VARCHAR(256) NOT NULL,"
                          "namespace_name VARCHAR(128) NOT NULL,"
                          "state_key VARCHAR(256) NOT NULL,"
                          "operation VARCHAR(32) NOT NULL,"
                          "version BIGINT NOT NULL,"
                          "risk_level VARCHAR(32) NOT NULL DEFAULT 'low',"
                          "payload JSON NOT NULL,"
                          "source_node VARCHAR(128) NOT NULL,"
                          "target_nodes JSON NOT NULL,"
                          "created_at BIGINT NOT NULL,"
                          "PRIMARY KEY(change_id),"
                          "KEY idx_mb_changes_after(change_id),"
                          "KEY idx_mb_changes_scope(user_id, conversation_id, namespace_name)) "
                          "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");
        impl_->mysql_exec(mysql.conn,
                          "CREATE TABLE IF NOT EXISTS mb_replica_progress ("
                          "node_id VARCHAR(128) NOT NULL,"
                          "last_applied_change_id BIGINT NOT NULL DEFAULT 0,"
                          "applied_count BIGINT NOT NULL DEFAULT 0,"
                          "skipped_count BIGINT NOT NULL DEFAULT 0,"
                          "updated_at BIGINT NOT NULL,"
                          "PRIMARY KEY(node_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");
        return;
    }
#endif
    exec(impl_->db, "PRAGMA journal_mode=WAL;");
    exec(impl_->db, "PRAGMA foreign_keys=ON;");
    exec(impl_->db,
         "CREATE TABLE IF NOT EXISTS state_records ("
         "user_id TEXT NOT NULL,"
         "conversation_id TEXT NOT NULL,"
         "namespace_name TEXT NOT NULL,"
         "state_key TEXT NOT NULL,"
         "version INTEGER NOT NULL,"
         "risk_level TEXT NOT NULL,"
         "payload TEXT NOT NULL,"
         "updated_at INTEGER NOT NULL,"
         "PRIMARY KEY(user_id, conversation_id, namespace_name, state_key));");
    exec(impl_->db,
         "CREATE TABLE IF NOT EXISTS state_changes ("
         "change_id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "user_id TEXT NOT NULL,"
         "conversation_id TEXT NOT NULL,"
         "namespace_name TEXT NOT NULL,"
         "state_key TEXT NOT NULL,"
         "operation TEXT NOT NULL,"
         "version INTEGER NOT NULL,"
         "risk_level TEXT NOT NULL,"
         "payload TEXT NOT NULL,"
         "source_node TEXT NOT NULL,"
         "target_nodes TEXT NOT NULL,"
         "created_at INTEGER NOT NULL);");
    exec(impl_->db,
         "CREATE TABLE IF NOT EXISTS replica_progress ("
         "node_id TEXT PRIMARY KEY,"
         "last_applied_change_id INTEGER NOT NULL,"
         "applied_count INTEGER NOT NULL,"
         "skipped_count INTEGER NOT NULL,"
         "updated_at INTEGER NOT NULL);");
}

// --- Cache helper methods ---

std::string DistributedStateStore::make_record_cache_key(
    const std::string& user_id, const std::string& conversation_id,
    const std::string& namespace_name, const std::string& key) {
    return user_id + ":" + conversation_id + ":" + namespace_name + ":" + key;
}

std::string DistributedStateStore::make_history_cache_key(
    const std::string& user_id, const std::string& conversation_id) {
    return user_id + ":" + conversation_id;
}

void DistributedStateStore::invalidate_record_cache(
    const std::string& user_id, const std::string& conversation_id,
    const std::string& namespace_name, const std::string& key) {
    if (record_cache_) {
        record_cache_->invalidate(make_record_cache_key(user_id, conversation_id, namespace_name, key));
    }
}

void DistributedStateStore::invalidate_history_cache(
    const std::string& user_id, const std::string& conversation_id) {
    if (history_cache_) {
        history_cache_->invalidate(make_history_cache_key(user_id, conversation_id));
    }
}

std::optional<StateRecord> DistributedStateStore::get_record(
    const std::string& user_id,
    const std::string& conversation_id,
    const std::string& namespace_name,
    const std::string& key) const {
    // L1 cache lookup
    if (record_cache_) {
        auto cache_key = make_record_cache_key(user_id, conversation_id, namespace_name, key);
        auto cached = record_cache_->get_if_present(cache_key);
        if (cached.has_value()) {
            return cached.value();
        }
    }

#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    if (impl_->mysql_mode()) {
        auto mysql = impl_->mysql_connect();
        auto esc = [&](const std::string& value) { return impl_->escape(mysql.conn, value); };
        const std::string sql =
            "SELECT user_id,conversation_id,namespace_name,state_key,version,risk_level,payload,updated_at "
            "FROM mb_state_records WHERE user_id='" + esc(user_id) + "' AND conversation_id='" +
            esc(conversation_id) + "' AND namespace_name='" + esc(namespace_name) + "' AND state_key='" +
            esc(key) + "' LIMIT 1";
        if (mysql_query(mysql.conn, sql.c_str()) != 0) {
            throw std::runtime_error(std::string("mysql query failed: ") + mysql_error(mysql.conn));
        }
        std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> res(mysql_store_result(mysql.conn),
                                                                     mysql_free_result);
        if (!res) {
            return std::nullopt;
        }
        MYSQL_ROW row = mysql_fetch_row(res.get());
        if (!row) {
            return std::nullopt;
        }
        StateRecord record;
        record.user_id = row[0] ? row[0] : "";
        record.conversation_id = row[1] ? row[1] : "";
        record.namespace_name = row[2] ? row[2] : "";
        record.key = row[3] ? row[3] : "";
        record.version = row[4] ? std::stoll(row[4]) : 0;
        record.risk_level = row[5] ? row[5] : "";
        record.payload = parse_json_or_object(row[6] ? row[6] : "");
        record.updated_at = row[7] ? std::stoll(row[7]) : 0;
        return record;
    }
#endif
    auto stmt = prepare(impl_->db,
                        "SELECT user_id, conversation_id, namespace_name, state_key, version, risk_level, "
                        "payload, updated_at FROM state_records "
                        "WHERE user_id=? AND conversation_id=? AND namespace_name=? AND state_key=?;");
    bind_text(stmt.get(), 1, user_id);
    bind_text(stmt.get(), 2, conversation_id);
    bind_text(stmt.get(), 3, namespace_name);
    bind_text(stmt.get(), 4, key);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return std::nullopt;
    }
    auto result = record_from_stmt(stmt.get());
    // Fill L1 cache
    if (record_cache_) {
        record_cache_->put(make_record_cache_key(user_id, conversation_id, namespace_name, key), result);
    }
    return std::optional<StateRecord>(std::move(result));
}

std::vector<StateRecord> DistributedStateStore::list_records(
    const std::string& user_id,
    const std::string& conversation_id,
    const std::string& namespace_name) const {
    std::vector<StateRecord> out;
#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    if (impl_->mysql_mode()) {
        auto mysql = impl_->mysql_connect();
        auto esc = [&](const std::string& value) { return impl_->escape(mysql.conn, value); };
        const std::string sql =
            "SELECT user_id,conversation_id,namespace_name,state_key,version,risk_level,payload,updated_at "
            "FROM mb_state_records WHERE user_id='" + esc(user_id) + "' AND conversation_id='" +
            esc(conversation_id) + "' AND namespace_name='" + esc(namespace_name) +
            "' ORDER BY state_key ASC";
        if (mysql_query(mysql.conn, sql.c_str()) != 0) {
            throw std::runtime_error(std::string("mysql query failed: ") + mysql_error(mysql.conn));
        }
        std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> res(mysql_store_result(mysql.conn),
                                                                     mysql_free_result);
        if (!res) {
            return out;
        }
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res.get()))) {
            StateRecord record;
            record.user_id = row[0] ? row[0] : "";
            record.conversation_id = row[1] ? row[1] : "";
            record.namespace_name = row[2] ? row[2] : "";
            record.key = row[3] ? row[3] : "";
            record.version = row[4] ? std::stoll(row[4]) : 0;
            record.risk_level = row[5] ? row[5] : "";
            record.payload = parse_json_or_object(row[6] ? row[6] : "");
            record.updated_at = row[7] ? std::stoll(row[7]) : 0;
            out.push_back(std::move(record));
        }
        return out;
    }
#endif
    auto stmt = prepare(impl_->db,
                        "SELECT user_id, conversation_id, namespace_name, state_key, version, risk_level, "
                        "payload, updated_at FROM state_records "
                        "WHERE user_id=? AND conversation_id=? AND namespace_name=? "
                        "ORDER BY state_key ASC;");
    bind_text(stmt.get(), 1, user_id);
    bind_text(stmt.get(), 2, conversation_id);
    bind_text(stmt.get(), 3, namespace_name);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        out.push_back(record_from_stmt(stmt.get()));
    }
    return out;
}

nlohmann::json DistributedStateStore::list_conversations(const std::string& user_id) const {
    nlohmann::json out = nlohmann::json::array();
#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    if (impl_->mysql_mode()) {
        auto mysql = impl_->mysql_connect();
        auto esc = [&](const std::string& value) { return impl_->escape(mysql.conn, value); };
        const std::string sql =
            "SELECT conversation_id, MAX(updated_at) AS updated_at, COUNT(*) AS turns "
            "FROM mb_state_records WHERE user_id='" + esc(user_id.empty() ? "anonymous" : user_id) +
            "' AND namespace_name IN ('chat_memory','consult_memory','risk_memory') "
            "GROUP BY conversation_id ORDER BY updated_at DESC";
        if (mysql_query(mysql.conn, sql.c_str()) != 0) {
            throw std::runtime_error(std::string("mysql query failed: ") + mysql_error(mysql.conn));
        }
        std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> res(mysql_store_result(mysql.conn),
                                                                     mysql_free_result);
        if (!res) {
            return out;
        }
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res.get()))) {
            out.push_back({{"conversation_id", row[0] ? row[0] : ""},
                           {"updated_at", row[1] ? std::stoll(row[1]) : 0},
                           {"turns", row[2] ? std::stoll(row[2]) : 0}});
        }
        return out;
    }
#endif
    auto stmt = prepare(impl_->db,
                        "SELECT conversation_id, MAX(updated_at) AS updated_at, COUNT(*) AS turns "
                        "FROM state_records WHERE user_id=? "
                        "AND namespace_name IN ('chat_memory','consult_memory','risk_memory') "
                        "GROUP BY conversation_id ORDER BY updated_at DESC;");
    bind_text(stmt.get(), 1, user_id.empty() ? "anonymous" : user_id);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        out.push_back({{"conversation_id", col_text(stmt.get(), 0)},
                       {"updated_at", sqlite3_column_int64(stmt.get(), 1)},
                       {"turns", sqlite3_column_int64(stmt.get(), 2)}});
    }
    return out;
}

nlohmann::json DistributedStateStore::conversation_history(
    const std::string& user_id,
    const std::string& conversation_id) const {
    // L1 cache lookup
    if (history_cache_) {
        auto cache_key = make_history_cache_key(
            user_id.empty() ? "anonymous" : user_id,
            conversation_id.empty() ? "default" : conversation_id);
        auto cached = history_cache_->get(cache_key);
        if (cached.has_value()) {
            return cached.value();
        }
    }

    nlohmann::json out = nlohmann::json::array();
#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    if (impl_->mysql_mode()) {
        auto mysql = impl_->mysql_connect();
        auto esc = [&](const std::string& value) { return impl_->escape(mysql.conn, value); };
        const std::string sql =
            "SELECT user_id,conversation_id,namespace_name,state_key,version,risk_level,payload,updated_at "
            "FROM mb_state_records WHERE user_id='" + esc(user_id.empty() ? "anonymous" : user_id) +
            "' AND conversation_id='" + esc(conversation_id.empty() ? "default" : conversation_id) +
            "' AND namespace_name IN ('chat_memory','consult_memory','risk_memory') "
            "ORDER BY state_key ASC";
        if (mysql_query(mysql.conn, sql.c_str()) != 0) {
            throw std::runtime_error(std::string("mysql query failed: ") + mysql_error(mysql.conn));
        }
        std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> res(mysql_store_result(mysql.conn),
                                                                     mysql_free_result);
        if (!res) {
            return out;
        }
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res.get()))) {
            StateRecord record;
            record.user_id = row[0] ? row[0] : "";
            record.conversation_id = row[1] ? row[1] : "";
            record.namespace_name = row[2] ? row[2] : "";
            record.key = row[3] ? row[3] : "";
            record.version = row[4] ? std::stoll(row[4]) : 0;
            record.risk_level = row[5] ? row[5] : "";
            record.payload = parse_json_or_object(row[6] ? row[6] : "");
            record.updated_at = row[7] ? std::stoll(row[7]) : 0;
            if (is_conversation_namespace(record.namespace_name)) {
                out.push_back(turn_from_record(record));
            }
        }
        return out;
    }
#endif
    auto stmt = prepare(impl_->db,
                        "SELECT user_id, conversation_id, namespace_name, state_key, version, risk_level, "
                        "payload, updated_at FROM state_records "
                        "WHERE user_id=? AND conversation_id=? "
                        "AND namespace_name IN ('chat_memory','consult_memory','risk_memory') "
                        "ORDER BY state_key ASC;");
    bind_text(stmt.get(), 1, user_id.empty() ? "anonymous" : user_id);
    bind_text(stmt.get(), 2, conversation_id.empty() ? "default" : conversation_id);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const auto record = record_from_stmt(stmt.get());
        if (is_conversation_namespace(record.namespace_name)) {
            out.push_back(turn_from_record(record));
        }
    }
    // Fill L1 cache
    if (history_cache_) {
        auto cache_key = make_history_cache_key(
            user_id.empty() ? "anonymous" : user_id,
            conversation_id.empty() ? "default" : conversation_id);
        history_cache_->put(cache_key, out);
    }
    return out;
}

std::int64_t DistributedStateStore::upsert_record(StateRecord record,
                                                  const std::vector<std::string>& target_nodes) {
    if (record.user_id.empty() || record.conversation_id.empty() ||
        record.namespace_name.empty() || record.key.empty()) {
        throw std::runtime_error("state record requires user_id/conversation_id/namespace/key");
    }
    const auto existing = get_record(
        record.user_id, record.conversation_id, record.namespace_name, record.key);
    if (record.version <= 0) {
        record.version = existing.has_value() ? existing->version + 1 : 1;
    }
    record.updated_at = now_ms();
    const std::string payload = record.payload.dump();
    const std::string target_json = nlohmann::json(target_nodes).dump();

#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    if (impl_->mysql_mode()) {
        auto mysql = impl_->mysql_connect();
        auto esc = [&](const std::string& value) { return impl_->escape(mysql.conn, value); };
        impl_->mysql_exec(mysql.conn, "START TRANSACTION");
        try {
            impl_->mysql_exec(
                mysql.conn,
                "INSERT INTO mb_state_records(user_id,conversation_id,namespace_name,state_key,version,"
                "risk_level,payload,updated_at) VALUES('" +
                    esc(record.user_id) + "','" + esc(record.conversation_id) + "','" +
                    esc(record.namespace_name) + "','" + esc(record.key) + "'," +
                    std::to_string(record.version) + ",'" +
                    esc(record.risk_level.empty() ? "low" : record.risk_level) + "','" + esc(payload) +
                    "'," + std::to_string(record.updated_at) + ") "
                "ON DUPLICATE KEY UPDATE "
                "version=IF(VALUES(version) >= version, VALUES(version), version),"
                "risk_level=IF(VALUES(version) >= version, VALUES(risk_level), risk_level),"
                "payload=IF(VALUES(version) >= version, VALUES(payload), payload),"
                "updated_at=IF(VALUES(version) >= version, VALUES(updated_at), updated_at)");
            impl_->mysql_exec(
                mysql.conn,
                "INSERT INTO mb_state_changes(user_id,conversation_id,namespace_name,state_key,operation,"
                "version,risk_level,payload,source_node,target_nodes,created_at) VALUES('" +
                    esc(record.user_id) + "','" + esc(record.conversation_id) + "','" +
                    esc(record.namespace_name) + "','" + esc(record.key) + "','upsert'," +
                    std::to_string(record.version) + ",'" +
                    esc(record.risk_level.empty() ? "low" : record.risk_level) + "','" + esc(payload) +
                    "','" + esc(node_id_) + "','" + esc(target_json) + "'," +
                    std::to_string(record.updated_at) + ")");
            const std::int64_t change_id = static_cast<std::int64_t>(mysql_insert_id(mysql.conn));
            impl_->mysql_exec(mysql.conn, "COMMIT");
            // Invalidate L1 caches after successful write
            invalidate_record_cache(record.user_id, record.conversation_id, record.namespace_name, record.key);
            if (is_conversation_namespace(record.namespace_name)) {
                invalidate_history_cache(record.user_id, record.conversation_id);
            }
            return change_id;
        } catch (...) {
            impl_->mysql_exec(mysql.conn, "ROLLBACK");
            throw;
        }
    }
#endif
    exec(impl_->db, "BEGIN IMMEDIATE;");
    try {
        auto upsert = prepare(impl_->db,
                              "INSERT INTO state_records(user_id, conversation_id, namespace_name, state_key, "
                              "version, risk_level, payload, updated_at) VALUES(?,?,?,?,?,?,?,?) "
                              "ON CONFLICT(user_id, conversation_id, namespace_name, state_key) DO UPDATE SET "
                              "version=excluded.version, risk_level=excluded.risk_level, "
                              "payload=excluded.payload, updated_at=excluded.updated_at "
                              "WHERE excluded.version >= state_records.version;");
        bind_text(upsert.get(), 1, record.user_id);
        bind_text(upsert.get(), 2, record.conversation_id);
        bind_text(upsert.get(), 3, record.namespace_name);
        bind_text(upsert.get(), 4, record.key);
        sqlite3_bind_int64(upsert.get(), 5, record.version);
        bind_text(upsert.get(), 6, record.risk_level.empty() ? "low" : record.risk_level);
        bind_text(upsert.get(), 7, payload);
        sqlite3_bind_int64(upsert.get(), 8, record.updated_at);
        step_done(impl_->db, upsert.get());

        auto change = prepare(impl_->db,
                              "INSERT INTO state_changes(user_id, conversation_id, namespace_name, state_key, "
                              "operation, version, risk_level, payload, source_node, target_nodes, created_at) "
                              "VALUES(?,?,?,?,?,?,?,?,?,?,?);");
        bind_text(change.get(), 1, record.user_id);
        bind_text(change.get(), 2, record.conversation_id);
        bind_text(change.get(), 3, record.namespace_name);
        bind_text(change.get(), 4, record.key);
        bind_text(change.get(), 5, "upsert");
        sqlite3_bind_int64(change.get(), 6, record.version);
        bind_text(change.get(), 7, record.risk_level.empty() ? "low" : record.risk_level);
        bind_text(change.get(), 8, payload);
        bind_text(change.get(), 9, node_id_);
        bind_text(change.get(), 10, target_json);
        sqlite3_bind_int64(change.get(), 11, record.updated_at);
        step_done(impl_->db, change.get());
        const std::int64_t change_id = sqlite3_last_insert_rowid(impl_->db);
        exec(impl_->db, "COMMIT;");
        // Invalidate L1 caches after successful write
        invalidate_record_cache(record.user_id, record.conversation_id, record.namespace_name, record.key);
        if (is_conversation_namespace(record.namespace_name)) {
            invalidate_history_cache(record.user_id, record.conversation_id);
        }
        return change_id;
    } catch (...) {
        exec(impl_->db, "ROLLBACK;");
        throw;
    }
}

std::int64_t DistributedStateStore::append_conversation_turn(
    const std::string& user_id,
    const std::string& conversation_id,
    const std::string& namespace_name,
    const std::string& role,
    const std::string& content,
    const std::string& risk_level) {
    const std::int64_t ts = now_ms();
    StateRecord record;
    record.user_id = user_id.empty() ? "anonymous" : user_id;
    record.conversation_id = conversation_id.empty() ? "default" : conversation_id;
    record.namespace_name = namespace_name.empty() ? "chat_memory" : namespace_name;
    record.key = conversation_key(ts, role.empty() ? "user" : role);
    record.risk_level = risk_level.empty() ? "low" : risk_level;
    record.payload = {{"role", role.empty() ? "user" : role},
                      {"content", content},
                      {"created_at", ts}};
    return upsert_record(std::move(record));
}

std::vector<StateChange> DistributedStateStore::fetch_changes_after(
    const std::int64_t change_id,
    const int limit) const {
    std::vector<StateChange> out;
    if (limit <= 0) {
        return out;
    }
#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    if (impl_->mysql_mode()) {
        auto mysql = impl_->mysql_connect();
        const std::string sql =
            "SELECT change_id,user_id,conversation_id,namespace_name,state_key,operation,version,"
            "risk_level,payload,source_node,target_nodes,created_at FROM mb_state_changes "
            "WHERE change_id>" + std::to_string(change_id) + " ORDER BY change_id ASC LIMIT " +
            std::to_string(limit);
        if (mysql_query(mysql.conn, sql.c_str()) != 0) {
            throw std::runtime_error(std::string("mysql query failed: ") + mysql_error(mysql.conn));
        }
        std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> res(mysql_store_result(mysql.conn),
                                                                     mysql_free_result);
        if (!res) {
            return out;
        }
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res.get()))) {
            StateChange change;
            change.change_id = row[0] ? std::stoll(row[0]) : 0;
            change.user_id = row[1] ? row[1] : "";
            change.conversation_id = row[2] ? row[2] : "";
            change.namespace_name = row[3] ? row[3] : "";
            change.key = row[4] ? row[4] : "";
            change.operation = row[5] ? row[5] : "";
            change.version = row[6] ? std::stoll(row[6]) : 0;
            change.risk_level = row[7] ? row[7] : "";
            change.payload = parse_json_or_object(row[8] ? row[8] : "");
            change.source_node = row[9] ? row[9] : "";
            change.target_nodes = parse_targets(row[10] ? row[10] : "");
            change.created_at = row[11] ? std::stoll(row[11]) : 0;
            out.push_back(std::move(change));
        }
        return out;
    }
#endif
    auto stmt = prepare(impl_->db,
                        "SELECT change_id, user_id, conversation_id, namespace_name, state_key, operation, "
                        "version, risk_level, payload, source_node, target_nodes, created_at "
                        "FROM state_changes WHERE change_id>? ORDER BY change_id ASC LIMIT ?;");
    sqlite3_bind_int64(stmt.get(), 1, change_id);
    sqlite3_bind_int(stmt.get(), 2, limit);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        out.push_back(change_from_stmt(stmt.get()));
    }
    return out;
}

ReplicaProgress DistributedStateStore::progress_for(const std::string& replica_node_id) const {
    ReplicaProgress progress;
    progress.node_id = replica_node_id.empty() ? node_id_ : replica_node_id;
#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    if (impl_->mysql_mode()) {
        auto mysql = impl_->mysql_connect();
        const std::string node = impl_->escape(mysql.conn, progress.node_id);
        const std::string sql =
            "SELECT node_id,last_applied_change_id,applied_count,skipped_count,updated_at "
            "FROM mb_replica_progress WHERE node_id='" + node + "' LIMIT 1";
        if (mysql_query(mysql.conn, sql.c_str()) != 0) {
            throw std::runtime_error(std::string("mysql query failed: ") + mysql_error(mysql.conn));
        }
        std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> res(mysql_store_result(mysql.conn),
                                                                     mysql_free_result);
        if (res) {
            MYSQL_ROW row = mysql_fetch_row(res.get());
            if (row) {
                progress.node_id = row[0] ? row[0] : progress.node_id;
                progress.last_applied_change_id = row[1] ? std::stoll(row[1]) : 0;
                progress.applied_count = row[2] ? std::stoll(row[2]) : 0;
                progress.skipped_count = row[3] ? std::stoll(row[3]) : 0;
                progress.updated_at = row[4] ? std::stoll(row[4]) : 0;
            }
        }
        return progress;
    }
#endif
    auto stmt = prepare(impl_->db,
                        "SELECT node_id, last_applied_change_id, applied_count, skipped_count, updated_at "
                        "FROM replica_progress WHERE node_id=?;");
    bind_text(stmt.get(), 1, progress.node_id);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        progress.node_id = col_text(stmt.get(), 0);
        progress.last_applied_change_id = sqlite3_column_int64(stmt.get(), 1);
        progress.applied_count = sqlite3_column_int64(stmt.get(), 2);
        progress.skipped_count = sqlite3_column_int64(stmt.get(), 3);
        progress.updated_at = sqlite3_column_int64(stmt.get(), 4);
    }
    return progress;
}

bool DistributedStateStore::apply_change(const StateChange& change, const std::string& replica_node_id) {
    const std::string progress_node = replica_node_id.empty() ? node_id_ : replica_node_id;
    const ReplicaProgress before = progress_for(progress_node);
#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    if (impl_->mysql_mode()) {
        auto mysql = impl_->mysql_connect();
        auto esc = [&](const std::string& value) { return impl_->escape(mysql.conn, value); };
        if (change.change_id <= before.last_applied_change_id) {
            impl_->mysql_exec(
                mysql.conn,
                "INSERT INTO mb_replica_progress(node_id,last_applied_change_id,applied_count,"
                "skipped_count,updated_at) VALUES('" +
                    esc(progress_node) + "'," + std::to_string(before.last_applied_change_id) +
                    ",0,1," + std::to_string(now_ms()) + ") "
                "ON DUPLICATE KEY UPDATE skipped_count=skipped_count+1,updated_at=VALUES(updated_at)");
            return false;
        }

        bool applied = false;
        bool skipped = false;
        if (!targets_contain(change.target_nodes, progress_node)) {
            skipped = true;
        } else {
            const auto existing = get_record(
                change.user_id, change.conversation_id, change.namespace_name, change.key);
            if (existing.has_value() && existing->version >= change.version) {
                applied = true;
            } else if (change.operation == "upsert") {
                impl_->mysql_exec(
                    mysql.conn,
                    "INSERT INTO mb_state_records(user_id,conversation_id,namespace_name,state_key,version,"
                    "risk_level,payload,updated_at) VALUES('" +
                        esc(change.user_id) + "','" + esc(change.conversation_id) + "','" +
                        esc(change.namespace_name) + "','" + esc(change.key) + "'," +
                        std::to_string(change.version) + ",'" +
                        esc(change.risk_level.empty() ? "low" : change.risk_level) + "','" +
                        esc(change.payload.dump()) + "'," + std::to_string(now_ms()) + ") "
                    "ON DUPLICATE KEY UPDATE "
                    "risk_level=IF(VALUES(version) >= version, VALUES(risk_level), risk_level),"
                    "payload=IF(VALUES(version) >= version, VALUES(payload), payload),"
                    "updated_at=IF(VALUES(version) >= version, VALUES(updated_at), updated_at),"
                    "version=IF(VALUES(version) >= version, VALUES(version), version)");
                applied = true;
            } else {
                skipped = true;
            }
        }

        impl_->mysql_exec(
            mysql.conn,
            "INSERT INTO mb_replica_progress(node_id,last_applied_change_id,applied_count,skipped_count,"
            "updated_at) VALUES('" +
                esc(progress_node) + "'," + std::to_string(change.change_id) + "," +
                std::to_string(applied ? 1 : 0) + "," + std::to_string(skipped ? 1 : 0) + "," +
                std::to_string(now_ms()) + ") "
            "ON DUPLICATE KEY UPDATE last_applied_change_id=VALUES(last_applied_change_id),"
            "applied_count=applied_count+VALUES(applied_count),"
            "skipped_count=skipped_count+VALUES(skipped_count),updated_at=VALUES(updated_at)");
        notify_observers(change, applied);
        return applied;
    }
#endif
    if (change.change_id <= before.last_applied_change_id) {
        auto duplicate = prepare(impl_->db,
                                 "INSERT INTO replica_progress(node_id, last_applied_change_id, applied_count, "
                                 "skipped_count, updated_at) VALUES(?,?,?,?,?) "
                                 "ON CONFLICT(node_id) DO UPDATE SET "
                                 "skipped_count=replica_progress.skipped_count + 1, "
                                 "updated_at=excluded.updated_at;");
        bind_text(duplicate.get(), 1, progress_node);
        sqlite3_bind_int64(duplicate.get(), 2, before.last_applied_change_id);
        sqlite3_bind_int64(duplicate.get(), 3, 0);
        sqlite3_bind_int64(duplicate.get(), 4, 1);
        sqlite3_bind_int64(duplicate.get(), 5, now_ms());
        step_done(impl_->db, duplicate.get());
        return false;
    }

    bool applied = false;
    bool skipped = false;
    if (!targets_contain(change.target_nodes, progress_node)) {
        skipped = true;
    } else {
        const auto existing = get_record(
            change.user_id, change.conversation_id, change.namespace_name, change.key);
        if (existing.has_value() && existing->version >= change.version) {
            skipped = true;
        } else if (change.operation == "upsert") {
            StateRecord record;
            record.user_id = change.user_id;
            record.conversation_id = change.conversation_id;
            record.namespace_name = change.namespace_name;
            record.key = change.key;
            record.version = change.version;
            record.risk_level = change.risk_level.empty() ? "low" : change.risk_level;
            record.payload = change.payload;
            record.updated_at = now_ms();
            auto stmt = prepare(impl_->db,
                                "INSERT INTO state_records(user_id, conversation_id, namespace_name, state_key, "
                                "version, risk_level, payload, updated_at) VALUES(?,?,?,?,?,?,?,?) "
                                "ON CONFLICT(user_id, conversation_id, namespace_name, state_key) DO UPDATE SET "
                                "version=excluded.version, risk_level=excluded.risk_level, "
                                "payload=excluded.payload, updated_at=excluded.updated_at "
                                "WHERE excluded.version >= state_records.version;");
            bind_text(stmt.get(), 1, record.user_id);
            bind_text(stmt.get(), 2, record.conversation_id);
            bind_text(stmt.get(), 3, record.namespace_name);
            bind_text(stmt.get(), 4, record.key);
            sqlite3_bind_int64(stmt.get(), 5, record.version);
            bind_text(stmt.get(), 6, record.risk_level);
            bind_text(stmt.get(), 7, record.payload.dump());
            sqlite3_bind_int64(stmt.get(), 8, record.updated_at);
            step_done(impl_->db, stmt.get());
            applied = true;
        } else {
            skipped = true;
        }
    }

    auto progress = prepare(impl_->db,
                            "INSERT INTO replica_progress(node_id, last_applied_change_id, applied_count, "
                            "skipped_count, updated_at) VALUES(?,?,?,?,?) "
                            "ON CONFLICT(node_id) DO UPDATE SET "
                            "last_applied_change_id=excluded.last_applied_change_id, "
                            "applied_count=replica_progress.applied_count + ?, "
                            "skipped_count=replica_progress.skipped_count + ?, "
                            "updated_at=excluded.updated_at;");
    bind_text(progress.get(), 1, progress_node);
    sqlite3_bind_int64(progress.get(), 2, change.change_id);
    sqlite3_bind_int64(progress.get(), 3, applied ? 1 : 0);
    sqlite3_bind_int64(progress.get(), 4, skipped ? 1 : 0);
    sqlite3_bind_int64(progress.get(), 5, now_ms());
    sqlite3_bind_int64(progress.get(), 6, applied ? 1 : 0);
    sqlite3_bind_int64(progress.get(), 7, skipped ? 1 : 0);
    step_done(impl_->db, progress.get());
    notify_observers(change, applied);
    return applied;
}

StoreStatus DistributedStateStore::status(const std::string& progress_node_id) const {
    StoreStatus status;
    status.node_id = node_id_;
#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    if (impl_->mysql_mode()) {
        auto mysql = impl_->mysql_connect();
        if (mysql_query(mysql.conn, "SELECT COUNT(*) FROM mb_state_records") != 0) {
            throw std::runtime_error(std::string("mysql query failed: ") + mysql_error(mysql.conn));
        }
        {
            std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> res(mysql_store_result(mysql.conn),
                                                                         mysql_free_result);
            if (res) {
                MYSQL_ROW row = mysql_fetch_row(res.get());
                status.record_count = row && row[0] ? std::stoll(row[0]) : 0;
            }
        }
        if (mysql_query(mysql.conn, "SELECT COUNT(*) FROM mb_state_changes") != 0) {
            throw std::runtime_error(std::string("mysql query failed: ") + mysql_error(mysql.conn));
        }
        {
            std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> res(mysql_store_result(mysql.conn),
                                                                         mysql_free_result);
            if (res) {
                MYSQL_ROW row = mysql_fetch_row(res.get());
                status.change_count = row && row[0] ? std::stoll(row[0]) : 0;
            }
        }
        status.progress = progress_for(progress_node_id.empty() ? node_id_ : progress_node_id);
        return status;
    }
#endif
    auto records = prepare(impl_->db, "SELECT COUNT(*) FROM state_records;");
    if (sqlite3_step(records.get()) == SQLITE_ROW) {
        status.record_count = sqlite3_column_int64(records.get(), 0);
    }
    auto changes = prepare(impl_->db, "SELECT COUNT(*) FROM state_changes;");
    if (sqlite3_step(changes.get()) == SQLITE_ROW) {
        status.change_count = sqlite3_column_int64(changes.get(), 0);
    }
    status.progress = progress_for(progress_node_id.empty() ? node_id_ : progress_node_id);
    return status;
}

ReplicaWorker::ReplicaWorker(DistributedStateStore& source,
                             DistributedStateStore& replica,
                             std::string replica_node_id)
    : source_(source),
      replica_(replica),
      replica_node_id_(std::move(replica_node_id)) {}

int ReplicaWorker::sync_once(const int limit) {
    const auto progress = replica_.progress_for(replica_node_id_);
    const auto changes = source_.fetch_changes_after(progress.last_applied_change_id, limit);
    int applied = 0;
    for (const auto& change : changes) {
        if (replica_.apply_change(change, replica_node_id_)) {
            ++applied;
        }
    }
    return applied;
}

nlohmann::json ReplicaWorker::status() const {
    const auto source = source_.status();
    const auto replica = replica_.status(replica_node_id_);
    return {{"source_node", source.node_id},
            {"replica_node", replica_node_id_},
            {"source_change_count", source.change_count},
            {"replica_record_count", replica.record_count},
            {"last_applied_change_id", replica.progress.last_applied_change_id},
            {"applied_count", replica.progress.applied_count},
            {"skipped_count", replica.progress.skipped_count}};
}

void DistributedStateStore::add_change_observer(ChangeObserver cb) {
    std::lock_guard<std::mutex> lock(observer_mutex_);
    change_observers_.push_back(std::move(cb));
}

void DistributedStateStore::notify_observers(const StateChange& change, bool applied) {
    std::vector<ChangeObserver> snapshot;
    {
        std::lock_guard<std::mutex> lock(observer_mutex_);
        snapshot = change_observers_;
    }
    for (const auto& observer : snapshot) {
        try {
            observer(change, applied);
        } catch (...) {
            // observer 异常不应影响主流程
        }
    }
}

}  // namespace state
}  // namespace mindbridge
