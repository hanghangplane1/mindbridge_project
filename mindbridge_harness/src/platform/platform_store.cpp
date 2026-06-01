#include "mindbridge/platform/platform_store.hpp"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mindbridge {
namespace platform {
namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void exec_or_throw(sqlite3* db, const std::string& sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : "sqlite error";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

std::string esc(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char ch : value) {
        if (ch == '\'') {
            out += "''";
        } else {
            out += ch;
        }
    }
    return out;
}

std::string join_caps(const std::vector<std::string>& values) {
    return nlohmann::json(values).dump();
}

std::vector<std::string> parse_caps(const std::string& text) {
    const auto parsed = nlohmann::json::parse(text.empty() ? "[]" : text, nullptr, false);
    if (!parsed.is_array()) {
        return {};
    }
    return parsed.get<std::vector<std::string>>();
}

nlohmann::json parse_object(const std::string& text) {
    const auto parsed = nlohmann::json::parse(text.empty() ? "{}" : text, nullptr, false);
    return parsed.is_object() ? parsed : nlohmann::json::object();
}

struct Statement {
    sqlite3_stmt* stmt{nullptr};
    ~Statement() {
        if (stmt) {
            sqlite3_finalize(stmt);
        }
    }
};

Statement prepare(sqlite3* db, const std::string& sql) {
    Statement out;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &out.stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
    return out;
}

std::string col_text(sqlite3_stmt* stmt, int idx) {
    const auto* text = sqlite3_column_text(stmt, idx);
    return text ? reinterpret_cast<const char*>(text) : "";
}

Workspace workspace_from_stmt(sqlite3_stmt* stmt) {
    Workspace workspace;
    workspace.workspace_id = col_text(stmt, 0);
    workspace.owner_user_id = col_text(stmt, 1);
    workspace.namespace_name = col_text(stmt, 2);
    workspace.resource_quota = parse_object(col_text(stmt, 3));
    workspace.created_at = sqlite3_column_int64(stmt, 4);
    return workspace;
}

AgentCore core_from_stmt(sqlite3_stmt* stmt) {
    AgentCore core;
    core.core_id = col_text(stmt, 0);
    core.type = col_text(stmt, 1);
    core.gateway_url = col_text(stmt, 2);
    core.capabilities = parse_caps(col_text(stmt, 3));
    core.created_at = sqlite3_column_int64(stmt, 4);
    return core;
}

AgentSession session_from_stmt(sqlite3_stmt* stmt) {
    AgentSession session;
    session.session_id = col_text(stmt, 0);
    session.workspace_id = col_text(stmt, 1);
    session.user_id = col_text(stmt, 2);
    session.conversation_id = col_text(stmt, 3);
    session.agent_core_id = col_text(stmt, 4);
    session.status = col_text(stmt, 5);
    session.created_at = sqlite3_column_int64(stmt, 6);
    session.updated_at = sqlite3_column_int64(stmt, 7);
    session.last_run_id = col_text(stmt, 8);
    return session;
}

}  // namespace

nlohmann::json to_json(const Workspace& workspace) {
    return {{"workspace_id", workspace.workspace_id},
            {"owner_user_id", workspace.owner_user_id},
            {"namespace", workspace.namespace_name},
            {"resource_quota", workspace.resource_quota},
            {"created_at", workspace.created_at}};
}

nlohmann::json to_json(const AgentCore& core) {
    return {{"core_id", core.core_id},
            {"type", core.type},
            {"gateway_url", core.gateway_url},
            {"capabilities", core.capabilities},
            {"created_at", core.created_at}};
}

nlohmann::json to_json(const AgentSession& session) {
    return {{"session_id", session.session_id},
            {"workspace_id", session.workspace_id},
            {"user_id", session.user_id},
            {"conversation_id", session.conversation_id},
            {"agent_core_id", session.agent_core_id},
            {"status", session.status},
            {"created_at", session.created_at},
            {"updated_at", session.updated_at},
            {"last_run_id", session.last_run_id}};
}

