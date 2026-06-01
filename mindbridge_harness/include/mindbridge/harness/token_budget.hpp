#pragma once

#include <cstdint>
#include <string>

namespace mindbridge {

enum class BudgetLevel {
    GREEN,    // Comfortable headroom
    YELLOW,   // Approaching limit, non-essential actions should be trimmed
    RED,      // Only critical tool calls allowed
    BREAKER   // Hard stop, no further actions permitted
};

class TokenBudgetController {
public:
    TokenBudgetController(int total_budget = 8000,
                          double warning_threshold = 0.6,
                          double critical_threshold = 0.8,
                          double hard_limit = 0.95);

    // Record token usage for a single turn. Accumulates and returns new level.
    BudgetLevel record_usage(int prompt_tokens, int completion_tokens, int tool_output_tokens);

    // Check whether an action is permitted at the current budget level.
    // GREEN/YELLOW: all actions allowed.
    // RED: only actions where is_critical_tool == true.
    // BREAKER: nothing allowed.
    bool check_action_allowed(bool is_critical_tool) const;

    int consumed() const;
    int total() const;
    BudgetLevel level() const;
    double usage_ratio() const;

private:
    void recalc_level();

    int total_budget_;
    int warning_threshold_;   // absolute token count for GREEN->YELLOW
    int critical_threshold_;  // absolute token count for YELLOW->RED
    int hard_limit_;          // absolute token count for RED->BREAKER
    int consumed_{0};
    BudgetLevel level_{BudgetLevel::GREEN};
};

}  // namespace mindbridge
