#include "mindbridge/storage/cloud_storage.hpp"

#include "mindbridge/apps/app_support.hpp"

#include <curl/curl.h>
#include <sqlite3.h>

#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
extern "C" {
#include <fastdfs/fdfs_client.h>
#include <fastdfs/storage_client1.h>
#include <fastdfs/tracker_client.h>
#include <fastcommon/logger.h>
#include <fastcommon/shared_func.h>
#include <hiredis/hiredis.h>
#include <mysql/mysql.h>
}
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace mindbridge {
namespace storage {
namespace {

using StmtPtr = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

size_t write_bytes_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::vector<unsigned char>*>(userp);
    const auto* begin = static_cast<unsigned char*>(contents);
    out->insert(out->end(), begin, begin + size * nmemb);
    return size * nmemb;
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string sanitize_name(std::string value) {
    if (value.empty()) {
        return "file";
    }
    for (char& ch : value) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u < 32 || ch == '/' || ch == '\\') {
            ch = '_';
        }
    }
    return value;
}

std::string stable_upload_id(const std::string& user_id,
                             const std::string& conversation_id,
                             const std::string& md5) {
    std::hash<std::string> h;
    std::ostringstream out;
    out << "up_" << std::hex << h(user_id + "#" + conversation_id + "#" + md5);
    return out.str();
}

std::string file_ext(const std::string& filename) {
    const auto slash = filename.find_last_of("/\\");
    const auto dot = filename.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash) ||
        dot + 1 >= filename.size()) {
        return "";
    }
    std::string ext = filename.substr(dot + 1);
    if (ext.size() > 16) {
        ext.resize(16);
    }
    return ext;
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

std::vector<unsigned char> decode_base64(const std::string& input) {
    int values[256];
    std::fill(std::begin(values), std::end(values), -1);
    const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < chars.size(); ++i) {
        values[static_cast<unsigned char>(chars[i])] = static_cast<int>(i);
    }
    std::vector<unsigned char> out;
    int val = 0;
    int bits = -8;
    for (unsigned char ch : input) {
        if (std::isspace(ch)) {
            continue;
        }
        if (ch == '=') {
            break;
        }
        if (values[ch] == -1) {
            throw std::runtime_error("invalid base64 file data");
        }
        val = (val << 6) + values[ch];
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<unsigned char>((val >> bits) & 0xff));
            bits -= 8;
        }
    }
    return out;
}

std::string encode_base64(const std::vector<unsigned char>& bytes) {
    static constexpr char kChars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    int val = 0;
    int bits = -6;
    for (unsigned char ch : bytes) {
        val = (val << 8) + ch;
        bits += 8;
        while (bits >= 0) {
            out.push_back(kChars[(val >> bits) & 0x3f]);
            bits -= 6;
        }
    }
    if (bits > -6) {
        out.push_back(kChars[((val << 8) >> (bits + 8)) & 0x3f]);
    }
    while (out.size() % 4) {
        out.push_back('=');
    }
    return out;
}

std::vector<int> parse_uploaded(const std::string& raw) {
    std::set<int> unique;
    std::stringstream in(raw);
    std::string item;
    while (std::getline(in, item, ',')) {
        if (item.empty()) {
            continue;
        }
        try {
            unique.insert(std::stoi(item));
        } catch (...) {
        }
    }
    return std::vector<int>(unique.begin(), unique.end());
}

std::string join_uploaded(std::vector<int> uploaded) {
    std::sort(uploaded.begin(), uploaded.end());
    uploaded.erase(std::unique(uploaded.begin(), uploaded.end()), uploaded.end());
    std::ostringstream out;
    for (size_t i = 0; i < uploaded.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        out << uploaded[i];
    }
    return out.str();
}

FileObject file_from_stmt(sqlite3_stmt* stmt) {
    FileObject file;
    file.md5 = col_text(stmt, 0);
    file.file_id = col_text(stmt, 1);
    file.url = col_text(stmt, 2);
    file.size = sqlite3_column_int64(stmt, 3);
    file.type = col_text(stmt, 4);
    file.ref_count = sqlite3_column_int64(stmt, 5);
    file.filename = col_text(stmt, 6);
    file.created_at = sqlite3_column_int64(stmt, 7);
    return file;
}

nlohmann::json file_json(const FileObject& file) {
    return {{"md5", file.md5},
            {"file_id", file.file_id},
            {"url", file.url},
            {"file_name", file.filename},
            {"filename", file.filename},
            {"size", file.size},
            {"type", file.type},
            {"ref_count", file.ref_count},
            {"created_at", file.created_at}};
}

}  // namespace

struct CloudStorageService::Impl {
    Impl(std::string root, std::string public_url, state::DistributedStateStore* store)
        : root_dir(std::move(root)),
          public_base_url(std::move(public_url)),
          state_store(store) {}

    std::filesystem::path root_dir;
    std::string public_base_url;
    state::DistributedStateStore* state_store{nullptr};
    sqlite3* db{nullptr};
    std::string backend{"local"};
#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    std::string mysql_host{"127.0.0.1"};
    int mysql_port{3306};
    std::string mysql_user{"root"};
    std::string mysql_password;
    std::string mysql_database{"mindbridge"};
    std::string redis_host{"127.0.0.1"};
    int redis_port{6379};
    std::string redis_password;
    int redis_db{0};
    std::string fdfs_client_conf;
#endif

    std::filesystem::path db_path() const { return root_dir / "metadata.sqlite"; }
    std::filesystem::path object_dir() const { return root_dir / "objects"; }
    std::filesystem::path chunk_root() const { return root_dir / "chunks"; }

    std::filesystem::path object_path(const std::string& md5) const {
        return object_dir() / sanitize_name(md5);
    }

    std::string object_url(const std::string& md5) const {
        if (public_base_url.empty()) {
            return "/api/storage/files/" + md5 + "/download";
        }
        std::string base = public_base_url;
        while (!base.empty() && base.back() == '/') {
            base.pop_back();
        }
        return base + "/api/storage/files/" + md5 + "/download";
    }

    bool is_cloud_backend() const {
        return backend == "cloud" || backend == "live";
    }

    void initialize() {
        backend = mindbridge::app::env_or("MINDBRIDGE_STORAGE_BACKEND", "local");
#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
        if (backend == "cloud" || backend == "live") {
            mysql_host = mindbridge::app::env_or("MINDBRIDGE_MYSQL_HOST", "127.0.0.1");
            mysql_port = std::stoi(mindbridge::app::env_or("MINDBRIDGE_MYSQL_PORT", "3307"));
            mysql_user = mindbridge::app::env_or("MINDBRIDGE_MYSQL_USER", "root");
            mysql_password = mindbridge::app::env_or("MINDBRIDGE_MYSQL_PASSWORD");
            mysql_database = mindbridge::app::env_or("MINDBRIDGE_MYSQL_DATABASE", "mindbridge");
            redis_host = mindbridge::app::env_or("MINDBRIDGE_REDIS_HOST", "127.0.0.1");
            redis_port = std::stoi(mindbridge::app::env_or("MINDBRIDGE_REDIS_PORT", "6379"));
            redis_password = mindbridge::app::env_or("MINDBRIDGE_REDIS_PASSWORD");
            redis_db = std::stoi(mindbridge::app::env_or("MINDBRIDGE_REDIS_DB", "0"));
            fdfs_client_conf = mindbridge::app::env_or("MINDBRIDGE_FASTDFS_CLIENT_CONF",
                                                       mindbridge::app::env_or("MINDBRIDGE_FDFS_CLIENT_CONF"));
            if (public_base_url.empty()) {
                public_base_url = mindbridge::app::env_or("MINDBRIDGE_FASTDFS_STORAGE_BASE_URL",
                                                          "http://127.0.0.1");
            }
            initialize_cloud();
            return;
        }
#else
        if (backend == "cloud" || backend == "live") {
            throw std::runtime_error("cloud storage live adapter was not compiled; FastDFS/hiredis/mysqlclient deps are missing");
        }
#endif
        std::filesystem::create_directories(root_dir);
        std::filesystem::create_directories(object_dir());
        std::filesystem::create_directories(chunk_root());
        if (sqlite3_open(db_path().string().c_str(), &db) != SQLITE_OK) {
            std::string error = db ? sqlite3_errmsg(db) : "sqlite open failed";
            if (db) {
                sqlite3_close(db);
                db = nullptr;
            }
            throw std::runtime_error(error);
        }
        exec(db, "PRAGMA journal_mode=WAL;");
        exec(db,
             "CREATE TABLE IF NOT EXISTS mb_file_info ("
             "md5 TEXT PRIMARY KEY,"
             "file_id TEXT NOT NULL,"
             "url TEXT NOT NULL,"
             "size INTEGER NOT NULL,"
             "type TEXT NOT NULL,"
             "ref_count INTEGER NOT NULL,"
             "status TEXT NOT NULL,"
             "created_at INTEGER NOT NULL,"
             "updated_at INTEGER NOT NULL);");
        exec(db,
             "CREATE TABLE IF NOT EXISTS mb_user_file_list ("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "user_id TEXT NOT NULL,"
             "conversation_id TEXT NOT NULL,"
             "run_id TEXT NOT NULL,"
             "md5 TEXT NOT NULL,"
             "file_name TEXT NOT NULL,"
             "shared_status INTEGER NOT NULL,"
             "pv INTEGER NOT NULL,"
             "created_at INTEGER NOT NULL,"
             "UNIQUE(user_id, conversation_id, md5, file_name));");
        exec(db,
             "CREATE TABLE IF NOT EXISTS mb_user_file_count ("
             "user_id TEXT PRIMARY KEY,"
             "file_count INTEGER NOT NULL,"
             "updated_at INTEGER NOT NULL);");
        exec(db,
             "CREATE TABLE IF NOT EXISTS mb_chunk_sessions ("
             "upload_id TEXT PRIMARY KEY,"
             "user_id TEXT NOT NULL,"
             "conversation_id TEXT NOT NULL,"
             "run_id TEXT NOT NULL,"
             "md5 TEXT NOT NULL,"
             "filename TEXT NOT NULL,"
             "type TEXT NOT NULL,"
             "size INTEGER NOT NULL,"
             "chunk_count INTEGER NOT NULL,"
             "uploaded TEXT NOT NULL,"
             "expires_at INTEGER NOT NULL,"
             "updated_at INTEGER NOT NULL);");
        exec(db,
             "CREATE TABLE IF NOT EXISTS mb_storage_change_tasks ("
             "task_id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "user_id TEXT NOT NULL,"
             "conversation_id TEXT NOT NULL,"
             "run_id TEXT NOT NULL,"
             "md5 TEXT NOT NULL,"
             "operation TEXT NOT NULL,"
             "payload TEXT NOT NULL,"
             "status TEXT NOT NULL,"
             "attempt INTEGER NOT NULL,"
             "reason TEXT NOT NULL,"
             "created_at INTEGER NOT NULL,"
             "updated_at INTEGER NOT NULL);");
    }

