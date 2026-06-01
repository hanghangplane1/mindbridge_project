#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PORT="${MINDBRIDGE_STORAGE_LIVE_PORT:-18091}"
STATE_DIR="$(mktemp -d /tmp/mindbridge_storage_live.XXXXXX)"
GATEWAY_PID=""

cleanup() {
  if [[ -n "$GATEWAY_PID" ]]; then
    kill "$GATEWAY_PID" 2>/dev/null || true
  fi
  rm -rf "$STATE_DIR"
}
trap cleanup EXIT

bash scripts/start_mindbridge_cloud_storage.sh >/dev/null
source .mindbridge/cloud_storage/live.env

required_env=(
  MINDBRIDGE_MYSQL_HOST
  MINDBRIDGE_MYSQL_PORT
  MINDBRIDGE_MYSQL_USER
  MINDBRIDGE_MYSQL_PASSWORD
  MINDBRIDGE_MYSQL_DATABASE
  MINDBRIDGE_REDIS_HOST
  MINDBRIDGE_REDIS_PORT
  MINDBRIDGE_FASTDFS_CLIENT_CONF
  MINDBRIDGE_FASTDFS_STORAGE_BASE_URL
  MINDBRIDGE_FASTDFS_STORAGE_HTTPS_BASE_URL
)
for key in "${required_env[@]}"; do
  if [[ -z "${!key:-}" ]]; then
    echo "FAIL: $key is required for live cloud storage verification"
    exit 1
  fi
done

curl -fsS "$MINDBRIDGE_FASTDFS_STORAGE_BASE_URL/healthz" >/dev/null
curl -fksS "$MINDBRIDGE_FASTDFS_STORAGE_HTTPS_BASE_URL/healthz" >/dev/null

cmake --build build --target mindbridge_gateway -j2 >/dev/null

export LD_LIBRARY_PATH="$ROOT_DIR/.deps/cloud_storage/stage/usr/lib64:$ROOT_DIR/.deps/cloud_storage/stage/usr/lib:$ROOT_DIR/.deps/hiredis/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"

MINDBRIDGE_STORAGE_BACKEND=cloud \
MINDBRIDGE_STATE_BACKEND=mysql \
MINDBRIDGE_STORAGE_ROOT="$STATE_DIR/storage" \
MINDBRIDGE_ORCHESTRATOR_URL=http://127.0.0.1:59999 \
setsid ./build/mindbridge_harness/mindbridge_gateway "$PORT" >"$STATE_DIR/gateway.log" 2>&1 < /dev/null &
GATEWAY_PID=$!

for _ in $(seq 1 80); do
  if ss -ltn "sport = :$PORT" | awk 'NR>1{found=1} END{exit found?0:1}'; then
    break
  fi
  sleep 0.25
done

if ! ss -ltn "sport = :$PORT" | awk 'NR>1{found=1} END{exit found?0:1}'; then
  echo "FAIL: live storage gateway did not start"
  tail -n 120 "$STATE_DIR/gateway.log" || true
  exit 1
fi

PY_OUT="$(python3 - <<'PY' "$PORT"
import base64
import hashlib
import json
import os
import time
import socket
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
    with urllib.request.urlopen(req, timeout=30) as response:
        out = json.loads(response.read().decode())
    if not out.get("ok"):
        raise SystemExit(f"FAIL: {path}: {out}")
    return out

def get(path):
    with urllib.request.urlopen(base + path, timeout=30) as response:
        out = json.loads(response.read().decode())
    if not out.get("ok"):
        raise SystemExit(f"FAIL: {path}: {out}")
    return out

def redis_command(*parts):
    host = os.environ["MINDBRIDGE_REDIS_HOST"]
    port = int(os.environ["MINDBRIDGE_REDIS_PORT"])
    password = os.environ.get("MINDBRIDGE_REDIS_PASSWORD", "")
    db = int(os.environ.get("MINDBRIDGE_REDIS_DB", "0") or "0")
    def encode_command(items):
        out = [f"*{len(items)}\r\n".encode()]
        for item in items:
            raw = str(item).encode()
            out.append(f"${len(raw)}\r\n".encode() + raw + b"\r\n")
        return b"".join(out)
    with socket.create_connection((host, port), timeout=10) as sock:
        if password:
            sock.sendall(encode_command(["AUTH", password]))
            sock.recv(1024)
        if db:
            sock.sendall(encode_command(["SELECT", db]))
            sock.recv(1024)
        sock.sendall(encode_command(parts))
        data = sock.recv(1024).decode()
    if data.startswith(":"):
        return int(data[1:].strip())
    raise SystemExit(f"FAIL: unexpected Redis reply for {parts}: {data!r}")

nonce = str(time.time_ns()).encode()
body = b"mindbridge live cloud storage\n" + nonce
md5 = hashlib.md5(body).hexdigest()
upload = post("/api/storage/upload", {
    "user": "live-user",
    "conversation_id": "live-conv",
    "run_id": "live-run",
    "filename": "live.txt",
    "md5": md5,
    "type": "text/plain",
    "data_base64": base64.b64encode(body).decode(),
})
file_id = upload["file"]["file_id"]
url = upload["file"]["url"]
if not file_id or not url:
    raise SystemExit(f"FAIL: upload missing FastDFS file_id/url: {upload}")

