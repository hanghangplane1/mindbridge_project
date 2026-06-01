# MindBridge

**MindBridge is an industrial C++ multi-agent runtime for governed mental-health agents.**

It is not only a chat demo. The project focuses on the engineering layer around LLM agents: service routing, tool governance, risk evaluation, persisted state, run artifacts, browser-visible QA, and optional eBPF boundary observability.

## Why This Project Matters

MindBridge is built to answer a practical question:

> How do you turn an LLM-powered mental-health assistant into a runtime that can be routed, audited, evaluated, persisted, and explained?

The strongest parts of the project are:

- **C++ multi-agent runtime**: Gateway, Orchestrator, Counselor, Evaluator, Benchmark, Platform, and demo frontend are wired as real services.
- **Governed tool execution**: tools go through `ToolRegistry -> PermissionChecker -> HookExecutor -> TraceRecorder`, instead of being called as untracked side effects.
- **Mental-health safety loop**: risk routing, evaluator fallback, session-end assessment, and MCP-style Excel/Email actions are integrated into the runtime path.
- **Layered persistence**: conversation state uses `user_id + conversation_id + namespace_name + state_key`; cloud storage uses MySQL, Redis, and FastDFS-style object metadata.
- **Observable runtime artifacts**: every run is expected to write `.mindbridge/runs/<run_id>/task_state.json`, `trace.jsonl`, and `report.json`.
- **eBPF boundary observability**: optional AgentSight-derived process, stdio, file/network/resource, and TLS plaintext tracing is correlated with agent run traces.
- **Verification-oriented engineering**: benchmark tasks, browser QA, platform smoke tests, and a single project verifier keep the demo from becoming a presentation-only prototype.

## Architecture

![MindBridge system architecture](docs/diagrams/01_system_architecture.png)

Main runtime flow:

```text
Frontend / Client
    -> mindbridge_gateway
    -> mindbridge_orchestrator
    -> mindbridge_counselor
    -> mindbridge_evaluator for high-risk or session-end assessment
    -> ToolRegistry / PermissionChecker / HookExecutor / TraceRecorder
    -> Run artifacts, state store, cloud storage, benchmark reports
```

At a high level:

- `mindbridge_gateway` is the HTTP entrypoint, auth/session boundary, storage API surface, and frontend-facing proxy.
- `mindbridge_orchestrator` routes requests, applies lightweight risk/intent logic, and decides when to involve the evaluator.
- `mindbridge_counselor` runs the main counseling path with prompt policy, memory, RAG, tools, and trace recording.
- `mindbridge_evaluator` handles risk assessment, high-risk fallback, and session summary/report behavior.
- `mindbridge_harness` contains the reusable runtime pieces: model clients, tool governance, state, storage, benchmark, observability, and network primitives.

## Feature Highlights

| Area | What It Demonstrates | Evidence |
|------|----------------------|----------|
| Industrial entrypoints | Separate C++ services for gateway, orchestrator, counselor, evaluator, platform, and benchmark | `mindbridge_harness/apps/*.cpp` |
| Runtime governance | Tool calls are registered, permission checked, hooked, and traced | `mindbridge_harness/include/mindbridge/harness/`, `mindbridge_harness/src/harness/` |
| Mental-health safety loop | Counselor + Evaluator chain, risk policy, session-end assessment, MCP alert/report path | `mindbridge_harness/apps/orchestrator_main.cpp`, `mindbridge_harness/apps/evaluator_main.cpp`, `mindbridge_harness/src/runtime/risk_policy.cpp` |
| Model abstraction | Ollama, OpenAI-compatible, and DashScope native model clients behind `ModelClient` | `mindbridge_harness/include/mindbridge/model/`, `docs/model_strategy.md` |
| Multimodal interaction | Qwen/DashScope multimodal emotion analysis, ASR, and TTS tools | `mindbridge_harness/src/multimodal/`, `mindbridge_harness/src/speech/`, `frontend/demo/app.js` |
| Distributed state | Master/follower replay and user/conversation/namespace/state-key isolation | `mindbridge_harness/src/state/distributed_state_store.cpp` |
| Cloud storage | Gateway storage endpoints backed by local fake mode or MySQL/Redis/FastDFS live stack | `mindbridge_harness/src/storage/cloud_storage.cpp`, `infra/cloud_storage/` |
| Run artifacts | Structured run outputs for traceability and benchmark verification | `mindbridge_harness/src/runtime/run_store.cpp`, `.mindbridge/runs/<run_id>/` contract |
| eBPF observability | AgentSight-derived process/stdout/TLS tracing with run-level correlation and frontend observability view | `mindbridge_harness/ebpf/agentsight_process/`, `mindbridge_harness/src/observability/`, `frontend/demo/` |
| Benchmark and QA | Harness benchmark, PsychoBench scripts, browser smoke, sandbox browser QA | `mindbridge_harness/benchmarks/mindbridge_tasks.json`, `benchmarks/psycho_bench/`, `sandbox/browser_qa/`, `scripts/verify_*.sh` |
| Cloud-native platform MVP | Workspace, AgentCore, AgentSession, UniversalEvent mapping, K8s manifests | `mindbridge_harness/src/platform/`, `k8s/`, `scripts/k8s-platform-deploy.sh` |