    ~Impl() {
        if (db) {
            sqlite3_close(db);
        }
    }

    std::optional<FileObject> find_file(const std::string& md5) const {
        auto stmt = prepare(db,
                            "SELECT f.md5, f.file_id, f.url, f.size, f.type, f.ref_count, "
                            "COALESCE(u.file_name, ''), f.created_at "
                            "FROM mb_file_info f LEFT JOIN mb_user_file_list u ON u.md5=f.md5 "
                            "WHERE f.md5=? AND f.status='active' ORDER BY u.created_at DESC LIMIT 1;");
        bind_text(stmt.get(), 1, md5);
        if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
            return std::nullopt;
        }
        return file_from_stmt(stmt.get());
    }

    void write_file_bytes(const std::string& md5, const std::vector<unsigned char>& bytes) const {
        std::ofstream out(object_path(md5), std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot open object file for write");
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    std::vector<unsigned char> read_file_bytes(const std::string& md5) const {
        std::ifstream in(object_path(md5), std::ios::binary);
        if (!in) {
            throw std::runtime_error("object bytes not found");
        }
        return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    }

    FileObject upsert_file_and_reference(const StorageIdentity& identity,
                                         const std::string& md5,
                                         const std::string& filename,
                                         const std::string& type,
                                         std::int64_t size) {
        if (md5.empty()) {
            throw std::runtime_error("md5 is required");
        }
        const std::int64_t ts = now_ms();
        const std::string clean_name = sanitize_name(filename);
        const std::string file_id = "local://" + md5;
        const std::string url = object_url(md5);
        exec(db, "BEGIN IMMEDIATE;");
        try {
            auto file = prepare(db,
                                "INSERT INTO mb_file_info(md5, file_id, url, size, type, ref_count, status, "
                                "created_at, updated_at) VALUES(?,?,?,?,?,?,?,?,?) "
                                "ON CONFLICT(md5) DO UPDATE SET "
                                "ref_count=mb_file_info.ref_count + 1, updated_at=excluded.updated_at;");
            bind_text(file.get(), 1, md5);
            bind_text(file.get(), 2, file_id);
            bind_text(file.get(), 3, url);
            sqlite3_bind_int64(file.get(), 4, size);
            bind_text(file.get(), 5, type);
            sqlite3_bind_int64(file.get(), 6, 1);
            bind_text(file.get(), 7, "active");
            sqlite3_bind_int64(file.get(), 8, ts);
            sqlite3_bind_int64(file.get(), 9, ts);
            step_done(db, file.get());

            auto user = prepare(db,
                                "INSERT OR IGNORE INTO mb_user_file_list(user_id, conversation_id, run_id, md5, "
                                "file_name, shared_status, pv, created_at) VALUES(?,?,?,?,?,?,?,?);");
            bind_text(user.get(), 1, identity.user_id.empty() ? "anonymous" : identity.user_id);
            bind_text(user.get(), 2, identity.conversation_id.empty() ? "default" : identity.conversation_id);
            bind_text(user.get(), 3, identity.run_id);
            bind_text(user.get(), 4, md5);
            bind_text(user.get(), 5, clean_name);
            sqlite3_bind_int(user.get(), 6, 0);
            sqlite3_bind_int(user.get(), 7, 0);
            sqlite3_bind_int64(user.get(), 8, ts);
            step_done(db, user.get());

            auto count = prepare(db,
                                 "INSERT INTO mb_user_file_count(user_id, file_count, updated_at) VALUES(?,?,?) "
                                 "ON CONFLICT(user_id) DO UPDATE SET "
                                 "file_count=(SELECT COUNT(*) FROM mb_user_file_list WHERE user_id=excluded.user_id), "
                                 "updated_at=excluded.updated_at;");
            bind_text(count.get(), 1, identity.user_id.empty() ? "anonymous" : identity.user_id);
            sqlite3_bind_int64(count.get(), 2, 1);
            sqlite3_bind_int64(count.get(), 3, ts);
            step_done(db, count.get());

            nlohmann::json payload = {{"md5", md5},
                                      {"file_id", file_id},
                                      {"url", url},
                                      {"file_name", clean_name},
                                      {"size", size},
                                      {"type", type},
                                      {"run_id", identity.run_id}};
            auto task = prepare(db,
                                "INSERT INTO mb_storage_change_tasks(user_id, conversation_id, run_id, md5, "
                                "operation, payload, status, attempt, reason, created_at, updated_at) "
                                "VALUES(?,?,?,?,?,?,?,?,?,?,?);");
            bind_text(task.get(), 1, identity.user_id.empty() ? "anonymous" : identity.user_id);
            bind_text(task.get(), 2, identity.conversation_id.empty() ? "default" : identity.conversation_id);
            bind_text(task.get(), 3, identity.run_id);
            bind_text(task.get(), 4, md5);
            bind_text(task.get(), 5, "artifact_upsert");
            bind_text(task.get(), 6, payload.dump());
            bind_text(task.get(), 7, "pending");
            sqlite3_bind_int(task.get(), 8, 0);
            bind_text(task.get(), 9, "");
            sqlite3_bind_int64(task.get(), 10, ts);
            sqlite3_bind_int64(task.get(), 11, ts);
            step_done(db, task.get());
            exec(db, "COMMIT;");
        } catch (...) {
            exec(db, "ROLLBACK;");
            throw;
        }
        auto saved = find_file(md5);
        if (!saved.has_value()) {
            throw std::runtime_error("file metadata was not saved");
        }
        saved->filename = clean_name;
        sync_pending_artifacts(20);
        return *saved;
    }

    int sync_pending_artifacts(int limit) {
        if (!state_store || limit <= 0) {
            return 0;
        }
        auto stmt = prepare(db,
                            "SELECT task_id, user_id, conversation_id, run_id, md5, payload, attempt "
                            "FROM mb_storage_change_tasks WHERE status IN ('pending','failed') "
                            "ORDER BY task_id ASC LIMIT ?;");
        sqlite3_bind_int(stmt.get(), 1, limit);
        struct Pending {
            std::int64_t task_id;
            std::string user_id;
            std::string conversation_id;
            std::string run_id;
            std::string md5;
            nlohmann::json payload;
            int attempt;
        };
        std::vector<Pending> tasks;
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            tasks.push_back({sqlite3_column_int64(stmt.get(), 0),
                             col_text(stmt.get(), 1),
                             col_text(stmt.get(), 2),
                             col_text(stmt.get(), 3),
                             col_text(stmt.get(), 4),
                             nlohmann::json::parse(col_text(stmt.get(), 5), nullptr, false),
                             sqlite3_column_int(stmt.get(), 6)});
        }
        int applied = 0;
        for (const auto& task : tasks) {
            try {
                if (task.payload.is_discarded()) {
                    throw std::runtime_error("invalid task payload");
                }
                state::StateRecord record;
                record.user_id = task.user_id;
                record.conversation_id = task.conversation_id.empty() ? "default" : task.conversation_id;
                record.namespace_name = "run_artifact";
                record.key = "file_" + task.md5;
                record.risk_level = "low";
                record.payload = task.payload;
                state_store->upsert_record(std::move(record));
                auto done = prepare(db,
                                    "UPDATE mb_storage_change_tasks SET status='done', attempt=?, reason='', "
                                    "updated_at=? WHERE task_id=?;");
                sqlite3_bind_int(done.get(), 1, task.attempt + 1);
                sqlite3_bind_int64(done.get(), 2, now_ms());
                sqlite3_bind_int64(done.get(), 3, task.task_id);
                step_done(db, done.get());
                ++applied;
            } catch (const std::exception& e) {
                auto failed = prepare(db,
                                      "UPDATE mb_storage_change_tasks SET status='failed', attempt=?, reason=?, "
                                      "updated_at=? WHERE task_id=?;");
                sqlite3_bind_int(failed.get(), 1, task.attempt + 1);
                bind_text(failed.get(), 2, e.what());
                sqlite3_bind_int64(failed.get(), 3, now_ms());
                sqlite3_bind_int64(failed.get(), 4, task.task_id);
                step_done(db, failed.get());
            }
        }
        return applied;
    }

#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    struct MysqlHandle {
        MYSQL* conn{nullptr};
        ~MysqlHandle() {
            if (conn) {
                mysql_close(conn);
            }
        }
    };

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

    std::string mysql_escape(MYSQL* conn, const std::string& value) const {
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

    void initialize_cloud() {
        if (fdfs_client_conf.empty()) {
            throw std::runtime_error("MINDBRIDGE_FASTDFS_CLIENT_CONF is required for cloud storage");
        }
        auto mysql = mysql_connect();
        mysql_exec(mysql.conn,
                   "CREATE TABLE IF NOT EXISTS mb_file_info ("
                   "id BIGINT NOT NULL AUTO_INCREMENT,"
                   "md5 VARCHAR(256) NOT NULL,"
                   "file_id VARCHAR(512) NOT NULL,"
                   "url VARCHAR(1024) NOT NULL,"
                   "size BIGINT NOT NULL DEFAULT 0,"
                   "type VARCHAR(64) NOT NULL DEFAULT '',"
                   "ref_count INT NOT NULL DEFAULT 1,"
                   "status VARCHAR(32) NOT NULL DEFAULT 'active',"
                   "created_at BIGINT NOT NULL,"
                   "updated_at BIGINT NOT NULL,"
                   "PRIMARY KEY(id), UNIQUE KEY uq_mb_file_md5(md5(191)),"
                   "KEY idx_mb_file_status(status)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");
        mysql_exec(mysql.conn,
                   "CREATE TABLE IF NOT EXISTS mb_user_file_list ("
                   "id BIGINT NOT NULL AUTO_INCREMENT,"
                   "user_id VARCHAR(128) NOT NULL,"
                   "conversation_id VARCHAR(256) NOT NULL DEFAULT 'default',"
                   "run_id VARCHAR(256) NOT NULL DEFAULT '',"
                   "md5 VARCHAR(256) NOT NULL,"
                   "file_name VARCHAR(512) NOT NULL,"
                   "shared_status INT NOT NULL DEFAULT 0,"
                   "pv INT NOT NULL DEFAULT 0,"
                   "created_at BIGINT NOT NULL,"
                   "PRIMARY KEY(id),"
                   "UNIQUE KEY uq_mb_user_file(user_id, conversation_id, md5(191), file_name(191)),"
                   "KEY idx_mb_user_files(user_id, conversation_id, created_at)) "
                   "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");
        mysql_exec(mysql.conn,
                   "CREATE TABLE IF NOT EXISTS mb_storage_change_tasks ("
                   "task_id BIGINT NOT NULL AUTO_INCREMENT,"
                   "user_id VARCHAR(128) NOT NULL,"
                   "conversation_id VARCHAR(256) NOT NULL DEFAULT 'default',"
                   "run_id VARCHAR(256) NOT NULL DEFAULT '',"
                   "md5 VARCHAR(256) NOT NULL,"
                   "operation VARCHAR(64) NOT NULL DEFAULT 'artifact_upsert',"
                   "payload JSON NOT NULL,"
                   "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
                   "attempt INT NOT NULL DEFAULT 0,"
                   "reason VARCHAR(1024) NOT NULL DEFAULT '',"
                   "created_at BIGINT NOT NULL,"
                   "updated_at BIGINT NOT NULL,"
                   "PRIMARY KEY(task_id), KEY idx_mb_storage_task_status(status, task_id)) "
                   "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");
        mysql_exec(mysql.conn,
                   "CREATE TABLE IF NOT EXISTS mb_user_file_count ("
                   "user_id VARCHAR(128) NOT NULL,"
                   "file_count INT NOT NULL DEFAULT 0,"
                   "updated_at BIGINT NOT NULL,"
                   "PRIMARY KEY(user_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");
        mysql_exec(mysql.conn,
                   "CREATE TABLE IF NOT EXISTS mb_storage_node_progress ("
                   "node_id VARCHAR(128) NOT NULL,"
                   "last_applied_task_id BIGINT NOT NULL DEFAULT 0,"
                   "applied_count BIGINT NOT NULL DEFAULT 0,"
                   "skipped_count BIGINT NOT NULL DEFAULT 0,"
                   "updated_at BIGINT NOT NULL,"
                   "PRIMARY KEY(node_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");
        try {
            mysql_exec(mysql.conn, "ALTER TABLE mb_file_info MODIFY created_at BIGINT NOT NULL");
            mysql_exec(mysql.conn, "ALTER TABLE mb_file_info MODIFY updated_at BIGINT NOT NULL");
            mysql_exec(mysql.conn, "ALTER TABLE mb_user_file_list MODIFY created_at BIGINT NOT NULL");
            mysql_exec(mysql.conn, "ALTER TABLE mb_user_file_count MODIFY updated_at BIGINT NOT NULL");
        } catch (...) {
        }
        auto redis = redis_connect();
        (void)redis;
        std::filesystem::create_directories(chunk_root());
        std::filesystem::create_directories(root_dir / "tmp");
    }

    struct RedisHandle {
        redisContext* conn{nullptr};
        ~RedisHandle() {
            if (conn) {
                redisFree(conn);
            }
        }
    };

    RedisHandle redis_connect() const {
        RedisHandle handle;
        timeval timeout{2, 0};
        handle.conn = redisConnectWithTimeout(redis_host.c_str(), redis_port, timeout);
        if (!handle.conn || handle.conn->err) {
            const std::string err = handle.conn ? handle.conn->errstr : "redis connect failed";
            throw std::runtime_error(err);
        }
        if (!redis_password.empty()) {
            redisReply* reply = static_cast<redisReply*>(
                redisCommand(handle.conn, "AUTH %s", redis_password.c_str()));
            if (!reply || reply->type == REDIS_REPLY_ERROR) {
                std::string err = reply && reply->str ? reply->str : "redis auth failed";
                if (reply) {
                    freeReplyObject(reply);
                }
                throw std::runtime_error(err);
            }
            freeReplyObject(reply);
        }
        if (redis_db > 0) {
            redisReply* reply = static_cast<redisReply*>(
                redisCommand(handle.conn, "SELECT %d", redis_db));
            if (!reply || reply->type == REDIS_REPLY_ERROR) {
                std::string err = reply && reply->str ? reply->str : "redis select failed";
                if (reply) {
                    freeReplyObject(reply);
                }
                throw std::runtime_error(err);
            }
            freeReplyObject(reply);
        }
        return handle;
    }

    std::optional<FileObject> cloud_find_file(const std::string& md5) const {
        auto mysql = mysql_connect();
        const std::string emd5 = mysql_escape(mysql.conn, md5);
        const std::string sql =
            "SELECT f.md5,f.file_id,f.url,f.size,f.type,f.ref_count,"
            "COALESCE(u.file_name,''),f.created_at FROM mb_file_info f "
            "LEFT JOIN mb_user_file_list u ON u.md5=f.md5 "
            "WHERE f.md5='" + emd5 + "' AND f.status='active' ORDER BY u.created_at DESC LIMIT 1";
        if (mysql_query(mysql.conn, sql.c_str()) != 0) {
            throw std::runtime_error(std::string("mysql query failed: ") + mysql_error(mysql.conn));
        }
        MYSQL_RES* raw = mysql_store_result(mysql.conn);
        std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> res(raw, mysql_free_result);
        if (!res) {
            return std::nullopt;
        }
        MYSQL_ROW row = mysql_fetch_row(res.get());
        if (!row) {
            return std::nullopt;
        }
        FileObject file;
        file.md5 = row[0] ? row[0] : "";
        file.file_id = row[1] ? row[1] : "";
        file.url = row[2] ? row[2] : "";
        file.size = row[3] ? std::stoll(row[3]) : 0;
        file.type = row[4] ? row[4] : "";
        file.ref_count = row[5] ? std::stoll(row[5]) : 0;
        file.filename = row[6] ? row[6] : "";
        file.created_at = row[7] ? std::stoll(row[7]) : 0;
        return file;
    }

    struct CloudChunkSession {
        std::string upload_id;
        std::string user_id;
        std::string conversation_id;
        std::string run_id;
        std::string md5;
        std::string filename;
        std::string type;
        std::int64_t size{0};
        int chunk_count{0};
        std::string uploaded;
    };

    std::string chunk_key(const std::string& upload_id) const {
        return "mb:chunk:" + upload_id;
    }

    std::string chunk_md5_key(const std::string& md5) const {
        return "mb:chunk_md5:" + md5;
    }

    void redis_expect_ok(redisContext*, redisReply* raw, const std::string& operation) const {
        std::unique_ptr<redisReply, decltype(&freeReplyObject)> reply(raw, freeReplyObject);
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            const std::string err = reply && reply->str ? reply->str : operation + " failed";
            throw std::runtime_error(err);
        }
    }

    std::string redis_get_string(redisContext* conn, const std::string& key) const {
        std::unique_ptr<redisReply, decltype(&freeReplyObject)> reply(
            static_cast<redisReply*>(redisCommand(conn, "GET %s", key.c_str())), freeReplyObject);
        if (!reply || reply->type == REDIS_REPLY_NIL) {
            return "";
        }
        if (reply->type != REDIS_REPLY_STRING) {
            throw std::runtime_error("redis GET returned non-string value");
        }
        return reply->str ? reply->str : "";
    }

    std::optional<CloudChunkSession> redis_load_chunk_session(const std::string& upload_id) const {
        if (upload_id.empty()) {
            return std::nullopt;
        }
        auto redis = redis_connect();
        const auto key = chunk_key(upload_id);
        std::unique_ptr<redisReply, decltype(&freeReplyObject)> reply(
            static_cast<redisReply*>(redisCommand(redis.conn, "HGETALL %s", key.c_str())),
            freeReplyObject);
        if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements == 0) {
            return std::nullopt;
        }
        CloudChunkSession session;
        session.upload_id = upload_id;
        for (size_t i = 0; i + 1 < reply->elements; i += 2) {
            const std::string field = reply->element[i]->str ? reply->element[i]->str : "";
            const std::string value = reply->element[i + 1]->str ? reply->element[i + 1]->str : "";
            if (field == "user_id") {
                session.user_id = value;
            } else if (field == "conversation_id") {
                session.conversation_id = value;
            } else if (field == "run_id") {
                session.run_id = value;
            } else if (field == "md5") {
                session.md5 = value;
            } else if (field == "filename") {
                session.filename = value;
            } else if (field == "type") {
                session.type = value;
            } else if (field == "size") {
                session.size = value.empty() ? 0 : std::stoll(value);
            } else if (field == "chunk_count") {
                session.chunk_count = value.empty() ? 0 : std::stoi(value);
            } else if (field == "uploaded") {
                session.uploaded = value;
            }
        }
        return session;
    }

    std::string redis_resolve_upload_id(const std::string& upload_id, const std::string& md5) const {
        if (!upload_id.empty()) {
            return upload_id;
        }
        if (md5.empty()) {
            return "";
        }
        auto redis = redis_connect();
        return redis_get_string(redis.conn, chunk_md5_key(md5));
    }

    std::string fdfs_url(const std::string& file_id) const {
        std::string base = public_base_url;
        while (!base.empty() && base.back() == '/') {
            base.pop_back();
        }
        return base + "/" + file_id;
    }

    std::string fdfs_upload_file(const std::filesystem::path& path, const std::string& filename) const {
        log_init();
        g_log_context.log_level = LOG_ERR;
        ignore_signal_pipe();
        const int init_result = fdfs_client_init(fdfs_client_conf.c_str());
        if (init_result != 0) {
            throw std::runtime_error("fdfs_client_init failed: " + std::to_string(init_result));
        }
        ConnectionInfo* tracker = tracker_get_connection();
        if (!tracker) {
            fdfs_client_destroy();
            throw std::runtime_error("tracker_get_connection failed");
        }
        char group_name[FDFS_GROUP_NAME_MAX_LEN + 1] = {0};
        ConnectionInfo storage_server;
        int store_path_index = 0;
        int result = tracker_query_storage_store(tracker, &storage_server, group_name, &store_path_index);
        if (result != 0) {
            tracker_close_connection_ex(tracker, true);
            fdfs_client_destroy();
            throw std::runtime_error("tracker_query_storage_store failed: " + std::to_string(result));
        }
        char file_id[1024] = {0};
        const std::string ext = file_ext(filename);
        result = storage_upload_by_filename1(tracker,
                                             &storage_server,
                                             store_path_index,
                                             path.string().c_str(),
                                             ext.empty() ? nullptr : ext.c_str(),
                                             nullptr,
                                             0,
                                             group_name,
                                             file_id);
        tracker_close_connection_ex(tracker, true);
        fdfs_client_destroy();
        if (result != 0) {
            throw std::runtime_error("storage_upload_by_filename1 failed: " + std::to_string(result));
        }
        return file_id;
    }

    std::vector<unsigned char> fdfs_download_file(const std::string& file_id) const {
        const int init_result = fdfs_client_init(fdfs_client_conf.c_str());
        if (init_result != 0) {
            throw std::runtime_error("fdfs_client_init failed: " + std::to_string(init_result));
        }
        ConnectionInfo* tracker = tracker_get_connection();
        if (!tracker) {
            fdfs_client_destroy();
            throw std::runtime_error("tracker_get_connection failed");
        }
        ConnectionInfo storage_server;
        char* file_buff = nullptr;
        int64_t file_size = 0;
        int result = storage_download_file_to_buff1(
            tracker, &storage_server, file_id.c_str(), &file_buff, &file_size);
        tracker_close_connection_ex(tracker, true);
        fdfs_client_destroy();
        if (result != 0) {
            throw std::runtime_error("storage_download_file_to_buff1 failed: " + std::to_string(result));
        }
        std::vector<unsigned char> bytes(
            reinterpret_cast<unsigned char*>(file_buff),
            reinterpret_cast<unsigned char*>(file_buff) + file_size);
        free(file_buff);
        return bytes;
    }

    std::vector<unsigned char> http_download_file(const std::string& url) const {
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("curl_easy_init failed");
        }
        char error_buffer[CURL_ERROR_SIZE] = {0};
        std::vector<unsigned char> bytes;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_bytes_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bytes);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        if (url.rfind("https://", 0) == 0) {
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        }
        CURLcode rc = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(curl);
        if (rc != CURLE_OK) {
            const std::string detail = error_buffer[0] ? error_buffer : curl_easy_strerror(rc);
            throw std::runtime_error("FastDFS HTTP download failed: " + detail);
        }
        if (code >= 400) {
            throw std::runtime_error("FastDFS HTTP download returned HTTP " + std::to_string(code));
        }
        return bytes;
    }

    FileObject cloud_upsert_file_and_reference(const StorageIdentity& identity,
                                               const std::string& md5,
                                               const std::string& filename,
                                               const std::string& type,
                                               std::int64_t size,
                                               const std::string& file_id) {
        auto mysql = mysql_connect();
        const std::int64_t ts = now_ms();
        const std::string user_id = identity.user_id.empty() ? "anonymous" : identity.user_id;
        const std::string conversation_id = identity.conversation_id.empty() ? "default" : identity.conversation_id;
        const std::string clean_name = sanitize_name(filename);
        const std::string url = fdfs_url(file_id);
        auto esc = [&](const std::string& value) { return mysql_escape(mysql.conn, value); };
        mysql_exec(mysql.conn, "START TRANSACTION");
        try {
            mysql_exec(mysql.conn,
                       "INSERT INTO mb_file_info(md5,file_id,url,size,type,ref_count,status,created_at,updated_at) "
                       "VALUES('" + esc(md5) + "','" + esc(file_id) + "','" + esc(url) + "'," +
                           std::to_string(size) + ",'" + esc(type) + "',1,'active'," +
                           std::to_string(ts) + "," + std::to_string(ts) + ") "
                       "ON DUPLICATE KEY UPDATE ref_count=ref_count+1,updated_at=VALUES(updated_at)");
            mysql_exec(mysql.conn,
                       "INSERT IGNORE INTO mb_user_file_list(user_id,conversation_id,run_id,md5,file_name,"
                       "shared_status,pv,created_at) VALUES('" +
                           esc(user_id) + "','" + esc(conversation_id) + "','" + esc(identity.run_id) + "','" +
                           esc(md5) + "','" + esc(clean_name) + "',0,0," + std::to_string(ts) + ")");
            nlohmann::json payload = {{"md5", md5},
                                      {"file_id", file_id},
                                      {"url", url},
                                      {"file_name", clean_name},
                                      {"size", size},
                                      {"type", type},
                                      {"run_id", identity.run_id}};
            mysql_exec(mysql.conn,
                       "INSERT INTO mb_storage_change_tasks(user_id,conversation_id,run_id,md5,operation,payload,"
                       "status,attempt,reason,created_at,updated_at) VALUES('" +
                           esc(user_id) + "','" + esc(conversation_id) + "','" + esc(identity.run_id) + "','" +
                           esc(md5) + "','artifact_upsert','" + esc(payload.dump()) +
                           "','pending',0,''," + std::to_string(ts) + "," + std::to_string(ts) + ")");
            mysql_exec(mysql.conn,
                       "INSERT INTO mb_user_file_count(user_id,file_count,updated_at) VALUES('" +
                           esc(user_id) + "',1," + std::to_string(ts) + ") "
                       "ON DUPLICATE KEY UPDATE file_count=("
                       "SELECT COUNT(*) FROM mb_user_file_list WHERE user_id='" + esc(user_id) +
                           "'),updated_at=" + std::to_string(ts));
            mysql_exec(mysql.conn, "COMMIT");
        } catch (...) {
            mysql_exec(mysql.conn, "ROLLBACK");
            throw;
        }
        try {
            cloud_sync_pending_artifacts(20);
        } catch (const std::exception& e) {
            if (mindbridge::app::env_or("MINDBRIDGE_STORAGE_STRICT_SYNC", "false") == "true") {
                throw std::runtime_error(std::string("cloud artifact sync failed: ") + e.what());
            }
        }
        auto saved = cloud_find_file(md5);
        if (!saved.has_value()) {
            throw std::runtime_error("cloud file metadata was not saved");
        }
        saved->filename = clean_name;
        return *saved;
    }

    nlohmann::json cloud_instant_upload(const StorageIdentity& identity,
                                        const std::string& md5,
                                        const std::string& filename,
                                        const std::string& type) {
        auto existing = cloud_find_file(md5);
        if (!existing.has_value()) {
            return {{"ok", false}, {"code", 1}, {"instant", false}, {"exists", false}};
        }
        const auto saved = cloud_upsert_file_and_reference(
            identity, md5, filename, type.empty() ? existing->type : type, existing->size, existing->file_id);
        return {{"ok", true}, {"code", 0}, {"instant", true}, {"exists", true}, {"file", file_json(saved)}};
    }

    nlohmann::json cloud_upload_base64(const StorageIdentity& identity,
                                       const std::string& filename,
                                       const std::string& md5,
                                       const std::string& type,
                                       const std::string& data_base64) {
        auto existing = cloud_find_file(md5);
        if (existing.has_value()) {
            const auto saved = cloud_upsert_file_and_reference(
                identity, md5, filename, type.empty() ? existing->type : type, existing->size, existing->file_id);
            return {{"ok", true}, {"code", 0}, {"instant", true}, {"file", file_json(saved)}};
        }
        const auto bytes = decode_base64(data_base64);
        std::filesystem::create_directories(root_dir / "tmp");
        const auto tmp = root_dir / "tmp" / (sanitize_name(md5) + ".upload");
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        const std::string file_id = fdfs_upload_file(tmp, filename);
        std::filesystem::remove(tmp);
        const auto saved = cloud_upsert_file_and_reference(
            identity, md5, filename, type, static_cast<std::int64_t>(bytes.size()), file_id);
        return {{"ok", true}, {"code", 0}, {"instant", false}, {"file", file_json(saved)}};
    }

    nlohmann::json cloud_init_chunk_upload(const StorageIdentity& identity,
                                           const std::string& filename,
                                           const std::string& md5,
                                           const std::string& type,
                                           std::int64_t size,
                                           int chunk_count) {
        if (filename.empty() || md5.empty() || chunk_count <= 0 || size <= 0) {
            throw std::runtime_error("filename, md5, positive size, and chunk_count are required");
        }
        if (auto existing = cloud_find_file(md5); existing.has_value()) {
            const auto saved = cloud_upsert_file_and_reference(
                identity, md5, filename, type.empty() ? existing->type : type, existing->size, existing->file_id);
            return {{"ok", true}, {"code", 0}, {"instant", true}, {"file", file_json(saved)}};
        }
        const std::string upload_id = stable_upload_id(identity.user_id, identity.conversation_id, md5);
        auto redis = redis_connect();
        std::string uploaded;
        if (auto existing = redis_load_chunk_session(upload_id); existing.has_value()) {
            uploaded = existing->uploaded;
        }
        const std::string key = chunk_key(upload_id);
        redis_expect_ok(redis.conn,
                        static_cast<redisReply*>(redisCommand(
                            redis.conn,
                            "HMSET %s user_id %s conversation_id %s run_id %s md5 %s filename %s type %s size %lld chunk_count %d uploaded %s owner_node %s",
                            key.c_str(),
                            (identity.user_id.empty() ? "anonymous" : identity.user_id).c_str(),
                            (identity.conversation_id.empty() ? "default" : identity.conversation_id).c_str(),
                            identity.run_id.c_str(),
                            md5.c_str(),
                            sanitize_name(filename).c_str(),
                            type.c_str(),
                            static_cast<long long>(size),
                            chunk_count,
                            uploaded.c_str(),
                            mindbridge::app::env_or("MINDBRIDGE_GATEWAY_NODE_ID", "gateway-node").c_str())),
                        "redis HMSET chunk session");
        redis_expect_ok(redis.conn,
                        static_cast<redisReply*>(
                            redisCommand(redis.conn, "EXPIRE %s %d", key.c_str(), 24 * 60 * 60)),
                        "redis EXPIRE chunk session");
        redis_expect_ok(redis.conn,
                        static_cast<redisReply*>(redisCommand(
                            redis.conn, "SETEX %s %d %s", chunk_md5_key(md5).c_str(), 24 * 60 * 60, upload_id.c_str())),
                        "redis SETEX chunk md5 index");
        std::filesystem::create_directories(chunk_root() / upload_id);
        return {{"ok", true},
                {"code", 0},
                {"instant", false},
                {"upload_id", upload_id},
                {"chunkCount", chunk_count},
                {"uploadedChunks", uploaded},
                {"uploaded", uploaded}};
    }

    nlohmann::json cloud_upload_chunk(const std::string& upload_id,
                                      const std::string& md5,
                                      int index,
                                      const std::string& bytes) {
        if (index < 0 || bytes.empty()) {
            throw std::runtime_error("chunk index and bytes are required");
        }
        const std::string resolved_upload_id = redis_resolve_upload_id(upload_id, md5);
        auto session = redis_load_chunk_session(resolved_upload_id);
        if (!session.has_value()) {
            throw std::runtime_error("upload session not found");
        }
        if (index >= session->chunk_count) {
            throw std::runtime_error("chunk index out of range");
        }
        std::filesystem::create_directories(chunk_root() / resolved_upload_id);
        std::ofstream out(chunk_root() / resolved_upload_id / std::to_string(index),
                          std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot write chunk");
        }
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        auto uploaded = parse_uploaded(session->uploaded);
        uploaded.push_back(index);
        const std::string uploaded_text = join_uploaded(uploaded);
        auto redis = redis_connect();
        const auto key = chunk_key(resolved_upload_id);
        redis_expect_ok(redis.conn,
                        static_cast<redisReply*>(
                            redisCommand(redis.conn, "HSET %s uploaded %s", key.c_str(), uploaded_text.c_str())),
                        "redis HSET uploaded chunks");
        redis_expect_ok(redis.conn,
                        static_cast<redisReply*>(
                            redisCommand(redis.conn, "EXPIRE %s %d", key.c_str(), 24 * 60 * 60)),
                        "redis EXPIRE chunk session");
        return {{"ok", true}, {"code", 0}, {"upload_id", resolved_upload_id}, {"uploaded", uploaded_text}};
    }

    nlohmann::json cloud_merge_chunks(const StorageIdentity& identity,
                                      const std::string& upload_id,
                                      const std::string& md5,
                                      const std::string& filename,
                                      const std::string& type) {
        const std::string resolved_upload_id = redis_resolve_upload_id(upload_id, md5);
        auto session = redis_load_chunk_session(resolved_upload_id);
        if (!session.has_value()) {
            throw std::runtime_error("upload session not found");
        }
        auto uploaded = parse_uploaded(session->uploaded);
        if (static_cast<int>(uploaded.size()) < session->chunk_count) {
            return {{"ok", false}, {"code", 1}, {"error", "chunks incomplete"}};
        }
        std::filesystem::create_directories(root_dir / "tmp");
        const auto tmp = root_dir / "tmp" / (sanitize_name(session->md5) + ".merge");
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                throw std::runtime_error("cannot open merged chunk file");
            }
            for (int i = 0; i < session->chunk_count; ++i) {
                std::ifstream in(chunk_root() / resolved_upload_id / std::to_string(i), std::ios::binary);
                if (!in) {
                    return {{"ok", false}, {"code", 1}, {"error", "chunks incomplete"}};
                }
                out << in.rdbuf();
            }
        }
        const std::string file_id =
            fdfs_upload_file(tmp, filename.empty() ? session->filename : filename);
        std::filesystem::remove(tmp);
        StorageIdentity effective = identity;
        if (effective.user_id.empty()) {
            effective.user_id = session->user_id;
        }
        if (effective.conversation_id.empty()) {
            effective.conversation_id = session->conversation_id;
        }
        if (effective.run_id.empty()) {
            effective.run_id = session->run_id;
        }
        const auto saved = cloud_upsert_file_and_reference(
            effective,
            session->md5,
            filename.empty() ? session->filename : filename,
            type.empty() ? session->type : type,
            session->size,
            file_id);
        auto redis = redis_connect();
        redis_expect_ok(redis.conn,
                        static_cast<redisReply*>(
                            redisCommand(redis.conn, "DEL %s %s", chunk_key(resolved_upload_id).c_str(),
                                         chunk_md5_key(session->md5).c_str())),
                        "redis DEL chunk session");
        std::filesystem::remove_all(chunk_root() / resolved_upload_id);
        return {{"ok", true}, {"code", 0}, {"instant", false}, {"file", file_json(saved)}};
    }

    nlohmann::json cloud_list_files(const StorageIdentity& identity, int limit, int offset) const {
        limit = std::max(1, std::min(limit, 100));
        offset = std::max(0, offset);
        auto mysql = mysql_connect();
        const std::string user_id = identity.user_id.empty() ? "anonymous" : identity.user_id;
        const std::string conversation_id = identity.conversation_id.empty() ? "default" : identity.conversation_id;
        auto esc = [&](const std::string& value) { return mysql_escape(mysql.conn, value); };
        const std::string sql =
            "SELECT f.md5,f.file_id,f.url,f.size,f.type,f.ref_count,u.file_name,u.created_at "
            "FROM mb_user_file_list u JOIN mb_file_info f ON f.md5=u.md5 "
            "WHERE u.user_id='" + esc(user_id) + "' AND u.conversation_id='" + esc(conversation_id) +
            "' AND f.status='active' ORDER BY u.created_at DESC LIMIT " + std::to_string(limit) +
            " OFFSET " + std::to_string(offset);
        if (mysql_query(mysql.conn, sql.c_str()) != 0) {
            throw std::runtime_error(std::string("mysql query failed: ") + mysql_error(mysql.conn));
        }
        MYSQL_RES* raw = mysql_store_result(mysql.conn);
        std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> res(raw, mysql_free_result);
        nlohmann::json files = nlohmann::json::array();
        if (res) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res.get()))) {
                FileObject file;
                file.md5 = row[0] ? row[0] : "";
                file.file_id = row[1] ? row[1] : "";
                file.url = row[2] ? row[2] : "";
                file.size = row[3] ? std::stoll(row[3]) : 0;
                file.type = row[4] ? row[4] : "";
                file.ref_count = row[5] ? std::stoll(row[5]) : 0;
                file.filename = row[6] ? row[6] : "";
                file.created_at = row[7] ? std::stoll(row[7]) : 0;
                files.push_back(file_json(file));
            }
        }
        return {{"ok", true}, {"code", 0}, {"files", files}};
    }

    nlohmann::json cloud_download_file(const StorageIdentity& identity, const std::string& md5) const {
        auto mysql = mysql_connect();
        const std::string user_id = identity.user_id.empty() ? "anonymous" : identity.user_id;
        auto esc = [&](const std::string& value) { return mysql_escape(mysql.conn, value); };
        const std::string sql =
            "SELECT f.md5,f.file_id,f.url,f.size,f.type,f.ref_count,u.file_name,u.created_at "
            "FROM mb_user_file_list u JOIN mb_file_info f ON f.md5=u.md5 "
            "WHERE u.user_id='" + esc(user_id) + "' AND u.md5='" + esc(md5) +
            "' AND f.status='active' LIMIT 1";
        if (mysql_query(mysql.conn, sql.c_str()) != 0) {
            throw std::runtime_error(std::string("mysql query failed: ") + mysql_error(mysql.conn));
        }
        MYSQL_RES* raw = mysql_store_result(mysql.conn);
        std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> res(raw, mysql_free_result);
        if (!res) {
            return {{"ok", false}, {"code", 404}, {"error", "file not found"}};
        }
        MYSQL_ROW row = mysql_fetch_row(res.get());
        if (!row) {
            return {{"ok", false}, {"code", 404}, {"error", "file not found"}};
        }
        FileObject file;
        file.md5 = row[0] ? row[0] : "";
        file.file_id = row[1] ? row[1] : "";
        file.url = row[2] ? row[2] : "";
        file.size = row[3] ? std::stoll(row[3]) : 0;
        file.type = row[4] ? row[4] : "";
        file.ref_count = row[5] ? std::stoll(row[5]) : 0;
        file.filename = row[6] ? row[6] : "";
        file.created_at = row[7] ? std::stoll(row[7]) : 0;
        if (!file.url.empty() && mindbridge::app::env_or("MINDBRIDGE_STORAGE_DOWNLOAD_MODE", "inline") == "redirect") {
            return {{"ok", true}, {"code", 0}, {"redirect", true}, {"location", file.url}, {"file", file_json(file)}};
        }
        std::vector<unsigned char> bytes;
        if (!file.url.empty()) {
            bytes = http_download_file(file.url);
        } else {
            bytes = fdfs_download_file(file.file_id);
        }
        return {{"ok", true},
                {"code", 0},
                {"file", file_json(file)},
                {"mime_type", file.type.empty() ? "application/octet-stream" : file.type},
                {"data_base64", encode_base64(bytes)}};
    }

    nlohmann::json cloud_status() const {
        auto mysql = mysql_connect();
        std::int64_t file_count = 0;
        std::int64_t pending_count = 0;
        if (mysql_query(mysql.conn, "SELECT COUNT(*) FROM mb_file_info WHERE status='active'") == 0) {
            std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> res(mysql_store_result(mysql.conn),
                                                                         mysql_free_result);
            if (res) {
                MYSQL_ROW row = mysql_fetch_row(res.get());
                file_count = row && row[0] ? std::stoll(row[0]) : 0;
            }
        }
        if (mysql_query(mysql.conn, "SELECT COUNT(*) FROM mb_storage_change_tasks WHERE status!='done'") == 0) {
            std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> res(mysql_store_result(mysql.conn),
                                                                         mysql_free_result);
            if (res) {
                MYSQL_ROW row = mysql_fetch_row(res.get());
                pending_count = row && row[0] ? std::stoll(row[0]) : 0;
            }
        }
        return {{"ok", true},
                {"backend", "cloud"},
                {"mysql_host", mysql_host},
                {"redis_host", redis_host},
                {"fastdfs_client_conf", fdfs_client_conf},
                {"file_count", file_count},
                {"pending_artifact_tasks", pending_count},
                {"state_store_attached", state_store != nullptr}};
    }

    int cloud_sync_pending_artifacts(int limit) {
        if (!state_store || limit <= 0) {
            return 0;
        }
        std::string stage = "connect read mysql";
        try {
        auto mysql = mysql_connect();
        const std::string sql =
            "SELECT task_id,user_id,conversation_id,run_id,md5,payload,attempt "
            "FROM mb_storage_change_tasks WHERE status IN ('pending','failed') "
            "ORDER BY task_id ASC LIMIT " + std::to_string(limit);
        stage = "select pending storage tasks";
        if (mysql_query(mysql.conn, sql.c_str()) != 0) {
            throw std::runtime_error(std::string("mysql query failed: ") + mysql_error(mysql.conn));
        }
        stage = "store pending storage tasks";
        MYSQL_RES* raw = mysql_store_result(mysql.conn);
        std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> res(raw, mysql_free_result);
        struct Pending {
            std::int64_t task_id;
            std::string user_id;
            std::string conversation_id;
            std::string run_id;
            std::string md5;
            nlohmann::json payload;
            int attempt;
        };
        std::vector<Pending> tasks;
        if (res) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res.get()))) {
                tasks.push_back({row[0] ? std::stoll(row[0]) : 0,
                                 row[1] ? row[1] : "",
                                 row[2] ? row[2] : "",
                                 row[3] ? row[3] : "",
                                 row[4] ? row[4] : "",
                                 nlohmann::json::parse(row[5] ? row[5] : "{}", nullptr, false),
                                 row[6] ? std::stoi(row[6]) : 0});
            }
        }
        res.reset(nullptr);
        stage = "connect update mysql";
        auto update_mysql = mysql_connect();
        auto esc = [&](const std::string& value) { return mysql_escape(update_mysql.conn, value); };
        int applied = 0;
        std::int64_t last_applied_task_id = 0;
        for (const auto& task : tasks) {
            try {
                if (task.payload.is_discarded()) {
                    throw std::runtime_error("invalid task payload");
                }
                state::StateRecord record;
                record.user_id = task.user_id;
                record.conversation_id = task.conversation_id.empty() ? "default" : task.conversation_id;
                record.namespace_name = "run_artifact";
                record.key = "file_" + task.md5;
                record.risk_level = "low";
                record.payload = task.payload;
                stage = "upsert run_artifact state";
                state_store->upsert_record(std::move(record));
                stage = "mark storage task done";
                mysql_exec(update_mysql.conn,
                           "UPDATE mb_storage_change_tasks SET status='done',attempt=" +
                               std::to_string(task.attempt + 1) + ",reason='',updated_at=" +
                               std::to_string(now_ms()) + " WHERE task_id=" +
                               std::to_string(task.task_id));
                ++applied;
                last_applied_task_id = task.task_id;
            } catch (const std::exception& e) {
                stage = "mark storage task failed";
                mysql_exec(update_mysql.conn,
                           "UPDATE mb_storage_change_tasks SET status='failed',attempt=" +
                               std::to_string(task.attempt + 1) + ",reason='" + esc(e.what()) +
                               "',updated_at=" + std::to_string(now_ms()) + " WHERE task_id=" +
                               std::to_string(task.task_id));
            }
        }
        if (applied > 0) {
            const std::string node_id =
                mindbridge::app::env_or("MINDBRIDGE_STORAGE_STATE_NODE_ID", "gateway-storage");
            stage = "update storage node progress";
            mysql_exec(update_mysql.conn,
                       "INSERT INTO mb_storage_node_progress(node_id,last_applied_task_id,applied_count,"
                       "skipped_count,updated_at) VALUES('" +
                           esc(node_id) + "'," + std::to_string(last_applied_task_id) + "," +
                           std::to_string(applied) + ",0," + std::to_string(now_ms()) + ") "
                       "ON DUPLICATE KEY UPDATE last_applied_task_id=VALUES(last_applied_task_id),"
                       "applied_count=applied_count+VALUES(applied_count),updated_at=VALUES(updated_at)");
        }
        return applied;
        } catch (const std::exception& e) {
            throw std::runtime_error(stage + ": " + e.what());
        }
    }
