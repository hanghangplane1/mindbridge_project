#pragma once

#include "agent_rpc/orchestrator/agent_info.h"
#include "agent_rpc/orchestrator/agent_router.h"

#include <a2a/examples/registry_client.hpp>

#include <string>
#include <vector>

namespace mindbridge {

/**
 * 将 RegistryClient 发现的 Agent 同步到 AgentRouter，
 * 用 required_skills（通常与 tag 同名，如 evaluator）做候选过滤，
 * 再按 AgentRouter 策略选择目标实例。
 */
class RegistryAgentRouter {
public:
    explicit RegistryAgentRouter(RegistryClient& registry);

    void set_strategy(agent_rpc::orchestrator::RoutingStrategy s);
    void sync_from_registry();

    /**
     * @param question 用于 SKILL_MATCH 的文本（可传用户输入）
     * @param required_skill 例如 "evaluator"
     * @param routing_key 一致性哈希的亲和 key（如 context_id）
     * @return 选中 Agent 的 HTTP base URL
     */
    std::string select_agent_url(const std::string& question,
                                 const std::string& required_skill,
                                 const std::string& routing_key = "");

    agent_rpc::orchestrator::AgentRouter& router() { return router_; }

private:
    static agent_rpc::orchestrator::AgentInfo registration_to_info(const AgentRegistration& r);

    RegistryClient& registry_;
    agent_rpc::orchestrator::AgentRouter router_;
};

}  // namespace mindbridge
