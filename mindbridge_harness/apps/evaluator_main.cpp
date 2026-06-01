#include "mindbridge/apps/app_support.hpp"
#include "mindbridge/harness/agent_loop.hpp"
#include "mindbridge/harness/hook_executor.hpp"
#include "mindbridge/harness/permission_checker.hpp"
#include "mindbridge/harness/tool_registry.hpp"
#include "mindbridge/harness/trace_recorder.hpp"
#include "mindbridge/net/async_http_server.hpp"
#include "mindbridge/runtime/conversation_memory.hpp"
#include "mindbridge/runtime/prompt_policy.hpp"
#include "mindbridge/tools/core_tools.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

using json = nlohmann::json;

namespace {

json parse_model_json_or_fallback(const std::string& raw,
                                  const std::string& fallback_text,
                                  mindbridge::AgentLoop& loop,
                                  mindbridge::ToolContext& tool_ctx) {
    try {
        const auto begin = raw.find('{');
        const auto end = raw.rfind('}');
        if (begin != std::string::npos && end != std::string::npos && begin < end) {
            return json::parse(raw.substr(begin, end - begin + 1));
        }
        return json::parse(raw);
    } catch (...) {
        return loop.invoke_tool("assess_risk", {{"text", fallback_text}}, tool_ctx);
    }
}

std::string join_json_array(const json& values) {
    if (!values.is_array()) {
        return "";
    }
    std::ostringstream out;
    bool first = true;
    for (const auto& value : values) {
        if (!first) {
            out << "; ";
        }
        first = false;
        out << (value.is_string() ? value.get<std::string>() : value.dump());
    }
    return out.str();
}

std::string session_email_body(const std::string& conversation_id,
                               const std::string& conversation_history,
                               const json& assessment) {
    std::ostringstream out;
    out << "MindBridge session assessment\n";
    out << "conversation_id: " << conversation_id << "\n";
    out << "risk_level: " << assessment.value("riskLevel", "low") << "\n";
    out << "emotion_tags: " << join_json_array(assessment.value("emotionTags", json::array())) << "\n";
    out << "\nsummary:\n" << assessment.value("summary", assessment.value("suggestion", "")) << "\n";
    out << "\nconcerns:\n" << join_json_array(assessment.value("concerns", json::array())) << "\n";
    out << "\nprotective_factors:\n" << join_json_array(assessment.value("protectiveFactors", json::array())) << "\n";
    out << "\nsuggested_follow_up:\n" << assessment.value("suggestedFollowUp", assessment.value("suggestion", "")) << "\n";
    out << "\nrecent_conversation:\n" << conversation_history << "\n";
    return out.str();
}

json safe_email_dispatch(mindbridge::AgentLoop& loop,
                         const json& arguments,
                         mindbridge::ToolContext& tool_ctx) {
    try {
        return loop.invoke_tool("mcp_email_alert", arguments, tool_ctx);
    } catch (const std::exception& e) {
        return {{"ok", false}, {"dispatched", false}, {"error", e.what()}};
    }
}

class EvaluatorService {
public:
    EvaluatorService()
        : model_(mindbridge::app::create_model_client_from_env()),
          permission_(mindbridge::PermissionChecker::Mode::FullAuto),
          trace_(mindbridge::app::env_or("MINDBRIDGE_TRACE_PATH", "/tmp/mindbridge_evaluator_trace.jsonl")),
          loop_(*model_, tools_, permission_, hooks_, &trace_) {
        mindbridge::register_core_tools(tools_, memory_, nullptr, model_.get());
    }

