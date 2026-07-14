#!/usr/bin/env python3
import os
import pty
import select
import subprocess
import sys
import time


def drain(master):
    output = bytearray()
    while select.select([master], [], [], 0)[0]:
        try:
            chunk = os.read(master, 65536)
            if not chunk:
                return bytes(output)
            output.extend(chunk)
        except OSError:
            return bytes(output)
    return bytes(output)


def send(master, text, delay=0.35):
    os.write(master, text.encode("utf-8"))
    time.sleep(delay)
    return drain(master)


def require_running(process, description):
    if process.poll() is not None:
        raise RuntimeError(f"{description} exited after Ctrl+C")


def verify_editor_minibuffer(binary, target_path, save_path):
    if os.path.exists(save_path):
        os.remove(save_path)
    master, slave = pty.openpty()
    process = subprocess.Popen(
        [binary, "--provider", "none", "--editor"],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
    )
    os.close(slave)
    try:
        time.sleep(0.25)
        drain(master)
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
    process = subprocess.Popen(
        [binary, "--provider", "none", "--editor", "--allow-private-url-fetch"],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
    )
    os.close(slave)
    try:
        time.sleep(0.25)
        drain(master)
        send(master, "before\n")
        send(master, "\x1b", 0.15)
        send(master, f"/insert {odd_path[:-6]}\t", 0.2)
        send(master, "\r", 0.7)
        send(master, "\x1b", 0.15)
        send(master, "/insert ", 0.1)
        send(master, f"\x1b[200~{fetch_url}\r\n\x1b[201~", 0.2)
        send(master, "\r", 0.8)
        send(master, "\x1b", 0.15)
        send(master, "/auto-convert-html-to-md no\r", 0.2)
        send(master, "\x1b", 0.15)
        send(master, f"/insert {fetch_url}\r", 0.8)
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
        time.sleep(0.25)
        drain(master)
        send(master, "\x03", 0.2)
        require_running(process, "TUI")
        help_output = send(master, "/help\r")
        if b"/fetch URL" not in help_output:
            raise RuntimeError("TUI help panel did not render slash commands")
        send(master, "/help\r")
        completion_output = send(master, f"/insert {insert_path[:-4]}\t")
        if b"Completed path:" not in completion_output:
            raise RuntimeError("TUI did not report a unique path completion")
        send(master, "\r")
        response_output = send(master, "summarize-insert\r", 0.8)
        if b"context:" not in response_output:
            raise RuntimeError("TUI completion status did not render estimated context usage")
        send(master, f"/attach {image_path}\r")
        send(master, "describe-image\r", 0.8)
        send(master, f"/fetch {fetch_url}\r", 0.5)
        send(master, "summarize-url\r", 0.8)
        send(master, "\x11", 0.2)
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
