#include "emotion_types.hpp"
#include "http_server.hpp"
#include "ollama_client.hpp"
#include "redis_task_store.hpp"
#include "registry_client.hpp"

#include <a2a/core/error_code.hpp>
#include <a2a/core/jsonrpc_request.hpp>
#include <a2a/core/jsonrpc_response.hpp>
#include <a2a/models/agent_message.hpp>
#include <a2a/models/agent_task.hpp>
#include <a2a/models/message_part.hpp>
#include <a2a/models/task_status.hpp>

#include <agent_rpc/mcp/mcp_agent_integration.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace a2a;
using json = nlohmann::json;
using namespace agent_rpc::mcp;

namespace {

class SimpleHttpClient {
public:
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append(static_cast<char*>(contents), size * nmemb);
        return size * nmemb;
    }

    static std::string post(const std::string& url, const std::string& body, long timeout_sec = 30L) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("curl_easy_init failed");
        }
        char error_buffer[CURL_ERROR_SIZE] = {0};
        std::string response;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
        CURLcode rc = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (rc != CURLE_OK) {
            const std::string detail = error_buffer[0] ? error_buffer : curl_easy_strerror(rc);
            throw std::runtime_error("POST " + url + " failed: " + detail);
        }
        if (http_code >= 400) {
            throw std::runtime_error("POST " + url + " returned HTTP " + std::to_string(http_code) + ": " + response);
        }
        return response;
    }
};

std::string extract_text_from_message(const AgentMessage& message) {
    if (message.parts().empty()) {
        return "";
    }
    auto* text_part = dynamic_cast<TextPart*>(message.parts()[0].get());
    return text_part ? text_part->text() : "";
}

std::string extract_json_from_text(const std::string& text) {
    const auto begin = text.find('{');
    const auto end = text.rfind('}');
    if (begin == std::string::npos || end == std::string::npos || begin >= end) {
        return "{}";
    }
    return text.substr(begin, end - begin + 1);
}

}  // namespace

class EvaluatorAgent {
public:
    EvaluatorAgent(const std::string& agent_id,
                   const std::string& listen_address,
                   const std::string& registry_url,
                   const std::string& redis_host,
                   int redis_port,
                   const std::string& ollama_url,
                   const std::string& model,
                   const std::string& mcp_server_path,
                   const std::string& mcp_plugins_dir)
        : agent_id_(agent_id),
          listen_address_(listen_address),
          task_store_(std::make_shared<RedisTaskStore>(redis_host, redis_port)),
          registry_client_(registry_url),
          ollama_(ollama_url, model) {
        if (!mcp_server_path.empty()) {
            MCPAgentConfig cfg;
            cfg.enable_mcp = true;
            cfg.mcp_server_path = mcp_server_path;
            cfg.mcp_args = {"--plugins", mcp_plugins_dir};
            if (!mcp_integration_.initialize(cfg)) {
                std::cerr << "[Evaluator] MCP init failed, continue without MCP." << std::endl;
            }
        }
    }

    ~EvaluatorAgent() { mcp_integration_.shutdown(); }

