#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BUILD_DIR="${MINDBRIDGE_EBPF_BUILD_DIR:-build-ebpf}"
HELPER="$BUILD_DIR/mindbridge_harness/mindbridge_ebpf_monitor"
STDIOCAP_HELPER="$BUILD_DIR/mindbridge_harness/mindbridge_stdiocap_monitor"
SSLSNIFF_HELPER="$BUILD_DIR/mindbridge_harness/mindbridge_sslsniff_monitor"
RUNTIME_SMOKE="$BUILD_DIR/mindbridge_harness/mindbridge_ebpf_runtime_smoke"
DEFAULT_LOG="${MINDBRIDGE_EBPF_DEFAULT_LOG:-/tmp/mindbridge_ebpf_default.log}"
EXTENDED_LOG="${MINDBRIDGE_EBPF_EXTENDED_LOG:-/tmp/mindbridge_ebpf_extended.log}"
STDIOCAP_LOG="${MINDBRIDGE_STDIOCAP_LOG:-/tmp/mindbridge_stdiocap.log}"
SSLSNIFF_LOG="${MINDBRIDGE_SSLSNIFF_LOG:-/tmp/mindbridge_sslsniff.log}"
RUNTIME_LOG="${MINDBRIDGE_EBPF_RUNTIME_LOG:-/tmp/mindbridge_ebpf_runtime_smoke.log}"
WRAPPER="${MINDBRIDGE_EBPF_WRAPPER:-/tmp/mindbridge_ebpf_helper_wrapper.sh}"
TLS_WRAPPER="${MINDBRIDGE_EBPF_TLS_WRAPPER:-/tmp/mindbridge_ebpf_tls_wrapper.sh}"
read -r -a SUDO_CMD <<<"${MINDBRIDGE_SUDO:-sudo}"
SUDO_PASSWORD="${MINDBRIDGE_SUDO_PASSWORD:-}"

sudo_refresh() {
  if [[ -n "$SUDO_PASSWORD" ]]; then
    printf '%s\n' "$SUDO_PASSWORD" | sudo -S -v
  else
    "${SUDO_CMD[@]}" -v
  fi
}

run_sudo_timeout() {
  local seconds="$1"
  shift
  if [[ -n "$SUDO_PASSWORD" ]]; then
    printf '%s\n' "$SUDO_PASSWORD" | sudo -S timeout "$seconds" "$@"
  else
    "${SUDO_CMD[@]}" timeout "$seconds" "$@"
  fi
}

echo "==> eBPF live verify: configure/build"
cmake -S . -B "$BUILD_DIR" -DBUILD_TESTING=OFF -DMINDBRIDGE_ENABLE_EBPF=ON
cmake --build "$BUILD_DIR" --target mindbridge_ebpf_monitor mindbridge_stdiocap_monitor mindbridge_sslsniff_monitor mindbridge_ebpf_runtime_smoke -j2

if [[ ! -x "$HELPER" ]]; then
  echo "FAIL: helper not executable: $HELPER" >&2
  exit 1
fi
if [[ ! -x "$RUNTIME_SMOKE" ]]; then
  echo "FAIL: runtime smoke not executable: $RUNTIME_SMOKE" >&2
  exit 1
fi
if [[ ! -x "$STDIOCAP_HELPER" ]]; then
  echo "FAIL: stdiocap helper not executable: $STDIOCAP_HELPER" >&2
  exit 1
fi
if [[ ! -x "$SSLSNIFF_HELPER" ]]; then
  echo "FAIL: sslsniff helper not executable: $SSLSNIFF_HELPER" >&2
  exit 1
fi

echo "==> eBPF live verify: sudo availability"
sudo_refresh

echo "==> eBPF live verify: default process/file-open surface"
rm -f "$DEFAULT_LOG"
set +e
run_sudo_timeout 5s "$HELPER" -m 0 -v >"$DEFAULT_LOG" 2>&1
default_status=$?
set -e
if [[ "$default_status" != "124" ]]; then
  echo "FAIL: default helper exited unexpectedly: $default_status" >&2
  tail -80 "$DEFAULT_LOG" >&2 || true
  exit 1
fi
grep -q "Loaded process_new: trace_fs=0 trace_net=0 trace_signals=0 trace_mem=0" "$DEFAULT_LOG"
grep -q '"event":"FILE_OPEN"' "$DEFAULT_LOG"
grep -q "trace_unlinkat': skipped loading" "$DEFAULT_LOG"
grep -q "trace_bind': skipped loading" "$DEFAULT_LOG"

echo "==> eBPF live verify: fs/net/signals/mem extension surface"
rm -f "$EXTENDED_LOG"
tmp_dir="$(mktemp -d /tmp/mindbridge-ebpf.XXXXXX)"
set +e
run_sudo_timeout 8s "$HELPER" -m 0 --trace-fs --trace-net --trace-signals --trace-mem -v >"$EXTENDED_LOG" 2>&1 &
monitor_pid=$!
sleep 2
echo hi >"$tmp_dir/file"
truncate -s 0 "$tmp_dir/file"
mv "$tmp_dir/file" "$tmp_dir/file2"
python3 -m http.server 0 --bind 127.0.0.1 >/tmp/mindbridge-ebpf-http.log 2>&1 &
http_pid=$!
sleep 1
kill -TERM "$http_pid" 2>/dev/null || true
rm -f "$tmp_dir/file2"
rmdir "$tmp_dir"
wait "$monitor_pid"
extended_status=$?
set -e
if [[ "$extended_status" != "124" ]]; then
  echo "FAIL: extended helper exited unexpectedly: $extended_status" >&2
  tail -120 "$EXTENDED_LOG" >&2 || true
  exit 1
