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


def main():
    binary, base, model, insert_path, image_path, fetch_url, save_path = sys.argv[1:]
    master, slave = pty.openpty()
    process = subprocess.Popen(
        [binary, base, "--quiet", "--chat", "--no-stream", "-m", model,
         "--image-capability", "allow", "--allow-private-url-fetch", "--save-chat", save_path],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
    )
    os.close(slave)
    try:
        time.sleep(0.25)
        drain(master)
        help_output = send(master, "/help\r")
        if b"/fetch URL" not in help_output:
            raise RuntimeError("TUI help panel did not render slash commands")
        send(master, "/help\r")
        send(master, f"/insert {insert_path}\r")
        send(master, "summarize-insert\r", 0.8)
        send(master, f"/attach {image_path}\r")
        send(master, "describe-image\r", 0.8)
        send(master, f"/fetch {fetch_url}\r", 0.5)
        send(master, "summarize-url\r", 0.8)
        send(master, "/quit\r", 0.2)
        process.wait(timeout=5)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=2)
        os.close(master)
    if process.returncode != 0:
        raise SystemExit(f"TUI exited with status {process.returncode}")


if __name__ == "__main__":
    main()
