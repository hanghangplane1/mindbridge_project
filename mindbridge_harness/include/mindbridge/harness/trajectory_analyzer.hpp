#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace mindbridge {

struct ToolCallRecord {
    std::string tool_name;
    nlohmann::json arguments;
    bool success{true};
    int output_chars{0};
};

struct TrajectoryResult {
    bool loop_detected{false};
    std::string loop_tool_name;
    int loop_count{0};
    double efficiency_score{1.0};
    std::vector<std::string> anomalies;
    int tool_call_count{0};
};

class TrajectoryAnalyzer {
public:
    struct Config {
        int loop_threshold{3};
        int max_tool_steps{6};
        int min_expected_chat{1};
        int min_expected_consult{3};
        int min_expected_risk{5};
    };

    TrajectoryAnalyzer();
    explicit TrajectoryAnalyzer(Config config);

    TrajectoryResult analyze(const std::vector<ToolCallRecord>& calls,
                             const std::string& intent) const;

    const Config& config() const;

private:
    Config config_;
};

}  // namespace mindbridge
