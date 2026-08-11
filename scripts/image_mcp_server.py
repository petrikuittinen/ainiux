#!/usr/bin/env python3
"""Local vision-bridge MCP server for ainiux.

Listens as Streamable HTTP MCP and forwards image analysis to an
OpenAI-compatible Chat Completions endpoint (llama.cpp, vLLM, LM Studio, …).

Example:
  # Vision model on :30000, MCP bridge on :8765
  python3 scripts/image_mcp_server.py http://localhost:30000 --port 8765

  ainiux --add-mcp local-image --mcp-url http://127.0.0.1:8765/mcp --mcp-allow-private
  ainiux deepseek -m deepseek-v4-flash -r "Describe the attached image." \\
    --attach tests/image_files/sea_view.jpg

Stdlib only (Python 3.8+).
"""

from __future__ import annotations

import argparse
import base64
import json
import mimetypes
import os
import sys
import threading
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, List, Optional, Tuple
from urllib.error import HTTPError, URLError
from urllib.parse import urlparse
from urllib.request import Request, urlopen

SERVER_NAME = "ainiux-image-mcp"
SERVER_VERSION = "1.0.0"

DEFAULT_PROMPT = "Describe this image in detail. Be specific about objects, text, colors, and layout."


def tool_result_text(text: str, is_error: bool = False) -> Dict[str, Any]:
    return {
        "content": [{"type": "text", "text": text}],
        "isError": is_error,
        "resultType": "complete",
    }


def normalize_base_url(raw: str) -> str:
    url = raw.strip().rstrip("/")
    if not url:
        raise ValueError("empty base URL")
    if not url.startswith(("http://", "https://")):
        url = "http://" + url
    # Accept .../v1 or bare host:port
    if url.endswith("/v1"):
        return url
    parsed = urlparse(url)
    if parsed.path in ("", "/"):
        return url + "/v1"
    # path already something else — still append /v1 if not present
    if not parsed.path.rstrip("/").endswith("/v1"):
        return url + "/v1"
    return url


def mime_from_path(path: str) -> str:
    ext = os.path.splitext(path)[1].lower()
    mapping = {
        ".png": "image/png",
        ".jpg": "image/jpeg",
        ".jpeg": "image/jpeg",
        ".gif": "image/gif",
        ".webp": "image/webp",
    }
    if ext in mapping:
        return mapping[ext]
    guessed, _ = mimetypes.guess_type(path)
    return guessed or "application/octet-stream"


def _looks_like_base64_blob(value: str) -> bool:
    """Heuristic: long base64 payload (optionally data: URL), not a filesystem path."""
    s = value.strip()
    if s.startswith("data:") and ";base64," in s:
        return True
    if len(s) < 128:
        return False
    if "/" in s or "\\" in s or s.startswith(".") or s.startswith("~"):
        # Still allow pure base64 that happens to include only + / =
        # if it has no path separators beyond base64 alphabet... base64 uses /
        pass
    # Paths with extensions are short; multi-KB alnum/+/= is base64.
    sample = s[:4096].replace("\n", "").replace("\r", "")
    if len(sample) < 128:
        return False
    allowed = set(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/="
    )
    bad = sum(1 for c in sample if c not in allowed)
    return bad == 0 and len(s) >= 128


def _short_path_for_error(path: str, limit: int = 160) -> str:
    if len(path) <= limit:
        return path
    return path[:60] + f"…[{len(path)} chars]…" + path[-40:]


def load_image_from_args(
    args: Dict[str, Any], max_bytes: int
) -> Tuple[Optional[str], Optional[str], Optional[str]]:
    """Return (base64, mime, error_message)."""
    b64 = args.get("image_base64") or args.get("image") or args.get("data") or ""
    path = args.get("path") or args.get("file") or args.get("filename") or ""

    # Older ainiux HTTP rewrite stuffed base64 into `path`. Accept that shape.
    if (not isinstance(b64, str) or not b64.strip()) and isinstance(path, str) and path.strip():
        if _looks_like_base64_blob(path):
            b64 = path.strip()
            path = ""

    if isinstance(b64, str) and b64.strip():
        b64 = b64.strip()
        # strip data-URL prefix if present
        if b64.startswith("data:") and ";base64," in b64:
            header, b64 = b64.split(";base64,", 1)
            mime = header[5:] if header.startswith("data:") else "image/png"
        else:
            mime = str(args.get("mime_type") or args.get("mime") or "image/png")
        try:
            raw = base64.b64decode(b64, validate=False)
        except Exception as exc:  # noqa: BLE001
            return None, None, f"invalid base64: {exc}"
        if len(raw) > max_bytes:
            return None, None, f"image exceeds max-image-bytes ({max_bytes})"
        return b64, mime, None

    if not isinstance(path, str) or not path.strip():
        return None, None, "provide path or image_base64"
    path = os.path.expanduser(path.strip())
    if ".." in path.split(os.sep):
        # soft check; still require absolute resolved path exists
        pass
    if not os.path.isfile(path):
        return None, None, f"image file not found: {_short_path_for_error(path)}"
    size = os.path.getsize(path)
    if size > max_bytes:
        return None, None, (
            f"image file too large ({size} > {max_bytes}): {_short_path_for_error(path)}"
        )
    with open(path, "rb") as fh:
        raw = fh.read()
    mime = str(args.get("mime_type") or args.get("mime") or mime_from_path(path))
    return base64.b64encode(raw).decode("ascii"), mime, None


