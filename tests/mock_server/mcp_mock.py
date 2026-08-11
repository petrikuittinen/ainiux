#!/usr/bin/env python3
"""Local MCP mock for ainiux tests.

Supports:
  - Streamable HTTP legacy (initialize + optional session)  --mode legacy
  - Streamable HTTP stateless 2026-07-28                   --mode stateless
  - Both on one port (probe by request shape)              --mode both
  - stdio newline JSON-RPC                                 --stdio

Tools: echo, add, slow, fail
"""

from __future__ import annotations

import argparse
import json
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, Optional, Tuple
from urllib.parse import urlparse


TOOLS = [
    {
        "name": "echo",
        "description": "Echo the text argument.",
        "inputSchema": {
            "type": "object",
            "properties": {"text": {"type": "string"}},
            "required": ["text"],
        },
    },
    {
        "name": "add",
        "description": "Add two numbers a and b.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "a": {"type": "number"},
                "b": {"type": "number"},
            },
            "required": ["a", "b"],
        },
    },
    {
        "name": "slow",
        "description": "Sleep for seconds then return ok.",
        "inputSchema": {
            "type": "object",
            "properties": {"seconds": {"type": "number"}},
            "required": [],
        },
    },
    {
        "name": "fail",
        "description": "Always return a tool error.",
        "inputSchema": {
            "type": "object",
            "properties": {"message": {"type": "string"}},
            "required": [],
        },
    },
]


def tool_result_text(text: str, is_error: bool = False) -> Dict[str, Any]:
    return {
        "content": [{"type": "text", "text": text}],
        "isError": is_error,
        "resultType": "complete",
    }


def handle_tools_call(params: Dict[str, Any]) -> Dict[str, Any]:
    name = params.get("name") or ""
    args = params.get("arguments") or {}
    if not isinstance(args, dict):
        args = {}
    if name == "echo":
        return tool_result_text(str(args.get("text", "")))
    if name == "add":
        try:
            total = float(args.get("a", 0)) + float(args.get("b", 0))
        except (TypeError, ValueError):
            return tool_result_text("invalid numbers", is_error=True)
        return tool_result_text(str(total))
    if name == "slow":
        try:
            seconds = float(args.get("seconds", 1.0))
        except (TypeError, ValueError):
            seconds = 1.0
        seconds = max(0.0, min(seconds, 30.0))
        time.sleep(seconds)
        return tool_result_text("slept")
    if name == "fail":
        msg = str(args.get("message") or "intentional failure")
        return tool_result_text(msg, is_error=True)
    return tool_result_text(f"unknown tool: {name}", is_error=True)


def handle_rpc(
    message: Dict[str, Any],
    *,
    dialect: str,
    session_id: Optional[str],
) -> Tuple[Optional[Dict[str, Any]], Optional[str], int]:
    """Return (response_or_none_for_notification, new_session_id, http_status)."""
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

    # Notifications have no id.
    is_notification = "id" not in message

    if method == "initialize":
        version = params.get("protocolVersion") or "2025-11-25"
        result = {
            "protocolVersion": version,
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": "ainiux-mcp-mock", "version": "1.0.0"},
        }
        new_session = session_id or str(uuid.uuid4())
        if is_notification:
            return None, new_session, 202
        return (
            {"jsonrpc": "2.0", "id": msg_id, "result": result},
            new_session,
            200,
        )

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
            "serverInfo": {"name": "ainiux-mcp-mock", "version": "1.0.0"},
            "resultType": "complete",
        }
        return {"jsonrpc": "2.0", "id": msg_id, "result": result}, session_id, 200

    if method == "tools/list":
        result: Dict[str, Any] = {
            "tools": TOOLS,
            "resultType": "complete",
        }
        if dialect == "stateless":
            result["ttlMs"] = 60000
            result["cacheScope"] = "private"
        return {"jsonrpc": "2.0", "id": msg_id, "result": result}, session_id, 200

    if method == "tools/call":
        result = handle_tools_call(params)
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


class State:
    def __init__(self, mode: str, require_session: bool):
        self.mode = mode
        self.require_session = require_session
        self.sessions: Dict[str, bool] = {}
        self.lock = threading.Lock()


