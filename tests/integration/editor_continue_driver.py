#!/usr/bin/env python3
"""Drive standalone editor AI continue (Ctrl+Space) against a live endpoint."""

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


def visible_text(raw: bytes) -> str:
    return raw.decode("utf-8", errors="replace")


def main():
    if len(sys.argv) < 4:
        print("usage: editor_continue_driver.py BINARY BASE_URL MODEL", file=sys.stderr)
        return 2

    binary, base_url, model = sys.argv[1:4]
    test_path = os.path.join(os.path.dirname(os.path.abspath(binary)), "build", "continue_test_input.py")
    prefix = "def greet(name):\n    "
    postfix = "\n\nprint(greet(\"Ada\"))\n"
    completion = "return f\"Hello, {name}!\"\n"
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

        # Place the cursor in the Python gap, then trigger mode-aware AI completion.
        output.extend(send(master, b"\x1b[1;5H"))  # Ctrl+Home
        output.extend(send(master, b"\x1b[C" * len(prefix)))
        output.extend(send(master, b"\x00"))  # Ctrl+Space

        deadline = time.time() + 30.0
        saw_thinking = False
        saw_completion = "Hello, {name}!" in visible_text(output)
        while time.time() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f"editor exited early with status {process.returncode}")
            chunk = drain(master, 2.0)
            if chunk:
                output.extend(chunk)
                text = visible_text(chunk)
                if "thinking... ESC to abort" in text:
                    saw_thinking = True
                    print("saw thinking status in minibuffer", file=sys.stderr)
                if "writing. Press ESC to stop." in text:
                    print("saw writing status in minibuffer", file=sys.stderr)
                if "Hello, {name}!" in visible_text(output):
                    saw_completion = True
                    print("saw streamed Python completion", file=sys.stderr)
                    break
            elif saw_thinking and not saw_completion:
                continue
            elif saw_completion:
                break

        if not saw_completion:
            print("timeout waiting for code completion; sending Esc to abort", file=sys.stderr)
            output.extend(send(master, b"\x1b"))
            output.extend(drain(master, 5.0))
            raise RuntimeError(
                "did not observe streamed Python completion; terminal tail="
                + repr(visible_text(output[-4000:]))
            )

        # Let the closing fence and [DONE] event finish before saving.
        time.sleep(0.5)
        output.extend(drain(master, 1.0))

        output.extend(send(master, b"\x13"))  # Ctrl+S save
        output.extend(send(master, b"\x11"))  # Ctrl+Q quit
        process.wait(timeout=10)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=2)
        os.close(master)

    if process.returncode != 0:
        raise RuntimeError(f"editor exited with status {process.returncode}")

    with open(test_path, "r", encoding="utf-8") as handle:
        final_text = handle.read()

    expected = prefix + completion + postfix
    print("prefix:", repr(prefix))
    print("postfix:", repr(postfix))
    print("final:", repr(final_text))
    print("growth:", len(final_text) - len(prefix + postfix), "bytes")
    print("thinking_status_seen:", saw_thinking)
    print("completion_seen:", saw_completion)

    if final_text != expected:
        raise RuntimeError("code completion did not insert only the generated code between the original halves")

    print("editor code-gap completion integration check passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
