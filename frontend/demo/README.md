# MindBridge Demo Dashboard

Static frontend cockpit for the MindBridge Harness Runtime. Zero dependencies — open the HTML file directly in a browser.

## Quick Start

### 1. Start the backend services

```bash
# From mindbridge_project/
bash scripts/start_demo.sh
```

Stop the demo services:

```bash
bash scripts/stop_demo.sh
```

For build-only verification:

```bash
bash scripts/verify_mindbridge.sh
```

For the Platform tab browser smoke only:

```bash
bash scripts/verify_platform_browser_smoke.sh
```

### 2. Open the frontend

```bash
# start_demo.sh serves the dashboard here:
http://127.0.0.1:5173/index.html
```

### 3. Verify

- Top status bar: all 4 chips should turn green (Gateway / Orchestrator / Counselor / Evaluator)
- Top view switcher: **Agent Cockpit** keeps the original runtime dashboard; **Platform** opens Workspace/Session control; **Cloud Storage** opens the conversation asset dashboard
- Click "Normal" demo button: chat shows agent reply, timeline shows all steps, details panel shows intent/risk/run info
- Click "High Risk" demo button: Evaluator node shows "active" (not skipped), risk level is high
- Click "End" after one or more turns: Evaluator summarizes the session and attempts to dispatch the assessment through MCP email
- Click "Arch" demo button: shows architecture overview without calling the backend
- In **Cloud Storage**, upload a file and verify the stats, recent uploads, local search, file list, and open/download action update

## Demo Script (Interview Talking Points)

### Scenario A: Normal Consultation

1. Click the **Normal** button
2. Point to the timeline: Gateway → Orchestrator → **Evaluator: skipped** → Counselor → RunStore
3. Point to details: `intent: CONSULT`, `risk_level: low`, `Evaluator: not called`
4. Explain: "The orchestrator classified this as low-risk, so the evaluator was skipped. The counselor handled it directly."

### Scenario B: High-Risk Request

1. Click the **High Risk** button
2. Point to the timeline: Gateway → Orchestrator → **Evaluator: active** → Counselor → RunStore
3. Point to details: `intent: CRISIS`, `risk_level: high`, `Evaluator: CALLED`
4. Explain: "The orchestrator detected crisis intent. It dispatched to the evaluator first for risk assessment, then forwarded to the counselor with evaluation context."

### Scenario C: Harness Architecture

1. Click the **Arch** button
2. Walk through the 7 pipeline nodes and the constraint points list
3. Explain: "This is the full Harness loop — every request passes through these gates. Prompt policy, risk policy, tool registry, permission checks, and run persistence are all enforced."

### Scenario D: End Session Assessment

1. Send one or more consultation messages
2. Click the **End** button
3. Point to the timeline: Gateway -> Orchestrator transcript -> Evaluator session summary -> MCP email
4. Explain: "The evaluator now has a second role: it can analyze the recent session at explicit close and send the assessment through the MCP email path."

### Scenario E: Cloud Storage Dashboard

1. Click **Cloud Storage** in the top view switcher
2. Point to the four storage stats: total files, storage used, image count, and other file count
3. Upload a file through the drag/drop area and watch hashing/upload progress
4. Use search to filter by file name or type, then open an uploaded file
5. Explain: "The dashboard borrows YunCunChu's resource-home workflow but keeps MindBridge's runtime cockpit style and uses Gateway `/api/storage/*` endpoints."

### Scenario F: Agent Platform MVP

1. Start `mindbridge_platform` on port `8077` or open the dashboard with `?platform=http://host:8077`
2. Click **Platform** in the top view switcher
3. Create a Workspace, then start a Session bound to a conversation id
4. Send a chat turn from Agent Cockpit; the selected Platform Session supplies the conversation id and receives the generated run id
5. Select the Session and inspect Universal Events plus controlled artifacts
6. Explain: "This is the cloud-native control plane view. It manages Workspace and Session inventory while the data plane remains Gateway -> Orchestrator -> Counselor/Evaluator."

## API Endpoints (Read-Only Inspector)

| Endpoint | Description |
|----------|-------------|
| `GET /api/health` | Gateway health check |
| `GET /api/demo/runs/latest` | Latest run artifacts (task_state, report, trace) |
| `GET /api/demo/runs/{run_id}` | Specific run artifacts |
| `GET /api/demo/feature-status` | Feature implementation status |
| `GET /api/storage/files` | User/conversation-scoped storage file list |
| `POST /api/storage/upload` | Small file upload |
| `POST /api/storage/chunks/*` | Chunk init/upload/merge for large files |
| `GET /api/storage/files/{md5}/download` | Open or download a stored file |
| `GET /api/platform/workspaces` | Platform Workspace inventory |
| `POST /api/platform/workspaces` | Create a Platform Workspace |
| `GET /api/platform/sessions` | Platform Session inventory |
| `POST /api/platform/sessions` | Start a controlled Agent Session |
| `GET /api/platform/sessions/{session_id}/events` | Universal Event stream for a Session |
| `GET /api/platform/sessions/{session_id}/artifacts` | Controlled run artifact list for a Session |

## Technical Notes

- **No CORS preflight**: All POST requests use `Content-Type: text/plain;charset=utf-8` to avoid triggering OPTIONS requests, since the C++ HttpServer does not implement OPTIONS.
- **No build step**: Pure HTML + CSS + JS. No npm, no bundler, no framework.
- **Gateway URL**: Defaults to `http://127.0.0.1:8090`. Change `GATEWAY` in `app.js` to point elsewhere.
- **Platform URL**: Defaults to `http://127.0.0.1:8077`. Use `?platform=...` or the Platform URL field to point at a remote control plane.
- **No API keys exposed**: The frontend does not read, store, or display any API keys. Keys are configured via environment variables on the backend only.
- **Cloud Storage scope**: The dashboard is scoped to the demo conversation id `demo-session`; sharing, transfer-save, deletion, download rankings, and AI semantic search are intentionally not exposed in this view yet.
- **Platform scope**: MVP exposes controlled Workspace/Session inventory, selected-session chat context, Universal Events, and run artifacts only. It does not expose arbitrary terminal commands or full server filesystem browsing.