def extract_message_text(message: Dict[str, Any]) -> str:
    """Prefer assistant content; fall back to reasoning fields (Qwen/llama.cpp)."""
    content = message.get("content")
    if isinstance(content, str) and content.strip():
        return content.strip()
    if isinstance(content, list):
        parts: List[str] = []
        for part in content:
            if isinstance(part, dict) and part.get("type") == "text":
                text = part.get("text")
                if isinstance(text, str) and text:
                    parts.append(text)
            elif isinstance(part, str) and part:
                parts.append(part)
        joined = "\n".join(parts).strip()
        if joined:
            return joined

    for key in ("reasoning_content", "reasoning", "thinking"):
        value = message.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()
    return ""


class VisionClient:
    def __init__(
        self,
        base_v1: str,
        model: str,
        api_key: str,
        timeout: float,
        max_tokens: int,
        enable_thinking: bool,
    ) -> None:
        self.base_v1 = base_v1.rstrip("/")
        self.model = model
        self.api_key = api_key
        self.timeout = timeout
        self.max_tokens = max_tokens
        self.enable_thinking = enable_thinking

    def chat_completions_url(self) -> str:
        return self.base_v1 + "/chat/completions"

    def models_url(self) -> str:
        return self.base_v1 + "/models"

    def _headers(self) -> Dict[str, str]:
        headers = {"Content-Type": "application/json", "Accept": "application/json"}
        if self.api_key:
            headers["Authorization"] = "Bearer " + self.api_key
        return headers

    def fetch_default_model(self) -> Optional[str]:
        try:
            req = Request(self.models_url(), headers=self._headers(), method="GET")
            with urlopen(req, timeout=min(self.timeout, 30.0)) as resp:
                body = json.loads(resp.read().decode("utf-8"))
        except Exception:  # noqa: BLE001
            return None
        data = body.get("data") or body.get("models") or []
        if not isinstance(data, list) or not data:
            return None
        first = data[0]
        if isinstance(first, dict):
            return str(first.get("id") or first.get("name") or first.get("model") or "") or None
        return None

    def describe(self, b64: str, mime: str, prompt: str, max_tokens: Optional[int]) -> str:
        payload: Dict[str, Any] = {
            "model": self.model,
            "messages": [
                {
                    "role": "user",
                    "content": [
                        {"type": "text", "text": prompt},
                        {
                            "type": "image_url",
                            "image_url": {
                                "url": f"data:{mime};base64,{b64}",
                            },
                        },
                    ],
                }
            ],
            "max_tokens": int(max_tokens if max_tokens is not None else self.max_tokens),
            "stream": False,
        }
        # Qwen3 / many llama.cpp builds put long chains in reasoning_content and leave
        # content empty when thinking is on and max_tokens is small. Default off for
        # captions; --enable-thinking opts back in.
        payload["chat_template_kwargs"] = {"enable_thinking": bool(self.enable_thinking)}

        data = json.dumps(payload).encode("utf-8")
        req = Request(
            self.chat_completions_url(),
            data=data,
            headers=self._headers(),
            method="POST",
        )
        try:
            with urlopen(req, timeout=self.timeout) as resp:
                raw = resp.read().decode("utf-8")
        except HTTPError as exc:
            err_body = exc.read().decode("utf-8", errors="replace")[:800]
            raise RuntimeError(f"upstream HTTP {exc.code}: {err_body}") from exc
        except URLError as exc:
            raise RuntimeError(f"upstream connection failed: {exc.reason}") from exc

        try:
            body = json.loads(raw)
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"upstream returned non-JSON: {raw[:200]}") from exc

        if isinstance(body.get("error"), dict):
            msg = body["error"].get("message") or json.dumps(body["error"])
            raise RuntimeError(f"upstream error: {msg}")

        choices = body.get("choices") or []
        if not choices:
            raise RuntimeError("upstream response has no choices")
        message = choices[0].get("message") or {}
        if not isinstance(message, dict):
            raise RuntimeError("upstream message missing or unsupported shape")
        text = extract_message_text(message)
        if text:
            return text
        finish = choices[0].get("finish_reason")
        raise RuntimeError(
            "upstream message.content empty"
            + (f" (finish_reason={finish})" if finish else "")
        )