#endif
};

CloudStorageService::CloudStorageService(std::string root_dir,
                                         std::string public_base_url,
                                         state::DistributedStateStore* state_store)
    : impl_(std::make_unique<Impl>(std::move(root_dir),
                                   std::move(public_base_url),
                                   state_store)) {}

CloudStorageService::~CloudStorageService() = default;

void CloudStorageService::initialize() {
    impl_->initialize();
}

nlohmann::json CloudStorageService::instant_upload(const StorageIdentity& identity,
                                                   const std::string& md5,
                                                   const std::string& filename,
                                                   const std::string& type) {
#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    if (impl_->is_cloud_backend()) {
        return impl_->cloud_instant_upload(identity, md5, filename, type);
    }
#endif
    auto existing = impl_->find_file(md5);
    if (!existing.has_value()) {
        return {{"ok", false}, {"code", 1}, {"instant", false}, {"exists", false}};
    }
    const auto saved = impl_->upsert_file_and_reference(identity, md5, filename, type, existing->size);
    return {{"ok", true}, {"code", 0}, {"instant", true}, {"exists", true}, {"file", file_json(saved)}};
}

nlohmann::json CloudStorageService::upload_base64(const StorageIdentity& identity,
                                                  const std::string& filename,
                                                  const std::string& md5,
                                                  const std::string& type,
                                                  const std::string& data_base64) {
#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    if (impl_->is_cloud_backend()) {
        return impl_->cloud_upload_base64(identity, filename, md5, type, data_base64);
    }
