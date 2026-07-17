#!/usr/bin/env python3
"""Drive standalone editor save (Ctrl+S) for named files."""

import os
import pty
import select
import subprocess
import sys
import tempfile
import time


def drain(master, timeout=0.1):
    output = bytearray()
    end = time.time() + timeout
    while time.time() < end:
        if select.select([master], [], [], 0.05)[0]:
            try:
                chunk = os.read(master, 65536)
                if not chunk:
                    break
                output.extend(chunk)
            except OSError:
                break
    return bytes(output)


def send(master, data, delay=0.35):
    if isinstance(data, str):
        data = data.encode("utf-8")
    os.write(master, data)
    time.sleep(delay)
    return drain(master, 0.5)


def verify_save(binary, path, text, csi_ctrl_s=False):
    if os.path.exists(path):
        os.remove(path)

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
        time.sleep(0.25)
        drain(master)
        send(master, text)
        if csi_ctrl_s:
            save_output = send(master, "\x1b[19;5u")
        else:
            save_output = send(master, "\x13")
        if b"Saved " not in save_output:
            raise RuntimeError(f"editor Ctrl+S did not report save for {path!r}")
        send(master, "\x11")
        process.wait(timeout=5)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=2)
        os.close(master)

    if process.returncode != 0:
        raise RuntimeError(f"editor exited with status {process.returncode}")
    if not os.path.exists(path):
        raise RuntimeError(f"editor save did not create {path!r}")
    with open(path, "r", encoding="utf-8") as handle:
        saved = handle.read()
    if saved != text:
        raise RuntimeError(f"editor saved {saved!r}, expected {text!r}")


def main():
    binary = sys.argv[1]
    tmpdir = tempfile.mkdtemp(prefix="ainiux-editor-save-")
    new_path = os.path.join(tmpdir, "new.txt")
    existing_path = os.path.join(tmpdir, "existing.txt")
    with open(existing_path, "w", encoding="utf-8") as handle:
        handle.write("original\n")

    verify_save(binary, new_path, "hello new\n", csi_ctrl_s=False)
    verify_save(binary, new_path, "hello csi\n", csi_ctrl_s=True)
    verify_save(binary, existing_path, "changed existing\n", csi_ctrl_s=False)


if __name__ == "__main__":
    main()