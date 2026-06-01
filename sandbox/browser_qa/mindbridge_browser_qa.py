#!/usr/bin/env python3
"""Run MindBridge browser QA inside an OpenSandbox Playwright sandbox."""

import argparse
import asyncio
import datetime
import json
import os
import textwrap
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional


DEFAULT_FRONTEND_URL = "http://127.0.0.1:5173/index.html"
DEFAULT_GATEWAY_URL = "http://127.0.0.1:8090"
DEFAULT_IMAGE = "opensandbox/playwright:latest"
DEFAULT_OUTPUT_ROOT = ".mindbridge/sandbox_qa"
RESULT_NAME = "qa_result.json"
SCREENSHOT_NAME = "mindbridge-browser-qa.png"
DRY_RUN_NOTE_NAME = "mindbridge-browser-qa.txt"
REMOTE_WORK_DIR = "/home/playwright"


@dataclass
class CheckResult:
    name: str
    ok: bool
    detail: str

    def to_json(self) -> Dict[str, Any]:
        return {"name": self.name, "ok": self.ok, "detail": self.detail}


@dataclass
class BrowserQaConfig:
    frontend_url: str
    gateway_url: str
    sandbox_frontend_url: str
    sandbox_gateway_url: str
    output_dir: Path
    run_id: str
    image: str
    timeout_sec: int


def make_run_id() -> str:
    return time.strftime("%Y%m%dT%H%M%S", time.gmtime())


def resolve_config(
    *,
    frontend_url: str,
    gateway_url: str,
    sandbox_frontend_url: str,
    sandbox_gateway_url: str,
    output_dir: str,
    run_id: str,
    image: str,
    timeout_sec: int,
) -> BrowserQaConfig:
    resolved_run_id = run_id or make_run_id()
    resolved_output = Path(output_dir or DEFAULT_OUTPUT_ROOT) / resolved_run_id
    return BrowserQaConfig(
        frontend_url=frontend_url or DEFAULT_FRONTEND_URL,
        gateway_url=(gateway_url or DEFAULT_GATEWAY_URL).rstrip("/"),
        sandbox_frontend_url=sandbox_frontend_url or frontend_url or DEFAULT_FRONTEND_URL,
        sandbox_gateway_url=(sandbox_gateway_url or gateway_url or DEFAULT_GATEWAY_URL).rstrip("/"),
        output_dir=resolved_output,
        run_id=resolved_run_id,
        image=image or DEFAULT_IMAGE,
        timeout_sec=max(10, int(timeout_sec or 60)),
    )


def build_result(
    *,
    run_id: str,
    status: str,
    frontend_url: str,
    gateway_url: str,
    checks: Iterable[CheckResult],
    screenshot: str,
    sandbox_id: str,
    error: str = "",
) -> Dict[str, Any]:
    payload: Dict[str, Any] = {
        "run_id": run_id,
        "status": status,
        "sandbox_id": sandbox_id,
        "target": {
            "frontend_url": frontend_url,
            "gateway_url": gateway_url,
        },
        "checks": [check.to_json() for check in checks],
        "artifacts": {
            "screenshot": screenshot,
        },
    }
    if error:
        payload["error"] = error
    return payload


def write_local_result(output_dir: Path, result: Dict[str, Any]) -> Path:
    output_dir.mkdir(parents=True, exist_ok=True)
    result_path = output_dir / RESULT_NAME
    result_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return result_path


