#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace mindbridge::observability {

struct BoundaryTraceSummary {
    int intent_events{0};
    int action_events{0};
    int resource_events{0};
    int correlated_spans{0};
    int suppressed_action_events{0};
    int warnings{0};
    std::string boundary_trace_path;
    std::string observability_report_path;

    nlohmann::json to_json() const;
};

class BoundaryTraceCorrelator {
public:
    explicit BoundaryTraceCorrelator(std::string run_dir);

    BoundaryTraceSummary correlate(const nlohmann::json& ebpf_summary);

private:
    std::string run_dir_;
};

}  // namespace mindbridge::observability
