#!/usr/bin/env python3
"""Small OpenAI-compatible mock server for sandbox browser QA verification."""

import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        return

    def _read_json(self):
        length = int(self.headers.get("content-length", "0") or "0")
        raw = self.rfile.read(length) if length else b"{}"
        try:
            return json.loads(raw.decode("utf-8"))
        except Exception:
            return {}

    def _send_json(self, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            self._send_json({"ok": True, "service": "fake_openai"})
            return
        self.send_error(404)

    def do_POST(self):
        payload = self._read_json()
        if self.path.endswith("/embeddings"):
            self._send_json({
                "object": "list",
                "data": [{"object": "embedding", "index": 0, "embedding": [0.01] * 16}],
                "model": payload.get("model", "fake-embedding"),
            })
            return
        if self.path.endswith("/chat/completions"):
            text = "我听到了你的压力。我们可以先一起把今天最困扰你的事情说清楚。"
            if payload.get("stream"):
                self.send_response(200)
                self.send_header("content-type", "text/event-stream")
                self.send_header("cache-control", "no-cache")
                self.end_headers()
                for chunk in ["我听到了你的压力。", "我们可以先一起把今天最困扰你的事情说清楚。"]:
                    event = {
                        "choices": [
                            {
                                "index": 0,
                                "delta": {"content": chunk},
                                "finish_reason": None,
                            }
                        ]
                    }
                    self.wfile.write(("data: " + json.dumps(event, ensure_ascii=False) + "\n\n").encode("utf-8"))
                    self.wfile.flush()
                    time.sleep(0.03)
                self.wfile.write(b"data: [DONE]\n\n")
                self.wfile.flush()
                return
            self._send_json({
                "choices": [{"message": {"role": "assistant", "content": text}}],
                "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2},
            })
            return
        self.send_error(404)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18080)
    args = parser.parse_args()
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"fake_openai_server listening on {args.host}:{args.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
