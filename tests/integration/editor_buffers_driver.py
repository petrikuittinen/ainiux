#!/usr/bin/env python3
"""Drive editor multi-buffer switching, shared clipboard, and close prompts."""

import os
import pty
import re
import select
import subprocess
import sys
import tempfile
import time


def color_capable_env():
    """PTY drivers must force a color-capable TERM; make/CI may inherit TERM=dumb."""
    env = os.environ.copy()
    term = env.get("TERM", "")
    if not term or term in ("dumb", "unknown"):
        env["TERM"] = "xterm-256color"
    # Prefer truecolor when advertised; otherwise leave unset so auto picks 256-color.
    if not env.get("COLORTERM"):
        env["COLORTERM"] = "truecolor"
    return env


def open_color_pty(rows=40, cols=120):
    """Open a PTY with a wide enough window that long panel hints are not clipped."""
    import fcntl
    import struct
    import termios

    master, slave = pty.openpty()
    try:
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    except OSError:
        pass
    return master, slave


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


def plain_text(raw):
    return re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text(raw))


def require_seen(raw, needle, context):
    if needle not in text(raw):
        raise RuntimeError(f"expected {needle!r} while {context}; saw {text(raw)[-500:]!r}")


def require_seen_plain(raw, needle, context):
    rendered = plain_text(raw)
    if needle not in rendered:
        raise RuntimeError(f"expected {needle!r} while {context}; saw {rendered[-500:]!r}")


def require_match(raw, pattern, context):
    rendered = text(raw)
    if re.search(pattern, rendered) is None:
        raise RuntimeError(f"expected /{pattern}/ while {context}; saw {rendered[-500:]!r}")


def check_new_file_mode(binary, tmpdir, filename, expected_mode):
    path = os.path.join(tmpdir, filename)
    master, slave = open_color_pty()
    process = subprocess.Popen(
        [binary, "--provider", "none", "--editor", path],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
        env=color_capable_env(),
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


def check_language_reformat(binary, tmpdir):
    path = os.path.join(tmpdir, "reformat.cpp")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("if (ready) {\ncall();\n}\n")
    master, slave = open_color_pty()
    process = subprocess.Popen(
        [binary, "--provider", "none", "--editor", path],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
        env=color_capable_env(),
    )
    os.close(slave)
    output = bytearray()
    try:
        time.sleep(0.4)
        output.extend(drain(master, 1.0))
        output.extend(send(master, "\x1b"))
        output.extend(send(master, "/reformat-all\r", delay=0.4))
        time.sleep(0.4)
        output.extend(drain(master, 1.0))
        require_seen(output, "Reformatted 4 line(s)", "reformatting a C++ buffer")
        output.extend(send(master, "\x13"))
        require_seen(output, f"Saved {path}", "saving the reformatted C++ buffer")
        output.extend(send(master, "\x11"))
        process.wait(timeout=10)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=2)
        os.close(master)
    if process.returncode != 0:
        raise RuntimeError(f"reformat editor exited with status {process.returncode}")
    with open(path, "r", encoding="utf-8") as handle:
        reformatted = handle.read()
    expected = "if (ready) {\n    call();\n}\n"
    if reformatted != expected:
        raise RuntimeError(f"C++ reformat produced {reformatted!r}, expected {expected!r}")


def check_detected_indentation(binary, tmpdir):
    source = os.path.abspath("tests/highlight/javascript_file.js")
    path = os.path.join(tmpdir, "detected-indentation.js")
    with open(source, "r", encoding="utf-8") as source_handle:
        with open(path, "w", encoding="utf-8") as target_handle:
            target_handle.write(source_handle.read())
    master, slave = open_color_pty()
    process = subprocess.Popen(
        [binary, "--provider", "none", "--editor", path],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
        env=color_capable_env(),
    )
    os.close(slave)
    output = bytearray()
    try:
        time.sleep(0.4)
        output.extend(drain(master, 1.0))
        output.extend(send(master, "\x1b", delay=0.5))
        require_seen(output, "Command:", "opening command mode for detected indentation")
        output.extend(send(master, "/tab-width\r", delay=0.5))
        require_seen(output, "Tab width: 2", "reporting detected JavaScript indentation")
        output.extend(send(master, "\x1b", delay=0.5))
        output.extend(send(master, "/tab-width 6\r", delay=0.5))
        require_seen(output, "Tab width: 6", "overriding detected indentation")
        output.extend(send(master, "\x11"))
        process.wait(timeout=10)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=2)
        os.close(master)
    if process.returncode != 0:
        raise RuntimeError(f"indent-detection editor exited with status {process.returncode}")


