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
    # Chat opens the thread selector first (Ctrl+L UI); Tab starts a new thread
    # and then the provider picker. Agent still opens the provider picker first.
    actions = [("\x11", 0.3)]
    if mode == "--chat":
        actions = [("\t", 0.5), ("\x11", 0.3)]
    output = run_case(
        binary,
        args,
        actions,
        isolated_workspace=mode == "--agent",
    )
    context = f"starting {mode} without a provider"
    if model is not None:
        context += " even though a model name was supplied"
    if mode == "--chat":
        require(output, "Newest first", "showing chat startup thread selector")
    require(output, "── Provider", context)
    require(output, "openai", "showing providers during required startup setup")


def check_single_model_chat(binary, base_url, model):
    output = run_case(
        binary,
        [base_url, "--quiet", "--chat"],
        # Thread selector first, then new thread so model discovery can run.
        [("\t", 0.5), ("\x11", 0.3)],
    )
    require(output, "Newest first", "showing chat startup thread selector")
    require(output, "only model auto-selected", "discovering one startup chat model")
    require(output, model, "auto-selecting the sole startup chat model")
    if "── Model" in plain(output):
        raise RuntimeError("single-model chat startup unnecessarily opened a picker")


def check_multiple_model_surface(binary, base_url, model, mode):
    actions = [
        ("\r", 0.6),
        ("\x11", 0.3),
    ]
    if mode == "--chat":
        actions = [
            ("\t", 0.5),
            ("\r", 0.6),
            ("\x11", 0.3),
        ]
    output = run_case(
        binary,
        [
            base_url,
            "--models-url",
            base_url + "/v1/models-multiple",
            "--quiet",
            mode,
        ],
        actions,
        isolated_workspace=mode == "--agent",
    )
    if mode == "--chat":
        require(output, "Newest first", "showing chat startup thread selector")
    require(output, "── Model", f"opening startup {mode} model selection")
    require(output, model, f"showing the first startup {mode} model")
    require(output, model + "-second", f"showing the second startup {mode} model")
    require(output, "ready", f"accepting a startup {mode} model")

def open_wide_pty(rows=40, cols=120):
    import fcntl
    import struct
    import termios

    master, slave = pty.openpty()
    try:
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    except OSError:
        pass
    return master, slave


def check_agent_permission_persistence(binary, base_url):
    workspace = tempfile.mkdtemp(prefix="ainiux-agent-permissions-")
    env = os.environ.copy()
    env["HOME"] = workspace
    # make/CI often inherit TERM=dumb; force a color-capable wide terminal.
    if not env.get("TERM") or env.get("TERM") in ("dumb", "unknown"):
        env["TERM"] = "xterm-256color"
    if not env.get("COLORTERM"):
        env["COLORTERM"] = "truecolor"

    def launch_and_idle():
        """Start agent TUI, decline optional index offer, wait until idle ready."""
        master, slave = open_wide_pty()
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
            deadline = time.time() + 25.0
            saw_index_offer = False
            while time.time() < deadline:
                output.extend(drain(master, 0.4))
                rendered = plain(output)
                if (
                    "Build code index" in rendered
                    or "index-build" in rendered.lower()
                    or "Build code" in rendered
                ):
                    saw_index_offer = True
                    break
                if "Agent ready" in rendered:
                    break
            if saw_index_offer:
                # Only send n when the Yes/No index offer is actually showing.
                output.extend(send(master, "n", 0.8))
            # Wait until prepare finishes and no turn is running.
            deadline = time.time() + 20.0
            while time.time() < deadline:
                output.extend(drain(master, 0.4))
                rendered = plain(output)
                if "Agent ready" in rendered and "Task complete" not in rendered[-200:]:
                    # Prefer a stable idle ready after any accidental turn ends.
                    if "thinking" not in rendered.lower()[-300:]:
                        break
                if "Task complete" in rendered or "Task completed" in rendered:
                    # Turn finished; continue until chrome settles.
                    time.sleep(0.3)
            time.sleep(0.3)
            output.extend(drain(master, 0.5))
            return process, master, output
        except Exception:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=2)
            os.close(master)
            raise

    def finish(process, master, output, actions):
        try:
            for data, delay in actions:
                output.extend(send(master, data, delay))
            process.wait(timeout=15)
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
        process, master, output = launch_and_idle()
        changed = finish(
            process,
            master,
            output,
            [
                ("/permissions yolo\r", 1.5),
                ("/quit\r", 0.8),
            ],
        )
        # Status text is transient under retained row-diff painting; accept either
        # the status phrase or the mode chrome badge, then verify SQLite.
        rendered = plain(changed)
        status_ok = (
            "Permissions set to yolo" in rendered
            or "Permissions already yolo" in rendered
            or re.search(r"\byolo\b", rendered) is not None
        )
        if not status_ok:
            raise RuntimeError(
                "expected yolo permissions status or chrome while switching agent "
                f"permissions; saw {rendered[-700:]!r}"
            )
        database = os.path.join(workspace, ".ainiux-pr", "agent.sqlite")
        with sqlite3.connect(database) as connection:
            settings = connection.execute(
                "SELECT settings_json FROM project WHERE id=1"
            ).fetchone()[0]
        if '"permission_mode":"yolo"' not in settings:
            raise RuntimeError(f"permission mode was not persisted: {settings!r}")
        # Reopen: decline index if offered; chrome should already show yolo.
        process, master, output = launch_and_idle()
        restored = finish(process, master, output, [("/quit\r", 0.8)])
        require(restored, "yolo", "restoring persisted agent permissions")
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