def run_dry_run(config: BrowserQaConfig) -> Dict[str, Any]:
    config.output_dir.mkdir(parents=True, exist_ok=True)
    note_path = config.output_dir / DRY_RUN_NOTE_NAME
    note_path.write_text(
        "\n".join(
            [
                "MindBridge sandbox browser QA dry run",
                f"frontend_url={config.sandbox_frontend_url}",
                f"gateway_url={config.sandbox_gateway_url}",
                "This mode validates local artifact wiring only; it does not create an OpenSandbox sandbox.",
                "",
            ]
        ),
        encoding="utf-8",
    )
    result = build_result(
        run_id=config.run_id,
        status="passed",
        frontend_url=config.sandbox_frontend_url,
        gateway_url=config.sandbox_gateway_url,
        checks=[
            CheckResult("dry_run_artifact_schema", True, "qa_result.json schema generated"),
            CheckResult("dry_run_target_urls", True, "sandbox target URLs resolved"),
        ],
        screenshot="",
        sandbox_id="dry-run",
    )
    result["artifacts"]["note"] = DRY_RUN_NOTE_NAME
    write_local_result(config.output_dir, result)
    return result


def playwright_script(frontend_url: str, gateway_url: str, timeout_sec: int) -> str:
    timeout_ms = int(timeout_sec) * 1000
    return textwrap.dedent(
        f"""
        import asyncio
        import json
        import urllib.request
        from pathlib import Path
        from playwright.async_api import async_playwright

        FRONTEND_URL = {json.dumps(frontend_url)}
        GATEWAY_URL = {json.dumps(gateway_url)}
        TIMEOUT_MS = {timeout_ms}
        WORK_DIR = Path({json.dumps(REMOTE_WORK_DIR)})
        RESULT = Path({json.dumps(f"{REMOTE_WORK_DIR}/{RESULT_NAME}")})
        SCREENSHOT = Path({json.dumps(f"{REMOTE_WORK_DIR}/{SCREENSHOT_NAME}")})
        checks = []

        def record(name, ok, detail):
            checks.append({{"name": name, "ok": bool(ok), "detail": str(detail or "")}})

        def http_status(url):
            try:
                with urllib.request.urlopen(url, timeout=max(2, TIMEOUT_MS / 1000)) as response:
                    return response.status
            except Exception as exc:
                record("http_error", False, f"{{url}}: {{exc}}")
                return 0

        def http_json(url):
            try:
                with urllib.request.urlopen(url, timeout=max(2, TIMEOUT_MS / 1000)) as response:
                    return json.loads(response.read().decode("utf-8"))
            except Exception as exc:
                record("http_json_error", False, f"{{url}}: {{exc}}")
                return {{}}

        async def message_count(page, selector):
            return await page.locator(selector).count()

        async def wait_for_new_message(page, selector, before):
            await page.wait_for_function(
                "(args) => document.querySelectorAll(args.selector).length > args.before",
                arg={{"selector": selector, "before": before}},
                timeout=TIMEOUT_MS,
            )
            return await page.locator(selector).last.inner_text(timeout=TIMEOUT_MS)

        async def send_demo_and_wait(page, expression):
            agent_before = await message_count(page, ".chat-msg.agent")
            error_before = await message_count(page, ".chat-msg.error")
            await page.evaluate(expression)
            await page.wait_for_function(
                "(args) => document.querySelectorAll('.chat-msg.agent').length > args.agentBefore || document.querySelectorAll('.chat-msg.error').length > args.errorBefore",
                arg={{"agentBefore": agent_before, "errorBefore": error_before}},
                timeout=TIMEOUT_MS,
            )
            if await message_count(page, ".chat-msg.error") > error_before:
                return "", await page.locator(".chat-msg.error").last.inner_text(timeout=TIMEOUT_MS)
            return await page.locator(".chat-msg.agent").last.inner_text(timeout=TIMEOUT_MS), ""

        def latest_run():
            return http_json(f"{{GATEWAY_URL}}/api/demo/runs/latest")

        def latest_observability():
            return http_json(f"{{GATEWAY_URL}}/api/demo/runs/latest/observability")

        def run_has_event(run_payload, event_name):
            return any(item.get("event") == event_name for item in run_payload.get("trace", []))

        def extract_latest_conversation_id():
            payload = http_json(f"{{GATEWAY_URL}}/api/conversations")
            conversations = payload.get("conversations", [])
            if not conversations:
                return ""
            record("stored_history_visible", True, f"conversations={{len(conversations)}}")
            return conversations[0].get("conversation_id", "")

        def conversation_turns(conversation_id):
            if not conversation_id:
                return []
            payload = http_json(f"{{GATEWAY_URL}}/api/conversations/{{conversation_id}}")
            return payload.get("turns", [])

        def validate_conversation_turns(conversation_id):
            turns = conversation_turns(conversation_id)
            if not turns:
                record("stored_history_turns", False, "missing conversation turns")
                return
            roles = {{item.get("role") for item in turns}}
            namespaces = {{item.get("namespace_name") for item in turns}}
            risk_levels = {{item.get("risk_level") for item in turns}}
            ok = "user" in roles and "assistant" in roles and len(turns) >= 2
            detail = f"turns={{len(turns)}} namespaces={{sorted(namespaces)}} risk_levels={{sorted(risk_levels)}}"
            record("stored_history_turns", ok, detail)

        def has_high_risk_memory(conversation_id):
            turns = conversation_turns(conversation_id)
            return any(
                item.get("namespace_name") == "risk_memory"
                or item.get("risk_level") in {{"high", "critical"}}
                for item in turns
            )

        def session_risk_level(text):
            marker = "Risk:"
            if marker not in text:
                return ""
            tail = text.split(marker, 1)[1].strip()
            return tail.split(";", 1)[0].strip().lower()

        async def run():
            WORK_DIR.mkdir(parents=True, exist_ok=True)
            health_status = http_status(f"{{GATEWAY_URL}}/api/health")
            record("gateway_health", 200 <= health_status < 300, f"status={{health_status}}")

            async with async_playwright() as p:
                browser = await p.chromium.launch(headless=True)
                try:
                    page = await browser.new_page(viewport={{"width": 1440, "height": 1100}})
                    page.set_default_timeout(TIMEOUT_MS)
                    await page.goto(FRONTEND_URL, wait_until="domcontentloaded", timeout=TIMEOUT_MS)
                    await page.wait_for_selector("#send-btn", timeout=TIMEOUT_MS)
                    await page.wait_for_selector("#chat-input", timeout=TIMEOUT_MS)
                    record("frontend_loaded", True, await page.title())

                    consult_text, consult_error = await send_demo_and_wait(page, "sendDemo('normal')")
                    record(
                        "consult_response_rendered",
                        bool(consult_text.strip()) and not consult_error,
                        consult_error or f"chars={{len(consult_text.strip())}}",
                    )
                    consult_run = latest_run()
                    consult_report = consult_run.get("report", {{}})
                    consult_state = consult_run.get("task_state", {{}})
                    record(
                        "run_artifact_contract",
                        consult_run.get("ok") is True
                        and bool(consult_run.get("run_id"))
                        and consult_report.get("status") == "completed"
                        and consult_state.get("status") == "completed"
                        and run_has_event(consult_run, "run_started")
                        and run_has_event(consult_run, "model_requested")
                        and run_has_event(consult_run, "run_finished"),
                        f"run_id={{consult_run.get('run_id')}} status={{consult_report.get('status')}}",
                    )

                    risk_text, risk_error = await send_demo_and_wait(page, "sendDemo('risk')")
                    record(
                        "risk_response_rendered",
                        bool(risk_text.strip()) and not risk_error,
                        risk_error or f"chars={{len(risk_text.strip())}}",
                    )
                    risk_run = latest_run()
                    risk_report = risk_run.get("report", {{}})
                    risk_state = risk_run.get("task_state", {{}})
                    risk_level = risk_state.get("risk_level") or risk_report.get("risk_level") or ""
                    risk_conversation_id = extract_latest_conversation_id()
                    risk_memory_seen = has_high_risk_memory(risk_conversation_id)

                    system_before = await message_count(page, ".chat-msg.system")
                    await page.evaluate("endSession()")
                    session_text = await wait_for_new_message(page, ".chat-msg.system", system_before)
                    session_risk = session_risk_level(session_text)
                    record(
                        "risk_route_recorded",
                        risk_run.get("ok") is True
                        and (risk_level in {{"high", "critical"}} or risk_memory_seen or session_risk in {{"high", "critical"}}),
                        f"run_risk={{risk_level}} risk_memory={{risk_memory_seen}} session_risk={{session_risk}} run_id={{risk_run.get('run_id')}}",
                    )
                    record(
                        "session_assessment_completed",
                        "Session assessment completed" in session_text and session_risk in {{"low", "medium", "high", "critical"}},
                        session_text,
                    )

                    conversation_id = risk_conversation_id or extract_latest_conversation_id()
                    validate_conversation_turns(conversation_id)

                    try:
                        await page.click("#tab-observability")
                    except Exception:
                        pass
                    run_status = http_status(f"{{GATEWAY_URL}}/api/demo/runs/latest")
                    record("latest_run_artifact_api", run_status < 500 and run_status > 0, f"status={{run_status}}")
                    obs_status = http_status(f"{{GATEWAY_URL}}/api/demo/runs/latest/observability")
                    record("observability_api", obs_status < 500 and obs_status > 0, f"status={{obs_status}}")
                    observability = latest_observability()
                    boundary_events = observability.get("boundary_trace", [])
                    record(
                        "observability_business_events",
                        any(item.get("event") == "model_requested" for item in boundary_events)
                        and any(item.get("event") == "run_finished" for item in boundary_events),
                        f"events={{len(boundary_events)}}",
                    )

                    await page.screenshot(path=str(SCREENSHOT), full_page=True)
                    record("screenshot_written", SCREENSHOT.exists(), str(SCREENSHOT))
                finally:
                    await browser.close()

        try:
            asyncio.run(run())
        except Exception as exc:
            record("browser_qa_uncaught", False, repr(exc))
        passed = len(checks) > 0 and all(item["ok"] for item in checks)
        RESULT.write_text(json.dumps({{
            "status": "passed" if passed else "failed",
            "checks": checks,
            "artifacts": {{"screenshot": {json.dumps(SCREENSHOT_NAME)}}},
        }}, ensure_ascii=False, indent=2), encoding="utf-8")
        if not passed:
            raise SystemExit(1)
        """
    ).strip() + "\n"