#endif
    if (filename.empty() || md5.empty() || data_base64.empty()) {
        throw std::runtime_error("filename, md5, and data_base64 are required");
    }
    const auto existing = impl_->find_file(md5);
    if (existing.has_value()) {
        const auto saved = impl_->upsert_file_and_reference(identity, md5, filename, type, existing->size);
        return {{"ok", true}, {"code", 0}, {"instant", true}, {"file", file_json(saved)}};
    }
    const auto bytes = decode_base64(data_base64);
    impl_->write_file_bytes(md5, bytes);
    const auto saved = impl_->upsert_file_and_reference(
        identity, md5, filename, type, static_cast<std::int64_t>(bytes.size()));
    return {{"ok", true}, {"code", 0}, {"instant", false}, {"file", file_json(saved)}};
}

nlohmann::json CloudStorageService::init_chunk_upload(const StorageIdentity& identity,
                                                      const std::string& filename,
                                                      const std::string& md5,
                                                      const std::string& type,
                                                      std::int64_t size,
                                                      int chunk_count) {
#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    if (impl_->is_cloud_backend()) {
        return impl_->cloud_init_chunk_upload(identity, filename, md5, type, size, chunk_count);
    }
#endif
    if (filename.empty() || md5.empty() || chunk_count <= 0 || size <= 0) {
        throw std::runtime_error("filename, md5, positive size, and chunk_count are required");
    }
    if (impl_->find_file(md5).has_value()) {
        const auto saved = impl_->upsert_file_and_reference(identity, md5, filename, type, size);
        return {{"ok", true}, {"code", 0}, {"instant", true}, {"file", file_json(saved)}};
    }
    const std::string upload_id = stable_upload_id(identity.user_id, identity.conversation_id, md5);
    std::filesystem::create_directories(impl_->chunk_root() / upload_id);
    auto existing = prepare(impl_->db, "SELECT uploaded FROM mb_chunk_sessions WHERE upload_id=?;");
    bind_text(existing.get(), 1, upload_id);
    std::string uploaded;
    if (sqlite3_step(existing.get()) == SQLITE_ROW) {
        uploaded = col_text(existing.get(), 0);
    }
    auto stmt = prepare(impl_->db,
                        "INSERT INTO mb_chunk_sessions(upload_id, user_id, conversation_id, run_id, md5, filename, "
                        "type, size, chunk_count, uploaded, expires_at, updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?) "
                        "ON CONFLICT(upload_id) DO UPDATE SET run_id=excluded.run_id, filename=excluded.filename, "
                        "type=excluded.type, size=excluded.size, chunk_count=excluded.chunk_count, "
                        "expires_at=excluded.expires_at, updated_at=excluded.updated_at;");
    bind_text(stmt.get(), 1, upload_id);
    bind_text(stmt.get(), 2, identity.user_id.empty() ? "anonymous" : identity.user_id);
    bind_text(stmt.get(), 3, identity.conversation_id.empty() ? "default" : identity.conversation_id);
    bind_text(stmt.get(), 4, identity.run_id);
    bind_text(stmt.get(), 5, md5);
    bind_text(stmt.get(), 6, sanitize_name(filename));
    bind_text(stmt.get(), 7, type);
    sqlite3_bind_int64(stmt.get(), 8, size);
    sqlite3_bind_int(stmt.get(), 9, chunk_count);
    bind_text(stmt.get(), 10, uploaded);
    sqlite3_bind_int64(stmt.get(), 11, now_ms() + 24LL * 60 * 60 * 1000);
    sqlite3_bind_int64(stmt.get(), 12, now_ms());
    step_done(impl_->db, stmt.get());
    return {{"ok", true},
            {"code", 0},
            {"instant", false},
            {"upload_id", upload_id},
            {"chunkCount", chunk_count},
            {"uploadedChunks", uploaded},
            {"uploaded", uploaded}};
}

