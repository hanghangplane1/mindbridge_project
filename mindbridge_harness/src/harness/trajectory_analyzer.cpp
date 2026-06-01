#include "mindbridge/harness/trajectory_analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace mindbridge {

TrajectoryAnalyzer::TrajectoryAnalyzer() = default;

TrajectoryAnalyzer::TrajectoryAnalyzer(Config config)
    : config_(config) {}

const TrajectoryAnalyzer::Config& TrajectoryAnalyzer::config() const {
    return config_;
}

TrajectoryResult TrajectoryAnalyzer::analyze(
    const std::vector<ToolCallRecord>& calls,
    const std::string& intent) const {

    TrajectoryResult result;
    result.tool_call_count = static_cast<int>(calls.size());

    if (calls.empty()) {
        return result;
    }

    // --- 1. Loop detection: track consecutive runs of identical tool_name + args ---
    {
        std::string prev_tool;
        nlohmann::json prev_args;
        int current_run = 0;

        for (size_t i = 0; i < calls.size(); ++i) {
            const auto& call = calls[i];

            if (i == 0 || call.tool_name != prev_tool ||
                call.arguments != prev_args) {
                // New run started
                prev_tool = call.tool_name;
                prev_args = call.arguments;
                current_run = 1;
            } else {
                // Same tool+args as previous call, extend run
                ++current_run;
            }

            if (current_run >= config_.loop_threshold && !result.loop_detected) {
                result.loop_detected = true;
                result.loop_tool_name = call.tool_name;
            }

            if (current_run > result.loop_count) {
                result.loop_count = current_run;
            }
        }
    }

    // --- 2. Efficiency score: min_expected(intent) / actual_count ---
    {
        int min_expected = config_.min_expected_chat;
        if (intent == "CONSULT") {
            min_expected = config_.min_expected_consult;
        } else if (intent == "RISK") {
            min_expected = config_.min_expected_risk;
        }

        int actual_count = std::max(result.tool_call_count, 1);
        double raw = static_cast<double>(min_expected) /
                     static_cast<double>(actual_count);
        result.efficiency_score = std::clamp(raw, 0.0, 1.0);
    }

    // --- 3. Anomalies ---
    {
        const double total = static_cast<double>(result.tool_call_count);

        // Count failures
        int failure_count = 0;
        for (const auto& call : calls) {
            if (!call.success) {
                ++failure_count;
            }
        }

        if (total > 0 &&
            static_cast<double>(failure_count) / total > 0.5) {
            result.anomalies.push_back("high_failure_rate");
        }

        if (result.tool_call_count >
            static_cast<int>(config_.max_tool_steps * 1.5)) {
            result.anomalies.push_back("excessive_steps");
        }
    }

    return result;
}

}  // namespace mindbridge
