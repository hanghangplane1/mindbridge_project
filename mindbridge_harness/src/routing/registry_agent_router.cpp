#include "mindbridge/routing/registry_agent_router.hpp"

#include <stdexcept>

namespace mindbridge {

RegistryAgentRouter::RegistryAgentRouter(RegistryClient& registry) : registry_(registry) {
    router_.initialize(agent_rpc::orchestrator::RoutingStrategy::SKILL_MATCH);
}

void RegistryAgentRouter::set_strategy(agent_rpc::orchestrator::RoutingStrategy s) {
    router_.setStrategy(s);
}

agent_rpc::orchestrator::AgentInfo RegistryAgentRouter::registration_to_info(const AgentRegistration& r) {
    agent_rpc::orchestrator::AgentInfo a;
    a.id = r.id;
    a.name = r.name;
    a.url = r.address;
    a.tags = r.tags;
    a.is_healthy = true;
    a.description = r.name;
    a.version = "1.0.0";
    // 每个 tag 同时作为 skill，便于 SKILL_MATCH / findAgentsByTags
    a.skills = r.tags;
    return a;
}

void RegistryAgentRouter::sync_from_registry() {
    std::vector<AgentRegistration> all = registry_.get_all_agents();
    std::vector<agent_rpc::orchestrator::AgentInfo> infos;
    infos.reserve(all.size());
    for (const auto& reg : all) {
        infos.push_back(registration_to_info(reg));
    }
    router_.updateAgentList(infos);
}

std::string RegistryAgentRouter::select_agent_url(const std::string& question,
                                                  const std::string& required_skill,
                                                  const std::string& routing_key) {
    sync_from_registry();
    std::vector<std::string> skills = {required_skill};
    auto picked = router_.selectAgent(question, skills, routing_key);
    if (!picked.has_value()) {
        // 回退：与 tag 查找一致
        auto by_tag = router_.findAgentsByTags({required_skill});
        if (!by_tag.empty()) {
            return by_tag.front().url;
        }
        throw std::runtime_error("No agent for skill/tag: " + required_skill);
    }
    return picked->url;
}

}  // namespace mindbridge
