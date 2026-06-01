/**
 * MindBridge Gateway：统一健康检查、request_id 注入、轻量鉴权占位和 JSON-RPC 转发。
 *
 * 环境变量 MINDBRIDGE_ORCHESTRATOR_URL 默认 http://127.0.0.1:5009
 * 可选 MINDBRIDGE_GATEWAY_TOKEN：如果设置，请求 body 需带 metadata.gateway_token。
 */
#include "mindbridge/apps/app_support.hpp"
#include "mindbridge/auth/auth_service.hpp"
#include "mindbridge/harness/tool_registry.hpp"
#include "mindbridge/net/async_http_server.hpp"
#include "mindbridge/net/managed_endpoint_router.hpp"
#include "mindbridge/speech/speech_tools.hpp"
#include "mindbridge/state/distributed_state_store.hpp"
#include "mindbridge/storage/cloud_storage.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <sstream>
#include <optional>
#include <vector>

namespace {

int env_int(const char* key, int fallback);

std::string trim_copy(const std::string& value) {
    auto begin = value.begin();
    while (begin != value.end() && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    auto end = value.end();
    while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }
    return std::string(begin, end);
}

std::vector<std::string> split_urls(const std::string& raw) {
    std::vector<std::string> urls;
    std::stringstream input(raw);
    std::string item;
    while (std::getline(input, item, ',')) {
        item = trim_copy(item);
        if (!item.empty()) {
            urls.push_back(item);
        }
    }
    return urls;
}

std::vector<std::string> orchestrator_urls() {
    auto urls = split_urls(mindbridge::app::env_or("MINDBRIDGE_ORCHESTRATOR_URLS"));
    if (urls.empty()) {
        urls.push_back(mindbridge::app::env_or("MINDBRIDGE_ORCHESTRATOR_URL", "http://127.0.0.1:5009"));
    }
    return urls;
}

mindbridge::net::EndpointRouterOptions endpoint_router_options() {
    mindbridge::net::EndpointRouterOptions options;
    options.health_interval_ms = env_int("MINDBRIDGE_HEALTH_INTERVAL_MS", 2000);
    options.health_fail_threshold = env_int("MINDBRIDGE_HEALTH_FAIL_THRESHOLD", 2);
    options.health_recover_threshold = env_int("MINDBRIDGE_HEALTH_RECOVER_THRESHOLD", 1);
    options.circuit_open_ms = env_int("MINDBRIDGE_CIRCUIT_OPEN_MS", 10000);
    options.health_timeout_sec = env_int("MINDBRIDGE_HEALTH_TIMEOUT_SEC", 2);
    options.client_options.verify_tls =
        mindbridge::app::env_or("MINDBRIDGE_NET_TLS_VERIFY", "true") != "false";
    options.client_options.ca_file = mindbridge::app::env_or("MINDBRIDGE_NET_CA_FILE");
    return options;
}

mindbridge::net::ManagedEndpointRouter& orchestrator_router() {
    static mindbridge::net::ManagedEndpointRouter router(
        "orchestrator", "orchestrator", orchestrator_urls(), endpoint_router_options());
    return router;
}

std::string downstream_url() {
    return orchestrator_router().select("gateway", "gateway");
}

mindbridge::ToolRegistry& speech_tools() {
    static std::unique_ptr<mindbridge::ToolRegistry> registry = [] {
        auto tools = std::make_unique<mindbridge::ToolRegistry>();
        mindbridge::register_speech_tools(*tools);
        return tools;
    }();
    return *registry;
}

bool auth_required() {
    return mindbridge::app::env_or("MINDBRIDGE_AUTH_REQUIRED", "false") == "true";
}

mindbridge::auth::AuthService& auth_service() {
    static std::unique_ptr<mindbridge::auth::AuthService> service = [] {
        auto auth = std::make_unique<mindbridge::auth::AuthService>(
            mindbridge::app::env_or("MINDBRIDGE_AUTH_DB_PATH", ".mindbridge/mindbridge.db"));
        auth->initialize();
        auth->bootstrap_from_env();
        return auth;
    }();
    return *service;
}

mindbridge::state::DistributedStateStore& storage_state_store() {
    static std::unique_ptr<mindbridge::state::DistributedStateStore> store = [] {
        std::string db_path = mindbridge::app::env_or("MINDBRIDGE_STATE_DB_PATH");
        if (db_path.empty()) {
            db_path = mindbridge::app::env_or("MINDBRIDGE_STORAGE_STATE_DB_PATH",
                                              ".mindbridge/state/counselor_master.sqlite");
        }
        auto state = std::make_unique<mindbridge::state::DistributedStateStore>(
            db_path,
            mindbridge::app::env_or("MINDBRIDGE_GATEWAY_STATE_NODE_ID", "gateway-state"));
        state->initialize();
        return state;
    }();
    return *store;
}

mindbridge::storage::CloudStorageService& cloud_storage() {
    static std::unique_ptr<mindbridge::storage::CloudStorageService> service =
        mindbridge::storage::create_cloud_storage_from_env(&storage_state_store());
    return *service;
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::map<std::string, std::string> query_params(const std::string& target) {
    std::map<std::string, std::string> out;
    const auto pos = target.find('?');
    if (pos == std::string::npos) {
        return out;
    }
    std::stringstream input(target.substr(pos + 1));
    std::string item;
    while (std::getline(input, item, '&')) {
        const auto eq = item.find('=');
        if (eq == std::string::npos) {
            out[item] = "";
        } else {
            out[item.substr(0, eq)] = item.substr(eq + 1);
        }
    }
    return out;
}

std::string header_value(const mindbridge::net::AsyncHttpServer::RequestContext& ctx,
                         const std::string& name) {
    const std::string wanted = lower_copy(name);
    for (const auto& item : ctx.headers) {
        if (lower_copy(item.first) == wanted) {
            return item.second;
        }
    }
    return "";
}

std::string token_from_context(const mindbridge::net::AsyncHttpServer::RequestContext& ctx,
                               const nlohmann::json* parsed_body = nullptr) {
    std::string auth = header_value(ctx, "Authorization");
    const std::string prefix = "Bearer ";
    if (auth.rfind(prefix, 0) == 0) {
        return auth.substr(prefix.size());
    }
    const auto query = query_params(ctx.target);
    const auto token_it = query.find("auth_token");
    if (token_it != query.end()) {
        return token_it->second;
    }
    if (parsed_body && parsed_body->is_object()) {
        const auto metadata = parsed_body->value("metadata", nlohmann::json::object());
        if (metadata.is_object()) {
            return metadata.value("auth_token", "");
        }
        return parsed_body->value("auth_token", "");
    }
    if (!ctx.body.empty()) {
        try {
            const auto body = nlohmann::json::parse(ctx.body);
            return token_from_context(ctx, &body);
        } catch (...) {
        }
    }
    return "";
}

std::optional<mindbridge::auth::AuthIdentity> identity_from_context(
    const mindbridge::net::AsyncHttpServer::RequestContext& ctx,
    const nlohmann::json* parsed_body = nullptr) {
    return auth_service().authenticate_token(token_from_context(ctx, parsed_body));
}

mindbridge::auth::AuthIdentity anonymous_identity() {
    return {"anonymous", "Anonymous Session", "user", "anonymous"};
}

std::optional<mindbridge::auth::AuthIdentity> visible_history_identity(
    const mindbridge::net::AsyncHttpServer::RequestContext& ctx,
    const nlohmann::json* parsed_body = nullptr) {
    auto identity = identity_from_context(ctx, parsed_body);
    if (identity.has_value()) {
        return identity;
    }
    if (!auth_required()) {
        return anonymous_identity();
    }
    return std::nullopt;
}

std::string unauthorized_json(const std::string& id = "unknown") {
    return mindbridge::app::json_error(id, 401, "authentication required");
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

int env_int(const char* key, int fallback) {
    const char* value = std::getenv(key);
    if (!value || std::string(value).empty()) {
        return fallback;
    }
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool looks_like_dashscope_system_voice(const std::string& voice) {
    return starts_with(voice, "long") || starts_with(voice, "loong");
}

std::string dashscope_tts_model_for_voice(std::string model, const std::string& voice) {
    if (looks_like_dashscope_system_voice(voice) &&
        (model == "cosyvoice-v3.5-plus" || model == "cosyvoice-v3.5-flash")) {
        return "cosyvoice-v3-flash";
    }
    return model;
}

std::string dashscope_voice_from_env() {
    return mindbridge::app::env_or(
        "MINDBRIDGE_DASHSCOPE_TTS_VOICE_ID",
        mindbridge::app::env_or("MINDBRIDGE_DASHSCOPE_TTS_FALLBACK_VOICE", "longanyang"));
}

std::filesystem::path workspace_root() {
    return std::filesystem::current_path();
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open " + path.string());
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

nlohmann::json read_json_file(const std::filesystem::path& path) {
    return nlohmann::json::parse(read_text_file(path));
}

std::vector<nlohmann::json> read_trace_tail(const std::filesystem::path& path, size_t limit = 40) {
    std::ifstream in(path);
    if (!in) {
        return {};
    }
    std::deque<nlohmann::json> tail;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        try {
            tail.push_back(nlohmann::json::parse(line));
        } catch (...) {
            tail.push_back({{"raw", line}});
        }
        while (tail.size() > limit) {
            tail.pop_front();
        }
    }
    return {tail.begin(), tail.end()};
}

std::vector<nlohmann::json> read_jsonl_tail(const std::filesystem::path& path, size_t limit = 200) {
    return read_trace_tail(path, limit);
}

bool valid_run_id(const std::string& value) {
    if (value.empty() || value == "." || value == "..") {
        return false;
    }
    for (unsigned char ch : value) {
        const bool ok = std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::filesystem::path runs_root() {
    return workspace_root() / ".mindbridge" / "runs";
}

std::string latest_run_id() {
    const std::filesystem::path root = runs_root();
    if (!std::filesystem::exists(root)) {
        return "";
    }
    std::filesystem::path latest;
    std::filesystem::file_time_type latest_time{};
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_directory()) {
            continue;
        }
        const auto updated = entry.last_write_time();
        if (latest.empty() || updated > latest_time) {
            latest = entry.path();
            latest_time = updated;
        }
    }
    return latest.empty() ? "" : latest.filename().string();
}

nlohmann::json run_payload(const std::string& run_id) {
    if (!valid_run_id(run_id)) {
        return {{"ok", false}, {"error", "invalid run_id"}};
    }
    const std::filesystem::path dir = runs_root() / run_id;
    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
        return {{"ok", false}, {"error", "run not found"}, {"run_id", run_id}};
    }
    nlohmann::json out = {
        {"ok", true},
        {"run_id", run_id},
        {"run_dir", dir.string()},
        {"task_state", nlohmann::json::object()},
        {"report", nlohmann::json::object()},
        {"trace", nlohmann::json::array()},
    };
    try {
        out["task_state"] = read_json_file(dir / "task_state.json");
    } catch (const std::exception& e) {
        out["task_state_error"] = e.what();
    }
    try {
        out["report"] = read_json_file(dir / "report.json");
    } catch (const std::exception& e) {
        out["report_error"] = e.what();
    }
    out["trace"] = read_trace_tail(dir / "trace.jsonl");
    return out;
}

nlohmann::json run_observability_payload_json(const std::string& run_id) {
    if (!valid_run_id(run_id)) {
        return {{"ok", false}, {"error", "invalid run_id"}};
    }
    const std::filesystem::path dir = runs_root() / run_id;
    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
        return {{"ok", false}, {"error", "run not found"}, {"run_id", run_id}};
    }
    nlohmann::json out = {
        {"ok", true},
        {"run_id", run_id},
        {"run_dir", dir.string()},
        {"capture_boundaries", nlohmann::json::array({"A2L", "A2T_COMMAND", "A2T_NETWORK", "TLS_PLAINTEXT", "MCP_STDIO", "A2A"})},
        {"ebpf_events_tail", read_jsonl_tail(dir / "ebpf_events.jsonl", 200)},
        {"boundary_trace", read_jsonl_tail(dir / "boundary_trace.jsonl", 400)},
        {"observability_report", nlohmann::json::object()},
    };
    try {
        out["observability_report"] = read_json_file(dir / "observability_report.json");
    } catch (const std::exception& e) {
        out["observability_report_error"] = e.what();
    }
    return out;
}

std::string latest_run_payload() {
    const std::string run_id = latest_run_id();
    if (run_id.empty()) {
        return nlohmann::json{{"ok", false}, {"error", "no runs found"}}.dump();
    }
    return run_payload(run_id).dump();
}

std::string run_payload_for(const std::optional<mindbridge::auth::AuthIdentity>& identity,
                            const std::string& run_id) {
    if (auth_required()) {
        if (!identity.has_value()) {
            return unauthorized_json();
        }
        if (!auth_service().owns_run(*identity, run_id)) {
            return mindbridge::app::json_error("unknown", 403, "run does not belong to current user");
        }
    }
    return run_payload(run_id).dump();
}

std::string run_observability_payload_for(const std::optional<mindbridge::auth::AuthIdentity>& identity,
                                          const std::string& run_id) {
    if (auth_required()) {
        if (!identity.has_value()) {
            return unauthorized_json();
        }
        if (!auth_service().owns_run(*identity, run_id)) {
            return mindbridge::app::json_error("unknown", 403, "run does not belong to current user");
        }
    }
    return run_observability_payload_json(run_id).dump();
}

std::string feature_status_payload() {
    try {
        return read_json_file(workspace_root() / "mindbridge_harness" / "configs" / "feature_status.json").dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"ok", false}, {"error", e.what()}}.dump();
    }
}

