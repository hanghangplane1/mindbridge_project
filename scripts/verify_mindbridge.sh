#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "==> MindBridge verify: configure"
cmake -S . -B build -DBUILD_TESTING=OFF

echo "==> MindBridge verify: build core targets"
cmake --build build \
  --target \
  mindbridge_harness \
  mindbridge_gateway \
  mindbridge_orchestrator \
  mindbridge_counselor \
  mindbridge_evaluator \
  mindbridge_state_demo \
  mindbridge_benchmark \
  mindbridge_net_demo \
  rpc_server \
  rpc_client \
  ai_counselor_agent \
  ai_evaluator_agent \
  -j2

echo "==> MindBridge verify: benchmark"
BENCHMARK_JSON="$(./build/mindbridge_harness/mindbridge_benchmark || true)"
echo "$BENCHMARK_JSON"
python3 - <<'PY' "$BENCHMARK_JSON"
import json
import sys

try:
    payload = json.loads(sys.argv[1])
except Exception as exc:
    raise SystemExit(f"FAIL: benchmark output is not valid JSON: {exc}")

required = ["total", "passed", "failed", "details", "failure_categories", "quality_report"]
for key in required:
    if key not in payload:
        raise SystemExit(f"FAIL: benchmark output missing key: {key}")
print("PASS: benchmark schema validated")
PY

echo "==> MindBridge verify: distributed state visible history"
STATE_DEMO_JSON="$(./build/mindbridge_harness/mindbridge_state_demo)"
echo "$STATE_DEMO_JSON"
python3 - <<'PY' "$STATE_DEMO_JSON"
import json
import sys

payload = json.loads(sys.argv[1])
visible = payload.get("visible_history", {})
if visible.get("source") != "distributed_state_store_master":
    raise SystemExit("FAIL: visible history is not sourced from DistributedStateStore master")
if visible.get("conversation_count") != 1 or visible.get("turn_count") != 2:
    raise SystemExit("FAIL: visible history did not expose persisted conversation turns")
print("PASS: distributed state visible history validated")
PY

echo "==> MindBridge verify: remote model guard"
REMOTE_GUARD_OUTPUT="$(
  MINDBRIDGE_REQUIRE_REMOTE_MODEL=1 \
  MINDBRIDGE_MODEL_PROVIDER=auto \
  env -u MINDBRIDGE_MODEL_API_KEY \
      -u DASHSCOPE_API_KEY \
      -u MINDBRIDGE_DASHSCOPE_API_KEY \
      -u MINDBRIDGE_MODEL_BASE_URL \
      bash scripts/start_demo.sh 2>&1 || true
)"
echo "$REMOTE_GUARD_OUTPUT"
if [[ "$REMOTE_GUARD_OUTPUT" != *"remote API model is required"* ]]; then
  echo "FAIL: remote model guard did not reject missing API configuration"
  exit 1
fi
if [[ "$REMOTE_GUARD_OUTPUT" == *"model_provider=ollama"* || "$REMOTE_GUARD_OUTPUT" == *"START counselor"* ]]; then
  echo "FAIL: remote model guard fell through to local services"
  exit 1
fi
echo "PASS: remote model guard validated"

echo "==> MindBridge verify: feature status schema"
python3 - <<'PY'
import json
from pathlib import Path

path = Path("mindbridge_harness/configs/feature_status.json")
payload = json.loads(path.read_text(encoding="utf-8"))
features = {f.get("name"): f for f in payload.get("features", [])}
required = {
    "pico_tool_governance",
    "pico_run_session_artifacts",
    "pico_structured_memory_context",
    "pico_prompt_prefix_runtime_loop",
    "pico_benchmark_evidence_chain",
}
missing = [name for name in required if name not in features]
if missing:
    raise SystemExit("FAIL: missing feature status entries: " + ", ".join(missing))
print("PASS: feature_status entries validated")
PY

echo "==> MindBridge verify: run artifact schema (if any)"
python3 - <<'PY'
import json
from pathlib import Path

runs_dir = Path(".mindbridge/runs")
if not runs_dir.exists():
    print("WARN: .mindbridge/runs not found; skip artifact file checks")
    raise SystemExit(0)

latest = sorted([p for p in runs_dir.iterdir() if p.is_dir()], key=lambda p: p.stat().st_mtime, reverse=True)
if not latest:
    print("WARN: no run artifacts found; skip")
    raise SystemExit(0)

run = latest[0]
required_files = ["task_state.json", "trace.jsonl", "report.json"]
for name in required_files:
    fp = run / name
    if not fp.exists():
        raise SystemExit(f"FAIL: missing run artifact file: {fp}")

