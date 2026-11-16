#!/usr/bin/env python3
"""Offline CLI and WebUI-agent skills checks against a local mock provider."""

import http.server
import json
import os
from pathlib import Path
import secrets
import socket
import sqlite3
import signal
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.request


ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / ("ainiux.exe" if os.name == "nt" else "ainiux")


def main():
    requests = []
    waiting = threading.Event()
    release_wait = threading.Event()

    class Provider(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def respond(self, body):
            raw = json.dumps(body).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

        def do_GET(self):
            self.respond({"data": [{"id": "mock-model", "object": "model"}]})

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            requests.append(body)
            messages = body.get("messages", [])
            if any(message.get("content") == "FAIL_RUN" for message in messages):
                self.send_error(400, "expected run failure")
                return
            if any(message.get("content") == "WAIT_RUN" for message in messages):
                waiting.set()
                release_wait.wait(10)
                return
            # Restored conversations retain earlier tool results. Only this
            # user's turn determines whether its skill call has completed.
            last_user = max((index for index, message in enumerate(messages)
                             if message.get("role") == "user"), default=-1)
            has_result = any(message.get("role") == "tool" for message in messages[last_user + 1:])
            use_skill = last_user >= 0 and "USE_SKILL" in str(messages[last_user].get("content"))
            message = {"role": "assistant", "content": "skills-ok"}
            if use_skill and not has_result:
                message = {"role": "assistant", "content": None, "tool_calls": [{
                    "id": "read-skill", "type": "function", "function": {
                        "name": "skill", "arguments": json.dumps({
                            "name": "demo", "path": "references/guide.md"})}}]}
            completion = {"id": "skills-test", "object": "chat.completion", "model": "mock-model",
                          "choices": [{"index": 0, "message": message,
                                       "finish_reason": "tool_calls" if "tool_calls" in message else "stop"}],
                          "usage": {"prompt_tokens": 100, "completion_tokens": 5, "total_tokens": 105}}
            if not body.get("stream"):
                self.respond(completion)
                return
            for index, call in enumerate(message.get("tool_calls", [])):
                call["index"] = index
            chunk = {"id": "skills-test", "object": "chat.completion.chunk", "model": "mock-model",
                     "choices": [{"index": 0, "delta": message, "finish_reason": None}]}
            finish = {**chunk, "choices": [{"index": 0, "delta": {},
                      "finish_reason": completion["choices"][0]["finish_reason"]}], "usage": completion["usage"]}
            raw = ("data: " + json.dumps(chunk) + "\n\ndata: " + json.dumps(finish) +
                   "\n\ndata: [DONE]\n\n").encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

    provider = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="ainiux-skills-integration-") as temporary:
            work = Path(temporary).resolve()
            home, project = work / "home", work / "project"
            home.mkdir()
            project.mkdir()
            env = dict(os.environ, HOME=str(home), XDG_CONFIG_HOME=str(work / "config"))
            base = f"http://127.0.0.1:{provider.server_port}/v1"

            def write(path, content):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding="utf-8")

            def run(*args, success=True):
                result = subprocess.run([str(BIN), *args], cwd=project, env=env,
                                        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=20)
                assert (result.returncode == 0) == success, (args, result.returncode, result.stderr)
                return result

            user_source = work / "source/demo"
            write(user_source / "SKILL.md", "---\ndescription: User catalog\n---\nUSER BODY\n")
            write(user_source / "references/guide.md", "GUIDE FROM USER\n")
            installed = run("--add-skill", str(user_source))
            assert not installed.stdout and "Installed skill" in installed.stderr
            assert (home / ".ainiux/skills/demo/references/guide.md").read_text() == "GUIDE FROM USER\n"
            write(project / "skills/demo/SKILL.md", "---\ndescription: Project catalog\n---\nPROJECT BODY\n")
            write(project / "skills/demo/references/guide.md", "GUIDE FROM PROJECT\n")
            entries = json.loads(run("--list-skills", "--format", "json").stdout)["skills"]
            assert len(entries) == 1 and entries[0]["source"] == "project" and entries[0]["name"] == "demo"
            assert Path(entries[0]["path"]) == project / "skills/demo"
            assert json.loads(run("--list-skills", "--format", "ndjson").stdout) == entries[0]
            run("--list-skills", "--chat", success=False)
            run("--remove-skill", "../demo", success=False)
            run("--remove-skill", "demo")
            assert (project / "skills/demo/SKILL.md").exists()
            run("--add-skill", str(user_source))

            common = ["--no-config", "--provider", "lmstudio", "--base-url", base,
                      "-m", "mock-model", "--no-stream", "--quiet"]
            before = len(requests)
            invalid = run(*common, "-r", "task", "--load-skill", str(work / "missing"), success=False)
            assert len(requests) == before and "skill" in invalid.stderr.lower()
            assert run(*common, "-r", "NO_SKILLS", "--no-agent-log").stdout.strip() == "skills-ok"
            plain = requests[-1]
            assert "[Ainiux skills]" not in json.dumps(plain)
            assert not any(tool.get("function", {}).get("name") == "skill" for tool in plain.get("tools", []))
            assert run(*common, "-r", "USE_SKILL", "--no-agent-log",
                       "--load-skill", str(project / "skills/demo")).stdout.strip() == "skills-ok"
            explicit = requests[-2]
            assert "PROJECT BODY" in json.dumps(explicit) and "USER BODY" not in json.dumps(explicit)
            result = next(message for message in requests[-1]["messages"] if message.get("role") == "tool")
            assert "GUIDE FROM PROJECT" in result["content"]
            assert run(*common, "-p", "ordinary chat").stdout.strip() == "skills-ok"
            assert "[Ainiux skills]" not in json.dumps(requests[-1])

            assert not (project / ".ainiux-pr/agent.sqlite").exists(), "headless runs created durable history"

            # Exercise the exact interactive session API used by the WebUI.
            web_project = work / "web-project"
            write(web_project / "skills/demo/SKILL.md", (project / "skills/demo/SKILL.md").read_text())
            write(web_project / "skills/demo/references/guide.md", "GUIDE FROM PROJECT\n")
            token = secrets.token_hex(32)
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                port = reservation.getsockname()[1]
            server_url = f"http://127.0.0.1:{port}"

            def api(method, path, body=None):
                request = urllib.request.Request(server_url + "/ainiux/v1/" + path,
                    data=None if body is None else json.dumps(body).encode(), method=method,
                    headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
                with urllib.request.urlopen(request, timeout=3) as response:
                    return json.load(response)

            def wait_for(callback, label):
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    try:
                        result = callback()
                        if result:
                            return result
                    except (urllib.error.URLError, ConnectionError):
                        pass
                    time.sleep(0.025)
                raise AssertionError("timed out waiting for " + label)

            with (work / "server.log").open("w+") as log:
                server = subprocess.Popen([str(BIN), "server", *common, "--workspace", str(web_project),
                    "--bind", "127.0.0.1", "--port", str(port)],
                    cwd=project, env={**env, "AINIUX_SERVER_SECRET": token},
                    stdin=subprocess.DEVNULL, stdout=log, stderr=log)
                try:
                    wait_for(lambda: api("GET", "health"), "control server")
                    session = api("POST", "sessions/agent", {"kind": "agent",
                                  "model": "mock-model", "permission_mode": "confirm", "lane": "act"})["session"]
                    session_path = "sessions/" + session["id"]
                    wait_for(lambda: api("GET", session_path)["status"] == "ready", "agent preparation")
                    before = len(requests)
                    api("POST", session_path + "/turns", {"text": "USE_SKILL"})
                    wait_for(lambda: len(requests) >= before + 2, "skill tool round")
                    wait_for(lambda: api("GET", session_path)["status"] == "ready", "agent completion")
                    assert api("GET", session_path + "/history")["messages"][-1]["content"] == "skills-ok"
                    automatic = requests[before]
                    serialized = json.dumps(automatic)
                    assert "[Ainiux skills]" in serialized and "Project catalog" in serialized
                    assert "PROJECT BODY" not in serialized and "USER BODY" not in serialized
                    result = next(message for message in requests[-1]["messages"] if message.get("role") == "tool")
                    assert "GUIDE FROM PROJECT" in result["content"]
                    api("DELETE", session_path)
                    database = web_project / ".ainiux-pr/agent.sqlite"
                    def snapshot_database():
                        with sqlite3.connect(database) as connection:
                            return list(connection.iterdump())
                    original_database = snapshot_database()
                    task_file = work / "task.txt"
                    task_file.write_text("FILE_RUN", encoding="utf-8")
                    variants = [(["-r", "SHORT_RUN"], None),
                                (["--run", "LONG_RUN"], None),
                                (["run", "-r", "VERB_RUN"], None),
                                (["--run-file", str(task_file)], None),
                                (["--run-file", "-"], "STDIN_RUN")]
                    for args, stdin in variants:
                        before_run = len(requests)
                        result = subprocess.run([str(BIN), *args, *common, "--no-agent-log"],
                            cwd=web_project, env=env, input=stdin, capture_output=True,
                            text=True, timeout=20)
                        assert result.returncode == 0 and result.stdout.strip() == "skills-ok", result.stderr
                        sent = json.dumps(requests[before_run:])
                        assert "USE_SKILL" not in sent, "interactive history entered headless request"
                        assert snapshot_database() == original_database, "headless run changed project state"
                    failed = subprocess.run([str(BIN), *common, "-r", "FAIL_RUN", "--no-agent-log"],
                        cwd=web_project, env=env, stdin=subprocess.DEVNULL,
                        capture_output=True, text=True, timeout=20)
                    assert failed.returncode == 4 and not failed.stdout, failed
                    assert snapshot_database() == original_database
                    if os.name != "nt":
                        process = subprocess.Popen([str(BIN), *common, "-r", "WAIT_RUN", "--no-agent-log"],
                            cwd=web_project, env=env, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                        try:
                            assert waiting.wait(10), "headless request did not start"
                            process.send_signal(signal.SIGINT)
                            stdout, stderr = process.communicate(timeout=10)
                            assert process.returncode == 130 and not stdout, (process.returncode, stderr)
                        finally:
                            release_wait.set()
                            if process.poll() is None:
                                process.kill()
                                process.wait()
                        assert snapshot_database() == original_database
                    write(web_project / "skills/demo/SKILL.md",
                          "---\ndescription: Refreshed catalog\n---\nUPDATED BODY\n")
                    reopened = api("POST", "sessions/agent", {"kind": "agent"})["session"]
                    session_path = "sessions/" + reopened["id"]
                    wait_for(lambda: api("GET", session_path)["status"] == "ready", "restored agent")
                    before = len(requests)
                    api("POST", session_path + "/turns", {"text": "USE_SKILL"})
                    wait_for(lambda: len(requests) >= before + 2, "restored skill tool round")
                    wait_for(lambda: api("GET", session_path)["status"] == "ready", "restored completion")
                    assert api("GET", session_path + "/history")["messages"][-1]["content"] == "skills-ok"
                    # Live snapshot restoration preserves the original seed;
                    # the supporting-file tool still reads the current catalog.
                    assert "Project catalog" in json.dumps(requests[before])
                    restored_request = json.dumps(requests[before])
                    for marker in ("SHORT_RUN", "LONG_RUN", "VERB_RUN", "FILE_RUN", "STDIN_RUN", "FAIL_RUN", "WAIT_RUN"):
                        assert marker not in restored_request, "headless task entered restored interactive context"
                    assert "UPDATED BODY" not in json.dumps(requests[before])
                    api("DELETE", session_path)
                except BaseException as error:
                    log.seek(0)
                    diagnostics = {"provider_requests": len(requests), "requests_before_turn": before}
                    if "session_path" in locals():
                        diagnostics["session"] = api("GET", session_path)
                        diagnostics["history"] = api("GET", session_path + "/history")
                    raise AssertionError("WebUI skills integration failed: " + json.dumps(diagnostics) +
                                         "; server log:\n" + log.read()) from error
                finally:
                    server.terminate()
                    try:
                        server.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait()
        print("skills CLI and WebUI-agent integration tests passed")
    finally:
        provider.shutdown()
        provider.server_close()
        thread.join(timeout=5)


if __name__ == "__main__":
    main()
