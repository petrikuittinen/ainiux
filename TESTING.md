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
| `make test-full` | Units, fault tests, and comprehensive integration; Windows also runs native SQLite/ConPTY parity paths |
| `make test-unit` | In-process `test_runner` plus the fast preserved-config migration check |
| `make test-unit-faults` | Fault tests only |
| `make test-integration-smoke` | Small Chat/Responses/agent mock smoke |
| `make test-integration` | Code-index, mock-server, and SQLite TUI end-to-end scripts |
| `make test-integration-sqlite` | SQLite TUI persistence (POSIX PTY or native Windows ConPTY reopen) |
| `make test-windows-conpty` | Native Windows ConPTY terminal/mode-cycle smoke |
| `make package-windows` | Native portable ZIP plus SHA-256 checksum |
| `make test-sanitize` | AddressSanitizer/UBSan build of the full `make test` path |
| `make test-leak` | Valgrind on `test_runner`, `test_io_faults`, and `ainiux --version` |

Code-index unit coverage includes all definition scanners, static importance across C++, Python, TypeScript, Java/C#, Rust, and Go, eager/lazy lexical ranking equivalence, deterministic tie-breaking, cancellable owned SQLite file/symbol/totals queries, 80%-of-cores worker selection and work-item bounding, ordered progress phases, read-only missing/completed/corrupt probing, database-free live discovery, indexed/live glob and text-search behavior, strict indexing-disabled runtime/tool behavior, in-session `/index-code` enablement, instant missing-index readiness, compact totals formatting, display-only `/show-index` refresh reports, task-end incremental full-tree refresh semantics, schema 1–3 migration, graph-table removal, compaction, and cancellation preservation.

Agent-loop/runtime unit coverage includes native/XML prepared-call accounting, strict top-level Boolean `ok` result normalization, failed-call totals across invalid arguments, denials, cancellation, malformed results and early exits, zero-tool runs, stable one-shot metrics formatting, non-blocking supersession and cleanup of background metadata jobs, catalog-only automatic context fallback without `/models`, automatic credit timeout fallback, ordered Agent preparation phases, truthful Preparing/Unavailable chrome, and display-only retry notices. Agent compaction coverage includes strict strategy/config/slash parsing (including one-shot `/compact all`), the universal 75% threshold, chronological message/tool timelines, display-role and duplicate-row exclusion, protected head and bounded whole-item tail partitioning, repeated-summary carry-forward, smart escalation, summary budgets/reasoning selection, persistent lifecycle notices, the dedicated animated `Agent compacting` state, elapsed-time success and explicit no-op/failure formatting, hidden raw checkpoint replay, the `fast` no-model guarantee, active-API summary injection, and transactional preservation on summary failure. Terminal frame coverage verifies that identical chat/agent/editor frames emit no output, partial changes emit only affected rows without cursor visibility toggles, cursor-only updates preserve visibility, and resize invalidates the retained frame.

Manual CI (`.github/workflows/ci.yml`) retains the Ubuntu `make test-full` plus
Valgrind gate. Its UCRT64 jobs build native Windows, run unit/process-tree and
fault tests, smoke and comprehensive mock-provider/index integration, native SQLite
integration, the ConPTY harness, a Clang ASan/UBSan unit pass, and portable ZIP
packaging.

## Layout

- `tests/unit/` — module-oriented C++ unit tests. `test_runner` dispatches `run_all()` from each module directory.
- `tests/unit/mcp/` — MCP registry, HTTP/stdio client against `tests/mock_server/mcp_mock.py`, tool envelope, prepare-cancel regression.
- `build/test_io_faults` — separate binary for slower or environment-dependent checks.
- `tests/integration/test_mock_smoke.sh` — fast protocol-isolated Chat, Responses,
  provider-fault (empty/malformed/non-UTF-8), no-input CLI, and one-shot-agent transport smoke.
