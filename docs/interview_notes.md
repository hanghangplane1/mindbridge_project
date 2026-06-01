# MindBridge 面试拷问答题卡

## 1) 你的项目里体现了哪些 Harness Engineering 思想

可回答要点：
- PromptPolicy 分场景约束（CHAT/CONSULT/RISK/EVALUATOR）
- ToolRegistry 统一外部动作入口
- PermissionChecker 做工具权限治理
- Hook + Trace 提供可观测与复盘
- RuntimeContext + Memory 做上下文工程
- BenchmarkRunner + HardnessScorer 做持续评测

## 2) 单 Agent 还是多 Agent

多 Agent：
- Gateway：统一入口
- Orchestrator：编排入口
- Counselor：咨询主链路
- Evaluator：风险评估兜底

注意：MCP 是工具协议层，不是独立 ToolAgent。

## 3) 如何约束幻觉

- PromptPolicy 约束回复边界
- RAG 提供领域证据
- 高风险交给 Evaluator 二次判断
- MCP 动作必须经工具注册和权限检查

## 4) 召回流程怎么讲

输入 -> embedding -> Chroma query -> topK contexts -> PromptPolicy 注入 -> 模型生成。  
仅 CONSULT/RISK 走 RAG，CHAT 走轻链路降低延迟。

## 5) 上下文工程怎么做

- 结构化 RuntimeContext
- ConversationMemory 只取最近 N 条
- 工具结果和风险状态显式注入
- 避免超长上下文污染输出

## 6) 哪些问题不适合硬答

以下更偏代码生成 Agent，不是本项目主线：
- 分支覆盖率插桩
- AST/LSP 自动单测生成
- 代码级 mock 生成策略

回答策略：明确项目边界，并给出可借鉴点（评测体系、自动验证流程）。

## 7) 还可继续补强的方向

- Orchestrator 策略深化（健康、负载、熔断）
- benchmark 接入真实标注集
- MCP 调用可观测性与重试策略增强
- 统一鉴权（JWT/RBAC）与压测报告

## 8) 面试一句话总结模板

“我们不是裸调用 LLM，而是把模型放进 C++ Harness Runtime：用 PromptPolicy 约束输入，用 ToolRegistry 和 PermissionChecker 约束动作，用 Evaluator 约束风险，用 Trace 和 Benchmark 约束质量，形成可治理的多 Agent 工程系统。”
