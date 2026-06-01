#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace mindbridge::observability {

struct EbpfMonitorConfig {
    bool enabled{false};
    std::string binary_path;
    std::string scope{"run"};
    std::string cgroup_path;
    std::string command_filter;
    std::string filter_mode{"0"};
    int pid{0};
    int sample_interval_ms{1000};
    int max_events_per_run{20000};
    std::string capture_llm{"off"};
    bool trace_fs{false};
    bool trace_net{false};
    bool trace_signals{false};
    bool trace_mem{false};
    bool trace_cow{false};
    bool trace_resources{false};
    bool trace_tls{false};
    bool verbose{false};
    std::string tls_binary_path;
    std::string tls_command_filter;
};

struct EbpfMonitorSummary {
    bool enabled{false};
    bool started{false};
    bool unavailable{false};
    std::string reason;
    std::string binary_path;
    std::string scope;
    std::string artifact_path;
    int events{0};
    int invalid_events{0};

    nlohmann::json to_json() const;
};

class EbpfMonitor {
public:
    EbpfMonitor(EbpfMonitorConfig config, std::string run_id, std::string run_dir);
    ~EbpfMonitor();

    static EbpfMonitorConfig from_environment();

    EbpfMonitorSummary start();
    EbpfMonitorSummary stop();
    const EbpfMonitorSummary& summary() const { return summary_; }

private:
    EbpfMonitorConfig config_;
    std::string run_id_;
    std::string run_dir_;
    EbpfMonitorSummary summary_;

#ifndef _WIN32
    struct HelperProcess {
        int pipe_fd{-1};
        int child_pid{-1};
        std::string name;
        bool sudo_control{false};
    };
    std::vector<HelperProcess> helpers_;
#endif
};

}  // namespace mindbridge::observability