instant = post("/api/md5", {
    "user": "live-user",
    "conversation_id": "live-conv",
    "run_id": "live-run",
    "fileName": "live-again.txt",
    "md5": md5,
    "type": "text/plain",
})
assert instant["instant"] is True

files = get("/api/storage/files?user=live-user&conversation_id=live-conv")
assert any(item["md5"] == md5 for item in files["files"])
download = get(f"/api/storage/files/{md5}/download?user=live-user&conversation_id=live-conv")
if base64.b64decode(download.get("data_base64", "")) != body:
    raise SystemExit(f"FAIL: downloaded bytes did not match upload: {download}")

chunk_body = b"abcdefghi" + nonce
chunk_md5 = hashlib.md5(chunk_body).hexdigest()
init = post("/api/chunk_init", {
    "user": "live-user",
    "conversation_id": "live-conv",
    "run_id": "live-run",
    "filename": "chunk-live.bin",
    "md5": chunk_md5,
    "type": "application/octet-stream",
    "size": len(chunk_body),
    "chunkCount": 3,
})
upload_id = init["upload_id"]
print(json.dumps({"phase": "chunk_init", "upload_id": upload_id}), flush=True)
ttl = redis_command("TTL", f"mb:chunk:{upload_id}")
if ttl <= 0:
    raise SystemExit(f"FAIL: Redis chunk TTL is not positive: {ttl}")
post(f"/api/chunk_upload?upload_id={upload_id}&index=0", raw=chunk_body[:3])
post(f"/api/chunk_upload?upload_id={upload_id}&index=1", raw=chunk_body[3:6])
post(f"/api/chunk_upload?upload_id={upload_id}&index=2", raw=chunk_body[6:])
merged = post("/api/storage/chunks/merge", {
    "user": "live-user",
    "conversation_id": "live-conv",
    "run_id": "live-run",
    "upload_id": upload_id,
    "md5": chunk_md5,
    "filename": "chunk-live.bin",
})
if not merged["file"]["file_id"]:
    raise SystemExit(f"FAIL: merged upload missing file_id: {merged}")

status = get("/api/storage/status")
if status.get("backend") != "cloud":
    raise SystemExit(f"FAIL: expected cloud backend: {status}")

print(json.dumps({"md5": md5, "chunk_md5": chunk_md5, "upload_id": upload_id, "file_id": file_id, "url": url}))
PY
)"

echo "$PY_OUT"
SUMMARY="$(printf '%s\n' "$PY_OUT" | tail -n 1)"
MD5="$(python3 - <<'PY' "$SUMMARY"
import json, sys
print(json.loads(sys.argv[1])["md5"])
PY
)"
UPLOAD_ID="$(python3 - <<'PY' "$SUMMARY"
import json, sys
print(json.loads(sys.argv[1])["upload_id"])
PY
)"

if [[ -n "${MINDBRIDGE_MYSQL_CONTAINER:-}" ]]; then
  MYSQL_CHECK="$(docker exec "$MINDBRIDGE_MYSQL_CONTAINER" mysql \
    -u"$MINDBRIDGE_MYSQL_USER" "-p$MINDBRIDGE_MYSQL_PASSWORD" "$MINDBRIDGE_MYSQL_DATABASE" \
    -N -e "SELECT COUNT(*) FROM mb_file_info WHERE md5='$MD5'; SELECT COUNT(*) FROM mb_storage_change_tasks WHERE md5='$MD5' AND status='done'; SELECT COUNT(*) FROM mb_storage_node_progress WHERE applied_count > 0; SELECT COUNT(*) FROM mb_state_records WHERE namespace_name='run_artifact' AND state_key='file_$MD5'; SELECT COUNT(*) FROM mb_state_changes WHERE namespace_name='run_artifact' AND state_key='file_$MD5';" 2>/dev/null)"
  printf '%s\n' "$MYSQL_CHECK"
  if [[ "$(printf '%s\n' "$MYSQL_CHECK" | sed -n '1p')" -lt 1 ]]; then
    echo "FAIL: MySQL mb_file_info row was not written"
    exit 1
  fi
  if [[ "$(printf '%s\n' "$MYSQL_CHECK" | sed -n '2p')" -lt 1 ]]; then
    echo "FAIL: MySQL storage change task was not applied"
    exit 1
  fi
  if [[ "$(printf '%s\n' "$MYSQL_CHECK" | sed -n '3p')" -lt 1 ]]; then
    echo "FAIL: MySQL storage node progress was not updated"
    exit 1
  fi
  if [[ "$(printf '%s\n' "$MYSQL_CHECK" | sed -n '4p')" -lt 1 ]]; then
    echo "FAIL: MySQL run_artifact state record was not written"
    exit 1
  fi
  if [[ "$(printf '%s\n' "$MYSQL_CHECK" | sed -n '5p')" -lt 1 ]]; then
    echo "FAIL: MySQL run_artifact state change was not written"
    exit 1
  fi
fi

TTL="$(docker exec mindbridge_redis redis-cli TTL "mb:chunk:$UPLOAD_ID")"
if [[ "$TTL" != "-2" && "$TTL" -le 0 ]]; then
  echo "FAIL: Redis chunk TTL was not positive before merge cleanup"
  exit 1
fi

echo "PASS: cloud storage live verify"