## Runtime Governance

MindBridge treats the model as one component inside a governed runtime, not as the whole system.

Tool execution follows this boundary:

```text
ToolRegistry
    -> schema + validation
    -> PermissionChecker
    -> HookExecutor
    -> TraceRecorder
    -> structured ToolResult
```

This is important for mental-health workflows because external actions such as alerting, reporting, retrieval, speech, or storage should be auditable. The design also keeps MCP as a tool protocol integrated through `ToolRegistry`; it is not modeled as a standalone `ToolAgent`.

## State, Storage, and Artifacts

MindBridge uses three different persistence layers for different jobs:

| Layer | Purpose |
|-------|---------|
| `ConversationMemory` / context manager | Short-term prompt-window context and structured conversation memory |
| `DistributedStateStore` | Durable agent state with `user_id + conversation_id + namespace_name + state_key` isolation and follower replay |
| Cloud storage | File/object metadata and chunk upload state through MySQL, Redis, and FastDFS-compatible storage |

Run artifacts are written under:

```text
.mindbridge/runs/<run_id>/
    task_state.json
    trace.jsonl
    report.json
    ebpf_events.jsonl           # optional
    boundary_trace.jsonl        # optional
    observability_report.json   # optional
```

These artifacts make the project easier to inspect in an interview: the runtime can show what happened, which tool path was used, which risk route was selected, and what the benchmark validated.

## eBPF Boundary Observability

MindBridge includes an optional observability path inspired by AgentSight. It correlates semantic agent events with low-level runtime actions.

Implemented pieces include:

- process lifecycle tracing through `process_new`
- file/network/resource extension probes
- stdout/stderr capture through `stdiocap`
- optional TLS plaintext capture through `sslsniff`, guarded by PID or command filters
- correlation into `boundary_trace.jsonl` and `observability_report.json`
- frontend observability panels for run-level inspection

The eBPF path is default-off and does not block the normal counseling flow. It is designed as runtime observability, not as a separate agent and not as an MCP tool.

## Quick Start

Build and verify the default project path:

```bash
bash scripts/verify_mindbridge.sh
```

Manual build:

```bash
cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build --target mindbridge_harness mindbridge_gateway mindbridge_orchestrator mindbridge_counselor mindbridge_evaluator mindbridge_benchmark ai_counselor_agent ai_evaluator_agent -j2
./build/mindbridge_harness/mindbridge_benchmark
```

Run with a remote OpenAI-compatible model:

```bash
bash scripts/start_demo_openai.sh
```

Run with DashScope native mode:

```bash
bash scripts/start_demo_dashscope.sh
```

Both remote startup scripts set `MINDBRIDGE_REQUIRE_REMOTE_MODEL=1` so the demo does not silently fall back to local Ollama when remote model configuration is required.

## Verification Matrix

