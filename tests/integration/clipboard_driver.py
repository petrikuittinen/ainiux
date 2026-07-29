#!/usr/bin/env python3
"""Focused PTY coverage for native-helper, OSC 52, and bracketed clipboard paths."""

import base64
import fcntl
import os
import pathlib
import pty
import select
import struct
import sys
import tempfile
import termios
import time


def drain(fd, seconds=0.25):
    data = bytearray()
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.05)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        data.extend(chunk)
    return bytes(data)


def send(fd, data, settle=0.2):
    os.write(fd, data)
    return drain(fd, settle)


def spawn(binary, arguments, environment, cwd):
    pid, master = pty.fork()
    if pid == 0:
        os.chdir(cwd)
        os.execve(binary, [binary] + arguments, environment)
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
    output = drain(master, 0.6)
    return pid, master, bytearray(output)


def finish(pid, master, output):
    output.extend(send(master, b"\x11", 0.3))  # Ctrl+Q
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        waited, status = os.waitpid(pid, os.WNOHANG)
        if waited == pid:
            os.close(master)
            if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
                raise AssertionError(f"interactive process exited with status {status}")
            return bytes(output)
        output.extend(drain(master, 0.1))
    os.kill(pid, 9)
    os.waitpid(pid, 0)
    os.close(master)
    raise AssertionError("interactive process did not exit")


def write_helper(path, body):
    path.write_text("#!/bin/sh\n" + body + "\n", encoding="utf-8")
    path.chmod(0o700)


def editor_case(binary, root, environment, initial, keys):
    path = root / "editor.txt"
    path.write_text(initial, encoding="utf-8")
    pid, master, output = spawn(
        binary, ["--provider", "none", "--editor", str(path)], environment, str(root)
    )
    for data, delay in keys:
        output.extend(send(master, data, delay))
    output.extend(send(master, b"\x13", 0.35))  # Ctrl+S
    finish(pid, master, output)
    return path.read_text(encoding="utf-8"), bytes(output)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: clipboard_driver.py /absolute/path/to/ainiux")
    binary = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="ainiux-clipboard-pty-") as temp:
        root = pathlib.Path(temp)
        helpers = root / "helpers"
        helpers.mkdir()
        clipboard = root / "clipboard"
        copied = root / "copied"
        clipboard.write_text("external\nΩ", encoding="utf-8")
        write_helper(helpers / "wl-paste", 'exec /bin/cat "$AINIUX_TEST_CLIPBOARD"')
        write_helper(helpers / "wl-copy", 'exec /bin/cat > "$AINIUX_TEST_COPIED"')
        environment = os.environ.copy()
        environment.update(
            {
                "HOME": str(root / "home"),
                "XDG_CONFIG_HOME": str(root / "config"),
                "WAYLAND_DISPLAY": "wayland-test",
                "PATH": str(helpers) + ":" + environment.get("PATH", ""),
                "AINIUX_TEST_CLIPBOARD": str(clipboard),
                "AINIUX_TEST_COPIED": str(copied),
            }
        )
        for name in ("SSH_CONNECTION", "SSH_CLIENT", "SSH_TTY"):
            environment.pop(name, None)
        pathlib.Path(environment["HOME"]).mkdir()

        text, editor_output = editor_case(
            binary, root, environment, "AB", [(b"\x16", 0.5)]
        )
        if text != "external\nΩAB":
            raise AssertionError(
                f"external editor paste mismatch: {text!r}\n"
                + editor_output[-20000:].decode("utf-8", "replace")
            )

        text, _ = editor_case(
            binary,
            root,
            environment,
            "seed",
            [(b"\x01", 0.1), (b"\x03", 0.35), (b"\x1b[C", 0.1), (b"\x16", 0.3)],
        )
        if copied.read_text(encoding="utf-8") != "seed":
            raise AssertionError("Ctrl+C was not visible through wl-copy")
        if text != "seedseed":
            raise AssertionError("internal clipboard did not take precedence")

        text, _ = editor_case(
            binary,
            root,
            environment,
            "",
            [(b"\x1b[200~bracketed\npaste\x1b[201~", 0.25)],
        )
        if text != "bracketed\npaste":
            raise AssertionError("bracketed terminal paste regressed")

        osc_environment = environment.copy()
        osc_environment.pop("WAYLAND_DISPLAY", None)
        osc_environment["PATH"] = "/no/clipboard/helpers"
        path = root / "osc.txt"
        path.write_text("", encoding="utf-8")
        pid, master, output = spawn(
            binary,
            ["--provider", "none", "--editor", str(path)],
            osc_environment,
            str(root),
        )
        output.extend(send(master, b"\x16", 0.15))
        encoded = base64.b64encode("osc\npaste".encode()).decode()
        output.extend(send(master, f"\x1b]52;c;{encoded}\x07".encode(), 0.3))
        output.extend(send(master, b"\x13", 0.3))
        finish(pid, master, output)
        if path.read_text(encoding="utf-8") != "osc\npaste":
            raise AssertionError("OSC 52 paste fallback failed")

        for mode in ("--chat", "--agent"):
            pid, master, output = spawn(
                binary,
                ["http://127.0.0.1:9/v1", "-m", "clipboard-test", mode],
                environment,
                str(root),
            )
            if mode == "--agent":
                # Decline the first-run index-build offer so paste reaches Chat input.
                time.sleep(0.8)
                output.extend(send(master, b"n", 0.3))
            output.extend(send(master, b"\x16", 0.6))
            rendered = finish(pid, master, output)
            if b"external" not in rendered:
                raise AssertionError(f"{mode} did not use shared external clipboard input")

        write_helper(helpers / "wl-paste", "exit 7")
        pid, master, output = spawn(
            binary,
            ["http://127.0.0.1:9/v1", "-m", "clipboard-test", "--chat"],
            environment,
            str(root),
        )
        output.extend(send(master, b"\x16", 0.6))
        rendered = finish(pid, master, output)
        if b"clipboard helper exited unsuccessfully" not in rendered:
            raise AssertionError("clipboard helper failure was not actionable")

    print("clipboard PTY tests passed")


if __name__ == "__main__":
    main()
