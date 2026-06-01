#include "mindbridge/auth/auth_service.hpp"

#include "mindbridge/runtime/utf8.hpp"

#include <nlohmann/json.hpp>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <sqlite3.h>

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mindbridge {
namespace auth {
namespace {

using SqlitePtr = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using StmtPtr = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

long long now_sec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string env_or(const char* key, const std::string& fallback = "") {
    const char* value = std::getenv(key);
    return value && *value ? std::string(value) : fallback;
}

int env_int(const char* key, int fallback) {
    const char* value = std::getenv(key);
    if (!value || !*value) {
        return fallback;
    }
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

SqlitePtr open_db(const std::string& path) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    sqlite3* raw = nullptr;
    if (sqlite3_open(path.c_str(), &raw) != SQLITE_OK) {
        std::string error = raw ? sqlite3_errmsg(raw) : "sqlite open failed";
        if (raw) {
            sqlite3_close(raw);
        }
        throw std::runtime_error(error);
    }
    return SqlitePtr(raw, sqlite3_close);
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

LoginResult login_error(const std::string& message) {
    LoginResult result;
    result.ok = false;
    result.error = message;
    return result;
}

std::string col_text(sqlite3_stmt* stmt, int index) {
    const unsigned char* text = sqlite3_column_text(stmt, index);
    return text ? reinterpret_cast<const char*>(text) : "";
}

void bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

void step_done(sqlite3* db, sqlite3_stmt* stmt) {
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

std::string hex(const unsigned char* bytes, std::size_t size) {
    std::ostringstream out;
    for (std::size_t i = 0; i < size; ++i) {
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[i]);
    }
    return out.str();
}

std::string random_hex(std::size_t bytes) {
    std::string raw(bytes, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&raw[0]), static_cast<int>(raw.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    return hex(reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
}

std::string sha256_hex(const std::string& value) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
    return hex(digest, sizeof(digest));
}

std::string iso_time(long long seconds) {
    return std::to_string(seconds);
}

std::string token_hash(const std::string& token) {
    return sha256_hex("token:" + token);
}

std::string safe_id(std::string value, const std::string& fallback) {
    value = sanitize_utf8(value);
    std::string out;
    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.') {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out.empty() ? fallback : out;
}

}  // namespace

AuthService::AuthService(std::string db_path) : db_path_(std::move(db_path)) {}

void AuthService::initialize() {
    auto db = open_db(db_path_);
    exec(db.get(), "PRAGMA journal_mode=WAL;");
    exec(db.get(), "CREATE TABLE IF NOT EXISTS auth_users ("
                   "user_id TEXT PRIMARY KEY,"
                   "display_name TEXT NOT NULL,"
                   "role TEXT NOT NULL,"
                   "disabled INTEGER NOT NULL DEFAULT 0,"
                   "created_at INTEGER NOT NULL);");
    exec(db.get(), "CREATE TABLE IF NOT EXISTS auth_access_keys ("
                   "access_key_id TEXT PRIMARY KEY,"
                   "user_id TEXT NOT NULL,"
                   "secret_hash TEXT NOT NULL,"
                   "valid_until INTEGER NOT NULL DEFAULT 0,"
                   "disabled INTEGER NOT NULL DEFAULT 0,"
                   "failed_attempts INTEGER NOT NULL DEFAULT 0,"
                   "locked_until INTEGER NOT NULL DEFAULT 0);");
    exec(db.get(), "CREATE TABLE IF NOT EXISTS auth_sessions ("
                   "token_hash TEXT PRIMARY KEY,"
                   "user_id TEXT NOT NULL,"
                   "issued_at INTEGER NOT NULL,"
                   "expires_at INTEGER NOT NULL,"
                   "revoked_at INTEGER NOT NULL DEFAULT 0,"
                   "last_seen_at INTEGER NOT NULL);");
    exec(db.get(), "CREATE TABLE IF NOT EXISTS conversation_turns ("
                   "user_id TEXT NOT NULL,"
                   "conversation_id TEXT NOT NULL,"
                   "role TEXT NOT NULL,"
                   "content TEXT NOT NULL,"
                   "run_id TEXT,"
                   "created_at INTEGER NOT NULL);");
    exec(db.get(), "CREATE TABLE IF NOT EXISTS run_ownership ("
                   "run_id TEXT PRIMARY KEY,"
                   "user_id TEXT NOT NULL,"
                   "conversation_id TEXT NOT NULL,"
                   "created_at INTEGER NOT NULL);");
}

void AuthService::bootstrap_from_env() {
    const std::string raw = env_or("MINDBRIDGE_AUTH_BOOTSTRAP_USERS");
    if (raw.empty()) {
        return;
    }
    auto db = open_db(db_path_);
    const auto users = nlohmann::json::parse(raw);
    if (!users.is_array()) {
        throw std::runtime_error("MINDBRIDGE_AUTH_BOOTSTRAP_USERS must be a JSON array");
    }
    for (const auto& user : users) {
        const std::string user_id = safe_id(user.value("user_id", ""), "");
        const std::string access_key_id = safe_id(user.value("access_key_id", ""), "");
        const std::string secret_key = user.value("secret_key", "");
        if (user_id.empty() || access_key_id.empty() || secret_key.empty()) {
            continue;
        }
        const std::string salt = user.value("salt", random_hex(16));
        const std::string secret_hash = hash_secret_for_bootstrap(salt, secret_key);
        {
            auto stmt = prepare(db.get(), "INSERT OR IGNORE INTO auth_users "
                                         "(user_id, display_name, role, disabled, created_at) "
                                         "VALUES (?, ?, ?, 0, ?);");
            bind_text(stmt.get(), 1, user_id);
            bind_text(stmt.get(), 2, sanitize_utf8(user.value("display_name", user_id)));
            bind_text(stmt.get(), 3, safe_id(user.value("role", "user"), "user"));
            sqlite3_bind_int64(stmt.get(), 4, now_sec());
            step_done(db.get(), stmt.get());
        }
        {
            auto stmt = prepare(db.get(), "INSERT OR REPLACE INTO auth_access_keys "
                                         "(access_key_id, user_id, secret_hash, valid_until, disabled, failed_attempts, locked_until) "
                                         "VALUES (?, ?, ?, ?, 0, 0, 0);");
            bind_text(stmt.get(), 1, access_key_id);
            bind_text(stmt.get(), 2, user_id);
            bind_text(stmt.get(), 3, secret_hash);
            sqlite3_bind_int64(stmt.get(), 4, user.value("valid_until", 0LL));
            step_done(db.get(), stmt.get());
        }
    }
}

LoginResult AuthService::login(const std::string& access_key_id, const std::string& secret_key) {
    auto db = open_db(db_path_);
    auto stmt = prepare(db.get(), "SELECT k.user_id, k.secret_hash, k.valid_until, k.disabled, "
                                 "k.failed_attempts, k.locked_until, u.display_name, u.role, u.disabled "
                                 "FROM auth_access_keys k JOIN auth_users u ON u.user_id = k.user_id "
                                 "WHERE k.access_key_id = ?;");
    bind_text(stmt.get(), 1, access_key_id);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return login_error("invalid access key");
    }
    const std::string user_id = col_text(stmt.get(), 0);
    const std::string stored_hash = col_text(stmt.get(), 1);
    const long long valid_until = sqlite3_column_int64(stmt.get(), 2);
    const bool key_disabled = sqlite3_column_int(stmt.get(), 3) != 0;
    const int failed_attempts = sqlite3_column_int(stmt.get(), 4);
    const long long locked_until = sqlite3_column_int64(stmt.get(), 5);
    const std::string display_name = col_text(stmt.get(), 6);
    const std::string role = col_text(stmt.get(), 7);
    const bool user_disabled = sqlite3_column_int(stmt.get(), 8) != 0;
    const long long now = now_sec();
    if (user_disabled || key_disabled) {
        return login_error("user or key disabled");
    }
    if (valid_until > 0 && valid_until < now) {
        return login_error("access key expired");
    }
    if (locked_until > now) {
        return login_error("access key locked");
    }
    const auto salt_begin = stored_hash.find(':');
    const auto salt_end = stored_hash.find(':', salt_begin == std::string::npos ? 0 : salt_begin + 1);
    if (salt_begin == std::string::npos || salt_end == std::string::npos) {
        return login_error("invalid key format");
    }
    const std::string salt = stored_hash.substr(salt_begin + 1, salt_end - salt_begin - 1);
    const std::string provided = hash_secret_for_bootstrap(salt, secret_key);
    if (provided != stored_hash) {
        const int next_failures = failed_attempts + 1;
        auto update = prepare(db.get(), "UPDATE auth_access_keys SET failed_attempts = ?, locked_until = ? WHERE access_key_id = ?;");
        sqlite3_bind_int(update.get(), 1, next_failures);
        sqlite3_bind_int64(update.get(), 2, next_failures >= 5 ? now + 15 * 60 : 0);
        bind_text(update.get(), 3, access_key_id);
        step_done(db.get(), update.get());
        return login_error("invalid secret");
    }
    {
        auto update = prepare(db.get(), "UPDATE auth_access_keys SET failed_attempts = 0, locked_until = 0 WHERE access_key_id = ?;");
        bind_text(update.get(), 1, access_key_id);
        step_done(db.get(), update.get());
    }
    const int ttl = env_int("MINDBRIDGE_AUTH_SESSION_TTL_SEC", 8 * 60 * 60);
    const std::string token = random_hex(32);
    const long long expires_at = now + ttl;
    auto insert = prepare(db.get(), "INSERT INTO auth_sessions "
                                  "(token_hash, user_id, issued_at, expires_at, revoked_at, last_seen_at) "
                                  "VALUES (?, ?, ?, ?, 0, ?);");
    bind_text(insert.get(), 1, token_hash(token));
    bind_text(insert.get(), 2, user_id);
    sqlite3_bind_int64(insert.get(), 3, now);
    sqlite3_bind_int64(insert.get(), 4, expires_at);
    sqlite3_bind_int64(insert.get(), 5, now);
    step_done(db.get(), insert.get());
    LoginResult result;
    result.ok = true;
    result.token = token;
    result.expires_at = iso_time(expires_at);
    result.identity = {user_id, display_name, role, token_hash(token)};
    return result;
}

std::optional<AuthIdentity> AuthService::authenticate_token(const std::string& token) {
    if (token.empty()) {
        return std::nullopt;
    }
    auto db = open_db(db_path_);
    auto stmt = prepare(db.get(), "SELECT s.user_id, s.expires_at, s.revoked_at, u.display_name, u.role, u.disabled "
                                 "FROM auth_sessions s JOIN auth_users u ON u.user_id = s.user_id "
                                 "WHERE s.token_hash = ?;");
    const std::string hashed = token_hash(token);
    bind_text(stmt.get(), 1, hashed);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return std::nullopt;
    }
    const long long now = now_sec();
    const long long expires_at = sqlite3_column_int64(stmt.get(), 1);
    const long long revoked_at = sqlite3_column_int64(stmt.get(), 2);
    const bool disabled = sqlite3_column_int(stmt.get(), 5) != 0;
    if (disabled || revoked_at > 0 || expires_at <= now) {
        return std::nullopt;
    }
    auto update = prepare(db.get(), "UPDATE auth_sessions SET last_seen_at = ? WHERE token_hash = ?;");
    sqlite3_bind_int64(update.get(), 1, now);
    bind_text(update.get(), 2, hashed);
    step_done(db.get(), update.get());
    return AuthIdentity{col_text(stmt.get(), 0), col_text(stmt.get(), 3), col_text(stmt.get(), 4), hashed};
}

void AuthService::logout(const std::string& token) {
    auto db = open_db(db_path_);
    auto stmt = prepare(db.get(), "UPDATE auth_sessions SET revoked_at = ? WHERE token_hash = ?;");
    sqlite3_bind_int64(stmt.get(), 1, now_sec());
    bind_text(stmt.get(), 2, token_hash(token));
    step_done(db.get(), stmt.get());
}

std::string AuthService::scoped_conversation_id(const AuthIdentity& identity,
                                                const std::string& client_context_id) const {
    return safe_id(identity.user_id, "user") + "__" + safe_id(client_context_id, "default");
}

void AuthService::record_turn(const AuthIdentity& identity,
                              const std::string& conversation_id,
                              const std::string& role,
                              const std::string& content,
                              const std::string& run_id) {
    auto db = open_db(db_path_);
    auto stmt = prepare(db.get(), "INSERT INTO conversation_turns "
                                 "(user_id, conversation_id, role, content, run_id, created_at) "
                                 "VALUES (?, ?, ?, ?, ?, ?);");
    bind_text(stmt.get(), 1, identity.user_id);
    bind_text(stmt.get(), 2, conversation_id);
    bind_text(stmt.get(), 3, safe_id(role, "user"));
    bind_text(stmt.get(), 4, sanitize_utf8(content));
    bind_text(stmt.get(), 5, run_id);
    sqlite3_bind_int64(stmt.get(), 6, now_sec());
    step_done(db.get(), stmt.get());
}

void AuthService::record_run(const AuthIdentity& identity,
                             const std::string& conversation_id,
                             const std::string& run_id) {
    if (run_id.empty()) {
        return;
    }
    auto db = open_db(db_path_);
    auto stmt = prepare(db.get(), "INSERT OR REPLACE INTO run_ownership "
                                 "(run_id, user_id, conversation_id, created_at) VALUES (?, ?, ?, ?);");
    bind_text(stmt.get(), 1, run_id);
    bind_text(stmt.get(), 2, identity.user_id);
    bind_text(stmt.get(), 3, conversation_id);
    sqlite3_bind_int64(stmt.get(), 4, now_sec());
    step_done(db.get(), stmt.get());
}

bool AuthService::owns_run(const AuthIdentity& identity, const std::string& run_id) const {
    auto db = open_db(db_path_);
    auto stmt = prepare(db.get(), "SELECT 1 FROM run_ownership WHERE run_id = ? AND user_id = ?;");
    bind_text(stmt.get(), 1, run_id);
    bind_text(stmt.get(), 2, identity.user_id);
    return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

std::string AuthService::latest_run_id_for(const AuthIdentity& identity) const {
    auto db = open_db(db_path_);
    auto stmt = prepare(db.get(), "SELECT run_id FROM run_ownership WHERE user_id = ? ORDER BY created_at DESC LIMIT 1;");
    bind_text(stmt.get(), 1, identity.user_id);
    return sqlite3_step(stmt.get()) == SQLITE_ROW ? col_text(stmt.get(), 0) : "";
}

nlohmann::json AuthService::list_conversations(const AuthIdentity& identity) const {
    auto db = open_db(db_path_);
    auto stmt = prepare(db.get(), "SELECT conversation_id, MAX(created_at), COUNT(*) "
                                 "FROM conversation_turns WHERE user_id = ? GROUP BY conversation_id ORDER BY MAX(created_at) DESC;");
    bind_text(stmt.get(), 1, identity.user_id);
    nlohmann::json out = nlohmann::json::array();
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        out.push_back({{"conversation_id", col_text(stmt.get(), 0)},
                       {"updated_at", sqlite3_column_int64(stmt.get(), 1)},
                       {"turns", sqlite3_column_int(stmt.get(), 2)}});
    }
    return out;
}

nlohmann::json AuthService::conversation_history(const AuthIdentity& identity,
                                                 const std::string& conversation_id) const {
    auto db = open_db(db_path_);
    auto stmt = prepare(db.get(), "SELECT role, content, run_id, created_at FROM conversation_turns "
                                 "WHERE user_id = ? AND conversation_id = ? ORDER BY created_at ASC;");
    bind_text(stmt.get(), 1, identity.user_id);
    bind_text(stmt.get(), 2, conversation_id);
    nlohmann::json out = nlohmann::json::array();
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        out.push_back({{"role", col_text(stmt.get(), 0)},
                       {"content", col_text(stmt.get(), 1)},
                       {"run_id", col_text(stmt.get(), 2)},
                       {"created_at", sqlite3_column_int64(stmt.get(), 3)}});
    }
    return out;
}

std::string AuthService::hash_secret_for_bootstrap(const std::string& salt,
                                                   const std::string& secret) {
    return "sha256:" + salt + ":" + sha256_hex("mindbridge:" + salt + ":" + secret);
}

}  // namespace auth
}  // namespace mindbridge