std::string auth_login_payload(const std::string& body) {
    try {
        const auto request = nlohmann::json::parse(body);
        const auto result = auth_service().login(request.value("access_key_id", ""),
                                                request.value("secret_key", ""));
        if (!result.ok) {
            return nlohmann::json{{"ok", false}, {"error", result.error}}.dump();
        }
        return nlohmann::json{{"ok", true},
                              {"token", result.token},
                              {"expires_at", result.expires_at},
                              {"user",
                               {{"user_id", result.identity.user_id},
                                {"display_name", result.identity.display_name},
                                {"role", result.identity.role}}}}
            .dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"ok", false}, {"error", e.what()}}.dump();
    }
}

std::string auth_status_payload() {
    return nlohmann::json{{"ok", true}, {"required", auth_required()}}.dump();
}

std::string auth_logout_payload(const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
    auth_service().logout(token_from_context(ctx));
    return nlohmann::json{{"ok", true}}.dump();
}

std::string auth_me_payload(const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
    const auto identity = identity_from_context(ctx);
    if (!identity.has_value()) {
        return unauthorized_json();
    }
    return nlohmann::json{{"ok", true},
                          {"user",
                           {{"user_id", identity->user_id},
                            {"display_name", identity->display_name},
                            {"role", identity->role}}}}
        .dump();
}

