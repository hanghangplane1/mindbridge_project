#include "mindbridge/observability/boundary_trace_correlator.hpp"
#include "mindbridge/runtime/utf8.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

namespace mindbridge::observability {
namespace {

constexpr long long kNsPerMs = 1000000LL;

struct IntentRecord {
    nlohmann::json event;
    std::set<std::string> refs;
    long long wall_time_ns{0};
};

struct ActionRecord {
    nlohmann::json event;
    std::string name;
    std::string args_text;
    long long wall_time_ns{0};
    int pid{0};
    int ppid{0};
    bool visible_without_span{false};
};

std::vector<nlohmann::json> read_jsonl(const std::filesystem::path& path) {
    std::vector<nlohmann::json> rows;
    std::ifstream in(path);
    if (!in) {
        return rows;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        try {
            rows.push_back(nlohmann::json::parse(line));
        } catch (...) {
            rows.push_back({{"raw", sanitize_utf8(line)}});
        }
    }
    return rows;
}

std::string as_string(const nlohmann::json& value, const std::string& fallback = "") {
    if (value.is_string()) {
        return sanitize_utf8(value.get<std::string>());
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<long long>());
    }
    if (value.is_number_float()) {
        return std::to_string(value.get<double>());
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    return fallback;
}

long long as_i64(const nlohmann::json& row, const char* key, long long fallback = 0) {
    if (!row.contains(key)) {
        return fallback;
    }
    const auto& value = row[key];
    if (value.is_number_integer()) {
        return value.get<long long>();
    }
    if (value.is_number_unsigned()) {
        return static_cast<long long>(value.get<unsigned long long>());
    }
    if (value.is_string()) {
        try {
            return std::stoll(value.get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

double as_double(const nlohmann::json& row, const char* key, double fallback = 0.0) {
    if (!row.contains(key)) {
        return fallback;
    }
    const auto& value = row[key];
    if (value.is_number()) {
        return value.get<double>();
    }
    if (value.is_string()) {
        try {
            return std::stod(value.get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::string lower(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

int env_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (!value) {
        return fallback;
    }
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

std::string event_name(const nlohmann::json& row) {
    if (row.contains("event")) {
        return as_string(row["event"], "unknown");
    }
    if (row.contains("type")) {
        return as_string(row["type"], "unknown");
    }
    if (row.contains("function")) {
        std::string name = as_string(row["function"], "unknown");
        for (char& ch : name) {
            if (ch == '/' || ch == ' ') {
                ch = '_';
            }
        }
        return "TLS_" + name;
    }
    return "unknown";
}

bool is_resource_event(const std::string& name) {
    return name == "RESOURCE_SAMPLE" || name == "RESOURCE_DETAIL" ||
           name.find("RESOURCE") != std::string::npos;
}

bool is_warning_event(const std::string& name) {
    return name.find("WARNING") != std::string::npos || name.find("UNAVAILABLE") != std::string::npos ||
           name.find("DENIED") != std::string::npos || name.find("DISABLED") != std::string::npos;
}

bool is_tls_event(const nlohmann::json& row, const std::string& name) {
    return name.rfind("TLS_", 0) == 0 || row.contains("function") || row.value("helper", "") == "sslsniff";
}

bool is_observer_noise(const nlohmann::json& row, const std::string& name) {
    if (name == "CLOCK_SYNC") {
        return true;
    }
    const std::string comm = lower(row.value("comm", ""));
    const std::string helper = lower(row.value("helper", ""));
    const std::string path = lower(row.value("filepath", ""));
    const std::string detail = lower(row.value("detail", ""));
    if (comm.find("mindbridge_ebpf") != std::string::npos ||
        comm.find("mindbridge_ssls") != std::string::npos ||
        comm == "sudo" ||
        comm == "kill" ||
        (helper == "process_new" && path.find("/sys/kernel/debug/tracing/") != std::string::npos)) {
        return true;
    }
    if (path.find("/sys/kernel/debug/tracing/") != std::string::npos ||
        (path.find("/proc/") == 0 && (path.find("/status") != std::string::npos ||
                                      path.find("/stat") != std::string::npos ||
                                      path.find("/statm") != std::string::npos ||
                                      path.find("/cgroup") != std::string::npos))) {
        return true;
    }
    return detail.find("target=0,sig=") != std::string::npos;
}

void append_json_text(const nlohmann::json& value, std::ostringstream& out, int depth = 0) {
    if (depth > 3) {
        return;
    }
    if (value.is_string() || value.is_number() || value.is_boolean()) {
        out << ' ' << as_string(value);
        return;
    }
    if (value.is_array()) {
        for (const auto& item : value) {
            append_json_text(item, out, depth + 1);
        }
        return;
    }
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            out << ' ' << it.key();
            append_json_text(it.value(), out, depth + 1);
        }
    }
}

std::set<std::string> extract_references(const std::string& text) {
    static const std::vector<std::regex> patterns = {
        std::regex(R"((/[\w.\-+/=:@]+))"),
        std::regex(R"((https?://[^\s"'<>]+))", std::regex::icase),
        std::regex(R"(\b([A-Za-z0-9_.-]+\.(?:cpp|hpp|cc|c|h|py|js|ts|json|yaml|yml|md|txt|log|sh|cmake))\b)",
                   std::regex::icase),
        std::regex(R"(\b(cmake|make|bash|sh|python3?|node|npm|curl|git|grep|rg|sed|cat|ls|clang|gcc|g\+\+)\b)",
                   std::regex::icase),
        std::regex(R"(\b([A-Za-z0-9.-]+\.[A-Za-z]{2,})(?::\d+)?\b)", std::regex::icase),
    };
    std::set<std::string> refs;
    for (const auto& pattern : patterns) {
        for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
            std::string ref = it->str(1).empty() ? it->str(0) : it->str(1);
            while (!ref.empty() && std::string(".,;:!?)]}\"'").find(ref.back()) != std::string::npos) {
                ref.pop_back();
            }
            if (ref.size() > 2) {
                refs.insert(lower(ref));
            }
        }
    }
    return refs;
}

std::set<std::string> extract_references(const nlohmann::json& row) {
    std::ostringstream text;
    for (const auto& key : {"tool", "arguments", "summary", "user_message_excerpt", "prompt_excerpt", "response_excerpt",
                            "filename", "filepath", "full_command", "remote_addr", "data"}) {
        if (row.contains(key)) {
            append_json_text(row[key], text);
        }
    }
    return extract_references(text.str());
}

std::string action_argument_text(const nlohmann::json& row) {
    std::ostringstream text;
    for (const auto& key : {"comm", "filename", "filepath", "full_command", "remote_addr", "remote_port", "data"}) {
        if (row.contains(key)) {
            append_json_text(row[key], text);
        }
    }
    return lower(text.str());
}

int argument_match_count(const std::set<std::string>& refs, const std::string& args_text) {
    int matches = 0;
    for (const auto& ref : refs) {
        if (!ref.empty() && args_text.find(ref) != std::string::npos) {
            ++matches;
        }
    }
    return matches;
}

bool is_related_pid(int pid, int last_pid, const std::map<int, int>& parents) {
    if (pid <= 0 || last_pid <= 0) {
        return false;
    }
    if (pid == last_pid) {
        return true;
    }
    int current = pid;
    for (int i = 0; i < 16 && current > 0; ++i) {
        auto it = parents.find(current);
        if (it == parents.end()) {
            break;
        }
        current = it->second;
        if (current == last_pid) {
            return true;
        }
    }
    current = last_pid;
    for (int i = 0; i < 16 && current > 0; ++i) {
        auto it = parents.find(current);
        if (it == parents.end()) {
            break;
        }
        current = it->second;
        if (current == pid) {
            return true;
        }
    }
    return false;
}

nlohmann::json compact_intent(const nlohmann::json& row, int index) {
    nlohmann::json out = {{"source", "mindbridge_trace"},
                          {"kind", "intent"},
                          {"sequence", index},
                          {"event", event_name(row)}};
    for (const auto& key : {"run_id", "task_id", "conversation_id", "attempt", "stream", "tool", "ok",
                            "duration_ms", "status", "stop_reason", "wall_time", "wall_time_ns",
                            "prompt_chars", "user_message_chars", "response_chars", "user_message_excerpt",
                            "prompt_excerpt", "response_excerpt"}) {
        if (row.contains(key)) {
            out[key] = row[key];
        }
    }
    return out;
}

nlohmann::json compact_action(const nlohmann::json& row, int index, long long wall_time_ns) {
    const std::string name = event_name(row);
    nlohmann::json out = {{"source", "ebpf"},
                          {"kind", is_resource_event(name) ? "resource_metric" : "action"},
                          {"sequence", index},
                          {"event", name}};
    for (const auto& key : {"timestamp", "pid", "ppid", "comm", "filename", "filepath", "full_command",
                            "duration_ms", "exit_code", "count", "event_type", "summary_type",
                            "remote_addr", "remote_port", "total_rss_kb", "total_cpu_percent",
                            "total_cpu_user_ms", "total_cpu_sys_ms", "num_processes",
                            "cgroup_memory_bytes", "cgroup_memory_peak_bytes", "cgroup_cpu_usage_usec",
                            "container_id", "ns_pid", "reason", "helper", "function", "len", "buf_size",
                            "latency_ms", "is_handshake", "truncated", "data"}) {
        if (row.contains(key)) {
            out[key] = row[key];
        }
    }
    if (wall_time_ns > 0) {
        out["wall_time_ns"] = wall_time_ns;
    }
    return out;
}

long long action_wall_time_ns(const nlohmann::json& row, long long mono_sync_ns, long long wall_sync_ns) {
    const long long direct = as_i64(row, "wall_time_ns", 0);
    if (direct > 0) {
        return direct;
    }
    const long long mono = as_i64(row, "timestamp", 0);
    if (mono > 0 && mono_sync_ns > 0 && wall_sync_ns > 0) {
        return wall_sync_ns + (mono - mono_sync_ns);
    }
    return 0;
}

nlohmann::json performance_metrics(const std::vector<ActionRecord>& actions) {
    std::vector<ActionRecord> resources;
    for (const auto& action : actions) {
        if (action.event.value("kind", "") == "resource_metric" && action.name == "RESOURCE_SAMPLE") {
            resources.push_back(action);
        }
    }
    std::sort(resources.begin(), resources.end(), [](const auto& a, const auto& b) {
        return a.wall_time_ns < b.wall_time_ns;
    });

    std::vector<double> memory_mb;
    std::vector<double> cpu_percent;
    std::vector<nlohmann::json> samples;
    long long prev_time = 0;
    double prev_cpu_ms = 0.0;
    for (const auto& resource : resources) {
        const auto& row = resource.event;
        double memory = as_double(row, "total_rss_kb", 0.0) / 1024.0;
        if (memory <= 0.0 && row.contains("cgroup_memory_bytes")) {
            memory = as_double(row, "cgroup_memory_bytes", 0.0) / (1024.0 * 1024.0);
        }
        const double cumulative_cpu_ms = as_double(row, "total_cpu_user_ms", 0.0) +
                                         as_double(row, "total_cpu_sys_ms", 0.0);
        double cpu = as_double(row, "total_cpu_percent", -1.0);
        if (cpu < 0.0 && prev_time > 0 && resource.wall_time_ns > prev_time) {
            const double elapsed_ms = static_cast<double>(resource.wall_time_ns - prev_time) / 1000000.0;
            if (elapsed_ms > 0.0 && cumulative_cpu_ms >= prev_cpu_ms) {
                cpu = ((cumulative_cpu_ms - prev_cpu_ms) / elapsed_ms) * 100.0;
            }
        }
        if (cpu < 0.0) {
            cpu = 0.0;
        }
        memory_mb.push_back(memory);
        cpu_percent.push_back(cpu);
        samples.push_back({{"wall_time_ns", resource.wall_time_ns},
                           {"memory_mb", memory},
                           {"cpu_percent", cpu},
                           {"num_processes", row.value("num_processes", 0)}});
        prev_time = resource.wall_time_ns;
        prev_cpu_ms = cumulative_cpu_ms;
    }

    auto avg = [](const std::vector<double>& values) {
        if (values.empty()) {
            return 0.0;
        }
        return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    };
    auto peak = [](const std::vector<double>& values) {
        if (values.empty()) {
            return 0.0;
        }
        return *std::max_element(values.begin(), values.end());
    };
    return {{"sample_count", samples.size()},
            {"avg_cpu_percent", avg(cpu_percent)},
            {"peak_cpu_percent", peak(cpu_percent)},
            {"avg_memory_mb", avg(memory_mb)},
            {"peak_memory_mb", peak(memory_mb)},
            {"samples", samples}};
}

}  // namespace

nlohmann::json BoundaryTraceSummary::to_json() const {
    return {{"intent_events", intent_events},
            {"action_events", action_events},
            {"resource_events", resource_events},
            {"correlated_spans", correlated_spans},
            {"suppressed_action_events", suppressed_action_events},
            {"warnings", warnings},
            {"boundary_trace_path", boundary_trace_path},
            {"observability_report_path", observability_report_path}};
}

BoundaryTraceCorrelator::BoundaryTraceCorrelator(std::string run_dir) : run_dir_(std::move(run_dir)) {}

BoundaryTraceSummary BoundaryTraceCorrelator::correlate(const nlohmann::json& ebpf_summary) {
    const std::filesystem::path dir(run_dir_);
    const auto trace_rows = read_jsonl(dir / "trace.jsonl");
    const auto ebpf_rows = read_jsonl(dir / "ebpf_events.jsonl");
    const std::filesystem::path boundary_path = dir / "boundary_trace.jsonl";
    const std::filesystem::path report_path = dir / "observability_report.json";
    const int window_ms = env_int("MINDBRIDGE_OBS_CORRELATION_WINDOW_MS", 500);

    long long mono_sync_ns = 0;
    long long wall_sync_ns = 0;
    for (const auto& row : ebpf_rows) {
        if (event_name(row) == "CLOCK_SYNC") {
            mono_sync_ns = as_i64(row, "mono_ns", as_i64(row, "timestamp", 0));
            wall_sync_ns = as_i64(row, "wall_time_ns", 0);
            break;
        }
    }

    BoundaryTraceSummary summary;
    summary.boundary_trace_path = boundary_path.string();
    summary.observability_report_path = report_path.string();

    std::vector<IntentRecord> intents;
    std::vector<ActionRecord> actions;
    std::map<std::string, int> action_counts;
    std::map<std::string, int> source_counts;
    std::set<int> pids;
    std::map<int, int> parents;

    int sequence = 0;
    for (const auto& row : trace_rows) {
        IntentRecord intent;
        intent.event = compact_intent(row, sequence++);
        intent.refs = extract_references(row);
        intent.wall_time_ns = as_i64(row, "wall_time_ns", 0);
        if (!intent.refs.empty()) {
            intent.event["references"] = nlohmann::json::array();
            for (const auto& ref : intent.refs) {
                intent.event["references"].push_back(ref);
            }
        }
        intents.push_back(std::move(intent));
        ++summary.intent_events;
        ++source_counts["mindbridge_trace"];
    }

    for (const auto& row : ebpf_rows) {
        const std::string name = event_name(row);
        const long long wall_time = action_wall_time_ns(row, mono_sync_ns, wall_sync_ns);
        ActionRecord action;
        action.name = name;
        action.event = compact_action(row, sequence++, wall_time);
        action.args_text = action_argument_text(row);
        action.wall_time_ns = wall_time;
        action.pid = static_cast<int>(as_i64(row, "pid", 0));
        action.ppid = static_cast<int>(as_i64(row, "ppid", 0));
        const bool observer_noise = is_observer_noise(row, name);
        if (observer_noise) {
            action.event["observer_noise"] = true;
        }
        action.visible_without_span = !observer_noise && (is_resource_event(name) || is_warning_event(name) || is_tls_event(row, name));
        ++action_counts[name];
        ++source_counts[row.value("source", "ebpf")];
        if (action.pid > 0) {
            pids.insert(action.pid);
        }
        if (action.pid > 0 && action.ppid > 0) {
            parents[action.pid] = action.ppid;
        }
        if (action.event["kind"] == "resource_metric") {
            ++summary.resource_events;
        } else {
            ++summary.action_events;
        }
        if (is_warning_event(name)) {
            ++summary.warnings;
        }
        actions.push_back(std::move(action));
    }

    std::ofstream boundary(boundary_path, std::ios::trunc);
    int out_sequence = 0;
    for (auto& intent : intents) {
        intent.event["sequence"] = out_sequence++;
        boundary << intent.event.dump() << "\n";
    }

    std::map<int, int> last_pid_by_intent;
    int raw_action_events = 0;
    int correlated_action_events = 0;
    int visible_action_events = 0;
    for (auto& action : actions) {
        if (action.name == "CLOCK_SYNC") {
            continue;
        }
        ++raw_action_events;
        if (action.event.value("observer_noise", false)) {
            ++summary.suppressed_action_events;
            continue;
        }
        int best_index = -1;
        double best_score = 0.0;
        nlohmann::json best_evidence = nlohmann::json::object();
        for (int i = 0; i < static_cast<int>(intents.size()); ++i) {
            const auto& intent = intents[i];
            double score = 0.0;
            nlohmann::json evidence = nlohmann::json::object();
            if (intent.wall_time_ns > 0 && action.wall_time_ns > 0) {
                const long long delta_ms = (action.wall_time_ns - intent.wall_time_ns) / kNsPerMs;
                if (delta_ms >= 0 && delta_ms <= window_ms) {
                    score += 0.55;
                    evidence["temporal_delta_ms"] = delta_ms;
                }
            }
            const int arg_matches = argument_match_count(intent.refs, action.args_text);
            if (arg_matches > 0) {
                score += std::min(0.35, 0.15 * static_cast<double>(arg_matches));
                evidence["argument_matches"] = arg_matches;
            }
            auto last_pid = last_pid_by_intent.find(i);
            if (last_pid != last_pid_by_intent.end() && is_related_pid(action.pid, last_pid->second, parents)) {
                score += 0.15;
                evidence["process_lineage"] = true;
            }
            if (score > best_score) {
                best_score = score;
                best_index = i;
                best_evidence = evidence;
            }
        }

        const bool correlated = best_index >= 0 && best_score >= 0.45;
        const bool visible = correlated || action.visible_without_span;
        if (!visible) {
            ++summary.suppressed_action_events;
            continue;
        }
        action.event["sequence"] = out_sequence++;
        boundary << action.event.dump() << "\n";
        ++visible_action_events;
        if (correlated) {
            ++correlated_action_events;
            ++summary.correlated_spans;
            if (action.pid > 0) {
                last_pid_by_intent[best_index] = action.pid;
            }
            const std::string confidence = best_score >= 0.8 ? "high" : (best_score >= 0.55 ? "medium" : "low");
            nlohmann::json span = {{"source", "boundary_correlator"},
                                   {"kind", "correlated_span"},
                                   {"sequence", out_sequence++},
                                   {"intent_event", intents[best_index].event.value("event", "intent")},
                                   {"action_event", action.name},
                                   {"rule", "temporal-window-argument-process"},
                                   {"correlation_score", best_score},
                                   {"confidence", confidence},
                                   {"window_ms", window_ms},
                                   {"evidence", best_evidence},
                                   {"intent", intents[best_index].event},
                                   {"action", action.event}};
            boundary << span.dump() << "\n";
        }
    }

    nlohmann::json report = {
        {"schema_version", 2},
        {"summary", summary.to_json()},
        {"ebpf", ebpf_summary},
        {"event_counts", action_counts},
        {"source_counts", source_counts},
        {"process_count", pids.size()},
        {"performance_metrics", performance_metrics(actions)},
        {"correlation_summary",
         {{"rule", "temporal-window-argument-process"},
          {"window_ms", window_ms},
          {"raw_action_events", raw_action_events},
          {"visible_action_events", visible_action_events},
          {"correlated_action_events", correlated_action_events},
          {"suppressed_action_events", summary.suppressed_action_events},
          {"clock_sync_available", mono_sync_ns > 0 && wall_sync_ns > 0}}},
        {"capture_boundaries",
         nlohmann::json::array({"A2L", "A2T_COMMAND", "A2T_NETWORK", "TLS_PLAINTEXT", "MCP_STDIO", "A2A"})},
        {"notes",
         nlohmann::json::array({"LLM text excerpts require MINDBRIDGE_OBSERVABILITY_CAPTURE_TEXT=1.",
                                "MCP remains behind ToolRegistry, PermissionChecker, HookExecutor, and TraceRecorder.",
                                "Uncorrelated raw eBPF events remain available in ebpf_events.jsonl."})}};
    std::ofstream report_out(report_path, std::ios::trunc);
    report_out << report.dump(2) << "\n";
    return summary;
}

}  // namespace mindbridge::observability