nlohmann::json CloudStorageService::upload_chunk(const std::string& upload_id,
                                                 const std::string& md5,
                                                 int index,
                                                 const std::string& bytes) {
#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    if (impl_->is_cloud_backend()) {
        return impl_->cloud_upload_chunk(upload_id, md5, index, bytes);
    }
#endif
    if (index < 0 || bytes.empty()) {
        throw std::runtime_error("chunk index and bytes are required");
    }
    std::string resolved_upload_id = upload_id;
    if (resolved_upload_id.empty() && !md5.empty()) {
        auto lookup = prepare(impl_->db,
                              "SELECT upload_id FROM mb_chunk_sessions WHERE md5=? ORDER BY updated_at DESC LIMIT 1;");
        bind_text(lookup.get(), 1, md5);
        if (sqlite3_step(lookup.get()) == SQLITE_ROW) {
            resolved_upload_id = col_text(lookup.get(), 0);
        }
    }
    if (resolved_upload_id.empty()) {
        throw std::runtime_error("upload session not found");
    }
    auto session = prepare(impl_->db, "SELECT uploaded, chunk_count FROM mb_chunk_sessions WHERE upload_id=?;");
    bind_text(session.get(), 1, resolved_upload_id);
    if (sqlite3_step(session.get()) != SQLITE_ROW) {
        throw std::runtime_error("upload session not found");
    }
    auto uploaded = parse_uploaded(col_text(session.get(), 0));
    const int chunk_count = sqlite3_column_int(session.get(), 1);
    if (index >= chunk_count) {
        throw std::runtime_error("chunk index out of range");
    }
    std::filesystem::create_directories(impl_->chunk_root() / resolved_upload_id);
    std::ofstream out(impl_->chunk_root() / resolved_upload_id / std::to_string(index),
                      std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot write chunk");
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    uploaded.push_back(index);
    const std::string uploaded_text = join_uploaded(uploaded);
    auto update = prepare(impl_->db,
                          "UPDATE mb_chunk_sessions SET uploaded=?, updated_at=? WHERE upload_id=?;");
    bind_text(update.get(), 1, uploaded_text);
    sqlite3_bind_int64(update.get(), 2, now_ms());
    bind_text(update.get(), 3, resolved_upload_id);
    step_done(impl_->db, update.get());
    return {{"ok", true}, {"code", 0}, {"upload_id", resolved_upload_id}, {"uploaded", uploaded_text}};
}

