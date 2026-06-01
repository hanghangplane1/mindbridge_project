#!/usr/bin/env bash
set -euo pipefail

BASE_URL="${1:-http://localhost:5000}"
CTX_ID="itest-$(date +%s)"

echo "[itest] target: ${BASE_URL}, context: ${CTX_ID}"

REQ_SEND="$(cat <<EOF
{"jsonrpc":"2.0","id":"1","method":"message/send","params":{"message":{"role":"user","contextId":"${CTX_ID}","parts":[{"kind":"text","text":"我最近很焦虑，感觉活着没意义。"}]},"historyLength":5}}
EOF
)"

echo "[itest] send request..."
curl -sS -X POST "${BASE_URL}" \
  -H "Content-Type: application/json" \
  -d "${REQ_SEND}" | tee /tmp/mindcare_send_response.json

echo
echo "[itest] stream request..."
REQ_STREAM="$(cat <<EOF
{"jsonrpc":"2.0","id":"2","method":"message/stream","params":{"message":{"role":"user","contextId":"${CTX_ID}","parts":[{"kind":"text","text":"我感到很绝望。"}]},"historyLength":5}}
EOF
)"

curl -N -sS -X POST "${BASE_URL}" \
  -H "Content-Type: application/json" \
  -d "${REQ_STREAM}" | tee /tmp/mindcare_stream_response.sse

echo
echo "[itest] done. check:"
echo "  - /tmp/mindcare_send_response.json"
echo "  - /tmp/mindcare_stream_response.sse"
echo "  - examples/mindcare/logs/*.log"
