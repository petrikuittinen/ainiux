#!/usr/bin/env python3
"""Drive standalone editor line-ending commands and block indentation."""

import os
import pty
import select
import subprocess
import sys
import tempfile
import time


def drain(master, timeout=0.1):
    output = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        wait = max(0.0, deadline - time.time())
        if not select.select([master], [], [], min(wait, 0.05))[0]:
            continue
        try:
            chunk = os.read(master, 65536)
            if not chunk:
                break
            output.extend(chunk)
        except OSError:
            break
    return bytes(output)


def send(master, data, delay=0.25):
    if isinstance(data, str):
        data = data.encode("utf-8")
    os.write(master, data)
    time.sleep(delay)
    return drain(master, 0.6)


def require_seen(raw, needle, context):
    decoded = raw.decode("utf-8", errors="replace")
    if needle not in decoded:
        raise RuntimeError(f"expected {needle!r} while {context}; saw {decoded[-500:]!r}")


def command(master, text):
    output = bytearray(send(master, "\x1b"))
    output.extend(send(master, text + "\r"))
    return bytes(output)


def main():
    if len(sys.argv) != 2:
        print("usage: editor_text_modes_driver.py BINARY", file=sys.stderr)
        return 2

    binary = sys.argv[1]
    tmpdir = tempfile.mkdtemp(prefix="pkchat-editor-text-modes-")
    path = os.path.join(tmpdir, "crlf.txt")
    second_path = os.path.join(tmpdir, "lf.txt")
    with open(path, "wb") as handle:
        handle.write(b"alpha\r\nbeta")
    with open(second_path, "wb") as handle:
        handle.write(b"second\nfile\n")

    master, slave = pty.openpty()
    process = subprocess.Popen(
        [binary, "--provider", "none", "--editor", path],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
    )
    os.close(slave)
    try:
        time.sleep(0.4)
        drain(master, 1.0)

        require_seen(command(master, "/linebreak"), "Line break: crlf", "reporting detected CRLF")
        require_seen(command(master, "/tab-width 2"), "Tab width: 2", "setting tab width")
        require_seen(command(master, "/tab-style spaces"), "Tab style: spaces", "setting tab style")

        send(master, "\x01")  # Ctrl+A
        send(master, "\t")
        require_seen(send(master, "\x13"), f"Saved {path}", "saving an indented selection")
        with open(path, "rb") as handle:
            saved = handle.read()
        if saved != b"  alpha\r\n  beta":
            raise RuntimeError(f"block indentation or CRLF preservation failed: {saved!r}")

        send(master, "\x1b[Z")  # common xterm Shift+Tab
        require_seen(send(master, "\x13"), f"Saved {path}", "saving an outdented selection")
        with open(path, "rb") as handle:
            saved = handle.read()
        if saved != b"alpha\r\nbeta":
            raise RuntimeError(f"block outdent or final-ending preservation failed: {saved!r}")

        require_seen(command(master, "/linebreak cr"), "Line break: cr", "changing linebreak mode")
        require_seen(send(master, "\x13"), f"Saved {path}", "saving with CR line endings")
        with open(path, "rb") as handle:
            saved = handle.read()
        if saved != b"alpha\rbeta":
            raise RuntimeError(f"linebreak command did not produce CR output: {saved!r}")

        require_seen(command(master, f"/open {second_path}"), f"Opened {second_path}",
                     "opening a second buffer")
        require_seen(command(master, "/linebreak"), "Line break: lf",
                     "detecting LF independently in the second buffer")
        require_seen(command(master, "/tab-width"), "Tab width: 4",
                     "using the configured tab width in the second buffer")
        send(master, "\x0c")  # Ctrl+L
        send(master, "\x1b[A")
        require_seen(send(master, "\r"), path, "switching back to the first buffer")
        require_seen(command(master, "/linebreak"), "Line break: cr",
                     "restoring the first buffer linebreak setting")
        require_seen(command(master, "/tab-width"), "Tab width: 2",
                     "restoring the first buffer tab width")

        send(master, "\x11")  # Ctrl+Q
        process.wait(timeout=10)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=2)
        os.close(master)

    if process.returncode != 0:
        raise RuntimeError(f"editor exited with status {process.returncode}")
    print("editor text mode integration check passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