std::string conversations_payload(const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
    const auto identity = visible_history_identity(ctx);
    if (!identity.has_value()) {
        return unauthorized_json();
    }
    return nlohmann::json{{"ok", true},
                          {"source", "distributed_state_store_master"},
                          {"conversations", storage_state_store().list_conversations(identity->user_id)}}
        .dump();
}

std::string conversation_payload(const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
    const auto identity = visible_history_identity(ctx);
    if (!identity.has_value()) {
        return unauthorized_json();
    }
    const std::string prefix = "/api/conversations/";
    if (ctx.path.rfind(prefix, 0) != 0 || ctx.path.size() <= prefix.size()) {
        return nlohmann::json{{"ok", false}, {"error", "missing conversation id"}}.dump();
    }
    const std::string conversation_id = ctx.path.substr(prefix.size());
    return nlohmann::json{{"ok", true},
                          {"source", "distributed_state_store_master"},
                          {"conversation_id", conversation_id},
                          {"turns", storage_state_store().conversation_history(identity->user_id, conversation_id)}}
        .dump();
}

std::string json_value(const nlohmann::json& object,
                       const std::string& first,
                       const std::string& second = "",
                       const std::string& fallback = "") {
    if (object.is_object() && object.contains(first) && object[first].is_string()) {
        return object[first].get<std::string>();
    }
    if (!second.empty() && object.is_object() && object.contains(second) && object[second].is_string()) {
        return object[second].get<std::string>();
    }
    return fallback;
}