class ServerConfig:
    def __init__(
        self,
        vision: VisionClient,
        mode: str,
        max_image_bytes: int,
        require_session: bool,
    ) -> None:
        self.vision = vision
        self.mode = mode
        self.max_image_bytes = max_image_bytes
        self.require_session = require_session
        self.sessions: Dict[str, bool] = {}
        self.lock = threading.Lock()


def tools_list() -> List[Dict[str, Any]]:
    props = {
        "path": {
            "type": "string",
            "description": "Absolute filesystem path to a PNG/JPEG/GIF (or WebP if the endpoint allows).",
        },
        "image_base64": {
            "type": "string",
            "description": "Raw base64 image bytes (no data: prefix required; data URLs accepted).",
        },
        "image": {
            "type": "string",
            "description": "Alias for image_base64.",
        },
        "mime_type": {
            "type": "string",
            "description": "MIME type when using base64 (default image/png or from path extension).",
        },
        "prompt": {
            "type": "string",
            "description": "Question or instruction for the vision model.",
        },
        "max_tokens": {
            "type": "integer",
            "description": "Max completion tokens for the vision model.",
            "minimum": 1,
            "maximum": 8192,
        },
    }
    schema = {
        "type": "object",
        "properties": props,
        "required": [],
        "additionalProperties": False,
    }
    return [
        {
            "name": "describe_image",
            "description": (
                "Analyze an image with a local vision-capable OpenAI-compatible model "
                "and return text. Provide path and/or image_base64. Use for OCR, "
                "captions, charts, and visual Q&A when the agent model is text-only."
            ),
            "inputSchema": schema,
        }
    ]


def handle_describe(cfg: ServerConfig, args: Dict[str, Any]) -> Dict[str, Any]:
    b64, mime, err = load_image_from_args(args, cfg.max_image_bytes)
    if err or not b64 or not mime:
        return tool_result_text(err or "failed to load image", is_error=True)
    prompt = args.get("prompt") or args.get("question") or DEFAULT_PROMPT
    if not isinstance(prompt, str) or not prompt.strip():
        prompt = DEFAULT_PROMPT
    max_tokens = args.get("max_tokens")
    try:
        mt = int(max_tokens) if max_tokens is not None else None
    except (TypeError, ValueError):
        mt = None
    try:
        text = cfg.vision.describe(b64, mime, prompt.strip(), mt)
    except Exception as exc:  # noqa: BLE001
        return tool_result_text(str(exc), is_error=True)
    if not text:
        return tool_result_text("vision model returned empty content", is_error=True)
    return tool_result_text(text)


def handle_tools_call(cfg: ServerConfig, params: Dict[str, Any]) -> Dict[str, Any]:
    name = params.get("name") or ""
    args = params.get("arguments") or {}
    if not isinstance(args, dict):
        args = {}
    if name in ("describe_image", "analyze_image"):
        return handle_describe(cfg, args)
    return tool_result_text(f"unknown tool: {name}", is_error=True)


