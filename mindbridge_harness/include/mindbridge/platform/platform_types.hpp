#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace mindbridge {
namespace platform {

struct Workspace {
    std::string workspace_id;
    std::string owner_user_id{"anonymous"};
    std::string namespace_name;
    nlohmann::json resource_quota = nlohmann::json::object();
    std::int64_t created_at{0};
};

struct AgentCore {
    std::string core_id;
    std::string type{"mindbridge"};
    std::string gateway_url;
    std::vector<std::string> capabilities;
    std::int64_t created_at{0};
};

struct AgentSession {
    std::string session_id;
    std::string workspace_id;
    std::string user_id{"anonymous"};
    std::string conversation_id{"default"};
    std::string agent_core_id{"mindbridge-default"};
    std::string status{"pending"};
    std::int64_t created_at{0};
    std::int64_t updated_at{0};
    std::string last_run_id;
};

struct UniversalEvent {
    std::string event_id;
    std::string session_id;
    std::string run_id;
    std::string type;
    std::string source;
    std::int64_t timestamp_ms{0};
    nlohmann::json payload = nlohmann::json::object();
};

nlohmann::json to_json(const Workspace& workspace);
nlohmann::json to_json(const AgentCore& core);
nlohmann::json to_json(const AgentSession& session);
nlohmann::json to_json(const UniversalEvent& event);

}  // namespace platform
}  // namespace mindbridge