def check_provider_model_picker(binary, base_url, model):
    master, slave = open_color_pty()
    process = subprocess.Popen(
        [binary, "--provider", "none", "--editor"],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
        env=color_capable_env(),
    )
    os.close(slave)
    output = bytearray()
    try:
        time.sleep(0.4)
        output.extend(drain(master, 1.0))

        picker_output = bytearray()
        picker_output.extend(send(master, "\x1b"))
        picker_output.extend(send(master, "/provider\r"))
        require_seen(picker_output, "Enter select", "opening the editor provider selector")
        require_seen_plain(picker_output, "── Provider", "rendering the shared provider selector panel")
        # Truecolor may use colon form (38:2:R:G:B) or semicolon form (38;2;R;G;B);
        # 256-color uses 38:5:N / 38;5;N. Accept either separator style.
        require_match(
            picker_output,
            r"\x1b\[(?:38[;:]2[;:]\d+[;:]\d+[;:]\d+|38[;:]5[;:]\d+)m"
            r"\x1b\[(?:48[;:]2[;:]\d+[;:]\d+[;:]\d+|48[;:]5[;:]\d+)mProvider",
            "coloring the editor provider selector title",
        )
        output.extend(picker_output)
        output.extend(send(master, "\x1b"))
        require_seen(output, "Provider selection cancelled", "cancelling the provider selector")

        output.extend(send(master, "\x1b"))
        provider_change = send(master, f"/provider {base_url}\r", delay=0.5)
        output.extend(provider_change)
        time.sleep(0.4)
        output.extend(drain(master, 1.0))
        require_seen(output, "auto-selected", "chaining provider selection into model discovery")
        require_seen(output, model, "automatically choosing the only returned model")

        output.extend(send(master, "\x11"))
        process.wait(timeout=10)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=2)
        os.close(master)
    if process.returncode != 0:
        raise RuntimeError(f"provider/model picker editor exited with status {process.returncode}")


def check_bare_editor_startup(binary):
    master, slave = open_color_pty()
    process = subprocess.Popen(
        [binary, "--editor"],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
        env=color_capable_env(),
    )
    os.close(slave)
    output = bytearray()
    try:
        time.sleep(0.5)
        output.extend(drain(master, 1.0))
        require_seen(output, "Local editor", "starting the editor without an AI provider")
        rendered = plain_text(output)
        if "── Provider" in rendered or "── Model" in rendered or "Loading models" in rendered:
            raise RuntimeError("bare editor startup unexpectedly opened provider/model selection")
        output.extend(send(master, "\x11"))
        process.wait(timeout=10)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=2)
        os.close(master)
    if process.returncode != 0:
        raise RuntimeError(f"bare editor startup exited with status {process.returncode}")


def check_single_model_editor_startup(binary, base_url, model):
    master, slave = open_color_pty()
    process = subprocess.Popen(
        [binary, base_url, "--editor"],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
        env=color_capable_env(),
    )
    os.close(slave)
    output = bytearray()
    try:
        time.sleep(0.5)
        output.extend(drain(master, 1.2))
        require_seen(output, "auto-selected", "discovering one startup editor model")
        require_seen(output, model, "auto-selecting the sole startup editor model")
        if "── Model" in plain_text(output):
            raise RuntimeError("single-model editor startup unnecessarily opened a picker")
        output.extend(send(master, "\x11"))
        process.wait(timeout=10)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=2)
        os.close(master)
    if process.returncode != 0:
        raise RuntimeError(f"single-model editor startup exited with status {process.returncode}")


def check_multiple_model_editor_startup(binary, base_url, model):
    master, slave = open_color_pty()
    process = subprocess.Popen(
        [
            binary,
            base_url,
            "--models-url",
            base_url + "/v1/models-multiple",
            "--editor",
        ],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
        env=color_capable_env(),
    )
    os.close(slave)
    output = bytearray()
    try:
        time.sleep(0.5)
        output.extend(drain(master, 1.2))
        require_seen_plain(output, "── Model", "opening startup editor model selection")
        require_seen(output, model, "showing the first startup editor model")
        require_seen(output, model + "-second", "showing the second startup editor model")
        output.extend(send(master, "\r", delay=0.6))
        require_seen(output, "ready", "accepting a startup editor model")
        output.extend(send(master, "\x11"))
        process.wait(timeout=10)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=2)
        os.close(master)
    if process.returncode != 0:
        raise RuntimeError(f"multiple-model editor startup exited with status {process.returncode}")


def main():
    if len(sys.argv) != 4:
        print("usage: editor_buffers_driver.py BINARY BASE_URL MODEL", file=sys.stderr)
        return 2

    binary = sys.argv[1]
    base_url = sys.argv[2]
    model = sys.argv[3]
    tmpdir = tempfile.mkdtemp(prefix="ainiux-editor-buffers-")
    check_bare_editor_startup(binary)
    check_single_model_editor_startup(binary, base_url, model)
    check_multiple_model_editor_startup(binary, base_url, model)
    check_provider_model_picker(binary, base_url, model)
    check_new_file_mode(binary, tmpdir, "new-document.md", "markdown")
    check_new_file_mode(binary, tmpdir, "new-document.html", "html")
    check_new_file_mode(binary, tmpdir, "new-document.xhtml", "htmlonly")
    check_new_file_mode(binary, tmpdir, "new-document.php", "php")
    check_new_file_mode(binary, tmpdir, "new-document.yaml", "yaml")
    check_language_reformat(binary, tmpdir)
    check_detected_indentation(binary, tmpdir)
    file1 = os.path.join(tmpdir, "file1.txt")
    file2 = os.path.join(tmpdir, "file2.txt")
    with open(file1, "w", encoding="utf-8") as handle:
        handle.write("alpha")
    with open(file2, "w", encoding="utf-8") as handle:
        handle.write("beta")

    master, slave = open_color_pty()
    process = subprocess.Popen(
        [binary, "--provider", "none", "--editor", file1],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
        env=color_capable_env(),
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
        require_seen(
            output,
            "Buffers - Enter opens - / search - . sort - Tab/Insert new - DEL close - Esc cancels",
            "listing buffers",
        )
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
        require_seen(
            output,
            f"Close {file2} (modified)? (y/n)",
            "prompting before modified close",
        )
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
        require_seen(
            output,
            "Close [scratch 2] (modified)? (y/n)",
            "prompting before closing scratch",
        )
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