    void start(int port) {
        HttpServer server(port);
        server.register_handler("/", [this](const std::string& body) { return handle_request(body); });
        server.register_stream_handler(
            "/", [this](const std::string& body, std::function<bool(const std::string&)> write_callback) {
                handle_stream_request(body, write_callback);
            });
        server.register_handler("/.well-known/agent-card.json", [this](const std::string&) { return get_agent_card(); });

        std::thread server_thread([&server]() { server.start(); });
        std::this_thread::sleep_for(std::chrono::seconds(1));

        AgentRegistration registration;
        registration.id = agent_id_;
        registration.name = "PsychoEvaluator";
        registration.address = listen_address_;
        registration.tags = {"evaluator", "mindcare"};
        registry_client_.register_agent(registration);

        std::cout << "[Evaluator] listening on " << listen_address_ << std::endl;
        server_thread.join();
    }

private:
    std::string handle_request(const std::string& body) {
        try {
            auto request_json = json::parse(body);
            auto request = JsonRpcRequest::from_json(body);
            if (request.method() != "message/send") {
                return JsonRpcResponse::create_error(request.id(), ErrorCode::MethodNotFound, "Method not found").to_json();
            }

            auto params_json = request_json["params"];
            auto message = AgentMessage::from_json(params_json["message"].dump());
            const std::string context_id = message.context_id().value_or("default");
            const std::string content = extract_text_from_message(message);
            const std::string push_url = params_json.value("pushUrl", "");

            save_message(context_id, message);
            const mindcare::EmotionResult result = evaluate(content);
            post_actions(context_id, content, result, push_url);

            auto response_msg = AgentMessage::create().with_role(MessageRole::Agent).with_context_id(context_id);
            response_msg.add_text_part(mindcare::emotion_to_json(result).dump());
            save_message(context_id, response_msg);

            return JsonRpcResponse::create_success(request.id(), response_msg.to_json()).to_json();
        } catch (const std::exception& e) {
            return JsonRpcResponse::create_error("1", ErrorCode::InternalError, e.what()).to_json();
        }
    }

    void handle_stream_request(const std::string& body,
                               const std::function<bool(const std::string&)>& write_callback) {
        try {
            auto request_json = json::parse(body);
            auto request = JsonRpcRequest::from_json(body);
            auto params_json = request_json["params"];
            auto message = AgentMessage::from_json(params_json["message"].dump());
            const std::string context_id = message.context_id().value_or("default");
            const std::string content = extract_text_from_message(message);
            const std::string push_url = params_json.value("pushUrl", "");

            json start = {
                {"jsonrpc", "2.0"},
                {"id", request.id()},
                {"result", {{"type", "stream_start"}, {"contextId", context_id}}},
            };
            write_callback(start.dump());

            write_callback(json({{"jsonrpc", "2.0"}, {"id", request.id()}, {"result", {{"type", "chunk"}, {"content", "evaluating..."}}}}).dump());
            const mindcare::EmotionResult result = evaluate(content);
            post_actions(context_id, content, result, push_url);

            auto response_msg = AgentMessage::create().with_role(MessageRole::Agent).with_context_id(context_id);
            response_msg.add_text_part(mindcare::emotion_to_json(result).dump());
            save_message(context_id, response_msg);

            json end = {
                {"jsonrpc", "2.0"},
                {"id", request.id()},
                {"result", {{"type", "stream_end"}, {"message", json::parse(response_msg.to_json())}}},
            };
            write_callback(end.dump());
        } catch (const std::exception& e) {
            write_callback(std::string("{\"error\":\"") + e.what() + "\"}");
        }
    }

    mindcare::EmotionResult evaluate(const std::string& conversation_text) {
        const std::string prompt =
            "You are a psychological evaluator.\n"
            "Return JSON only:\n"
            "{\"riskLevel\":\"low|medium|high\",\"emotionTags\":[\"normal|anxious|depressed|pressure|despair|anger\"],"
            "\"confidence\":0.0,\"suggestion\":\"...\"}\n"
            "Risk rule: high if explicit self-harm or suicide intent.\n"
            "Conversation:\n" +
            conversation_text;

        try {
            const std::string raw = ollama_.chat("", prompt);
            const auto parsed = json::parse(extract_json_from_text(raw));
            return mindcare::emotion_from_json(parsed);
        } catch (...) {
            mindcare::EmotionResult fallback;
            fallback.risk_level = "medium";
            fallback.emotion_tags = {"pressure"};
            fallback.confidence = 0.5;
            fallback.suggestion = "Need manual follow-up.";
            return fallback;
        }
    }

