#!/usr/bin/env python3
import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    model = "mock-model"
    empty_models = False
    stream_delay = 0.0

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
            if self.empty_models:
                self._send(200, json.dumps({"object": "list", "data": []}))
                return
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
        if request.get("model") == "":
            self._send(400, json.dumps({"error": {"message": "empty model field"}}))
            return
        messages = request.get("messages", [])
        last = messages[-1].get("content", "") if messages and isinstance(messages[-1], dict) else ""
        reply = "Hello"
        if last == "count-messages":
            reply = f"messages:{len(messages)}"
        elif last == "model?":
            reply = request.get("model", "<missing>")
        elif last == "repl-one":
            reply = "repl-one-reply"
        elif last == "reasoning":
            reply = "Visible answer"
        elif last == "previous-assistant":
            reply = ""
            for message in reversed(messages[:-1]):
                if isinstance(message, dict) and message.get("role") == "assistant":
                    reply = message.get("content", "")
                    break

        if request.get("stream"):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            midpoint = max(1, len(reply) // 2)
            if last == "reasoning":
                chunks = [
                    "data: " + json.dumps({"choices": [{"delta": {"reasoning_content": "internal"}}]}) + "\n\n",
                    "data: " + json.dumps({"choices": [{"delta": {"reasoning_content": " trace"}}]}) + "\n\n",
                    "data: " + json.dumps({"choices": [{"delta": {"content": reply}}]}) + "\n\n",
                    "data: [DONE]\n\n",
                ]
            else:
                chunks = [
                    "data: " + json.dumps({"choices": [{"delta": {"content": reply[:midpoint]}}]}) + "\n\n",
                    ": comment\n\n",
                    "data: " + json.dumps({"choices": [{"delta": {"content": reply[midpoint:]}}]}) + "\n\n",
                    "data: [DONE]\n\n",
                ]
            for chunk in chunks:
                if self.stream_delay:
                    time.sleep(self.stream_delay)
                self.wfile.write(chunk.encode("utf-8"))
                self.wfile.flush()
            return
        self._send(
            200,
            json.dumps(
                {
                    "id": "chatcmpl_mock",
                    "object": "chat.completion",
                    "model": self.model,
                    "choices": [
                        {
                            "index": 0,
                            "message": {
                                "role": "assistant",
                                "content": reply,
                                **({"reasoning_content": "internal trace"} if last == "reasoning" else {}),
                            },
                        }
                    ],
                    "usage": {"prompt_tokens": len(messages), "completion_tokens": 1, "total_tokens": len(messages) + 1},
                }
            ),
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--model", default="mock-model")
    parser.add_argument("--empty-models", action="store_true")
    parser.add_argument("--stream-delay", type=float, default=0.0)
    args = parser.parse_args()
    Handler.model = args.model
    Handler.empty_models = args.empty_models
    Handler.stream_delay = args.stream_delay
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.serve_forever()


if __name__ == "__main__":
    main()
