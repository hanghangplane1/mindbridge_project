## Context

MindBridge 是面向心理健康咨询/合规/文档审计场景的 C++ 多 agent 运行时。当前架构已完成核心链路（Gateway → Orchestrator → Counselor/Evaluator → ToolRegistry），但在成本控制、质量评估和记忆管理三个方面存在空白。

现有关键约束：
- AgentLoop 是单轮对话 + 工具调用循环，不是多步骤流水线
- ModelClient 是纯虚接口，已有 Ollama/OpenAI/DashScope 三个实现
- ContextManager 用字符数（max_chars=12000）做上下文截断，不感知 token
- ConversationMemory 是纯内存存储，FIFO 无评分
- trace.jsonl 已覆盖全链路事件（run_started/tool_executed/run_finished）
- PromptPolicy 已做意图分类（CHAT/CONSULT/RISK）

## Goals / Non-Goals

**Goals:**
- 在 AgentLoop 中引入实时 token 预算控制，防止成本失控
- 根据意图分类动态选择模型，降低常规请求推理成本
- 为每个 MCP Server 设置独立配额，防止单点资源耗尽
- 用评分机制替代 FIFO 淘汰，保留高价值记忆
- 利用已有 trace 数据做轻量级轨迹分析

**Non-Goals:**
- 不引入 DAG/步骤列表执行计划（对话场景不需要）
- 不构建独立的评估 pipeline（结果直接写入 trace）
- 不实现跨 session 的分布式 token 预算（单 session 内控制即可）
- 不做模型自动扩缩容（模型路由是逻辑层，不涉及基础设施）

## Decisions

### D0: RAG 激活 — 补齐入库管线 + 启动配置，作为前置依赖

**选择**：新增知识入库 CLI 工具 + 修改启动脚本，将 RAG 从"编译了但没跑"变为端到端可用。

**当前断点分析**：
1. `ChromaVectorStore::query()` 已实现（HTTP POST 到 ChromaDB），但 `MINDBRIDGE_CHROMA_URL` 从未被设置 → `make_chroma()` 返回 `nullptr`
2. 没有知识入库代码：整个项目没有 document → chunk → embedding → ChromaDB upsert 的管线
3. 启动脚本不拉起 ChromaDB 实例

**实现方案**：
- 新增 `tools/knowledge_ingest` CLI：读取指定目录的文档，按 ~500 token 切片（50 token overlap），调用 `ModelClient::embedding()` 生成向量，POST 到 ChromaDB `/api/v1/collections/{name}/upsert`
- 修改 `scripts/start_demo.sh`：启动前检查 ChromaDB 可达性，不可达则用 docker 拉起；导出 `MINDBRIDGE_CHROMA_URL` 和 `MINDBRIDGE_CHROMA_COLLECTION`
- 启动时做一次 test embedding 验证链路可用

**替代方案**：
- 用 MCP 模块的 RAG-MCP（`mcp/src/rag/`）替代：它是工具检索系统，不是知识库检索，职责不同
- 不用 ChromaDB，用内存向量索引：`mcp/src/rag/vector_index.cpp` 可用，但不持久化，重启丢失

**理由**：ChromaVectorStore 代码已写好且编译通过，只差配置和入库。最小改动路径是补齐这两块，而非重写。

### D1: Token 计数方案 — 使用 tiktoken-cpp 做近似计数

**选择**：引入 tiktoken-cpp 库，对 prompt 和 completion 做 BPE token 计数。

**替代方案**：
- 字符数估算（当前方案）：不准确，中英文 token 密度差异大
- 调用模型 API 返回 usage：依赖模型提供商，部分本地模型不返回

**理由**：tiktoken-cpp 是纯 C++ 实现，无 Python 依赖，支持 cl100k_base 编码，对 GPT 系列和大部分中文模型误差可接受（±10%）。本地模型可用字符数回退。

### D2: TokenBudget 作为独立组件注入 AgentLoop

**选择**：新增 `TokenBudgetController` 类，通过 RunInput 注入 AgentLoop，每步执行后更新消耗。

**替代方案**：
- 内嵌到 AgentLoop：耦合度高，难以测试和复用
- 全局单例：不利于多 session 隔离

**理由**：组合优于继承。TokenBudgetController 是纯数据+策略组件，AgentLoop 通过接口查询预算状态，两者可独立测试。

### D3: 模型路由策略 — 基于规则表而非 ML 分类

**选择**：ModelRouter 维护一张 `intent → model_profile` 映射表，由配置文件驱动。

**替代方案**：
- 训练一个复杂度分类器：需要标注数据，冷启动问题
- 让 LLM 自己判断用哪个模型：元开销大，不可靠

