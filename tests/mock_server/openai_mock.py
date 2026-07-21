#!/usr/bin/env python3
import argparse
import json
import re
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
        if self.path == "/plain":
            accept = self.headers.get("Accept", "")
            if "text/plain" not in accept:
                self._send(406, "plain text required", "text/plain; charset=utf-8")
                return
            self._send(200, "Benchmark reference text: 你好 مرحبا\n", "text/plain; charset=utf-8")
            return
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
        if self.path in ("/v1/models", "/v1/models-multiple"):
            if self.empty_models:
                self._send(200, json.dumps({"object": "list", "data": []}))
                return
            model_ids = [self.model]
            if self.path == "/v1/models-multiple":
                model_ids.append(self.model + "-second")
            self._send(
                200,
                json.dumps(
                    {
                        "object": "list",
                        "data": [
                            {
                                "id": model_id,
                                "object": "model",
                                "context_length": 10000,
                            }
                            for model_id in model_ids
                        ],
                    }
                ),
            )
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
            return "", 0
        content = messages[-1].get("content", "")
        if isinstance(content, str):
            return content, 0
        if not isinstance(content, list):
            return "", 0
        text = []
        image_count = 0
        for part in content:
            if not isinstance(part, dict):
                continue
            if part.get("type") == "text" and isinstance(part.get("text"), str):
                text.append(part["text"])
            if part.get("type") == "image_url" and isinstance(part.get("image_url"), dict):
                url = part["image_url"].get("url", "")
                if isinstance(url, str) and url.startswith(("data:image/png;base64,", "data:image/jpeg;base64,", "data:image/gif;base64,")):
                    image_count += 1
        return "".join(text), image_count

    def _chat_image_count(self, request):
        count = 0
        for message in request.get("messages", []):
            if not isinstance(message, dict):
                continue
            content = message.get("content", [])
            if not isinstance(content, list):
                continue
            for part in content:
                if not isinstance(part, dict) or part.get("type") != "image_url":
                    continue
                image = part.get("image_url", {})
                url = image.get("url", "") if isinstance(image, dict) else ""
                if isinstance(url, str) and url.startswith("data:image/"):
                    count += 1
        return count

    def _chat_text(self, request):
        chunks = []
        for message in request.get("messages", []):
            if not isinstance(message, dict):
                continue
            content = message.get("content", "")
            if isinstance(content, str):
                chunks.append(content)
                continue
            if not isinstance(content, list):
                continue
            for part in content:
                if isinstance(part, dict) and part.get("type") == "text":
                    text = part.get("text", "")
                    if isinstance(text, str):
                        chunks.append(text)
        return "\n".join(chunks)

    def _fail_validation(self, message):
        self._send(400, json.dumps({"error": {"message": message}}))
        return False

    def _forbid_fields(self, request, fields, label):
        for name in fields:
            if name in request:
                return self._fail_validation(f"{label}: unexpected {name}")
        return True

    def _validate_request_shape(self, request, last, responses=False):
        if responses:
            if last != "expect-openai-responses-reasoning":
                return True
            if request.get("reasoning") != {"effort": 4096}:
                return self._fail_validation("openai responses: expected exact reasoning.effort=4096")
            return self._forbid_fields(
                request,
                ["reasoning_effort", "thinking", "enable_thinking", "thinking_budget"],
                "openai responses",
            )

        if last == "expect-openai-chat-reasoning":
            if request.get("reasoning_effort") != "high":
                return self._fail_validation("openai chat: expected reasoning_effort=high")
            return self._forbid_fields(
                request,
                ["reasoning", "thinking", "enable_thinking", "thinking_budget"],
                "openai chat",
            )
        if last == "expect-restored-image" and self._chat_image_count(request) < 1:
            return self._fail_validation("restored chat: expected a historical image")
        if last == "expect-restored-markdown":
            text = self._chat_text(request)
            if "Persistent HTML heading" not in text or "small-native-marker" not in text:
                return self._fail_validation(
                    "restored chat: expected historical inline and managed Markdown"
                )
            if "<h1>" in text:
                return self._fail_validation(
                    "restored chat: HTML attachment was not converted to Markdown"
                )
        if last == "expect-anthropic-thinking":
            if request.get("thinking") != {"type": "enabled", "budget_tokens": 2048}:
                return self._fail_validation("anthropic: expected thinking enabled with budget_tokens=2048")
            return self._forbid_fields(
                request,
                ["reasoning", "reasoning_effort", "enable_thinking", "thinking_budget"],
                "anthropic",
            )
        if last == "expect-gemini-reasoning":
            if request.get("reasoning_effort") != 4096:
                return self._fail_validation("gemini: expected exact reasoning_effort=4096")
            return self._forbid_fields(
                request,
                ["reasoning", "extra_body", "thinking", "enable_thinking", "thinking_budget"],
                "gemini",
            )
        if last == "expect-kimi-thinking":
            if request.get("reasoning_effort") != "off":
                return self._fail_validation("kimi: expected reasoning_effort=off")
            return self._forbid_fields(
                request,
                ["reasoning", "thinking", "enable_thinking", "thinking_budget"],
                "kimi",
            )
        if last == "expect-deepseek-v4-thinking":
            if request.get("thinking") != {"type": "enabled"}:
                return self._fail_validation("deepseek: expected thinking enabled")
            if request.get("reasoning_effort") != "xhigh":
                return self._fail_validation("deepseek: expected reasoning_effort=xhigh")
            return self._forbid_fields(
                request,
                ["reasoning", "enable_thinking", "thinking_budget"],
                "deepseek",
            )
        if last == "expect-qwen-thinking":
            if request.get("enable_thinking") is not True:
                return self._fail_validation("qwen: expected enable_thinking=true")
            if request.get("thinking_budget") != "high":
                return self._fail_validation("qwen: expected unmodified thinking_budget=high")
            return self._forbid_fields(
                request,
                ["reasoning", "reasoning_effort", "thinking"],
                "qwen",
            )
        if last == "expect-glm-thinking":
            if request.get("thinking") != {"type": "enabled"}:
                return self._fail_validation("glm: expected thinking enabled")
            if request.get("reasoning_effort") != "xhigh":
                return self._fail_validation("glm: expected reasoning_effort=xhigh")
            return self._forbid_fields(
                request,
                ["reasoning", "enable_thinking", "thinking_budget"],
                "glm",
            )
        return True

    def _handle_responses(self, request):
        if request.get("model") == "":
            self._send(400, json.dumps({"error": {"message": "empty model field"}}))
            return
        last = self._responses_last_input(request)
        if not self._validate_request_shape(request, last, responses=True):
            return
        input_items = request.get("input", [])
        system_text = request.get("instructions", "")
        if not isinstance(system_text, str):
            system_text = ""
        system_text += "\n" + "\n".join(
            item.get("content", "")
            for item in input_items
            if isinstance(item, dict)
            and item.get("role") == "system"
            and isinstance(item.get("content"), str)
        )
        # Detect security-review from the task-layer prompt / tool schema, not
        # the shared master foundation wording (which may change).
        if request.get("tools") and (
            "submit_security_review" in system_text
            or "Review the supplied source batch" in system_text
        ):
            if "serialized cross-project coordinator" in system_text:
                reply = json.dumps(
                    {"keep": [], "reject": [], "merge": [], "findings": [], "notes": []}
                )
                self._send(
                    200,
                    json.dumps(
                        {
                            "id": "resp_review_coordinator",
                            "object": "response",
                            "model": self.model,
                            "status": "completed",
                            "output": [{"type": "message", "role": "assistant", "content": [{"type": "output_text", "text": reply, "annotations": []}]}],
                        }
                    ),
                )
                return
            tool_result_seen = any(
                isinstance(item, dict) and item.get("type") == "function_call_output"
                for item in input_items
            )
            user_text = "\n".join(
                item.get("content", "")
                for item in input_items
                if isinstance(item, dict)
                and item.get("role") == "user"
                and isinstance(item.get("content"), str)
            )
            path_matches = re.findall(r"## File path \(JSON\): (\"(?:\\.|[^\"])*\")", user_text)
            review_paths = [json.loads(path) for path in path_matches] or ["review.cpp"]
            if not tool_result_seen:
                output = [{
                    "type": "function_call",
                    "id": "fc_review_1",
                    "call_id": "review_call_1",
                    "name": "read_file",
                    "arguments": json.dumps({"path": review_paths[0], "start_line": 1, "end_line": 1}),
                }]
            else:
                output = [{
                    "type": "function_call",
                    "id": "fc_review_submit_1",
                    "call_id": "review_submit_1",
                    "name": "submit_security_review",
                    "arguments": json.dumps(
                        {"findings": [], "coverage": review_paths, "notes": []}
                    ),
                }]
            self._send(
                200,
                json.dumps(
                    {
                        "id": "resp_review_worker",
                        "object": "response",
                        "model": self.model,
                        "status": "completed",
                        "output": output,
                    }
                ),
            )
            return
        reply = "Hello"
        if last == "model?":
            reply = request.get("model", "<missing>")
        elif last == "reasoning":
            reply = "Visible answer"
        elif last == "markdown":
            reply = "# Mock Title\n\nHello **bold** and [docs](https://example.com/docs)."
        elif last == "expect-openai-responses-reasoning":
            reply = "request-ok"

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
        last, image_count = self._chat_last_input(request)
        if not self._validate_request_shape(request, last):
            return
        system_text = "\n".join(
            message.get("content", "")
            for message in messages
            if isinstance(message, dict)
            and message.get("role") == "system"
            and isinstance(message.get("content"), str)
        )
        # Detect security-review from the task-layer prompt / tool schema, not
        # the shared master foundation wording (which may change).
        if request.get("tools") and (
            "submit_security_review" in system_text
            or "Review the supplied source batch" in system_text
        ):
            if "serialized cross-project coordinator" in system_text:
                reply = json.dumps(
                    {"keep": [], "reject": [], "merge": [], "findings": [], "notes": []}
                )
                self._send(
                    200,
                    json.dumps(
                        {
                            "model": self.model,
                            "choices": [{"index": 0, "finish_reason": "stop", "message": {"role": "assistant", "content": reply}}],
                        }
                    ),
                )
                return
            tool_result_seen = any(
                isinstance(message, dict) and message.get("role") == "tool"
                for message in messages
            )
            tool_result_count = sum(
                1
                for message in messages
                if isinstance(message, dict) and message.get("role") == "tool"
            )
            user_text = "\n".join(
                message.get("content", "")
                for message in messages
                if isinstance(message, dict)
                and message.get("role") == "user"
                and isinstance(message.get("content"), str)
            )
            path_matches = re.findall(r"## File path \(JSON\): (\"(?:\\.|[^\"])*\")", user_text)
            review_paths = [json.loads(path) for path in path_matches] or ["review.cpp"]
            review_path = review_paths[0]
            overexplore = "OVEREXPLORE_REVIEW" in user_text
            finalization_requested = "Inspection budget is nearly exhausted." in user_text
            if not tool_result_seen or (overexplore and not finalization_requested):
                self._send(
                    200,
                    json.dumps(
                        {
                            "model": self.model,
                            "choices": [
                                {
                                    "index": 0,
                                    "finish_reason": "tool_calls",
                                    "message": {
                                        "role": "assistant",
                                        "content": None,
                                        "reasoning_details": [{"type": "reasoning.encrypted", "data": "opaque-review-state"}],
                                        "tool_calls": [
                                            {
                                                "id": f"review_call_{tool_result_count + 1}",
                                                "type": "function",
                                                "function": {
                                                    "name": "read_file",
                                                    "arguments": json.dumps({"path": review_path, "start_line": 1, "end_line": 1}),
                                                },
                                            }
                                        ],
                                    },
                                }
                            ],
                        }
                    ),
                )
                return
            if "MALFORMED_REVIEW_OUTPUT" in user_text:
                reply = "{malformed review output"
                choice = {
                    "index": 0,
                    "finish_reason": "stop",
                    "message": {"role": "assistant", "content": reply},
                }
            else:
                review_findings = []
                if overexplore:
                    review_findings = [{
                        "path": review_path,
                        "line_start": 1,
                        "line_end": 1,
                        "impact": "Mock evidence-backed impact with omitted optional metadata",
                    }]
                choice = {
                    "index": 0,
                    "finish_reason": "tool_calls",
                    "message": {
                        "role": "assistant",
                        "content": None,
                        "tool_calls": [{
                            "id": "review_submit_1",
                            "type": "function",
                            "function": {
                                "name": "submit_security_review",
                                "arguments": json.dumps(
                                    {"findings": review_findings, "coverage": review_paths}
                                ),
                            },
                        }],
                    },
                }
            self._send(
                200,
                json.dumps(
                    {
                        "model": self.model,
                        "choices": [choice],
                    }
                ),
            )
            return
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
            and "Input context from" in message.get("content", "")
            and "Local Input Title" in message.get("content", "")
            for message in messages
        )
        attachment_alpha_seen = any(
            isinstance(message, dict)
            and isinstance(message.get("content"), str)
            and "Input context from" in message.get("content", "")
            and "Attachment Alpha" in message.get("content", "")
            for message in messages
        )
        attachment_beta_seen = any(
            isinstance(message, dict)
            and isinstance(message.get("content"), str)
            and "Input context from" in message.get("content", "")
            and "Attachment Beta" in message.get("content", "")
            for message in messages
        )
        inserted_context_seen = any(
            isinstance(message, dict)
            and isinstance(message.get("content"), str)
            and "Inserted Context Marker" in message.get("content", "")
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
        elif last == "summarize-attachments":
            reply = "attachments-ok" if attachment_alpha_seen and attachment_beta_seen else "missing-attachments"
        elif last == "summarize-insert" or last.endswith("summarize-insert"):
            reply = "insert-ok" if inserted_context_seen else "missing-insert"
        elif last == "describe-image":
            reply = "image-input-ok" if image_count == 1 else "missing-image-input"
        elif last == "describe-images":
            reply = f"images:{image_count}"
        elif last == "previous-assistant":
            reply = ""
            for message in reversed(messages[:-1]):
                if isinstance(message, dict) and message.get("role") == "assistant":
                    reply = message.get("content", "")
                    break
        elif last.startswith("expect-") and last.endswith(("-reasoning", "-thinking")):
            reply = "request-ok"
        elif last.startswith("AINIUX_CODE_CONTEXT_V1\n"):
            expected_prefix = "def greet(name):\n    "
            expected_postfix = "\n\nprint(greet(\"Ada\"))\n"
            prefix_frame = f"PREFIX_BYTES {len(expected_prefix.encode('utf-8'))}\n{expected_prefix}\n<CURSOR/>\n"
            postfix_frame = f"POSTFIX_BYTES {len(expected_postfix.encode('utf-8'))}\n{expected_postfix}\n"
            if (
                "LANGUAGE python\n" in last
                and prefix_frame in last
                and postfix_frame in last
            ):
                reply = "```python\nreturn f\"Hello, {name}!\"\n```"
            else:
                print("invalid editor code completion context:", repr(last), flush=True)
                reply = "```python\nraise RuntimeError(\"bad completion context\")\n```"
        elif last.startswith("AINIUX_PROSE_CONTEXT_V1\n"):
            expected_prefix = "Mara entered the silent observatory. The clock stopped.\n"
            expected_postfix = "\nAt dawn, the brass door was sealed again.\n"
            prefix_frame = (
                f"PREFIX_BYTES {len(expected_prefix.encode('utf-8'))}\n"
                f"{expected_prefix}\nCURSOR_BYTES 9\n<CURSOR/>\n"
            )
            postfix_frame = (
                f"POSTFIX_BYTES {len(expected_postfix.encode('utf-8'))}\n"
                f"{expected_postfix}\n"
            )
            prose_rules_seen = any(
                isinstance(message, dict)
                and message.get("role") == "system"
                and "natural bridge" in message.get("content", "")
                and "Never summarize, paraphrase, recap, restart, repeat" in message.get("content", "")
                for message in messages
            )
            if (
                "MODE_BYTES 4\ntext\n" in last
                and prefix_frame in last
                and postfix_frame in last
                and prose_rules_seen
            ):
                reply = "<content>A hidden panel opened, and she slipped inside before the gears resumed.\n</content>"
            else:
                print("invalid editor prose completion context:", repr(last), flush=True)
                reply = "<content>BAD PROSE CONTEXT</content>"
        elif (
            len(messages) == 2
            and isinstance(messages[0], dict)
            and messages[0].get("role") == "system"
            and messages[0].get("content") == "Integration grading system prompt."
            and last.startswith("Integration case prompt.\n")
            and last.endswith("\nIntegration case end.")
        ):
            payload_text = last[
                len("Integration case prompt.\n") : -len("\nIntegration case end.")
            ]
            try:
                grade_payload = json.loads(payload_text)
                evaluation_items = grade_payload["evaluation_basis"]["evaluation_items"]
                transcript = grade_payload["transcript"]
                if not isinstance(evaluation_items, list) or not isinstance(transcript, list):
                    raise ValueError("invalid grading arrays")
                if grade_payload.get("id") != "grade-cancel" and (
                    request.get("stream") is not False or request.get("temperature") != 0
                ):
                    raise ValueError("grading must default to stream=false and temperature=0")
                if grade_payload.get("id") == "judge-malformed":
                    reply = "{malformed judge output"
                else:
                    reply = json.dumps(
                        {
                            "score": 100,
                            "verdict": "pass",
                            "rationale": f"Audited {len(transcript)} transcript messages.",
                            "criteria": [
                                {
                                    "index": index,
                                    "verdict": "met",
                                    "reason": "The response satisfies this evaluation item.",
                                }
                                for index in range(len(evaluation_items))
                            ],
                        },
                        ensure_ascii=False,
                    )
            except (KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
                print("invalid grading request:", repr(last), repr(exc), flush=True)
                reply = "{invalid grading request"

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
                    "data: " + json.dumps({"model": self.model, "choices": [], "usage": {"prompt_tokens": len(messages), "completion_tokens": 1, "total_tokens": len(messages) + 1}}) + "\n\n",
                    "data: [DONE]\n\n",
                ]
            else:
                chunks = [
                    "data: " + json.dumps({"choices": [{"delta": {"content": reply[:midpoint]}}]}) + "\n\n",
                    ": comment\n\n",
                    "data: " + json.dumps({"choices": [{"delta": {"content": reply[midpoint:]}}]}) + "\n\n",
                    "data: " + json.dumps({"model": self.model, "choices": [], "usage": {"prompt_tokens": len(messages), "completion_tokens": 1, "total_tokens": len(messages) + 1}}) + "\n\n",
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