nlohmann::json to_json(const UniversalEvent& event) {
    return {{"event_id", event.event_id},
            {"session_id", event.session_id},
            {"run_id", event.run_id},
            {"type", event.type},
            {"source", event.source},
            {"timestamp_ms", event.timestamp_ms},
            {"payload", event.payload}};
}

struct PlatformStore::Impl {
    sqlite3* db{nullptr};
};

PlatformStore::PlatformStore(std::string db_path) : db_path_(std::move(db_path)), impl_(new Impl()) {}

PlatformStore::~PlatformStore() {
    if (impl_) {
        if (impl_->db) {
            sqlite3_close(impl_->db);
        }
        delete impl_;
    }
}

void PlatformStore::initialize() {
    const auto parent = std::filesystem::path(db_path_).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    if (sqlite3_open(db_path_.c_str(), &impl_->db) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(impl_->db));
    }
    exec_or_throw(impl_->db,
                  "CREATE TABLE IF NOT EXISTS platform_workspaces ("
                  "workspace_id TEXT PRIMARY KEY, owner_user_id TEXT NOT NULL, "
                  "namespace_name TEXT NOT NULL, resource_quota TEXT NOT NULL, created_at INTEGER NOT NULL);"
                  "CREATE TABLE IF NOT EXISTS platform_agent_cores ("
                  "core_id TEXT PRIMARY KEY, type TEXT NOT NULL, gateway_url TEXT NOT NULL, "
                  "capabilities TEXT NOT NULL, created_at INTEGER NOT NULL);"
                  "CREATE TABLE IF NOT EXISTS platform_sessions ("
                  "session_id TEXT PRIMARY KEY, workspace_id TEXT NOT NULL, user_id TEXT NOT NULL, "
                  "conversation_id TEXT NOT NULL, agent_core_id TEXT NOT NULL, status TEXT NOT NULL, "
                  "created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, last_run_id TEXT NOT NULL);");
}

void PlatformStore::create_workspace(Workspace workspace) {
    if (workspace.workspace_id.empty()) {
        throw std::runtime_error("workspace_id is required");
    }
    if (workspace.namespace_name.empty()) {
        workspace.namespace_name = "mindbridge-" + workspace.workspace_id;
    }
    if (workspace.created_at == 0) {
        workspace.created_at = now_ms();
    }
    exec_or_throw(impl_->db,
                  "INSERT INTO platform_workspaces(workspace_id,owner_user_id,namespace_name,resource_quota,created_at) "
                  "VALUES('" +
                      esc(workspace.workspace_id) + "','" + esc(workspace.owner_user_id) + "','" +
                      esc(workspace.namespace_name) + "','" + esc(workspace.resource_quota.dump()) + "'," +
                      std::to_string(workspace.created_at) + ")");
}

std::optional<Workspace> PlatformStore::get_workspace(const std::string& workspace_id) const {
    auto stmt = prepare(impl_->db,
                        "SELECT workspace_id,owner_user_id,namespace_name,resource_quota,created_at "
                        "FROM platform_workspaces WHERE workspace_id='" +
                            esc(workspace_id) + "'");
    if (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
        return workspace_from_stmt(stmt.stmt);
    }
    return std::nullopt;
}

std::vector<Workspace> PlatformStore::list_workspaces(const std::string& owner_user_id) const {
    std::string sql =
        "SELECT workspace_id,owner_user_id,namespace_name,resource_quota,created_at FROM platform_workspaces";
    if (!owner_user_id.empty()) {
        sql += " WHERE owner_user_id='" + esc(owner_user_id) + "'";
    }
    sql += " ORDER BY created_at DESC";
    auto stmt = prepare(impl_->db, sql);
    std::vector<Workspace> out;
    while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
        out.push_back(workspace_from_stmt(stmt.stmt));
    }
    return out;
}

void PlatformStore::upsert_agent_core(AgentCore core) {
    if (core.core_id.empty()) {
        throw std::runtime_error("core_id is required");
    }
    if (core.created_at == 0) {
        core.created_at = now_ms();
    }
    exec_or_throw(impl_->db,
                  "INSERT INTO platform_agent_cores(core_id,type,gateway_url,capabilities,created_at) VALUES('" +
                      esc(core.core_id) + "','" + esc(core.type) + "','" + esc(core.gateway_url) + "','" +
                      esc(join_caps(core.capabilities)) + "'," + std::to_string(core.created_at) +
                      ") ON CONFLICT(core_id) DO UPDATE SET type=excluded.type,gateway_url=excluded.gateway_url,"
                      "capabilities=excluded.capabilities");
}

