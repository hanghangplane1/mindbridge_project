#pragma once

#include <string>

namespace mindbridge {

class RiskPolicy {
public:
    static std::string heuristic_intent(const std::string& text);
    static std::string heuristic_risk_level(const std::string& text);
    static bool is_high_risk(const std::string& risk_level);
};

}  // namespace mindbridge