def handle_rpc(
    cfg: ServerConfig,
    message: Dict[str, Any],
    *,
    dialect: str,
    session_id: Optional[str],
) -> Tuple[Optional[Dict[str, Any]], Optional[str], int]:
    if not isinstance(message, dict):
        return (
            {
                "jsonrpc": "2.0",
                "id": None,
                "error": {"code": -32600, "message": "Invalid Request"},
            },
            session_id,
            400,
        )

    msg_id = message.get("id")
    method = message.get("method")
    params = message.get("params") or {}
    if not isinstance(params, dict):
        params = {}
    is_notification = "id" not in message

    if method == "initialize":
        version = params.get("protocolVersion") or "2025-11-25"
        result = {
            "protocolVersion": version,
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
        }
        new_session = session_id or str(uuid.uuid4())
        if is_notification:
            return None, new_session, 202
        return {"jsonrpc": "2.0", "id": msg_id, "result": result}, new_session, 200

    if method in (
        "notifications/initialized",
        "initialized",
        "notifications/cancelled",
    ):
        return None, session_id, 202

    if method == "server/discover":
        result = {
            "protocolVersions": ["2026-07-28", "2025-11-25", "2025-03-26"],
            "capabilities": {"tools": {}},
            "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
            "resultType": "complete",
        }
        return {"jsonrpc": "2.0", "id": msg_id, "result": result}, session_id, 200

    if method == "tools/list":
        result: Dict[str, Any] = {
            "tools": tools_list(),
            "resultType": "complete",
        }
        if dialect == "stateless":
            result["ttlMs"] = 60000
            result["cacheScope"] = "private"
        return {"jsonrpc": "2.0", "id": msg_id, "result": result}, session_id, 200

    if method == "tools/call":
        result = handle_tools_call(cfg, params)
        return {"jsonrpc": "2.0", "id": msg_id, "result": result}, session_id, 200

    if method == "ping":
        return {"jsonrpc": "2.0", "id": msg_id, "result": {}}, session_id, 200

    if is_notification:
        return None, session_id, 202

    return (
        {
            "jsonrpc": "2.0",
            "id": msg_id,
            "error": {"code": -32601, "message": f"Method not found: {method}"},
        },
        session_id,
        200,
    )


def detect_dialect(headers: Dict[str, str], body: Dict[str, Any], mode: str) -> str:
    if mode == "stateless":
        return "stateless"
    if mode == "legacy":
        return "legacy"
    method_header = headers.get("mcp-method") or ""
    version = headers.get("mcp-protocol-version") or ""
    rpc_method = body.get("method") if isinstance(body, dict) else ""
    if method_header or version == "2026-07-28" or rpc_method == "server/discover":
        return "stateless"
    return "legacy"


