#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

export MINDBRIDGE_MODEL_PROVIDER="${MINDBRIDGE_MODEL_PROVIDER:-openai_compatible}"
export MINDBRIDGE_MODEL_BASE_URL="${MINDBRIDGE_MODEL_BASE_URL:-https://dashscope.aliyuncs.com/compatible-mode/v1}"
export MINDBRIDGE_MODEL_NAME="${MINDBRIDGE_MODEL_NAME:-qwen3.6-plus}"

if [[ -z "${MINDBRIDGE_MODEL_API_KEY:-}" ]]; then
  read -rsp "MINDBRIDGE_MODEL_API_KEY: " MINDBRIDGE_MODEL_API_KEY
  echo
  export MINDBRIDGE_MODEL_API_KEY
fi

if [[ -z "${MINDBRIDGE_MODEL_API_KEY:-}" ]]; then
  echo "FAIL: MINDBRIDGE_MODEL_API_KEY is required"
  exit 1
fi

if [[ ! -d node_modules/@playwright/test ]]; then
  echo "FAIL: @playwright/test is not installed. Run: npm install --save-dev @playwright/test && npx playwright install chromium"
  exit 1
fi

echo "==> Full live flow: start cloud services"
bash scripts/start_mindbridge_cloud_storage.sh >/dev/null
source .mindbridge/cloud_storage/live.env

export LD_LIBRARY_PATH="$ROOT_DIR/.deps/cloud_storage/stage/usr/lib64:$ROOT_DIR/.deps/cloud_storage/stage/usr/lib:$ROOT_DIR/.deps/hiredis/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"

echo "==> Full live flow: build formal targets"
cmake --build build --target \
  mindbridge_gateway mindbridge_orchestrator mindbridge_counselor mindbridge_evaluator mindbridge_net_demo -j2 >/dev/null

python3 - <<'PY'
import base64
import hashlib
import json
import os
import socket
import subprocess
import tempfile
import textwrap
import time
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path("/home/hang/work/a2a/mindbridge_project")
TMP = Path(tempfile.mkdtemp(prefix="mindbridge_live_model."))
ports = {
    "gateway": int(os.environ.get("MINDBRIDGE_FULLFLOW_GATEWAY_PORT", "19190")),
    "orchestrator": int(os.environ.get("MINDBRIDGE_FULLFLOW_ORCHESTRATOR_PORT", "19109")),
    "counselor": int(os.environ.get("MINDBRIDGE_FULLFLOW_COUNSELOR_PORT", "19110")),
    "evaluator": int(os.environ.get("MINDBRIDGE_FULLFLOW_EVALUATOR_PORT", "19111")),
    "frontend": int(os.environ.get("MINDBRIDGE_FULLFLOW_FRONTEND_PORT", "19173")),
}
procs = []

def note(name, ok, detail=""):
    print(("PASS" if ok else "FAIL") + f": {name}" + (f" - {detail}" if detail else ""), flush=True)
    if not ok:
        raise SystemExit(1)

