#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace mindbridge {

/**
 * 心理场景任务难度分（与 plan 中 L1–L4 思路一致，可扩展权重）。
 */
class HardnessScorer {
public:
    static int score_from_task(const nlohmann::json& task) {
        int s = 0;
        const std::string tier = task.value("difficulty", std::string("L1"));
        if (tier == "L1") {
            s += 1;
        } else if (tier == "L2") {
            s += 3;
        } else if (tier == "L3") {
            s += 6;
        } else if (tier == "L4") {
            s += 10;
        } else {
            s += 2;
        }
        if (task.value("requires_rag", false)) {
            s += 2;
        }
        if (task.value("requires_mcp", false)) {
            s += 2;
        }
        if (task.value("multi_turn", false)) {
            s += 2;
        }
        if (task.value("crisis", false)) {
            s += 4;
        }
        return s;
    }

    static std::string tier_from_score(int score) {
        if (score <= 3) {
            return "L1";
        }
        if (score <= 7) {
            return "L2";
        }
        if (score <= 12) {
            return "L3";
        }
        return "L4";
    }
};

}  // namespace mindbridge
