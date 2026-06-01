#include "mindbridge/runtime/risk_policy.hpp"

#include <vector>

namespace mindbridge {

namespace {

bool contains_any(const std::string& text, const std::vector<std::string>& keywords) {
    for (const auto& keyword : keywords) {
        if (text.find(keyword) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string RiskPolicy::heuristic_intent(const std::string& text) {
    static const std::vector<std::string> risk = {
        "不想活", "自杀", "自残", "结束生命", "伤害自己", "活着没意义"};
    static const std::vector<std::string> consult = {
        "焦虑", "抑郁", "压力", "失眠", "难受", "崩溃", "咨询", "心理"};
    if (contains_any(text, risk)) {
        return "RISK";
    }
    if (contains_any(text, consult)) {
        return "CONSULT";
    }
    return "CHAT";
}

std::string RiskPolicy::heuristic_risk_level(const std::string& text) {
    const std::string intent = heuristic_intent(text);
    if (intent == "RISK") {
        return "high";
    }
    if (intent == "CONSULT") {
        return "medium";
    }
    return "low";
}

bool RiskPolicy::is_high_risk(const std::string& risk_level) {
    return risk_level == "high" || risk_level == "HIGH" || risk_level == "RISK";
}

}  // namespace mindbridge
