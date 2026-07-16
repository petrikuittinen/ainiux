#!/usr/bin/env python3
"""Drive cursor-aware prose continuation against a live OpenAI-compatible endpoint."""

import os
import pty
import select
import subprocess
import sys
import time


def drain(master, timeout=0.0):
    output = bytearray()
    deadline = time.time() + timeout
    while True:
        wait = max(0.0, deadline - time.time()) if timeout > 0 else 0.0
        ready, _, _ = select.select([master], [], [], wait)
        if not ready:
            break
        try:
            chunk = os.read(master, 65536)
            if not chunk:
                break
            output.extend(chunk)
        except OSError:
            break
    return bytes(output)


def send(master, data, delay=0.2):
    if isinstance(data, str):
        data = data.encode("utf-8")
    os.write(master, data)
    time.sleep(delay)
    return drain(master, 0.5)


def visible_text(raw):
    return raw.decode("utf-8", errors="replace")


def main():
    if len(sys.argv) < 4:
        print("usage: editor_prose_continue_driver.py BINARY BASE_URL MODEL", file=sys.stderr)
        return 2

    binary, base_url, model = sys.argv[1:4]
    test_path = os.path.join(
        os.path.dirname(os.path.abspath(binary)), "build", "continue_story_input.txt"
    )
    prefix = "Mara entered the silent observatory. The clock stopped.\n"
    postfix = "\nAt dawn, the brass door was sealed again.\n"
    bridge = "A hidden panel opened, and she slipped inside before the gears resumed.\n"
    with open(test_path, "w", encoding="utf-8") as handle:
        handle.write(prefix + postfix)

    master, slave = pty.openpty()
    process = subprocess.Popen(
        [binary, base_url, "--quiet", "--editor", test_path, "-m", model],
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
        output.extend(send(master, b"\x1b[1;5H"))  # Ctrl+Home
        output.extend(send(master, b"\x1b[C" * len(prefix)))
        output.extend(send(master, b"\x00"))  # Ctrl+Space

        deadline = time.time() + 30.0
        saw_bridge = bridge.strip() in visible_text(output)
        while time.time() < deadline and not saw_bridge:
            if process.poll() is not None:
                raise RuntimeError(f"editor exited early with status {process.returncode}")
            chunk = drain(master, 2.0)
            if chunk:
                output.extend(chunk)
                saw_bridge = bridge.strip() in visible_text(output)

        if not saw_bridge:
            output.extend(send(master, b"\x1b"))
            raise RuntimeError(
                "did not observe streamed prose bridge; terminal tail="
                + repr(visible_text(output[-4000:]))
            )

        time.sleep(0.5)
        output.extend(drain(master, 1.0))
        output.extend(send(master, b"\x13"))  # Ctrl+S
        output.extend(send(master, b"\x11"))  # Ctrl+Q
        process.wait(timeout=10)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=2)
        os.close(master)

    if process.returncode != 0:
        raise RuntimeError(f"editor exited with status {process.returncode}")

    with open(test_path, "rb") as handle:
        final_bytes = handle.read()
    expected = (prefix + bridge + postfix).encode("utf-8")
    if final_bytes != expected:
        raise RuntimeError(
            "prose completion did not insert only the bridge before the byte-identical suffix: "
            + repr(final_bytes)
        )

    print("editor prose-gap continuation integration check passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