fi
grep -q "Loaded process_new: trace_fs=1 trace_net=1 trace_signals=1 trace_mem=1" "$EXTENDED_LOG"
grep -Eq '"type":"(DIR_CREATE|FILE_TRUNCATE|FILE_RENAME|FILE_DELETE)"' "$EXTENDED_LOG"
grep -Eq '"type":"(NET_BIND|NET_LISTEN|NET_CONNECT)"' "$EXTENDED_LOG"
grep -Eq '"type":"(PROC_FORK|SIGNAL_SEND|SESSION_CREATE)"' "$EXTENDED_LOG"
grep -q '"type":"MMAP_SHARED"' "$EXTENDED_LOG"

echo "==> eBPF live verify: stdiocap stdout/stderr capture"
rm -f "$STDIOCAP_LOG"
(
  for i in 1 2 3 4 5; do
    echo "mindbridge-stdio-stdout-$i"
    echo "mindbridge-stdio-stderr-$i" >&2
    sleep 1
  done
) >/tmp/mindbridge-stdio-target.out 2>/tmp/mindbridge-stdio-target.err &
stdio_pid=$!
sleep 1
set +e
run_sudo_timeout 5s "$STDIOCAP_HELPER" -p "$stdio_pid" --all-fds --max-bytes 256 >"$STDIOCAP_LOG" 2>&1
stdiocap_status=$?
set -e
wait "$stdio_pid" 2>/dev/null || true
if [[ "$stdiocap_status" != "124" ]]; then
  echo "FAIL: stdiocap helper exited unexpectedly: $stdiocap_status" >&2
  tail -120 "$STDIOCAP_LOG" >&2 || true
  exit 1
fi
grep -q '"direction":"WRITE"' "$STDIOCAP_LOG"
grep -Eq 'mindbridge-stdio-(stdout|stderr)' "$STDIOCAP_LOG"

echo "==> eBPF live verify: sslsniff TLS plaintext capture"
rm -f "$SSLSNIFF_LOG"
set +e
run_sudo_timeout 8s "$SSLSNIFF_HELPER" -c curl >"$SSLSNIFF_LOG" 2>&1 &
tls_pid=$!
sleep 2
curl -fsS --max-time 4 https://example.com/ >/tmp/mindbridge-sslsniff-curl.out 2>/tmp/mindbridge-sslsniff-curl.err
curl_status=$?
wait "$tls_pid"
sslsniff_status=$?
set -e
if [[ "$curl_status" != "0" ]]; then
  echo "FAIL: curl HTTPS smoke failed: $curl_status" >&2
  cat /tmp/mindbridge-sslsniff-curl.err >&2 || true
  exit 1
fi
if [[ "$sslsniff_status" != "124" ]]; then
  echo "FAIL: sslsniff helper exited unexpectedly: $sslsniff_status" >&2
  tail -120 "$SSLSNIFF_LOG" >&2 || true
  exit 1
fi
grep -Eq '"function":"(WRITE/SEND|READ/RECV)"' "$SSLSNIFF_LOG"
grep -Eq 'Example Domain|PRI \\* HTTP/2.0' "$SSLSNIFF_LOG"

echo "==> eBPF live verify: MindBridge runtime artifact integration"
cat >"$WRAPPER" <<EOF
#!/usr/bin/env bash
if [[ -n "\${MINDBRIDGE_SUDO_PASSWORD:-}" ]]; then
  printf '%s\n' "\$MINDBRIDGE_SUDO_PASSWORD" | exec sudo -S "$ROOT_DIR/$HELPER" "\$@"
fi
exec sudo "$ROOT_DIR/$HELPER" "\$@"
EOF
chmod +x "$WRAPPER"
cat >"$TLS_WRAPPER" <<EOF
#!/usr/bin/env bash
if [[ -n "\${MINDBRIDGE_SUDO_PASSWORD:-}" ]]; then
  printf '%s\n' "\$MINDBRIDGE_SUDO_PASSWORD" | exec sudo -S "$ROOT_DIR/$SSLSNIFF_HELPER" "\$@"
fi
exec sudo "$ROOT_DIR/$SSLSNIFF_HELPER" "\$@"
EOF
chmod +x "$TLS_WRAPPER"
sudo_refresh
rm -f "$RUNTIME_LOG"
MINDBRIDGE_EBPF_ENABLED=1 \
MINDBRIDGE_EBPF_USE_SUDO=1 \
MINDBRIDGE_EBPF_BINARY="$HELPER" \
MINDBRIDGE_EBPF_TLS_BINARY="$SSLSNIFF_HELPER" \
MINDBRIDGE_EBPF_TRACE_TLS=1 \
MINDBRIDGE_EBPF_TLS_COMMANDS=curl \
MINDBRIDGE_EBPF_TRACE_FS=1 \
MINDBRIDGE_EBPF_TRACE_NET=1 \
MINDBRIDGE_EBPF_TRACE_SIGNALS=1 \
MINDBRIDGE_EBPF_TRACE_MEM=1 \
MINDBRIDGE_EBPF_RUNTIME_TLS_SMOKE=1 \
"$RUNTIME_SMOKE" >"$RUNTIME_LOG" 2>&1
grep -q '"ok": true' "$RUNTIME_LOG"

echo "PASS: eBPF live helper verified"
echo "default_log=$DEFAULT_LOG"
echo "extended_log=$EXTENDED_LOG"
echo "stdiocap_log=$STDIOCAP_LOG"
echo "sslsniff_log=$SSLSNIFF_LOG"
echo "runtime_log=$RUNTIME_LOG"
