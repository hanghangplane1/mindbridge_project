#include "mindbridge/harness/hook_executor.hpp"

namespace mindbridge {

void HookExecutor::add_listener(HookCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.push_back(std::move(cb));
}

void HookExecutor::execute(HookEvent event, const nlohmann::json& payload) {
    std::vector<HookCallback> copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        copy = listeners_;
    }
    nlohmann::json enriched = payload;
    enriched["hook"] = hook_event_name(event);
    for (const auto& cb : copy) {
        if (cb) {
            cb(event, enriched);
        }
    }
}

}  // namespace mindbridge
