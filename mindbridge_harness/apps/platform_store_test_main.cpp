#include "mindbridge/platform/platform_store.hpp"
#include "mindbridge/platform/universal_event_mapper.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    const std::filesystem::path root = ".mindbridge/platform_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    mindbridge::platform::PlatformStore store((root / "platform.sqlite").string());
    store.initialize();

    mindbridge::platform::Workspace workspace;
    workspace.workspace_id = "ws-demo";
    workspace.owner_user_id = "demo-user";
    workspace.namespace_name = "mindbridge-ws-demo";
    workspace.resource_quota = {{"cpu", "2"}, {"memory", "2Gi"}};
    store.create_workspace(workspace);

    auto loaded_workspace = store.get_workspace("ws-demo");
    require(loaded_workspace.has_value(), "workspace should load after create");
    require(loaded_workspace->owner_user_id == "demo-user", "workspace owner should round-trip");

    mindbridge::platform::AgentCore core;
    core.core_id = "mindbridge-default";
    core.type = "mindbridge";
    core.gateway_url = "http://gateway-svc:8090";
    core.capabilities = {"chat", "stream", "artifacts", "observability"};
    store.upsert_agent_core(core);

    mindbridge::platform::AgentSession session;
    session.session_id = "sess-001";
    session.workspace_id = "ws-demo";
    session.user_id = "demo-user";
    session.conversation_id = "conv-001";
    session.agent_core_id = "mindbridge-default";
    session.status = "running";
    session.last_run_id = "run-001";
    store.create_session(session);

    auto loaded_session = store.get_session("sess-001");
    require(loaded_session.has_value(), "session should load after create");
    require(loaded_session->workspace_id == "ws-demo", "session workspace should round-trip");
    require(loaded_session->last_run_id == "run-001", "session run id should round-trip");

    const auto event = mindbridge::platform::map_trace_event(
        {{"event", "model_requested"}, {"wall_time_ns", 1700000000123000000LL}, {"attempt", 1}},
        "sess-001",
        "run-001");
    require(event.type == "model.requested", "model_requested should map to model.requested");
    require(event.session_id == "sess-001", "event session id should be preserved");
    require(event.run_id == "run-001", "event run id should be preserved");

    std::cout << "PASS: platform store and universal event mapper\n";
    return 0;
}
