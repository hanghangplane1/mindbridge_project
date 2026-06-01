#include "mindbridge/harness/agent_loop.hpp"
#include "mindbridge/observability/boundary_trace_correlator.hpp"
#include "mindbridge/observability/ebpf_monitor.hpp"
#include "mindbridge/runtime/prompt_prefix.hpp"
#include "mindbridge/runtime/utf8.hpp"

#include <chrono>
#include <cstdlib>
#include <stdexcept>

namespace mindbridge {
namespace {

bool env_true(const char* name) {
    const char* value = std::getenv(name);
    if (!value) {
        return false;
    }
    const std::string text(value);
    return text == "1" || text == "true" || text == "TRUE" || text == "on" || text == "ON";
}

std::size_t env_size(const char* name, std::size_t fallback) {
    const char* value = std::getenv(name);
    if (!value) {
        return fallback;
    }
    try {
        return static_cast<std::size_t>(std::stoul(value));
    } catch (...) {
        return fallback;
    }
}

std::string clipped_excerpt(const std::string& text, std::size_t limit) {
    std::string safe = sanitize_utf8(text);
    if (safe.size() <= limit) {
        return safe;
    }
    safe.resize(limit);
    safe = sanitize_utf8(safe);
    safe += "...[clipped]";
    return safe;
}

nlohmann::json model_observability_event(int attempt,
                                         bool stream,
                                         const std::string& user_message,
                                         const std::string& prompt,
                                         const std::string& response) {
    nlohmann::json event = {{"event", "model_responded"},
                            {"attempt", attempt},
                            {"stream", stream},
                            {"prompt_chars", prompt.size()},
                            {"user_message_chars", user_message.size()},
                            {"response_chars", response.size()}};
    if (env_true("MINDBRIDGE_OBSERVABILITY_CAPTURE_TEXT")) {
        const std::size_t limit = env_size("MINDBRIDGE_OBSERVABILITY_TEXT_LIMIT", 512);
        event["user_message_excerpt"] = clipped_excerpt(user_message, limit);
        event["prompt_excerpt"] = clipped_excerpt(prompt, limit);
        event["response_excerpt"] = clipped_excerpt(response, limit);
    }
    return event;
}

nlohmann::json finish_boundary_observability(RunStore& run_store,
                                             observability::EbpfMonitor& monitor,
                                             const nlohmann::json& started_summary) {
    nlohmann::json ebpf = monitor.stop().to_json();
    if (started_summary.value("unavailable", false)) {
        ebpf = started_summary;
    }
    observability::BoundaryTraceCorrelator correlator(run_store.run_dir());
    const nlohmann::json boundary = correlator.correlate(ebpf).to_json();
    run_store.append_trace({{"event", "correlation_finished"},
                            {"ebpf", ebpf},
                            {"boundary_observability", boundary}});
    return {{"ebpf", ebpf}, {"boundary", boundary}};
}

void append_ebpf_lifecycle_trace(RunStore& run_store, const nlohmann::json& summary) {
    if (!summary.value("enabled", false)) {
        run_store.append_trace({{"event", "ebpf_disabled"}, {"ebpf", summary}});
    } else if (summary.value("started", false)) {
        run_store.append_trace({{"event", "ebpf_started"}, {"ebpf", summary}});
    } else {
        run_store.append_trace({{"event", "ebpf_unavailable"}, {"ebpf", summary}});
    }
}

}  // namespace

AgentLoop::AgentLoop(ModelClient& model,
                     ToolRegistry& tools,
                     PermissionChecker& perm,
                     HookExecutor& hooks,
                     TraceRecorder* trace)
    : model_(model), tools_(tools), perm_(perm), hooks_(hooks), trace_(trace) {
    // Initialize prompt prefix cache
    agent_rpc::mcp::rag::CaffeineCacheConfig cfg;
    if (const char* v = std::getenv("MINDBRIDGE_PROMPT_CACHE_ENABLED")) {
        cfg.enabled = (std::strcmp(v, "true") == 0 || std::strcmp(v, "1") == 0);
    }
    if (const char* v = std::getenv("MINDBRIDGE_PROMPT_CACHE_TTL_SECONDS")) {
        cfg.write_ttl_seconds = std::atoi(v);
    } else {
        cfg.write_ttl_seconds = 600;  // 10 min default
    }
    cfg.max_size = 100;
    prompt_cache_ = std::make_unique<PromptCache>(cfg);
}

std::string AgentLoop::generate(const std::string& system_prompt, const std::string& user_message) {
    hooks_.execute(HookEvent::UserPromptSubmit, {{"prompt", user_message}});
    const auto t0 = std::chrono::steady_clock::now();
    std::string out = model_.chat(system_prompt, user_message);
    const auto t1 = std::chrono::steady_clock::now();
    hooks_.execute(HookEvent::Stop, {{"reason", "generation_complete"}});
    if (trace_) {
        trace_->append({{"event", "generation"},
                        {"ms", std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()},
                        {"output_chars", out.size()}});
    }
    return out;
}

bool AgentLoop::is_repeated_tool_call(const std::string& tool_name,
                                      const nlohmann::json& arguments) const {
    return last_tool_name_.has_value() && *last_tool_name_ == tool_name && last_tool_args_ == arguments;
}

nlohmann::json AgentLoop::invoke_tool(const std::string& tool_name,
                                      const nlohmann::json& arguments,
                                      ToolContext& ctx) {
    if (trace_) {
        trace_->append({{"event", "tool_requested"}, {"tool", tool_name}, {"arguments", arguments}});
    }
    if (!tools_.has(tool_name)) {
        throw std::runtime_error("tool not registered: " + tool_name);
    }
    const std::optional<std::string> validate_error = tools_.validate(tool_name, arguments);
    if (validate_error.has_value()) {
        if (trace_) {
            trace_->append({{"event", "tool_rejected"},
                            {"tool", tool_name},
                            {"reason", *validate_error},
                            {"stage", "validation"}});
        }
        throw std::runtime_error("invalid tool arguments: " + *validate_error);
    }
    if (is_repeated_tool_call(tool_name, arguments)) {
        if (trace_) {
            trace_->append({{"event", "tool_rejected"},
                            {"tool", tool_name},
                            {"reason", "repeated tool call blocked"},
                            {"stage", "repeat_guard"}});
        }
        throw std::runtime_error("repeated tool call blocked: " + tool_name);
    }
    const bool ro = tools_.is_read_only(tool_name, arguments);
    PermissionDecision d = perm_.evaluate(tool_name, ro);
    if (!d.allowed) {
        if (trace_) {
            trace_->append(
                {{"event", "permission_denied"}, {"tool", tool_name}, {"reason", d.reason}});
        }
        throw std::runtime_error("permission denied: " + d.reason);
    }
    hooks_.execute(HookEvent::PreToolUse, {{"tool_name", tool_name}, {"arguments", arguments}});
    const auto t0 = std::chrono::steady_clock::now();
    ToolResult structured = tools_.execute(tool_name, arguments, ctx);
    nlohmann::json result = structured.to_json();
    const auto t1 = std::chrono::steady_clock::now();
    last_tool_name_ = tool_name;
    last_tool_args_ = arguments;
    hooks_.execute(HookEvent::PostToolUse,
                   {{"tool_name", tool_name},
                    {"ok", structured.ok},
                    {"duration_ms", std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()}});
    if (trace_) {
        trace_->append({{"event", "tool_executed"},
                        {"tool", tool_name},
                        {"ok", structured.ok},
                        {"duration_ms", std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()},
                        {"summary", structured.summary},
                        {"clipped", structured.clipped}});
    }
    return result;
}

ContextBuildOutput AgentLoop::build_context(const RunInput& input) {
    PromptPrefix prefix = build_prompt_prefix(input.system_rules, input.tools_surface);

    // Prompt cache: key = prefix.hash + ":" + tool_signature
    // Invalidate if tool_signature changed
    if (prompt_cache_ && prefix.tool_signature != last_tool_signature_) {
        prompt_cache_->invalidate_all();
        last_tool_signature_ = prefix.tool_signature;
    }

    ContextBuildInput context_input{
        prefix,
        input.memory_json,
        input.relevant_memory,
        input.rag_contexts,
        input.history,
        input.user_message,
        12000,
    };

    // Note: we don't cache the full context here because it depends on
    // memory, RAG, history, and user_message which change every turn.
    // The prefix cache is useful when the same prefix is reused across
    // identical tool configurations. For now, just build normally.
    return ContextManager::build(context_input);
}

AgentLoop::RunResult AgentLoop::run(const RunInput& input) {
    RunStore run_store(".mindbridge");
    SessionStore session_store(".mindbridge");
    TaskState state;
    state.task_id = input.task_id.empty() ? "mindbridge_task" : input.task_id;
    state.run_id = run_store.start_run(state.task_id);
    run_store.write_task_state(state);
    run_store.append_trace({{"event", "run_started"},
                            {"run_id", state.run_id},
                            {"task_id", state.task_id},
                            {"user_id", input.user_id},
                            {"conversation_id", input.conversation_id}});
    observability::EbpfMonitor monitor(observability::EbpfMonitor::from_environment(), state.run_id, run_store.run_dir());
    const nlohmann::json ebpf_started = monitor.start().to_json();
    append_ebpf_lifecycle_trace(run_store, ebpf_started);

    // --- Budget & trajectory setup ---
    TokenBudgetController budget(input.token_budget_limit);
    budget_ = &budget;
    tool_calls_.clear();

    ContextBuildOutput built = build_context(input);
    nlohmann::json metadata = built.metadata;
    metadata["cache_supported"] = model_.supports_prompt_cache();

    std::string final = "";
    for (int attempt = 1; attempt <= input.max_attempts; ++attempt) {
        state.attempts = attempt;
        run_store.append_trace({{"event", "model_requested"}, {"attempt", attempt}});
        try {
            final = model_.chat(built.prompt, input.user_message);

            // --- Budget: record prompt + completion tokens ---
            int prompt_tokens = static_cast<int>(built.prompt.size()) / 4;
            int completion_tokens = static_cast<int>(final.size()) / 4;
            budget_->record_usage(prompt_tokens, completion_tokens, 0);
            if (budget_->level() == BudgetLevel::BREAKER) {
                state.status = "stopped";
                state.stop_reason = "token_budget_breaker";
                break;
            }

        } catch (const std::exception& e) {
            state.status = "failed";
            state.stop_reason = sanitize_utf8(e.what());
            state.final_answer = "";
            run_store.append_trace({{"event", "run_finished"},
                                    {"status", state.status},
                                    {"stop_reason", state.stop_reason},
                                    {"attempts", state.attempts}});
            const nlohmann::json observability = finish_boundary_observability(run_store, monitor, ebpf_started);
            run_store.write_task_state(state);
            run_store.write_report({{"run_id", state.run_id},
                                    {"task_id", state.task_id},
                                    {"status", state.status},
                                    {"stop_reason", state.stop_reason},
                                    {"final_answer", state.final_answer},
                                    {"user_id", input.user_id},
                                    {"attempts", state.attempts},
                                    {"tool_steps", state.tool_steps},
                                    {"prompt_metadata", metadata},
                                    {"ebpf_observability", observability["ebpf"]},
                                    {"boundary_observability", observability["boundary"]}});
            throw;
        }
        run_store.append_trace(model_observability_event(attempt, false, input.user_message, built.prompt, final));
        metadata["model_metadata"] = model_.last_completion_metadata();
        if (!final.empty()) {
            state.status = "completed";
            state.stop_reason = "final_answer_returned";
            break;
        }
    }
    if (state.status != "completed") {
        state.status = "stopped";
        state.stop_reason = "retry_limit_reached";
    }
    state.final_answer = sanitize_utf8(final);

    // --- Trajectory analysis ---
    TrajectoryResult traj = trajectory_.analyze(tool_calls_, input.intent);
    run_store.append_trace({{"event", "trajectory_analyzed"},
                            {"loop_detected", traj.loop_detected},
                            {"efficiency_score", traj.efficiency_score},
                            {"tool_call_count", traj.tool_call_count}});

    run_store.append_trace({{"event", "run_finished"},
                            {"status", state.status},
                            {"stop_reason", state.stop_reason},
                            {"attempts", state.attempts}});
    const nlohmann::json observability = finish_boundary_observability(run_store, monitor, ebpf_started);
    run_store.write_task_state(state);
    run_store.write_report({{"run_id", state.run_id},
                            {"task_id", state.task_id},
                            {"status", state.status},
                            {"stop_reason", state.stop_reason},
                            {"final_answer", state.final_answer},
                            {"user_id", input.user_id},
                            {"attempts", state.attempts},
                            {"tool_steps", state.tool_steps},
                            {"prompt_metadata", metadata},
                            {"ebpf_observability", observability["ebpf"]},
                            {"boundary_observability", observability["boundary"]}});
    session_store.save(input.session_id.empty() ? "default" : input.session_id,
                       input.conversation_id.empty() ? "default" : input.conversation_id,
                       nlohmann::json::array({{{"role", "user"}, {"content", sanitize_utf8(input.user_message)}},
                                              {{"role", "assistant"}, {"content", state.final_answer}}}),
                       ConversationMemory{});

    RunResult result{state, state.final_answer, metadata, state.run_id, run_store.run_dir()};
    result.token_budget_used = budget.consumed();
    result.token_budget_limit = budget.total();
    result.budget_level_reached = [&]() -> std::string {
        switch (budget.level()) {
            case BudgetLevel::GREEN:  return "GREEN";
            case BudgetLevel::YELLOW: return "YELLOW";
            case BudgetLevel::RED:    return "RED";
            case BudgetLevel::BREAKER: return "BREAKER";
        }
        return "GREEN";
    }();
    result.trajectory_efficiency_score = traj.efficiency_score;
    result.trajectory_loop_detected = traj.loop_detected;
    return result;
}

AgentLoop::RunResult AgentLoop::run_stream(
    const RunInput& input,
    const std::function<bool(const nlohmann::json& event)>& on_event) {
    RunStore run_store(".mindbridge");
    SessionStore session_store(".mindbridge");
    TaskState state;
    state.task_id = input.task_id.empty() ? "mindbridge_task" : input.task_id;
    state.run_id = run_store.start_run(state.task_id);
    run_store.write_task_state(state);
    run_store.append_trace({{"event", "run_started"},
                            {"run_id", state.run_id},
                            {"task_id", state.task_id},
                            {"user_id", input.user_id},
                            {"conversation_id", input.conversation_id}});
    observability::EbpfMonitor monitor(observability::EbpfMonitor::from_environment(), state.run_id, run_store.run_dir());
    const nlohmann::json ebpf_started = monitor.start().to_json();
    append_ebpf_lifecycle_trace(run_store, ebpf_started);

    // --- Budget & trajectory setup ---
    TokenBudgetController budget(input.token_budget_limit);
    budget_ = &budget;
    tool_calls_.clear();

    ContextBuildOutput built = build_context(input);
    nlohmann::json metadata = built.metadata;
    metadata["cache_supported"] = model_.supports_prompt_cache();

    auto emit = [&](nlohmann::json event) {
        event["run_id"] = state.run_id;
        return !on_event || on_event(event);
    };
    emit({{"event", "run_started"},
          {"task_id", state.task_id},
          {"conversation_id", input.conversation_id},
          {"prompt_metadata", metadata}});

    std::string final;
    state.attempts = 1;
    run_store.append_trace({{"event", "model_requested"}, {"attempt", state.attempts}, {"stream", true}});
    try {
        model_.chat_stream(built.prompt, input.user_message, [&](const std::string& chunk) {
            final += chunk;
            return emit({{"event", "token"}, {"text", chunk}});
        });

        // --- Budget: record prompt + completion tokens ---
        int prompt_tokens = static_cast<int>(built.prompt.size()) / 4;
        int completion_tokens = static_cast<int>(final.size()) / 4;
        budget_->record_usage(prompt_tokens, completion_tokens, 0);

        run_store.append_trace(model_observability_event(state.attempts, true, input.user_message, built.prompt, final));
        metadata["model_metadata"] = model_.last_completion_metadata();
        state.status = final.empty() ? "stopped" : "completed";
        state.stop_reason = final.empty() ? "empty_stream" : "final_answer_returned";
    } catch (const std::exception& e) {
        state.status = "failed";
        state.stop_reason = sanitize_utf8(e.what());
        state.final_answer = "";
        run_store.append_trace({{"event", "run_finished"},
                                {"status", state.status},
                                {"stop_reason", state.stop_reason},
                                {"attempts", state.attempts}});
        const nlohmann::json observability = finish_boundary_observability(run_store, monitor, ebpf_started);
        run_store.write_task_state(state);
        run_store.write_report({{"run_id", state.run_id},
                                {"task_id", state.task_id},
                                {"status", state.status},
                                {"stop_reason", state.stop_reason},
                                {"final_answer", state.final_answer},
                                {"user_id", input.user_id},
                                {"attempts", state.attempts},
                                {"tool_steps", state.tool_steps},
                                {"prompt_metadata", metadata},
                                {"ebpf_observability", observability["ebpf"]},
                                {"boundary_observability", observability["boundary"]}});
        emit({{"event", "error"}, {"message", state.stop_reason}});
        throw;
    }

    state.final_answer = sanitize_utf8(final);

    // --- Trajectory analysis ---
    TrajectoryResult traj = trajectory_.analyze(tool_calls_, input.intent);
    run_store.append_trace({{"event", "trajectory_analyzed"},
                            {"loop_detected", traj.loop_detected},
                            {"efficiency_score", traj.efficiency_score},
                            {"tool_call_count", traj.tool_call_count}});

    run_store.append_trace({{"event", "run_finished"},
                            {"status", state.status},
                            {"stop_reason", state.stop_reason},
                            {"attempts", state.attempts}});
    const nlohmann::json observability = finish_boundary_observability(run_store, monitor, ebpf_started);
    run_store.write_task_state(state);
    run_store.write_report({{"run_id", state.run_id},
                            {"task_id", state.task_id},
                            {"status", state.status},
                            {"stop_reason", state.stop_reason},
                            {"final_answer", state.final_answer},
                            {"user_id", input.user_id},
                            {"attempts", state.attempts},
                            {"tool_steps", state.tool_steps},
                            {"prompt_metadata", metadata},
                            {"ebpf_observability", observability["ebpf"]},
                            {"boundary_observability", observability["boundary"]}});
    session_store.save(input.session_id.empty() ? "default" : input.session_id,
                       input.conversation_id.empty() ? "default" : input.conversation_id,
                       nlohmann::json::array({{{"role", "user"}, {"content", sanitize_utf8(input.user_message)}},
                                              {{"role", "assistant"}, {"content", state.final_answer}}}),
                       ConversationMemory{});
    emit({{"event", "message_done"},
          {"text", state.final_answer},
          {"status", state.status},
          {"stop_reason", state.stop_reason},
          {"prompt_metadata", metadata}});
    emit({{"event", "run_finished"},
          {"status", state.status},
          {"stop_reason", state.stop_reason},
          {"attempts", state.attempts}});

    RunResult result{state, state.final_answer, metadata, state.run_id, run_store.run_dir()};
    result.token_budget_used = budget.consumed();
    result.token_budget_limit = budget.total();
    result.budget_level_reached = [&]() -> std::string {
        switch (budget.level()) {
            case BudgetLevel::GREEN:  return "GREEN";
            case BudgetLevel::YELLOW: return "YELLOW";
            case BudgetLevel::RED:    return "RED";
            case BudgetLevel::BREAKER: return "BREAKER";
        }
        return "GREEN";
    }();
    result.trajectory_efficiency_score = traj.efficiency_score;
    result.trajectory_loop_detected = traj.loop_detected;
    return result;
}

}  // namespace mindbridge