class Handler(BaseHTTPRequestHandler):
    config: ServerConfig

    def log_message(self, fmt: str, *args: Any) -> None:
        return

    def _read_json(self) -> Tuple[Optional[Dict[str, Any]], Optional[str]]:
        length = int(self.headers.get("Content-Length") or "0")
        raw = self.rfile.read(length) if length > 0 else b""
        if not raw:
            return None, "empty body"
        try:
            value = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            return None, str(exc)
        if not isinstance(value, dict):
            return None, "body must be a JSON object"
        return value, None

    def _send_json(
        self,
        status: int,
        body: Optional[Dict[str, Any]],
        extra_headers: Optional[Dict[str, str]] = None,
    ) -> None:
        data = b"" if body is None else json.dumps(body, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        if body is not None:
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
        if extra_headers:
            for key, value in extra_headers.items():
                self.send_header(key, value)
        self.end_headers()
        if data:
            self.wfile.write(data)

    def do_GET(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        if path in ("/", "/health"):
            self._send_json(
                200,
                {
                    "ok": True,
                    "service": SERVER_NAME,
                    "version": SERVER_VERSION,
                    "mode": self.config.mode,
                    "upstream": self.config.vision.base_v1,
                    "model": self.config.vision.model,
                },
            )
            return
        self.send_response(405)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(b"Use POST /mcp")

    def do_POST(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        if path not in ("/mcp", "/"):
            self._send_json(
                404,
                {
                    "jsonrpc": "2.0",
                    "id": None,
                    "error": {"code": -32000, "message": f"unknown path {path}"},
                },
            )
            return

        body, err = self._read_json()
        if err or body is None:
            self._send_json(
                400,
                {
                    "jsonrpc": "2.0",
                    "id": None,
                    "error": {"code": -32700, "message": err or "parse error"},
                },
            )
            return

        header_map = {k.lower(): v for k, v in self.headers.items()}
        dialect = detect_dialect(header_map, body, self.config.mode)

        if dialect == "stateless" and self.config.mode in ("stateless", "both"):
            method_header = header_map.get("mcp-method", "")
            rpc_method = str(body.get("method") or "")
            if method_header and rpc_method and method_header != rpc_method:
                self._send_json(
                    400,
                    {
                        "jsonrpc": "2.0",
                        "id": body.get("id"),
                        "error": {
                            "code": -32020,
                            "message": "Mcp-Method header does not match body method",
                        },
                    },
                )
                return

        session_id = header_map.get("mcp-session-id")
        if dialect == "legacy" and self.config.require_session:
            method = body.get("method")
            if method != "initialize" and not session_id:
                self._send_json(
                    400,
                    {
                        "jsonrpc": "2.0",
                        "id": body.get("id"),
                        "error": {
                            "code": -32000,
                            "message": "Mcp-Session-Id required",
                        },
                    },
                )
                return

        response, new_session, status = handle_rpc(
            self.config, body, dialect=dialect, session_id=session_id
        )
        extra: Dict[str, str] = {}
        if dialect == "legacy" and new_session and body.get("method") == "initialize":
            extra["Mcp-Session-Id"] = new_session
            with self.config.lock:
                self.config.sessions[new_session] = True

        if response is None:
            self._send_json(202, None, extra if extra else None)
            return
        self._send_json(status, response, extra if extra else None)

    def do_DELETE(self) -> None:  # noqa: N802
        self.send_response(405)
        self.end_headers()


def run_self_test(cfg: ServerConfig, image_path: str) -> int:
    print(f"self-test: describing {image_path} via {cfg.vision.chat_completions_url()}", flush=True)
    result = handle_describe(cfg, {"path": image_path, "prompt": "In one sentence, what is shown?"})
    text = result["content"][0]["text"]
    if result.get("isError"):
        print("self-test FAILED:", text, file=sys.stderr)
        return 1
    print("self-test OK:", text[:500])
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Local OpenAI-compatible vision bridge as an MCP server for ainiux"
    )
    parser.add_argument(
        "base_url",
        help="OpenAI-compatible base (e.g. http://localhost:30000 or http://localhost:30000/v1)",
    )
    parser.add_argument("--host", default="127.0.0.1", help="Bind address (default loopback)")
    parser.add_argument(
        "-p",
        "--port",
        type=int,
        default=8765,
        help="MCP listen port (default 8765)",
    )
    parser.add_argument(
        "-m",
        "--model",
        default="",
        help="Vision model id (default: first entry from /v1/models)",
    )
    parser.add_argument(
        "--api-key",
        default=os.environ.get("OPENAI_API_KEY", ""),
        help="Optional Bearer token (or OPENAI_API_KEY)",
    )
    parser.add_argument("--timeout", type=float, default=120.0, help="Upstream timeout seconds")
    parser.add_argument(
        "--max-tokens",
        type=int,
        default=1024,
        help="Default max_tokens for vision completions",
    )
    parser.add_argument(
        "--max-image-bytes",
        type=int,
        default=20 * 1024 * 1024,
        help="Max image file / decoded size",
    )
    parser.add_argument(
        "--mode",
        choices=("legacy", "stateless", "both"),
        default="both",
        help="MCP dialect mode (default both)",
    )
    parser.add_argument(
        "--require-session",
        action="store_true",
        help="Legacy mode: require Mcp-Session-Id after initialize",
    )
    parser.add_argument(
        "--enable-thinking",
        action="store_true",
        help=(
            "Send chat_template_kwargs.enable_thinking=true (default: false so "
            "Qwen-style models return content instead of only reasoning_content)"
        ),
    )
    parser.add_argument(
        "--self-test",
        metavar="IMAGE_PATH",
        help="Call the upstream vision model once with IMAGE_PATH and exit",
    )
    args = parser.parse_args(argv)

    try:
        base_v1 = normalize_base_url(args.base_url)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    vision = VisionClient(
        base_v1=base_v1,
        model=args.model,
        api_key=args.api_key,
        timeout=args.timeout,
        max_tokens=args.max_tokens,
        enable_thinking=args.enable_thinking,
    )
    if not vision.model:
        detected = vision.fetch_default_model()
        if not detected:
            print(
                "error: could not auto-detect model from /v1/models; pass --model MODEL",
                file=sys.stderr,
            )
            return 2
        vision.model = detected

    cfg = ServerConfig(
        vision=vision,
        mode=args.mode,
        max_image_bytes=args.max_image_bytes,
        require_session=args.require_session,
    )

    if args.self_test:
        return run_self_test(cfg, args.self_test)

    Handler.config = cfg
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    host, port = server.server_address[:2]
    thinking = "on" if vision.enable_thinking else "off"
    print(
        f"ainiux-image-mcp listening on http://{host}:{port}/mcp "
        f"→ {vision.base_v1} model={vision.model} mode={args.mode} thinking={thinking}",
        flush=True,
    )
    print(
        f"Install: ainiux --add-mcp local-image --mcp-url http://127.0.0.1:{port}/mcp "
        f"--mcp-allow-private",
        flush=True,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nshutting down", flush=True)
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
