#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

RUN_ID="${MINDBRIDGE_SANDBOX_QA_VERIFY_RUN_ID:-verify-sandbox-browser-qa}"

echo "==> Sandbox Browser QA verify: unit tests"
python3 -m unittest sandbox.browser_qa.test_mindbridge_browser_qa -v

echo "==> Sandbox Browser QA verify: syntax"
bash -n scripts/run_sandbox_browser_qa.sh
bash -n scripts/verify_sandbox_browser_qa_dashscope_live.sh
python3 -m py_compile sandbox/browser_qa/mindbridge_browser_qa.py
python3 -m py_compile sandbox/browser_qa/fake_openai_server.py

echo "==> Sandbox Browser QA verify: dry-run artifacts"
MINDBRIDGE_SANDBOX_QA_DRY_RUN=1 \
MINDBRIDGE_SANDBOX_QA_RUN_ID="$RUN_ID" \
bash scripts/run_sandbox_browser_qa.sh

RESULT_PATH=".mindbridge/sandbox_qa/$RUN_ID/qa_result.json"
NOTE_PATH=".mindbridge/sandbox_qa/$RUN_ID/mindbridge-browser-qa.txt"

python3 - <<'PY' "$RESULT_PATH" "$NOTE_PATH"
import json
import sys
from pathlib import Path

result_path = Path(sys.argv[1])
note_path = Path(sys.argv[2])
if not result_path.exists():
    raise SystemExit(f"FAIL: missing dry-run result: {result_path}")
if not note_path.exists():
    raise SystemExit(f"FAIL: missing dry-run note: {note_path}")
payload = json.loads(result_path.read_text(encoding="utf-8"))
if payload.get("status") != "passed":
    raise SystemExit(f"FAIL: dry-run status is not passed: {payload.get('status')}")
if payload.get("sandbox_id") != "dry-run":
    raise SystemExit("FAIL: dry-run sandbox_id mismatch")
checks = payload.get("checks", [])
required = {"dry_run_artifact_schema", "dry_run_target_urls"}
seen = {item.get("name") for item in checks}
missing = sorted(required - seen)
if missing:
    raise SystemExit("FAIL: dry-run missing checks: " + ", ".join(missing))
print("PASS: dry-run artifact schema validated")
PY

if [[ "${MINDBRIDGE_SANDBOX_QA_LIVE:-0}" == "1" ]]; then
  echo "==> Sandbox Browser QA verify: live OpenSandbox run"
  bash scripts/run_sandbox_browser_qa.sh
else
  echo "SKIP: live OpenSandbox run (set MINDBRIDGE_SANDBOX_QA_LIVE=1 to enable)"
fi

echo "PASS: sandbox browser QA verification completed"
