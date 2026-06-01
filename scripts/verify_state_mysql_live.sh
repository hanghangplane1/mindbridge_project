#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

bash scripts/start_mindbridge_cloud_storage.sh >/dev/null
source .mindbridge/cloud_storage/live.env

export LD_LIBRARY_PATH="$ROOT_DIR/.deps/cloud_storage/stage/usr/lib64:$ROOT_DIR/.deps/cloud_storage/stage/usr/lib:$ROOT_DIR/.deps/hiredis/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"

docker exec "$MINDBRIDGE_MYSQL_CONTAINER" mysql \
  -u"$MINDBRIDGE_MYSQL_USER" "-p$MINDBRIDGE_MYSQL_PASSWORD" "$MINDBRIDGE_MYSQL_DATABASE" \
  -e "TRUNCATE TABLE mb_replica_progress; TRUNCATE TABLE mb_state_changes; TRUNCATE TABLE mb_state_records;" >/dev/null

cmake --build build --target mindbridge_state_demo -j2 >/dev/null

OUT="$(MINDBRIDGE_STATE_BACKEND=mysql ./build/mindbridge_harness/mindbridge_state_demo)"
echo "$OUT"

python3 - <<'PY' "$OUT"
import json
import sys

payload = json.loads(sys.argv[1])
if payload["master"]["change_count"] < 3:
    raise SystemExit("FAIL: expected master MySQL change log entries")
if payload["follower"]["record_count"] < 3:
    raise SystemExit("FAIL: expected follower replayed records")
if payload["follower"]["applied_count"] < 3:
    raise SystemExit("FAIL: expected applied replay progress")
if not payload["isolation"]["user_namespace_isolated"]:
    raise SystemExit("FAIL: namespace isolation failed")
print("PASS: mysql state demo JSON validated")
PY

MYSQL_CHECK="$(docker exec "$MINDBRIDGE_MYSQL_CONTAINER" mysql \
  -u"$MINDBRIDGE_MYSQL_USER" "-p$MINDBRIDGE_MYSQL_PASSWORD" "$MINDBRIDGE_MYSQL_DATABASE" \
  -N -e "SELECT COUNT(*) FROM mb_state_records; SELECT COUNT(*) FROM mb_state_changes; SELECT COUNT(*) FROM mb_replica_progress WHERE applied_count > 0;" 2>/dev/null)"
printf '%s\n' "$MYSQL_CHECK"

if [[ "$(printf '%s\n' "$MYSQL_CHECK" | sed -n '1p')" -lt 3 ]]; then
  echo "FAIL: mb_state_records did not persist replayed state"
  exit 1
fi
if [[ "$(printf '%s\n' "$MYSQL_CHECK" | sed -n '2p')" -lt 3 ]]; then
  echo "FAIL: mb_state_changes did not persist master changes"
  exit 1
fi
if [[ "$(printf '%s\n' "$MYSQL_CHECK" | sed -n '3p')" -lt 1 ]]; then
  echo "FAIL: mb_replica_progress did not record follower progress"
  exit 1
fi

echo "PASS: MySQL-backed DistributedStateStore live verify"
