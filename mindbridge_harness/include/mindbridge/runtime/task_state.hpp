#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace mindbridge {

struct TaskState {
    std::string run_id;
    std::string task_id;
    std::string status{"running"};
    int tool_steps{0};
    int attempts{0};
    std::string last_tool;
    std::string stop_reason;
    std::string final_answer;
    std::string risk_level{"low"};

    nlohmann::json to_json() const {
        return {
            {"run_id", run_id},
            {"task_id", task_id},
            {"status", status},
            {"tool_steps", tool_steps},
            {"attempts", attempts},
            {"last_tool", last_tool},
            {"stop_reason", stop_reason},
            {"final_answer", final_answer},
            {"risk_level", risk_level},
        };
    }
};

}  // namespace mindbridge
