#!/usr/bin/env python3
"""Exercise editor locking, read-only retry, reload, and final save through PTYs."""

import os
import pty
import select
import subprocess
import sys
import tempfile
import time


def drain(master, timeout=0.3):
    output = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not select.select([master], [], [], min(0.05, deadline - time.time()))[0]:
            continue
        try:
            chunk = os.read(master, 65536)
        except OSError:
            break
        if not chunk:
            break
        output.extend(chunk)
    return bytes(output)


def send(master, data, delay=0.25):
    if isinstance(data, str):
        data = data.encode("utf-8")
    os.write(master, data)
    time.sleep(delay)
    return drain(master, 0.6)


def spawn(binary, path):
    master, slave = pty.openpty()
    process = subprocess.Popen(
        [binary, "--provider", "none", "--editor", path],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
    )
    os.close(slave)
    time.sleep(0.5)
    return process, master, bytearray(drain(master, 1.0))


def require(output, needle, context):
    rendered = output.decode("utf-8", errors="replace")
    if needle not in rendered:
        raise RuntimeError(f"expected {needle!r} while {context}; saw {rendered[-800:]!r}")


def stop(process, master):
    if process.poll() is None:
        process.terminate()
        process.wait(timeout=3)
    os.close(master)


def main():
    if len(sys.argv) != 2:
        print("usage: editor_locking_driver.py BINARY", file=sys.stderr)
        return 2
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="pkchat-editor-lock-") as directory:
        path = os.path.join(directory, "shared.txt")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("original")

        first, first_master, first_output = spawn(binary, path)
        second = second_master = None
        try:
            deadline = time.time() + 5
            while not os.path.isdir(os.path.realpath(path) + ".LOCK") and time.time() < deadline:
                time.sleep(0.05)
            if not os.path.isdir(os.path.realpath(path) + ".LOCK"):
                raise RuntimeError("first editor did not acquire FILE.LOCK")

            second, second_master, second_output = spawn(binary, path)
            require(second_output, "[RO]", "opening an actively locked file read-only")
            second_output.extend(send(second_master, "X"))
            require(second_output, "editor file is locked", "blocking a read-only edit")

            first_output.extend(send(first_master, "\x1b[4;5~"))
            first_output.extend(send(first_master, " one"))
            first_output.extend(send(first_master, "\x13"))
            require(first_output, f"Saved {path}", "saving in the lock-owning editor")
            first_output.extend(send(first_master, "\x11"))
            first.wait(timeout=10)
            if first.returncode != 0:
                raise RuntimeError(f"first editor exited with {first.returncode}")

            second_output.extend(send(second_master, "Y"))
            require(second_output, "changed while locked by another", "detecting change on retry")
            second_output.extend(send(second_master, "y"))
            require(second_output, "Repeat the edit", "accepting reload after lock acquisition")
            second_output.extend(send(second_master, "\x1b[4;5~"))
            second_output.extend(send(second_master, " two"))
            second_output.extend(send(second_master, "\x13"))
            require(second_output, f"Saved {path}", "saving from the upgraded editor")
            second_output.extend(send(second_master, "\x11"))
            second.wait(timeout=10)
            if second.returncode != 0:
                raise RuntimeError(f"second editor exited with {second.returncode}")

            with open(path, "r", encoding="utf-8") as handle:
                final = handle.read()
            if final != "original one two":
                raise RuntimeError(f"final locked-editor content was {final!r}")
        finally:
            if first.poll() is None:
                stop(first, first_master)
            else:
                os.close(first_master)
            if second is not None:
                if second.poll() is None:
                    stop(second, second_master)
                else:
                    os.close(second_master)
    print("editor locking integration test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
