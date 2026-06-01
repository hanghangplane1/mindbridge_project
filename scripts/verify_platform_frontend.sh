#!/usr/bin/env bash
set -euo pipefail

python3 - <<'PY'
from pathlib import Path

root = Path("frontend/demo")
html = (root / "index.html").read_text()
js = (root / "app.js").read_text()
css = (root / "styles.css").read_text()

required_html = [
    'id="tab-platform"',
    'id="platform-dashboard"',
    'id="platform-workspace-list"',
    'id="platform-session-list"',
    'id="platform-event-list"',
    'id="platform-artifact-list"',
]
required_js = [
    "const PLATFORM",
    "loadPlatformDashboard",
    "createPlatformWorkspace",
    "createPlatformSession",
    "loadPlatformSession",
    "platformConversationId",
    "attachPlatformRun",
    "renderPlatformEvents",
    "selectedPlatformSessionId",
    "/api/platform/workspaces",
    "/api/platform/sessions",
    "/attach-run",
]
required_css = [
    ".platform-dashboard",
    ".platform-grid",
    ".platform-session-row",
    ".platform-event-row",
    ".platform-artifact-row",
]

missing = []
for needle in required_html:
    if needle not in html:
        missing.append(f"HTML missing {needle}")
for needle in required_js:
    if needle not in js:
        missing.append(f"JS missing {needle}")
for needle in required_css:
    if needle not in css:
        missing.append(f"CSS missing {needle}")

if missing:
    raise SystemExit("\n".join(missing))

print("PASS: platform frontend wiring present")
PY
