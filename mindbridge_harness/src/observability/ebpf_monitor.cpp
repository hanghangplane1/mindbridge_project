#include "mindbridge/observability/ebpf_monitor.hpp"
#include "mindbridge/runtime/utf8.hpp"

#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace mindbridge::observability {
namespace {

std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

bool env_true(const char* name) {
    const std::string value = env_or_empty(name);
    return value == "1" || value == "true" || value == "TRUE" || value == "on" || value == "ON";
}

int env_int(const char* name, int fallback) {
    const std::string value = env_or_empty(name);
    if (value.empty()) {
        return fallback;
    }
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

std::string default_binary_path() {
    const std::string configured = env_or_empty("MINDBRIDGE_EBPF_BINARY");
    if (!configured.empty()) {
        return configured;
    }
    return "./build/mindbridge_harness/mindbridge_ebpf_monitor";
}

std::string default_tls_binary_path() {
    const std::string configured = env_or_empty("MINDBRIDGE_EBPF_TLS_BINARY");
    if (!configured.empty()) {
        return configured;
    }
    return "./build/mindbridge_harness/mindbridge_sslsniff_monitor";
}

nlohmann::json sanitize_event(nlohmann::json event) {
    static const std::vector<std::string> sensitive = {
        "api_key", "apikey", "authorization", "token", "secret", "password", "full_prompt", "payload"};
    for (auto it = event.begin(); it != event.end(); ++it) {
        const std::string key = it.key();
        std::string lowered;
        lowered.reserve(key.size());
        for (unsigned char ch : key) {
            lowered.push_back(static_cast<char>(std::tolower(ch)));
        }
        for (const auto& needle : sensitive) {
            if (lowered.find(needle) != std::string::npos) {
                it.value() = "[redacted]";
                break;
            }
        }
        if (it.value().is_string()) {
            std::string value = sanitize_utf8(it.value().get<std::string>());
            if (value.size() > 2048) {
                value.resize(2048);
                value += "...[clipped]";
            }
            it.value() = value;
        }
    }
    return event;
}

#ifndef _WIN32
bool write_all(int fd, const std::string& text) {
    const char* data = text.data();
    size_t remaining = text.size();
    while (remaining > 0) {
        const ssize_t n = write(fd, data, remaining);
        if (n <= 0) {
            return false;
        }
        data += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

void sudo_kill_process_group(pid_t pgid, const std::string& password) {
    if (pgid <= 0 || password.empty()) {
        return;
    }
    int fds[2];
    if (pipe(fds) != 0) {
        return;
    }
    const pid_t child = fork();
    if (child == 0) {
        close(fds[1]);
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]);
        const std::string target = "-" + std::to_string(static_cast<int>(pgid));
        char sudo[] = "sudo";
        char flag[] = "-S";
        char kill_cmd[] = "kill";
        char sig[] = "-TERM";
        char sep[] = "--";
        char* argv[] = {sudo, flag, kill_cmd, sig, sep, const_cast<char*>(target.c_str()), nullptr};
        execvp("sudo", argv);
        _exit(127);
    }
    close(fds[0]);
    write_all(fds[1], password + "\n");
    close(fds[1]);
    int status = 0;
    waitpid(child, &status, 0);
}
#endif

}  // namespace

nlohmann::json EbpfMonitorSummary::to_json() const {
    return {{"enabled", enabled},
            {"started", started},
            {"unavailable", unavailable},
            {"reason", reason},
            {"binary_path", binary_path},
            {"scope", scope},
            {"artifact_path", artifact_path},
            {"events", events},
            {"invalid_events", invalid_events}};
}

EbpfMonitor::EbpfMonitor(EbpfMonitorConfig config, std::string run_id, std::string run_dir)
    : config_(std::move(config)), run_id_(std::move(run_id)), run_dir_(std::move(run_dir)) {
    summary_.enabled = config_.enabled;
    summary_.binary_path = config_.binary_path;
    summary_.scope = config_.scope;
    summary_.artifact_path = (std::filesystem::path(run_dir_) / "ebpf_events.jsonl").string();
}

EbpfMonitor::~EbpfMonitor() {
    stop();
}

EbpfMonitorConfig EbpfMonitor::from_environment() {
    EbpfMonitorConfig config;
    config.enabled = env_true("MINDBRIDGE_EBPF_ENABLED");
    config.binary_path = default_binary_path();
    const std::string scope = env_or_empty("MINDBRIDGE_EBPF_SCOPE");
    if (!scope.empty()) {
        config.scope = scope;
    }
    config.cgroup_path = env_or_empty("MINDBRIDGE_EBPF_CGROUP_PATH");
    config.command_filter = env_or_empty("MINDBRIDGE_EBPF_COMMANDS");
    const std::string filter_mode = env_or_empty("MINDBRIDGE_EBPF_FILTER_MODE");
    if (!filter_mode.empty()) {
        config.filter_mode = filter_mode;
    }
    config.pid = env_int("MINDBRIDGE_EBPF_PID", 0);
    // If MINDBRIDGE_EBPF_PID is not set, try to read from PID file
    if (config.pid <= 0) {
        std::ifstream pid_file("/tmp/mindbridge_counselor.pid");
        if (pid_file.is_open()) {
            int pid;
            if (pid_file >> pid && pid > 0) {
                config.pid = pid;
            }
        }
    }
    config.sample_interval_ms = env_int("MINDBRIDGE_EBPF_SAMPLE_INTERVAL_MS", 1000);
    config.max_events_per_run = env_int("MINDBRIDGE_EBPF_MAX_EVENTS_PER_RUN", 20000);
    const std::string capture = env_or_empty("MINDBRIDGE_EBPF_CAPTURE_LLM");
    if (!capture.empty()) {
        config.capture_llm = capture;
    }
    const bool trace_all = env_true("MINDBRIDGE_EBPF_TRACE_ALL");
    config.trace_fs = trace_all || env_true("MINDBRIDGE_EBPF_TRACE_FS");
    config.trace_net = trace_all || env_true("MINDBRIDGE_EBPF_TRACE_NET");
    config.trace_signals = trace_all || env_true("MINDBRIDGE_EBPF_TRACE_SIGNALS");
    config.trace_mem = trace_all || env_true("MINDBRIDGE_EBPF_TRACE_MEM");
    config.trace_cow = env_true("MINDBRIDGE_EBPF_TRACE_COW");
    config.trace_resources = env_true("MINDBRIDGE_EBPF_TRACE_RESOURCES");
    config.trace_tls = env_true("MINDBRIDGE_EBPF_TRACE_TLS");
    config.tls_binary_path = default_tls_binary_path();
    config.tls_command_filter = env_or_empty("MINDBRIDGE_EBPF_TLS_COMMANDS");
    if (config.tls_command_filter.empty()) {
        config.tls_command_filter = config.command_filter;
    }
    config.verbose = env_true("MINDBRIDGE_EBPF_VERBOSE");
    return config;
}

EbpfMonitorSummary EbpfMonitor::start() {
    std::ofstream artifact(summary_.artifact_path, std::ios::trunc);
    if (!artifact) {
        summary_.unavailable = true;
        summary_.reason = "cannot create ebpf_events.jsonl";
        return summary_;
    }

    if (!config_.enabled) {
        artifact << nlohmann::json{{"source", "ebpf"},
                                   {"event", "EBPF_DISABLED"},
                                   {"run_id", run_id_},
                                   {"reason", "MINDBRIDGE_EBPF_ENABLED is not set"}}
                        .dump()
                 << "\n";
        summary_.reason = "disabled";
        return summary_;
    }

    if (!std::filesystem::exists(config_.binary_path)) {
        artifact << nlohmann::json{{"source", "ebpf"},
                                   {"event", "EBPF_UNAVAILABLE"},
                                   {"run_id", run_id_},
                                   {"reason", "helper binary not found"},
                                   {"binary_path", config_.binary_path}}
                        .dump()
                 << "\n";
        summary_.unavailable = true;
        summary_.reason = "helper binary not found";
        return summary_;
    }

#ifdef _WIN32
    artifact << nlohmann::json{{"source", "ebpf"},
                               {"event", "EBPF_UNAVAILABLE"},
                               {"run_id", run_id_},
                               {"reason", "eBPF monitor is only supported on Linux"}}
                    .dump()
             << "\n";
    summary_.unavailable = true;
    summary_.reason = "unsupported platform";
    return summary_;
#else
    const bool use_sudo = env_true("MINDBRIDGE_EBPF_USE_SUDO");
    const std::string sudo_script = env_or_empty("MINDBRIDGE_EBPF_SUDO_SCRIPT");
    const std::string sudo_password = env_or_empty("MINDBRIDGE_SUDO_PASSWORD");
    auto start_helper = [this, use_sudo, sudo_script, sudo_password](std::vector<std::string> args, const std::string& name) -> bool {
        int fds[2];
        int stdin_fds[2] = {-1, -1};
        if (pipe(fds) != 0) {
            return false;
        }
        const bool needs_password_pipe = use_sudo && sudo_script.empty() && !sudo_password.empty();
        if (needs_password_pipe && pipe(stdin_fds) != 0) {
            close(fds[0]);
            close(fds[1]);
            return false;
        }
        const pid_t child = fork();
        if (child == 0) {
            setpgid(0, 0);
            close(fds[0]);
            dup2(fds[1], STDOUT_FILENO);
            close(fds[1]);
            if (needs_password_pipe) {
                close(stdin_fds[1]);
                dup2(stdin_fds[0], STDIN_FILENO);
                close(stdin_fds[0]);
            }

            // Close all file descriptors except stdin/stdout/stderr
            // This prevents inheriting listening sockets from parent process
            for (int fd = 3; fd < 1024; fd++) {
                close(fd);
            }

            std::vector<char*> argv;
            if (use_sudo && !sudo_script.empty()) {
                argv.push_back(const_cast<char*>(sudo_script.c_str()));
            } else if (use_sudo) {
                argv.push_back(const_cast<char*>("sudo"));
                argv.push_back(const_cast<char*>("-S"));
            }
            for (auto& arg : args) {
                argv.push_back(arg.data());
            }
            argv.push_back(nullptr);
            if (use_sudo && !sudo_script.empty()) {
                execv(sudo_script.c_str(), argv.data());
            } else if (use_sudo) {
                execvp("sudo", argv.data());
            } else {
                execv(args.front().c_str(), argv.data());
            }
            _exit(127);
        }
        close(fds[1]);
        if (needs_password_pipe) {
            close(stdin_fds[0]);
            write_all(stdin_fds[1], sudo_password + "\n");
            close(stdin_fds[1]);
        }
        if (child < 0) {
            close(fds[0]);
            return false;
        }
        setpgid(child, child);
        helpers_.push_back({fds[0], child, name, use_sudo && sudo_script.empty()});
        return true;
    };

    std::vector<std::string> args = {config_.binary_path, "-m", config_.filter_mode};
    if (config_.pid > 0) {
        args.push_back("-p");
        args.push_back(std::to_string(config_.pid));
    }
    if (!config_.command_filter.empty()) {
        args.push_back("-c");
        args.push_back(config_.command_filter);
    }
    if (config_.trace_fs) {
        args.push_back("--trace-fs");
    }
    if (config_.trace_net) {
        args.push_back("--trace-net");
    }
    if (config_.trace_signals) {
        args.push_back("--trace-signals");
    }
    if (config_.trace_mem) {
        args.push_back("--trace-mem");
    }
    if (config_.trace_cow) {
        args.push_back("--trace-cow");
    }
    if (config_.trace_resources) {
        args.push_back("--trace-resources");
        args.push_back("--sample-interval");
        args.push_back(std::to_string(config_.sample_interval_ms));
    }
    if (!config_.cgroup_path.empty()) {
        if (config_.trace_resources) {
            args.push_back("--cgroup");
            args.push_back(config_.cgroup_path);
        }
        args.push_back("--cgroup-filter");
        args.push_back(config_.cgroup_path);
        args.push_back("--cgroup-filter-children");
    }
    if (config_.verbose) {
        args.push_back("-v");
    }
    if (!start_helper(std::move(args), "process_new")) {
        summary_.unavailable = true;
        summary_.reason = "cannot fork monitor helper";
        return summary_;
    }

    if (config_.trace_tls) {
        if (config_.pid <= 0 && config_.tls_command_filter.empty()) {
            artifact << nlohmann::json{{"source", "ebpf"},
                                       {"event", "EBPF_TLS_DISABLED"},
                                       {"run_id", run_id_},
                                       {"reason", "TLS plaintext capture requires MINDBRIDGE_EBPF_PID or MINDBRIDGE_EBPF_TLS_COMMANDS"}}
                            .dump()
                     << "\n";
        } else if (!std::filesystem::exists(config_.tls_binary_path)) {
            artifact << nlohmann::json{{"source", "ebpf"},
                                       {"event", "EBPF_TLS_UNAVAILABLE"},
                                       {"run_id", run_id_},
                                       {"reason", "TLS helper binary not found"},
                                       {"binary_path", config_.tls_binary_path}}
                            .dump()
                     << "\n";
        } else {
            std::vector<std::string> tls_args = {config_.tls_binary_path};
            if (!config_.tls_command_filter.empty()) {
                tls_args.push_back("-c");
                tls_args.push_back(config_.tls_command_filter);
            } else if (config_.pid > 0) {
                tls_args.push_back("-p");
                tls_args.push_back(std::to_string(config_.pid));
            }
            if (config_.verbose) {
                tls_args.push_back("-v");
            }
            if (!start_helper(std::move(tls_args), "sslsniff")) {
                artifact << nlohmann::json{{"source", "ebpf"},
                                           {"event", "EBPF_TLS_UNAVAILABLE"},
                                           {"run_id", run_id_},
                                           {"reason", "cannot fork TLS helper"},
                                           {"binary_path", config_.tls_binary_path}}
                                .dump()
                         << "\n";
            }
        }
    }

    summary_.started = true;
    summary_.reason = "started";
    return summary_;
#endif
}

EbpfMonitorSummary EbpfMonitor::stop() {
#ifndef _WIN32
    if (helpers_.empty()) {
        return summary_;
    }
    for (const auto& helper : helpers_) {
        if (helper.child_pid > 0) {
            const pid_t pgid = static_cast<pid_t>(helper.child_pid);
            if (helper.sudo_control) {
                sudo_kill_process_group(pgid, env_or_empty("MINDBRIDGE_SUDO_PASSWORD"));
            }
            kill(-pgid, SIGTERM);
        }
    }

    std::ofstream artifact(summary_.artifact_path, std::ios::app);
    char buffer[4096];
    for (auto& helper : helpers_) {
        std::string pending;
        int idle_polls = 0;
        while (helper.pipe_fd >= 0) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(helper.pipe_fd, &rfds);
            timeval timeout{0, 200000};
            const int ready = select(helper.pipe_fd + 1, &rfds, nullptr, nullptr, &timeout);
            if (ready <= 0) {
                if (++idle_polls >= 10) {
                    break;
                }
                continue;
            }
            idle_polls = 0;
            const ssize_t n = read(helper.pipe_fd, buffer, sizeof(buffer));
            if (n <= 0) {
                break;
            }
            pending.append(buffer, static_cast<size_t>(n));
            size_t newline = std::string::npos;
            while ((newline = pending.find('\n')) != std::string::npos) {
                const std::string line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                if (line.empty()) {
                    continue;
                }
                try {
                    nlohmann::json event = sanitize_event(nlohmann::json::parse(line));
                    event["run_id"] = run_id_;
                    event["helper"] = helper.name;
                    artifact << event.dump() << "\n";
                    ++summary_.events;
                } catch (...) {
                    ++summary_.invalid_events;
                }
                if (summary_.events >= config_.max_events_per_run) {
                    break;
                }
            }
            if (summary_.events >= config_.max_events_per_run) {
                break;
            }
        }
    }
    for (auto& helper : helpers_) {
        int status = 0;
        if (helper.child_pid > 0) {
            waitpid(helper.child_pid, &status, WNOHANG);
        }
        if (helper.pipe_fd >= 0) {
            close(helper.pipe_fd);
        }
    }
    helpers_.clear();
    summary_.reason = summary_.started ? "stopped" : summary_.reason;
#endif
    return summary_;
}

}  // namespace mindbridge::observability
