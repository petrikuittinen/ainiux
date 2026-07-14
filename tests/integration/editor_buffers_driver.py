#!/usr/bin/env python3
"""Drive editor multi-buffer switching, shared clipboard, and close prompts."""

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


def text(raw):
    return raw.decode("utf-8", errors="replace")


def require_seen(raw, needle, context):
    if needle not in text(raw):
        raise RuntimeError(f"expected {needle!r} while {context}; saw {text(raw)[-500:]!r}")


def check_new_file_mode(binary, tmpdir, filename, expected_mode):
    path = os.path.join(tmpdir, filename)
    master, slave = pty.openpty()
    process = subprocess.Popen(
        [binary, "--provider", "none", "--editor", path],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
    )
    os.close(slave)
    output = bytearray()
    try:
        time.sleep(0.4)
        output.extend(drain(master, 1.0))
        output.extend(send(master, "\x1b"))
        output.extend(send(master, "/mode\r"))
        require_seen(
            output,
            f"Mode: {expected_mode} (automatic)",
            f"opening new {expected_mode} file",
        )
        output.extend(send(master, "\x11"))
        process.wait(timeout=10)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=2)
        os.close(master)
    if process.returncode != 0:
        raise RuntimeError(f"new {expected_mode} editor exited with status {process.returncode}")


def main():
    if len(sys.argv) != 2:
        print("usage: editor_buffers_driver.py BINARY", file=sys.stderr)
        return 2

    binary = sys.argv[1]
    tmpdir = tempfile.mkdtemp(prefix="pkchat-editor-buffers-")
    check_new_file_mode(binary, tmpdir, "new-document.md", "markdown")
    check_new_file_mode(binary, tmpdir, "new-document.html", "html")
    check_new_file_mode(binary, tmpdir, "new-document.xhtml", "htmlonly")
    check_new_file_mode(binary, tmpdir, "new-document.php", "php")
    check_new_file_mode(binary, tmpdir, "new-document.yaml", "yaml")
    file1 = os.path.join(tmpdir, "file1.txt")
    file2 = os.path.join(tmpdir, "file2.txt")
    with open(file1, "w", encoding="utf-8") as handle:
        handle.write("alpha")
    with open(file2, "w", encoding="utf-8") as handle:
        handle.write("beta")

    master, slave = pty.openpty()
    process = subprocess.Popen(
        [binary, "--provider", "none", "--editor", file1],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
    )
    os.close(slave)
    output = bytearray()
    try:
        time.sleep(0.4)
        output.extend(drain(master, 1.0))

        output.extend(send(master, "\x1b"))  # command mode
        output.extend(send(master, f"/open {file2}\r"))
        require_seen(output, f"Opened {file2}", "opening second buffer")

        output.extend(send(master, "\x0c"))  # Ctrl+L buffer list
        require_seen(output, "Buffers - Enter opens - N new - Esc cancels", "listing buffers")
        require_seen(output, "file1.txt", "listing first buffer")
        require_seen(output, "file2.txt", "listing second buffer")
        output.extend(send(master, "\x1b[A"))  # Up to file1
        output.extend(send(master, "\r"))  # choose file1
        require_seen(output, "file1.txt", "switching to first buffer")

        output.extend(send(master, "\x01"))  # Ctrl+A select all
        output.extend(send(master, "\x03"))  # Ctrl+C copy
        require_seen(output, "Copied selection", "copying from first buffer")

        output.extend(send(master, "\x0c"))  # Ctrl+L buffer list
        output.extend(send(master, "\x1b[B"))  # Down to file2
        output.extend(send(master, "\r"))
        require_seen(output, "file2.txt", "switching to second buffer")

        output.extend(send(master, "\x1b[4;5~"))  # Ctrl+End
        output.extend(send(master, "\nal"))
        output.extend(send(master, "\t"))
        require_seen(output, "Completed: alpha", "completing a word from the first buffer")
        output.extend(send(master, "\x1a"))  # Ctrl+Z: whole completion session
        output.extend(send(master, "\x7f\x7f\x7f"))  # remove al and newline

        output.extend(send(master, "\x1b[4;5~"))  # Ctrl+End
        output.extend(send(master, "\x16"))  # Ctrl+V paste shared clipboard
        require_seen(output, "Pasted", "pasting into second buffer")
        output.extend(send(master, "\x13"))  # Ctrl+S save
        require_seen(output, f"Saved {file2}", "saving second buffer")

        output.extend(send(master, "!"))
        output.extend(send(master, "\x17"))  # Ctrl+W close prompt
        require_seen(output, "Buffer modified; close anyway?", "prompting before modified close")
        output.extend(send(master, "n"))
        require_seen(output, "Close cancelled", "cancelling modified close")
        output.extend(send(master, "\x17"))  # Ctrl+W close prompt again
        output.extend(send(master, "y"))
        require_seen(output, "Closed buffer", "closing modified buffer after confirmation")

        output.extend(send(master, "\x0e"))  # Ctrl+N new empty buffer
        require_seen(output, "New buffer", "creating a new empty buffer")
        output.extend(send(master, "scratch text"))
        output.extend(send(master, "\x0c"))  # Ctrl+L buffer list
        require_seen(output, "[scratch", "listing new scratch buffer")
        output.extend(send(master, "\r"))
        output.extend(send(master, "\x17"))  # Ctrl+W close modified scratch
        require_seen(output, "Buffer modified; close anyway?", "prompting before closing scratch")
        output.extend(send(master, "y"))
        require_seen(output, "Closed buffer", "closing scratch buffer")

        output.extend(send(master, "\x11"))  # Ctrl+Q quit
        process.wait(timeout=10)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=2)
        os.close(master)

    if process.returncode != 0:
        raise RuntimeError(f"editor exited with status {process.returncode}")

    with open(file1, "r", encoding="utf-8") as handle:
        saved1 = handle.read()
    with open(file2, "r", encoding="utf-8") as handle:
        saved2 = handle.read()
    if saved1 != "alpha":
        raise RuntimeError(f"first buffer changed unexpectedly: {saved1!r}")
    if saved2 != "betaalpha":
        raise RuntimeError(f"second buffer save did not include pasted text: {saved2!r}")
    print("editor buffer integration check passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
