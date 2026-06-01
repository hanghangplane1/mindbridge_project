# MindBridge Sandboxed Browser QA

This demo adapts OpenSandbox's Playwright browser automation pattern to MindBridge.
It is not part of the Counselor/Evaluator runtime path and it does not require
Kubernetes. The sandbox runs a headless Chromium/Playwright QA script against the
MindBridge frontend and gateway, then writes auditable artifacts locally.

## Why this scenario fits

MindBridge is a browser-facing multi-agent system: `frontend/demo` talks to the
Gateway, which routes to Orchestrator, Counselor, and Evaluator. The user-visible
contract includes SSE chat, Stored History, run artifact APIs, and the
Observability tab. Those boundaries are better checked with a real browser than
with isolated unit tests.

OpenSandbox provides the isolated browser environment. MindBridge keeps the QA
story project-specific by validating its counseling workflow and writing
artifacts under `.mindbridge/sandbox_qa/<run_id>/`.

Live QA is not a screenshot smoke test. The screenshot is only an attachment.
The pass/fail contract is `qa_result.json`, which records business checks:

- `consult_response_rendered`: the normal counseling demo produces a counselor response.
- `risk_response_rendered`: the high-risk counseling demo produces a response.
- `risk_route_recorded`: high-risk evidence is visible through `risk_memory`, a high/critical
  session assessment, or the run risk metadata.
- `session_assessment_completed`: explicit `session/end` returns a staged assessment.
- `stored_history_visible` / `stored_history_turns`: Gateway visible history can read the same
  conversation, including role, namespace, and risk-level evidence.
- `run_artifact_contract`: latest run artifacts expose completed state and required trace events.
- `observability_business_events`: observability includes model request and run completion events.

## Prerequisites

- MindBridge demo running:

```bash
bash scripts/start_demo.sh
```

- OpenSandbox server/runtime available locally.
- Python environment with the OpenSandbox SDK installed. OpenSandbox currently
  expects a modern Python runtime; if the system `python3` is too old, set
  `MINDBRIDGE_SANDBOX_PYTHON` to a Python 3.10+ interpreter.

OpenSandbox's Playwright example starts a local server and SDK client roughly as:

```bash
uv pip install opensandbox-server opensandbox
opensandbox-server init-config ~/.sandbox.toml --example docker
opensandbox-server
```

The runner follows that SDK shape: `ConnectionConfig(domain=...)`,
`Sandbox.create(image, connection_config=..., env=...)`, then a Python
Playwright script inside the sandbox.

## Run

```bash
bash scripts/run_sandbox_browser_qa.sh
```

Artifact-only dry run, useful before OpenSandbox is installed:

```bash
MINDBRIDGE_SANDBOX_QA_DRY_RUN=1 bash scripts/run_sandbox_browser_qa.sh
```

Dry run validates the MindBridge-side artifact schema and script wiring. It does
not create an OpenSandbox sandbox, launch Chromium, or prove browser automation.

Verification command:

```bash
bash scripts/verify_sandbox_browser_qa.sh
```

The verifier runs unit tests, syntax checks, and dry-run artifact validation. It
skips live OpenSandbox execution unless explicitly enabled:

```bash
MINDBRIDGE_SANDBOX_QA_LIVE=1 bash scripts/verify_sandbox_browser_qa.sh
```

For deterministic local verification without a real model provider, start the
fake OpenAI-compatible server and point `scripts/start_demo.sh` at it:

```bash
python3 sandbox/browser_qa/fake_openai_server.py --port 18080
MINDBRIDGE_MODEL_PROVIDER=openai_compatible \
MINDBRIDGE_MODEL_BASE_URL=http://127.0.0.1:18080 \
MINDBRIDGE_MODEL_API_KEY=dummy \
MINDBRIDGE_MODEL_NAME=fake-mindbridge \
bash scripts/start_demo.sh
```

Real DashScope live verification should use the dedicated wrapper so the API key
is only read from the current shell or an interactive hidden prompt:

```bash
export MINDBRIDGE_MODEL_NAME=qwen3.6-flash
bash scripts/verify_sandbox_browser_qa_dashscope_live.sh
```

The wrapper starts the DashScope demo with `MINDBRIDGE_REQUIRE_REMOTE_MODEL=1`
and then runs `MINDBRIDGE_SANDBOX_QA_LIVE=1 bash scripts/verify_sandbox_browser_qa.sh`.
It prints only whether the key is set; it does not echo or save the key.

Useful overrides:

```bash
export MINDBRIDGE_FRONTEND_URL=http://127.0.0.1:5173/index.html
export MINDBRIDGE_GATEWAY_URL=http://127.0.0.1:8090
export MINDBRIDGE_SANDBOX_FRONTEND_URL=http://172.17.0.1:5173/index.html
export MINDBRIDGE_SANDBOX_GATEWAY_URL=http://172.17.0.1:8090
export MINDBRIDGE_SANDBOX_PYTHON=python3.11
export MINDBRIDGE_OPENSANDBOX_IMAGE=opensandbox/playwright:latest
export MINDBRIDGE_OPENSANDBOX_DOMAIN=localhost:8080
```

On Linux Docker bridge networks, `172.17.0.1` is usually the host gateway. The
shell entrypoint auto-detects `docker0`; override the two `MINDBRIDGE_SANDBOX_*`
URLs if your Docker runtime uses a different host address.

Artifacts:

- `qa_result.json`
- `mindbridge-browser-qa.png`
- `mindbridge-browser-qa.txt` in dry-run mode

Default output root:

```text
.mindbridge/sandbox_qa/<run_id>/
```

## Scope

This version deliberately avoids Kubernetes and avoids wiring the sandbox into
`ToolRegistry`. It proves MindBridge's browser-facing counseling workflow from an
isolated Playwright sandbox. Later phases can move the same QA runner into a K8s
runtime, add NetworkPolicy, or expose it through a governed MindBridge tool.
