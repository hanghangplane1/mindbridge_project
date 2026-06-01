# MindCare 双 Agent Demo（保留）

本目录为 **已调通** 的参考实现：`ai_counselor_agent`（RAG + Ollama + 异步 evaluator）、`ai_evaluator_agent`（风险 JSON + MCP）、`ai_mindcare_client`。

## 与 MindBridge Harness 的关系

- 工程化能力与可复用模块见仓库根目录 [`mindbridge_harness/`](../mindbridge_harness/README.md)。
- `counselor_agent_main.cpp` 已接入 `mindbridge::RegistryAgentRouter`，在注册中心有多实例时由 `AgentRouter` 先按 skill/tag 过滤，再按一致性哈希做 `contextId` 级稳定路由；单机单 evaluator 时退化为直接命中唯一实例，行为与原先按 tag 轮询一致。

## LoRA 训练

见 [lora/README.md](lora/README.md)。
