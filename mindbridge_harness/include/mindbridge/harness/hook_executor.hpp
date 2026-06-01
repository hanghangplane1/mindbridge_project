#pragma once

#include "mindbridge/harness/hook_events.hpp"

#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <vector>

namespace mindbridge {

using HookCallback = std::function<void(HookEvent, const nlohmann::json&)>;

class HookExecutor {
public:
    void add_listener(HookCallback cb);
    void execute(HookEvent event, const nlohmann::json& payload);

private:
    std::mutex mutex_;
    std::vector<HookCallback> listeners_;
};

}  // namespace mindbridge
