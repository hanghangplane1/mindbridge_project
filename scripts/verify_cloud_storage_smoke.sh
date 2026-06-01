#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PORT="${MINDBRIDGE_STORAGE_SMOKE_PORT:-18090}"
STATE_DIR="$(mktemp -d /tmp/mindbridge_storage_smoke.XXXXXX)"
cleanup() {
  if [[ -n "${GATEWAY_PID:-}" ]]; then
    kill "$GATEWAY_PID" 2>/dev/null || true
  fi
  rm -rf "$STATE_DIR"
}
trap cleanup EXIT

cmake --build build --target mindbridge_gateway -j2 >/dev/null

MINDBRIDGE_STORAGE_ROOT="$STATE_DIR/storage" \
MINDBRIDGE_STORAGE_STATE_DB_PATH="$STATE_DIR/storage_state.sqlite" \
MINDBRIDGE_ORCHESTRATOR_URL=http://127.0.0.1:59999 \
setsid ./build/mindbridge_harness/mindbridge_gateway "$PORT" >"$STATE_DIR/gateway.log" 2>&1 < /dev/null &
GATEWAY_PID=$!

for _ in $(seq 1 40); do
  if ss -ltn "sport = :$PORT" | awk 'NR>1{found=1} END{exit found?0:1}'; then
    break
  fi
  sleep 0.2
done

if ! ss -ltn "sport = :$PORT" | awk 'NR>1{found=1} END{exit found?0:1}'; then
  echo "FAIL: storage smoke gateway did not start"
  tail -n 80 "$STATE_DIR/gateway.log" || true
  exit 1
fi

python3 - <<'PY' "$PORT"
import base64
import json
import sys
import urllib.request

port = sys.argv[1]
base = f"http://127.0.0.1:{port}"

def post(path, payload=None, raw=None):
    data = raw if raw is not None else json.dumps(payload or {}).encode()
    req = urllib.request.Request(
        base + path,
        data=data,
        headers={"Content-Type": "text/plain;charset=utf-8"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=10) as response:
        out = json.loads(response.read().decode())
    if not out.get("ok"):
        raise SystemExit(f"FAIL: {path}: {out}")
    return out

def get(path):
    with urllib.request.urlopen(base + path, timeout=10) as response:
        out = json.loads(response.read().decode())
    if not out.get("ok"):
        raise SystemExit(f"FAIL: {path}: {out}")
    return out

body = b"hello mindbridge storage"
upload = post("/api/storage/upload", {
    "user": "smoke-user",
    "conversation_id": "smoke-conv",
    "filename": "hello.txt",
    "md5": "smoke-md5-hello",
    "type": "text/plain",
    "data_base64": base64.b64encode(body).decode(),
})
assert upload["file"]["md5"] == "smoke-md5-hello"

instant = post("/api/md5", {
    "user": "smoke-user",
    "conversation_id": "smoke-conv",
    "fileName": "hello-again.txt",
    "md5": "smoke-md5-hello",
    "type": "text/plain",
})
assert instant["instant"] is True

files = get("/api/storage/files?user=smoke-user&conversation_id=smoke-conv")
assert len(files["files"]) >= 1

download = get("/api/storage/files/smoke-md5-hello/download?user=smoke-user&conversation_id=smoke-conv")
assert base64.b64decode(download["data_base64"]) == body

init = post("/api/chunk_init", {
    "user": "smoke-user",
    "conversation_id": "smoke-conv",
    "filename": "chunk.bin",
    "md5": "smoke-md5-chunk",
    "type": "application/octet-stream",
    "size": 6,
    "chunkCount": 2,
})
upload_id = init["upload_id"]
post(f"/api/chunk_upload?upload_id={upload_id}&index=0", raw=b"abc")
post(f"/api/chunk_upload?upload_id={upload_id}&index=1", raw=b"def")
merged = post("/api/chunk_merge", {
    "user": "smoke-user",
    "conversation_id": "smoke-conv",
    "upload_id": upload_id,
    "md5": "smoke-md5-chunk",
    "filename": "chunk.bin",
})
assert merged["file"]["md5"] == "smoke-md5-chunk"
print("PASS: cloud storage gateway smoke")
PY
