#!/usr/bin/env python3
"""PTY driver for SQLite-backed TUI persistence integration tests."""

import os
import pty
import select
import shutil
import sqlite3
import subprocess
import sys
import tempfile
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


def send(master, text, delay=0.35):
    os.write(master, text.encode("utf-8"))
    time.sleep(delay)
    return drain(master)


def require_running(process, description):
    if process.poll() is not None:
        raise RuntimeError(f"{description} exited early with status {process.returncode}")


def start_tui(binary, base, model, home_dir):
    env = os.environ.copy()
    env["HOME"] = home_dir
    env["TERM"] = env.get("TERM", "xterm-256color")
    env.setdefault("OPENAI_API_KEY", "integration-test-key")
    master, slave = pty.openpty()
    process = subprocess.Popen(
        [binary, base, "--quiet", "--chat", "--no-stream", "-m", model,
         "--image-capability", "allow"],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
        env=env,
    )
    os.close(slave)
    # Wait briefly for the first paint, but keep the bytes: startup status
    # (for example the /list hint) is part of the transcript scenarios assert on.
    time.sleep(0.3)
    startup = drain(master, timeout=0.5)
    return master, process, startup


def stop_tui(master, process, timeout=10):
    try:
        if process.poll() is None:
            process.wait(timeout=timeout)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=3)
        os.close(master)
    if process.returncode != 0:
        raise RuntimeError(f"TUI exited with status {process.returncode}")


def run_tui(binary, base, model, home_dir, script, timeout=45,
            dismiss_startup_thread_list=True):
    master, process, startup = start_tui(binary, base, model, home_dir)
    transcript = bytearray(startup)
    try:
        # Chat opens the thread selector on startup (same as Ctrl+L). Most
        # scenarios expect normal chat input next, so N starts a new thread.
        if dismiss_startup_thread_list:
            transcript.extend(send(master, "n", 0.4))
        for item in script:
            if isinstance(item, tuple):
                text, delay = item
            else:
                text, delay = item, 0.35
            transcript.extend(send(master, text, delay))
            if process.poll() is not None:
                break
            require_running(process, "TUI")
        if process.poll() is None:
            process.wait(timeout=timeout)
    finally:
        if process.poll() is None:
            stop_tui(master, process, timeout)
        else:
            os.close(master)
    if process.returncode != 0:
        raise RuntimeError(f"TUI exited with status {process.returncode}")
    return bytes(transcript)


