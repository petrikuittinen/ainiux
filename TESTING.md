# Testing ainiux

This document describes how to run the automated test suite, what it covers, and which mocks are used.

## Quick start

```sh
make test
```

This is the fast development gate: the in-process unit runner plus a small
OpenAI-mock smoke covering streamed Chat Completions, Responses, and a native
tool-calling one-shot agent round.

Run the comprehensive path explicitly:

```sh
make test-full
```

Useful targets:

| Target | What it runs |
|--------|----------------|
| `make test` | In-process units plus the small mock smoke |
| `make test-full` | Units, fault injection, and comprehensive integration |
| `make test-unit` | In-process `test_runner` only |
| `make test-unit-faults` | Fault tests only |
| `make test-integration-smoke` | Small Chat/Responses/agent mock smoke |
| `make test-integration` | Code-index, mock-server, and SQLite TUI end-to-end scripts |
| `make test-integration-sqlite` | SQLite TUI persistence only |
| `make test-sanitize` | AddressSanitizer/UBSan build of the full `make test` path |
| `make test-leak` | Valgrind on `test_runner`, `test_io_faults`, and `ainiux --version` |

Code-index unit coverage includes all definition scanners, static importance across C++, Python, TypeScript, Java/C#, Rust, and Go, eager/lazy lexical ranking equivalence, deterministic tie-breaking, cancellable owned SQLite file/symbol/totals queries, 80%-of-cores worker selection and work-item bounding, ordered progress phases, read-only missing/completed/corrupt probing, database-free live discovery, indexed/live glob and text-search behavior, strict indexing-disabled runtime/tool behavior, in-session `/index-code` enablement, instant missing-index readiness, compact totals formatting, display-only `/show-index` refresh reports, task-end incremental full-tree refresh semantics, schema 1–3 migration, graph-table removal, compaction, and cancellation preservation.

Agent-loop/runtime unit coverage includes native/XML prepared-call accounting, strict top-level Boolean `ok` result normalization, failed-call totals across invalid arguments, denials, cancellation, malformed results and early exits, zero-tool runs, stable one-shot metrics formatting, non-blocking supersession and cleanup of background metadata jobs, catalog-only automatic context fallback without `/models`, automatic credit timeout fallback, ordered Agent preparation phases, truthful Preparing/Unavailable chrome, and display-only retry notices. Agent compaction coverage includes strict strategy/config/slash parsing, the universal 75% threshold, chronological message/tool timelines, display-role and duplicate-row exclusion, protected head and bounded whole-item tail partitioning, repeated-summary carry-forward, smart escalation, summary budgets/reasoning selection, persistent lifecycle notices, the dedicated animated `Agent compacting` state, elapsed-time success and explicit no-op/failure formatting, hidden raw checkpoint replay, the `fast` no-model guarantee, active-API summary injection, and transactional preservation on summary failure. Terminal frame coverage verifies that identical chat/agent/editor frames emit no output, partial changes emit only affected rows without cursor visibility toggles, cursor-only updates preserve visibility, and resize invalidates the retained frame.

Manual CI (`.github/workflows/ci.yml`) runs `make test-full` and
`make test-leak` on Ubuntu with libcurl, libsqlite3, Python 3, and Valgrind
installed.

## Layout

- `tests/unit/` — module-oriented C++ unit tests. `test_runner` dispatches `run_all()` from each module directory.
- `build/test_io_faults` — separate binary for slower or environment-dependent checks.
- `tests/integration/test_mock_smoke.sh` — fast Chat, Responses, and one-shot-agent transport smoke.
- `tests/integration/test_mock_server.sh` — comprehensive CLI, REPL, benchmark/grade, fetch, config, attachments, TUI, SQLite, and native-tool security-review coverage against one shared local mock API.
- `tests/integration/test_code_index.sh` — project-local refresh, ignore/skip behavior, Markdown output, stale snapshots, and CLI isolation.
- `tests/integration/test_sqlite_persistence.sh` — SQLite-backed TUI persistence via `tui_sqlite_driver.py`.
- `tests/integration/tui_startup_selection_driver.py` — isolated PTY coverage for bare-offline chat and one-/multiple-model startup discovery.
- `tests/integration/clipboard_driver.py` — focused fake-helper/OSC 52 PTY coverage for editor and shared chat/agent input clipboard behavior.
- `tests/mock_server/` — Python HTTP mocks for OpenAI-compatible APIs and slow responses.
- `tests/mock/` — POSIX `LD_PRELOAD` shim for disk-full simulation.

