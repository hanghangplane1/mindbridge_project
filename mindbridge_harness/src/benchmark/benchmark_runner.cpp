#include "mindbridge/benchmark/benchmark_runner.hpp"

#include "mindbridge/benchmark/hardness_scorer.hpp"
#include "mindbridge/runtime/risk_policy.hpp"

#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>

namespace mindbridge {

namespace {

size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::string*>(userp);
    out->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

std::string http_get(const std::string& url, long timeout_sec) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl init");
    }
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        throw std::runtime_error(curl_easy_strerror(rc));
    }
    return response;
}

std::string http_post_json(const std::string& url, const std::string& body, long timeout_sec) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl init");
    }
    std::string response;
    struct curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        throw std::runtime_error(curl_easy_strerror(rc));
    }
    return response;
}

void add_failure(BenchmarkSummary& summary, const std::string& category) {
    summary.failure_categories[category] = summary.failure_categories.value(category, 0) + 1;
}

bool verify_artifacts(const nlohmann::json& task, nlohmann::json& row, std::string& category) {
    if (!task.contains("expected_artifacts") || !task["expected_artifacts"].is_array()) {
        return true;
    }
    for (const auto& artifact : task["expected_artifacts"]) {
        const std::string path = artifact.get<std::string>();
        if (!std::filesystem::exists(path)) {
            row["missing_artifact"] = path;
            category = "verifier_failed";
            return false;
        }
    }
    return true;
}

bool verify_trace_events(const nlohmann::json& task, nlohmann::json& row, std::string& category) {
    if (!task.contains("trace_file") || !task.contains("trace_expect_events")) {
        return true;
    }
    const std::string trace_file = task["trace_file"].get<std::string>();
    std::ifstream in(trace_file);
    if (!in) {
        row["missing_trace"] = trace_file;
        category = "protocol_error";
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (const auto& event : task["trace_expect_events"]) {
        const std::string needle = event.get<std::string>();
        if (content.find(needle) == std::string::npos) {
            row["missing_trace_event"] = needle;
            category = "protocol_error";
            return false;
        }
    }
    return true;
}

}  // namespace

BenchmarkSummary BenchmarkRunner::run_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open benchmark file: " + path);
    }
    nlohmann::json root;
    in >> root;
    if (!root.contains("tasks") || !root["tasks"].is_array()) {
        throw std::runtime_error("invalid benchmark json: missing tasks[]");
    }

    BenchmarkSummary summary;
    for (const auto& task : root["tasks"]) {
        summary.total++;
        nlohmann::json row;
        row["id"] = task.value("id", "");
        const std::string kind = task.value("kind", "hardness");
        bool ok = true;
        std::string failure_category = "none";
        try {
            if (kind == "hardness") {
                const int score = HardnessScorer::score_from_task(task);
                row["hardness_score"] = score;
                row["tier"] = HardnessScorer::tier_from_score(score);
            } else if (kind == "quality") {
                int score = 0;
                score += task.value("requires_empathy", false) ? 2 : 0;
                score += task.value("requires_rag", false) ? 2 : 0;
                score += task.value("multi_turn", false) ? 2 : 0;
                score += task.value("crisis", false) ? 4 : 0;
                score += task.value("requires_mcp", false) ? 3 : 0;
                row["quality_score"] = score;
                row["metrics"] = {
                    {"request_success_rate", task.value("request_success_rate", 1.0)},
                    {"rag_hit_rate", task.value("rag_hit_rate", task.value("requires_rag", false) ? 1.0 : 0.0)},
                    {"risk_detection_accuracy", task.value("risk_detection_accuracy", task.value("crisis", false) ? 1.0 : 0.0)},
                    {"mcp_delivery_rate", task.value("mcp_delivery_rate", task.value("requires_mcp", false) ? 1.0 : 0.0)},
                    {"protocol_compliance", task.value("protocol_compliance", 1.0)},
                };
            } else if (kind == "contract") {
                const std::string prompt = task.value("prompt", "");
                const std::string expected_intent = task.value("expected_intent", "");
                const std::string expected_risk = task.value("expected_risk_level", "");
                const std::string predicted_intent = RiskPolicy::heuristic_intent(prompt);
                const std::string predicted_risk = RiskPolicy::heuristic_risk_level(prompt);
                row["predicted_intent"] = predicted_intent;
                row["predicted_risk_level"] = predicted_risk;
                if (!expected_intent.empty() && predicted_intent != expected_intent) {
                    ok = false;
                    failure_category = "verifier_failed";
                    row["error"] = "intent mismatch";
                }
                if (ok && !expected_risk.empty() && predicted_risk != expected_risk) {
                    ok = false;
                    failure_category = "risk_mismatch";
                    row["error"] = "risk level mismatch";
                }
                if (ok && task.value("requires_mcp", false) &&
                    !task.value("mcp_triggered", false)) {
                    ok = false;
                    failure_category = "missing_mcp";
                    row["error"] = "mcp is required but not triggered";
                }
                if (ok && task.value("prompt_chars", 0) > task.value("prompt_char_budget", 12000)) {
                    ok = false;
                    failure_category = "budget_exceeded";
                    row["error"] = "prompt exceeds char budget";
                }
                if (ok) {
                    const std::string verifier = task.value("verifier", "");
                    const std::string output = task.value("deterministic_output", task.value("output", ""));
                    if (!verifier.empty()) {
                        try {
                            if (!std::regex_search(output, std::regex(verifier))) {
                                ok = false;
                                failure_category = "verifier_failed";
                                row["error"] = "verifier regex not matched";
                            }
                        } catch (...) {
                            if (output.find(verifier) == std::string::npos) {
                                ok = false;
                                failure_category = "verifier_failed";
                                row["error"] = "verifier string not matched";
                            }
                        }
                    }
                }
                if (ok && !verify_artifacts(task, row, failure_category)) {
                    ok = false;
                }
                if (ok && !verify_trace_events(task, row, failure_category)) {
                    ok = false;
                }
            } else if (kind == "http") {
                const std::string method = task.value("method", "GET");
                const std::string url = task.at("url").get<std::string>();
                std::string body_str;
                if (method == "GET") {
                    body_str = http_get(url, task.value("timeout_sec", 10L));
                } else if (method == "POST") {
                    const auto& body = task.contains("body") ? task["body"] : nlohmann::json::object();
                    body_str = http_post_json(url, body.dump(), task.value("timeout_sec", 30L));
                } else {
                    throw std::runtime_error("unsupported http method: " + method);
                }
                if (task.contains("expect_substring")) {
                    const std::string needle = task["expect_substring"].get<std::string>();
                    if (body_str.find(needle) == std::string::npos) {
                        ok = false;
                        row["error"] = "expect_substring not found";
                    }
                }
                row["response_chars"] = body_str.size();
            } else {
                ok = false;
                failure_category = "protocol_error";
                row["error"] = "unknown kind: " + kind;
            }
        } catch (const std::exception& e) {
            ok = false;
            if (failure_category == "none") {
                failure_category = "protocol_error";
            }
            row["error"] = e.what();
        }
        row["passed"] = ok;
        row["failure_category"] = ok ? "none" : failure_category;
        if (ok) {
            summary.passed++;
        } else {
            summary.failed++;
            add_failure(summary, failure_category);
        }
        summary.details.push_back(row);
    }
    return summary;
}

}  // namespace mindbridge
