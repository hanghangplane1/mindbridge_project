## Why

MindBridge 已覆盖腾讯 agentic 架构 28 项要求中的 15 项（完全实现）和 14 项（部分实现），但仍有 7 项缺失。这些缺失涉及成本控制、质量评估和记忆管理三个关键维度——在生产环境中，不受控的 token 消费会直接产生费用风险，缺乏轨迹评估会掩盖低效的 agent 行为，而 FIFO 淘汰会丢失高价值记忆。补齐这些短板是 MindBridge 从"能跑"到"能上线"的必要条件。

**前置依赖**：项目当前 RAG 链路未激活（ChromaDB 环境变量未设置、无知识入库管线、启动脚本不拉起 Chroma），导致 `retrieve_knowledge` 工具始终返回空结果。memory-scoring 的语义相似度因子和 token-budget 的 RAG 上下文压缩均依赖可用的 embedding/RAG 链路。因此本次 change 需要将 RAG 激活作为前置任务一并纳入。

## What Changes

经过逐项评估，7 项缺失分为三档：

**应当实现（5 项）：**

- **实时 Token Budget**：新增 `TokenBudgetController`，在 AgentLoop 每步执行后累计 token 消耗，超阈值时触发降级或终止。解决当前只有 `max_chars` 静态截断、无跨 run 累计预算的问题。
- **预算分级（绿/黄/红/熔断）**：作为 TokenBudget 的一部分实现，定义四级阈值（<60% 绿色正常 / 60-80% 黄色警告 / 80-95% 红色仅允许关键工具 / >95% 熔断终止），替代当前服务级 circuit_breaker 的粗粒度控制。**黄色级别的降级策略按 intent 区分**：CHAT 意图可压缩上下文（截断历史、精简 RAG），RISK 意图绝不压缩（保留完整上下文以防丢失危机信号），仅记录警告。
- **模型路由**：在 ModelClient 之上新增 `ModelRouter`，根据 PromptPolicy 的意图分类（CHAT/CONSULT/RISK）+ 复杂度评分选择不同模型（轻量模型处理闲聊、标准模型处理咨询、强模型处理风险评估），降低 40-60% 常规请求的推理成本。
- **MCP Server 独立配额**：在 ToolRegistry 中为每个 MCP Server 声明 `rate_limit`（每分钟调用次数）和 `cost_budget`（token 上限），超限时排队或拒绝，防止单一外部服务耗尽系统资源。
- **记忆评分遗忘**：替代 ConversationMemory 的纯 FIFO 淘汰，引入 `MemoryScorer` 基于 recency（时间衰减）× relevance（语义相似度）× importance（风险等级/情绪标记）综合评分，淘汰低分记忆，保留高价值上下文。

**可以不做（1 项）：**

- **声明式执行计划（DAG/步骤列表）**：MindBridge 是对话式 agent（心理咨询/合规咨询/文档审计），任务天然是单轮对话 + 工具调用的交互模式，不是多步骤流水线。当前 AgentLoop 的 `max_attempts` + `max_tool_steps` + duplicate-tool-call 检测已足够覆盖对话场景的执行控制。引入 DAG planner 会增加不必要的复杂度，与实际业务模式不匹配。**如未来需要支持多步骤文档审计流程，可再评估。**

**可简化实现（1 项）：**

- **轨迹评估（Trajectory Eval）**：不需要完整的 Trajectory Eval 框架。在 trace.jsonl 已有全链路事件的基础上，新增 `TrajectoryAnalyzer` 做三件事即可：(1) 循环检测（连续相同 tool_call 超过阈值报警），(2) 步骤效率评分（完成任务的 tool 调用次数 vs 理论最小值），(3) 异常模式标记（超时、频繁失败、不必要的工具调用）。结果写入 trace 的 `run_finished` 事件中，不需要独立的评估 pipeline。

## Capabilities

### New Capabilities
- `rag-activation`: 激活 RAG 链路——知识入库管线（文档读取→切片→embedding→ChromaDB upsert）、ChromaDB 启动配置、embedding 链路验证。**前置任务**，memory-scoring 和 token-budget 依赖此能力。
- `token-budget`: 实时 token 预算控制，含预算分级（绿/黄/红/熔断），跨 run 累计追踪
- `model-routing`: 按意图分类 + 复杂度评分动态选择模型，支持成本/质量权衡
- `mcp-server-quota`: 每个 MCP Server 独立的调用频率限制和 token 配额
- `memory-scoring`: 基于 recency × relevance × importance 的记忆评分淘汰机制
- `trajectory-analysis`: 轻量级轨迹分析（循环检测、效率评分、异常标记）

### Modified Capabilities
- 无现有 spec 需要修改（openspec/specs/ 为空）

## Impact

- **代码变更**：主要涉及 `mindbridge_harness/` 下的 `agent_loop.cpp`（集成 TokenBudget 和 ModelRouter）、`conversation_memory.cpp`（替换 FIFO 为 MemoryScorer）、`tool_registry.cpp`（增加 MCP 配额检查）、新增知识入库工具和 5 个头文件/实现文件
- **API 变更**：RunInput 新增 `token_budget_limit` 可选字段；ToolRegistry 新增 `mcp_quota` 配置；ModelClient 接口不变，ModelRouter 是上层包装
- **依赖**：TokenBudget 需要 tokenizer 库（tiktoken-cpp 或类似）做精确 token 计数；MemoryScorer 和 RAG 需要 embedding 接口可用；RAG 需要 ChromaDB 实例部署
- **风险**：ModelRouter 引入多模型管理复杂度，需确保 fallback 逻辑健壮；MemoryScorer 的评分参数需要调优；RAG 激活需要用户提供知识数据并验证 embedding 链路
