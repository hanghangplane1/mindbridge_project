#include "chroma_client.hpp"
#include "emotion_types.hpp"
#include "http_server.hpp"
#include "ollama_client.hpp"
#include "redis_task_store.hpp"
#include "registry_client.hpp"

#include "mindbridge/routing/registry_agent_router.hpp"

#include <a2a/core/error_code.hpp>
#include <a2a/core/jsonrpc_request.hpp>
#include <a2a/core/jsonrpc_response.hpp>
#include <a2a/models/agent_message.hpp>
#include <a2a/models/agent_task.hpp>
#include <a2a/models/message_part.hpp>
#include <a2a/models/task_status.hpp>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
using namespace a2a;

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
    if (!text_part) {
        return "";
    }
    return text_part->text();
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

class CounselorAgent {
public:
    CounselorAgent(const std::string& agent_id,
                   const std::string& listen_address,
                   const std::string& registry_url,
                   const std::string& redis_host,
                   int redis_port,
                   const std::string& ollama_url,
                   const std::string& model,
                   const std::string& chroma_url,
                   const std::string& chroma_collection)
        : agent_id_(agent_id),
          listen_address_(listen_address),
          task_store_(std::make_shared<RedisTaskStore>(redis_host, redis_port)),
          registry_client_(registry_url),
          registry_router_(registry_client_),
          ollama_(ollama_url, model),
          chroma_(chroma_url, chroma_collection) {
        registry_router_.set_strategy(agent_rpc::orchestrator::RoutingStrategy::CONSISTENT_HASH);
    }

    void start(int port) {
        HttpServer server(port);
        server.register_handler("/", [this](const std::string& body) { return handle_request(body); });
        server.register_stream_handler(
            "/", [this](const std::string& body, std::function<bool(const std::string&)> write_callback) {
                handle_stream_request(body, write_callback);
            });
        server.register_handler("/.well-known/agent-card.json", [this](const std::string&) { return get_agent_card(); });
        server.register_handler("/a2a/push", [this](const std::string& body) { return handle_push(body); });

        std::thread server_thread([&server]() { server.start(); });
        std::this_thread::sleep_for(std::chrono::seconds(1));

        AgentRegistration registration;
        registration.id = agent_id_;
        registration.name = "PsychoCounselor";
        registration.address = listen_address_;
        registration.tags = {"counselor", "mindcare"};
        registry_client_.register_agent(registration);

        std::cout << "[Counselor] listening on " << listen_address_ << std::endl;
        server_thread.join();
    }

private:
    std::string handle_request(const std::string& body) {
        try {
            auto request_json = json::parse(body);
            auto request = JsonRpcRequest::from_json(body);
            if (request.method() == "tasks/get") {
                const std::string task_id = request_json["params"].value("id", "default");
                auto task = task_store_->get_task(task_id);
                if (!task.has_value()) {
                    return JsonRpcResponse::create_error(request.id(), ErrorCode::TaskNotFound, "Task not found").to_json();
                }
                return JsonRpcResponse::create_success(request.id(), task->to_json()).to_json();
            }
            if (request.method() != "message/send") {
                return JsonRpcResponse::create_error(request.id(), ErrorCode::MethodNotFound, "Method not found").to_json();
            }

            auto params_json = request_json["params"];
            auto message = AgentMessage::from_json(params_json["message"].dump());
            const std::string context_id = message.context_id().value_or("default");
            const std::string user_text = extract_text_from_message(message);

            save_message(context_id, message);
            const std::string reply_text = build_counselor_reply(context_id, user_text);
            maybe_trigger_evaluator_async(context_id, user_text);

            auto response_msg = AgentMessage::create().with_role(MessageRole::Agent).with_context_id(context_id);
            response_msg.add_text_part(reply_text);
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
            if (request.method() != "message/stream") {
                write_callback("{\"error\":\"Method not found for streaming\"}");
                return;
            }

            auto params_json = request_json["params"];
            auto message = AgentMessage::from_json(params_json["message"].dump());
            const std::string context_id = message.context_id().value_or("default");
            const std::string user_text = extract_text_from_message(message);
            save_message(context_id, message);

            json start_event = {
                {"jsonrpc", "2.0"},
                {"id", request.id()},
                {"result", {{"type", "stream_start"}, {"contextId", context_id}}},
            };
            write_callback(start_event.dump());

            const std::string system_prompt = build_system_prompt(context_id, user_text);
            std::string aggregated;
            ollama_.chat_stream(system_prompt, user_text, [&](const std::string& chunk) {
                aggregated += chunk;
                json event = {
                    {"jsonrpc", "2.0"},
                    {"id", request.id()},
                    {"result", {{"type", "chunk"}, {"content", chunk}}},
                };
                return write_callback(event.dump());
            });

            auto response_msg = AgentMessage::create().with_role(MessageRole::Agent).with_context_id(context_id);
            response_msg.add_text_part(aggregated);
            save_message(context_id, response_msg);
            maybe_trigger_evaluator_async(context_id, user_text);

            json end_event = {
                {"jsonrpc", "2.0"},
                {"id", request.id()},
                {"result", {{"type", "stream_end"}, {"message", json::parse(response_msg.to_json())}}},
            };
            write_callback(end_event.dump());
        } catch (const std::exception& e) {
            write_callback(std::string("{\"error\":\"") + e.what() + "\"}");
        }
    }

