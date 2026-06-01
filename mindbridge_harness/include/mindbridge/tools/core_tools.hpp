#pragma once

#include "mindbridge/harness/tool.hpp"
#include "mindbridge/harness/tool_registry.hpp"
#include "mindbridge/model/model_client.hpp"
#include "mindbridge/rag/vector_store_client.hpp"
#include "mindbridge/runtime/conversation_memory.hpp"

#include <memory>
#include <string>

namespace mindbridge {

namespace state {
class DistributedStateStore;
}  // namespace state

class RetrieveKnowledgeTool : public ITool {
public:
    RetrieveKnowledgeTool(VectorStoreClient* store, ModelClient* model);
    std::string name() const override { return "retrieve_knowledge"; }
    std::string description() const override { return "Retrieve relevant knowledge from vector store."; }
    ToolSchema schema() const override;
    bool is_read_only(const nlohmann::json&) const override { return true; }
    std::optional<std::string> validate(const nlohmann::json& arguments) const override;
    ToolResult execute(const nlohmann::json& arguments, ToolContext& ctx) override;

private:
    VectorStoreClient* store_;
    ModelClient* model_;
};

class AssessRiskTool : public ITool {
public:
    std::string name() const override { return "assess_risk"; }
    std::string description() const override { return "Assess mental-health risk level."; }
    ToolSchema schema() const override;
    bool is_read_only(const nlohmann::json&) const override { return true; }
    std::optional<std::string> validate(const nlohmann::json& arguments) const override;
    ToolResult execute(const nlohmann::json& arguments, ToolContext& ctx) override;
};

class ConversationMemoryReadTool : public ITool {
public:
    explicit ConversationMemoryReadTool(ConversationMemory& memory);
    std::string name() const override { return "conversation_memory_read"; }
    std::string description() const override { return "Read recent conversation memory."; }
    ToolSchema schema() const override;
    bool is_read_only(const nlohmann::json&) const override { return true; }
    std::optional<std::string> validate(const nlohmann::json& arguments) const override;
    ToolResult execute(const nlohmann::json& arguments, ToolContext& ctx) override;

private:
    ConversationMemory& memory_;
};

class ConversationMemoryWriteTool : public ITool {
public:
    explicit ConversationMemoryWriteTool(ConversationMemory& memory,
                                         state::DistributedStateStore* state_store = nullptr);
    std::string name() const override { return "conversation_memory_write"; }
    std::string description() const override { return "Append a message to conversation memory."; }
    ToolSchema schema() const override;
    bool is_read_only(const nlohmann::json&) const override { return false; }
    std::optional<std::string> validate(const nlohmann::json& arguments) const override;
    ToolResult execute(const nlohmann::json& arguments, ToolContext& ctx) override;

private:
    ConversationMemory& memory_;
    state::DistributedStateStore* state_store_;
};

class StateReplicationStatusTool : public ITool {
public:
    explicit StateReplicationStatusTool(state::DistributedStateStore& state_store);
    std::string name() const override { return "state_replication_status"; }
    std::string description() const override { return "Read distributed state replication status."; }
    ToolSchema schema() const override;
    bool is_read_only(const nlohmann::json&) const override { return true; }
    std::optional<std::string> validate(const nlohmann::json& arguments) const override;
    ToolResult execute(const nlohmann::json& arguments, ToolContext& ctx) override;

private:
    state::DistributedStateStore& state_store_;
};

class McpDispatchTool : public ITool {
public:
    McpDispatchTool(std::string tool_name, std::string description, std::string endpoint_env);
    std::string name() const override { return tool_name_; }
    std::string description() const override { return description_; }
    ToolSchema schema() const override;
    bool is_read_only(const nlohmann::json&) const override { return false; }
    std::optional<std::string> validate(const nlohmann::json& arguments) const override;
    ToolResult execute(const nlohmann::json& arguments, ToolContext& ctx) override;

private:
    std::string tool_name_;
    std::string description_;
    std::string endpoint_env_;
};

void register_core_tools(ToolRegistry& registry,
                         ConversationMemory& memory,
                         VectorStoreClient* store,
                         ModelClient* model,
                         state::DistributedStateStore* state_store = nullptr);

}  // namespace mindbridge
