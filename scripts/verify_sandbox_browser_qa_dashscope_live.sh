#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

API_KEY="${DASHSCOPE_API_KEY:-${MINDBRIDGE_MODEL_API_KEY:-}}"
if [[ -t 0 ]]; then
  INPUT_KEY=""
  read -r -s -p "DASHSCOPE_API_KEY (leave empty to reuse current env): " INPUT_KEY
  echo
  if [[ -n "$INPUT_KEY" ]]; then
    API_KEY="$INPUT_KEY"
  fi
fi

if [[ -z "$API_KEY" ]]; then
  echo "FAIL: DASHSCOPE_API_KEY is required for live DashScope sandbox QA."
  echo "Run from an interactive shell so this script can read it without saving it."
  exit 1
fi

export DASHSCOPE_API_KEY="$API_KEY"
export MINDBRIDGE_MODEL_API_KEY="$API_KEY"
export MINDBRIDGE_MODEL_PROVIDER="${MINDBRIDGE_MODEL_PROVIDER:-dashscope_native}"
export MINDBRIDGE_MODEL_NAME="${MINDBRIDGE_MODEL_NAME:-qwen3.6-flash}"
export MINDBRIDGE_REQUIRE_REMOTE_MODEL=1
export MINDBRIDGE_SANDBOX_QA_LIVE=1
export MINDBRIDGE_SANDBOX_PYTHON="${MINDBRIDGE_SANDBOX_PYTHON:-.venv-sandbox/bin/python}"

echo "==> MindBridge DashScope sandbox QA: starting real DashScope demo"
echo "    provider: $MINDBRIDGE_MODEL_PROVIDER"
echo "    model:    $MINDBRIDGE_MODEL_NAME"
echo "    key:      SET"

bash scripts/start_demo_dashscope.sh < /dev/null

echo "==> MindBridge DashScope sandbox QA: running live OpenSandbox verifier"
bash scripts/verify_sandbox_browser_qa.sh
