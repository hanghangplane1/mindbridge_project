## Context

MindBridge 当前 counselor 处理流程（counselor_main.cpp handle()）：
1. `assess_risk` 工具调用 → 返回 intent 和 riskLevel（一次 LLM 调用）
2. 如果 CONSULT/RISK → `retrieve_knowledge` 用原始用户消息检索（一次 embedding + ChromaDB 查询）
3. 读取对话历史
4. 构建 system_prompt（PromptPolicy::counselor_prompt）
5. `model.chat` 生成回答（一次 LLM 调用）

问题：步骤 1 和 5 是两次独立的 LLM 调用，各自有独立的 prompt tokens 开销。步骤 2 用口语化原始消息做检索，跟知识库专业术语匹配度低。步骤 5 后没有验证，无法纠错。

## Goals / Non-Goals

**Goals:**
- 合并意图分类+query改写+检索决策为单次 LLM 调用
- 用改写后的 search_query 做 RAG 检索，提升召回率
- 新增响应验证层，验证失败时用更宽泛的 query 重新检索并重新生成
- 保持 RISK 场景的安全性（不压缩上下文、不丢失危机信号）

**Non-Goals:**
- 不改变现有的 intent 分类（CHAT/CONSULT/RISK）
- 不改变 RAG 入库管线（knowledge_ingest 工具不变）
- 不改变 TokenBudget / TrajectoryAnalyzer / MemoryScorer 等已集成组件
- 不引入 LLM 做验证（用规则验证，避免额外调用开销）

## Decisions

### D1: Agentic Reasoning Prompt — 单次调用替代 assess_risk

**选择**：新增 `agentic_reasoning_prompt()`，一次 LLM 调用输出：
```json
{
  "intent": "CHAT|CONSULT|RISK",
  "rewritten_query": "改写后的查询文本",
  "thought": "推理思考过程",
  "action": "RETRIEVE|ANSWER",
  "search_query": "检索关键词"
}
```

**替代方案**：
- 保留 assess_risk 工具调用 + 单独的 query rewriter：两次调用，不省钱
- 用规则做意图分类，LLM 只做 query 改写：规则覆盖不全，中文语境下准确率低

**理由**：Mental-Health-Assistant 的实践证明，意图分类、query 改写、检索决策三个任务可以共享同一个 prompt context，一次调用完成。qwen3.6-flash 的 JSON 遵从能力足够支持这种结构化输出。

### D2: RAG 检索用 search_query 而非原始消息

**选择**：`retrieve_knowledge` 工具调用的 query 参数从原始用户消息改为 agentic_reasoning 输出的 `search_query`。

**理由**：search_query 是 LLM 从用户口语中提取的核心关键词（如"失眠 焦虑 放松"），比原始消息（"我最近压力好大，晚上翻来覆去睡不着"）更接近知识库的专业术语，embedding 相似度更高。

### D3: Validation-Retry Loop — 规则验证 + 重新检索

**选择**：新增 `ResponseValidator` 类，规则验证包括：
- 回复长度 >= 15 字符
- 有 RAG 上下文时，回复必须包含至少一个关键术语（从上下文中提取标题和加粗文本）
- 不包含有害模式（"你可以去死"、"放弃吧"、"没救了"等）
- 危机回复必须包含热线号（400-161-9995、12355）

验证失败 → 用 `rewritten_query`（更宽泛）重新检索 → 重新生成。最多重试 1 次。

**替代方案**：
- LLM 做验证（RESPONSE_VALIDATOR prompt）：多一次 LLM 调用，成本高
- 不验证：简单但不安全

**理由**：规则验证零成本（纯字符串匹配），覆盖了最关键的有害内容和危机热线号检查。

### D4: Intent-aware RAG 触发 — CONSULT 和 RISK 走 RAG，CHAT 不走

**选择**：CONSULT 和 RISK 意图走 RAG 检索。CHAT 不检索。

**理由**：CHAT 不需要知识库（闲聊）。RISK 走 RAG 是因为知识库中的 `crisis_intervention.md` 包含危机干预五步法、"不该做的"清单、转介标准等专业指导，检索这些内容能提升危机响应质量。2 秒检索延迟换来更专业的危机干预，值得。这与 Mental-Health-Assistant 的设计不同——他们用硬编码 CRISIS_GUIDE prompt，而我们用 RAG 动态获取更丰富的危机指导。

## Risks / Trade-offs

- **[JSON 解析失败]** → agentic_reasoning 要求 LLM 输出严格 JSON，qwen3.6-flash 偶尔会输出非 JSON。缓解：解析失败时 fallback 到规则意图分类（RiskPolicy::heuristic_intent），search_query 默认为原始消息。
- **[验证过于严格]** → 关键术语匹配可能导致合法回答被误判为失败。缓解：关键术语提取只取标题和加粗文本，匹配用子串包含而非精确匹配。
- **[重试增加延迟]** → 验证失败触发重新检索+重新生成，增加一次 embedding 查询和 LLM 调用。缓解：最多重试 1 次，且只在有 RAG 上下文时重试。
