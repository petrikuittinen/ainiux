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
        if self.path == "/page":
            user_agent = self.headers.get("User-Agent", "")
            accept = self.headers.get("Accept", "")
            if "Mozilla/5.0" not in user_agent or "text/html" not in accept:
                self._send(403, json.dumps({"error": {"message": "browser-like headers required"}}))
                return
            self._send(
                200,
                """
                <!doctype html>
                <html>
                  <head><title>ignored title</title><style>.hidden { display: none; }</style></head>
                  <body>
                    <h1>Mock Page</h1>
                    <p>Hello <strong>bold</strong> and <em>emphasis</em> with <a href="https://example.com/docs">docs</a>.</p>
                    <script>bad()</script>
                  </body>
                </html>
                """,
                "text/html; charset=utf-8",
            )
            return
        if self.path == "/v1/models":
            if self.empty_models:
                self._send(200, json.dumps({"object": "list", "data": []}))
                return
            self._send(200, json.dumps({"object": "list", "data": [{"id": self.model, "object": "model"}]}))
        else:
            self._send(404, json.dumps({"error": {"message": "not found"}}))

    def _responses_last_input(self, request):
        items = request.get("input", [])
        if isinstance(items, str):
            return items
        if not isinstance(items, list):
            return ""
        for item in reversed(items):
            if not isinstance(item, dict) or item.get("role") != "user":
                continue
            content = item.get("content", "")
            if isinstance(content, str):
                return content
            if isinstance(content, list):
                parts = []
                for part in content:
                    if isinstance(part, str):
                        parts.append(part)
                    elif isinstance(part, dict) and isinstance(part.get("text"), str):
                        parts.append(part["text"])
                return "".join(parts)
        return ""

    def _chat_last_input(self, request):
        messages = request.get("messages", [])
        if not messages or not isinstance(messages[-1], dict):
            return "", False
        content = messages[-1].get("content", "")
        if isinstance(content, str):
            return content, False
        if not isinstance(content, list):
            return "", False
        text = []
        image_seen = False
        for part in content:
            if not isinstance(part, dict):
                continue
            if part.get("type") == "text" and isinstance(part.get("text"), str):
                text.append(part["text"])
            if part.get("type") == "image_url" and isinstance(part.get("image_url"), dict):
                url = part["image_url"].get("url", "")
                image_seen = isinstance(url, str) and url.startswith("data:image/png;base64,iVBOR")
        return "".join(text), image_seen

    def _handle_responses(self, request):
        if request.get("model") == "":
            self._send(400, json.dumps({"error": {"message": "empty model field"}}))
            return
        last = self._responses_last_input(request)
        reply = "Hello"
        if last == "model?":
            reply = request.get("model", "<missing>")
        elif last == "reasoning":
            reply = "Visible answer"
        elif last == "markdown":
            reply = "# Mock Title\n\nHello **bold** and [docs](https://example.com/docs)."

        if request.get("stream"):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            midpoint = max(1, len(reply) // 2)
            if last == "reasoning":
                chunks = [
                    "data: " + json.dumps({"type": "response.reasoning_summary_text.delta", "delta": "internal"}) + "\n\n",
                    "data: " + json.dumps({"type": "response.reasoning_summary_text.delta", "delta": " trace"}) + "\n\n",
                    "data: " + json.dumps({"type": "response.output_text.delta", "delta": reply}) + "\n\n",
                    "data: " + json.dumps({"type": "response.completed", "response": {"model": self.model, "usage": {"input_tokens": 1, "output_tokens": 1, "total_tokens": 2}}}) + "\n\n",
                ]
            else:
                chunks = [
                    "data: " + json.dumps({"type": "response.output_text.delta", "delta": reply[:midpoint]}) + "\n\n",
                    "data: " + json.dumps({"type": "response.output_text.delta", "delta": reply[midpoint:]}) + "\n\n",
                    "data: " + json.dumps({"type": "response.completed", "response": {"model": self.model, "usage": {"input_tokens": 1, "output_tokens": 1, "total_tokens": 2}}}) + "\n\n",
                ]
            for chunk in chunks:
                if self.stream_delay:
                    time.sleep(self.stream_delay)
                self.wfile.write(chunk.encode("utf-8"))
                self.wfile.flush()
            return
        output = []
        if last == "reasoning":
            output.append({"type": "reasoning", "summary": [{"text": "internal trace"}]})
        output.append({"type": "message", "role": "assistant", "content": [{"type": "output_text", "text": reply, "annotations": []}]})
        self._send(
            200,
            json.dumps(
                {
                    "id": "resp_mock",
                    "object": "response",
                    "model": self.model,
                    "output": output,
                    "usage": {"input_tokens": 1, "output_tokens": 1, "total_tokens": 2},
                }
            ),
        )

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8")
        try:
            request = json.loads(body)
        except json.JSONDecodeError:
            self._send(400, json.dumps({"error": {"message": "bad json"}}))
            return
        if self.path == "/v1/responses":
            self._handle_responses(request)
            return
        if self.path != "/v1/chat/completions":
            self._send(404, json.dumps({"error": {"message": "not found"}}))
            return
        if request.get("model") == "":
            self._send(400, json.dumps({"error": {"message": "empty model field"}}))
            return
        messages = request.get("messages", [])
        last, image_seen = self._chat_last_input(request)
        url_context_seen = any(
            isinstance(message, dict)
            and isinstance(message.get("content"), str)
            and (
                "Input context from URL" in message.get("content", "")
                or "Fetched HTML context from URL" in message.get("content", "")
            )
            and "Mock Page" in message.get("content", "")
            for message in messages
        )
        input_context_seen = any(
            isinstance(message, dict)
            and isinstance(message.get("content"), str)
            and "Input context from file" in message.get("content", "")
            and "Local Input Title" in message.get("content", "")
            for message in messages
        )
        system_context_seen = any(
            isinstance(message, dict)
            and message.get("role") == "system"
            and "url-system" in message.get("content", "")
            for message in messages
        )
        reply = "Hello"
        if last == "count-messages":
            reply = f"messages:{len(messages)}"
        elif last == "model?":
            reply = request.get("model", "<missing>")
        elif last == "repl-one":
            reply = "repl-one-reply"
        elif last == "reasoning":
            reply = "Visible answer"
        elif last == "markdown":
            reply = "# Mock Title\n\nHello **bold** and [docs](https://example.com/docs)."
        elif last == "summarize-url":
            reply = "url-context-ok" if url_context_seen else "missing-url-context"
        elif last == "summarize-url-system":
            reply = "url-system-context-ok" if url_context_seen and system_context_seen else "missing-url-system-context"
        elif last == "summarize-input":
            reply = "input-context-ok" if input_context_seen else "missing-input-context"
        elif last == "describe-image":
            reply = "image-input-ok" if image_seen else "missing-image-input"
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
