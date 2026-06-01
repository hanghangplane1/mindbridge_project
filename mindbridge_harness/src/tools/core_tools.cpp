#include "mindbridge/tools/core_tools.hpp"

#include "mindbridge/runtime/risk_policy.hpp"
#include "mindbridge/state/distributed_state_store.hpp"

#include <curl/curl.h>

#include <cstdlib>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace mindbridge {

namespace {

size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::string*>(userp);
    out->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

std::string post_json(const std::string& url, const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl_easy_init failed");
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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        throw std::runtime_error(curl_easy_strerror(rc));
    }
    if (http_code >= 400) {
        throw std::runtime_error("HTTP " + std::to_string(http_code) + ": " + response);
    }
    return response;
}

std::string runtime_value(const ToolContext& ctx,
                          const std::string& key,
                          const std::string& fallback) {
    if (ctx.metadata.contains("runtime") && ctx.metadata.at("runtime").is_object()) {
        return ctx.metadata.at("runtime").value(key, fallback);
    }
    return fallback;
}

}  // namespace

RetrieveKnowledgeTool::RetrieveKnowledgeTool(VectorStoreClient* store, ModelClient* model)
    : store_(store), model_(model) {}

ToolSchema RetrieveKnowledgeTool::schema() const {
    return ToolSchema{
        {{"query", ToolSchema::Field::Type::String, true},
         {"top_k", ToolSchema::Field::Type::Number, false}},
        ToolRisk::Low,
        4000};
}

std::optional<std::string> RetrieveKnowledgeTool::validate(const nlohmann::json& arguments) const {
    const std::string query = arguments.value("query", "");
    const int top_k = arguments.value("top_k", 3);
    if (query.empty()) {
        return "query cannot be empty";
    }
    if (top_k < 1 || top_k > 10) {
        return "top_k must be in [1, 10]";
    }
    return std::nullopt;
}

ToolResult RetrieveKnowledgeTool::execute(const nlohmann::json& arguments, ToolContext&) {
    if (!store_ || !model_) {
        static bool warned = false;
        if (!warned) {
            fprintf(stderr, "[RAG] WARNING: retrieve_knowledge called but RAG is disabled (no vector store or model)\n");
            warned = true;
        }
        ToolResult result;
        result.summary = "vector store is disabled";
        result.data = {{"contexts", nlohmann::json::array()}, {"disabled", true}};
        return result;
    }
    const std::string query = arguments.value("query", "");
    const int top_k = arguments.value("top_k", 3);
    ToolResult result;
    result.summary = "knowledge retrieved";
    result.data = {{"contexts", store_->query(*model_, query, top_k)}};
    return result;
}

ToolSchema AssessRiskTool::schema() const {
    return ToolSchema{
        {{"text", ToolSchema::Field::Type::String, true}},
        ToolRisk::Low,
        2000};
}

std::optional<std::string> AssessRiskTool::validate(const nlohmann::json& arguments) const {
    if (arguments.value("text", "").empty()) {
        return "text cannot be empty";
    }
    return std::nullopt;
}

ToolResult AssessRiskTool::execute(const nlohmann::json& arguments, ToolContext&) {
    const std::string text = arguments.value("text", "");
    const std::string intent = RiskPolicy::heuristic_intent(text);
    const std::string risk = RiskPolicy::heuristic_risk_level(text);
    ToolResult result;
    result.summary = "risk assessed";
    result.data = {{"intent", intent}, {"riskLevel", risk}};
    return result;
}

ConversationMemoryReadTool::ConversationMemoryReadTool(ConversationMemory& memory) : memory_(memory) {}

ToolSchema ConversationMemoryReadTool::schema() const {
    return ToolSchema{
        {{"conversation_id", ToolSchema::Field::Type::String, true},
         {"limit", ToolSchema::Field::Type::Number, false}},
        ToolRisk::Low,
        4000};
}

std::optional<std::string> ConversationMemoryReadTool::validate(const nlohmann::json& arguments) const {
    const int limit = arguments.value("limit", 8);
    if (limit < 1 || limit > 50) {
        return "limit must be in [1, 50]";
    }
    return std::nullopt;
}

ToolResult ConversationMemoryReadTool::execute(const nlohmann::json& arguments, ToolContext&) {
    const std::string conversation_id = arguments.value("conversation_id", "default");
    const int limit = arguments.value("limit", 8);
    ToolResult result;
    result.summary = "conversation history loaded";
    result.data = {{"history", memory_.recent_text(conversation_id, limit)}};
    return result;
}

ConversationMemoryWriteTool::ConversationMemoryWriteTool(
    ConversationMemory& memory,
    state::DistributedStateStore* state_store)
    : memory_(memory), state_store_(state_store) {}

ToolSchema ConversationMemoryWriteTool::schema() const {
    return ToolSchema{
        {{"conversation_id", ToolSchema::Field::Type::String, true},
         {"user_id", ToolSchema::Field::Type::String, false},
         {"namespace", ToolSchema::Field::Type::String, false},
         {"risk_level", ToolSchema::Field::Type::String, false},
         {"role", ToolSchema::Field::Type::String, true},
         {"content", ToolSchema::Field::Type::String, true}},
        ToolRisk::High,
        2000};
}

