#include "agent_rpc/orchestrator/agent_info.h"
#include "agent_rpc/orchestrator/agent_router.h"
#include "mindbridge/apps/app_support.hpp"
#include "mindbridge/harness/tool_registry.hpp"
#include "mindbridge/multimodal/multimodal_tools.hpp"
#include "mindbridge/net/async_http_server.hpp"
#include "mindbridge/net/managed_endpoint_router.hpp"
#include "mindbridge/runtime/risk_policy.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace {

struct MessageTurn {
    std::string role;
    std::string content;
};

std::mutex transcript_mutex;
std::unordered_map<std::string, std::vector<MessageTurn>> transcripts;
constexpr size_t kMaxTranscriptMessages = 20;

std::string trim_copy(const std::string& value) {
    auto begin = value.begin();
    while (begin != value.end() && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    auto end = value.end();
    while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }
    return std::string(begin, end);
}

std::vector<std::string> split_urls(const std::string& raw) {
    std::vector<std::string> urls;
    std::stringstream input(raw);
    std::string item;
    while (std::getline(input, item, ',')) {
        item = trim_copy(item);
        if (!item.empty()) {
            urls.push_back(item);
        }
    }
    return urls;
}

std::vector<std::string> endpoint_urls(const std::string& list_env,
    const std::string& single_env,
    const std::string& fallback_url) {
    std::vector<std::string> urls = split_urls(mindbridge::app::env_or(list_env.c_str(), ""));
    if (urls.empty()) {
        urls.push_back(mindbridge::app::env_or(single_env.c_str(), fallback_url));
    }
    return urls;
}