The comprehensive mock path intentionally retains end-to-end surface coverage,
but detailed format/error matrices belong in unit tests whenever no process,
transport, filesystem, or PTY boundary is involved.

## Coverage overview

**Strong unit coverage**

- CLI parsing and options
- Full editor-language code-index definition parity: Markdown, Python, C/C++, C#, Java, JavaScript/TypeScript and React/module endings, HTML/HTML-only, CSS, XML, JSON, Bash, PHP, Perl, Ruby, Rust, Go, PowerShell, Assembly, SQL, TOML, YAML, and INI; plus embedded HTML scanning, qualification, ranges, documentation, false-positive masking, static importance, deterministic lexical ranking, incremental refresh, line totals, stale detection, schema migration, graph-storage removal, clearing, and Markdown reports.
- Provider registry, every registered reasoning request protocol, ordinary response parsing, and native-tool readable-reasoning extraction for Chat/Responses streams, summaries, details, think tags, and encrypted-state omission
- Main configuration plus `models.conf` parsing, embedded fallback availability outside the source directory, layering, disabling, regex validation, context-window fallback precedence, and final-component/case-insensitive family matching
- HTML/Markdown/input/output conversion, including punctuation-adjacent emphasis,
  literal intraword underscores, exact inline-code delimiter runs, and blockquote
  text preservation across editor and TUI styling
- Editor piece table, panels, selection, and file I/O
- Chat JSON save/load, including named/numeric/Auto per-thread reasoning
- SQLite store round-trip, editor model-selection app state, listing, soft delete, corrupt DB, and missing-thread handling
- Runtime cancellation and event delivery
- Security redaction helpers
- Agent Act/Plan prompt selection, CLI/TUI switching, typed mutation policy with atomic planning-patch preflight, structured in-place Thinking/tool activity, Unicode-safe redacted preview clipping, project preview settings, persisted preview restore, and display-role exclusion from provider projections
- Native Chat/Responses tool definition, call, streamed-fragment/index validation, multi-item text, continuation, and result serialization
- Security-review read tools, shared tool-argument pipeline (empty/`{}`, fenced JSON, single-object extraction, one-pass repair including unquoted path-like globs, schema coercion, case/snake-camel name repair, XML channel parse), exact-path and wildcard text-search filtering, conservative omitted-regex alternation inference, `pattern`/`query` compatibility, Agent-mode `read_many` preference/order, 1–100 item validation, per-item/default and aggregate byte limits, line-numbered hashes/ranges, partial results, native serialization, compact display, and prompt guidance, agent loop history hygiene / transport-retry classification / identical-call and consecutive-failure limits / native→XML downgrade, trusted prompt layering (master foundation + security task layer; agent master+coding + native/XML static appendices; seed_agent_conversation), CLI/dispatch for headless `run`/`--run` and interactive `agent`/`--agent` (goal required, no system override, separate agent log dir), explicit exact batch coverage, native final submission, conservative normalization of omitted/empty optional finding metadata, bounded finalization of over-exploring workers, safe single-object extraction from JSON preambles/fences, ambiguous/malformed response rejection, index fingerprint checks, ignored/traversal/symlink rejection, secret redaction, command/helper allowlisting, Markdown-field escaping, deterministic report rendering, and concurrent secure JSONL diagnostic logging with mid-run flush of the live `.partial` path plus finalization/retention
- Unicode, numeric, and malformed-input edge cases

**Integration coverage**

