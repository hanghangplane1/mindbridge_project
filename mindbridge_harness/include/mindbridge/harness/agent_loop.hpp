#pragma once

#include "mindbridge/harness/hook_executor.hpp"
#include "mindbridge/harness/permission_checker.hpp"
#include "mindbridge/harness/token_budget.hpp"
#include "mindbridge/harness/tool_registry.hpp"
#include "mindbridge/harness/trace_recorder.hpp"
#include "mindbridge/harness/trajectory_analyzer.hpp"
#include "mindbridge/model/model_client.hpp"
#include "mindbridge/runtime/context_manager.hpp"
#include "mindbridge/runtime/run_store.hpp"
#include "mindbridge/runtime/session_store.hpp"
#include "mindbridge/runtime/task_state.hpp"
#include "mindbridge/cache/caffeine_cache.h"

#include <nlohmann/json.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mindbridge {

/**
 * 单轮 / 工具调用编排：生成文本、在权限与 Hook 下执行已注册工具。
 */
class AgentLoop {
public:
    struct RunInput {
        std::string task_id;
        std::string user_id{"anonymous"};
        std::string session_id;
        std::string conversation_id;
        std::string system_rules;
        std::string user_message;
        std::vector<std::string> tools_surface;
        nlohmann::json memory_json = nlohmann::json::object();
        std::vector<std::string> relevant_memory;
        std::vector<std::string> rag_contexts;
        std::string history;
        int max_attempts{3};
        int max_tool_steps{6};
        int token_budget_limit{8000};  // total token budget for this run
        std::string intent{"CHAT"};    // intent from PromptPolicy, used for budget degradation
    };

    struct RunResult {
        TaskState task_state;
        std::string final_answer;
        nlohmann::json prompt_metadata = nlohmann::json::object();
        std::string run_id;
        std::string run_dir;
        int token_budget_used{0};
        int token_budget_limit{0};
        std::string budget_level_reached{"GREEN"};
        double trajectory_efficiency_score{1.0};
        bool trajectory_loop_detected{false};
    };

    AgentLoop(ModelClient& model,
              ToolRegistry& tools,
              PermissionChecker& perm,
              HookExecutor& hooks,
              TraceRecorder* trace = nullptr);

    /** 直接调用模型，不走路由工具 */
    std::string generate(const std::string& system_prompt, const std::string& user_message);

    /** 执行已注册工具（先权限、再 Hook） */
    nlohmann::json invoke_tool(const std::string& tool_name,
                               const nlohmann::json& arguments,
                               ToolContext& ctx);
    RunResult run(const RunInput& input);
    RunResult run_stream(const RunInput& input,
                         const std::function<bool(const nlohmann::json& event)>& on_event);

    ModelClient& model() { return model_; }

private:
    bool is_repeated_tool_call(const std::string& tool_name, const nlohmann::json& arguments) const;
    ContextBuildOutput build_context(const RunInput& input);

    ModelClient& model_;
    ToolRegistry& tools_;
    PermissionChecker& perm_;
    HookExecutor& hooks_;
    TraceRecorder* trace_;
    std::optional<std::string> last_tool_name_;
    nlohmann::json last_tool_args_;
    TokenBudgetController* budget_{nullptr};  // non-owning, set per-run
    TrajectoryAnalyzer trajectory_;
    std::vector<ToolCallRecord> tool_calls_;

    // Prompt prefix cache: key = prefix.hash + tool_signature, value = built context JSON
    using PromptCache = agent_rpc::mcp::rag::CaffeineCache<std::string, nlohmann::json>;
    std::unique_ptr<PromptCache> prompt_cache_;
    std::string last_tool_signature_;
};

}  // namespace mindbridge
