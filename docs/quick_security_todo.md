# Security Hardening Plan — ainiux

Scope: security review of the most critical surfaces first (agent process execution,
tool path containment, Guard rules, HTTP/fetch/SSRF, credential handling, SQLite
stores, JSON/HTML parsing). Findings are ranked by severity. Each finding cites
file:line evidence, an exploit sketch, a concrete fix, and test coverage. The
project's documented threat model (`docs/security.md`) is the baseline: the agent is
an LLM-driven actor inside a user-owned workspace; `/shell` is the only full-shell
surface; network tools must never reach private/loopback targets.

---

## 1. Summary

| # | Severity | Area | Finding |
|---|----------|------|---------|
| F1 | **Medium** | agent session store | `.ainiux-pr/agent.sqlite` transcript created world-readable (0644) |
| F2 | **Medium** | agent Guard | `git restore` / `git checkout -- <path>` / `git rm` silently discard uncommitted work without Ask |
| F3 | **Medium** | agent Guard + archive tools | Zip-slip: `tar`/`unzip`/`7z` extraction can write outside the workspace |
| F4 | **Medium** | process runner | Daemonizing child (`setsid`/double-fork) escapes timeout kill and can wedge the parent in an infinite drain loop |
| F5 | **Medium** | JSON facade | No nesting-depth limit → stack overflow (crash/DoS) on deeply nested provider/tool input |
| F6 | **Medium** | file writes | `write_bytes_atomic` renames a 0644 temp over the target: overwriting a 0600 file silently widens its mode |
| F7 | Low–Med | process runner | Child inherits all parent fds (no `close_range` before `execve`) |
| F8 | Low–Med | credential redaction | Redaction is substring-based; URL-embedded credentials appear unredacted in transport errors |
| F9 | Low | Guard + process | Egress/destructive gaps: `rsync`, `git push`, `pkill`/`killall`, `blkdiscard`, `git reset` non-hard, `git stash drop` |
| F10 | Low | apply_patch | Add File silently `create_directories` — bypasses Plan "no directory creation" and `ask_on_create_dirs` Guard |
| F11 | Low | headers | `validate_header` accepts CR/LF (currently blocked only by libcurl) |
| F12 | Low | workspace writes | TOCTOU window between path validation and open/write (symlink swap by local attacker) |
| F13 | Info | key files | `--key-file` content read without a permission warning |

Verified solid (do not regress): connect-time SSRF socket filtering incl. IPv4-mapped
IPv6 and DNS rebinding; scheme/userinfo/proxy checks in `fetch`; shell-free
tokenizer + fixed PATH + restricted child env; component-level path containment with
canonical re-checks and symlink refusal; parameterized SQLite everywhere; 0600 modes
for chat DB / index.sqlite / review+agent JSONL logs / editor locks / media store;
credential redaction in transcripts, logs, tool results, and HTTP traces; Guard
denies for shells/sudo/disk destroyers/package managers/remote shells; Plan-mode
write allowlist; bounded outputs, timeouts, reaping, RAII.

---

## 2. Findings

### F1 — `agent.sqlite` transcript created world-readable (Medium)

- Evidence: `src/agent/session_store.cpp` `AgentSessionStore::open` calls
  `sqlite3_open_v2(path, ..., SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|...)` with no
  `chmod`; `Makefile` has no `SQLITE_DEFAULT_FILE_PERMISSIONS`. SQLite's default
  creation mode is 0644, so under umask 022 the file is world-readable; `-wal`/`-shm`
  sidecars inherit the same mode. Contrast: chat DB pre-creates with `open(..., 0600)`
  (`src/chat/sqlite_store.cpp:100-109`), `index.sqlite` is chmod'd 0600
  (`src/agent/index/index.cpp:1051,1061`), review/agent JSONL logs are chmod'd 0600
  (`src/agent/review_log.cpp:81`).