    std::string handle(const std::string& body) {
        try {
            const auto request = json::parse(body);
            const std::string id = mindbridge::app::request_id_from(request);
            const std::string text = mindbridge::app::extract_text_from_jsonrpc(request);
            const json metadata = request.value("metadata", json::object());
            const std::string mode = metadata.value("evaluator_mode", "risk_check");
            const std::string conversation_id = metadata.value("conversation_id", id);
            const std::string conversation_history = metadata.value("conversation_history", text);
            const json multimodal = metadata.value("multimodal_emotion", json::object());
            const std::string multimodal_context = mindbridge::app::multimodal_context_text(multimodal);
            mindbridge::ToolContext tool_ctx{{{"request_id", id},
                                              {"evaluator_mode", mode},
                                              {"multimodal_emotion", multimodal}}};
            mindbridge::AgentLoop::RunInput run_input;
            run_input.task_id = mode == "session_summary" ? "evaluator_session_summary" : "evaluator_request";
            run_input.session_id = conversation_id;
            run_input.conversation_id = conversation_id;
            run_input.system_rules = mode == "session_summary"
                                         ? mindbridge::PromptPolicy::session_evaluator_prompt()
                                         : mindbridge::PromptPolicy::evaluator_prompt();
            run_input.user_message = mode == "session_summary" ? conversation_history : text;
            if (!multimodal_context.empty()) {
                run_input.user_message += "\n\n" + multimodal_context;
            }
            run_input.tools_surface = tools_.tool_names();
            run_input.memory_json = memory_.to_json();
            const auto run_result = loop_.run(run_input);
            std::string raw = run_result.final_answer;
            json result = parse_model_json_or_fallback(
                raw, mode == "session_summary" ? conversation_history : text, loop_, tool_ctx);
            result["mode"] = mode;

            if (mode == "session_summary") {
                if (!result.contains("summary")) {
                    result["summary"] = result.value("suggestion", "No structured summary was returned.");
                }
                if (!result.contains("concerns")) {
                    result["concerns"] = json::array();
                }
                if (!result.contains("protectiveFactors")) {
                    result["protectiveFactors"] = json::array();
                }
                if (!result.contains("suggestedFollowUp")) {
                    result["suggestedFollowUp"] = result.value("suggestion", "请继续关注用户状态，必要时建议联系可信支持资源。");
                }
                const std::string risk_level = result.value("riskLevel", "low");
                const json email = safe_email_dispatch(
                    loop_,
                    {{"assessment", result},
                     {"risk_level", risk_level},
                     {"conversation_id", conversation_id},
                     {"subject", "MindBridge session assessment: " + risk_level},
                     {"body", session_email_body(conversation_id, conversation_history, result)},
                     {"message", conversation_history}},
                    tool_ctx);
                result["email"] = email;
            } else if (result.value("riskLevel", "low") == "high") {
                const json email = safe_email_dispatch(
                    loop_,
                    {{"risk", result},
                     {"risk_level", "high"},
                     {"conversation_id", conversation_id},
                     {"message", text}},
                    tool_ctx);
                result["email"] = email;
            }
            return json{{"jsonrpc", "2.0"},
                        {"id", id},
                        {"result", result},
                        {"run_id", run_result.run_id},
                        {"run_dir", run_result.run_dir},
                        {"prompt_metadata", run_result.prompt_metadata}}
                .dump();
        } catch (const std::exception& e) {
            return mindbridge::app::json_error("unknown", -32000, e.what());
        }
    }

private:
    std::unique_ptr<mindbridge::ModelClient> model_;
    mindbridge::ConversationMemory memory_;
    mindbridge::ToolRegistry tools_;
    mindbridge::PermissionChecker permission_;
    mindbridge::HookExecutor hooks_;
    mindbridge::TraceRecorder trace_;
    mindbridge::AgentLoop loop_;
};

}  // namespace

int main(int argc, char** argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const int port = (argc > 1) ? std::atoi(argv[1]) : 5011;
    EvaluatorService service;
    mindbridge::net::AsyncHttpServer server(port, mindbridge::net::AsyncHttpServer::options_from_env());
    server.register_handler("/api/health", [](const std::string&) {
        return R"({"ok":true,"service":"mindbridge_evaluator"})";
    });
    server.register_handler("/", [&service](const std::string& body) { return service.handle(body); });
    std::cout << "[mindbridge_evaluator] port=" << port << std::endl;
    server.start();
    curl_global_cleanup();
    return 0;
}
