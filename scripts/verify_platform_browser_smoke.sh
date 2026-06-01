#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if ! command -v node >/dev/null 2>&1; then
  echo "SKIP: node is not available; cannot run platform browser smoke"
  exit 0
fi

node -e "require('playwright')" >/dev/null 2>&1 || {
  echo "SKIP: playwright is not available to node; cannot run platform browser smoke"
  exit 0
}

cmake --build build --target mindbridge_platform -j2

pick_free_port() {
  python3 - <<'PY'
import socket
with socket.socket() as s:
    s.bind(("127.0.0.1", 0))
    print(s.getsockname()[1])
PY
}

PLATFORM_PORT="${MINDBRIDGE_PLATFORM_SMOKE_PLATFORM_PORT:-$(pick_free_port)}"
FRONTEND_PORT="${MINDBRIDGE_PLATFORM_SMOKE_FRONTEND_PORT:-$(pick_free_port)}"
SMOKE_ROOT=".mindbridge/platform_browser"
RUN_ID="run-browser-smoke"
rm -rf "$SMOKE_ROOT"
mkdir -p "$SMOKE_ROOT/runs/$RUN_ID"

cat > "$SMOKE_ROOT/runs/$RUN_ID/trace.jsonl" <<'EOF'
{"event":"run_started","wall_time_ns":1700000000000000000,"task_id":"browser-smoke"}
{"event":"model_requested","wall_time_ns":1700000000100000000,"attempt":1}
{"event":"run_finished","wall_time_ns":1700000000200000000,"status":"completed"}
EOF
cat > "$SMOKE_ROOT/runs/$RUN_ID/task_state.json" <<'EOF'
{"run_id":"run-browser-smoke","status":"completed","attempts":1,"tool_steps":0,"stop_reason":"completed"}
EOF
cat > "$SMOKE_ROOT/runs/$RUN_ID/report.json" <<'EOF'
{"ok":true,"prompt_metadata":{}}
EOF

MINDBRIDGE_PLATFORM_DB_PATH="$SMOKE_ROOT/platform/platform.sqlite" \
MINDBRIDGE_RUN_ROOT="$SMOKE_ROOT" \
  ./build/mindbridge_harness/mindbridge_platform "$PLATFORM_PORT" > "$SMOKE_ROOT/platform.log" 2>&1 &
platform_pid=$!

python3 scripts/serve_demo_frontend.py --port "$FRONTEND_PORT" --directory frontend/demo > "$SMOKE_ROOT/frontend.log" 2>&1 &
frontend_pid=$!

cleanup() {
  status=$?
  kill "$platform_pid" "$frontend_pid" 2>/dev/null || true
  if [[ "$status" -eq 0 ]]; then
    rm -rf "$SMOKE_ROOT"
  else
    echo "Platform browser smoke artifacts kept in $SMOKE_ROOT" >&2
  fi
}
trap cleanup EXIT

for _ in $(seq 1 60); do
  if curl -fsS "http://127.0.0.1:${PLATFORM_PORT}/api/platform/health" >/dev/null 2>&1 &&
     curl -fsS "http://127.0.0.1:${FRONTEND_PORT}/index.html" >/dev/null 2>&1; then
    break
  fi
  sleep 0.25
done

curl -fsS "http://127.0.0.1:${PLATFORM_PORT}/api/platform/health" >/dev/null
curl -fsS "http://127.0.0.1:${FRONTEND_PORT}/index.html" >/dev/null

PLATFORM_PORT="$PLATFORM_PORT" FRONTEND_PORT="$FRONTEND_PORT" node <<'NODE'
const { chromium } = require('playwright');

(async () => {
  const platformPort = process.env.PLATFORM_PORT;
  const frontendPort = process.env.FRONTEND_PORT;
  const browser = await chromium.launch({ headless: true });
  const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
  const errors = [];
  page.on('pageerror', (err) => errors.push(err.message));
  page.on('console', (msg) => {
    if (msg.type() === 'error' && !msg.text().includes('Failed to load resource')) {
      errors.push(msg.text());
    }
  });

  await page.goto(`http://127.0.0.1:${frontendPort}/index.html?platform=http://127.0.0.1:${platformPort}`, {
    waitUntil: 'domcontentloaded',
  });
  await page.click('#tab-platform');
  await page.fill('#platform-workspace-id', 'ws-browser-smoke');
  await page.fill('#platform-owner-id', 'browser-user');
  await page.locator('.platform-panel').filter({ hasText: 'Workspaces' }).getByRole('button', { name: 'Create' }).click();
  await page.locator('#platform-workspace-list .platform-row-title', { hasText: /^ws-browser-smoke$/ }).waitFor({ timeout: 5000 });

  await page.fill('#platform-session-id', 'session-browser-smoke');
  await page.fill('#platform-conversation-id', 'conv-browser-smoke');
  await page.locator('.platform-panel').filter({ hasText: 'Sessions' }).getByRole('button', { name: 'Start' }).click();
  await page.locator('#platform-session-list .platform-row-title', { hasText: /^session-browser-smoke$/ }).waitFor({ timeout: 5000 });

  const conversationId = await page.evaluate(() => platformConversationId());
  if (conversationId !== 'conv-browser-smoke') {
    throw new Error(`expected platform conversation id, got ${conversationId}`);
  }

  await page.evaluate(() => attachPlatformRun('run-browser-smoke'));
  await page.locator('#platform-event-list .platform-row-title', { hasText: /^model\.requested$/ }).waitFor({ timeout: 5000 });
  await page.locator('#platform-artifact-list .platform-row-title', { hasText: /^report\.json$/ }).waitFor({ timeout: 5000 });
  await page.screenshot({ path: '/tmp/mindbridge-platform-browser-smoke.png', fullPage: true });

  await browser.close();
  if (errors.length) {
    throw new Error(errors.join('\n'));
  }
  console.log('PASS: platform browser smoke');
})();
NODE
