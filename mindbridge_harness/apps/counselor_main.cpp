#include "mindbridge/apps/app_support.hpp"
#include "mindbridge/harness/agent_loop.hpp"
#include "mindbridge/harness/hook_executor.hpp"
#include "mindbridge/harness/permission_checker.hpp"
#include "mindbridge/harness/tool_registry.hpp"
#include "mindbridge/harness/trace_recorder.hpp"
#include "mindbridge/net/async_http_server.hpp"
#include "mindbridge/rag/chroma_vector_store.hpp"
#include "mindbridge/runtime/conversation_memory.hpp"
#include "mindbridge/runtime/prompt_policy.hpp"
#include "mindbridge/harness/response_validator.hpp"
#include "mindbridge/runtime/risk_policy.hpp"
#include "mindbridge/runtime/runtime_context.hpp"
#include "mindbridge/state/distributed_state_store.hpp"
#include "mindbridge/tools/core_tools.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace {

json normalize_multimodal_metadata(json multimodal) {
    if (!multimodal.is_object()) {
        return json{{"disabled", true},
                    {"risk_level", "low"},
                    {"final_emotion", "normal"},
                    {"weighted_score", 0.0},
                    {"evidence_summary", "multimodal metadata was not an object"}};
    }
    if (multimodal.value("ok", true) == false || !multimodal.value("error_code", "").empty() ||
        multimodal.contains("error")) {
        return json{{"disabled", true},
                    {"risk_level", "low"},
                    {"final_emotion", "normal"},
                    {"weighted_score", 0.0},
                    {"evidence_summary", multimodal.value("summary", "multimodal analysis failed")},
                    {"error", multimodal.value("error", multimodal.value("error_code", ""))}};
    }
    return multimodal;
}