def wait_for_thread_field(path, thread_id, column, expected, timeout=8.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        conn = query_db(path)
        try:
            row = conn.execute(
                f"SELECT {column} FROM threads WHERE id = ? AND deleted_at IS NULL",
                (thread_id,),
            ).fetchone()
            if row is not None and row[column] == expected:
                return
        finally:
            conn.close()
        time.sleep(0.1)
    raise RuntimeError(
        f"timed out waiting for thread {thread_id} {column} == {expected!r}"
    )


def db_path(home_dir):
    return os.path.join(home_dir, ".ainiux", "ainiux.db")


def query_db(path):
    if not os.path.exists(path):
        raise RuntimeError(f"SQLite database was not created: {path}")
    conn = sqlite3.connect(path)
    conn.row_factory = sqlite3.Row
    return conn


def active_threads(conn):
    rows = conn.execute(
        "SELECT id, name, last_provider, last_model, message_count "
        "FROM threads WHERE deleted_at IS NULL ORDER BY modified_at DESC, id DESC"
    ).fetchall()
    return [dict(row) for row in rows]


def message_texts(conn, thread_id):
    rows = conn.execute(
        "SELECT role, content FROM messages WHERE thread_id = ? ORDER BY ordinal",
        (thread_id,),
    ).fetchall()
    return [(row["role"], row["content"]) for row in rows]


def last_thread_id(conn):
    row = conn.execute(
        "SELECT value FROM app_state WHERE key = 'last_thread_id'"
    ).fetchone()
    if row is None:
        return None
    try:
        value = int(row["value"])
    except ValueError:
        return None
    return value if value > 0 else None


def scenario_seed_alpha(binary, base, model, home_dir):
    run_tui(
        binary,
        base,
        model,
        home_dir,
        [
            ("/new Alpha\r", 0.5),
            ("sqlite-save-one\r", 1.0),
            ("/quit\r", 0.2),
        ],
    )
    conn = query_db(db_path(home_dir))
    try:
        threads = active_threads(conn)
        if len(threads) != 1:
            raise RuntimeError(f"expected one saved thread, found {len(threads)}")
        thread = threads[0]
        if thread["name"] != "Alpha":
            raise RuntimeError(f"expected thread name Alpha, got {thread['name']!r}")
        messages = message_texts(conn, thread["id"])
        user_messages = [content for role, content in messages if role == "user"]
        if "sqlite-save-one" not in user_messages:
            raise RuntimeError("expected sqlite-save-one user message in database")
        assistant_messages = [content for role, content in messages if role == "assistant"]
        if not assistant_messages or assistant_messages[-1] != "Hello":
            raise RuntimeError("expected assistant reply Hello in database")
        if last_thread_id(conn) != thread["id"]:
            raise RuntimeError("expected last_thread_id to match saved thread")
    finally:
        conn.close()


def scenario_fresh_start(binary, base, model, home_dir):
    transcript = run_tui(
        binary,
        base,
        model,
        home_dir,
        [("/quit\r", 0.2)],
        dismiss_startup_thread_list=False,
    )
    if b"Loaded last thread" in transcript:
        raise RuntimeError("expected TUI startup to begin a fresh thread instead of reloading the last one")
    rendered = transcript.decode("utf-8", errors="replace")
    if "Newest first" not in rendered and b"/list" not in transcript:
        raise RuntimeError("expected chat startup thread selector or /list hint")


def scenario_beta_and_list_load(binary, base, model, home_dir):
    run_tui(
        binary,
        base,
        model,
        home_dir,
        [
            ("/new Beta\r", 0.5),
            ("sqlite-save-two\r", 1.0),
            ("\x0c", 0.5),  # Ctrl+L thread list
            ("\x1b[B", 0.3),
            ("\r", 1.5),
            ("/quit\r", 0.2),
        ],
    )
    conn = query_db(db_path(home_dir))
    try:
        threads = active_threads(conn)
        if len(threads) != 2:
            raise RuntimeError(f"expected two active threads, found {len(threads)}")
        if threads[0]["name"] != "Beta":
            raise RuntimeError("expected newest thread Beta after second save")
        alpha = next((thread for thread in threads if thread["name"] == "Alpha"), None)
        if alpha is None:
            raise RuntimeError("expected Alpha thread to remain after list/load")
        if last_thread_id(conn) != alpha["id"]:
            raise RuntimeError("expected last_thread_id to follow loaded Alpha thread")
    finally:
        conn.close()


def scenario_provider_update(binary, base, model, home_dir):
    # Self-contained: create a non-empty thread, switch provider, cancel the
    # model-list job (avoids hanging on api.openai.com offline), then quit.
    run_tui(
        binary,
        base,
        model,
        home_dir,
        [
            ("/new ProviderTest\r", 0.5),
            ("provider-ping\r", 1.5),
            ("/provider openai\r", 1.0),
            ("\x1b", 0.5),
            ("/quit\r", 0.5),
        ],
    )
    conn = query_db(db_path(home_dir))
    try:
        row = conn.execute(
            "SELECT id, last_provider FROM threads "
            "WHERE name = 'ProviderTest' AND deleted_at IS NULL"
        ).fetchone()
        if row is None:
            raise RuntimeError("expected ProviderTest thread after /provider scenario")
        if row["last_provider"] != "openai":
            raise RuntimeError(
                f"expected ProviderTest last_provider openai, got {row['last_provider']!r}"
            )
    finally:
        conn.close()


def scenario_remove_thread(binary, base, model, home_dir):
    # Self-contained remove: create a non-empty thread, remove it, confirm.
    run_tui(
        binary,
        base,
        model,
        home_dir,
        [
            ("/new ToRemove\r", 0.5),
            ("remove-me\r", 1.5),
            ("/remove\r", 0.5),
            ("y\r", 0.8),
            ("/quit\r", 0.5),
        ],
    )
    conn = query_db(db_path(home_dir))
    try:
        removed = conn.execute(
            "SELECT deleted_at FROM threads WHERE name = 'ToRemove'"
        ).fetchone()
        if removed is None or removed["deleted_at"] in (None, ""):
            raise RuntimeError("expected ToRemove thread to be soft-deleted")
        # Earlier seeded threads should still be present.
        for name in ("Alpha", "Beta"):
            row = conn.execute(
                "SELECT id FROM threads WHERE name = ? AND deleted_at IS NULL",
                (name,),
            ).fetchone()
            if row is None:
                raise RuntimeError(f"expected {name} thread to remain after /remove")
    finally:
        conn.close()


def scenario_stale_last_thread(binary, base, model, home_dir):
    # Auto-resume of last_thread_id on TUI startup is not implemented yet (see TODO.md).
    # This scenario still verifies that a stale last_thread_id value does not prevent
    # a normal chat session from starting and exiting cleanly.
    conn = query_db(db_path(home_dir))
    try:
        conn.execute(
            "INSERT INTO app_state(key, value, updated_at) VALUES('last_thread_id', '99999', '2026-06-28T00:00:00Z') "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at"
        )
        conn.commit()
    finally:
        conn.close()

    run_tui(
        binary,
        base,
        model,
        home_dir,
        [
            ("", 0.8),
            ("/quit\r", 0.5),
        ],
    )
    # Database must remain openable after the session.
    conn = query_db(db_path(home_dir))
    try:
        row = conn.execute(
            "SELECT value FROM app_state WHERE key = 'last_thread_id'"
        ).fetchone()
        if row is None or str(row["value"]) != "99999":
            raise RuntimeError("expected stale last_thread_id value to remain until a real load path uses it")
    finally:
        conn.close()


def scenario_media_restart(binary, base, model, home_dir):
    image_path = os.environ.get("AINIUX_SQLITE_TEST_IMAGE")
    if not image_path:
        raise RuntimeError("AINIUX_SQLITE_TEST_IMAGE is not set")
    media_home = home_dir + "-media-restart"
    shutil.rmtree(media_home, ignore_errors=True)
    os.makedirs(media_home, exist_ok=True)
    config_dir = os.path.join(media_home, ".config", "ainiux")
    os.makedirs(config_dir, exist_ok=True)
    with open(os.path.join(config_dir, "config.conf"), "w", encoding="utf-8") as config:
        config.write(
            "config_version = 1\n"
            "[media]\n"
            "expiration_days = 7\n"
            "auto_expiration_days = 0\n"
        )
    run_tui(
        binary,
        base,
        model,
        media_home,
        [
            (f"/attach {image_path}\r", 0.8),
            ("image-seed\r", 1.2),
            ("image-followup\r", 1.2),
            ("/quit\r", 0.5),
        ],
    )
    path = db_path(media_home)
    conn = query_db(path)
    try:
        threads = active_threads(conn)
        if len(threads) != 1:
            raise RuntimeError(f"expected one media thread, found {len(threads)}")
        thread_id = threads[0]["id"]
        attachment = conn.execute(
            "SELECT storage_ref, object_sha256 FROM attachments WHERE thread_id = ?",
            (thread_id,),
        ).fetchone()
        if attachment is None or attachment["storage_ref"] != "":
            raise RuntimeError("expected external managed-media attachment metadata")
        digest = attachment["object_sha256"]
        media_path = os.path.join(media_home, ".ainiux", "media", "sha256", digest[:2], digest)
        if not os.path.isfile(media_path):
            raise RuntimeError("expected managed image file beside the SQLite database")
    finally:
        conn.close()

    transcript = run_tui(
        binary,
        base,
        model,
        media_home,
        [
            ("/list\r", 0.6),
            ("\r", 1.0),
            ("expect-restored-image\r", 1.5),
            ("/quit\r", 0.5),
        ],
    )
    if b"AINIUX_ERR_HTTP_STATUS" in transcript:
        raise RuntimeError("restored media thread produced an HTTP validation error")
    conn = query_db(path)
    try:
        messages = message_texts(conn, thread_id)
        if ("user", "expect-restored-image") not in messages:
            raise RuntimeError("expected post-restart user prompt in the saved thread")
        if not messages or messages[-1] != ("assistant", "Hello"):
            raise RuntimeError("expected successful post-restart assistant response")
    finally:
        conn.close()

    conn = query_db(path)
    try:
        conn.execute(
            "UPDATE media_objects SET last_used_at = '2020-01-01T00:00:00Z'"
        )
        conn.commit()
    finally:
        conn.close()
    master, process, _ = start_tui(binary, base, model, media_home)
    try:
        # Dismiss startup thread selector before slash commands.
        send(master, "n", 0.4)
        send(master, "/cleanup\r", 0.05)
        wait_for_thread_field(path, thread_id, "read_only", 1)
        send(master, "/quit\r", 0.1)
        stop_tui(master, process)
        master = -1
    finally:
        if master >= 0:
            if process.poll() is None:
                os.write(master, b"\x11")
            stop_tui(master, process)
    conn = query_db(path)
    try:
        thread = conn.execute(
            "SELECT read_only, read_only_reason FROM threads WHERE id = ?",
            (thread_id,),
        ).fetchone()
        if thread is None or thread["read_only"] != 1 or "7 days" not in thread["read_only_reason"]:
            raise RuntimeError("expected /cleanup to mark the expired media thread read-only")
        if os.path.exists(media_path):
            raise RuntimeError("expected /cleanup to remove the expired managed image")
    finally:
        conn.close()

    transcript = run_tui(
        binary,
        base,
        model,
        media_home,
        [("/list\r", 0.6), ("\r", 1.0), ("must-not-send\r", 0.6), ("\x11", 0.5)],
    )
    if b"Thread is read-only" not in transcript:
        raise RuntimeError("expected a read-only status when continuing an expired-media thread")
    conn = query_db(path)
    try:
        messages = message_texts(conn, thread_id)
        if any(content == "must-not-send" for _, content in messages):
            raise RuntimeError("read-only thread accepted a new prompt")
    finally:
        conn.close()


def scenario_markdown_restart(binary, base, model, home_dir):
    markdown_home = home_dir + "-markdown-restart"
    shutil.rmtree(markdown_home, ignore_errors=True)
    os.makedirs(markdown_home, exist_ok=True)
    config_dir = os.path.join(markdown_home, ".config", "ainiux")
    os.makedirs(config_dir, exist_ok=True)
    with open(os.path.join(config_dir, "config.conf"), "w", encoding="utf-8") as config:
        config.write(
            "config_version = 1\n"
            "[media]\n"
            "max_size_to_store_to_db = 64\n"
            "auto_expiration_days = 0\n"
        )
    html_path = os.path.join(markdown_home, "large.html")
    small_path = os.path.join(markdown_home, "small.md")
    with open(html_path, "w", encoding="utf-8") as attachment:
        attachment.write(
            "<html><body><h1>Persistent HTML heading</h1><p>" +
            ("converted-once " * 12) + "</p></body></html>"
        )
    with open(small_path, "w", encoding="utf-8") as attachment:
        attachment.write("small-native-marker\n")

    transcript = run_tui(
        binary,
        base,
        model,
        markdown_home,
        [
            (f"/attach {html_path}\r", 0.8),
            (f"/attach {small_path}\r", 0.8),
            ("markdown-seed\r", 1.2),
            ("expect-restored-markdown\r", 1.4),
            ("/quit\r", 0.5),
        ],
    )
    if b"AINIUX_ERR_HTTP_STATUS" in transcript:
        raise RuntimeError("same-process Markdown follow-up lost attachment context")

    path = db_path(markdown_home)
    conn = query_db(path)
    try:
        threads = active_threads(conn)
        if len(threads) != 1:
            raise RuntimeError(f"expected one Markdown thread, found {len(threads)}")
        thread_id = threads[0]["id"]
        attachments = conn.execute(
            "SELECT display_name, inline_content, object_sha256 FROM attachments "
            "WHERE thread_id = ? AND kind = 'markdown' ORDER BY ordinal, id",
            (thread_id,),
        ).fetchall()
        if len(attachments) != 2:
            raise RuntimeError(f"expected two Markdown attachments, found {len(attachments)}")
        inline = next((row for row in attachments if row["display_name"] == small_path), None)
        managed = next((row for row in attachments if row["display_name"] == html_path), None)
        if inline is None or inline["inline_content"] != "small-native-marker\n" or inline["object_sha256"]:
            raise RuntimeError("small Markdown attachment was not stored inline")
        if managed is None or managed["inline_content"] or not managed["object_sha256"]:
            raise RuntimeError("large converted HTML attachment was not stored as managed Markdown")
        digest = managed["object_sha256"]
        managed_path = os.path.join(
            markdown_home, ".ainiux", "media", "sha256", digest[:2], digest + ".md"
        )
        if not os.path.isfile(managed_path):
            raise RuntimeError("expected a managed .md attachment object")
        with open(managed_path, "r", encoding="utf-8") as attachment:
            converted = attachment.read()
        if "Persistent HTML heading" not in converted or "<h1>" in converted:
            raise RuntimeError("managed HTML attachment was not converted once to Markdown")
    finally:
        conn.close()

    # Changing the source after import must not affect durable replay.
    with open(html_path, "w", encoding="utf-8") as attachment:
        attachment.write("<h1>CHANGED SOURCE MUST NOT BE USED</h1>")
    transcript = run_tui(
        binary,
        base,
        model,
        markdown_home,
        [
            ("/list\r", 0.6),
            ("\r", 1.0),
            ("expect-restored-markdown\r", 1.5),
            ("/quit\r", 0.5),
        ],
    )
    if b"AINIUX_ERR_HTTP_STATUS" in transcript:
        raise RuntimeError("restored Markdown thread failed durable replay validation")


def scenario_incomplete_thread_setup(binary, base, model, home_dir):
    setup_home = home_dir + "-incomplete-thread"
    shutil.rmtree(setup_home, ignore_errors=True)
    os.makedirs(setup_home, exist_ok=True)
    run_tui(
        binary,
        base,
        model,
        setup_home,
        [
            ("/new IncompleteModel\r", 0.5),
            ("setup-seed\r", 1.2),
            ("/quit\r", 0.5),
        ],
    )

    path = db_path(setup_home)
    conn = query_db(path)
    try:
        row = conn.execute(
            "SELECT id FROM threads WHERE name = 'IncompleteModel' AND deleted_at IS NULL"
        ).fetchone()
        if row is None:
            raise RuntimeError("expected seeded IncompleteModel thread")
        thread_id = row["id"]
        conn.execute(
            "UPDATE threads SET last_provider = 'openrouter', "
            "last_base_url = 'https://openrouter.ai/api/v1', last_model = '' "
            "WHERE id = ?",
            (thread_id,),
        )
        conn.commit()
    finally:
        conn.close()

    transcript = run_tui(
        binary,
        base,
        model,
        setup_home,
        [
            ("/list\r", 0.6),
            ("\r", 0.8),
            ("\x1b", 0.4),
            ("must-not-send\r", 0.6),
            ("\x11", 0.5),
        ],
    )
    if b"[SETUP: model missing]" not in transcript:
        raise RuntimeError("expected the thread picker to label the missing model")
    if b"/provider, then /model" not in transcript or b"sending disabled" not in transcript:
        raise RuntimeError("expected incomplete-thread sending to remain visibly disabled")
    conn = query_db(path)
    try:
        messages = message_texts(conn, thread_id)
        if any(content == "must-not-send" for _, content in messages):
            raise RuntimeError("incomplete thread accepted a prompt before provider/model setup")
    finally:
        conn.close()

    transcript = run_tui(
        binary,
        base,
        model,
        setup_home,
        [
            ("/list\r", 0.6),
            ("\r", 0.8),
            ("\x1b", 0.4),
            (f"/provider {base}\r", 1.2),
            ("after-required-setup\r", 1.4),
            ("/quit\r", 0.5),
        ],
    )
    if b"AINIUX_ERR_HTTP_STATUS" in transcript:
        raise RuntimeError("configured incomplete thread produced an HTTP error")
    conn = query_db(path)
    try:
        row = conn.execute(
            "SELECT last_provider, last_model FROM threads WHERE id = ?", (thread_id,)
        ).fetchone()
        if row is None or row["last_model"] != model:
            raise RuntimeError("provider/model setup was not persisted on the repaired thread")
        messages = message_texts(conn, thread_id)
        if ("user", "after-required-setup") not in messages or messages[-1] != ("assistant", "Hello"):
            raise RuntimeError("repaired thread did not accept a successful follow-up prompt")
    finally:
        conn.close()


def scenario_corrupt_database(binary, base, model, home_dir):
    path = db_path(home_dir)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(b"not-a-sqlite-database")

    transcript = run_tui(
        binary,
        base,
        model,
        home_dir,
        [("/quit\r", 0.2)],
    )
    if (
        b"Saved chat database unavailable" not in transcript
        and b"could not open SQLite database" not in transcript
        and b"notadb" not in transcript.lower()
    ):
        raise RuntimeError("expected corrupt SQLite database to surface a persistence error")

    list_transcript = run_tui(
        binary,
        base,
        model,
        home_dir,
        [("/list\r", 0.2), ("/quit\r", 0.2)],
    )
    if b"Saved chat database unavailable" not in list_transcript:
        raise RuntimeError("expected /list to repeat the saved chat database error")


def scenario_agent_deferred_startup_prompt(binary, base, model, home_dir):
    # Keep the agent workspace outside the source tree: a parent .ainiux-pr
    # (for example the developer's repo project) makes nested build/ paths
    # ambiguous and prepare() fails before the deferred -p turn can run.
    del home_dir  # scenario isolates HOME; shared sqlite home is unused
    agent_home = tempfile.mkdtemp(prefix="ainiux-agent-startup-")
    workspace = os.path.join(agent_home, "workspace")
    os.makedirs(workspace, exist_ok=True)

    env = os.environ.copy()
    env["HOME"] = agent_home
    env["TERM"] = env.get("TERM", "xterm-256color")
    env.setdefault("OPENAI_API_KEY", "integration-test-key")
    master, slave = pty.openpty()
    process = subprocess.Popen(
        [
            binary,
            base,
            "--quiet",
            "--agent",
            "--disable-indexing",
            "--no-stream",
            "-m",
            model,
            "-p",
            "deferred startup prompt marker",
        ],
        cwd=workspace,
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
        env=env,
    )
    os.close(slave)
    transcript = bytearray()
    deadline = time.time() + 15
    try:
        while time.time() < deadline and process.poll() is None:
            transcript.extend(drain(master, timeout=0.1))
            if b"Task complete" in transcript:
                break
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=3)
        os.close(master)

    try:
        if b"Task complete" not in transcript:
            raise RuntimeError(
                "agent launch-time prompt was dropped during asynchronous preparation"
            )

        project_db = os.path.join(workspace, ".ainiux-pr", "agent.sqlite")
        conn = query_db(project_db)
        try:
            row = conn.execute(
                "SELECT COUNT(*) AS count FROM messages "
                "WHERE role = 'user' AND content = 'deferred startup prompt marker'"
            ).fetchone()
            if row is None or row["count"] != 1:
                raise RuntimeError(
                    "deferred agent prompt was not persisted exactly once"
                )
        finally:
            conn.close()
    finally:
        shutil.rmtree(agent_home, ignore_errors=True)