- `tests/integration/test_mock_server.sh` — comprehensive CLI, REPL, benchmark/grade, fetch, config, attachments, TUI, SQLite, and native-tool security-review coverage against one shared local mock API.
- `tests/integration/test_code_index.sh` — project-local refresh, ignore/skip behavior, Markdown output, stale snapshots, and CLI isolation.
- `tests/integration/test_sqlite_persistence.sh` — SQLite-backed TUI persistence via `tui_sqlite_driver.py`.
- `tests/integration/tui_startup_selection_driver.py` — isolated PTY coverage for bare-offline chat and one-/multiple-model startup discovery.
- `tests/integration/clipboard_driver.py` — focused fake-helper/OSC 52 PTY coverage for editor and shared chat/agent input clipboard behavior.
- `tests/mock_server/` — Python HTTP mocks for OpenAI-compatible APIs and slow responses. The integration scripts launch protocol-isolated Chat Completions and Responses instances.
- `tests/mock/` — POSIX `LD_PRELOAD` shim for disk-full simulation.
- `tests/fixtures/subprocess_fixture.cpp` — native Unicode argv/cwd/environment,
  stdin/stdout/stderr, nonzero/exception exit, output-cap, timeout, cancellation,
  and descendant-tree fixture.
- `tests/windows/conpty_harness.cpp` — native pseudo-console startup, VT mouse,
  resize, PowerShell-job cancellation, chat→agent→editor→chat cycling,
  quit, alternate-screen restoration, and isolated project/SQLite profile
  creation and reopen.

The comprehensive mock path intentionally retains end-to-end surface coverage,
but detailed format/error matrices belong in unit tests whenever no process,
transport, filesystem, or PTY boundary is involved.

## Coverage overview

**Strong unit coverage**

- CLI parsing and options
- Full editor-language code-index definition parity: Markdown, Python, C/C++, C#, Java, JavaScript/TypeScript and React/module endings, HTML/HTML-only, CSS, XML, JSON, Bash, PHP, Perl, Ruby, Rust, Go, PowerShell, Assembly, SQL, TOML, YAML, and INI; plus embedded HTML scanning, qualification, ranges, documentation, false-positive masking, static importance, deterministic lexical ranking, incremental refresh, line totals, stale detection, schema migration, graph-storage removal, clearing, and Markdown reports.
- Provider registry, every registered reasoning request protocol, ordinary response parsing, and native-tool readable-reasoning extraction for Chat/Responses streams, summaries, details, think tags, and encrypted-state omission
- Main configuration plus `models.conf` parsing, embedded fallback availability outside the source directory, layering, disabling, regex validation, context-window fallback precedence, and final-component/case-insensitive family matching
- `images.conf` parsing, id overlay, default image model, `openai_images` / `replicate_predictions` / `fal_queue` / `gemini_interactions` mapping, and catalog-driven size/quality/format mapping for `ainiux image`
- HTML/Markdown/input/output conversion, including punctuation-adjacent emphasis,
  literal intraword underscores, exact inline-code delimiter runs, blockquote
  text preservation across editor and TUI styling, and pretty table layout
  (Unicode box / padded GFM, streaming open tables, fenced-code exemption,
  display-width padding, and index/benchmark report generators)
- Editor piece table, panels, selection, and file I/O
- Chat JSON save/load, including named/numeric/Auto per-thread reasoning
- Canonical `on`/`off` config and settings, model-aware reasoning-off mapping, theme-off state, and chat/editor trace-shortcut separation
- SQLite store round-trip, editor model-selection app state, listing, soft delete, corrupt DB, and missing-thread handling
- Runtime cancellation and event delivery
- Shared subprocess Unicode/LF normalization, malformed-output repair, bounded
  capture, nonzero status, timeout/cancel, descendant termination, Windows
  exception codes, and repeated Windows handle-count stability
- Windows path syntax, identity containment for existing/new targets,
  symlink/reparse detection, protected directory/file creation, Unicode long
  paths, and atomic replacement