nlohmann::json parse_json_body(const std::string& body) {
    if (body.empty()) {
        return nlohmann::json::object();
    }
    return nlohmann::json::parse(body);
}

mindbridge::storage::StorageIdentity storage_identity_from(
    const mindbridge::net::AsyncHttpServer::RequestContext& ctx,
    const nlohmann::json& body = nlohmann::json::object()) {
    const auto identity = identity_from_context(ctx, body.is_object() ? &body : nullptr);
    if (auth_required() && !identity.has_value()) {
        throw std::runtime_error("authentication required");
    }
    mindbridge::storage::StorageIdentity out;
    out.user_id = identity.has_value() ? identity->user_id : json_value(body, "user", "user_id", "anonymous");
    out.conversation_id = json_value(body, "conversation_id", "contextId", "default");
    out.run_id = json_value(body, "run_id", "runId", "");
    if (out.user_id.empty()) {
        out.user_id = "anonymous";
    }
    if (out.conversation_id.empty()) {
        out.conversation_id = "default";
    }
    return out;
}

std::string storage_status_payload() {
    return cloud_storage().status().dump();
}

std::string storage_instant_payload(const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
    try {
        const auto body = parse_json_body(ctx.body);
        const auto identity = storage_identity_from(ctx, body);
        const std::string md5 = json_value(body, "md5");
        const std::string filename = json_value(body, "filename", "fileName", "file");
        const std::string type = json_value(body, "type", "mime_type", "");
        return cloud_storage().instant_upload(identity, md5, filename, type).dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"ok", false}, {"code", 1}, {"error", e.what()}}.dump();
    }
}

std::string storage_upload_payload(const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
    try {
        const auto body = parse_json_body(ctx.body);
        const auto identity = storage_identity_from(ctx, body);
        return cloud_storage()
            .upload_base64(identity,
                           json_value(body, "filename", "fileName", "file"),
                           json_value(body, "md5"),
                           json_value(body, "type", "mime_type", "application/octet-stream"),
                           json_value(body, "data_base64", "data"))
            .dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"ok", false}, {"code", 1}, {"error", e.what()}}.dump();
    }
}

std::string storage_chunk_init_payload(const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
    try {
        const auto body = parse_json_body(ctx.body);
        const auto identity = storage_identity_from(ctx, body);
        const std::int64_t size = body.value("size", body.value("filesize", 0LL));
        const int chunk_count = body.value("chunkCount", body.value("chunk_count", 0));
        return cloud_storage()
            .init_chunk_upload(identity,
                               json_value(body, "filename", "fileName", "file"),
                               json_value(body, "md5"),
                               json_value(body, "type", "mime_type", "application/octet-stream"),
                               size,
                               chunk_count)
            .dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"ok", false}, {"code", 1}, {"error", e.what()}}.dump();
    }
}

std::string storage_chunk_upload_payload(const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
    try {
        const auto query = query_params(ctx.target);
        const auto upload_it = query.find("upload_id");
        const auto md5_it = query.find("md5");
        const auto index_it = query.find("index");
        const int index = index_it == query.end() ? -1 : std::stoi(index_it->second);
        return cloud_storage()
            .upload_chunk(upload_it == query.end() ? "" : upload_it->second,
                          md5_it == query.end() ? "" : md5_it->second,
                          index,
                          ctx.body)
            .dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"ok", false}, {"code", 1}, {"error", e.what()}}.dump();
    }
}

std::string storage_chunk_merge_payload(const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
    try {
        const auto body = parse_json_body(ctx.body);
        const auto identity = storage_identity_from(ctx, body);
        return cloud_storage()
            .merge_chunks(identity,
                          json_value(body, "upload_id", "uploadId"),
                          json_value(body, "md5"),
                          json_value(body, "filename", "fileName", ""),
                          json_value(body, "type", "mime_type", ""))
            .dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"ok", false}, {"code", 1}, {"error", e.what()}}.dump();
    }
}

std::string storage_files_payload(const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
    try {
        const auto query = query_params(ctx.target);
        nlohmann::json body = nlohmann::json::object();
        if (!ctx.body.empty()) {
            body = parse_json_body(ctx.body);
        }
        if (query.count("conversation_id")) {
            body["conversation_id"] = query.at("conversation_id");
        }
        if (query.count("user")) {
            body["user"] = query.at("user");
        }
        if (query.count("run_id")) {
            body["run_id"] = query.at("run_id");
        }
        const auto identity = storage_identity_from(ctx, body);
        const int limit = query.count("limit") ? std::stoi(query.at("limit")) : body.value("count", 50);
        const int offset = query.count("offset") ? std::stoi(query.at("offset")) : body.value("start", 0);
        return cloud_storage().list_files(identity, limit, offset).dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"ok", false}, {"code", 1}, {"error", e.what()}, {"files", nlohmann::json::array()}}.dump();
    }
}