def main():
    if len(sys.argv) < 2:
        print(
            "usage: tui_sqlite_driver.py BINARY BASE MODEL HOME_DIR [scenario]",
            file=sys.stderr,
        )
        sys.exit(2)

    binary, base, model, home_dir = sys.argv[1:5]
    scenario = sys.argv[5] if len(sys.argv) > 5 else "all"

    scenarios = {
        "media-restart": scenario_media_restart,
        "markdown-restart": scenario_markdown_restart,
        "incomplete-thread": scenario_incomplete_thread_setup,
        "seed-alpha": scenario_seed_alpha,
        "fresh-start": scenario_fresh_start,
        "beta-list-load": scenario_beta_and_list_load,
        "provider-update": scenario_provider_update,
        "remove": scenario_remove_thread,
        "stale-last": scenario_stale_last_thread,
        "corrupt-db": scenario_corrupt_database,
        "agent-startup-prompt": scenario_agent_deferred_startup_prompt,
    }

    if scenario == "all":
        for name, func in scenarios.items():
            func(binary, base, model, home_dir)
        print("sqlite integration scenarios passed")
        return

    if scenario not in scenarios:
        print(f"unknown scenario: {scenario}", file=sys.stderr)
        sys.exit(2)
    scenarios[scenario](binary, base, model, home_dir)
    print(f"sqlite scenario passed: {scenario}")


if __name__ == "__main__":
    main()
