## Why

当前 MindBridge 的 RAG 流程存在三个效率和质量问题：

1. **两次 LLM 调用浪费 token**：先调 `assess_risk` 工具做意图分类，再调 `model.chat` 生成回答。两次调用各自消耗独立的 prompt tokens，而意图分类和回答生成可以合并为一次调用。
2. **RAG 检索召回率低**：`retrieve_knowledge` 直接用用户的口语化消息做 embedding 检索（如"翻来覆去睡不着"），跟知识库中的专业术语（"失眠"、"4-7-8 呼吸法"）语义距离大，导致召回效果差。
3. **无响应质量验证**：模型直接输出回答，没有验证机制。可能出现回答过短、包含有害内容、危机场景缺少热线号等问题，且无法自我纠错。

参考 Mental-Health-Assistant 的 Agentic RAG 模式，通过合并推理、query 改写、验证重试三个机制解决上述问题。

## What Changes

- **新增 agentic_reasoning prompt**：单次 LLM 调用同时完成意图分类、query 改写、检索决策，输出结构化 JSON（intent / rewritten_query / thought / action / search_query），替代当前的 `assess_risk` 工具调用。
- **新增 generate_answer prompt**：基于 RAG 上下文生成回答的专用 prompt，注入情绪标签、风险等级、检索到的知识片段。
- **改造 counselor_main.cpp handle() 流程**：assess_risk 工具调用 → agentic_reasoning 单次调用；retrieve_knowledge 用 search_query 而非原始消息；新增 validation-retry 循环。
- **新增 ResponseValidator**：规则验证模块，检查回答长度、关键术语覆盖、有害模式、危机热线号，验证失败时触发重新检索和重新生成。

## Capabilities

### New Capabilities
- `agentic-rag-reasoning`: 合并意图分类+query改写+检索决策的单次 LLM 调用 prompt 和解析逻辑
- `response-validation`: 回答质量规则验证（长度、关键术语、有害模式、热线号），验证失败触发 retry

### Modified Capabilities
- 无现有 spec 需要修改（openspec/specs/ 为空）

## Impact

- **代码变更**：`counselor_main.cpp`（handle 流程重构）、`prompt_policy.cpp`（新增 2 个 prompt）、新增 `response_validator.hpp/cpp`
- **LLM 调用次数**：CONSULT 场景从 2 次（assess_risk + model.chat）降为 1-2 次（agentic_reasoning + 可选 retry）
- **RAG 召回率**：预期提升（用改写后的关键词检索 vs 用口语化原文检索）
- **响应安全性**：新增规则验证层，覆盖有害模式和危机热线号检查