- Impact: the transcript (user prompts, full tool arguments/results, reasoning
  previews, approval records) is readable by any local user. This contradicts the
  documented "user-only" posture for other stores.
- Fix:
  1. After `sqlite3_open_v2` succeeds (or immediately after creating the file),
     `::chmod(path, 0600)`; also `chmod` the `-wal`/`-shm` sidecars once they appear
     (or set `PRAGMA journal_mode=DELETE` for the project DB, or pre-create the file
     with `open(O_CREAT, 0600)` exactly like `sqlite_store.cpp`).
  2. Do the same for any other `sqlite3_open_v2` + CREATE call site (grep the tree).
- Tests: `tests/unit/chat/`-style test that opens a store in a temp dir and asserts
  `stat` mode is 0600 for `agent.sqlite` and sidecars; keep umask at 022 in the test.

### F2 — `git restore`/`git checkout -- <path>`/`git rm` discard uncommitted work without Ask (Medium)

- Evidence: `src/agent/command_guard.cpp:140-145` only Ask for `checkout|restore|switch`
  with `--force|-f|--ours|--theirs`. `git restore <path>` and `git checkout -- <path>`
  overwrite working-tree files (destroying uncommitted edits) with no Ask. `git rm`
  (incl. `-r`) is not guarded at all (deletes tracked files + index entries).
- Fix: in the `git` Guard block add Ask rules:
  - `restore`, `checkout`, `switch`: Ask when any non-option operand is a path (`--`
    separator present, or operand is not a known revision-like token — conservative:
    Ask whenever the subcommand has any operand after options).
  - `rm`: Ask for all forms (this is a delete).
  - `reset`: Ask for `--hard|--merge|--soft` and any `<commit>` operand (branch
    rewind).
  - `stash drop|clear`, `branch|tag -d` already partially covered (`branch -D` Ask);
    add `stash`.
- Tests: `tests/unit/agent/` Guard unit tests for `git restore file`, `git checkout -- f`,
  `git rm -r dir`, `git reset HEAD~1`, `git stash drop` → Ask/Deny; existing allow
  forms (`git status`, `git diff`) stay Allow.

### F3 — Zip-slip via archive extraction (Medium)

- Evidence: agent `run_command` Guard (`src/agent/command_guard.cpp`) denies shells,
  sudo, disk destroyers, package managers, remote shells — but not `tar`, `unzip`,
  `7z`, `unrar`, `unar`, `bsdtar`. `enforce_common_safety`
  (`src/agent/process.cpp:261-281`) validates only command-line operands; archive
  member names are not inspected. `tar xf evil.tar` inside the workspace can write
  `../…` or follow symlink members to arbitrary user-writable paths — a workspace
  containment escape the current checks cannot see.
- Fix (defense in depth):
  1. Guard: Ask for `tar`/`unzip`/`7z`/`unrar`/`unar`/`bsdtar` invocations that
     extract (`-x`, `extract`, `-d` target present for unzip/7z) — matches the
     existing "Ask for high-risk" posture; headless keeps Deny.
  2. Runtime hardening (cheap, no new deps): when `tar` is executed, inject
     `--no-same-owner --no-same-permissions`; document that member-name traversal is
     the tool layer's residual risk and steer models to `apply_patch`/`write_file`
     instead.
  3. Document in `docs/security.md` § agent tools.
- Tests: Guard unit tests for `tar -xf x.tar`, `unzip x.zip -d dir`, `7z x a.7z`
  → Ask; `tar -tf x.tar` (list) stays Allow. Note: full member-traversal defense
  would require extracting into a sandbox dir and re-validating every member with the
  same containment helpers — record as future work if extraction becomes a supported
  workflow.

### F4 — Daemonizing child escapes kill and can wedge the parent (Medium)