std::optional<std::string> ConversationMemoryWriteTool::validate(const nlohmann::json& arguments) const {
    const std::string role = arguments.value("role", "");
    if (role != "user" && role != "assistant" && role != "system" && role != "agent") {
        return "role must be one of user/assistant/system/agent";
    }
    if (arguments.value("content", "").empty()) {
        return "content cannot be empty";
    }
    return std::nullopt;
}

ToolResult ConversationMemoryWriteTool::execute(const nlohmann::json& arguments, ToolContext& ctx) {
    const std::string conversation_id = arguments.value("conversation_id", "default");
    const std::string role = arguments.value("role", "user");
    const std::string content = arguments.value("content", "");
    memory_.append(conversation_id, role, content);

    std::int64_t change_id = 0;
    bool distributed_saved = false;
    if (state_store_) {
        const std::string user_id = arguments.value(
            "user_id", runtime_value(ctx, "user_id", "anonymous"));
        const std::string risk_level = arguments.value(
            "risk_level", runtime_value(ctx, "risk_level", "low"));
        std::string namespace_name = arguments.value("namespace", "");
        if (namespace_name.empty()) {
            namespace_name = (risk_level == "high") ? "risk_memory" : "chat_memory";
        }
        change_id = state_store_->append_conversation_turn(
            user_id, conversation_id, namespace_name, role, content, risk_level);
        distributed_saved = true;
    }

    ToolResult result;
    result.summary = "conversation memory updated";
    result.data = {{"saved", true},
                   {"distributed_saved", distributed_saved},
                   {"change_id", change_id}};
    return result;
}

StateReplicationStatusTool::StateReplicationStatusTool(state::DistributedStateStore& state_store)
    : state_store_(state_store) {}

ToolSchema StateReplicationStatusTool::schema() const {
    return ToolSchema{
        {{"node_id", ToolSchema::Field::Type::String, false}},
        ToolRisk::Low,
        2000};
}

std::optional<std::string> StateReplicationStatusTool::validate(const nlohmann::json&) const {
    return std::nullopt;
}

ToolResult StateReplicationStatusTool::execute(const nlohmann::json& arguments, ToolContext&) {
    const auto status = state_store_.status(arguments.value("node_id", ""));
    ToolResult result;
    result.summary = "state replication status loaded";
    result.data = {{"node_id", status.node_id},
                   {"record_count", status.record_count},
                   {"change_count", status.change_count},
                   {"progress",
                    {{"node_id", status.progress.node_id},
                     {"last_applied_change_id", status.progress.last_applied_change_id},
                     {"applied_count", status.progress.applied_count},
                     {"skipped_count", status.progress.skipped_count},
                     {"updated_at", status.progress.updated_at}}}};
    return result;
}

McpDispatchTool::McpDispatchTool(std::string tool_name,
                                 std::string description,
                                 std::string endpoint_env)
    : tool_name_(std::move(tool_name)),
      description_(std::move(description)),
      endpoint_env_(std::move(endpoint_env)) {}

ToolSchema McpDispatchTool::schema() const {
    return ToolSchema{
        {{"conversation_id", ToolSchema::Field::Type::String, false},
         {"risk_level", ToolSchema::Field::Type::String, false},
         {"message", ToolSchema::Field::Type::String, false},
         {"subject", ToolSchema::Field::Type::String, false},
         {"body", ToolSchema::Field::Type::String, false},
         {"assessment", ToolSchema::Field::Type::Object, false}},
        ToolRisk::High,
        6000};
}

std::optional<std::string> McpDispatchTool::validate(const nlohmann::json& arguments) const {
    if (tool_name_ == "mcp_email_alert") {
        const std::string level = arguments.value("risk_level", "");
        if (level.empty()) {
            return "risk_level is required for mcp_email_alert";
        }
    }
    return std::nullopt;
}

ToolResult McpDispatchTool::execute(const nlohmann::json& arguments, ToolContext&) {
    const char* endpoint = std::getenv(endpoint_env_.c_str());
    if (!endpoint || std::string(endpoint).empty()) {
        ToolResult result;
        result.summary = "mcp endpoint not configured";
        result.data = {{"dispatched", false}, {"reason", endpoint_env_ + " is not configured"}};
        return result;
    }
    const std::string response = post_json(endpoint, arguments.dump());
    ToolResult result;
    result.summary = "mcp dispatch completed";
    result.data = {{"dispatched", true}, {"response_chars", response.size()}};
    return result;
}

void register_core_tools(ToolRegistry& registry,
                         ConversationMemory& memory,
                         VectorStoreClient* store,
                         ModelClient* model,
                         state::DistributedStateStore* state_store) {
    registry.register_tool(std::make_shared<RetrieveKnowledgeTool>(store, model));
    registry.register_tool(std::make_shared<AssessRiskTool>());
    registry.register_tool(std::make_shared<ConversationMemoryReadTool>(memory));
    registry.register_tool(std::make_shared<ConversationMemoryWriteTool>(memory, state_store));
    if (state_store) {
        registry.register_tool(std::make_shared<StateReplicationStatusTool>(*state_store));
    }
    registry.register_tool(std::make_shared<McpDispatchTool>(
        "mcp_excel_write", "Dispatch consultation record to MCP Excel endpoint.", "MINDBRIDGE_MCP_EXCEL_URL"));
    registry.register_tool(std::make_shared<McpDispatchTool>(
        "mcp_email_alert", "Dispatch high-risk alert to MCP Email endpoint.", "MINDBRIDGE_MCP_EMAIL_URL"));
}

}  // namespace mindbridge
