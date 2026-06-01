## 1. Agentic Reasoning Prompt

- [ ] 1.1 在 `prompt_policy.cpp` 新增 `agentic_reasoning_prompt()` 函数，返回 prompt 文本：要求 LLM 同时完成意图分类、query 改写、检索决策，输出 JSON（intent / rewritten_query / thought / action / search_query）
- [ ] 1.2 在 `prompt_policy.hpp` 中声明 `agentic_reasoning_prompt()` 函数
- [ ] 1.3 在 `prompt_policy.cpp` 新增 `generate_answer_prompt()` 函数，接受 emotion_label、risk_level、context、query 参数，返回基于 RAG 上下文生成回答的 prompt
- [ ] 1.4 在 `prompt_policy.hpp` 中声明 `generate_answer_prompt()` 函数

## 2. ResponseValidator 模块

- [ ] 2.1 新增 `include/mindbridge/harness/response_validator.hpp`：定义 ValidationResult 结构体（is_valid、reason、issues）和 ResponseValidator 类
- [ ] 2.2 实现 `validate(query, response, rag_context)` 方法：检查长度 >= 15、有害模式匹配、危机热线号、RAG 关键术语覆盖
- [ ] 2.3 实现 `extract_key_terms(context)` 方法：从 RAG 上下文中提取关键术语（## 标题、**加粗** 文本）
- [ ] 2.4 在 `mindbridge_harness/CMakeLists.txt` 中添加 `src/harness/response_validator.cpp`
- [ ] 2.5 为 ResponseValidator 编写单元测试

## 3. Counselor 流程重构

- [ ] 3.1 重构 `counselor_main.cpp` handle() 方法：将 `assess_risk` 工具调用替换为 `model_.chat(agentic_reasoning_prompt(), user_text)` + JSON 解析
- [ ] 3.2 实现 JSON 解析逻辑：从 LLM 输出中提取 intent、rewritten_query、thought、action、search_query，解析失败时 fallback 到 RiskPolicy::heuristic_intent()
- [ ] 3.3 改造 retrieve_knowledge 调用：将 query 参数从原始 user_text 改为 search_query（来自 agentic_reasoning 输出）
- [ ] 3.4 实现 intent-aware RAG 触发：intent==CONSULT 或 intent==RISK 且 action==RETRIEVE 时调用 retrieve_knowledge，CHAT 不调用
- [ ] 3.5 将 model.chat 调用的 prompt 从 `counselor_prompt()` 改为 `generate_answer_prompt()`，注入 emotion_label、risk_level、RAG context、query
- [ ] 3.6 集成 ResponseValidator：在 model.chat 返回后调用 validate()，验证失败时用 rewritten_query 重新检索并重新生成（最多 1 次）
- [ ] 3.7 将 agentic_reasoning 的 thought 字段写入 trace（agentic_reasoning 事件）

## 4. 测试验证

- [ ] 4.1 端到端测试 CONSULT 场景：发送"我最近压力很大，失眠"，验证 RAG 检索使用改写后的 query
- [ ] 4.2 端到端测试 CHAT 场景：发送"今天天气不错"，验证不触发 RAG 检索
- [ ] 4.3 端到端测试 RISK 场景：发送"活着没意思"，验证走 RAG 检索（crisis_intervention.md）+ 危机流程
- [ ] 4.4 测试 validation-retry：构造一个会导致验证失败的场景，验证 retry 逻辑生效
- [ ] 4.5 测试 JSON 解析失败 fallback：mock LLM 返回非 JSON，验证 fallback 到规则分类