int env_int(const char* key, int fallback) {
    const char* value = std::getenv(key);
    if (!value || std::string(value).empty()) {
        return fallback;
    }
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

mindbridge::net::EndpointRouterOptions endpoint_router_options() {
    mindbridge::net::EndpointRouterOptions options;
    options.health_interval_ms = env_int("MINDBRIDGE_HEALTH_INTERVAL_MS", 2000);
    options.health_fail_threshold = env_int("MINDBRIDGE_HEALTH_FAIL_THRESHOLD", 2);
    options.health_recover_threshold = env_int("MINDBRIDGE_HEALTH_RECOVER_THRESHOLD", 1);
    options.circuit_open_ms = env_int("MINDBRIDGE_CIRCUIT_OPEN_MS", 10000);
    options.health_timeout_sec = env_int("MINDBRIDGE_HEALTH_TIMEOUT_SEC", 2);
    options.client_options.verify_tls =
        mindbridge::app::env_or("MINDBRIDGE_NET_TLS_VERIFY", "true") != "false";
    options.client_options.ca_file = mindbridge::app::env_or("MINDBRIDGE_NET_CA_FILE");
    return options;
}

mindbridge::ToolRegistry& multimodal_tools() {
    static std::unique_ptr<mindbridge::ToolRegistry> registry = [] {
        auto tools = std::make_unique<mindbridge::ToolRegistry>();
        mindbridge::register_multimodal_tools(*tools);
        return tools;
    }();
    return *registry;
}

mindbridge::net::ManagedEndpointRouter& counselor_router() {
    static mindbridge::net::ManagedEndpointRouter router(
        "counselor",
        "counselor",
        endpoint_urls("MINDBRIDGE_COUNSELOR_URLS",
                      "MINDBRIDGE_COUNSELOR_URL",
                      "http://127.0.0.1:5010"),
        endpoint_router_options());
    return router;
}

mindbridge::net::ManagedEndpointRouter& evaluator_router() {
    static mindbridge::net::ManagedEndpointRouter router(
        "evaluator",
        "evaluator",
        endpoint_urls("MINDBRIDGE_EVALUATOR_URLS",
                      "MINDBRIDGE_EVALUATOR_URL",
                      "http://127.0.0.1:5011"),
        endpoint_router_options());
    return router;
}

std::string counselor_url_for(const std::string& question, const std::string& conversation_id) {
    return counselor_router().select(question, conversation_id);
}

std::string evaluator_url_for(const std::string& question, const std::string& conversation_id) {
    return evaluator_router().select(question, conversation_id);
}

bool should_call_evaluator(const std::string& intent, const std::string& risk_level) {
    if (mindbridge::RiskPolicy::is_high_risk(risk_level)) {
        return true;
    }
    const std::string medium_enabled =
        mindbridge::app::env_or("MINDBRIDGE_EVALUATOR_ON_MEDIUM", "false");
    return medium_enabled == "true" && intent == "CONSULT";
}

std::string extract_context_id_lenient(const json& request) {
    if (request.contains("params") && request["params"].is_object()) {
        const auto& params = request["params"];
        if (params.contains("contextId") && params["contextId"].is_string()) {
            return params["contextId"].get<std::string>();
        }
        if (params.contains("context_id") && params["context_id"].is_string()) {
            return params["context_id"].get<std::string>();
        }
        if (params.contains("message") && params["message"].is_object()) {
            const auto& message = params["message"];
            return message.value("contextId", message.value("context_id", "default"));
        }
    }
    return "default";
}

void append_transcript(const std::string& conversation_id,
                       const std::string& user_text,
                       const std::string& assistant_text) {
    std::lock_guard<std::mutex> lock(transcript_mutex);
    auto& turns = transcripts[conversation_id.empty() ? "default" : conversation_id];
    turns.push_back({"user", user_text});
    turns.push_back({"assistant", assistant_text});
    while (turns.size() > kMaxTranscriptMessages) {
        turns.erase(turns.begin());
    }
}

std::string transcript_text(const std::string& conversation_id) {
    std::lock_guard<std::mutex> lock(transcript_mutex);
    const auto it = transcripts.find(conversation_id.empty() ? "default" : conversation_id);
    if (it == transcripts.end()) {
        return "";
    }
    std::ostringstream out;
    for (const auto& turn : it->second) {
        out << turn.role << ": " << turn.content << "\n";
    }
    return out.str();
}

void clear_transcript(const std::string& conversation_id) {
    std::lock_guard<std::mutex> lock(transcript_mutex);
    transcripts.erase(conversation_id.empty() ? "default" : conversation_id);
}

std::string extract_agent_text(const std::string& counselor_body) {
    try {
        const json response = json::parse(counselor_body);
        const auto& parts = response.at("result").at("message").at("parts");
        if (parts.is_array() && !parts.empty()) {
            return parts[0].value("text", "");
        }
    } catch (...) {
    }
    return "";
}

void prepend_text_part(json& request, const std::string& text) {
    if (text.empty()) {
        return;
    }
    auto& parts = request["params"]["message"]["parts"];
    if (!parts.is_array()) {
        parts = json::array();
    }
    parts.insert(parts.begin(), json{{"kind", "text"}, {"text", text}});
}

json try_call_evaluator(const json& request,
                        const std::string& question,
                        const std::string& conversation_id) {
    json outcome = {{"called", false}};
    try {
        const std::string url = evaluator_url_for(question, conversation_id);
        const std::string raw = evaluator_router().post_json(question, conversation_id, request.dump(), false, 120L);
        outcome["called"] = true;
        outcome["url"] = url;
        outcome["response"] = json::parse(raw);
    } catch (const std::exception& e) {
        outcome["called"] = true;
        outcome["error"] = e.what();
    }
    return outcome;
}

json try_analyze_multimodal(const json& request, const std::string& id) {
    json outcome = {{"disabled", true}, {"risk_level", "low"}, {"final_emotion", "normal"}};
    if (!mindbridge::app::has_non_text_parts(request)) {
        return outcome;
    }
    try {
        const json arguments = mindbridge::app::extract_multimodal_arguments_from_jsonrpc(request);
        auto& tools = multimodal_tools();
        const auto validation = tools.validate("analyze_multimodal_emotion", arguments);
        if (validation.has_value()) {
            outcome["error"] = *validation;
            return outcome;
        }
        mindbridge::ToolContext tool_ctx{{{"request_id", id}, {"owner", "mindbridge_orchestrator"}}};
        const auto result = tools.execute("analyze_multimodal_emotion", arguments, tool_ctx);
        outcome = result.to_json();
        if (outcome.contains("data") && outcome["data"].is_object()) {
            for (const auto& item : outcome["data"].items()) {
                outcome[item.key()] = item.value();
            }
        }
    } catch (const std::exception& e) {
        outcome["disabled"] = false;
        outcome["error"] = e.what();
    }
    return outcome;
}

std::string merge_orchestration(const std::string& counselor_body, const json& orchestration) {
    json response = json::parse(counselor_body);
    if (response.contains("result") && response["result"].is_object()) {
        response["result"]["orchestration"] = orchestration;
    } else {
        response["orchestration"] = orchestration;
    }
    return response.dump();
}

std::string handle_orchestrate(const std::string& body) {
    try {
        auto request = json::parse(body);
        const std::string method = request.value("method", "message/send");
        const std::string id = mindbridge::app::request_id_from(request);
        if (method == "session/end") {
            const std::string conversation_id = extract_context_id_lenient(request);
            const std::string history = transcript_text(conversation_id);
            if (history.empty()) {
                return json{{"jsonrpc", "2.0"},
                            {"id", id},
                            {"result",
                             {{"ended", false},
                              {"conversation_id", conversation_id},
                              {"message", "no conversation history to evaluate"},
                              {"evaluator", {{"called", false}}},
                              {"email", {{"dispatched", false}, {"reason", "no conversation history"}}}}}}
                    .dump();
            }

            json evaluator_request = {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"method", "message/send"},
                {"params",
                 {{"message",
                   {{"role", "user"},
                    {"contextId", conversation_id},
                    {"parts", json::array({{{"kind", "text"}, {"text", history}}})}}},
                  {"historyLength", static_cast<int>(kMaxTranscriptMessages)}}},
                {"metadata",
                 {{"request_id", id},
                  {"orchestrator", "mindbridge_orchestrator"},
                  {"evaluator_mode", "session_summary"},
                  {"conversation_id", conversation_id},
                  {"conversation_history", history}}},
            };
            const json evaluator = try_call_evaluator(evaluator_request, history, conversation_id);
            clear_transcript(conversation_id);
            json result = {
                {"ended", true},
                {"conversation_id", conversation_id},
                {"evaluator", evaluator},
                {"email", evaluator.value("response", json::object())
                              .value("result", json::object())
                              .value("email", json::object({{"dispatched", false}}))},
            };
            return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}.dump();
        }

        const std::string text = mindbridge::app::extract_text_from_jsonrpc(request);
        const std::string conversation_id = extract_context_id_lenient(request);
        json multimodal = try_analyze_multimodal(request, id);
        const std::string transcript = multimodal.value("transcript", "");
        std::string effective_text = text;
        if (effective_text.empty()) {
            effective_text = transcript.empty() ? "（语音输入未识别出文字）" : transcript;
            prepend_text_part(request, effective_text);
        }
        const std::string intent = mindbridge::RiskPolicy::heuristic_intent(effective_text);
        std::string risk_level = mindbridge::RiskPolicy::heuristic_risk_level(effective_text);
        const std::string multimodal_risk = multimodal.value("risk_level", "low");
        if (mindbridge::RiskPolicy::is_high_risk(multimodal_risk)) {
            risk_level = "high";
        } else if (risk_level == "low" && multimodal_risk == "medium") {
            risk_level = "medium";
        }
        request["metadata"]["request_id"] = id;
        request["metadata"]["orchestrator"] = "mindbridge_orchestrator";
        request["metadata"]["preliminary_intent"] = intent;
        request["metadata"]["preliminary_risk_level"] = risk_level;
        request["metadata"]["multimodal_emotion"] = multimodal;

        json orchestration = {
            {"preliminary_intent", intent},
            {"preliminary_risk_level", risk_level},
            {"effective_user_text", effective_text},
            {"multimodal_emotion", multimodal},
            {"evaluator", {{"called", false}}},
        };

        if (should_call_evaluator(intent, risk_level)) {
            request["metadata"]["evaluator_alert_owner"] = true;
            orchestration["evaluator"] = try_call_evaluator(request, effective_text, conversation_id);
        }

        const std::string counselor_url = counselor_url_for(effective_text, conversation_id);
        orchestration["counselor"] = {{"url", counselor_url}};
        const std::string counselor_body =
            counselor_router().post_json(effective_text, conversation_id, request.dump(), true, 120L);
        const std::string assistant_text = extract_agent_text(counselor_body);
        if (!assistant_text.empty()) {
            append_transcript(conversation_id, effective_text, assistant_text);
        }
        return merge_orchestration(counselor_body, orchestration);
    } catch (const std::exception& e) {
        return mindbridge::app::json_error("unknown", -32000, e.what());
    }
}

