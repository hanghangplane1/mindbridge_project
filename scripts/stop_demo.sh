#!/usr/bin/env bash
set -euo pipefail

STATE_DIR="${MINDBRIDGE_DEMO_STATE_DIR:-/tmp/mindbridge_demo}"

stop_pid_file() {
  local file="$1"
  local name
  name="$(basename "$file" .pid)"
  local pid
  pid="$(cat "$file" 2>/dev/null || true)"
  if [[ -z "$pid" ]]; then
    rm -f "$file"
    return 0
  fi
  if kill -0 "$pid" 2>/dev/null; then
    echo "STOP $name pid=$pid"
    kill "$pid" 2>/dev/null || true
  fi
  rm -f "$file"
}

if [[ -d "$STATE_DIR" ]]; then
  for file in "$STATE_DIR"/*.pid; do
    [[ -e "$file" ]] || continue
    stop_pid_file "$file"
  done
fi

echo "MindBridge demo stop requested."
