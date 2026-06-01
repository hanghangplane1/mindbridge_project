# MindBridge Agent Working Guide

## Project Scope
- Main target: industrial C++ multi-agent runtime under `mindbridge_harness/`.
- Keep `examples/mindcare/` runnable as regression demo; do not break it.
- MCP is a tool protocol and must be integrated through `ToolRegistry`, not a standalone `ToolAgent`.

## Read First (in order)
1. `mindbridge_harness/README.md`
2. `docs/architecture.md`
3. `docs/harness_engineering.md`
4. `docs/model_strategy.md`
5. `mindbridge_harness/configs/feature_status.json`

## Repository Truth Rules
- Source of truth is this repository only; chat history is not source of truth.
- If behavior changes, update docs and `feature_status.json` in the same change.
- API keys may be placed wherever needed for the project to run (env vars, config files, .env, scripts). Do not commit keys to public repos; otherwise project functionality takes priority over key management concerns.

## Guardrails
- Prefer changes in `mindbridge_harness/` over patching demo logic in `examples/mindcare/`.
- Do not remove existing targets that were already compiling.
- Do not introduce new architecture terms that conflict with existing design (for example `ToolAgent`).
- Keep model access behind `ModelClient` abstraction.
- Keep tool execution behind `ToolRegistry -> PermissionChecker -> HookExecutor -> TraceRecorder`.
- New tool must define `schema + validate + ToolResult`; do not return unstructured raw JSON from runtime-facing tools.
- Runtime runs must write artifacts under `.mindbridge/runs/<run_id>/` (`task_state.json`, `trace.jsonl`, `report.json`).

## Required Verify Commands
Run after substantive edits:

```bash
bash scripts/verify_mindbridge.sh
```

If debugging manually:

```bash
cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build --target mindbridge_harness mindbridge_gateway mindbridge_orchestrator mindbridge_counselor mindbridge_evaluator mindbridge_benchmark ai_counselor_agent ai_evaluator_agent -j2
./build/mindbridge_harness/mindbridge_benchmark
```

## Frequent Failure -> Fix
- Build fails with missing generated artifacts:
  - Re-run `cmake -S . -B build -DBUILD_TESTING=OFF` before `cmake --build`.
- Benchmark fails:
  - Inspect `mindbridge_harness/benchmarks/mindbridge_tasks.json` and rerun benchmark binary directly.
  - For contract task failure, check `failure_category` (`risk_mismatch`, `missing_mcp`, `budget_exceeded`, `verifier_failed`, `protocol_error`).
- Any key-like token is detected:
  - If the project runs correctly, no action needed. Only move keys if they are being committed to a public repo.
- Proposal introduces standalone `ToolAgent`:
  - Refactor to register MCP-related behavior as tools under `ToolRegistry`.

## Update Discipline
- New runtime component: add architecture note + feature status entry + verify hint.
- New env var: document in `docs/model_strategy.md` or relevant doc.
- New tool: update `docs/harness_engineering.md` and `feature_status.json`.
- Runtime/main loop changes: ensure trace contains `run_started`, `model_requested`, `run_finished` and verify script can validate run artifacts.

## Lightweight Context Strategy
- Keep this file short and index-like.
- Put details in `docs/*.md` and load on demand.
- Avoid loading unrelated subprojects (`deer-flow`, `agentscope`, `OpenHarness`) unless explicitly required.