void handle_orchestrate_stream(const std::string& body,
                               const std::function<bool(const std::string&)>& write_event) {
    auto emit = [&](const json& event) {
        return !write_event || write_event(event.dump());
    };
    try {
        auto request = json::parse(body);
        const std::string id = mindbridge::app::request_id_from(request);
        const std::string text = mindbridge::app::extract_text_from_jsonrpc(request);
        const std::string conversation_id = extract_context_id_lenient(request);
        json multimodal = try_analyze_multimodal(request, id);
        const std::string transcript = multimodal.value("transcript", "");
        std::string effective_text = text;
        if (effective_text.empty()) {
            effective_text = transcript.empty() ? "（语音输入未识别出文字）" : transcript;
            prepend_text_part(request, effective_text);
        }
        const std::string intent = mindbridge::RiskPolicy::heuristic_intent(effective_text);
        std::string risk_level = mindbridge::RiskPolicy::heuristic_risk_level(effective_text);
        const std::string multimodal_risk = multimodal.value("risk_level", "low");
        if (mindbridge::RiskPolicy::is_high_risk(multimodal_risk)) {
            risk_level = "high";
        } else if (risk_level == "low" && multimodal_risk == "medium") {
            risk_level = "medium";
        }

        request["method"] = "message/stream";
        request["metadata"]["request_id"] = id;
        request["metadata"]["orchestrator"] = "mindbridge_orchestrator";
        request["metadata"]["preliminary_intent"] = intent;
        request["metadata"]["preliminary_risk_level"] = risk_level;
        request["metadata"]["multimodal_emotion"] = multimodal;

        json orchestration = {
            {"event", "orchestration"},
            {"preliminary_intent", intent},
            {"preliminary_risk_level", risk_level},
            {"effective_user_text", effective_text},
            {"multimodal_emotion", multimodal},
            {"evaluator", {{"called", false}}},
        };

        if (should_call_evaluator(intent, risk_level)) {
            request["metadata"]["evaluator_alert_owner"] = true;
            orchestration["evaluator"] = try_call_evaluator(request, effective_text, conversation_id);
        }
        const std::string counselor_url = counselor_url_for(effective_text, conversation_id);
        orchestration["counselor"] = {{"url", counselor_url}};
        emit(orchestration);

        std::string assistant_text;
        counselor_router().post_sse(effective_text, conversation_id, request.dump(), [&](const std::string& payload) {
            try {
                const auto event = json::parse(payload);
                if (event.value("event", "") == "message_done") {
                    assistant_text = event.value("text", assistant_text);
                } else if (event.value("event", "") == "final_result") {
                    assistant_text = extract_agent_text(event.dump());
                }
            } catch (...) {
            }
            return !write_event || write_event(payload);
        }, 120L);
        if (!assistant_text.empty()) {
            append_transcript(conversation_id, effective_text, assistant_text);
        }
    } catch (const std::exception& e) {
        emit({{"event", "error"}, {"message", e.what()}});
    }
}

}  // namespace

int main(int argc, char** argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const int port = (argc > 1) ? std::atoi(argv[1]) : 5009;
    mindbridge::net::AsyncHttpServer server(port, mindbridge::net::AsyncHttpServer::options_from_env());
    server.register_handler("/api/health", [](const std::string&) {
        return json{{"ok", true},
                    {"service", "mindbridge_orchestrator"},
                    {"network",
                     {{"backend", "asio_beast_client"},
                      {"counselors", counselor_router().health_json()},
                      {"evaluators", evaluator_router().health_json()}}},
                    {"counselors", counselor_router().urls()},
                    {"evaluators", evaluator_router().urls()}}
            .dump();
    });
    server.register_handler("/", [](const std::string& body) { return handle_orchestrate(body); });
    server.register_stream_handler("/", [](const std::string& body,
                                           std::function<bool(const std::string&)> write_event) {
        handle_orchestrate_stream(body, write_event);
    });
    std::cout << "[mindbridge_orchestrator] port=" << port
              << " counselors=" << json(counselor_router().urls()).dump()
              << " evaluators=" << json(evaluator_router().urls()).dump() << std::endl;
    server.start();
    curl_global_cleanup();
    return 0;
}
