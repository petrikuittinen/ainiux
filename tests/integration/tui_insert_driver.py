#!/usr/bin/env python3
import fcntl
import os
import pty
import select
import struct
import subprocess
import sys
import termios
import time


def set_winsize(fd, rows=40, cols=120):
    # Integration PTYs default to 0x0; chrome needs a real layout to paint help.
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def drain(master, timeout=0.0):
    output = bytearray()
    deadline = time.time() + timeout
    while True:
        wait = max(0.0, deadline - time.time()) if timeout > 0 else 0.0
        ready, _, _ = select.select([master], [], [], wait)
        if not ready:
            if timeout <= 0 or time.time() >= deadline:
                break
            continue
        try:
            chunk = os.read(master, 65536)
            if not chunk:
                break
            output.extend(chunk)
        except OSError:
            break
        if timeout <= 0:
            # Non-blocking mode: keep reading while ready.
            continue
    # Always empty whatever is already queued.
    while select.select([master], [], [], 0)[0]:
        try:
            chunk = os.read(master, 65536)
            if not chunk:
                break
            output.extend(chunk)
        except OSError:
            break
    return bytes(output)


def send(master, text, delay=0.35):
    os.write(master, text.encode("utf-8"))
    time.sleep(delay)
    return drain(master, 0.4)


def wait_for_terminal(master, timeout=5.0):
    deadline = time.monotonic() + timeout
    output = bytearray()
    while time.monotonic() < deadline:
        ready, _, _ = select.select([master], [], [], 0.05)
        if not ready:
            continue
        try:
            chunk = os.read(master, 65536)
        except OSError as error:
            raise RuntimeError("terminal exited before initialization") from error
        if not chunk:
            raise RuntimeError("terminal exited before initialization")
        output.extend(chunk)
        if b"\x1b[2J" in output:
            return
    raise RuntimeError("terminal did not initialize before the test timeout")


def require_running(process, description):
    if process.poll() is not None:
        raise RuntimeError(f"{description} exited after Ctrl+C")


def verify_editor_minibuffer(binary, target_path, save_path):
    if os.path.exists(save_path):
        os.remove(save_path)
    master, slave = pty.openpty()
    set_winsize(master)
    process = subprocess.Popen(
        [binary, "--provider", "none", "--editor"],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
    )
    os.close(slave)
    try:
        wait_for_terminal(master)
        send(master, "\x03", 0.2)
        require_running(process, "editor")
        typed_text = target_path[:-4]
        expected_text = typed_text + " " * (4 - (len(typed_text) % 4))
        send(master, typed_text + "\t")
        require_running(process, "editor")
        quit_prompt = send(master, "\x11", 0.2)
        if b"save before quit? (y/n)" not in quit_prompt:
            raise RuntimeError("editor Ctrl+Q did not ask whether to save modified scratch buffer")
        send(master, "\x1b", 0.2)
        save_prompt = send(master, "\x13")
        if b"Save file:" not in save_prompt:
            raise RuntimeError("editor Ctrl+S did not open the minibuffer save prompt")
        save_result = send(master, save_path + "\r", 0.2)
        if b"Press y to overwrite" in save_result:
            send(master, "y", 0.2)
        quit_result = send(master, "\x11", 0.2)
        if b"save before quit? (y/n)" in quit_result:
            send(master, "n", 0.2)
        process.wait(timeout=5)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=2)
        os.close(master)
    if process.returncode != 0:
        raise RuntimeError(f"editor exited with status {process.returncode}")
    with open(save_path, "r", encoding="utf-8") as saved:
        if saved.read() != expected_text:
            raise RuntimeError("editor Tab did not fall back to the configured indentation")