std::string storage_download_payload(const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
    try {
        const std::string prefix = "/api/storage/files/";
        std::string md5;
        if (ctx.path.rfind(prefix, 0) == 0) {
            md5 = ctx.path.substr(prefix.size());
            const std::string suffix = "/download";
            if (md5.size() > suffix.size() &&
                md5.compare(md5.size() - suffix.size(), suffix.size(), suffix) == 0) {
                md5.resize(md5.size() - suffix.size());
            }
        }
        nlohmann::json body = nlohmann::json::object();
        const auto query = query_params(ctx.target);
        if (query.count("conversation_id")) {
            body["conversation_id"] = query.at("conversation_id");
        }
        if (query.count("user")) {
            body["user"] = query.at("user");
        }
        const auto identity = storage_identity_from(ctx, body);
        return cloud_storage().download_file(identity, md5).dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"ok", false}, {"code", 1}, {"error", e.what()}}.dump();
    }
}

std::string storage_sync_payload(const mindbridge::net::AsyncHttpServer::RequestContext&) {
    const int applied = cloud_storage().sync_pending_artifacts(100);
    return nlohmann::json{{"ok", true}, {"applied", applied}, {"status", cloud_storage().status()}}.dump();
}

std::string tts_status_payload() {
    const bool has_key =
        !mindbridge::app::env_or("MINDBRIDGE_DASHSCOPE_TTS_API_KEY").empty() ||
        !mindbridge::app::env_or("MINDBRIDGE_DASHSCOPE_API_KEY").empty() ||
        !mindbridge::app::env_or("DASHSCOPE_API_KEY").empty() ||
        !mindbridge::app::env_or("MINDBRIDGE_MIMO_TTS_API_KEY").empty() ||
        !mindbridge::app::env_or("MINDBRIDGE_MIMO_API_KEY").empty() ||
        !mindbridge::app::env_or("MINDBRIDGE_MODEL_API_KEY").empty();
    const std::string provider = mindbridge::app::env_or("MINDBRIDGE_TTS_PROVIDER", "dashscope");
    const std::string voice = dashscope_voice_from_env();
    const std::string configured_model = mindbridge::app::env_or(
        "MINDBRIDGE_DASHSCOPE_TTS_MODEL",
        mindbridge::app::env_or("MINDBRIDGE_MIMO_TTS_MODEL", "cosyvoice-v3-flash"));
    const std::string effective_model =
        provider == "dashscope" ? dashscope_tts_model_for_voice(configured_model, voice) : configured_model;
    return nlohmann::json{{"ok", true},
                          {"provider", provider},
                          {"configured", has_key},
                          {"model", configured_model},
                          {"effective_model", effective_model},
                          {"voice", voice.empty() ? nlohmann::json(nullptr) : nlohmann::json(voice)},
                          {"path", mindbridge::app::env_or(
                                       "MINDBRIDGE_DASHSCOPE_TTS_WS_URL",
                                       mindbridge::app::env_or("MINDBRIDGE_MIMO_TTS_PATH",
                                                               "wss://dashscope.aliyuncs.com/api-ws/v1/inference"))}}
        .dump();
}

std::string synthesize_speech_payload(const std::string& body) {
    try {
        const auto request = nlohmann::json::parse(body);
        const nlohmann::json arguments = {
            {"text", request.value("text", "")},
            {"voice", request.value(
                          "voice",
                          mindbridge::app::env_or("MINDBRIDGE_TTS_PROVIDER", "dashscope") == "mimo"
                              ? mindbridge::app::env_or("MINDBRIDGE_MIMO_TTS_VOICE", "mimo_default")
                              : dashscope_voice_from_env())},
            {"format", request.value(
                           "format",
                           mindbridge::app::env_or(
                               "MINDBRIDGE_DASHSCOPE_TTS_FORMAT",
                               mindbridge::app::env_or("MINDBRIDGE_MIMO_TTS_FORMAT", "mp3")))},
        };
        auto& tools = speech_tools();
        const auto validation = tools.validate("synthesize_speech", arguments);
        if (validation.has_value()) {
            return nlohmann::json{{"ok", false}, {"error", *validation}}.dump();
        }
        mindbridge::ToolContext tool_ctx{{{"owner", "mindbridge_gateway"}}};
        const auto result = tools.execute("synthesize_speech", arguments, tool_ctx);
        return result.to_json().dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"ok", false}, {"error", e.what()}}.dump();
    }
}

