#!/usr/bin/env python3
import os
import pty
import select
import subprocess
import sys
import time


def drain(master):
    while select.select([master], [], [], 0)[0]:
        try:
            if not os.read(master, 65536):
                return
        except OSError:
            return


def send(master, text, delay=0.35):
    os.write(master, text.encode("utf-8"))
    time.sleep(delay)
    drain(master)


def main():
    binary, base, model, insert_path, save_path = sys.argv[1:]
    master, slave = pty.openpty()
    process = subprocess.Popen(
        [binary, base, "--quiet", "--chat", "--no-stream", "-m", model, "--save-chat", save_path],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
    )
    os.close(slave)
    try:
        time.sleep(0.25)
        drain(master)
        send(master, f"/insert {insert_path}\r")
        send(master, "summarize-insert\r", 0.8)
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