def verify_editor_insert(binary, target_path, fetch_url, save_path):
    odd_path = target_path + ".arbitrary-ending"
    inserted_file_text = "local α\nlocal 你好\n"
    with open(odd_path, "wb") as inserted:
        inserted.write("local α\r\nlocal 你好\r".encode("utf-8"))
    if os.path.exists(save_path):
        os.remove(save_path)
    master, slave = pty.openpty()
    set_winsize(master)
    process = subprocess.Popen(
        [binary, "--provider", "none", "--editor", "--allow-private-url-fetch"],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
    )
    os.close(slave)
    try:
        wait_for_terminal(master)
        send(master, "before\n")
        send(master, "\x1b", 0.4)
        send(master, f"/insert {odd_path[:-6]}\t", 0.5)
        send(master, "\r", 0.7)
        send(master, "\x1b", 0.4)
        send(master, "/insert ", 0.1)
        send(master, f"\x1b[200~{fetch_url}\r\n\x1b[201~", 0.2)
        send(master, "\r", 0.8)
        send(master, "\x1b", 0.4)
        send(master, "/auto-convert-html-to-md off\r", 0.3)
        send(master, "\x1b", 0.4)
        send(master, f"/insert {fetch_url}\r", 1.0)
        send(master, "after\n")
        save_prompt = send(master, "\x13")
        if b"Save file:" not in save_prompt:
            raise RuntimeError("editor Ctrl+S did not open save prompt after /insert")
        save_result = send(master, save_path + "\r", 0.3)
        if b"Press y to overwrite" in save_result:
            send(master, "y", 0.2)
        send(master, "\x11", 0.2)
        process.wait(timeout=5)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=2)
        os.close(master)
    if process.returncode != 0:
        raise RuntimeError(f"editor /insert test exited with status {process.returncode}")
    with open(save_path, "r", encoding="utf-8") as saved:
        content = saved.read()
    if not content.startswith("before\n" + inserted_file_text):
        raise RuntimeError("editor /insert did not insert arbitrary-extension UTF-8 text at the cursor")
    if "# Mock Page" not in content:
        raise RuntimeError("editor /insert URL did not auto-convert HTML to Markdown")
    if "<!doctype html>" not in content:
        raise RuntimeError("editor /insert URL did not preserve raw HTML after conversion was disabled")
    if not content.endswith("after\n"):
        raise RuntimeError("editor typing did not continue after inserted content")


def main():
    binary, base, model, insert_path, image_path, fetch_url, save_path = sys.argv[1:]
    odd_path = insert_path + ".arbitrary-ending"
    if os.path.exists(odd_path):
        os.remove(odd_path)
    master, slave = pty.openpty()
    set_winsize(master)
    process = subprocess.Popen(
        [binary, base, "--quiet", "--chat", "--no-stream", "-m", model,
         "--context", "64k", "--image-capability", "allow", "--allow-private-url-fetch",
         "--save-chat", save_path],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
    )
    os.close(slave)
    try:
        wait_for_terminal(master)
        # Chat opens the thread list first; Tab starts a new thread before input works.
        send(master, "\t", 0.6)
        require_running(process, "TUI")
        help_output = send(master, "/help\r", 0.8)
        # Prefer an early command so short terminals still observe the panel; also
        # accept /fetch URL when the taller PTY paints the full help list.
        if b"/provider" not in help_output and b"/fetch URL" not in help_output:
            raise RuntimeError("TUI help panel did not render slash commands")
        send(master, "/help\r", 0.4)
        completion_output = send(master, f"/insert {insert_path[:-4]}\t", 0.5)
        if b"Completed path:" not in completion_output:
            raise RuntimeError("TUI did not report a unique path completion")
        send(master, "\r", 0.4)
        response_output = send(master, "summarize-insert\r", 1.0)
        # Context usage lives on the input label as "N tok (x.x%)", not on the
        # status row (which shows TTFT/token-s after generation).
        if b" tok" not in response_output and b"tok (" not in response_output:
            raise RuntimeError("TUI input label did not render estimated context usage")
        send(master, f"/attach {image_path}\r", 0.5)
        send(master, "describe-image\r", 1.0)
        send(master, f"/fetch {fetch_url}\r", 0.6)
        send(master, "summarize-url\r", 1.0)
        send(master, "\x11", 0.3)
        process.wait(timeout=5)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=2)
        os.close(master)
    if process.returncode != 0:
        raise SystemExit(f"TUI exited with status {process.returncode}")
    verify_editor_minibuffer(binary, insert_path, save_path + ".editor-minibuffer")
    verify_editor_insert(binary, insert_path, fetch_url, save_path + ".editor-insert")


if __name__ == "__main__":
    main()
