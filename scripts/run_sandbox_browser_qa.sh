#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

FRONTEND_URL="${MINDBRIDGE_FRONTEND_URL:-http://127.0.0.1:5173/index.html}"
GATEWAY_URL="${MINDBRIDGE_GATEWAY_URL:-http://127.0.0.1:8090}"
DOCKER_BRIDGE_IP="$(ip -4 addr show docker0 2>/dev/null | awk '/inet / { print $2; exit }' | cut -d/ -f1 || true)"
SANDBOX_HOST_DEFAULT="${DOCKER_BRIDGE_IP:-host.docker.internal}"
SANDBOX_FRONTEND_URL="${MINDBRIDGE_SANDBOX_FRONTEND_URL:-http://${SANDBOX_HOST_DEFAULT}:5173/index.html}"
SANDBOX_GATEWAY_URL="${MINDBRIDGE_SANDBOX_GATEWAY_URL:-http://${SANDBOX_HOST_DEFAULT}:8090}"
PYTHON_BIN="${MINDBRIDGE_SANDBOX_PYTHON:-python3}"
RUN_ID="${MINDBRIDGE_SANDBOX_QA_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
OUTPUT_ROOT="${MINDBRIDGE_SANDBOX_QA_OUTPUT_DIR:-.mindbridge/sandbox_qa}"
DRY_RUN="${MINDBRIDGE_SANDBOX_QA_DRY_RUN:-0}"

require_url() {
  local label="$1"
  local url="$2"
  if ! curl -fsS --max-time 5 "$url" >/dev/null; then
    echo "FAIL: $label is not reachable at $url"
    echo "Start the demo first with: bash scripts/start_demo.sh"
    exit 1
  fi
}

echo "==> MindBridge sandbox browser QA: local preflight"
if [[ "$DRY_RUN" == "1" || "$DRY_RUN" == "true" || "$DRY_RUN" == "TRUE" || "$DRY_RUN" == "yes" ]]; then
  echo "SKIP local URL preflight in dry-run mode"
else
  require_url "frontend" "$FRONTEND_URL"
  require_url "gateway health" "${GATEWAY_URL%/}/api/health"
fi

echo "==> MindBridge sandbox browser QA: OpenSandbox Playwright run"
echo "    frontend(local):  $FRONTEND_URL"
echo "    gateway(local):   $GATEWAY_URL"
echo "    frontend(sbox):   $SANDBOX_FRONTEND_URL"
echo "    gateway(sbox):    $SANDBOX_GATEWAY_URL"
echo "    output:           $OUTPUT_ROOT/$RUN_ID"

args=(
  --frontend-url "$FRONTEND_URL" \
  --gateway-url "$GATEWAY_URL" \
  --sandbox-frontend-url "$SANDBOX_FRONTEND_URL" \
  --sandbox-gateway-url "$SANDBOX_GATEWAY_URL" \
  --output-dir "$OUTPUT_ROOT" \
  --run-id "$RUN_ID"
)

if [[ "$DRY_RUN" == "1" || "$DRY_RUN" == "true" || "$DRY_RUN" == "TRUE" || "$DRY_RUN" == "yes" ]]; then
  args+=(--dry-run)
fi

"$PYTHON_BIN" sandbox/browser_qa/mindbridge_browser_qa.py "${args[@]}"
