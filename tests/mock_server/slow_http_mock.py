#!/usr/bin/env python3
"""Local HTTP mock for timeout and slow-response unit tests."""

import argparse
import socket
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    response_delay_seconds = 0.0
    chunk_delay_seconds = 0.0
    chunk_count = 8

    def log_message(self, fmt, *args):
        return

    def _send_bytes(self, status, body, content_type="text/plain; charset=utf-8"):
        data = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except BrokenPipeError:
            return

    def do_GET(self):
        if self.path == "/health":
            self._send_bytes(200, "ok")
            return

        if self.path.startswith("/delay/"):
            try:
                delay = float(self.path.split("/delay/", 1)[1].split("/", 1)[0])
            except ValueError:
                self._send_bytes(400, "bad delay")
                return
            time.sleep(max(0.0, delay))
            self._send_bytes(200, "delayed response")
            return

        if self.path == "/slow-body":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            try:
                for _ in range(self.chunk_count):
                    time.sleep(max(0.0, self.chunk_delay_seconds))
                    chunk = b"x" * 128
                    self.wfile.write(f"{len(chunk):x}\r\n".encode("ascii"))
                    self.wfile.write(chunk)
                    self.wfile.write(b"\r\n")
                    self.wfile.flush()
                self.wfile.write(b"0\r\n\r\n")
                self.wfile.flush()
            except BrokenPipeError:
                return
            return

        self._send_bytes(404, "not found")


def wait_for_listen(host, port, timeout_seconds=5.0):
    deadline = time.time() + timeout_seconds
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--response-delay", type=float, default=0.0)
    parser.add_argument("--chunk-delay", type=float, default=0.0)
    parser.add_argument("--chunk-count", type=int, default=8)
    parser.add_argument("--ready-fd", type=int, default=-1,
                        help="Write one readiness byte to this file descriptor when listening")
    args = parser.parse_args()

    Handler.response_delay_seconds = args.response_delay
    Handler.chunk_delay_seconds = args.chunk_delay
    Handler.chunk_count = max(1, args.chunk_count)

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads = True

    if args.ready_fd >= 0:
        os_write = getattr(__import__("os"), "write", None)
        if os_write is not None:
            os_write(args.ready_fd, b"1")

    print(f"slow_http_mock listening on {args.host}:{args.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)