void synthesize_speech_stream_payload(
    const std::string& body,
    const std::function<bool(const std::string&)>& write_event) {
    static int failure_count = 0;
    static bool disabled = false;
    auto emit = [&](const nlohmann::json& event) {
        return !write_event || write_event(event.dump());
    };
    if (disabled) {
        emit({{"event", "tts_error"}, {"message", "TTS auto-disabled after repeated failures"}});
        emit({{"event", "tts_close"}});
        return;
    }
    try {
        const auto request = nlohmann::json::parse(body);
        mindbridge::SpeechSynthesisRequest synth_request{
            request.value("text", ""),
            request.value("voice",
                          mindbridge::app::env_or("MINDBRIDGE_TTS_PROVIDER", "dashscope") == "mimo"
                              ? mindbridge::app::env_or("MINDBRIDGE_MIMO_TTS_VOICE", "mimo_default")
                              : dashscope_voice_from_env()),
            request.value("format", "pcm")};
        if (synth_request.text.empty()) {
            emit({{"event", "tts_error"}, {"message", "text cannot be empty"}});
            emit({{"event", "tts_close"}});
            return;
        }
        std::unique_ptr<mindbridge::SpeechSynthesizer> synthesizer;
        const int create_retries = std::max(1, env_int("MINDBRIDGE_TTS_CREATE_RETRIES", 3));
        for (int attempt = 0; attempt < create_retries && !synthesizer; ++attempt) {
            synthesizer = mindbridge::create_speech_synthesizer_from_env();
        }
        if (!synthesizer) {
            emit({{"event", "tts_error"}, {"message", "tts synthesizer is disabled"}});
            emit({{"event", "tts_close"}});
            return;
        }

        class SseTtsCallback : public mindbridge::SpeechSynthesisCallback {
        public:
            explicit SseTtsCallback(std::function<bool(const nlohmann::json&)> emit)
                : emit_(std::move(emit)) {}
            void on_open() override {
                if (opened_) {
                    return;
                }
                opened_ = true;
                emit_({{"event", "tts_open"},
                       {"mime_type", "audio/pcm"},
                       {"sample_rate", 22050},
                       {"channels", 1}});
            }
            bool on_data(const std::vector<unsigned char>& data) override {
                return emit_({{"event", "audio"},
                              {"mime_type", "audio/pcm"},
                              {"sample_rate", 22050},
                              {"channels", 1},
                              {"data_base64", encode_base64(data)}});
            }
            void on_complete() override {
                emit_({{"event", "tts_complete"}});
            }
            void on_error(const std::string& message) override {
                emit_({{"event", "tts_error"}, {"message", message}});
            }
            void on_close() override {
                emit_({{"event", "tts_close"}});
            }

        private:
            std::function<bool(const nlohmann::json&)> emit_;
            bool opened_{false};
        };

        SseTtsCallback callback(emit);
        const bool streamed = synthesizer->synthesize_stream(synth_request, callback);
        if (streamed) {
            failure_count = 0;
            return;
        }
        ++failure_count;
        if (failure_count >= env_int("MINDBRIDGE_TTS_AUTO_DISABLE_THRESHOLD", 5)) {
            disabled = true;
        }
    } catch (const std::exception& e) {
        ++failure_count;
        emit({{"event", "tts_error"}, {"message", e.what()}});
        if (failure_count >= env_int("MINDBRIDGE_TTS_AUTO_DISABLE_THRESHOLD", 5)) {
            disabled = true;
            emit({{"event", "tts_error"}, {"message", "TTS auto-disabled after repeated failures"}});
        }
        emit({{"event", "tts_close"}});
    }
}

std::string recognize_speech_payload(const std::string& body) {
    try {
        const auto request = nlohmann::json::parse(body);
        const nlohmann::json arguments = {
            {"audio", request.value("audio", nlohmann::json::object())},
            {"format", request.value("format", "wav")},
            {"sample_rate", request.value("sample_rate", 16000)},
        };
        auto& tools = speech_tools();
        const auto validation = tools.validate("recognize_speech", arguments);
        if (validation.has_value()) {
            return nlohmann::json{{"ok", false}, {"error", *validation}}.dump();
        }
        mindbridge::ToolContext tool_ctx{{{"owner", "mindbridge_gateway"}}};
        const auto result = tools.execute("recognize_speech", arguments, tool_ctx);
        return result.to_json().dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"ok", false}, {"error", e.what()}, {"transcript", ""}}.dump();
    }
}

std::string demo_websocket_payload(const std::string& body) {
    try {
        const auto request = nlohmann::json::parse(body);
        const std::string type = request.value("type", "echo");
        const std::string client_id = request.value("client_id", "unknown");
        if (type == "auth") {
            return nlohmann::json{{"type", "auth_ok"},
                                  {"client_id", client_id},
                                  {"service", "mindbridge_gateway"},
                                  {"message", "demo websocket authenticated"}}
                .dump();
        }
        if (type == "heartbeat") {
            return nlohmann::json{{"type", "heartbeat_ack"},
                                  {"client_id", client_id},
                                  {"sequence", request.value("sequence", 0)},
                                  {"service", "mindbridge_gateway"}}
                .dump();
        }
        if (type == "subscribe") {
            const std::string topic = request.value("topic", "health");
            nlohmann::json payload = {{"type", "subscription"},
                                      {"topic", topic},
                                      {"client_id", client_id},
                                      {"service", "mindbridge_gateway"}};
            if (topic == "feature_status") {
                payload["data"] = nlohmann::json::parse(feature_status_payload());
            } else if (topic == "latest_run") {
                payload["data"] = nlohmann::json::parse(latest_run_payload());
            } else {
                payload["data"] = nlohmann::json{{"ok", true},
                                                 {"orchestrator", downstream_url()},
                                                 {"orchestrators", orchestrator_router().urls()}};
            }
            return payload.dump();
        }
        return nlohmann::json{{"type", "echo"},
                              {"client_id", client_id},
                              {"service", "mindbridge_gateway"},
                              {"data", request}}
            .dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"type", "error"}, {"message", e.what()}}.dump();
    }
}

std::string make_request_id(const nlohmann::json& request) {
    const std::string id = mindbridge::app::request_id_from(request);
    if (id != "unknown") {
        return id;
    }
    return "gw-" + std::to_string(std::time(nullptr));
}

