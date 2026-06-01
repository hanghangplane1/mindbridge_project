#include "mindbridge/harness/token_budget.hpp"

#include <algorithm>
#include <stdexcept>

namespace mindbridge {

TokenBudgetController::TokenBudgetController(int total_budget,
                                             double warning_threshold,
                                             double critical_threshold,
                                             double hard_limit)
    : total_budget_(total_budget) {
    if (total_budget <= 0) {
        throw std::invalid_argument("TokenBudgetController: total_budget must be positive");
    }
    if (warning_threshold < 0.0 || warning_threshold > 1.0) {
        throw std::invalid_argument("TokenBudgetController: warning_threshold must be in [0, 1]");
    }
    if (critical_threshold < 0.0 || critical_threshold > 1.0) {
        throw std::invalid_argument("TokenBudgetController: critical_threshold must be in [0, 1]");
    }
    if (hard_limit < 0.0 || hard_limit > 1.0) {
        throw std::invalid_argument("TokenBudgetController: hard_limit must be in [0, 1]");
    }

    warning_threshold_  = static_cast<int>(total_budget * warning_threshold);
    critical_threshold_ = static_cast<int>(total_budget * critical_threshold);
    hard_limit_         = static_cast<int>(total_budget * hard_limit);
}

BudgetLevel TokenBudgetController::record_usage(int prompt_tokens,
                                                 int completion_tokens,
                                                 int tool_output_tokens) {
    // Guard against negative inputs
    prompt_tokens      = std::max(prompt_tokens, 0);
    completion_tokens  = std::max(completion_tokens, 0);
    tool_output_tokens = std::max(tool_output_tokens, 0);

    consumed_ += prompt_tokens + completion_tokens + tool_output_tokens;
    recalc_level();
    return level_;
}

bool TokenBudgetController::check_action_allowed(bool is_critical_tool) const {
    switch (level_) {
        case BudgetLevel::GREEN:
        case BudgetLevel::YELLOW:
            return true;
        case BudgetLevel::RED:
            return is_critical_tool;
        case BudgetLevel::BREAKER:
            return false;
    }
    return false;  // unreachable, keeps compiler happy
}

int TokenBudgetController::consumed() const { return consumed_; }

int TokenBudgetController::total() const { return total_budget_; }

BudgetLevel TokenBudgetController::level() const { return level_; }

double TokenBudgetController::usage_ratio() const {
    if (total_budget_ == 0) return 1.0;
    return static_cast<double>(consumed_) / static_cast<double>(total_budget_);
}

void TokenBudgetController::recalc_level() {
    if (consumed_ >= hard_limit_) {
        level_ = BudgetLevel::BREAKER;
    } else if (consumed_ >= critical_threshold_) {
        level_ = BudgetLevel::RED;
    } else if (consumed_ >= warning_threshold_) {
        level_ = BudgetLevel::YELLOW;
    } else {
        level_ = BudgetLevel::GREEN;
    }
}

}  // namespace mindbridge
