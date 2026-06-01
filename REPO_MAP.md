# MindBridge 在 A2A 仓库中的代码地图

本目录 `mindbridge_project/` 是仓库里 **MindBridge 工业级 C++ 运行时** 的聚合副本，便于在杂项目混放的 monorepo 中单独打开、构建与评审。

## 已纳入本目录（与仓库根目录对应路径应保持一致）

| 路径 | 说明 |
|------|------|
| `mindbridge_harness/` | 核心：AgentLoop、ToolRegistry、RunStore、Benchmark、Gateway/Counselor/Evaluator 等 |
| `examples/mindcare/` | 回归演示（与 `AGENTS.md` 要求一致） |
| `docs/` | 架构、harness 工程、模型策略、上下文管理等 |
| `AGENTS.md` | Agent 工作指引（与根目录副本一致） |
| `scripts/verify_mindbridge.sh` | 统一验证入口 |
| `.agent/skills/mindbridge-smoke-test/` | 冒烟测试技能说明 |
| `include/`、`src/` | 早期/通用 `agent_rpc_framework` 封装：RpcFramework、RpcServer、RpcClient、注册、负载均衡、熔断、日志、指标；已纳入项目档案，但默认不构建，需 `MINDBRIDGE_BUILD_LEGACY_RPC_FRAMEWORK=ON` |
| `tests/` | A2A、proto、MCP、RPC 等测试；默认构建关闭，`BUILD_TESTING=ON` 时启用 |
| `server/`、`client/` | gRPC RPC 入口与测试客户端；完整链路为 `rpc_client -> rpc_server -> A2A/HTTP -> Orchestrator -> Agents` |
| `a2a/`、`a2a_adapter/`、`common/`、`mcp/`、`orchestrator/`、`proto/` | CMake 中 `mindbridge_harness` / gRPC RPC / MindCare 所需的 C++ 依赖子树 |
| `integrations/mcp_server_integrated/` | 与 `MINDBRIDGE_MCP_EXCEL_URL` / `MINDBRIDGE_MCP_EMAIL_URL` 配套的 MCP 服务端参考实现（Excel/Email 插件等） |

构建时若使用本目录内 `.deps/` 中的本地 gRPC/protobuf，与在仓库根目录构建行为一致；**`.deps/`、`build/` 不应提交到版本库**（见 `.gitignore`）。

## 未纳入本目录、但与 MindBridge 相关的仓库根路径（按需查阅）

| 路径 | 说明 |
|------|------|
| 仓库根 `CMakeLists.txt` | 顶层工程入口；本目录有独立 `CMakeLists.txt` 可单独配置 |
| `MindBridge智能体项目/` | 历史 Java/参考材料，非当前 C++ 运行时源码 |
| `mcp_server_integrated/`（根目录） | 与 `integrations/mcp_server_integrated/` 为同内容；根目录为原位置，本目录为聚合副本 |

## 与根目录保持同步（开发者在根目录改代码时）

若在仓库根修改了 `mindbridge_harness/`、`docs/`、`scripts/verify_mindbridge.sh` 等，请同步到本目录（或反过来），避免两套漂移。示例：

```bash
# 在仓库根目录 a2a/ 下执行，将根目录改动同步到 mindbridge_project/
rsync -a --delete ./mindbridge_harness/ ./mindbridge_project/mindbridge_harness/
rsync -a ./docs/ ./mindbridge_project/docs/
rsync -a ./AGENTS.md ./mindbridge_project/AGENTS.md
rsync -a ./scripts/verify_mindbridge.sh ./mindbridge_project/scripts/
rsync -a --delete ./include/ ./mindbridge_project/include/
rsync -a --delete ./src/ ./mindbridge_project/src/
rsync -a --delete ./tests/ ./mindbridge_project/tests/
```

如需同步 MCP 集成服务端：`rsync -a --delete ./mcp_server_integrated/ ./mindbridge_project/integrations/mcp_server_integrated/`。

## 明确不属于 MindBridge C++ 运行时的目录（勿混入本聚合包）

`OpenHarness/`、`agentscope/`、`deer-flow/`、`AutoAI-Coding/`、`pico-hardness/` 等为参考或实验子项目，**不必**复制进 `mindbridge_project/`。