- Evidence: `src/agent/process.cpp` `execute_resolved_command`: timeout/cancel sends
  `SIGTERM`/`SIGKILL` to the process group (`::kill(-pid, …)`). A child that calls
  `setsid()` or double-forks leaves the group; if it keeps the inherited stdout/stderr
  pipe write-end open, `stdout_open`/`stderr_open` stay true forever and the loop
  `while (!reaped || stdout_open || stderr_open)` (`process.cpp:725`) spins on 25 ms
  polls indefinitely — a hang. The daemon also survives past the timeout.
- Fix:
  1. After sending SIGKILL, also `close` the read ends to force the loop to
     terminate even if a stray process holds the write ends; add a hard cap on total
     elapsed time (e.g., timeout + 5 s) after which the loop breaks regardless.
  2. Reduce the escape surface: in the child, before `execve`, call
     `close_range(3, ~0U, 0)` (Linux) / `closefrom(3)` (BSD/macOS) — this also fixes
     F7; on Linux additionally `prctl(PR_SET_PDEATHSIG, SIGKILL)` would bound
     grandchildren, but the group-kill + close-read-ends fix is the primary one.
- Tests: `tests/unit/agent/` — run `/bin/sh -c 'setsid sleep 5 &'`-style payload via a
  shell? Shells are denied in Agent policy; test via the process runner directly with
  a tiny helper binary (or `python3 -c "import os; os.setsid(); ..."` under a policy
  that allows python) that daemonizes and holds stdout; assert `run_command` returns
  within timeout + bound and does not hang. Also test the normal timeout path still
  returns `ErrorCode::Timeout`.

### F5 — JSON parser: no nesting-depth limit (Medium)

- Evidence: `src/json/json.cpp` `Parser::parse_value` recurses through
  `parse_array`/`parse_object` with no depth counter. Untrusted inputs parsed:
  provider HTTP/SSE responses (incl. Responses items), web-search JSON, tool
  arguments, `--load-chat` files, benchmark datasets (16 MiB cap), config files.
  A malicious or compromised endpoint (or a prompt-injected tool response) can send
  `[[[[…` to overflow the stack and crash the client.
- Fix: add a `depth_` member to `Parser`; increment in `parse_array`/`parse_object`,
  decrement on exit, reject > 200 with `fail("maximum nesting depth exceeded")`.
  Keep the limit far above legitimate provider payloads (typical depth < 20).
- Tests: `tests/unit/json/` — parse `[`×100000 + `]`×100000 returns
  `ErrorCode::JsonParse` (no crash); normal nested payloads still parse; also add a
  16 MiB-deep-ish regression case at the limit boundary.

### F6 — `write_bytes_atomic` widens file modes on overwrite (Medium)

- Evidence: `src/agent/tools.cpp:1628-1649`: temp file written with `std::ofstream`
  (default 0644 under umask) then `fs::rename` over the target; rename preserves the
  temp's mode, so overwriting a 0600 workspace file (or an approved external file —
  same helper used at `tools.cpp:5411`) silently leaves it 0644. Also: predictable
  temp name (`target + ".ainiux-tmp." + pid`, no `O_EXCL`), no fsync before rename.
- Fix:
  1. If the target exists, `stat` it first and `chmod` the temp to the target's mode
     before rename; for new files keep umask behavior (or 0600 for external approved
     writes — decide and document).
  2. Create the temp with `open(O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC, 0600)` using a
     randomized suffix; retry on `EEXIST`.
  3. `fsync` the temp before rename, and fsync the parent dir after rename where
     supported (matches `--save-chat` behavior described in `docs/security.md`).
- Tests: `tests/unit/agent/` — overwrite a 0600 file via `write_workspace_file` and
  assert mode stays 0600; assert temp file is cleaned on failure (ENOSPC mock path
  exists: `tests/unit/test_io_faults.cpp` + `tests/mock/posix_io_mock.c`); assert no
  leftover `.ainiux-tmp.*` after success/failure.

### F7 — Child inherits all parent fds before `execve` (Low–Med)