def detect_dialect(headers: Dict[str, str], body: Dict[str, Any], mode: str) -> str:
    if mode == "stateless":
        return "stateless"
    if mode == "legacy":
        return "legacy"
    # both: prefer stateless when Mcp-Method present or method is server/discover
    method_header = headers.get("mcp-method") or headers.get("Mcp-Method") or ""
    version = headers.get("mcp-protocol-version") or headers.get("MCP-Protocol-Version") or ""
    rpc_method = body.get("method") if isinstance(body, dict) else ""
    if method_header or version == "2026-07-28" or rpc_method == "server/discover":
        return "stateless"
    return "legacy"


class Handler(BaseHTTPRequestHandler):
    state: State

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

    def do_GET(self) -> None:
        path = urlparse(self.path).path
        if path in ("/", "/health"):
            self._send_json(
                200,
                {
                    "ok": True,
                    "mode": self.state.mode,
                    "service": "ainiux-mcp-mock",
                },
            )
            return
        # Legacy SSE endpoint stub: return 405 so clients fall back or stop.
        self.send_response(405)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(b"GET not supported for MCP mock; use POST /mcp")

    def do_POST(self) -> None:
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
        dialect = detect_dialect(header_map, body, self.state.mode)

        if dialect == "stateless" and self.state.mode in ("stateless", "both"):
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
            if body.get("method") == "tools/call":
                name_header = header_map.get("mcp-name", "")
                params = body.get("params") or {}
                tool_name = ""
                if isinstance(params, dict):
                    tool_name = str(params.get("name") or "")
                if name_header and tool_name and name_header != tool_name:
                    self._send_json(
                        400,
                        {
                            "jsonrpc": "2.0",
                            "id": body.get("id"),
                            "error": {
                                "code": -32020,
                                "message": "Mcp-Name header does not match tool name",
                            },
                        },
                    )
                    return

        session_id = header_map.get("mcp-session-id")
        if dialect == "legacy" and self.state.require_session:
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
            body, dialect=dialect, session_id=session_id
        )
        extra: Dict[str, str] = {}
        if dialect == "legacy" and new_session and body.get("method") == "initialize":
            extra["Mcp-Session-Id"] = new_session
            with self.state.lock:
                self.state.sessions[new_session] = True

        if response is None:
            self._send_json(202 if status == 202 else 202, None, extra if extra else None)
            return
        self._send_json(status, response, extra if extra else None)

    def do_DELETE(self) -> None:
        self.send_response(405)
        self.end_headers()


def run_http(host: str, port: int, mode: str, require_session: bool) -> None:
    Handler.state = State(mode=mode, require_session=require_session)
    server = ThreadingHTTPServer((host, port), Handler)
    # Print the bound port for test harnesses.
    print(f"ainiux-mcp-mock listening on http://{host}:{server.server_address[1]}/mcp mode={mode}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


def run_stdio(mode: str) -> None:
    session_id: Optional[str] = None
    dialect = "stateless" if mode == "stateless" else "legacy"
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            err = {
                "jsonrpc": "2.0",
                "id": None,
                "error": {"code": -32700, "message": "parse error"},
            }
            sys.stdout.write(json.dumps(err, separators=(",", ":")) + "\n")
            sys.stdout.flush()
            continue
        if not isinstance(message, dict):
            continue
        # Auto dialect from message for --stdio --mode both
        local_dialect = dialect
        if mode == "both":
            if message.get("method") == "server/discover":
                local_dialect = "stateless"
            elif message.get("method") == "initialize":
                local_dialect = "legacy"
        response, session_id, _status = handle_rpc(
            message, dialect=local_dialect, session_id=session_id
        )
        if response is not None:
            sys.stdout.write(json.dumps(response, separators=(",", ":")) + "\n")
            sys.stdout.flush()


def main() -> int:
    parser = argparse.ArgumentParser(description="ainiux MCP mock server")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=0, help="0 = ephemeral")
    parser.add_argument(
        "--mode",
        choices=("legacy", "stateless", "both"),
        default="both",
    )
    parser.add_argument(
        "--require-session",
        action="store_true",
        help="legacy mode: require Mcp-Session-Id after initialize",
    )
    parser.add_argument(
        "--stdio",
        action="store_true",
        help="speak MCP over stdin/stdout instead of HTTP",
    )
    args = parser.parse_args()
    if args.stdio:
        run_stdio(args.mode)
        return 0
    run_http(args.host, args.port, args.mode, args.require_session)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