- Security redaction helpers
- Agent Act/Plan prompt selection, CLI/TUI switching, typed mutation policy with atomic planning-patch preflight, structured in-place Thinking/tool activity, Unicode-safe redacted preview clipping, project preview settings, persisted preview restore, and display-role exclusion from provider projections
- Native Chat/Responses tool definition, call, streamed-fragment/index validation, multi-item text, continuation, and result serialization
- Security-review read tools, shared tool-argument pipeline (empty/`{}`, fenced JSON, single-object extraction, one-pass repair including unquoted path-like globs, schema coercion, case/snake-camel name repair, XML channel parse), exact-path, directory-root, and wildcard text-search filtering (combinable), conservative omitted-regex alternation inference, `pattern`/`query` compatibility, Agent-mode `read` batch `items` preference/order, 1–100 item validation, per-item/default and aggregate byte limits, line-numbered hashes/ranges, partial results, native serialization, compact display, and prompt guidance, agent loop history hygiene / transport-retry classification / identical-call and consecutive-failure limits / native→XML downgrade, trusted prompt layering (master foundation + security task layer; agent master+coding + native/XML static appendices; seed_agent_conversation), CLI/dispatch for headless `run`/`--run` and interactive `agent`/`--agent` (goal required, no system override, separate agent log dir), explicit exact batch coverage, native final submission, conservative normalization of omitted/empty optional finding metadata, bounded finalization of over-exploring workers, safe single-object extraction from JSON preambles/fences, ambiguous/malformed response rejection, index fingerprint checks, ignored/traversal/symlink rejection, secret redaction, command/helper allowlisting, Markdown-field escaping, deterministic report rendering, and concurrent secure JSONL diagnostic logging with mid-run flush of the live `.partial` path plus finalization/retention
- Unicode, numeric, and malformed-input edge cases
- Control-server WUI exact asset routing, API-auth separation, immutable/no-store
  caching, CSP and browser hardening headers, TUI-derived light/dark palettes,
  responsive/reduced-motion markers, strict browser query-path decoding, and
  managed-secret stability/private permissions, browser-server CLI aliases and
  URL reporting, persistent `localStorage` authentication, reconnect markers,
  and static rejection of external URLs, raw-HTML sinks, cookies, and URL tokens

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
- Real loopback control-server smoke through `scripts/test-control-server.sh`,
  including public embedded WUI boot assets, versioned caching/CSP, API scope,
  jobs/SSE, MCP, revision-safe chat/workspace routes, and Host/Origin rejection

**Fault injection**

- Slow HTTP response and chunked-body timeouts (`slow_http_mock.py`)
- Connect timeout to an unreachable TEST-NET address
- Read-only file and directory permission failures (`chmod`)
- Simulated `ENOSPC` on tagged paths via `posix_io_mock.so`
- Native Windows sharing violations for chat/editor reads and atomic writes,
  plus one-shot native disk-full/short-write injection with target and temporary
  cleanup checks (the POSIX path retains its `LD_PRELOAD` ENOSPC shim)

## Mocks

### `tests/mock_server/openai_mock.py`

Local OpenAI-compatible handler used by both mock scripts. They launch separate
`--api chat` and `--api responses` server instances so endpoint and schema
coverage cannot fall through to the other protocol. The handler supports one-,
multiple-, and empty-result model-list endpoints, chat completions, Responses,
streaming, request-local delayed streams, exact reasoning fields, attachments,
images, HTML fetch fixtures, strict benchmark grading, one-shot agent tools,
the security-review native tool/coordinator loop, and empty/malformed/non-UTF-8
provider response faults.

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
- Automated real-browser interaction and pixel/layout comparison are not part of
  the dependency-free default suite. The WUI has static security/responsive
  coverage plus real-listener curl coverage; hands-on browser/assistive-technology
  checks remain release acceptance. Resize and terminal restoration have Windows
  ConPTY smoke coverage, but real Windows Terminal/conhost acceptance remains a
  release checklist item.
- Native Windows clipboard save/restore integration is opt-in/manual so tests do
  not overwrite a developer's clipboard. A mocked Win32 boundary unit-covers
  busy retries/cancellation, UTF-16 and line-ending conversion, malformed data,
  size limits, and global-memory ownership; native desktop ownership remains
  part of acceptance.
- Linux uses Valgrind for leak checking. Windows CI uses the UCRT64 Clang
  ASan/UBSan unit path plus repeated process handle counts; it does not run
  Valgrind or the POSIX preload shim.
