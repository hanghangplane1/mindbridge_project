#include "mindbridge/platform/universal_event_mapper.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace mindbridge {
namespace platform {
namespace {

std::string mapped_type(const std::string& event) {
    if (event == "run_started") {
        return "session.run.started";
    }
    if (event == "model_requested") {
        return "model.requested";
    }
    if (event == "run_finished") {
        return "session.run.finished";
    }
    if (event.find("tool") != std::string::npos) {
        return "tool.event";
    }
    if (event.find("risk") != std::string::npos) {
        return "risk.assessed";
    }
    if (event.find("ebpf") != std::string::npos || event.find("correlation") != std::string::npos) {
        return "observability.updated";
    }
    return "trace." + (event.empty() ? std::string("event") : event);
}

std::int64_t timestamp_ms(const nlohmann::json& row) {
    if (row.contains("wall_time_ns") && row["wall_time_ns"].is_number_integer()) {
        return row["wall_time_ns"].get<std::int64_t>() / 1000000;
    }
    if (row.contains("timestamp_ms") && row["timestamp_ms"].is_number_integer()) {
        return row["timestamp_ms"].get<std::int64_t>();
    }
    return 0;
}

std::vector<nlohmann::json> read_jsonl(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::vector<nlohmann::json> out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (!parsed.is_discarded()) {
            out.push_back(std::move(parsed));
        }
    }
    return out;
}

bool exists_nonempty(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 0;
}

}  // namespace

UniversalEvent map_trace_event(const nlohmann::json& trace_row,
                               const std::string& session_id,
                               const std::string& run_id) {
    const std::string raw_event = trace_row.value("event", "");
    UniversalEvent event;
    event.session_id = session_id;
    event.run_id = run_id;
    event.type = mapped_type(raw_event);
    event.source = "mindbridge.trace";
    event.timestamp_ms = timestamp_ms(trace_row);
    event.event_id = session_id + ":" + run_id + ":" + event.type + ":" + std::to_string(event.timestamp_ms);
    event.payload = trace_row;
    return event;
}

std::vector<UniversalEvent> load_session_events(const std::filesystem::path& run_root,
                                                const std::string& session_id,
                                                const std::string& run_id) {
    std::vector<UniversalEvent> events;
    if (run_id.empty()) {
        return events;
    }
    const auto run_dir = run_root / "runs" / run_id;
    for (const auto& row : read_jsonl(run_dir / "trace.jsonl")) {
        events.push_back(map_trace_event(row, session_id, run_id));
    }
    for (const auto& row : read_jsonl(run_dir / "boundary_trace.jsonl")) {
        UniversalEvent event = map_trace_event(row, session_id, run_id);
        event.type = "observability.updated";
        event.source = "mindbridge.observability";
        events.push_back(std::move(event));
    }
    for (const auto& name : {"task_state.json", "report.json", "observability_report.json"}) {
        if (exists_nonempty(run_dir / name)) {
            UniversalEvent event;
            event.session_id = session_id;
            event.run_id = run_id;
            event.type = "artifact.created";
            event.source = "mindbridge.run_store";
            event.event_id = session_id + ":" + run_id + ":artifact:" + name;
            event.payload = {{"artifact", name}, {"path", (run_dir / name).string()}};
            events.push_back(std::move(event));
        }
    }
    std::stable_sort(events.begin(), events.end(), [](const UniversalEvent& lhs, const UniversalEvent& rhs) {
        return lhs.timestamp_ms < rhs.timestamp_ms;
    });
    return events;
}

nlohmann::json list_run_artifacts(const std::filesystem::path& run_root, const std::string& run_id) {
    nlohmann::json artifacts = nlohmann::json::array();
    if (run_id.empty()) {
        return artifacts;
    }
    const auto run_dir = run_root / "runs" / run_id;
    for (const auto& name :
         {"task_state.json", "trace.jsonl", "report.json", "boundary_trace.jsonl", "observability_report.json"}) {
        const auto path = run_dir / name;
        if (std::filesystem::exists(path)) {
            std::error_code ec;
            artifacts.push_back({{"name", name},
                                 {"path", path.string()},
                                 {"size_bytes", static_cast<std::int64_t>(std::filesystem::file_size(path, ec))}});
        }
    }
    return artifacts;
}

}  // namespace platform
}  // namespace mindbridge