    void post_actions(const std::string& context_id,
                      const std::string& content,
                      const mindcare::EmotionResult& result,
                      const std::string& push_url) {
        const bool high = (result.risk_level == "high");
        write_record_mcp(context_id, content, result);
        if (high) {
            send_email_mcp(context_id, result);
            if (!push_url.empty()) {
                json payload = {
                    {"contextId", context_id},
                    {"riskLevel", result.risk_level},
                    {"message", "High risk detected: " + result.suggestion},
                };
                SimpleHttpClient::post(push_url, payload.dump(), 10L);
            }
        }
    }

    void write_record_mcp(const std::string& context_id, const std::string& content, const mindcare::EmotionResult& result) {
        if (!mcp_integration_.isAvailable()) {
            return;
        }
        json args = {
            {"userId", context_id},
            {"content", content},
            {"emotionLabel", result.emotion_tags.empty() ? "unknown" : result.emotion_tags[0]},
            {"riskLevel", result.risk_level},
            {"confidence", result.confidence},
            {"timestamp", static_cast<long long>(std::time(nullptr))},
        };
        mcp_integration_.callTool("write_record", args.dump());
    }

    void send_email_mcp(const std::string& context_id, const mindcare::EmotionResult& result) {
        if (!mcp_integration_.isAvailable()) {
            return;
        }
        json args = {
            {"to", "counselor-alert@example.com"},
            {"subject", "MindCare high risk alert"},
            {"body", "context=" + context_id + " risk=" + result.risk_level + " suggestion=" + result.suggestion},
        };
        mcp_integration_.callTool("send_alert_email", args.dump());
    }

    void save_message(const std::string& context_id, const AgentMessage& message) {
        if (!task_store_->task_exists(context_id)) {
            auto task = AgentTask::create().with_id(context_id).with_context_id(context_id).with_status(TaskState::Running);
            task_store_->set_task(task);
        }
        task_store_->add_history_message(context_id, message);
    }

    std::string get_agent_card() const {
        json card = {
            {"name", "PsychoEvaluator"},
            {"description", "Psychological risk evaluator with MCP alert tools"},
            {"version", "1.0.0"},
            {"capabilities", {{"streaming", true}, {"push_notifications", true}, {"task_management", true}}},
            {"skills",
             json::array({
                 {{"name", "risk-evaluation"}, {"description", "evaluate risk level from dialogue"}, {"input_modes", {"text"}}, {"output_modes", {"text"}}},
             })},
        };
        return card.dump();
    }

    std::string agent_id_;
    std::string listen_address_;
    std::shared_ptr<RedisTaskStore> task_store_;
    RegistryClient registry_client_;
    OllamaClient ollama_;
    MCPAgentIntegration mcp_integration_;
};

int main(int argc, char* argv[]) {
    std::string agent_id = "evaluator-1";
    int port = 5001;
    std::string registry_url = "http://localhost:8500";
    std::string redis_host = "127.0.0.1";
    int redis_port = 6379;
    std::string ollama_url = "http://localhost:11434";
    std::string model = "qwen2.5:7b-chat";
    std::string mcp_server = "";
    std::string mcp_plugins_dir = "";

    if (argc > 1) {
        port = std::stoi(argv[1]);
    }
    for (int i = 2; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        std::string v = argv[i + 1];
        if (k == "--agent-id") agent_id = v;
        else if (k == "--registry") registry_url = v;
        else if (k == "--redis") {
            auto pos = v.find(':');
            redis_host = v.substr(0, pos);
            redis_port = std::stoi(v.substr(pos + 1));
        } else if (k == "--ollama") ollama_url = v;
        else if (k == "--model") model = v;
        else if (k == "--mcp-server") mcp_server = v;
        else if (k == "--mcp-plugins") mcp_plugins_dir = v;
    }

    const std::string listen_address = "http://localhost:" + std::to_string(port);
    EvaluatorAgent agent(agent_id, listen_address, registry_url, redis_host, redis_port, ollama_url, model, mcp_server,
                         mcp_plugins_dir);
    agent.start(port);
    return 0;
}
