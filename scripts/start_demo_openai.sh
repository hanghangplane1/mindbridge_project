#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DEFAULT_BASE_URL="https://api.xiaomimimo.com/v1"
DEFAULT_MODEL="mimo-v2.5"

read -r -p "OpenAI-compatible Base URL [$DEFAULT_BASE_URL]: " BASE_URL
BASE_URL="${BASE_URL:-$DEFAULT_BASE_URL}"

read -r -p "Model name [$DEFAULT_MODEL]: " MODEL_NAME
MODEL_NAME="${MODEL_NAME:-$DEFAULT_MODEL}"

API_KEY="${MINDBRIDGE_MODEL_API_KEY:-}"
if [[ -z "$API_KEY" ]]; then
  read -r -s -p "API key: " API_KEY
  echo
fi

if [[ -z "$API_KEY" ]]; then
  echo "ERROR: API key is required. It is only passed as an environment variable and is not saved."
  exit 1
fi

bash scripts/stop_demo.sh

export MINDBRIDGE_MODEL_PROVIDER=openai_compatible
export MINDBRIDGE_MODEL_BASE_URL="$BASE_URL"
export MINDBRIDGE_MODEL_NAME="$MODEL_NAME"
export MINDBRIDGE_MODEL_API_KEY="$API_KEY"
export MINDBRIDGE_REQUIRE_REMOTE_MODEL=1
export MINDBRIDGE_MULTIMODAL_PROVIDER="${MINDBRIDGE_MULTIMODAL_PROVIDER:-mimo}"
export MINDBRIDGE_MIMO_BASE_URL="${MINDBRIDGE_MIMO_BASE_URL:-$BASE_URL}"
export MINDBRIDGE_MIMO_MODEL="${MINDBRIDGE_MIMO_MODEL:-$MODEL_NAME}"
export MINDBRIDGE_MIMO_API_KEY="${MINDBRIDGE_MIMO_API_KEY:-$API_KEY}"
export MINDBRIDGE_TTS_PROVIDER="${MINDBRIDGE_TTS_PROVIDER:-mimo}"
export MINDBRIDGE_MIMO_TTS_BASE_URL="${MINDBRIDGE_MIMO_TTS_BASE_URL:-$BASE_URL}"
export MINDBRIDGE_MIMO_TTS_MODEL="${MINDBRIDGE_MIMO_TTS_MODEL:-mimo-v2.5-tts}"
export MINDBRIDGE_MIMO_TTS_API_KEY="${MINDBRIDGE_MIMO_TTS_API_KEY:-$API_KEY}"
export MINDBRIDGE_MIMO_TTS_VOICE="${MINDBRIDGE_MIMO_TTS_VOICE:-mimo_default}"
export MINDBRIDGE_MIMO_TTS_FORMAT="${MINDBRIDGE_MIMO_TTS_FORMAT:-wav}"
export MINDBRIDGE_MIMO_TTS_PATH="${MINDBRIDGE_MIMO_TTS_PATH:-/v1/chat/completions}"

exec bash scripts/start_demo.sh
