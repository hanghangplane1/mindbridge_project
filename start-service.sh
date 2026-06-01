#!/usr/bin/env bash
set -euo pipefail

case "${SERVICE_NAME:-}" in
  platform)
    exec ./mindbridge_platform "${MINDBRIDGE_PLATFORM_PORT:-8077}"
    ;;
  gateway)
    exec ./mindbridge_gateway "${MINDBRIDGE_GATEWAY_PORT:-8090}"
    ;;
  orchestrator)
    exec ./mindbridge_orchestrator "${MINDBRIDGE_ORCHESTRATOR_PORT:-5009}"
    ;;
  counselor)
    exec ./mindbridge_counselor "${MINDBRIDGE_COUNSELOR_PORT:-5010}"
    ;;
  evaluator)
    exec ./mindbridge_evaluator "${MINDBRIDGE_EVALUATOR_PORT:-5011}"
    ;;
  frontend)
    exec python3 scripts/serve_demo_frontend.py --port "${MINDBRIDGE_FRONTEND_PORT:-5173}" --directory frontend/demo
    ;;
  *)
    echo "ERROR: SERVICE_NAME must be one of platform, gateway, orchestrator, counselor, evaluator, frontend" >&2
    exit 1
    ;;
esac