std::string proxy_post(const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
    try {
        auto request = nlohmann::json::parse(ctx.body);
        const std::string token = mindbridge::app::env_or("MINDBRIDGE_GATEWAY_TOKEN");
        if (!token.empty() &&
            request.value("metadata", nlohmann::json::object()).value("gateway_token", "") != token) {
            return mindbridge::app::json_error(mindbridge::app::request_id_from(request), 401, "gateway token required");
        }
        const auto identity = visible_history_identity(ctx, &request);
        if (auth_required() && !identity.has_value()) {
            return unauthorized_json(mindbridge::app::request_id_from(request));
        }
        std::string original_context_id;
        if (identity.has_value()) {
            const auto params = request.value("params", nlohmann::json::object());
            if (request.value("method", "") == "session/end") {
                original_context_id = params.value("contextId", params.value("context_id", "default"));
            } else {
                original_context_id = params.value("message", nlohmann::json::object())
                                          .value("contextId", "default");
                if (original_context_id.empty()) {
                    original_context_id = params.value("contextId", params.value("context_id", "default"));
                }
            }
            const std::string scoped = auth_service().scoped_conversation_id(*identity, original_context_id);
            if (request.value("method", "") == "session/end") {
                request["params"]["contextId"] = scoped;
            } else {
                request["params"]["message"]["contextId"] = scoped;
            }
            request["metadata"]["user_id"] = identity->user_id;
            request["metadata"]["auth_session_id"] = identity->auth_session_id;
            request["metadata"]["original_context_id"] = original_context_id;
        }
        const std::string request_id = make_request_id(request);
        request["metadata"]["request_id"] = request_id;
        request["metadata"]["gateway"] = "mindbridge_gateway";
        const std::string raw = orchestrator_router().post_json("gateway", request_id, request.dump(), true, 120L);
        if (identity.has_value()) {
            try {
                const auto response = nlohmann::json::parse(raw);
                const auto result = response.value("result", nlohmann::json::object());
                const std::string run_id = result.value("run_id", "");
                const std::string scoped = request.value("method", "") == "session/end"
                                               ? request["params"].value("contextId", "default")
                                               : request["params"]["message"].value("contextId", "default");
                if (!run_id.empty()) {
                    auth_service().record_run(*identity, scoped, run_id);
                }
            } catch (...) {
            }
        }
        return raw;
    } catch (const std::exception& e) {
        return mindbridge::app::json_error("unknown", -32000, e.what());
    }
}

void proxy_stream(const mindbridge::net::AsyncHttpServer::RequestContext& ctx,
                  const std::function<bool(const std::string&)>& write_event) {
    auto emit = [&](const nlohmann::json& event) {
        return !write_event || write_event(event.dump());
    };
    try {
        auto request = nlohmann::json::parse(ctx.body);
        const std::string token = mindbridge::app::env_or("MINDBRIDGE_GATEWAY_TOKEN");
        if (!token.empty() &&
            request.value("metadata", nlohmann::json::object()).value("gateway_token", "") != token) {
            emit({{"event", "error"}, {"message", "gateway token required"}});
            return;
        }
        const auto identity = visible_history_identity(ctx, &request);
        if (auth_required() && !identity.has_value()) {
            emit({{"event", "error"}, {"message", "authentication required"}});
            return;
        }
        std::string scoped_context;
        std::string original_context_id;
        if (identity.has_value()) {
            original_context_id = request.value("params", nlohmann::json::object())
                                      .value("message", nlohmann::json::object())
                                      .value("contextId", "default");
            scoped_context = auth_service().scoped_conversation_id(*identity, original_context_id);
            request["params"]["message"]["contextId"] = scoped_context;
            request["metadata"]["user_id"] = identity->user_id;
            request["metadata"]["auth_session_id"] = identity->auth_session_id;
            request["metadata"]["original_context_id"] = original_context_id;
        }
        const std::string request_id = make_request_id(request);
        request["method"] = "message/stream";
        request["metadata"]["request_id"] = request_id;
        request["metadata"]["gateway"] = "mindbridge_gateway";
        std::string run_id;
        orchestrator_router().post_sse("gateway", request_id, request.dump(), [&](const std::string& payload) {
            if (identity.has_value()) {
                try {
                    const auto event = nlohmann::json::parse(payload);
                    run_id = event.value("run_id", run_id);
                    if (event.value("event", "") == "final_result") {
                        const auto result = event.value("result", nlohmann::json::object());
                        run_id = result.value("run_id", run_id);
                    }
                } catch (...) {
                }
            }
            return !write_event || write_event(payload);
        }, 120L);
        if (identity.has_value()) {
            if (!run_id.empty()) {
                auth_service().record_run(*identity, scoped_context, run_id);
            }
        }
    } catch (const std::exception& e) {
        emit({{"event", "error"}, {"message", e.what()}});
    }
}

}  // namespace