def start(name, args, extra_env=None):
    env = os.environ.copy()
    env.update({
        "MINDBRIDGE_STORAGE_BACKEND": "cloud",
        "MINDBRIDGE_STATE_BACKEND": "mysql",
        "MINDBRIDGE_AUTH_REQUIRED": "true",
        "MINDBRIDGE_AUTH_DB_PATH": str(TMP / "auth.sqlite"),
        "MINDBRIDGE_AUTH_BOOTSTRAP_USERS": json.dumps([
            {"user_id": "alice", "display_name": "Alice", "role": "user", "access_key_id": "ak-alice", "secret_key": "alice-secret"},
            {"user_id": "bob", "display_name": "Bob", "role": "user", "access_key_id": "ak-bob", "secret_key": "bob-secret"},
        ]),
        "MINDBRIDGE_ORCHESTRATOR_URL": f"http://127.0.0.1:{ports['orchestrator']}",
        "MINDBRIDGE_COUNSELOR_URL": f"http://127.0.0.1:{ports['counselor']}",
        "MINDBRIDGE_EVALUATOR_URL": f"http://127.0.0.1:{ports['evaluator']}",
        "MINDBRIDGE_STATE_DB_PATH": str(TMP / "unused_state.sqlite"),
        "MINDBRIDGE_STORAGE_STATE_DB_PATH": str(TMP / "unused_storage_state.sqlite"),
        "MINDBRIDGE_NET_IO_THREADS": "2",
    })
    if extra_env:
        env.update(extra_env)
    log = open(TMP / f"{name}.log", "w")
    proc = subprocess.Popen(args, cwd=str(ROOT), env=env, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    procs.append((name, proc, log))

def wait_port(name, port, timeout=25):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return
        except OSError:
            time.sleep(0.15)
    raise RuntimeError(f"{name} did not listen on {port}")

def request(method, url, payload=None, timeout=180):
    data = None if payload is None else json.dumps(payload, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "text/plain;charset=utf-8"}, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as response:
        raw = response.read().decode("utf-8")
    try:
        return json.loads(raw)
    except Exception:
        return raw

def get(url):
    return request("GET", url)

def post(url, payload):
    return request("POST", url, payload)

def run_playwright(gateway_url, frontend_url):
    upload = TMP / "playwright-upload.txt"
    upload.write_text(f"browser upload {time.time_ns()}", encoding="utf-8")
    script = TMP / "browser-flow.js"
    script.write_text(textwrap.dedent(f"""
        const {{ chromium }} = require('{(ROOT / "node_modules" / "@playwright" / "test").as_posix()}');
        (async () => {{
          const browser = await chromium.launch({{ headless: true }});
          const page = await browser.newPage();
          page.setDefaultTimeout(90000);
          await page.goto('{frontend_url}/index.html?gateway=' + encodeURIComponent('{gateway_url}'));
          await page.fill('#access-key-input', 'ak-alice');
          await page.fill('#secret-key-input', 'alice-secret');
          await page.click('.login-btn');
          await page.waitForFunction(() => document.querySelector('#auth-user-label')?.textContent.includes('Alice'));
          await page.fill('#chat-input', '浏览器端到端测试，请简短回复。');
          await page.click('#send-btn');
          await page.waitForFunction(() => {{
            const v = document.querySelector('#d-runid')?.textContent?.trim();
            return v && v !== '-';
          }});
          await page.setInputFiles('#storage-file-input', '{upload.as_posix()}');
          await page.click('#storage-upload-btn');
          await page.waitForFunction(() => document.querySelector('#storage-list')?.textContent.includes('playwright-upload.txt'));
          console.log('PASS: playwright browser login/chat/upload');
          await browser.close();
        }})().catch(async (e) => {{
          console.error('FAIL: playwright browser flow');
          console.error(e);
          process.exit(1);
        }});
    """), encoding="utf-8")
    result = subprocess.run(["node", str(script)], cwd=str(ROOT), text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
    print(result.stdout, end="")
    note("Playwright browser flow", result.returncode == 0)

try:
    start("counselor", ["./build/mindbridge_harness/mindbridge_counselor", str(ports["counselor"])], {"MINDBRIDGE_TRACE_PATH": str(TMP / "counselor_trace.jsonl"), "MINDBRIDGE_STATE_NODE_ID": "liveflow-counselor"})
    start("evaluator", ["./build/mindbridge_harness/mindbridge_evaluator", str(ports["evaluator"])], {"MINDBRIDGE_TRACE_PATH": str(TMP / "evaluator_trace.jsonl")})
    start("orchestrator", ["./build/mindbridge_harness/mindbridge_orchestrator", str(ports["orchestrator"])])
    start("gateway", ["./build/mindbridge_harness/mindbridge_gateway", str(ports["gateway"])], {"MINDBRIDGE_TRACE_PATH": str(TMP / "gateway_trace.jsonl"), "MINDBRIDGE_STORAGE_STATE_NODE_ID": "liveflow-gateway-storage"})
    start("frontend", ["python3", "scripts/serve_demo_frontend.py", "--port", str(ports["frontend"]), "--directory", "frontend/demo"])
    for name, port in ports.items():
        if name != "model":
            wait_port(name, port)

    gw = f"http://127.0.0.1:{ports['gateway']}"
    orch = f"http://127.0.0.1:{ports['orchestrator']}"
    fe = f"http://127.0.0.1:{ports['frontend']}"

    health = get(gw + "/api/health")
    note("gateway health/asio/cloud", health.get("ok") and health.get("network", {}).get("backend") == "asio_beast_client" and health.get("storage", {}).get("backend") == "cloud")
    orch_health = get(orch + "/api/health")
    note("orchestrator health/asio routing", orch_health.get("ok") and orch_health.get("network", {}).get("backend") == "asio_beast_client")

    unauth = post(gw + "/", {"jsonrpc": "2.0", "id": "noauth", "method": "message/send", "params": {"message": {"role": "user", "contextId": "c1", "parts": [{"kind": "text", "text": "hello"}]}}})
    note("auth blocks unauthenticated requests", unauth.get("error", {}).get("code") == 401)
    login_a = post(gw + "/api/auth/login", {"access_key_id": "ak-alice", "secret_key": "alice-secret"})
    login_b = post(gw + "/api/auth/login", {"access_key_id": "ak-bob", "secret_key": "bob-secret"})
    token_a, token_b = login_a.get("token"), login_b.get("token")
    note("login issues isolated tokens", bool(token_a and token_b and token_a != token_b))

    def rpc(text, rid, token, context="live-session"):
        return post(gw + "/?auth_token=" + urllib.parse.quote(token), {
            "jsonrpc": "2.0",
            "id": rid,
            "method": "message/send",
            "params": {"message": {"role": "user", "contextId": context, "parts": [{"kind": "text", "text": text}]}},
        })

    normal = rpc("我今天压力很大，想聊聊怎么缓解。", "normal-live", token_a)
    run_id = normal.get("result", {}).get("run_id", "")
    note("gateway->orchestrator->counselor->live model", bool(run_id and normal.get("result", {}).get("message")))
    high = rpc("我想自杀，感觉要结束生命。", "risk-live", token_a)
    note("high-risk request dispatches evaluator", high.get("result", {}).get("orchestration", {}).get("evaluator", {}).get("called") is True)

    stream_body = {
        "jsonrpc": "2.0",
        "id": "stream-live",
        "method": "message/stream",
        "params": {"message": {"role": "user", "contextId": "live-session", "parts": [{"kind": "text", "text": "请用一句话鼓励我。"}]}},
    }
    stream_raw = request("POST", gw + "/?auth_token=" + urllib.parse.quote(token_a), stream_body)
    note("SSE stream emits run/token/final events", all(item in stream_raw for item in ["run_started", "message_done", "final_result"]))

    ended = post(gw + "/?auth_token=" + urllib.parse.quote(token_a), {"jsonrpc": "2.0", "id": "end-live", "method": "session/end", "params": {"contextId": "live-session"}})
    note("session/end invokes evaluator summary", ended.get("result", {}).get("ended") is True and ended.get("result", {}).get("evaluator", {}).get("called") is True)

    conv_a = get(gw + "/api/conversations?auth_token=" + urllib.parse.quote(token_a))
    conv_b = get(gw + "/api/conversations?auth_token=" + urllib.parse.quote(token_b))
    note("conversation ownership is user isolated", len(conv_a.get("conversations", [])) > 0 and len(conv_b.get("conversations", [])) == 0)

    payload = f"live full flow attachment {time.time_ns()}".encode("utf-8")
    md5 = hashlib.md5(payload).hexdigest()
    upload = post(gw + "/api/storage/upload?auth_token=" + urllib.parse.quote(token_a), {
        "conversation_id": "live-session",
        "run_id": run_id,
        "filename": "live-full.txt",
        "md5": md5,
        "type": "text/plain",
        "data_base64": base64.b64encode(payload).decode("ascii"),
    })
    note("FastDFS upload returns project URL", upload.get("ok") and upload.get("file", {}).get("url", "").startswith("http://127.0.0.1:80/"))
    download = get(gw + f"/api/storage/files/{md5}/download?auth_token={urllib.parse.quote(token_a)}&conversation_id=live-session")
    note("FastDFS download bytes match upload", download.get("ok") and base64.b64decode(download.get("data_base64", "")) == payload)

    mysql_out = subprocess.check_output([
        "docker", "exec", os.environ["MINDBRIDGE_MYSQL_CONTAINER"],
        "mysql", "-u" + os.environ["MINDBRIDGE_MYSQL_USER"], "-p" + os.environ["MINDBRIDGE_MYSQL_PASSWORD"],
        os.environ["MINDBRIDGE_MYSQL_DATABASE"], "-N", "-e",
        "SELECT COUNT(*) FROM mb_state_records WHERE namespace_name IN ('chat_memory','risk_memory'); "
        "SELECT COUNT(*) FROM mb_state_changes WHERE namespace_name IN ('chat_memory','risk_memory'); "
        "SELECT COUNT(*) FROM mb_state_records WHERE namespace_name='run_artifact' AND state_key='file_" + md5 + "'; "
        "SELECT COUNT(*) FROM mb_file_info WHERE md5='" + md5 + "';"
    ], stderr=subprocess.DEVNULL).decode().strip().splitlines()
    counts = [int(x) for x in mysql_out[:4]]
    note("MySQL state has chat/risk records and changes", counts[0] >= 2 and counts[1] >= 2, str(counts))
    note("run_artifact and file metadata are durable", counts[2] >= 1 and counts[3] >= 1, str(counts))

    cfg = TMP / "net_demo_live.json"
    cfg.write_text(json.dumps({
        "gateway_url": gw,
        "websocket_url": f"ws://127.0.0.1:{ports['gateway']}/api/demo/ws",
        "client_count": 2,
        "request_frequency_ms": 200,
        "duration_sec": 2,
        "enable_http": False,
        "enable_sse": False,
        "enable_websocket": True,
        "enable_subscribe": True,
    }), encoding="utf-8")
    net_demo = subprocess.run(["./build/mindbridge_harness/mindbridge_net_demo", str(cfg)], cwd=str(ROOT), text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    note("Asio websocket load client", net_demo.returncode == 0)

    run_playwright(gw, fe)
    print("PASS: full live model flow completed")
    print("Artifacts/logs:", TMP)
finally:
    for name, proc, log in reversed(procs):
        if proc.poll() is None:
            proc.terminate()
    for name, proc, log in reversed(procs):
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
        log.close()
PY