nlohmann::json CloudStorageService::merge_chunks(const StorageIdentity& identity,
                                                 const std::string& upload_id,
                                                 const std::string& md5,
                                                 const std::string& filename,
                                                 const std::string& type) {
#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    if (impl_->is_cloud_backend()) {
        return impl_->cloud_merge_chunks(identity, upload_id, md5, filename, type);
    }
#endif
    std::string resolved_upload_id = upload_id;
    if (resolved_upload_id.empty() && !md5.empty()) {
        auto lookup = prepare(impl_->db,
                              "SELECT upload_id FROM mb_chunk_sessions WHERE md5=? ORDER BY updated_at DESC LIMIT 1;");
        bind_text(lookup.get(), 1, md5);
        if (sqlite3_step(lookup.get()) == SQLITE_ROW) {
            resolved_upload_id = col_text(lookup.get(), 0);
        }
    }
    auto stmt = prepare(impl_->db,
                        "SELECT md5, filename, type, size, chunk_count, uploaded FROM mb_chunk_sessions "
                        "WHERE upload_id=?;");
    bind_text(stmt.get(), 1, resolved_upload_id);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        throw std::runtime_error("upload session not found");
    }
    const std::string session_md5 = col_text(stmt.get(), 0);
    const std::string session_filename = col_text(stmt.get(), 1);
    const std::string session_type = col_text(stmt.get(), 2);
    const std::int64_t size = sqlite3_column_int64(stmt.get(), 3);
    const int chunk_count = sqlite3_column_int(stmt.get(), 4);
    auto uploaded = parse_uploaded(col_text(stmt.get(), 5));
    if (static_cast<int>(uploaded.size()) < chunk_count) {
        return {{"ok", false}, {"code", 1}, {"error", "chunks incomplete"}};
    }
    std::vector<unsigned char> merged;
    for (int i = 0; i < chunk_count; ++i) {
        std::ifstream in(impl_->chunk_root() / resolved_upload_id / std::to_string(i), std::ios::binary);
        if (!in) {
            return {{"ok", false}, {"code", 1}, {"error", "chunks incomplete"}};
        }
        merged.insert(merged.end(),
                      std::istreambuf_iterator<char>(in),
                      std::istreambuf_iterator<char>());
    }
    impl_->write_file_bytes(session_md5, merged);
    const auto saved = impl_->upsert_file_and_reference(
        identity,
        session_md5,
        filename.empty() ? session_filename : filename,
        type.empty() ? session_type : type,
        size > 0 ? size : static_cast<std::int64_t>(merged.size()));
    std::filesystem::remove_all(impl_->chunk_root() / resolved_upload_id);
    auto cleanup = prepare(impl_->db, "DELETE FROM mb_chunk_sessions WHERE upload_id=?;");
    bind_text(cleanup.get(), 1, resolved_upload_id);
    step_done(impl_->db, cleanup.get());
    return {{"ok", true}, {"code", 0}, {"instant", false}, {"file", file_json(saved)}};
}

