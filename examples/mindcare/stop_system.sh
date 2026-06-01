#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PID_FILE="${ROOT_DIR}/examples/mindcare/logs/mindcare.pids"

if [[ ! -f "${PID_FILE}" ]]; then
  echo "[mindcare] no pid file found"
  exit 0
fi

while read -r pid; do
  if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
    kill "${pid}" || true
  fi
done < "${PID_FILE}"

rm -f "${PID_FILE}"
echo "[mindcare] stopped"
