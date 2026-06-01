#include "mindbridge/apps/app_support.hpp"
#include "mindbridge/net/async_http_server.hpp"
#include "mindbridge/platform/platform_store.hpp"
#include "mindbridge/platform/universal_event_mapper.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

using nlohmann::json;

namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string id_with_prefix(const std::string& prefix) {
    return prefix + "-" + std::to_string(now_ms());
}

json parse_body(const std::string& body) {
    if (body.empty()) {
        return json::object();
    }
    auto parsed = json::parse(body, nullptr, false);
    return parsed.is_object() ? parsed : json::object();
}

std::string ok(json data = json::object()) {
    return json{{"ok", true}, {"data", std::move(data)}}.dump();
}

std::string fail(const std::string& message) {
    return json{{"ok", false}, {"error", {{"message", message}}}}.dump();
}

template <typename T>
json array_of(const std::vector<T>& values) {
    json out = json::array();
    for (const auto& value : values) {
        out.push_back(mindbridge::platform::to_json(value));
    }
    return out;
}

std::string after_prefix(const std::string& path, const std::string& prefix) {
    if (path.rfind(prefix, 0) != 0) {
        return "";
    }
    return path.substr(prefix.size());
}

std::string first_path_part(const std::string& tail) {
    const auto pos = tail.find('/');
    return pos == std::string::npos ? tail : tail.substr(0, pos);
}

json read_json_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return json::object();
    }
    json parsed = json::parse(in, nullptr, false);
    return parsed.is_discarded() ? json::object() : parsed;
}

json session_payload(const mindbridge::platform::AgentSession& session,
                     mindbridge::platform::PlatformStore& store) {
    json out = mindbridge::platform::to_json(session);
    if (const auto workspace = store.get_workspace(session.workspace_id)) {
        out["workspace"] = mindbridge::platform::to_json(*workspace);
    }
    if (const auto core = store.get_agent_core(session.agent_core_id)) {
        out["agent_core"] = mindbridge::platform::to_json(*core);
    }
    return out;
}

void ensure_default_core(mindbridge::platform::PlatformStore& store) {
    const std::string gateway_url =
        mindbridge::app::env_or("MINDBRIDGE_PLATFORM_GATEWAY_URL", "http://127.0.0.1:8090");
    mindbridge::platform::AgentCore core;
    core.core_id = "mindbridge-default";
    core.type = "mindbridge";
    core.gateway_url = gateway_url;
    core.capabilities = {"chat", "stream", "session_end", "artifacts", "observability"};
    store.upsert_agent_core(std::move(core));
}

}  // namespace

int main(int argc, char** argv) {
    const int port = argc > 1 ? std::stoi(argv[1]) : 8077;
    const std::string db_path =
        mindbridge::app::env_or("MINDBRIDGE_PLATFORM_DB_PATH", ".mindbridge/platform/platform.sqlite");
    const std::filesystem::path run_root =
        mindbridge::app::env_or("MINDBRIDGE_RUN_ROOT", ".mindbridge");

    mindbridge::platform::PlatformStore store(db_path);
    store.initialize();
    ensure_default_core(store);

    mindbridge::net::AsyncHttpServer server(port, mindbridge::net::AsyncHttpServer::options_from_env());

    server.register_handler("/api/platform/health", [](const std::string&) {
        return ok({{"service", "mindbridge_platform"}, {"status", "ok"}});
    });

    server.register_handler("/api/platform/agent-cores", [&store](const std::string&) {
        return ok({{"agent_cores", array_of(store.list_agent_cores())}});
    });

    server.register_handler("/api/platform/workspaces", [&store](const std::string& body) {
        try {
            const auto req = parse_body(body);
            if (req.empty()) {
                return ok({{"workspaces", array_of(store.list_workspaces())}});
            }
            mindbridge::platform::Workspace workspace;
            workspace.workspace_id = req.value("workspace_id", id_with_prefix("ws"));
            workspace.owner_user_id = req.value("owner_user_id", req.value("user_id", "anonymous"));
            workspace.namespace_name =
                req.value("namespace", req.value("namespace_name", "mindbridge-" + workspace.workspace_id));
            workspace.resource_quota = req.value("resource_quota", json::object());
            store.create_workspace(workspace);
            return ok({{"workspace", mindbridge::platform::to_json(*store.get_workspace(workspace.workspace_id))}});
        } catch (const std::exception& e) {
            return fail(e.what());
        }
    });

    server.register_handler("/api/platform/sessions", [&store](const std::string& body) {
        try {
            const auto req = parse_body(body);
            if (req.empty()) {
                return ok({{"sessions", array_of(store.list_sessions())}});
            }
            mindbridge::platform::AgentSession session;
            session.session_id = req.value("session_id", id_with_prefix("session"));
            session.workspace_id = req.value("workspace_id", "");
            session.user_id = req.value("user_id", "anonymous");
            session.conversation_id = req.value("conversation_id", session.session_id);
            session.agent_core_id = req.value("agent_core_id", "mindbridge-default");
            session.status = req.value("status", "running");
            session.last_run_id = req.value("last_run_id", "");
            if (!store.get_workspace(session.workspace_id)) {
                return fail("workspace not found: " + session.workspace_id);
            }
            store.create_session(session);
            return ok({{"session", session_payload(*store.get_session(session.session_id), store)}});
        } catch (const std::exception& e) {
            return fail(e.what());
        }
    });

    server.register_prefix_handler("/api/platform/sessions/", [&](const std::string& path, const std::string& body) {
        try {
            const std::string tail = after_prefix(path, "/api/platform/sessions/");
            const std::string session_id = first_path_part(tail);
            const auto session = store.get_session(session_id);
            if (!session) {
                return fail("session not found: " + session_id);
            }
            if (tail == session_id) {
                return ok({{"session", session_payload(*session, store)}});
            }
            if (tail == session_id + "/stop") {
                store.update_session_status(session_id, "stopped");
                return ok({{"session", session_payload(*store.get_session(session_id), store)}});
            }
            if (tail == session_id + "/events") {
                const auto events =
                    mindbridge::platform::load_session_events(run_root, session_id, session->last_run_id);
                json out = json::array();
                for (const auto& event : events) {
                    out.push_back(mindbridge::platform::to_json(event));
                }
                return ok({{"events", out}});
            }
            if (tail == session_id + "/artifacts") {
                return ok({{"artifacts", mindbridge::platform::list_run_artifacts(run_root, session->last_run_id)}});
            }
            if (tail == session_id + "/observability") {
                if (session->last_run_id.empty()) {
                    return ok({{"run_id", ""}, {"observability_report", json::object()}});
                }
                const auto run_dir = run_root / "runs" / session->last_run_id;
                return ok({{"run_id", session->last_run_id},
                           {"observability_report", read_json_file(run_dir / "observability_report.json")}});
            }
            if (tail == session_id + "/attach-run") {
                const auto req = parse_body(body);
                const std::string run_id = req.value("run_id", "");
                if (run_id.empty()) {
                    return fail("run_id is required");
                }
                store.update_session_run_id(session_id, run_id);
                return ok({{"session", session_payload(*store.get_session(session_id), store)}});
            }
            return fail("unknown platform session path: " + path);
        } catch (const std::exception& e) {
            return fail(e.what());
        }
    });

    std::cout << "MindBridge platform listening on " << port << " db=" << db_path
              << " run_root=" << run_root << std::endl;
    server.start();
    return 0;
}
