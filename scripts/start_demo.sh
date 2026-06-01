#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

STATE_DIR="${MINDBRIDGE_DEMO_STATE_DIR:-/tmp/mindbridge_demo}"
LOG_DIR="$STATE_DIR/logs"
DEMO_STATE_DB="${MINDBRIDGE_DEMO_STATE_DB:-.mindbridge/state/demo_master.sqlite}"
mkdir -p "$LOG_DIR"

MODEL_PROVIDER="${MINDBRIDGE_MODEL_PROVIDER:-auto}"
if [[ -z "${MINDBRIDGE_MODEL_API_KEY:-}" && -n "${DASHSCOPE_API_KEY:-}" ]]; then
  export MINDBRIDGE_MODEL_API_KEY="$DASHSCOPE_API_KEY"
fi
if [[ -z "${MINDBRIDGE_MODEL_API_KEY:-}" && -n "${MINDBRIDGE_MIMO_API_KEY:-}" ]]; then
  export MINDBRIDGE_MODEL_API_KEY="$MINDBRIDGE_MIMO_API_KEY"
fi
if [[ "${MINDBRIDGE_MODEL_API_KEY:-}" == http://* || "${MINDBRIDGE_MODEL_API_KEY:-}" == https://* ]]; then
  if [[ -n "${MINDBRIDGE_MIMO_API_KEY:-}" ]]; then
    export MINDBRIDGE_MODEL_API_KEY="$MINDBRIDGE_MIMO_API_KEY"
  else
    echo "ERROR: MINDBRIDGE_MODEL_API_KEY looks like a URL. Put the URL in MINDBRIDGE_MODEL_BASE_URL and the key in MINDBRIDGE_MODEL_API_KEY or MINDBRIDGE_MIMO_API_KEY."
    exit 1
  fi
fi
if [[ -z "${MINDBRIDGE_MODEL_BASE_URL:-}" && -n "${MINDBRIDGE_MIMO_BASE_URL:-}" ]]; then
  export MINDBRIDGE_MODEL_BASE_URL="$MINDBRIDGE_MIMO_BASE_URL"
fi
if [[ -z "${MINDBRIDGE_MODEL_BASE_URL:-}" && "${MINDBRIDGE_MODEL_API_KEY:-}" == tp-* ]]; then
  export MINDBRIDGE_MODEL_BASE_URL="https://token-plan-cn.xiaomimimo.com/v1"
fi
if [[ -z "${MINDBRIDGE_MODEL_BASE_URL:-}" && -n "${MINDBRIDGE_MIMO_API_KEY:-}" ]]; then
  if [[ "${MINDBRIDGE_MIMO_API_KEY:-}" == tp-* ]]; then
    export MINDBRIDGE_MODEL_BASE_URL="https://token-plan-cn.xiaomimimo.com/v1"
  else
    export MINDBRIDGE_MODEL_BASE_URL="https://api.xiaomimimo.com/v1"
  fi
fi
if [[ "$MODEL_PROVIDER" == "auto" || -z "$MODEL_PROVIDER" ]]; then
  if [[ -n "${MINDBRIDGE_MODEL_BASE_URL:-}" ]]; then
    MODEL_PROVIDER="openai_compatible"
  elif [[ -n "${DASHSCOPE_API_KEY:-}" || -n "${MINDBRIDGE_DASHSCOPE_API_KEY:-}" ]]; then
    MODEL_PROVIDER="dashscope_native"
  elif [[ -n "${MINDBRIDGE_MODEL_API_KEY:-}" ]]; then
    MODEL_PROVIDER="openai_compatible"
  else
    MODEL_PROVIDER="ollama"
  fi
fi
if [[ "$MODEL_PROVIDER" == "ollama" ]]; then
  if [[ "${MINDBRIDGE_REQUIRE_REMOTE_MODEL:-0}" == "1" || "${MINDBRIDGE_REQUIRE_REMOTE_MODEL:-}" == "true" ]]; then
    echo "ERROR: remote API model is required, but no remote provider was configured."
    echo "Use scripts/start_demo_dashscope.sh for DashScope, or scripts/start_demo_openai.sh for an OpenAI-compatible API."
    exit 1
  fi
  MODEL_NAME_DEFAULT="qwen2.5:7b-chat"
elif [[ "$MODEL_PROVIDER" == "dashscope_native" || "$MODEL_PROVIDER" == "dashscope" ]]; then
  MODEL_NAME_DEFAULT="qwen-plus"
else
  MODEL_NAME_DEFAULT="qwen3.6-plus"
fi

ASR_PROVIDER_DEFAULT="${MINDBRIDGE_ASR_PROVIDER:-}"
if [[ -z "$ASR_PROVIDER_DEFAULT" ]]; then
  ASR_PROVIDER_DEFAULT="disabled"
fi

# RAG / ChromaDB configuration
export MINDBRIDGE_CHROMA_URL="${MINDBRIDGE_CHROMA_URL:-http://localhost:8000}"
export MINDBRIDGE_CHROMA_COLLECTION="${MINDBRIDGE_CHROMA_COLLECTION:-mindbridge_knowledge}"

check_chroma() {
    local url="${MINDBRIDGE_CHROMA_URL:-http://localhost:8000}"
    if curl -s --connect-timeout 2 "$url/api/v1/heartbeat" >/dev/null 2>&1; then
        echo "[RAG] ChromaDB reachable at $url"
        return 0
    else
        echo "[RAG] WARNING: ChromaDB not reachable at $url. RAG will be disabled."
        echo "[RAG] To enable RAG, start ChromaDB: docker run -d -p 8000:8000 chromadb/chroma"
        return 1
    fi
}

port_in_use() {
  local port="$1"
  ss -ltn "sport = :$port" | awk 'NR > 1 { found=1 } END { exit found ? 0 : 1 }'
}

start_service() {
  local name="$1"
  local port="$2"
  shift 2
  local pid_file="$STATE_DIR/$name.pid"
  local log_file="$LOG_DIR/$name.log"

  if port_in_use "$port"; then
    echo "SKIP $name: port $port is already listening"
    return 0
  fi

  echo "START $name on port $port"
  setsid "$@" > "$log_file" 2>&1 < /dev/null &
  echo "$!" > "$pid_file"
  sleep 0.3

  if ! port_in_use "$port"; then
    echo "FAIL $name: port $port did not open. Log: $log_file"
    tail -n 40 "$log_file" || true
    exit 1
  fi
}

if [[ ! -x build/mindbridge_harness/mindbridge_gateway ||
      ! -x build/mindbridge_harness/mindbridge_orchestrator ||
      ! -x build/mindbridge_harness/mindbridge_counselor ||
      ! -x build/mindbridge_harness/mindbridge_evaluator ]]; then
  echo "Build artifacts are missing; building demo services..."
  cmake -S . -B build -DBUILD_TESTING=OFF
  cmake --build build --target \
    mindbridge_gateway \
    mindbridge_orchestrator \
    mindbridge_counselor \
    mindbridge_evaluator \
    -j2
fi

start_service frontend 5173 \
  python3 scripts/serve_demo_frontend.py --port 5173 --directory frontend/demo

start_evaluator_instance() {
  local name="$1"
  local port="$2"
  local trace_path="$3"
  start_service "$name" "$port" \
    env \
      MINDBRIDGE_MODEL_PROVIDER="$MODEL_PROVIDER" \
      MINDBRIDGE_MODEL_BASE_URL="${MINDBRIDGE_MODEL_BASE_URL:-}" \
      MINDBRIDGE_MODEL_API_KEY="${MINDBRIDGE_MODEL_API_KEY:-}" \
      MINDBRIDGE_MODEL_NAME="${MINDBRIDGE_MODEL_NAME:-$MODEL_NAME_DEFAULT}" \
      MINDBRIDGE_MULTIMODAL_PROVIDER="${MINDBRIDGE_MULTIMODAL_PROVIDER:-dashscope}" \
      MINDBRIDGE_DASHSCOPE_API_KEY="${MINDBRIDGE_DASHSCOPE_API_KEY:-${DASHSCOPE_API_KEY:-${MINDBRIDGE_MODEL_API_KEY:-}}}" \
      MINDBRIDGE_DASHSCOPE_BASE_URL="${MINDBRIDGE_DASHSCOPE_BASE_URL:-${MINDBRIDGE_MODEL_BASE_URL:-https://dashscope.aliyuncs.com/compatible-mode/v1}}" \
      MINDBRIDGE_MIMO_BASE_URL="${MINDBRIDGE_MIMO_BASE_URL:-${MINDBRIDGE_MODEL_BASE_URL:-https://api.xiaomimimo.com/v1}}" \
      MINDBRIDGE_MIMO_API_KEY="${MINDBRIDGE_MIMO_API_KEY:-${MINDBRIDGE_MODEL_API_KEY:-}}" \
      MINDBRIDGE_MIMO_MODEL="${MINDBRIDGE_MIMO_MODEL:-mimo-v2.5}" \
      MINDBRIDGE_TTS_PROVIDER="${MINDBRIDGE_TTS_PROVIDER:-dashscope}" \
      MINDBRIDGE_DASHSCOPE_TTS_VOICE_ID="${MINDBRIDGE_DASHSCOPE_TTS_VOICE_ID:-}" \
      MINDBRIDGE_MIMO_TTS_BASE_URL="${MINDBRIDGE_MIMO_TTS_BASE_URL:-${MINDBRIDGE_MIMO_BASE_URL:-${MINDBRIDGE_MODEL_BASE_URL:-https://api.xiaomimimo.com/v1}}}" \
      MINDBRIDGE_MIMO_TTS_API_KEY="${MINDBRIDGE_MIMO_TTS_API_KEY:-${MINDBRIDGE_MIMO_API_KEY:-${MINDBRIDGE_MODEL_API_KEY:-}}}" \
      MINDBRIDGE_MIMO_TTS_MODEL="${MINDBRIDGE_MIMO_TTS_MODEL:-mimo-v2.5-tts}" \
      MINDBRIDGE_TRACE_PATH="$trace_path" \
      ./build/mindbridge_harness/mindbridge_evaluator "$port"
}

start_counselor_instance() {
  local name="$1"
  local port="$2"
  local trace_path="$3"
  start_service "$name" "$port" \
    env \
      MINDBRIDGE_MODEL_PROVIDER="$MODEL_PROVIDER" \
      MINDBRIDGE_MODEL_BASE_URL="${MINDBRIDGE_MODEL_BASE_URL:-}" \
      MINDBRIDGE_MODEL_API_KEY="${MINDBRIDGE_MODEL_API_KEY:-}" \
      MINDBRIDGE_MODEL_NAME="${MINDBRIDGE_MODEL_NAME:-$MODEL_NAME_DEFAULT}" \
      MINDBRIDGE_MULTIMODAL_PROVIDER="${MINDBRIDGE_MULTIMODAL_PROVIDER:-dashscope}" \
      MINDBRIDGE_DASHSCOPE_API_KEY="${MINDBRIDGE_DASHSCOPE_API_KEY:-${DASHSCOPE_API_KEY:-${MINDBRIDGE_MODEL_API_KEY:-}}}" \
      MINDBRIDGE_DASHSCOPE_BASE_URL="${MINDBRIDGE_DASHSCOPE_BASE_URL:-${MINDBRIDGE_MODEL_BASE_URL:-https://dashscope.aliyuncs.com/compatible-mode/v1}}" \
      MINDBRIDGE_MIMO_BASE_URL="${MINDBRIDGE_MIMO_BASE_URL:-${MINDBRIDGE_MODEL_BASE_URL:-https://api.xiaomimimo.com/v1}}" \
      MINDBRIDGE_MIMO_API_KEY="${MINDBRIDGE_MIMO_API_KEY:-${MINDBRIDGE_MODEL_API_KEY:-}}" \
      MINDBRIDGE_MIMO_MODEL="${MINDBRIDGE_MIMO_MODEL:-mimo-v2.5}" \
      MINDBRIDGE_TTS_PROVIDER="${MINDBRIDGE_TTS_PROVIDER:-dashscope}" \
      MINDBRIDGE_DASHSCOPE_TTS_VOICE_ID="${MINDBRIDGE_DASHSCOPE_TTS_VOICE_ID:-}" \
      MINDBRIDGE_MIMO_TTS_BASE_URL="${MINDBRIDGE_MIMO_TTS_BASE_URL:-${MINDBRIDGE_MIMO_BASE_URL:-${MINDBRIDGE_MODEL_BASE_URL:-https://api.xiaomimimo.com/v1}}}" \
      MINDBRIDGE_MIMO_TTS_API_KEY="${MINDBRIDGE_MIMO_TTS_API_KEY:-${MINDBRIDGE_MIMO_API_KEY:-${MINDBRIDGE_MODEL_API_KEY:-}}}" \
      MINDBRIDGE_MIMO_TTS_MODEL="${MINDBRIDGE_MIMO_TTS_MODEL:-mimo-v2.5-tts}" \
      MINDBRIDGE_STATE_DB_PATH="${MINDBRIDGE_STATE_DB_PATH:-$DEMO_STATE_DB}" \
      MINDBRIDGE_STATE_NODE_ID="${MINDBRIDGE_STATE_NODE_ID:-${name}-master}" \
      MINDBRIDGE_STATE_FOLLOWER_DB_PATH="${MINDBRIDGE_STATE_FOLLOWER_DB_PATH:-.mindbridge/state/${name}_follower.sqlite}" \
      MINDBRIDGE_STATE_FOLLOWER_NODE_ID="${MINDBRIDGE_STATE_FOLLOWER_NODE_ID:-${name}-follower}" \
      MINDBRIDGE_STATE_REPLICA_ENABLED="${MINDBRIDGE_STATE_REPLICA_ENABLED:-true}" \
      MINDBRIDGE_STATE_REPLICA_POLL_MS="${MINDBRIDGE_STATE_REPLICA_POLL_MS:-1000}" \
      MINDBRIDGE_STATE_REPLICA_BATCH="${MINDBRIDGE_STATE_REPLICA_BATCH:-1000}" \
      MINDBRIDGE_TRACE_PATH="$trace_path" \
      ./build/mindbridge_harness/mindbridge_counselor "$port"
}

COUNSELOR_URLS_DEFAULT="http://127.0.0.1:5010,http://127.0.0.1:5012,http://127.0.0.1:5013"
EVALUATOR_URLS_DEFAULT="http://127.0.0.1:5011,http://127.0.0.1:5014,http://127.0.0.1:5015"

start_evaluator_instance evaluator-1 5011 "${MINDBRIDGE_TRACE_PATH:-/tmp/mindbridge_evaluator_1_trace.jsonl}"
start_evaluator_instance evaluator-2 5014 "/tmp/mindbridge_evaluator_2_trace.jsonl"
start_evaluator_instance evaluator-3 5015 "/tmp/mindbridge_evaluator_3_trace.jsonl"

check_chroma || true

start_counselor_instance counselor-1 5010 "${MINDBRIDGE_TRACE_PATH:-/tmp/mindbridge_counselor_1_trace.jsonl}"
start_counselor_instance counselor-2 5012 "/tmp/mindbridge_counselor_2_trace.jsonl"
start_counselor_instance counselor-3 5013 "/tmp/mindbridge_counselor_3_trace.jsonl"

start_service orchestrator 5009 \
  env \
    MINDBRIDGE_COUNSELOR_URLS="${MINDBRIDGE_COUNSELOR_URLS:-$COUNSELOR_URLS_DEFAULT}" \
    MINDBRIDGE_EVALUATOR_URLS="${MINDBRIDGE_EVALUATOR_URLS:-$EVALUATOR_URLS_DEFAULT}" \
    MINDBRIDGE_COUNSELOR_URL="${MINDBRIDGE_COUNSELOR_URL:-http://127.0.0.1:5010}" \
    MINDBRIDGE_EVALUATOR_URL="${MINDBRIDGE_EVALUATOR_URL:-http://127.0.0.1:5011}" \
    MINDBRIDGE_MULTIMODAL_PROVIDER="${MINDBRIDGE_MULTIMODAL_PROVIDER:-dashscope}" \
    MINDBRIDGE_DASHSCOPE_API_KEY="${MINDBRIDGE_DASHSCOPE_API_KEY:-${DASHSCOPE_API_KEY:-${MINDBRIDGE_MODEL_API_KEY:-}}}" \
    MINDBRIDGE_DASHSCOPE_BASE_URL="${MINDBRIDGE_DASHSCOPE_BASE_URL:-${MINDBRIDGE_MODEL_BASE_URL:-https://dashscope.aliyuncs.com/compatible-mode/v1}}" \
    MINDBRIDGE_MIMO_BASE_URL="${MINDBRIDGE_MIMO_BASE_URL:-${MINDBRIDGE_MODEL_BASE_URL:-https://api.xiaomimimo.com/v1}}" \
    MINDBRIDGE_MIMO_API_KEY="${MINDBRIDGE_MIMO_API_KEY:-${MINDBRIDGE_MODEL_API_KEY:-}}" \
    MINDBRIDGE_MIMO_MODEL="${MINDBRIDGE_MIMO_MODEL:-mimo-v2.5}" \
    ./build/mindbridge_harness/mindbridge_orchestrator 5009

start_service gateway 8090 \
  env \
    MINDBRIDGE_ORCHESTRATOR_URL="${MINDBRIDGE_ORCHESTRATOR_URL:-http://127.0.0.1:5009}" \
    MINDBRIDGE_TTS_PROVIDER="${MINDBRIDGE_TTS_PROVIDER:-dashscope}" \
    MINDBRIDGE_ASR_PROVIDER="$ASR_PROVIDER_DEFAULT" \
    MINDBRIDGE_DASHSCOPE_API_KEY="${MINDBRIDGE_DASHSCOPE_API_KEY:-${DASHSCOPE_API_KEY:-}}" \
    MINDBRIDGE_DASHSCOPE_ASR_API_KEY="${MINDBRIDGE_DASHSCOPE_ASR_API_KEY:-${MINDBRIDGE_DASHSCOPE_API_KEY:-${DASHSCOPE_API_KEY:-}}}" \
    MINDBRIDGE_DASHSCOPE_ASR_MODEL="${MINDBRIDGE_DASHSCOPE_ASR_MODEL:-fun-asr-realtime}" \
    MINDBRIDGE_DASHSCOPE_ASR_WS_URL="${MINDBRIDGE_DASHSCOPE_ASR_WS_URL:-wss://dashscope.aliyuncs.com/api-ws/v1/inference}" \
    MINDBRIDGE_DASHSCOPE_TTS_VOICE_ID="${MINDBRIDGE_DASHSCOPE_TTS_VOICE_ID:-}" \
    MINDBRIDGE_MIMO_TTS_BASE_URL="${MINDBRIDGE_MIMO_TTS_BASE_URL:-${MINDBRIDGE_MIMO_BASE_URL:-${MINDBRIDGE_MODEL_BASE_URL:-https://api.xiaomimimo.com/v1}}}" \
    MINDBRIDGE_MIMO_TTS_API_KEY="${MINDBRIDGE_MIMO_TTS_API_KEY:-${MINDBRIDGE_MIMO_API_KEY:-${MINDBRIDGE_MODEL_API_KEY:-}}}" \
    MINDBRIDGE_MIMO_TTS_MODEL="${MINDBRIDGE_MIMO_TTS_MODEL:-mimo-v2.5-tts}" \
    MINDBRIDGE_MIMO_TTS_VOICE="${MINDBRIDGE_MIMO_TTS_VOICE:-mimo_default}" \
    MINDBRIDGE_MIMO_TTS_FORMAT="${MINDBRIDGE_MIMO_TTS_FORMAT:-wav}" \
    MINDBRIDGE_MIMO_TTS_PATH="${MINDBRIDGE_MIMO_TTS_PATH:-/v1/chat/completions}" \
    MINDBRIDGE_STATE_DB_PATH="${MINDBRIDGE_STATE_DB_PATH:-$DEMO_STATE_DB}" \
    ./build/mindbridge_harness/mindbridge_gateway 8090

echo
echo "MindBridge demo is running:"
echo "  Frontend:     http://127.0.0.1:5173/index.html"
echo "  Gateway:      http://127.0.0.1:8090/api/health"
echo "  Orchestrator: http://127.0.0.1:5009/api/health"
echo "  Counselors:   ${MINDBRIDGE_COUNSELOR_URLS:-$COUNSELOR_URLS_DEFAULT}"
echo "  Evaluators:   ${MINDBRIDGE_EVALUATOR_URLS:-$EVALUATOR_URLS_DEFAULT}"
echo "  Logs:         $LOG_DIR"
echo "  Model:        $MODEL_PROVIDER / ${MINDBRIDGE_MODEL_NAME:-$MODEL_NAME_DEFAULT}"
