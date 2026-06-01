#pragma once

#include "mindbridge/platform/platform_types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mindbridge {
namespace platform {

class PlatformStore {
public:
    explicit PlatformStore(std::string db_path);
    ~PlatformStore();

    PlatformStore(const PlatformStore&) = delete;
    PlatformStore& operator=(const PlatformStore&) = delete;

    void initialize();

    void create_workspace(Workspace workspace);
    std::optional<Workspace> get_workspace(const std::string& workspace_id) const;
    std::vector<Workspace> list_workspaces(const std::string& owner_user_id = "") const;

    void upsert_agent_core(AgentCore core);
    std::optional<AgentCore> get_agent_core(const std::string& core_id) const;
    std::vector<AgentCore> list_agent_cores() const;

    void create_session(AgentSession session);
    std::optional<AgentSession> get_session(const std::string& session_id) const;
    std::vector<AgentSession> list_sessions(const std::string& workspace_id = "",
                                            const std::string& user_id = "") const;
    void update_session_status(const std::string& session_id, const std::string& status);
    void update_session_run_id(const std::string& session_id, const std::string& run_id);

private:
    struct Impl;
    std::string db_path_;
    Impl* impl_{nullptr};
};

}  // namespace platform
}  // namespace mindbridge