- Evidence: `src/agent/process.cpp:692-708`: the child closes only the two pipe read
  ends; the executed program inherits every other open fd (chat/agent SQLite, config,
  sockets, media, lock dirs). A long-running or compromised command can read through
  those fds without path access, and inherited fds keep resources alive.
- Fix: covered by F4 fix item 2 (`close_range(3, ~0U, 0)` /
  `closefrom(3)` in the child after the dup2s, before `chdir`/`execve`). Keep the
  "no allocation between fork and execve" property (syscalls only).
- Tests: extend the F4 process test to assert the helper child cannot read a test fd
  duplicated into the parent (e.g., open a temp file with `O_CLOEXEC` unset in the
  parent, have the child attempt to read fd 3 via a python helper; expect failure).

### F8 — Redaction gaps: substring matching and URL userinfo (Low–Med)

- Evidence: `src/security/redact.cpp:7-19` replaces only the exact matched length:
  a key `xyz` inside `xyzzy` yields `[REDACTED]zy` (suffix leak; real keys are long,
  but dummy/4-char keys such as local-profile defaults make this reachable in
  transcripts). `src/http/http.cpp` emits `request.url` unredacted in
  `classify_curl_error` (line 109) and the cancellation/blocked messages (lines
  450-463); a custom base URL with embedded userinfo (`https://user:secret@host/`)
  leaks into errors and HTTP traces are only redacted against the configured secret
  set — the URL userinfo is not in it.
- Fix:
  1. In `redact_secrets`, when a match is immediately preceded/followed by an
     alphanumeric char (token context), extend the replaced span to the whole
     surrounding token (or at least redact the full match plus a guard char) so
     suffixes cannot leak; add a documented cap: skip secrets shorter than 4 chars
     for log redaction, or always redact surrounding token.
  2. In `http::perform` error paths, redact URL userinfo before composing messages
     (helper `redact_url_userinfo(url)`), or pass the redacted URL through
     `redact_secrets` with the userinfo component as a derived secret.
- Tests: `tests/unit/security/` — `redact_secrets("pre xyzzy post", {"xyz"})` yields
  no `xyz` fragment; URL-with-userinfo appears redacted in an `http` error-path unit
  test (existing `tests/unit/http/`).

### F9 — Guard/egress gaps in agent `run_command` (Low)

- Evidence (`src/agent/command_guard.cpp`): `rsync` not in the remote-shell deny list
  (lines 191-195); `git push` (non-force) allowed; `pkill`/`killall`/`kill` unguarded
  (can kill the ainiux process itself); `blkdiscard`/`fstrim`/`hdparm`/`gdisk` not in
  the disk-destroy list (lines 172-175); `git fetch`/`clone`/`remote add` (network
  ingress) unguarded.
- Fix (align with existing posture; all are cheap additions):
  - Add `rsync`, `lftp`, `curl`/`wget`/`aria2c` with output-to-file forms to Ask, or
    at minimum `rsync` to the remote-shell deny list; document `git push` as Ask
    (code egress).
  - Add `pkill`, `killall`, `kill` (except `kill -0`/no-op signals) to Ask.
  - Add `blkdiscard`, `fstrim`, `gdisk`, `sgdisk`, `hdparm` to the disk-destroy deny
    list alongside `dd`/`mkfs`/`shred`/`fdisk`/`parted`.
  - Add `git fetch`/`pull`/`clone`/`remote add`/`push` Ask (network side effects).
- Tests: extend `tests/unit/agent/` Guard tests for each new rule; assert existing
  safe forms (`git status`, `git diff --stat`, `ls`, `cat`) remain Allow.

### F10 — `apply_patch` Add File silently creates directories (Low)

