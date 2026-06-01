#include "mindbridge/runtime/run_store.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include <filesystem>

namespace mindbridge {
namespace {

std::string iso_utc_from_time(std::chrono::system_clock::time_point now) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

}  // namespace

RunStore::RunStore(std::string root) : root_(std::move(root)) {}

std::string RunStore::start_run(const std::string& task_id) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    run_id_ = task_id + "-" +
              std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    const std::filesystem::path dir = run_dir();
    std::filesystem::create_directories(dir);
    return run_id_;
}

void RunStore::write_task_state(const TaskState& state) {
    if (run_id_.empty()) {
        throw std::runtime_error("run_id is empty; call start_run() first");
    }
    std::ofstream out(std::filesystem::path(run_dir()) / "task_state.json", std::ios::trunc);
    out << state.to_json().dump(2) << "\n";
}

void RunStore::append_trace(const nlohmann::json& event) {
    if (run_id_.empty()) {
        return;
    }
    nlohmann::json enriched = event;
    const auto now = std::chrono::system_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    if (!enriched.contains("wall_time_ns")) {
        enriched["wall_time_ns"] = ns;
    }
    if (!enriched.contains("wall_time")) {
        enriched["wall_time"] = iso_utc_from_time(now);
    }
    std::ofstream out(std::filesystem::path(run_dir()) / "trace.jsonl", std::ios::app);
    out << enriched.dump() << "\n";
}

void RunStore::write_report(const nlohmann::json& report) {
    if (run_id_.empty()) {
        throw std::runtime_error("run_id is empty; call start_run() first");
    }
    std::ofstream out(std::filesystem::path(run_dir()) / "report.json", std::ios::trunc);
    out << report.dump(2) << "\n";
}

std::string RunStore::run_dir() const {
    return (std::filesystem::path(root_) / "runs" / run_id_).string();
}

}  // namespace mindbridge