std::optional<AgentCore> PlatformStore::get_agent_core(const std::string& core_id) const {
    auto stmt = prepare(impl_->db,
                        "SELECT core_id,type,gateway_url,capabilities,created_at FROM platform_agent_cores "
                        "WHERE core_id='" +
                            esc(core_id) + "'");
    if (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
        return core_from_stmt(stmt.stmt);
    }
    return std::nullopt;
}

std::vector<AgentCore> PlatformStore::list_agent_cores() const {
    auto stmt = prepare(impl_->db,
                        "SELECT core_id,type,gateway_url,capabilities,created_at FROM platform_agent_cores "
                        "ORDER BY created_at DESC");
    std::vector<AgentCore> out;
    while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
        out.push_back(core_from_stmt(stmt.stmt));
    }
    return out;
}

void PlatformStore::create_session(AgentSession session) {
    if (session.session_id.empty() || session.workspace_id.empty()) {
        throw std::runtime_error("session_id and workspace_id are required");
    }
    const auto ts = now_ms();
    if (session.created_at == 0) {
        session.created_at = ts;
    }
    if (session.updated_at == 0) {
        session.updated_at = ts;
    }
    exec_or_throw(impl_->db,
                  "INSERT INTO platform_sessions(session_id,workspace_id,user_id,conversation_id,agent_core_id,status,"
                  "created_at,updated_at,last_run_id) VALUES('" +
                      esc(session.session_id) + "','" + esc(session.workspace_id) + "','" + esc(session.user_id) +
                      "','" + esc(session.conversation_id) + "','" + esc(session.agent_core_id) + "','" +
                      esc(session.status) + "'," + std::to_string(session.created_at) + "," +
                      std::to_string(session.updated_at) + ",'" + esc(session.last_run_id) + "')");
}

std::optional<AgentSession> PlatformStore::get_session(const std::string& session_id) const {
    auto stmt = prepare(impl_->db,
                        "SELECT session_id,workspace_id,user_id,conversation_id,agent_core_id,status,created_at,"
                        "updated_at,last_run_id FROM platform_sessions WHERE session_id='" +
                            esc(session_id) + "'");
    if (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
        return session_from_stmt(stmt.stmt);
    }
    return std::nullopt;
}

std::vector<AgentSession> PlatformStore::list_sessions(const std::string& workspace_id,
                                                       const std::string& user_id) const {
    std::string sql =
        "SELECT session_id,workspace_id,user_id,conversation_id,agent_core_id,status,created_at,updated_at,last_run_id "
        "FROM platform_sessions WHERE 1=1";
    if (!workspace_id.empty()) {
        sql += " AND workspace_id='" + esc(workspace_id) + "'";
    }
    if (!user_id.empty()) {
        sql += " AND user_id='" + esc(user_id) + "'";
    }
    sql += " ORDER BY updated_at DESC";
    auto stmt = prepare(impl_->db, sql);
    std::vector<AgentSession> out;
    while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
        out.push_back(session_from_stmt(stmt.stmt));
    }
    return out;
}

void PlatformStore::update_session_status(const std::string& session_id, const std::string& status) {
    exec_or_throw(impl_->db,
                  "UPDATE platform_sessions SET status='" + esc(status) + "',updated_at=" +
                      std::to_string(now_ms()) + " WHERE session_id='" + esc(session_id) + "'");
}

void PlatformStore::update_session_run_id(const std::string& session_id, const std::string& run_id) {
    exec_or_throw(impl_->db,
                  "UPDATE platform_sessions SET last_run_id='" + esc(run_id) + "',updated_at=" +
                      std::to_string(now_ms()) + " WHERE session_id='" + esc(session_id) + "'");
}

}  // namespace platform
}  // namespace mindbridge
