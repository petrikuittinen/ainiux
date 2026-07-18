#!/usr/bin/env python3
"""Verify provider/model startup behavior in the chat TUI."""

import os
import pty
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time


def drain(master, timeout=0.5):
    output = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        ready, _, _ = select.select(
            [master], [], [], max(0.0, min(0.05, deadline - time.time()))
        )
        if not ready:
            continue
        try:
            chunk = os.read(master, 65536)
        except OSError:
            break
        if not chunk:
            break
        output.extend(chunk)
    return bytes(output)


def send(master, data, delay=0.35):
    if isinstance(data, str):
        data = data.encode("utf-8")
    os.write(master, data)
    time.sleep(delay)
    return drain(master)


def plain(raw):
    decoded = raw.decode("utf-8", errors="replace")
    return re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", decoded)


def require(raw, needle, context):
    rendered = plain(raw)
    if needle not in rendered:
        raise RuntimeError(f"expected {needle!r} while {context}; saw {rendered[-700:]!r}")


def run_case(binary, args, actions):
    home_dir = tempfile.mkdtemp(prefix="ainiux-tui-startup-")
    env = os.environ.copy()
    env["HOME"] = home_dir
    env["TERM"] = env.get("TERM", "xterm-256color")
    master, slave = pty.openpty()
    process = subprocess.Popen(
        [binary, *args],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
        env=env,
    )
    os.close(slave)
    output = bytearray()
    try:
        time.sleep(0.5)
        output.extend(drain(master, 1.0))
        for data, delay in actions:
            output.extend(send(master, data, delay))
        process.wait(timeout=10)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=2)
        os.close(master)
        shutil.rmtree(home_dir, ignore_errors=True)
    if process.returncode != 0:
        raise RuntimeError(f"chat TUI startup case exited with status {process.returncode}")
    return bytes(output)


def check_bare_chat(binary):
    output = run_case(
        binary,
        ["--quiet", "--chat"],
        [
            ("must-not-send\r", 0.5),
            ("\x0c", 0.4),
            ("\x11", 0.3),
        ],
    )
    require(output, "/list browse", "starting chat without a provider")
    require(output, "/provider then /model", "explaining bare chat setup")
    require(output, "sending disabled", "blocking bare chat generation")
    require(output, "No saved chat threads", "using Ctrl+L while chat is offline")
    rendered = plain(output)
    if "── Provider" in rendered or "── Model" in rendered:
        raise RuntimeError("bare chat startup unexpectedly opened provider/model selection")


def check_single_model_chat(binary, base_url, model):
    output = run_case(
        binary,
        [base_url, "--quiet", "--chat"],
        [("\x11", 0.3)],
    )
    require(output, "only model auto-selected", "discovering one startup chat model")
    require(output, model, "auto-selecting the sole startup chat model")
    if "── Model" in plain(output):
        raise RuntimeError("single-model chat startup unnecessarily opened a picker")


def check_multiple_model_chat(binary, base_url, model):
    output = run_case(
        binary,
        [
            base_url,
            "--models-url",
            base_url + "/v1/models-multiple",
            "--quiet",
            "--chat",
        ],
        [
            ("\r", 0.6),
            ("\x11", 0.3),
        ],
    )
    require(output, "── Model", "opening startup chat model selection")
    require(output, model, "showing the first startup chat model")
    require(output, model + "-second", "showing the second startup chat model")
    require(output, "ready", "accepting a startup chat model")


def main():
    if len(sys.argv) != 4:
        print("usage: tui_startup_selection_driver.py BINARY BASE_URL MODEL", file=sys.stderr)
        return 2
    binary, base_url, model = sys.argv[1:]
    check_bare_chat(binary)
    check_single_model_chat(binary, base_url, model)
    check_multiple_model_chat(binary, base_url, model)
    print("TUI startup provider/model selection integration checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
