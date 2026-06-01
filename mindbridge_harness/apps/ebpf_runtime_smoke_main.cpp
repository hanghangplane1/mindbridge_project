#include "mindbridge/harness/agent_loop.hpp"
#include "mindbridge/harness/hook_executor.hpp"
#include "mindbridge/harness/permission_checker.hpp"
#include "mindbridge/harness/tool_registry.hpp"
#include "mindbridge/model/model_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

class SmokeModel final : public mindbridge::ModelClient {
public:
    std::string chat(const std::string&, const std::string&) override {
        generate_observable_activity();
        return "ebpf runtime smoke completed";
    }

    void chat_stream(const std::string& system_prompt,
                     const std::string& user_message,
                     const std::function<bool(const std::string& chunk)>& on_chunk) override {
        const std::string out = chat(system_prompt, user_message);
        if (on_chunk) {
            on_chunk(out);
        }
    }

    std::vector<double> embedding(const std::string&, const std::string& = "nomic-embed-text") override {
        return {0.0};
    }

    const std::string& model_id() const override { return model_id_; }

private:
    static void generate_observable_activity() {
        const auto dir = std::filesystem::temp_directory_path() / "mindbridge-ebpf-runtime-smoke";
        std::filesystem::create_directories(dir);
        const auto file = dir / "event.txt";
        const auto renamed = dir / "event-renamed.txt";
        {
            std::ofstream out(file, std::ios::trunc);
            out << "mindbridge ebpf runtime smoke\n";
        }
        std::filesystem::rename(file, renamed);
        {
            std::ofstream out(renamed, std::ios::app);
            out << "append\n";
        }

#ifndef _WIN32
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;
            bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            listen(fd, 1);
            close(fd);
        }
#endif

        std::filesystem::remove(renamed);
        std::filesystem::remove(dir);
        if (std::getenv("MINDBRIDGE_EBPF_RUNTIME_TLS_SMOKE")) {
            const int ignored = std::system(
                "curl -fsS --max-time 4 https://example.com/ >/tmp/mindbridge-ebpf-runtime-tls.out 2>/tmp/mindbridge-ebpf-runtime-tls.err");
            (void)ignored;
        }
        std::this_thread::sleep_for(std::chrono::seconds(6));
    }

    std::string model_id_{"smoke-model"};
};

bool file_contains(const std::filesystem::path& path, const std::string& needle) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
#ifndef _WIN32
    if (!std::getenv("MINDBRIDGE_EBPF_PID")) {
        const std::string self_pid = std::to_string(static_cast<int>(getpid()));
        setenv("MINDBRIDGE_EBPF_PID", self_pid.c_str(), 1);
    }
#endif
    SmokeModel model;
    mindbridge::ToolRegistry tools;
    mindbridge::PermissionChecker permissions(mindbridge::PermissionChecker::Mode::FullAuto);
    mindbridge::HookExecutor hooks;
    mindbridge::AgentLoop loop(model, tools, permissions, hooks);

    mindbridge::AgentLoop::RunInput input;
    input.task_id = "ebpf_runtime_smoke";
    input.user_id = "smoke";
    input.session_id = "ebpf-smoke";
    input.conversation_id = "ebpf-smoke";
    input.system_rules = "Return a deterministic smoke response.";
    input.user_message = "Run eBPF runtime smoke.";
    input.max_attempts = 1;

    const auto result = loop.run(input);
    const std::filesystem::path run_dir(result.run_dir);
    const std::filesystem::path ebpf_events = run_dir / "ebpf_events.jsonl";
    const std::filesystem::path boundary_trace = run_dir / "boundary_trace.jsonl";
    const std::filesystem::path report = run_dir / "observability_report.json";

    const bool has_run_events = file_contains(ebpf_events, "\"run_id\":\"" + result.run_id + "\"") &&
                                file_contains(ebpf_events, "\"event\":\"SUMMARY\"");
    const bool has_action_event = file_contains(ebpf_events, "\"type\":\"WRITE\"") ||
                                  file_contains(ebpf_events, "\"type\":\"MMAP_SHARED\"") ||
                                  file_contains(ebpf_events, "\"type\":\"NET_CONNECT\"") ||
                                  file_contains(ebpf_events, "\"type\":\"NET_BIND\"") ||
                                  file_contains(ebpf_events, "\"type\":\"NET_LISTEN\"") ||
                                  file_contains(ebpf_events, "\"type\":\"FILE_RENAME\"") ||
                                  file_contains(ebpf_events, "\"type\":\"FILE_DELETE\"");
    const bool wants_tls = std::getenv("MINDBRIDGE_EBPF_RUNTIME_TLS_SMOKE") != nullptr;
    const bool has_tls_event = file_contains(ebpf_events, "\"helper\":\"sslsniff\"") &&
                               (file_contains(ebpf_events, "\"function\":\"WRITE/SEND\"") ||
                                file_contains(ebpf_events, "\"function\":\"READ/RECV\""));
    const bool ok = std::filesystem::exists(ebpf_events) &&
                    std::filesystem::exists(boundary_trace) &&
                    std::filesystem::exists(report) &&
                    has_run_events &&
                    has_action_event &&
                    (!wants_tls || has_tls_event);

    nlohmann::json out = {
        {"ok", ok},
        {"run_id", result.run_id},
        {"run_dir", result.run_dir},
        {"ebpf_events", ebpf_events.string()},
        {"boundary_trace", boundary_trace.string()},
        {"observability_report", report.string()},
    };
    std::cout << out.dump(2) << std::endl;
    return ok ? 0 : 1;
}