nlohmann::json CloudStorageService::list_files(const StorageIdentity& identity,
                                               int limit,
                                               int offset) const {
#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    if (impl_->is_cloud_backend()) {
        return impl_->cloud_list_files(identity, limit, offset);
    }
#endif
    limit = std::max(1, std::min(limit, 100));
    offset = std::max(0, offset);
    auto stmt = prepare(impl_->db,
                        "SELECT f.md5, f.file_id, f.url, f.size, f.type, f.ref_count, u.file_name, u.created_at "
                        "FROM mb_user_file_list u JOIN mb_file_info f ON f.md5=u.md5 "
                        "WHERE u.user_id=? AND u.conversation_id=? AND f.status='active' "
                        "ORDER BY u.created_at DESC LIMIT ? OFFSET ?;");
    bind_text(stmt.get(), 1, identity.user_id.empty() ? "anonymous" : identity.user_id);
    bind_text(stmt.get(), 2, identity.conversation_id.empty() ? "default" : identity.conversation_id);
    sqlite3_bind_int(stmt.get(), 3, limit);
    sqlite3_bind_int(stmt.get(), 4, offset);
    nlohmann::json files = nlohmann::json::array();
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        files.push_back(file_json(file_from_stmt(stmt.get())));
    }
    return {{"ok", true}, {"code", 0}, {"files", files}};
}

