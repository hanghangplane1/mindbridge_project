#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
LOG_DIR="${ROOT_DIR}/examples/mindcare/logs"
PID_FILE="${LOG_DIR}/mindcare.pids"

mkdir -p "${LOG_DIR}"
rm -f "${PID_FILE}"

echo "[mindcare] starting registry..."
"${BUILD_DIR}/examples/ai_orchestrator/ai_registry_server" 8500 > "${LOG_DIR}/registry.log" 2>&1 &
echo $! >> "${PID_FILE}"

echo "[mindcare] starting mcp server..."
"${BUILD_DIR}/mcp_server_integrated/mcp_server" -s -p "${BUILD_DIR}/mcp_server_integrated/plugins" > "${LOG_DIR}/mcp.log" 2>&1 &
echo $! >> "${PID_FILE}"

echo "[mindcare] starting evaluator..."
"${BUILD_DIR}/examples/mindcare/ai_evaluator_agent" 5001 \
  --registry http://localhost:8500 \
  --redis 127.0.0.1:6379 \
  --ollama http://localhost:11434 \
  --model qwen2.5:7b-chat \
  --mcp-server "${BUILD_DIR}/mcp_server_integrated/mcp_server" \
  --mcp-plugins "${BUILD_DIR}/mcp_server_integrated/plugins" > "${LOG_DIR}/evaluator.log" 2>&1 &
echo $! >> "${PID_FILE}"

echo "[mindcare] starting counselor..."
"${BUILD_DIR}/examples/mindcare/ai_counselor_agent" 5000 \
  --registry http://localhost:8500 \
  --redis 127.0.0.1:6379 \
  --ollama http://localhost:11434 \
  --model mindcare-counselor:latest \
  --chroma http://localhost:8000 \
  --chroma-collection psyqa_knowledge > "${LOG_DIR}/counselor.log" 2>&1 &
echo $! >> "${PID_FILE}"

echo "[mindcare] started. pids in ${PID_FILE}"
