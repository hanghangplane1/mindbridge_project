import json
import tempfile
import unittest
from pathlib import Path

from sandbox.browser_qa import mindbridge_browser_qa as qa


class BrowserQaHelperTest(unittest.TestCase):
    def test_resolve_config_uses_sandbox_urls_when_present(self):
        config = qa.resolve_config(
            frontend_url="http://127.0.0.1:5173/index.html",
            gateway_url="http://127.0.0.1:8090",
            sandbox_frontend_url="http://host.docker.internal:5173/index.html",
            sandbox_gateway_url="http://host.docker.internal:8090",
            output_dir=".mindbridge/sandbox_qa",
            run_id="manual",
            image="ghcr.io/example/playwright:latest",
            timeout_sec=45,
        )

        self.assertEqual(config.frontend_url, "http://127.0.0.1:5173/index.html")
        self.assertEqual(config.gateway_url, "http://127.0.0.1:8090")
        self.assertEqual(config.sandbox_frontend_url, "http://host.docker.internal:5173/index.html")
        self.assertEqual(config.sandbox_gateway_url, "http://host.docker.internal:8090")
        self.assertEqual(config.run_id, "manual")
        self.assertEqual(config.image, "ghcr.io/example/playwright:latest")
        self.assertEqual(config.timeout_sec, 45)

    def test_qa_result_schema_records_required_checks(self):
        result = qa.build_result(
            run_id="run-123",
            status="passed",
            frontend_url="http://host.docker.internal:5173/index.html",
            gateway_url="http://host.docker.internal:8090",
            checks=[
                qa.CheckResult(name="frontend_loaded", ok=True, detail="loaded"),
                qa.CheckResult(name="gateway_health", ok=True, detail="ok"),
            ],
            screenshot="mindbridge-browser-qa.png",
            sandbox_id="sandbox-1",
        )

        self.assertEqual(result["run_id"], "run-123")
        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["sandbox_id"], "sandbox-1")
        self.assertEqual(result["artifacts"]["screenshot"], "mindbridge-browser-qa.png")
        self.assertEqual([item["name"] for item in result["checks"]], ["frontend_loaded", "gateway_health"])

    def test_write_local_result_creates_artifact_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = qa.build_result(
                run_id="run-123",
                status="failed",
                frontend_url="http://frontend",
                gateway_url="http://gateway",
                checks=[qa.CheckResult(name="frontend_loaded", ok=False, detail="timeout")],
                screenshot="",
                sandbox_id="",
            )

            output = qa.write_local_result(Path(tmp), result)

            self.assertEqual(output.name, "qa_result.json")
            loaded = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(loaded["status"], "failed")
            self.assertEqual(loaded["checks"][0]["detail"], "timeout")

    def test_playwright_script_targets_current_demo_controls(self):
        script = qa.playwright_script("http://frontend/index.html", "http://gateway", 30)

        self.assertIn("from playwright.async_api import async_playwright", script)
        self.assertIn("/home/playwright/mindbridge-browser-qa.png", script)
        self.assertIn("#chat-input", script)
        self.assertIn("#send-btn", script)
        self.assertIn(".chat-msg", script)
        self.assertIn(".chat-msg.agent", script)
        self.assertIn("/api/demo/runs/latest", script)

    def test_playwright_script_validates_counseling_business_flow(self):
        script = qa.playwright_script("http://frontend/index.html", "http://gateway", 30)

        for check_name in [
            "consult_response_rendered",
            "risk_response_rendered",
            "risk_route_recorded",
            "session_assessment_completed",
            "stored_history_visible",
            "run_artifact_contract",
        ]:
            self.assertIn(check_name, script)

        self.assertIn("sendDemo('normal')", script)
        self.assertIn("sendDemo('risk')", script)
        self.assertIn("endSession()", script)
        self.assertIn("/api/conversations", script)

    def test_dry_run_writes_expected_artifacts(self):
        with tempfile.TemporaryDirectory() as tmp:
            config = qa.resolve_config(
                frontend_url="http://127.0.0.1:5173/index.html",
                gateway_url="http://127.0.0.1:8090",
                sandbox_frontend_url="http://host.docker.internal:5173/index.html",
                sandbox_gateway_url="http://host.docker.internal:8090",
                output_dir=tmp,
                run_id="dry-run",
                image="image",
                timeout_sec=30,
            )

            result = qa.run_dry_run(config)

            self.assertEqual(result["status"], "passed")
            self.assertEqual(result["sandbox_id"], "dry-run")
            self.assertTrue((Path(tmp) / "dry-run" / "qa_result.json").exists())
            self.assertTrue((Path(tmp) / "dry-run" / "mindbridge-browser-qa.txt").exists())


if __name__ == "__main__":
    unittest.main()
