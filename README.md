# MindBridge Project

本目录聚合 A2A 大仓库里 **MindBridge 工业级 C++ 运行时** 及其构建所需子树，便于与 `OpenHarness/`、`pico-hardness/` 等参考项目区分开。

完整路径说明见 [`REPO_MAP.md`](REPO_MAP.md)。

## What Is Included

- `mindbridge_harness/`: main industrial C++ multi-agent runtime.
- `include/`, `src/`: legacy/general `agent_rpc_framework` layer kept with the project. It is not part of the default build; enable it with `MINDBRIDGE_BUILD_LEGACY_RPC_FRAMEWORK=ON` when you want to repair or inspect that older framework target.
- `server/`, `client/`: gRPC RPC entrypoint and client for the full communication path.
- `tests/`: optional tests enabled with `BUILD_TESTING=ON`.
- `examples/mindcare/`: runnable regression demo that must stay compatible.
- `docs/`: architecture, engineering, and model strategy notes.
- `scripts/verify_mindbridge.sh`: required verification entrypoint.
- `integrations/mcp_server_integrated/`: MCP 服务端参考（Excel/Email 等），对应环境变量 `MINDBRIDGE_MCP_EXCEL_URL` / `MINDBRIDGE_MCP_EMAIL_URL`。
- `a2a/`, `a2a_adapter/`, `orchestrator/`, `proto/`, `common/`, `mcp/`: local C++ dependencies needed by the MindBridge harness and MindCare demo.

External reference projects from the original workspace, such as `OpenHarness/`, `agentscope/`, `deer-flow`, `AutoAI-Coding/`, and `pico-hardness/`, are intentionally not included.

Optional at repo root (not bundled here): `client/` (generic A2A gRPC client), `MindBridge智能体项目/` (legacy/reference).

## Verify

From this directory:

```bash
bash scripts/verify_mindbridge.sh
```

The main verifier also runs the Platform frontend wiring and browser smoke when
Node/Playwright are available. To exercise only the browser-visible platform
console:

```bash
bash scripts/verify_platform_browser_smoke.sh
```

Manual build:

```bash
cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build --target mindbridge_harness mindbridge_gateway mindbridge_orchestrator mindbridge_counselor mindbridge_evaluator mindbridge_benchmark rpc_server rpc_client ai_counselor_agent ai_evaluator_agent -j2
./build/mindbridge_harness/mindbridge_benchmark
```

API keys must be supplied through environment variables only, for example `MINDBRIDGE_MODEL_API_KEY`.
For the OpenAI-compatible demo path, you can run `scripts/start_demo_openai.sh`; it reads the API key with hidden input and does not save it.

## Main Runtime Flow

```text
Gateway -> Orchestrator -> Counselor
                         -> Evaluator for high-risk requests
                         -> MCP tools through ToolRegistry
```

The orchestrator performs lightweight intent/risk routing before forwarding to the counselor. High-risk requests call the evaluator first; evaluator results are merged into the counselor response under `result.orchestration`.