async def run_in_opensandbox(config: BrowserQaConfig) -> Dict[str, Any]:
    try:
        from opensandbox import Sandbox  # type: ignore
        from opensandbox.config import ConnectionConfig  # type: ignore
        from opensandbox.models.execd import RunCommandOpts  # type: ignore
    except Exception as exc:
        raise RuntimeError(
            "OpenSandbox Python SDK is not installed. Install it in a Python 3.10+ environment "
            "or run this script through the project setup instructions."
        ) from exc

    sandbox_id = ""
    domain = os.getenv("SANDBOX_DOMAIN") or os.getenv("MINDBRIDGE_OPENSANDBOX_DOMAIN") or "localhost:8080"
    api_key = os.getenv("SANDBOX_API_KEY") or os.getenv("MINDBRIDGE_OPENSANDBOX_API_KEY")
    connection_config = ConnectionConfig(domain=domain, api_key=api_key)
    sandbox = await Sandbox.create(
        config.image,
        connection_config=connection_config,
        env={"PYTHON_VERSION": os.getenv("PYTHON_VERSION", "3.11")},
    )
    sandbox_id = getattr(sandbox, "id", "") or getattr(sandbox, "sandbox_id", "")
    try:
        async with sandbox:
            script = playwright_script(config.sandbox_frontend_url, config.sandbox_gateway_url, config.timeout_sec)
            command = "python - <<'PY'\n" + script + "PY"
            execution = await sandbox.commands.run(
                command,
                opts=RunCommandOpts(
                    timeout=datetime.timedelta(seconds=config.timeout_sec + 15),
                    working_directory=REMOTE_WORK_DIR,
                ),
            )
            exit_code = int(getattr(execution, "exit_code", 0) or 0)

            config.output_dir.mkdir(parents=True, exist_ok=True)
            local_result = config.output_dir / RESULT_NAME
            local_screenshot = config.output_dir / SCREENSHOT_NAME

            result_text = await sandbox.files.read_file(f"{REMOTE_WORK_DIR}/{RESULT_NAME}")
            local_result.write_text(result_text, encoding="utf-8")
            try:
                screenshot_bytes = await sandbox.files.read_bytes(f"{REMOTE_WORK_DIR}/{SCREENSHOT_NAME}")
                local_screenshot.write_bytes(screenshot_bytes)
            except Exception:
                pass

            result = json.loads(result_text)
            checks = [
                CheckResult(item.get("name", "unknown"), bool(item.get("ok")), item.get("detail", ""))
                for item in result.get("checks", [])
            ]
            return build_result(
                run_id=config.run_id,
                status="passed" if exit_code == 0 and result.get("status") == "passed" else "failed",
                frontend_url=config.sandbox_frontend_url,
                gateway_url=config.sandbox_gateway_url,
                checks=checks,
                screenshot=SCREENSHOT_NAME if local_screenshot.exists() else "",
                sandbox_id=sandbox_id,
                error="" if exit_code == 0 else f"sandbox command exited {exit_code}",
            )
    finally:
        kill = getattr(sandbox, "kill", None)
        if kill:
            maybe = kill()
            if hasattr(maybe, "__await__"):
                await maybe