int env_int(const char* key, const int fallback) {
    const std::string value = mindbridge::app::env_or(key);
    if (value.empty()) {
        return fallback;
    }
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

bool env_flag(const char* key, const bool fallback) {
    const std::string value = mindbridge::app::env_or(key, fallback ? "true" : "false");
    return value != "false" && value != "0";
}

std::string state_master_db_path() {
    return mindbridge::app::env_or("MINDBRIDGE_STATE_DB_PATH",
                                   ".mindbridge/state/counselor_master.sqlite");
}

std::string state_master_node_id() {
    return mindbridge::app::env_or("MINDBRIDGE_STATE_NODE_ID", "counselor-master");
}

std::unique_ptr<mindbridge::state::DistributedStateStore> make_state_store() {
    const std::string enabled = mindbridge::app::env_or("MINDBRIDGE_STATE_STORE_ENABLED", "true");
    if (enabled == "false" || enabled == "0") {
        return nullptr;
    }
    auto store = std::make_unique<mindbridge::state::DistributedStateStore>(
        state_master_db_path(),
        state_master_node_id());
    store->initialize();
    std::cerr << "[mindbridge_counselor] distributed_state_store=" << state_master_db_path()
              << " node=" << store->node_id() << std::endl;
    return store;
}

mindbridge::ToolContext runtime_tool_context(const mindbridge::RuntimeContext& ctx) {
    return mindbridge::ToolContext{{{"runtime", ctx.to_json()}}};
}

class StateReplicaRuntime {
public:
    StateReplicaRuntime(mindbridge::state::DistributedStateStore* master,
                        std::mutex& state_mutex)
        : master_(master),
          state_mutex_(state_mutex),
          master_db_path_(state_master_db_path()),
          master_node_id_(master ? master->node_id() : state_master_node_id()),
          follower_db_path_(mindbridge::app::env_or(
              "MINDBRIDGE_STATE_FOLLOWER_DB_PATH",
              ".mindbridge/state/counselor_follower.sqlite")),
          follower_node_id_(mindbridge::app::env_or(
              "MINDBRIDGE_STATE_FOLLOWER_NODE_ID",
              "counselor-follower")),
          poll_ms_(env_int("MINDBRIDGE_STATE_REPLICA_POLL_MS", 1000)),
          batch_(env_int("MINDBRIDGE_STATE_REPLICA_BATCH", 1000)),
          enabled_(master_ != nullptr && env_flag("MINDBRIDGE_STATE_REPLICA_ENABLED", true)) {
        if (!enabled_) {
            return;
        }
        follower_ = std::make_unique<mindbridge::state::DistributedStateStore>(
            follower_db_path_, follower_node_id_);
        follower_->initialize();
        worker_ = std::make_unique<mindbridge::state::ReplicaWorker>(
            *master_, *follower_, follower_node_id_);
        running_.store(true);
        worker_thread_ = std::thread([this]() { this->run_loop(); });
        std::cerr << "[mindbridge_counselor] state_replica master_db=" << master_db_path_
                  << " follower_db=" << follower_db_path_
                  << " follower_node=" << follower_node_id_
                  << " poll_ms=" << poll_ms_ << std::endl;
    }

    ~StateReplicaRuntime() {
        running_.store(false);
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }

    json status() const {
        std::lock_guard<std::mutex> state_guard(state_mutex_);
        std::lock_guard<std::mutex> guard(status_mutex_);
        json out = {
            {"enabled", enabled_},
            {"running", running_.load()},
            {"master", {{"node_id", master_node_id_}, {"db_path", master_db_path_}}},
            {"follower", {{"node_id", follower_node_id_}, {"db_path", follower_db_path_}}},
            {"poll_ms", poll_ms_},
            {"batch", batch_},
            {"last_sync_applied", last_sync_applied_},
            {"last_error", last_error_},
        };
        if (master_) {
            const auto master_status = master_->status();
            out["master"]["record_count"] = master_status.record_count;
            out["master"]["change_count"] = master_status.change_count;
        }
        if (follower_) {
            const auto follower_status = follower_->status(follower_node_id_);
            out["follower"]["record_count"] = follower_status.record_count;
            out["follower"]["change_count"] = follower_status.change_count;
            out["follower"]["progress"] = {
                {"last_applied_change_id", follower_status.progress.last_applied_change_id},
                {"applied_count", follower_status.progress.applied_count},
                {"skipped_count", follower_status.progress.skipped_count},
                {"updated_at", follower_status.progress.updated_at},
            };
            const auto source_change_count = out["master"].value("change_count", 0);
            out["in_sync"] = follower_status.progress.last_applied_change_id >= source_change_count;
        } else {
            out["in_sync"] = false;
        }
        return out;
    }

private:
    void run_loop() {
        while (running_.load()) {
            try {
                int applied = 0;
                {
                    std::lock_guard<std::mutex> state_guard(state_mutex_);
                    applied = worker_ ? worker_->sync_once(batch_) : 0;
                }
                {
                    std::lock_guard<std::mutex> guard(status_mutex_);
                    last_sync_applied_ = applied;
                    last_error_.clear();
                }
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> guard(status_mutex_);
                last_error_ = e.what();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(std::max(100, poll_ms_)));
        }
    }

    mindbridge::state::DistributedStateStore* master_{nullptr};
    std::mutex& state_mutex_;
    std::unique_ptr<mindbridge::state::DistributedStateStore> follower_;
    std::unique_ptr<mindbridge::state::ReplicaWorker> worker_;
    std::thread worker_thread_;
    std::atomic<bool> running_{false};
    mutable std::mutex status_mutex_;
    std::string master_db_path_;
    std::string master_node_id_;
    std::string follower_db_path_;
    std::string follower_node_id_;
    int poll_ms_{1000};
    int batch_{1000};
    bool enabled_{false};
    int last_sync_applied_{0};
    std::string last_error_;
};

class CounselorService {
public:
    CounselorService()
        : model_(mindbridge::app::create_model_client_from_env()),
          chroma_(make_chroma()),
          state_store_(make_state_store()),
          replica_(state_store_.get(), state_mutex_),
          permission_(mindbridge::PermissionChecker::Mode::FullAuto),
          trace_(mindbridge::app::env_or("MINDBRIDGE_TRACE_PATH", "/tmp/mindbridge_counselor_trace.jsonl")),
          loop_(*model_, tools_, permission_, hooks_, &trace_) {
        // Validate RAG embedding chain at startup
        if (chroma_ && model_) {
            try {
                chroma_->query(*model_, "health check", 1);
                std::cerr << "[RAG] RAG embedding chain OK" << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "[RAG] RAG disabled: embedding model unavailable (" << e.what() << ")" << std::endl;
                chroma_.reset();
            }
        }
        mindbridge::register_core_tools(
            tools_, memory_, chroma_.get(), model_.get(), state_store_.get());
        hooks_.add_listener([this](mindbridge::HookEvent, const json& payload) {
            trace_.append({{"event", "hook"}, {"payload", payload}});
        });
    }

    std::string handle(const std::string& body) {
        std::string stage = "parse_request";
        try {
            const auto request = json::parse(body);
            stage = "extract_request";
            const std::string id = mindbridge::app::request_id_from(request);
            const std::string text = mindbridge::app::extract_text_from_jsonrpc(request);
            const json metadata = request.value("metadata", json::object());
            const bool evaluator_alert_owner = metadata.value("evaluator_alert_owner", false);
            const json multimodal = normalize_multimodal_metadata(
                metadata.value("multimodal_emotion", json::object()));
            stage = "build_multimodal_context";
            const std::string multimodal_context = mindbridge::app::multimodal_context_text(multimodal);
            mindbridge::RuntimeContext ctx;
            ctx.request_id = id;
            ctx.user_id = metadata.value("user_id", "anonymous");
            ctx.conversation_id = mindbridge::app::extract_context_id(request);
            ctx.trace_path = trace_.path();

            mindbridge::ToolContext tool_ctx = runtime_tool_context(ctx);
            stage = "agentic_reasoning";
            // Fast path: only for short CHAT messages
            std::string search_query = text;
            std::string rewritten_query = text;
            std::string thought;
            std::string action = "RETRIEVE";
            ctx.intent = mindbridge::RiskPolicy::heuristic_intent(text);
            const bool is_short_chat = text.size() <= 15 &&
                (text.find("你好") != std::string::npos || text.find("hi") != std::string::npos ||
                 text.find("嗨") != std::string::npos || text.find("hello") != std::string::npos);
            if (is_short_chat) {
                ctx.intent = "CHAT";
                action = "ANSWER";
            } else {
                // Full Agentic RAG: LLM reasoning for all non-short-chat cases
                std::string reasoning_text = model_->chat(
                    mindbridge::PromptPolicy::agentic_reasoning_prompt(), text);
                try {
                    auto reasoning = json::parse(reasoning_text);
                    ctx.intent = reasoning.value("intent", ctx.intent);
                    rewritten_query = reasoning.value("rewritten_query", text);
                    thought = reasoning.value("thought", "");
                    action = reasoning.value("action", "RETRIEVE");
                    search_query = reasoning.value("search_query", rewritten_query);
                    if (search_query.empty()) search_query = rewritten_query;
                } catch (const std::exception&) {}
            }
            ctx.risk_level = (ctx.intent == "RISK") ? "high" : "low";
            const std::string multimodal_risk = multimodal.value("risk_level", "");
            if (mindbridge::RiskPolicy::is_high_risk(multimodal_risk)) {
                ctx.intent = "RISK";
                ctx.risk_level = "high";
            } else if (ctx.risk_level == "low" && multimodal_risk == "medium") {
                ctx.intent = "CONSULT";
                ctx.risk_level = "medium";
            }
            tool_ctx = runtime_tool_context(ctx);
            if (!thought.empty()) {
                trace_.append({{"event", "agentic_reasoning"},
                               {"intent", ctx.intent},
                               {"action", action},
                               {"search_query", search_query},
                               {"thought", thought}});
            }
            stage = "write_user_memory";
            write_memory({{"conversation_id", ctx.conversation_id},
                          {"user_id", ctx.user_id},
                          {"risk_level", ctx.risk_level},
                          {"role", "user"},
                          {"content", text}},
                         tool_ctx);

            std::vector<std::string> contexts;
            if ((ctx.intent == "CONSULT" || ctx.intent == "RISK") && action == "RETRIEVE") {
                stage = "retrieve_knowledge";
                const auto rag = loop_.invoke_tool("retrieve_knowledge",
                                                   {{"query", search_query}, {"top_k", 3}},
                                                   tool_ctx);
                for (const auto& item : rag.value("contexts", json::array())) {
                    if (item.is_string()) {
                        contexts.push_back(item.get<std::string>());
                    }
                }
            }
            stage = "read_history";
            const auto history = loop_.invoke_tool("conversation_memory_read",
                                                   {{"conversation_id", ctx.conversation_id}, {"limit", 8}},
                                                   tool_ctx);
            stage = "build_prompt";
            std::string rag_text;
            for (const auto& c : contexts) { rag_text += c + "\n\n"; }
            std::string system_prompt;
            if (ctx.intent == "RISK") {
                system_prompt = mindbridge::PromptPolicy::crisis_prompt();
                if (!rag_text.empty()) {
                    system_prompt += "\n\n参考内容:\n" + rag_text;
                }
            } else if (ctx.intent == "CHAT") {
                system_prompt = mindbridge::PromptPolicy::direct_chat_prompt();
            } else {
                system_prompt = mindbridge::PromptPolicy::generate_answer_prompt(
                    "normal", ctx.risk_level, rag_text, text);
            }
            if (!multimodal_context.empty()) {
                system_prompt += "\n\n" + multimodal_context +
                                 "\nUse this multimodal signal as context, but do not mention camera or "
                                 "microphone details unless the user asks.";
            }
            std::vector<std::string> notes;
            for (const auto& note : memory_.relevant_notes(text, 3)) {
                notes.push_back(note.text);
            }
            mindbridge::AgentLoop::RunInput run_input;
            run_input.task_id = "counselor_request";
            run_input.user_id = ctx.user_id;
            run_input.session_id = ctx.conversation_id;
            run_input.conversation_id = ctx.conversation_id;
            run_input.system_rules = system_prompt;
            run_input.user_message = text;
            run_input.tools_surface = tools_.tool_names();
            run_input.memory_json = memory_.to_json();
            run_input.relevant_memory = notes;
            run_input.rag_contexts = contexts;
            run_input.history = history.value("history", "");
            stage = "run_model";
            const auto run_result = loop_.run(run_input);
            std::string reply = run_result.final_answer;

            // Validation-retry loop
            stage = "validate_response";
            mindbridge::ResponseValidator validator;
            auto validation = validator.validate(text, reply, rag_text);
            if (!validation.is_valid) {
                // Retry with broader context and an explicit correction note.
                trace_.append({{"event", "validation_retry"},
                               {"reason", validation.reason},
                               {"original_query", search_query},
                               {"retry_query", rewritten_query}});
                std::vector<std::string> retry_contexts;
                const auto retry_rag = loop_.invoke_tool("retrieve_knowledge",
                                                         {{"query", rewritten_query}, {"top_k", 3}},
                                                         tool_ctx);
                for (const auto& item : retry_rag.value("contexts", json::array())) {
                    if (item.is_string()) {
                        retry_contexts.push_back(item.get<std::string>());
                    }
                }
                std::string retry_rag_text;
                for (const auto& c : retry_contexts) { retry_rag_text += c + "\n\n"; }
                std::string retry_prompt = mindbridge::PromptPolicy::generate_answer_prompt(
                    "normal", ctx.risk_level, retry_rag_text, text);
                retry_prompt += "\n上一版回复未通过质量校验，原因: " + validation.reason +
                                "。请重新回答：不要复述用户原话，不要把历史记录当材料点评，"
                                "要直接回应当前求助者并给出具体支持。\n";
                mindbridge::AgentLoop::RunInput retry_input = run_input;
                retry_input.system_rules = retry_prompt;
                retry_input.rag_contexts = retry_contexts;
                const auto retry_result = loop_.run(retry_input);
                reply = retry_result.final_answer;
            }
            stage = "write_assistant_memory";
            write_memory({{"conversation_id", ctx.conversation_id},
                          {"user_id", ctx.user_id},
                          {"risk_level", ctx.risk_level},
                          {"role", "assistant"},
                          {"content", reply}},
                         tool_ctx);
            loop_.invoke_tool("mcp_excel_write",
                              {{"conversation_id", ctx.conversation_id},
                               {"intent", ctx.intent},
                               {"risk_level", ctx.risk_level},
                               {"user_text", text},
                               {"assistant_text", reply}},
                              tool_ctx);
            if (ctx.risk_level == "high" && !evaluator_alert_owner) {
                loop_.invoke_tool("mcp_email_alert",
                                  {{"conversation_id", ctx.conversation_id},
                                   {"risk_level", ctx.risk_level},
                                   {"message", text}},
                                  tool_ctx);
            }

            json response = {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"result",
                 {{"message",
                   {{"role", "agent"},
                    {"contextId", ctx.conversation_id},
                    {"parts", json::array({{{"kind", "text"}, {"text", reply}}})}}},
                  {"runtime", ctx.to_json()},
                  {"multimodal_emotion", multimodal},
                  {"ragContexts", contexts.size()},
                  {"run_id", run_result.run_id},
                  {"run_dir", run_result.run_dir},
                  {"prompt_metadata", run_result.prompt_metadata}}},
            };
            trace_.append({{"event", "request_complete"}, {"runtime", ctx.to_json()}});
            return response.dump();
        } catch (const std::exception& e) {
            return mindbridge::app::json_error("unknown", -32000, stage + ": " + e.what());
        }
    }

    void handle_stream(const std::string& body,
                       const std::function<bool(const std::string&)>& write_event) {
        std::string stage = "parse_request";
        auto emit = [&](const json& event) {
            return !write_event || write_event(event.dump());
        };
        try {
            const auto request = json::parse(body);
            stage = "extract_request";
            const std::string id = mindbridge::app::request_id_from(request);
            const std::string text = mindbridge::app::extract_text_from_jsonrpc(request);
            const json metadata = request.value("metadata", json::object());
            const bool evaluator_alert_owner = metadata.value("evaluator_alert_owner", false);
            const json multimodal = normalize_multimodal_metadata(
                metadata.value("multimodal_emotion", json::object()));
            stage = "build_multimodal_context";
            const std::string multimodal_context = mindbridge::app::multimodal_context_text(multimodal);
            mindbridge::RuntimeContext ctx;
            ctx.request_id = id;
            ctx.user_id = metadata.value("user_id", "anonymous");
            ctx.conversation_id = mindbridge::app::extract_context_id(request);
            ctx.trace_path = trace_.path();

            mindbridge::ToolContext tool_ctx = runtime_tool_context(ctx);
            stage = "agentic_reasoning";
            // Fast path: only for short CHAT messages
            std::string search_query = text;
            std::string rewritten_query = text;
            std::string thought;
            std::string action = "RETRIEVE";
            ctx.intent = mindbridge::RiskPolicy::heuristic_intent(text);
            const bool is_short_chat = text.size() <= 15 &&
                (text.find("你好") != std::string::npos || text.find("hi") != std::string::npos ||
                 text.find("嗨") != std::string::npos || text.find("hello") != std::string::npos);
            if (is_short_chat) {
                ctx.intent = "CHAT";
                action = "ANSWER";
            } else {
                // Full Agentic RAG: LLM reasoning for all non-short-chat cases
                std::string reasoning_text = model_->chat(
                    mindbridge::PromptPolicy::agentic_reasoning_prompt(), text);
                try {
                    auto reasoning = json::parse(reasoning_text);
                    ctx.intent = reasoning.value("intent", ctx.intent);
                    rewritten_query = reasoning.value("rewritten_query", text);
                    thought = reasoning.value("thought", "");
                    action = reasoning.value("action", "RETRIEVE");
                    search_query = reasoning.value("search_query", rewritten_query);
                    if (search_query.empty()) search_query = rewritten_query;
                } catch (const std::exception&) {}
            }
            ctx.risk_level = (ctx.intent == "RISK") ? "high" : "low";
            const std::string multimodal_risk = multimodal.value("risk_level", "");
            if (mindbridge::RiskPolicy::is_high_risk(multimodal_risk)) {
                ctx.intent = "RISK";
                ctx.risk_level = "high";
            } else if (ctx.risk_level == "low" && multimodal_risk == "medium") {
                ctx.intent = "CONSULT";
                ctx.risk_level = "medium";
            }
            tool_ctx = runtime_tool_context(ctx);
            if (!thought.empty()) {
                trace_.append({{"event", "agentic_reasoning"},
                               {"intent", ctx.intent},
                               {"action", action},
                               {"search_query", search_query},
                               {"thought", thought}});
            }
            stage = "write_user_memory";
            write_memory({{"conversation_id", ctx.conversation_id},
                          {"user_id", ctx.user_id},
                          {"risk_level", ctx.risk_level},
                          {"role", "user"},
                          {"content", text}},
                         tool_ctx);

            std::vector<std::string> contexts;
            if ((ctx.intent == "CONSULT" || ctx.intent == "RISK") && action == "RETRIEVE") {
                stage = "retrieve_knowledge";
                const auto rag = loop_.invoke_tool("retrieve_knowledge",
                                                   {{"query", search_query}, {"top_k", 3}},
                                                   tool_ctx);
                for (const auto& item : rag.value("contexts", json::array())) {
                    if (item.is_string()) {
                        contexts.push_back(item.get<std::string>());
                    }
                }
            }
            stage = "read_history";
            const auto history = loop_.invoke_tool("conversation_memory_read",
                                                   {{"conversation_id", ctx.conversation_id}, {"limit", 8}},
                                                   tool_ctx);
            stage = "build_prompt";
            std::string rag_text;
            for (const auto& c : contexts) { rag_text += c + "\n\n"; }
            std::string system_prompt;
            if (ctx.intent == "RISK") {
                system_prompt = mindbridge::PromptPolicy::crisis_prompt();
                if (!rag_text.empty()) {
                    system_prompt += "\n\n参考内容:\n" + rag_text;
                }
            } else if (ctx.intent == "CHAT") {
                system_prompt = mindbridge::PromptPolicy::direct_chat_prompt();
            } else {
                system_prompt = mindbridge::PromptPolicy::generate_answer_prompt(
                    "normal", ctx.risk_level, rag_text, text);
            }
            if (!multimodal_context.empty()) {
                system_prompt += "\n\n" + multimodal_context +
                                 "\nUse this multimodal signal as context, but do not mention camera or "
                                 "microphone details unless the user asks.";
            }
            std::vector<std::string> notes;
            for (const auto& note : memory_.relevant_notes(text, 3)) {
                notes.push_back(note.text);
            }
            mindbridge::AgentLoop::RunInput run_input;
            run_input.task_id = "counselor_request";
            run_input.user_id = ctx.user_id;
            run_input.session_id = ctx.conversation_id;
            run_input.conversation_id = ctx.conversation_id;
            run_input.system_rules = system_prompt;
            run_input.user_message = text;
            run_input.tools_surface = tools_.tool_names();
            run_input.memory_json = memory_.to_json();
            run_input.relevant_memory = notes;
            run_input.rag_contexts = contexts;
            run_input.history = history.value("history", "");
            stage = "run_model_stream";
            const auto run_result = loop_.run_stream(run_input, emit);
            const std::string reply = run_result.final_answer;

            stage = "write_assistant_memory";
            write_memory({{"conversation_id", ctx.conversation_id},
                          {"user_id", ctx.user_id},
                          {"risk_level", ctx.risk_level},
                          {"role", "assistant"},
                          {"content", reply}},
                         tool_ctx);
            loop_.invoke_tool("mcp_excel_write",
                              {{"conversation_id", ctx.conversation_id},
                               {"intent", ctx.intent},
                               {"risk_level", ctx.risk_level},
                               {"user_text", text},
                               {"assistant_text", reply}},
                              tool_ctx);
            if (ctx.risk_level == "high" && !evaluator_alert_owner) {
                loop_.invoke_tool("mcp_email_alert",
                                  {{"conversation_id", ctx.conversation_id},
                                   {"risk_level", ctx.risk_level},
                                   {"message", text}},
                                  tool_ctx);
            }
            trace_.append({{"event", "request_complete"}, {"runtime", ctx.to_json()}, {"stream", true}});
            emit({{"event", "final_result"},
                  {"id", id},
                  {"result",
                   {{"message",
                     {{"role", "agent"},
                      {"contextId", ctx.conversation_id},
                      {"parts", json::array({{{"kind", "text"}, {"text", reply}}})}}},
                    {"runtime", ctx.to_json()},
                    {"multimodal_emotion", multimodal},
                    {"ragContexts", contexts.size()},
                    {"run_id", run_result.run_id},
                    {"run_dir", run_result.run_dir},
                    {"prompt_metadata", run_result.prompt_metadata}}}});
        } catch (const std::exception& e) {
            emit({{"event", "error"}, {"message", stage + ": " + std::string(e.what())}});
        }
    }

    std::string replication_status() const {
        return replica_.status().dump();
    }

private:
    static std::unique_ptr<mindbridge::ChromaVectorStore> make_chroma() {
        const std::string url = mindbridge::app::env_or("MINDBRIDGE_CHROMA_URL");
        const std::string collection = mindbridge::app::env_or("MINDBRIDGE_CHROMA_COLLECTION");
        if (url.empty() || collection.empty()) {
            return nullptr;
        }
        return std::make_unique<mindbridge::ChromaVectorStore>(
            url, collection, mindbridge::app::env_or("MINDBRIDGE_EMBEDDING_MODEL", "nomic-embed-text"));
    }

    json write_memory(const json& arguments, mindbridge::ToolContext& tool_ctx) {
        std::lock_guard<std::mutex> guard(state_mutex_);
        return loop_.invoke_tool("conversation_memory_write", arguments, tool_ctx);
    }

    std::unique_ptr<mindbridge::ModelClient> model_;
    std::unique_ptr<mindbridge::ChromaVectorStore> chroma_;
    std::unique_ptr<mindbridge::state::DistributedStateStore> state_store_;
    mutable std::mutex state_mutex_;
    StateReplicaRuntime replica_;
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
    const int port = (argc > 1) ? std::atoi(argv[1]) : 5010;
    CounselorService service;
    mindbridge::net::AsyncHttpServer server(port, mindbridge::net::AsyncHttpServer::options_from_env());
    server.register_handler("/api/health", [](const std::string&) {
        return R"({"ok":true,"service":"mindbridge_counselor"})";
    });
    server.register_handler("/api/state/replication/status", [&service](const std::string&) {
        return service.replication_status();
    });
    server.register_handler("/", [&service](const std::string& body) { return service.handle(body); });
    server.register_stream_handler("/", [&service](const std::string& body,
                                                   std::function<bool(const std::string&)> write_event) {
        service.handle_stream(body, write_event);
    });
    std::cout << "[mindbridge_counselor] port=" << port << std::endl;
    server.start();
    curl_global_cleanup();
    return 0;
}
