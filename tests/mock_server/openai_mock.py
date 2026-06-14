#!/usr/bin/env python3
import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    model = "mock-model"

    def log_message(self, fmt, *args):
        return

    def _send(self, status, body, content_type="application/json"):
        data = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/v1/models":
            self._send(200, json.dumps({"object": "list", "data": [{"id": self.model, "object": "model"}]}))
        else:
            self._send(404, json.dumps({"error": {"message": "not found"}}))

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8")
        try:
            request = json.loads(body)
        except json.JSONDecodeError:
            self._send(400, json.dumps({"error": {"message": "bad json"}}))
            return
        if self.path != "/v1/chat/completions":
            self._send(404, json.dumps({"error": {"message": "not found"}}))
            return
        if request.get("stream"):
            payload = (
                'data: {"choices":[{"delta":{"content":"Hel"}}]}\n\n'
                ': comment\n\n'
                'data: {"choices":[{"delta":{"content":"lo"}}]}\n\n'
                'data: [DONE]\n\n'
            )
            self._send(200, payload, "text/event-stream")
            return
        self._send(
            200,
            json.dumps(
                {
                    "id": "chatcmpl_mock",
                    "object": "chat.completion",
                    "model": self.model,
                    "choices": [{"index": 0, "message": {"role": "assistant", "content": "Hello"}}],
                    "usage": None,
                }
            ),
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--model", default="mock-model")
    args = parser.parse_args()
    Handler.model = args.model
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.serve_forever()


if __name__ == "__main__":
    main()
