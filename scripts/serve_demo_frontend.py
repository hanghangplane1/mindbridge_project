#!/usr/bin/env python3
import argparse
import os
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen
from urllib.parse import urlparse


class NoCacheHandler(SimpleHTTPRequestHandler):
    gateway_url = "http://127.0.0.1:8090"
    orchestrator_url = "http://127.0.0.1:5009"
    counselor_url = "http://127.0.0.1:5010"
    evaluator_url = "http://127.0.0.1:5011"

    def end_headers(self):
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()

    def do_GET(self):
        if self._maybe_proxy():
            return
        super().do_GET()

    def do_POST(self):
        if self._maybe_proxy():
            return
        self.send_error(404, "Not found")

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, Authorization")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.end_headers()

    def _maybe_proxy(self):
        upstream, forwarded_path = self._upstream_for_path(self.path)
        if not upstream:
            return False
        self._proxy(upstream, forwarded_path)
        return True

    def _upstream_for_path(self, raw_path):
        parsed = urlparse(raw_path)
        path = parsed.path or "/"
        suffix = f"?{parsed.query}" if parsed.query else ""
        if path.startswith("/proxy/orchestrator/"):
            return self.orchestrator_url, path[len("/proxy/orchestrator"):] + suffix
        if path.startswith("/proxy/counselor/"):
            return self.counselor_url, path[len("/proxy/counselor"):] + suffix
        if path.startswith("/proxy/evaluator/"):
            return self.evaluator_url, path[len("/proxy/evaluator"):] + suffix
        if path.startswith("/api/") or (path == "/" and self.command == "POST"):
            return self.gateway_url, path + suffix
        return None, None

    def _proxy(self, upstream, forwarded_path):
        target = upstream.rstrip("/") + forwarded_path
        body = None
        if self.command in {"POST", "PUT", "PATCH"}:
            length = int(self.headers.get("Content-Length", "0") or "0")
            body = self.rfile.read(length) if length else b""
        headers = {}
        for key in ("Content-Type", "Authorization", "Accept"):
            value = self.headers.get(key)
            if value:
                headers[key] = value
        request = Request(target, data=body, headers=headers, method=self.command)
        try:
            with urlopen(request, timeout=120) as response:
                payload = response.read()
                self.send_response(response.status)
                self.send_header("Access-Control-Allow-Origin", "*")
                content_type = response.headers.get("Content-Type")
                if content_type:
                    self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
        except HTTPError as err:
            payload = err.read()
            self.send_response(err.code)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Type", err.headers.get("Content-Type", "application/json"))
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        except URLError as err:
            payload = f'{{"ok":false,"error":"proxy failed: {err.reason}"}}'.encode("utf-8")
            self.send_response(502)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)


def main():
    parser = argparse.ArgumentParser(description="Serve the MindBridge demo frontend without browser caching.")
    parser.add_argument("--port", type=int, default=5173)
    parser.add_argument("--directory", default="frontend/demo")
    args = parser.parse_args()

    NoCacheHandler.gateway_url = os.environ.get("MINDBRIDGE_FRONTEND_GATEWAY_URL", "http://127.0.0.1:8090")
    NoCacheHandler.orchestrator_url = os.environ.get("MINDBRIDGE_FRONTEND_ORCHESTRATOR_URL", "http://127.0.0.1:5009")
    NoCacheHandler.counselor_url = os.environ.get("MINDBRIDGE_FRONTEND_COUNSELOR_URL", "http://127.0.0.1:5010")
    NoCacheHandler.evaluator_url = os.environ.get("MINDBRIDGE_FRONTEND_EVALUATOR_URL", "http://127.0.0.1:5011")

    handler = lambda *handler_args, **handler_kwargs: NoCacheHandler(
        *handler_args, directory=args.directory, **handler_kwargs)
    server = ThreadingHTTPServer(("", args.port), handler)
    print(f"MindBridge frontend listening on http://127.0.0.1:{args.port}/index.html")
    server.serve_forever()


if __name__ == "__main__":
    main()
