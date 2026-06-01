# Sandboxed Browser QA Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a non-K8s OpenSandbox Playwright Browser QA demo that validates MindBridge's browser-facing multi-agent flow from an isolated sandbox and writes auditable QA artifacts.

**Architecture:** The first version is intentionally outside the runtime tool path: a shell script starts a Python OpenSandbox client, creates a Playwright-capable sandbox, runs a browser QA script against the local MindBridge frontend/gateway, and downloads `qa_result.json` plus screenshots into `.mindbridge/sandbox_qa/<run_id>/`. This follows OpenSandbox `examples/playwright` while avoiding changes to Counselor/Evaluator/session business logic.

**Tech Stack:** Bash, Python OpenSandbox SDK, Playwright in the OpenSandbox image, MindBridge demo frontend on `5173`, Gateway on `8090`, JSON artifacts.

---

### Task 1: Add Browser QA Runner Skeleton

**Files:**
- Create: `sandbox/browser_qa/mindbridge_browser_qa.py`
- Create: `sandbox/browser_qa/test_mindbridge_browser_qa.py`

- [ ] **Step 1: Write tests for target URL construction and artifact naming**

```bash
python3 -m unittest sandbox.browser_qa.test_mindbridge_browser_qa -v
```

Expected before implementation: import or attribute failure because the module does not exist.

- [ ] **Step 2: Implement pure helpers**

Create `sandbox/browser_qa/mindbridge_browser_qa.py` with helper functions that resolve frontend/gateway URLs, build a stable `qa_result.json` object, and compute sandbox-visible URLs from CLI arguments.

- [ ] **Step 3: Run tests**

```bash
python3 -m unittest sandbox.browser_qa.test_mindbridge_browser_qa -v
```

Expected: all helper tests pass on Python 3.8 without importing OpenSandbox.

### Task 2: Add OpenSandbox Playwright Execution Path

**Files:**
- Modify: `sandbox/browser_qa/mindbridge_browser_qa.py`

- [ ] **Step 1: Add a lazy OpenSandbox import**

The script must import `opensandbox` only inside the execution function so unit tests work on hosts that have not installed the SDK.

- [ ] **Step 2: Upload and run a Playwright script**

The script will create a sandbox with an OpenSandbox Playwright image, write a JavaScript QA script inside the sandbox, run it with Node, and read back `qa_result.json` plus `mindbridge-browser-qa.png`.

- [ ] **Step 3: Write local artifacts**

The script writes artifacts under `.mindbridge/sandbox_qa/<run_id>/` by default and exits nonzero when the QA status is not `passed`.

### Task 3: Add Shell Entrypoints and Documentation

**Files:**
- Create: `scripts/run_sandbox_browser_qa.sh`
- Create: `sandbox/browser_qa/README.md`
- Modify: `docs/harness_engineering.md`
- Modify: `mindbridge_harness/configs/feature_status.json`

- [ ] **Step 1: Add shell entrypoint**

The shell script checks the frontend and gateway health endpoints, then calls the Python runner with configurable `MINDBRIDGE_SANDBOX_FRONTEND_URL`, `MINDBRIDGE_SANDBOX_GATEWAY_URL`, `MINDBRIDGE_OPENSANDBOX_IMAGE`, and `MINDBRIDGE_SANDBOX_QA_OUTPUT_DIR`.

- [ ] **Step 2: Add docs**

Document why this demo uses OpenSandbox Playwright, how it differs from K8s deployment, and how to run it.

- [ ] **Step 3: Add feature status**

Add `sandboxed_browser_qa` with status `partial` until live OpenSandbox verification is captured on this machine.

### Task 4: Verify

**Files:**
- None

- [ ] **Step 1: Run helper tests**

```bash
python3 -m unittest sandbox.browser_qa.test_mindbridge_browser_qa -v
```

Expected: tests pass.

- [ ] **Step 2: Syntax-check shell and Python**

```bash
bash -n scripts/run_sandbox_browser_qa.sh
python3 -m py_compile sandbox/browser_qa/mindbridge_browser_qa.py
```

Expected: both commands exit 0.

- [ ] **Step 3: Run full MindBridge verification when implementation affects required docs/status**

```bash
bash scripts/verify_mindbridge.sh
```

Expected: exits 0. If local model or environment dependencies block it, capture the exact failing command/output and do not claim full verification.

### Self-Review

- Spec coverage: The plan covers non-K8s OpenSandbox Playwright Browser QA, local artifacts, screenshots, `qa_result.json`, docs, and feature status.
- Placeholder scan: No TBD/TODO placeholders are used.
- Type consistency: Helper and artifact names are consistent across tasks.