| Command | Purpose |
|---------|---------|
| `bash scripts/verify_mindbridge.sh` | Main build, benchmark, and regression verifier |
| `./build/mindbridge_harness/mindbridge_benchmark` | Harness contract and hardness benchmark |
| `bash scripts/verify_cloud_storage_smoke.sh` | Local fake storage endpoint smoke |
| `MINDBRIDGE_STORAGE_BACKEND=cloud bash scripts/verify_cloud_storage_live.sh` | MySQL/Redis/FastDFS live storage verification |
| `bash scripts/verify_state_mysql_live.sh` | MySQL-backed distributed state verification |
| `bash scripts/verify_platform_browser_smoke.sh` | Browser-visible platform console smoke |
| `bash scripts/verify_sandbox_browser_qa.sh` | Sandbox/browser QA dry-run and unit checks |
| `bash scripts/verify_ebpf_live.sh` | Live kernel eBPF verification on supported Linux hosts |

## Repository Map

| Path | Role |
|------|------|
| `mindbridge_harness/` | Main industrial C++ agent runtime |
| `frontend/demo/` | Browser demo, storage dashboard, observability/platform views |
| `examples/mindcare/` | Runnable regression demo that should remain compatible |
| `docs/` | Architecture, engineering, model strategy, deployment, benchmark notes |
| `scripts/` | Startup, shutdown, browser QA, storage, platform, eBPF, and verification scripts |
| `infra/cloud_storage/` | Project-owned MySQL/Redis/FastDFS-style storage stack |
| `k8s/` | Cloud-native platform manifests and eBPF opt-in profile |
| `sandbox/browser_qa/` | Browser QA harness and fake model server |
| `benchmarks/psycho_bench/` | PsychoBench-related evaluation scripts and data |
| `integrations/mcp_server_integrated/` | MCP server reference with Excel/Email-style plugins |
| `a2a/`, `a2a_adapter/`, `mcp/`, `orchestrator/`, `proto/`, `common/` | Local C++ dependencies used by the harness and demo |
| `include/`, `src/`, `server/`, `client/` | Legacy/general RPC framework components kept for inspection and optional repair |

External reference projects from the original workspace, such as `OpenHarness/`, `agentscope/`, `deer-flow`, `AutoAI-Coding/`, and `pico-hardness/`, are intentionally not bundled in this repository.

## Configuration Notes

Model access is kept behind the `ModelClient` abstraction.

Common environment variables:

```bash
export MINDBRIDGE_MODEL_PROVIDER=dashscope_native
export MINDBRIDGE_MODEL_NAME=qwen-plus
export DASHSCOPE_API_KEY=...
```

or:

```bash
export MINDBRIDGE_MODEL_PROVIDER=openai_compatible
export MINDBRIDGE_MODEL_BASE_URL=https://api.example.com/v1
export MINDBRIDGE_MODEL_API_KEY=...
export MINDBRIDGE_MODEL_NAME=qwen-plus
```

API keys should be supplied through environment variables or hidden script prompts. Do not commit real keys.

## Interview Framing

A concise way to explain the project:

> MindBridge is an industrial-grade C++ agent harness for a mental-health assistant. My focus was not only prompt quality, but the runtime around the model: multi-service routing, risk evaluation, governed tool execution, persisted state, cloud storage, benchmark verification, browser QA, and optional eBPF observability. The result is a system that can be demonstrated, inspected, and debugged like an engineered agent platform rather than a single LLM wrapper.

What it is honest to claim:

- implemented C++ runtime entrypoints and demo services
- implemented tool governance and trace artifacts
- implemented risk/evaluator flow and mental-health-specific prompt policy
- implemented state isolation and storage integration paths
- implemented browser-visible demo, platform MVP pieces, benchmark, and QA scripts
- implemented optional eBPF observability path on supported Linux hosts

What remains future work:

- stronger production auth/RBAC and rate limiting
- fuller CI coverage for all live provider paths
- broader live K8s deployment verification
- deeper automated browser screenshot regression for observability dashboards

## Documentation

- [Architecture](docs/architecture.md)
- [Harness Engineering](docs/harness_engineering.md)
- [Model Strategy](docs/model_strategy.md)
- [Deployment](docs/deployment.md)
- [Repository Map](REPO_MAP.md)
- [MindBridge Harness README](mindbridge_harness/README.md)