- Evidence: `src/agent/tools.cpp:2451-2459` calls `fs::create_directories(parent)`
  unconditionally for Add File, after `validate_mutation_path(op.path, /*create_dirs=*/false,
  ...)` (line 2330) — so the interactive `ask_on_create_dirs` Guard used by
  `write_workspace_file` (lines 1741-1768) and the Plan-mode "no directory creation"
  rule (line 1419-1421) are bypassed. Plan mode can therefore create directories
  (files are still restricted to the planning-doc allowlist).
- Fix: route Add File parent creation through the same approval path as
  `write_workspace_file` (`ask_on_create_dirs` when `permission_controls_` is off),
  and reject missing parents in Plan mode (mirror `validate_mutation_path` with
  `create_dirs=false` semantics at apply time).
- Tests: `tests/unit/agent/` — apply_patch Add File with missing parent in a Plan
  policy session returns `UnsupportedFeature`; interactive session with no approval
  callback denies; with callback Allow creates the dir once.

### F11 — `validate_header` accepts CR/LF (Low)

- Evidence: `src/provider/provider.cpp:289-295` checks only for a colon. libcurl
  rejects CR/LF in header strings today, so this is defense-in-depth, not an active
  injection. Headers originate from CLI/config (user-controlled), not from the model.
- Fix: reject `\r`/`\n`/NUL in `validate_header` with a clear message.
- Tests: `tests/unit/provider/` — header with `\r\n` fails validation.

### F12 — TOCTOU between path validation and write (Low)

- Evidence: `resolve_writable_path`/`ensure_under_workspace` validate, then
  `write_bytes_atomic` opens the target by name (`src/agent/tools.cpp:1628`). A local
  attacker with workspace write access could swap a parent directory for a symlink
  between validation and open. The external-path flow already re-resolves and
  compares (`ensure_approved_external_path_unchanged`, lines 541-557).
- Fix (cheap hardening, optional): re-resolve `weakly_canonical(absolute)` and compare
  to the validated path immediately before the rename in `write_bytes_atomic` (same
  pattern), and use `O_NOFOLLOW` on the temp open. Full fix would be `openat2(...,
  RESOLVE_BENEATH)`; note as future work — acceptable given the documented threat
  model (workspace is user-owned).

### F13 — `--key-file` permission check (Info)

- Evidence: `src/provider/provider.cpp` `read_file` (lines 241-260) reads the key file
  with no mode warning.
- Fix: optional — warn on `stderr` when the key file is group/world-readable (mode &
  0o077), matching the existing `--key` warning style. Do not refuse (some setups use
  `--key-file -`).
- Tests: unit test for the warning helper only.

---

## 3. Recommended remediation order

1. **F1** (one function + chmod; highest confidentiality impact, smallest change).
2. **F4+F7** (process runner: close read ends on timeout, hard loop bound,
   `close_range` in child) — fixes a hang and the fd-inheritance leak together.
3. **F5** (JSON depth cap; one counter) — remote-triggerable crash.
4. **F6** (atomic write mode preservation + O_EXCL temp + fsync) — silent privilege
   widening on overwrite; also benefits external approved writes.
5. **F2+F9** (Guard additions) — pure rule-table changes plus unit tests.
6. **F3** (archive extraction Ask + tar hardening flags).
7. **F10** (apply_patch directory-creation parity with write_file).
8. **F8** (redaction token-context + URL userinfo).
9. **F11, F12, F13** (low-priority hardening).

Each fix lands with its unit tests; run `make test-unit`, then
`make test-integration-smoke` for agent/CLI regressions, and `make test-unit-faults`
for the F6 I/O failure paths. No change here alters provider wire formats, the agent
system prompt, or CLI stdout semantics.

## 4. Not verified / out of scope

- Full leak/ASan sweep and Valgrind suites are not part of this plan (project
  slow-test policy).
- Native Anthropic adapter, server mode, web UI: not reviewed (not implemented).
- The built-in agent system prompt was not audited (user-directed prompt-optimization
  pass is separate).
- Real-provider live tests (credit endpoints, reasoning probing) were not run;
  findings F1-F13 are code-level.