- Project-local code-index creation/printing/clearing, full editor-language discovery and reporting, JavaScript and TypeScript module/JSX endings, binary skips, ignore rules, stale reporting, output files, and option rejection
- Chat Completions and Responses API against `openai_mock.py`, including strict exact reasoning shapes and unlisted-value CLI/REPL warnings
- Headless security review with an incremental project index, native multi-round `read_file`, schema-defined `submit_security_review`, explicit expected coverage, opaque reasoning continuation, untrusted `AGENTS.md` data, coordinator output, per-run request/response/tool/validation logs, and clean stdout/stderr separation
- Streaming, JSON/NDJSON output, thinking-trace redaction
- REPL, benchmark modes, action-balanced safety ratings, configurable judge grading, URL fetch safety, attachments, images
- Editor/chat shared compact provider/model labels plus agent's model-only 80-column status line (aliases, custom URLs, model paths, Unicode truncation, reasoning, exact whitespace), separate ready/thinking/working activity text with live elapsed and completed-task timing, colored selector panels, explicit-provider startup discovery, one-model auto-selection, multiple-model selection, and non-modal bare-offline startup
- Per-model `/v1/models` context-window refresh across chat, editor, and agent; sticky CLI/`/context` overrides; and token-only usage when model metadata has no window
- Agent compact tool rows with independent execution durations, Guard-wait subtraction, failed-tool timing, persisted timed-row replay, and unchanged whole-task completion timing
- Agent permission parsing/persistence and Confirm/Smart/Yolo native-path/command policy, including external exact-path tools, hard denials, permission border/command parsing, shell-free `command -v` lookup, and quoted-literal versus unquoted-substitution handling
- OpenRouter routing-session serialization/stability boundaries, OpenRouter and DeepSeek credit-response schema parsing, currency formatting, provider endpoint registration, and Agent border placement
- TUI insert/attach/fetch driver
- SQLite TUI workflows: `/new`, autosave/reload, `/list`, `/provider`, `/remove`, stale `last_thread_id`, corrupt database, image persistence across restart, `/cleanup`, and read-only expired-media threads

**Fault injection**

- Slow HTTP response and chunked-body timeouts (`slow_http_mock.py`)
- Connect timeout to an unreachable TEST-NET address
- Read-only file and directory permission failures (`chmod`)
- Simulated `ENOSPC` on tagged paths via `posix_io_mock.so`

## Mocks

### `tests/mock_server/openai_mock.py`

Local OpenAI-compatible server used by both mock scripts. Supports one-,
multiple-, and empty-result model-list endpoints, chat completions, Responses,
streaming, request-local delayed streams, exact reasoning fields, attachments,
images, HTML fetch fixtures, strict benchmark grading, one-shot agent tools,
and the security-review native tool/coordinator loop.

### `tests/mock_server/slow_http_mock.py`

Used by `build/test_io_faults` for real local timeout tests:

- `/delay/N` — hold the response open
- `/slow-body` — slow chunked body
- `/health` — readiness probe

### `tests/mock/posix_io_mock.so`

Built by the Makefile and preloaded for ENOSPC tests:

```sh
AINIUX_MOCK_ENOSPC=1 LD_PRELOAD=build/posix_io_mock.so build/test_io_faults --enospc
```

Write attempts to paths containing `mock-enospc` fail with `ENOSPC`. Optional `AINIUX_MOCK_EACCES=1` blocks writes to `mock-eacces` paths.

### `tests/integration/tui_sqlite_driver.py`

PTY driver for TUI commands. Uses an isolated `HOME` so the database is created at `$HOME/.ainiux/ainiux.db` and verified with Python `sqlite3`.
It covers managed-image restart/expiration and canonical Markdown attachment replay: small inline Markdown, one-time HTML conversion to file-backed `.md`, same-process follow-ups, source changes after import, and restored-thread requests. It also corrupts a saved thread's model metadata while a different local endpoint is active, then verifies the visible setup warning, blocked send, forced provider/model repair flow, persisted repair, and successful follow-up request.

### `tests/integration/tui_startup_selection_driver.py`

PTY driver for startup selection policy. It verifies that bare chat opens without a provider/model modal, explains `/list` and setup, blocks sending, and still opens the thread library. With an explicit provider it verifies sole-model auto-selection and the shared selector for multiple results. `editor_buffers_driver.py` covers the equivalent bare, single-result, and multiple-result editor cases.

## Known gaps

- No automated tests against real OpenAI, LM Studio, or other production providers (mock-only CI).
- Valgrind does not cover the ENOSPC `LD_PRELOAD` path or the full integration shell scripts.
- Interactive TUI resize, long-running stress, and browser/web mode are not implemented yet and are not covered.
- Sanitizer builds are local/optional; CI currently uses Valgrind for leak checking on the unit/fault binaries.
