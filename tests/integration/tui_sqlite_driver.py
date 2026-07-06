#!/usr/bin/env python3
"""PTY driver for SQLite-backed TUI persistence integration tests."""

import os
import pty
import select
import sqlite3
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
        [binary, base, "--quiet", "--chat", "--no-stream", "-m", model],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
        env=env,
    )
    os.close(slave)
    time.sleep(0.3)
    drain(master)
    return master, process


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


def run_tui(binary, base, model, home_dir, script, timeout=45):
    master, process = start_tui(binary, base, model, home_dir)
    transcript = bytearray()
    try:
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
    return os.path.join(home_dir, ".pkchat", "pkchat.db")


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
    )
    if b"Loaded last thread" in transcript:
        raise RuntimeError("expected TUI startup to begin a fresh thread instead of reloading the last one")
    if b"/list" not in transcript:
        raise RuntimeError("expected configured startup status to mention /list")


def scenario_beta_and_list_load(binary, base, model, home_dir):
    run_tui(
        binary,
        base,
        model,
        home_dir,
        [
            ("/new Beta\r", 0.5),
            ("sqlite-save-two\r", 1.0),
            ("/list\r", 0.5),
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
    run_tui(
        binary,
        base,
        model,
        home_dir,
        [
            ("/provider openai\r", 2.0),
            ("/quit\r", 0.2),
        ],
    )
    conn = query_db(db_path(home_dir))
    try:
        active_id = last_thread_id(conn)
        if active_id is None:
            raise RuntimeError("expected active last_thread_id before provider update")
        row = conn.execute(
            "SELECT last_provider FROM threads WHERE id = ? AND deleted_at IS NULL",
            (active_id,),
        ).fetchone()
        if row is None:
            raise RuntimeError("expected active thread row after /provider")
        if row["last_provider"] != "openai":
            raise RuntimeError(
                f"expected /provider openai on active thread, got {row['last_provider']!r}"
            )
    finally:
        conn.close()


def scenario_remove_thread(binary, base, model, home_dir):
    run_tui(
        binary,
        base,
        model,
        home_dir,
        [
            ("/remove\r", 0.2),
            ("y\r", 0.5),
            ("/quit\r", 0.2),
        ],
    )
    conn = query_db(db_path(home_dir))
    try:
        threads = active_threads(conn)
        if len(threads) != 1 or threads[0]["name"] != "Beta":
            raise RuntimeError("expected only Beta thread after /remove")
        if last_thread_id(conn) not in (None, 0):
            raise RuntimeError("expected last_thread_id to clear after thread removal")
        removed = conn.execute(
            "SELECT deleted_at FROM threads WHERE name = 'Alpha'"
        ).fetchone()
        if removed is None or removed["deleted_at"] in (None, ""):
            raise RuntimeError("expected Alpha thread to be soft-deleted")
    finally:
        conn.close()


def scenario_stale_last_thread(binary, base, model, home_dir):
    conn = query_db(db_path(home_dir))
    try:
        conn.execute(
            "INSERT INTO app_state(key, value, updated_at) VALUES('last_thread_id', '99999', '2026-06-28T00:00:00Z') "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at"
        )
        conn.commit()
    finally:
        conn.close()

    transcript = run_tui(
        binary,
        base,
        model,
        home_dir,
        [("/quit\r", 0.2)],
    )
    if b"SQLite chat thread not found" not in transcript:
        raise RuntimeError("expected stale last_thread_id load failure message in TUI output")


def scenario_corrupt_database(binary, base, model, home_dir):
    path = db_path(home_dir)
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
        b"SQLite persistence unavailable" not in transcript
        and b"could not open SQLite database" not in transcript
        and b"notadb" not in transcript.lower()
    ):
        raise RuntimeError("expected corrupt SQLite database to surface a persistence error")


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
        "seed-alpha": scenario_seed_alpha,
        "fresh-start": scenario_fresh_start,
        "beta-list-load": scenario_beta_and_list_load,
        "provider-update": scenario_provider_update,
        "remove": scenario_remove_thread,
        "stale-last": scenario_stale_last_thread,
        "corrupt-db": scenario_corrupt_database,
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