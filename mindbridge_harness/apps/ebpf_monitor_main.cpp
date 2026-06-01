#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) {
    g_stop = 1;
}

std::string arg_value(int argc, char** argv, const std::string& name, const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) {
            return argv[i + 1];
        }
    }
    return fallback;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);

    const std::string run_id = arg_value(argc, argv, "--run-id", "");
    const std::string scope = arg_value(argc, argv, "--scope", "run");
    const int sample_interval_ms = std::max(100, std::stoi(arg_value(argc, argv, "--sample-interval", "1000")));

    std::cout << nlohmann::json{{"source", "ebpf"},
                                {"event", "EBPF_HELPER_STARTED"},
                                {"run_id", run_id},
                                {"scope", scope},
                                {"mode", "adapter-placeholder"},
                                {"note", "Set MINDBRIDGE_EBPF_BINARY to an AgentSight process_new based helper for live kernel events."}}
                     .dump()
              << std::endl;

    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(sample_interval_ms));
        std::cout << nlohmann::json{{"source", "ebpf"},
                                    {"event", "RESOURCE_SAMPLE"},
                                    {"run_id", run_id},
                                    {"scope", scope},
                                    {"total_rss_kb", 0},
                                    {"total_cpu_percent", 0},
                                    {"note", "placeholder sample"}}
                         .dump()
                  << std::endl;
    }

    std::cout << nlohmann::json{{"source", "ebpf"},
                                {"event", "EBPF_HELPER_STOPPED"},
                                {"run_id", run_id},
                                {"scope", scope}}
                     .dump()
              << std::endl;
    return 0;
}
