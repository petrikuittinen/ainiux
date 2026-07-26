#!/usr/bin/env python3
"""Verify provider/model startup behavior in the Chat and Agent TUIs."""

import os
import pty
import re
import select
import shutil
import sqlite3
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


def run_case(binary, args, actions, isolated_workspace=False):
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
        cwd=home_dir if isolated_workspace else None,
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
        raise RuntimeError(f"TUI startup case exited with status {process.returncode}")
    return bytes(output)


def check_missing_provider(binary, mode, model=None):
    args = ["--quiet", mode]
    if model is not None:
        args.extend(["--model", model])
    output = run_case(
        binary,
        args,
        [("\x11", 0.3)],
        isolated_workspace=mode == "--agent",
    )
    context = f"starting {mode} without a provider"
    if model is not None:
        context += " even though a model name was supplied"
    require(output, "── Provider", context)
    require(output, "openai", "showing providers during required startup setup")


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


def check_multiple_model_surface(binary, base_url, model, mode):
    output = run_case(
        binary,
        [
            base_url,
            "--models-url",
            base_url + "/v1/models-multiple",
            "--quiet",
            mode,
        ],
        [
            ("\r", 0.6),
            ("\x11", 0.3),
        ],
        isolated_workspace=mode == "--agent",
    )
    require(output, "── Model", f"opening startup {mode} model selection")
    require(output, model, f"showing the first startup {mode} model")
    require(output, model + "-second", f"showing the second startup {mode} model")
    require(output, "ready", f"accepting a startup {mode} model")

def check_agent_permission_persistence(binary, base_url):
    workspace = tempfile.mkdtemp(prefix="ainiux-agent-permissions-")
    env = os.environ.copy()
    env["HOME"] = workspace
    env["TERM"] = env.get("TERM", "xterm-256color")

    def run(actions):
        master, slave = pty.openpty()
        process = subprocess.Popen(
            [binary, base_url, "--quiet", "--agent"],
            stdin=slave,
            stdout=slave,
            stderr=slave,
            close_fds=True,
            env=env,
            cwd=workspace,
        )
        os.close(slave)
        output = bytearray()
        try:
            time.sleep(1.0)
            output.extend(drain(master, 1.0))
            for data, delay in actions:
                output.extend(send(master, data, delay))
            process.wait(timeout=10)
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=2)
            os.close(master)
        if process.returncode != 0:
            raise RuntimeError(
                f"agent permission TUI exited with status {process.returncode}"
            )
        return bytes(output)

    try:
        changed = run([("/permissions yolo\r", 0.6), ("/quit\r", 0.4)])
        require(changed, " yolo ", "switching agent permissions")
        database = os.path.join(workspace, ".ainiux-pr", "agent.sqlite")
        with sqlite3.connect(database) as connection:
            settings = connection.execute(
                "SELECT settings_json FROM project WHERE id=1"
            ).fetchone()[0]
        if '"permission_mode":"yolo"' not in settings:
            raise RuntimeError(f"permission mode was not persisted: {settings!r}")
        restored = run([("/quit\r", 0.4)])
        require(restored, " yolo ", "restoring persisted agent permissions")
    finally:
        shutil.rmtree(workspace, ignore_errors=True)


def main():
    if len(sys.argv) != 4:
        print("usage: tui_startup_selection_driver.py BINARY BASE_URL MODEL", file=sys.stderr)
        return 2
    binary, base_url, model = sys.argv[1:]
    binary = os.path.abspath(binary)
    for mode in ("--chat", "--agent"):
        check_missing_provider(binary, mode)
        check_missing_provider(binary, mode, model)
    check_single_model_chat(binary, base_url, model)
    for mode in ("--chat", "--agent"):
        check_multiple_model_surface(binary, base_url, model, mode)
    check_agent_permission_persistence(binary, base_url)
    print("TUI startup provider/model selection integration checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