task_state = json.loads((run / "task_state.json").read_text(encoding="utf-8"))
for key in ["run_id", "status", "attempts", "tool_steps", "stop_reason"]:
    if key not in task_state:
        raise SystemExit(f"FAIL: task_state missing key: {key}")

trace_lines = (run / "trace.jsonl").read_text(encoding="utf-8").splitlines()
trace_text = "\n".join(trace_lines)
for event in ["run_started", "model_requested", "run_finished"]:
    if event not in trace_text:
        raise SystemExit(f"FAIL: trace missing event: {event}")
print(f"PASS: run artifact schema validated in {run}")
PY

echo "==> MindBridge verify: cloud storage gateway smoke"
bash scripts/verify_cloud_storage_smoke.sh

echo "==> MindBridge verify: frontend error formatting"
bash scripts/verify_frontend_error_format.sh

echo "==> MindBridge verify: platform frontend wiring"
bash scripts/verify_platform_frontend.sh

echo "==> MindBridge verify: platform browser smoke"
bash scripts/verify_platform_browser_smoke.sh

scan_with_python() {
  local mode="$1"
  python3 - "$mode" <<'PY'
import pathlib
import re
import sys

mode = sys.argv[1]
root = pathlib.Path(".")
targets = []
for rel in ["AGENTS.md", "mindbridge_harness", "examples/mindcare", "docs", "scripts"]:
    p = root / rel
    if p.is_file():
        targets.append(p)
    elif p.is_dir():
        targets.extend([x for x in p.rglob("*") if x.is_file()])

if mode == "secret":
    pattern = re.compile(r"sk-[a-z0-9]{20,}|api[_-]?key\s*[:=]\s*['\"]?[a-z0-9_-]{16,}", re.IGNORECASE)
elif mode == "toolagent":
    pattern = re.compile(r"ToolAgent")
    targets = [p for p in targets if p.suffix in {".cpp", ".cc", ".c", ".hpp", ".h", ".cmake", ".txt"}]
else:
    raise SystemExit("unknown mode")

hits = []
for file_path in targets:
    try:
        content = file_path.read_text(encoding="utf-8", errors="ignore")
    except Exception:
        continue
    for idx, line in enumerate(content.splitlines(), start=1):
        if pattern.search(line):
            hits.append(f"{file_path}:{idx}:{line.strip()}")

if hits:
    print("\n".join(hits))
PY
}

echo "==> MindBridge verify: secret scan"
if command -v rg >/dev/null 2>&1; then
  SECRET_MATCHES_RAW="$(rg -n -i "sk-[a-z0-9]{20,}|api[_-]?key\\s*[:=]\\s*['\\\"]?[a-z0-9_-]{16,}" \
    --glob "AGENTS.md" \
    --glob "mindbridge_harness/**" \
    --glob "examples/mindcare/**" \
    --glob "docs/**" \
    --glob "scripts/**" \
    "$ROOT_DIR" || true)"
else
  SECRET_MATCHES_RAW="$(scan_with_python secret || true)"
fi
SECRET_MATCHES="$(printf "%s\n" "$SECRET_MATCHES_RAW" | awk '!/sk-your-|example-key|placeholder|your-openai-compatible-endpoint/' || true)"
if [[ -n "$SECRET_MATCHES" ]]; then
  echo "FAIL: possible secret detected in tracked project files."
  echo "$SECRET_MATCHES"
  echo "Fix: remove the literal secret and use MINDBRIDGE_MODEL_API_KEY env var."
  exit 1
fi

echo "==> MindBridge verify: architecture guardrail"
if command -v rg >/dev/null 2>&1; then
  TOOL_AGENT_MATCHES="$(rg -n "ToolAgent" \
    --glob "mindbridge_harness/**/*.cpp" \
    --glob "mindbridge_harness/**/*.hpp" \
    --glob "mindbridge_harness/**/*.h" \
    --glob "mindbridge_harness/**/*.cc" \
    --glob "mindbridge_harness/**/*.cmake" \
    --glob "mindbridge_harness/**/CMakeLists.txt" \
    "$ROOT_DIR" || true)"
else
  TOOL_AGENT_MATCHES="$(scan_with_python toolagent || true)"
fi
if [[ -n "$TOOL_AGENT_MATCHES" ]]; then
  echo "FAIL: 'ToolAgent' concept found. MCP must be integrated via ToolRegistry."
  echo "$TOOL_AGENT_MATCHES"
  echo "Fix: refactor to ToolRegistry-based tools and remove ToolAgent wording."
  exit 1
fi

echo "PASS: MindBridge verify completed."