int main(int argc, char** argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const int port = (argc > 1) ? std::atoi(argv[1]) : 8090;
    mindbridge::net::AsyncHttpServer server(port, mindbridge::net::AsyncHttpServer::options_from_env());

    server.register_handler("/api/health", [](const std::string&) {
        return nlohmann::json{{"ok", true},
                              {"service", "mindbridge_gateway"},
                              {"network",
                               {{"backend", "asio_beast_client"},
                                {"orchestrator", orchestrator_router().health_json()}}},
                              {"storage", cloud_storage().status()},
                              {"orchestrator", downstream_url()},
                              {"orchestrators", orchestrator_router().urls()}}
            .dump();
    });
    server.register_handler("/api/auth/login", [](const std::string& body) {
        return auth_login_payload(body);
    });
    server.register_handler("/api/auth/status", [](const std::string&) {
        return auth_status_payload();
    });
    server.register_context_handler("/api/auth/logout", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        return auth_logout_payload(ctx);
    });
    server.register_context_handler("/api/auth/me", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        return auth_me_payload(ctx);
    });
    server.register_context_handler("/api/conversations", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        return conversations_payload(ctx);
    });
    server.register_prefix_context_handler("/api/conversations/", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        return conversation_payload(ctx);
    });
    server.register_context_handler("/api/demo/runs/latest", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        const auto identity = identity_from_context(ctx);
        if (auth_required()) {
            if (!identity.has_value()) {
                return unauthorized_json();
            }
            const std::string run_id = auth_service().latest_run_id_for(*identity);
            if (run_id.empty()) {
                return nlohmann::json{{"ok", false}, {"error", "no runs found"}}.dump();
            }
            return run_payload_for(identity, run_id);
        }
        return latest_run_payload();
    });
    server.register_context_handler("/api/demo/runs/latest/observability", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        const auto identity = identity_from_context(ctx);
        if (auth_required()) {
            if (!identity.has_value()) {
                return unauthorized_json();
            }
            const std::string run_id = auth_service().latest_run_id_for(*identity);
            if (run_id.empty()) {
                return nlohmann::json{{"ok", false}, {"error", "no runs found"}}.dump();
            }
            return run_observability_payload_for(identity, run_id);
        }
        const std::string run_id = latest_run_id();
        if (run_id.empty()) {
            return nlohmann::json{{"ok", false}, {"error", "no runs found"}}.dump();
        }
        return run_observability_payload_json(run_id).dump();
    });
    server.register_handler("/api/demo/feature-status", [](const std::string&) {
        return feature_status_payload();
    });
    server.register_context_handler("/api/storage/status", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        (void)ctx;
        return storage_status_payload();
    });
    server.register_context_handler("/api/storage/sync", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        return storage_sync_payload(ctx);
    });
    server.register_context_handler("/api/md5", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        return storage_instant_payload(ctx);
    });
    server.register_context_handler("/api/storage/instant", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        return storage_instant_payload(ctx);
    });
    server.register_context_handler("/api/upload", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        return storage_upload_payload(ctx);
    });
    server.register_context_handler("/api/storage/upload", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        return storage_upload_payload(ctx);
    });
    server.register_context_handler("/api/chunk_init", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        return storage_chunk_init_payload(ctx);
    });
    server.register_context_handler("/api/storage/chunks/init", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        return storage_chunk_init_payload(ctx);
    });
    server.register_context_handler("/api/chunk_upload", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        return storage_chunk_upload_payload(ctx);
    });
    server.register_context_handler("/api/storage/chunks/upload", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        return storage_chunk_upload_payload(ctx);
    });
    server.register_context_handler("/api/chunk_merge", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        return storage_chunk_merge_payload(ctx);
    });
    server.register_context_handler("/api/storage/chunks/merge", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        return storage_chunk_merge_payload(ctx);
    });
    server.register_context_handler("/api/storage/files", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        return storage_files_payload(ctx);
    });
    server.register_context_handler("/api/myfiles", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        return storage_files_payload(ctx);
    });
    server.register_prefix_context_handler("/api/storage/files/", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        return storage_download_payload(ctx);
    });
    server.register_handler("/api/demo/tts/status", [](const std::string&) {
        return tts_status_payload();
    });
    server.register_handler("/api/demo/tts", [](const std::string& body) {
        return synthesize_speech_payload(body);
    });
    server.register_stream_handler("/api/demo/tts/stream", [](const std::string& body,
                                                              std::function<bool(const std::string&)> write_event) {
        synthesize_speech_stream_payload(body, write_event);
    });
    server.register_handler("/api/demo/asr", [](const std::string& body) {
        return recognize_speech_payload(body);
    });
    server.register_websocket_handler("/api/demo/ws", [](const std::string& body) {
        return demo_websocket_payload(body);
    });
    server.register_prefix_context_handler("/api/demo/runs/", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        const std::string prefix = "/api/demo/runs/";
        if (ctx.path.rfind(prefix, 0) != 0) {
            return nlohmann::json{{"ok", false}, {"error", "invalid runs path"}}.dump();
        }
        std::string tail = ctx.path.substr(prefix.size());
        const std::string suffix = "/observability";
        if (tail.size() > suffix.size() && tail.compare(tail.size() - suffix.size(), suffix.size(), suffix) == 0) {
            tail.resize(tail.size() - suffix.size());
            return run_observability_payload_for(identity_from_context(ctx), tail);
        }
        return run_payload_for(identity_from_context(ctx), tail);
    });

    server.register_context_handler("/", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx) {
        return proxy_post(ctx);
    });
    server.register_context_stream_handler("/", [](const mindbridge::net::AsyncHttpServer::RequestContext& ctx,
                                                  std::function<bool(const std::string&)> write_event) {
        proxy_stream(ctx, write_event);
    });

    std::cout << "[mindbridge_gateway] port=" << port << " downstream=" << downstream_url() << std::endl;
    server.start();
    curl_global_cleanup();
    return 0;
}