def copy_final_result(output_dir: Path, result: Dict[str, Any]) -> None:
    write_local_result(output_dir, result)


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run MindBridge Browser QA in an OpenSandbox Playwright sandbox.")
    parser.add_argument("--frontend-url", default=os.getenv("MINDBRIDGE_FRONTEND_URL", DEFAULT_FRONTEND_URL))
    parser.add_argument("--gateway-url", default=os.getenv("MINDBRIDGE_GATEWAY_URL", DEFAULT_GATEWAY_URL))
    parser.add_argument("--sandbox-frontend-url", default=os.getenv("MINDBRIDGE_SANDBOX_FRONTEND_URL", ""))
    parser.add_argument("--sandbox-gateway-url", default=os.getenv("MINDBRIDGE_SANDBOX_GATEWAY_URL", ""))
    parser.add_argument("--output-dir", default=os.getenv("MINDBRIDGE_SANDBOX_QA_OUTPUT_DIR", DEFAULT_OUTPUT_ROOT))
    parser.add_argument("--run-id", default=os.getenv("MINDBRIDGE_SANDBOX_QA_RUN_ID", ""))
    parser.add_argument("--image", default=os.getenv("MINDBRIDGE_OPENSANDBOX_IMAGE", DEFAULT_IMAGE))
    parser.add_argument("--timeout-sec", type=int, default=int(os.getenv("MINDBRIDGE_SANDBOX_QA_TIMEOUT_SEC", "60")))
    parser.add_argument("--dry-run", action="store_true", default=os.getenv("MINDBRIDGE_SANDBOX_QA_DRY_RUN", "") in {"1", "true", "TRUE", "yes"})
    return parser.parse_args(argv)


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)
    config = resolve_config(
        frontend_url=args.frontend_url,
        gateway_url=args.gateway_url,
        sandbox_frontend_url=args.sandbox_frontend_url,
        sandbox_gateway_url=args.sandbox_gateway_url,
        output_dir=args.output_dir,
        run_id=args.run_id,
        image=args.image,
        timeout_sec=args.timeout_sec,
    )
    if args.dry_run:
        result = run_dry_run(config)
    else:
        try:
            result = asyncio.run(run_in_opensandbox(config))
        except Exception as exc:
            result = build_result(
                run_id=config.run_id,
                status="failed",
                frontend_url=config.sandbox_frontend_url,
                gateway_url=config.sandbox_gateway_url,
                checks=[CheckResult("opensandbox_execution", False, str(exc))],
                screenshot="",
                sandbox_id="",
                error=str(exc),
            )
    copy_final_result(config.output_dir, result)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result.get("status") == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