    std::string handle_push(const std::string& body) {
        try {
            auto payload = json::parse(body);
            std::string context_id = payload.value("contextId", "default");
            std::string message_text = payload.value("message", "risk alert");

            auto msg = AgentMessage::create().with_role(MessageRole::Agent).with_context_id(context_id);
            msg.add_text_part("[EvaluatorPush] " + message_text);
            save_message(context_id, msg);
            return json({{"success", true}}).dump();
        } catch (...) {
            return json({{"success", false}}).dump();
        }
    }

    std::string build_system_prompt(const std::string& context_id, const std::string& user_text) {
        std::vector<std::string> docs;
        try {
            docs = chroma_.query(ollama_, user_text, 3);
        } catch (...) {
            // Chroma is optional for local bring-up.
        }

        std::string history = build_history_text(context_id, 5);
        std::string context;
        for (const auto& d : docs) {
            context += d + "\n";
        }

        std::string prompt =
            "You are a licensed psychological counselor. Respond in Chinese with warmth and structure.\n"
            "When appropriate, keep strategy tags such as <Approval and Reassurance> or <Interpretation>.\n";
        if (!context.empty()) {
            prompt += "\nKnowledge:\n" + context;
        }
        if (!history.empty()) {
            prompt += "\nRecentHistory:\n" + history;
        }
        return prompt;
    }

    std::string build_counselor_reply(const std::string& context_id, const std::string& user_text) {
        const std::string system_prompt = build_system_prompt(context_id, user_text);
        return ollama_.chat(system_prompt, user_text);
    }

    bool should_run_evaluator(const std::string& user_text) {
        const std::string prompt =
            "Classify user text into CHAT/CONSULT/RISK. Return JSON only: {\"intent\":\"CHAT|CONSULT|RISK\"}.";
        try {
            std::string out = ollama_.chat(prompt, user_text);
            auto j = json::parse(extract_json_from_text(out));
            std::string intent = j.value("intent", "CHAT");
            return intent == "RISK";
        } catch (...) {
            static const std::vector<std::string> hard_risk = {
                "不想活", "活着没意义", "自杀", "自残", "结束生命"};
            for (const auto& k : hard_risk) {
                if (user_text.find(k) != std::string::npos) {
                    return true;
                }
            }
            return false;
        }
    }

    std::string build_history_text(const std::string& context_id, int max_messages) {
        std::string out;
        auto history = task_store_->get_history(context_id, max_messages);
        for (const auto& msg : history) {
            std::string role = msg.role() == MessageRole::User ? "user" : "assistant";
            out += role + ": " + extract_text_from_message(msg) + "\n";
        }
        return out;
    }

    void maybe_trigger_evaluator_async(const std::string& context_id, const std::string& user_text) {
        if (!should_run_evaluator(user_text)) {
            return;
        }
        std::thread([this, context_id, user_text]() {
            try {
                const std::string evaluator_url =
                    registry_router_.select_agent_url(user_text, "evaluator", context_id);
                const std::string history_text = build_history_text(context_id, 5);
                json request = {
                    {"jsonrpc", "2.0"},
                    {"id", "mindcare-eval-" + context_id},
                    {"method", "message/send"},
                    {"params",
                     {
                         {"message",
                          {
                              {"role", "user"},
                              {"contextId", context_id},
                              {"parts", {{{"kind", "text"}, {"text", history_text}}}},
                          }},
                         {"pushUrl", listen_address_ + "/a2a/push"},
                     }},
                };
                SimpleHttpClient::post(evaluator_url, request.dump(), 30L);
            } catch (const std::exception& e) {
                std::cerr << "[Counselor] evaluator call failed: " << e.what() << std::endl;
            }
        }).detach();
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
            {"name", "PsychoCounselor"},
            {"description", "Counseling agent with LoRA-style response strategy and RAG support"},
            {"version", "1.0.0"},
            {"capabilities", {{"streaming", true}, {"push_notifications", true}, {"task_management", true}}},
            {"skills",
             json::array({
                 {{"name", "心理咨询"}, {"description", "情绪支持和心理建议"}, {"input_modes", {"text"}}, {"output_modes", {"text"}}},
                 {{"name", "知识检索"}, {"description", "基于心理知识库的检索增强回答"}, {"input_modes", {"text"}}, {"output_modes", {"text"}}},
             })},
        };
        return card.dump();
    }

    std::string agent_id_;
    std::string listen_address_;
    std::shared_ptr<RedisTaskStore> task_store_;
    RegistryClient registry_client_;
    mindbridge::RegistryAgentRouter registry_router_;
    OllamaClient ollama_;
    ChromaClient chroma_;
};

int main(int argc, char* argv[]) {
    std::string agent_id = "counselor-1";
    int port = 5000;
    std::string registry_url = "http://localhost:8500";
    std::string redis_host = "127.0.0.1";
    int redis_port = 6379;
    std::string ollama_url = "http://localhost:11434";
    std::string model = "mindcare-counselor:latest";
    std::string chroma_url = "http://localhost:8000";
    std::string chroma_collection = "psyqa_knowledge";

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
        else if (k == "--chroma") chroma_url = v;
        else if (k == "--chroma-collection") chroma_collection = v;
    }

    const std::string listen_address = "http://localhost:" + std::to_string(port);
    CounselorAgent agent(agent_id, listen_address, registry_url, redis_host, redis_port, ollama_url, model, chroma_url,
                         chroma_collection);
    agent.start(port);
    return 0;
}