nlohmann::json CloudStorageService::download_file(const StorageIdentity& identity,
                                                  const std::string& md5) const {
#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    if (impl_->is_cloud_backend()) {
        return impl_->cloud_download_file(identity, md5);
    }
#endif
    auto ownership = prepare(impl_->db,
                             "SELECT f.md5, f.file_id, f.url, f.size, f.type, f.ref_count, u.file_name, u.created_at "
                             "FROM mb_user_file_list u JOIN mb_file_info f ON f.md5=u.md5 "
                             "WHERE u.user_id=? AND u.md5=? AND f.status='active' LIMIT 1;");
    bind_text(ownership.get(), 1, identity.user_id.empty() ? "anonymous" : identity.user_id);
    bind_text(ownership.get(), 2, md5);
    if (sqlite3_step(ownership.get()) != SQLITE_ROW) {
        return {{"ok", false}, {"code", 404}, {"error", "file not found"}};
    }
    const auto file = file_from_stmt(ownership.get());
    const auto bytes = impl_->read_file_bytes(md5);
    return {{"ok", true},
            {"code", 0},
            {"file", file_json(file)},
            {"mime_type", file.type.empty() ? "application/octet-stream" : file.type},
            {"data_base64", encode_base64(bytes)}};
}

nlohmann::json CloudStorageService::status() const {
#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    if (impl_->is_cloud_backend()) {
        return impl_->cloud_status();
    }
#endif
    std::int64_t file_count = 0;
    std::int64_t pending_count = 0;
    auto files = prepare(impl_->db, "SELECT COUNT(*) FROM mb_file_info WHERE status='active';");
    if (sqlite3_step(files.get()) == SQLITE_ROW) {
        file_count = sqlite3_column_int64(files.get(), 0);
    }
    auto pending = prepare(impl_->db, "SELECT COUNT(*) FROM mb_storage_change_tasks WHERE status!='done';");
    if (sqlite3_step(pending.get()) == SQLITE_ROW) {
        pending_count = sqlite3_column_int64(pending.get(), 0);
    }
    return {{"ok", true},
            {"backend", "local_fake"},
            {"root_dir", impl_->root_dir.string()},
            {"file_count", file_count},
            {"pending_artifact_tasks", pending_count},
            {"state_store_attached", impl_->state_store != nullptr}};
}

int CloudStorageService::sync_pending_artifacts(int limit) {
#ifdef MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE
    if (impl_->is_cloud_backend()) {
        return impl_->cloud_sync_pending_artifacts(limit);
    }
#endif
    return impl_->sync_pending_artifacts(limit);
}

std::unique_ptr<CloudStorageService> create_cloud_storage_from_env(
    state::DistributedStateStore* state_store) {
    auto service = std::make_unique<CloudStorageService>(
        mindbridge::app::env_or("MINDBRIDGE_STORAGE_ROOT", ".mindbridge/storage"),
        mindbridge::app::env_or("MINDBRIDGE_STORAGE_PUBLIC_BASE_URL"),
        state_store);
    service->initialize();
    return service;
}

}  // namespace storage
}  // namespace mindbridge
