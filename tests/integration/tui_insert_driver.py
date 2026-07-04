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
        expected_text = target_path[:-4]
        send(master, expected_text + "\t")
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
            raise RuntimeError("editor Tab completed or modified text despite disabled completion")


def main():
    binary, base, model, insert_path, image_path, fetch_url, save_path = sys.argv[1:]
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
        if b"Context used:" not in response_output:
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


if __name__ == "__main__":
    main()