**理由**：MindBridge 的意图分类（CHAT/CONSULT/RISK）已经由 PromptPolicy 完成，模型路由只需做查表映射。规则表可热更新，运维友好。

### D4: MCP 配额 — 令牌桶限流 + 滑动窗口计数

**选择**：每个 MCP Server 配置 `rate_limit`（token bucket，每秒 N 次）和 `daily_budget`（滑动窗口 24h token 上限）。

**替代方案**：
- 固定间隔限流：突发流量处理差
- 仅超时控制（当前方案）：无法防止单位时间内的高频调用

**理由**：令牌桶允许合理的突发流量，滑动窗口防止日累计超标。两者组合覆盖短期和长期限流需求。

### D5: 记忆评分 — 三因子乘积模型

**选择**：`score = recency(t) × relevance(q, m) × importance(meta)`，其中：
- `recency(t) = exp(-λ × age_hours)`，λ=0.05（约 20 小时半衰期）
- `relevance(q, m) = cosine_similarity(embedding(q), embedding(m))`，复用已有 embedding 接口
- `importance(meta)`：risk_level=RISK 时 ×1.5，含情绪标记 ×1.3，普通 ×1.0

**替代方案**：
- LLM 评分：每次淘汰都要调模型，成本高
- 纯时间衰减：丢失重要性维度

**理由**：三因子模型计算量小（embedding 已有缓存），可解释性强，参数可调。recency 保证近期对话不丢，relevance 保证语义相关记忆保留，importance 保证高风险上下文不被遗忘。

### D6: 预算降级策略 — 按 intent 区分，RISK 不压缩

**选择**：Yellow 级别的降级行为按 PromptPolicy 意图分类差异化处理：
- **CHAT**：触发完整上下文压缩（截断 oldest history、精简 RAG contexts、降低 max_tokens）
- **CONSULT**：仅截断 oldest history，保留 RAG contexts（合规查询需要完整知识库上下文）
- **RISK**：**不压缩任何上下文**，仅记录 budget_warning 事件到 trace。高风险用户的心理危机信号可能分布在任意历史片段中，压缩上下文存在丢失关键信息的安全风险。

**替代方案**：
- 统一压缩所有意图：简单但对 RISK 场景危险
- RISK 意图直接跳到 Red 级别策略：过于激进，RISK 对话可能很长但仍有预算空间

**理由**：心理咨询场景中，高风险用户的每一句对话都可能是评估其心理状态的关键证据。宁可多消耗 token 也不丢失上下文。成本控制应通过模型路由（RISK 用强模型但控制调用频次）和 MCP 配额来实现，而非压缩高风险对话的上下文。

### D7: 轨迹分析 — 写入 trace 而非独立存储

**选择**：TrajectoryAnalyzer 在 run_finished 时计算三个指标，写入 trace.jsonl 的 finish 事件。

**替代方案**：
- 独立评估数据库：增加存储和查询复杂度
- 实时分析每一步：性能开销大

**理由**：trace.jsonl 已是不可变审计日志，轨迹分析结果作为 finish 事件的扩展字段即可。离线分析时一行 jq 命令就能提取。

## Risks / Trade-offs

- **[tiktoken-cpp 精度]** → 对非 GPT 模型（如 Qwen、GLM）token 计数有误差。缓解：设置 10% 安全余量，且提供字符数回退模式。
- **[MemoryScorer embedding 开销]** → 每次评分需要计算 embedding。缓解：复用 ConversationMemory 已有的 knowledge_summaries embedding 缓存，仅对新增记忆计算。
- **[ModelRouter fallback]** → 目标模型不可用时需要 fallback。缓解：每个 model_profile 配置 primary + fallback 模型，ModelRouter 自动降级。
- **[MCP 配额误杀]** → 合理的突发调用可能被限流。缓解：令牌桶允许短暂突发，且限流时返回排队而非直接拒绝。
- **[评分参数调优]** → λ、importance 权重等参数需要根据实际场景调整。缓解：参数外置到配置文件，支持运行时热更新。
- **[RAG 冷启动]** → 首次部署需要用户先入库知识数据，否则 RAG 链路虽通但无结果。缓解：提供示例知识文档和快速入库脚本；启动时检查 collection 是否为空并提示。
- **[Embedding 模型可用性]** → MemoryScorer 和 RAG 共用 embedding 接口，模型不可用时两者都退化。缓解：embedding 失败时 MemoryScorer 回退到纯 recency × importance，RAG 回退到 disabled 模式，均记录 trace 事件。
