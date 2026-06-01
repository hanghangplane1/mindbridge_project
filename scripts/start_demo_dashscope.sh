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
  echo "ERROR: API key is required. It is only passed as an environment variable and is not saved."
  exit 1
fi

if [[ "${MINDBRIDGE_SKIP_API_PREFLIGHT:-0}" != "1" ]]; then
  echo "Checking DashScope API key before starting services..."
  PREFLIGHT_MODEL="${MINDBRIDGE_PREFLIGHT_MODEL:-qwen-plus}"
  PREFLIGHT_STATUS="$(
    DASHSCOPE_API_KEY="$API_KEY" MINDBRIDGE_PREFLIGHT_MODEL="$PREFLIGHT_MODEL" python3 - <<'PY'
import json
import os
import sys
import urllib.error
import urllib.request

api_key = os.environ["DASHSCOPE_API_KEY"]
model = os.environ.get("MINDBRIDGE_PREFLIGHT_MODEL", "qwen-plus")
payload = json.dumps({
    "model": model,
    "input": {"messages": [{"role": "user", "content": "ping"}]},
    "parameters": {"result_format": "message", "max_tokens": 8},
}).encode("utf-8")
request = urllib.request.Request(
    "https://dashscope.aliyuncs.com/api/v1/services/aigc/text-generation/generation",
    data=payload,
    headers={
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    },
    method="POST",
)
try:
    with urllib.request.urlopen(request, timeout=20) as response:
        response.read()
    print("ok")
except urllib.error.HTTPError as exc:
    body = exc.read().decode("utf-8", errors="replace")
    print(f"http:{exc.code}:{body[:300]}")
    sys.exit(2)
except Exception as exc:
    print(f"error:{exc}")
    sys.exit(3)
PY
  )" || {
    echo "ERROR: DashScope API preflight failed: $PREFLIGHT_STATUS"
    echo "Fix: generate a new DashScope/Bailian API key, then rerun scripts/start_demo_dashscope.sh."
    echo "The key is not saved by this script. Set MINDBRIDGE_SKIP_API_PREFLIGHT=1 only if the preflight endpoint is blocked but the key is known valid."
    exit 1
  }
fi

if [[ -z "${MINDBRIDGE_DASHSCOPE_TTS_VOICE_ID:-}" ]]; then
  echo "WARN: MINDBRIDGE_DASHSCOPE_TTS_VOICE_ID is not set; TTS will use the default DashScope system voice fallback."
fi

bash scripts/stop_demo.sh
bash scripts/start_mindbridge_cloud_storage.sh >/dev/null
source .mindbridge/cloud_storage/live.env

export DASHSCOPE_API_KEY="$API_KEY"

export MINDBRIDGE_MODEL_PROVIDER="${MINDBRIDGE_MODEL_PROVIDER:-dashscope_native}"
export MINDBRIDGE_MODEL_NAME="${MINDBRIDGE_MODEL_NAME:-qwen3.6-plus}"
export MINDBRIDGE_MODEL_API_KEY="$API_KEY"
export MINDBRIDGE_REQUIRE_REMOTE_MODEL=1
export MINDBRIDGE_QWEN_ENABLE_THINKING="${MINDBRIDGE_QWEN_ENABLE_THINKING:-false}"

export MINDBRIDGE_MULTIMODAL_PROVIDER="${MINDBRIDGE_MULTIMODAL_PROVIDER:-dashscope}"
export MINDBRIDGE_DASHSCOPE_API_KEY="$API_KEY"
export MINDBRIDGE_DASHSCOPE_BASE_URL="${MINDBRIDGE_DASHSCOPE_BASE_URL:-https://dashscope.aliyuncs.com/compatible-mode/v1}"
export MINDBRIDGE_DASHSCOPE_MULTIMODAL_MODEL="${MINDBRIDGE_DASHSCOPE_MULTIMODAL_MODEL:-$MINDBRIDGE_MODEL_NAME}"

export MINDBRIDGE_ASR_PROVIDER="${MINDBRIDGE_ASR_PROVIDER:-dashscope}"
export MINDBRIDGE_DASHSCOPE_ASR_API_KEY="$API_KEY"
export MINDBRIDGE_DASHSCOPE_ASR_MODEL="${MINDBRIDGE_DASHSCOPE_ASR_MODEL:-fun-asr-realtime}"
export MINDBRIDGE_DASHSCOPE_ASR_WS_URL="${MINDBRIDGE_DASHSCOPE_ASR_WS_URL:-wss://dashscope.aliyuncs.com/api-ws/v1/inference}"

export MINDBRIDGE_TTS_PROVIDER="${MINDBRIDGE_TTS_PROVIDER:-dashscope}"
export MINDBRIDGE_DASHSCOPE_TTS_API_KEY="$API_KEY"
export MINDBRIDGE_DASHSCOPE_TTS_MODEL="${MINDBRIDGE_DASHSCOPE_TTS_MODEL:-cosyvoice-v3-flash}"
export MINDBRIDGE_DASHSCOPE_TTS_FORMAT="${MINDBRIDGE_DASHSCOPE_TTS_FORMAT:-mp3}"
export MINDBRIDGE_DASHSCOPE_TTS_WS_URL="${MINDBRIDGE_DASHSCOPE_TTS_WS_URL:-wss://dashscope.aliyuncs.com/api-ws/v1/inference}"

echo "DashScope demo config: model=$MINDBRIDGE_MODEL_NAME multimodal=$MINDBRIDGE_DASHSCOPE_MULTIMODAL_MODEL asr=$MINDBRIDGE_ASR_PROVIDER/$MINDBRIDGE_DASHSCOPE_ASR_MODEL storage=$MINDBRIDGE_STORAGE_BACKEND state=$MINDBRIDGE_STATE_BACKEND"

exec bash scripts/start_demo.sh
