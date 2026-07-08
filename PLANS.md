# PLANS.md

Project: `pkchat`

This file is the implementation roadmap and execution-plan template for coding agents. Work from the earliest incomplete milestone unless the user explicitly asks for something else.

## Product goal

Create the best practical command-line, terminal, local server, and future local-web chat client for OpenAI and OpenAI-compatible APIs.

`pkchat` should be:

- Fast and portable.
- Excellent in scripts.
- Useful interactively.
- Careful with credentials.
- Clear when errors happen.
- Robust with streaming output.
- Unicode-aware.
- Provider-adapter/profile based, not hard-coded to one API dialect.
- Friendly to local endpoints, especially LM Studio and llama.cpp-style servers.
- Responsive in full-screen mode and future server/web modes even while waiting for an endpoint, streaming, saving/loading chats, or processing files.
- Free of memory leaks.

## Deferred product work

The CLI, streaming, persistence, provider architecture, runtime/job layer, TUI foundation, and first attachment layer now exist. Keep the following work outside the v0.6 configuration milestone unless it is required to integrate configuration safely:

- Autonomous local agent mode.
- Full rich Markdown rendering in the TUI.
- PDF and DOCX input/output conversion.
- Clipboard integration.
- Complex terminal key protocols.
- Browser automation.
- Plugin system.

The browser-based local web UI is postponed. A local OpenAI-compatible server mode may come first, because it can expose `pkchat` conversions and later chained chat workflows to other OpenAI-compatible clients while reusing the same transport/runtime/security code. Autonomous local agent mode remains separate and must still have its own sandbox/approval design before any tool execution is added.

## High-level milestones

```text
v0.0  Repository skeleton, build system, and leak-check infrastructure
v0.1  Script-friendly CLI for Chat Completions plus LM Studio profile
v0.2  Simple interactive REPL and chat persistence
v0.3  Runtime/job layer and non-blocking full-screen TUI foundation
v0.4  Provider adapters, Responses API, LM Studio refinement, and compatibility profiles
v0.5  Context management, attachments, and safe URL fetching
v0.6  System and user TOML-alike configuration files
v0.7  Benchmark mode
v0.8  AI-assisted editor
v0.9  Benchmark cutoff mode, codebase refactor, and TUI/CLI polish
v0.90 Local OpenAI-compatible server mode; browser web UI postponed
v1.0  Local agent mode with sandbox/approval design
v1.1  Image generation from CLI, REPL, TUI, and future server/web surfaces
```

Each milestone should leave the program usable. Do not create a long-lived pile of half-wired features.

## Current baseline

Implementation status (2026-07-08): `pkchat` is at v0.91. Active development targets the v0.9 milestone (benchmark cutoff mode, codebase refactor, and TUI/CLI polish) before local OpenAI-compatible server mode. The repository has the scriptable CLI, built-in provider registry and aliases, Chat Completions, text-only Responses API support, streaming SSE, provider-specific reasoning/thinking request compatibility, credential lookup, JSON chat import/export, SQLite-backed TUI chat persistence, cancellable runtime jobs, REPL, full-screen TUI foundation, editor with multiple file buffers, selection, copy/cut/paste across buffers, grapheme-aware Unicode navigation and terminal cell-width rendering, and AI continue/auto-write (`Ctrl+Space`), request-only context policies, context-use estimates, bounded text/HTML/Markdown input, JPEG/PNG/GIF image input for Chat Completions, safe URL fetching, Markdown output conversion, automatic v0.6 system/user TOML-alike configuration loading, and concurrent JSONL benchmark execution. `--provider none` supports local conversion and editor workflows without a model endpoint. Standalone `--editor` accepts an optional startup path after a provider shortcut or base URL, creates a missing file before editing, prompts to save scratch buffers on quit, and asks for overwrite confirmation when saving to an existing path. Local `lmstudio`, `ollama`, `vllm`, and loopback base URLs auto-select the first `/v1/models` entry when `--model` is omitted.

Runtime defaults live in `cli::Options`, provider defaults live in `src/provider/`, and API keys are resolved while building the provider request context. Automatic system and user config files map into a base `cli::Options`, after which `main.cpp` parses CLI arguments over that base. `--no-config` can bypass the automatic user file while retaining system configuration, and `--debug` reports configuration discovery on `stderr`.

Implementation note (2026-06-30, v0.86): TUI panels for thread picker, help, and confirmations use dedicated colors and box-drawing separators; provider labels use registry aliases in status lines; thinking and streaming show single-character indicators. Editor mode embeds `docs/editor_help.md`, installable to `share/pkchat/`, and toggles read-only help via `Esc /help`.

Implementation note (2026-07-05, v0.88): Web search is available through `--search QUERY`, REPL/TUI `/search QUERY`, and editor `Esc /search QUERY`. Providers include Tavily, Firecrawl, Exa, Searxng, with DuckDuckGo Instant Answer and Google HTML fallbacks when API keys are absent. `MAXIMUM_WEB_SEARCH_RESULTS` defaults to 3 via config, CLI, or environment.

Implementation note (2026-07-06, v0.89): `--thinking` and `--thinking-budget` now pass through a provider compatibility layer for OpenAI Chat/Responses, OpenRouter, Gemini, Anthropic Claude through its OpenAI-compatible endpoint, Kimi K2.x, Qwen/DashScope, DeepSeek V4, GLM-5.2/Z.AI, xAI, and custom/local OpenAI-compatible fallback profiles. Standalone editor mode supports multiple file buffers, `/new`, `/list`, `/close`, and matching `Ctrl+N`, `Ctrl+L`, and `Ctrl+W` shortcuts. Native Anthropic Messages support, live model capability probing, and preservation of opaque reasoning state for future agentic tool loops remain open.

Implementation note (2026-07-08, v0.90): Chat and editor keyboard shortcuts were unified (`Ctrl+Z`/`Ctrl+U` undo, `Ctrl+Y` redo, `Ctrl+Home`/`Ctrl+End` buffer bounds, `PageUp`/`PageDown` in-input paging). Chat mode adds `Ctrl+R` regenerate, `Ctrl+B`/`Ctrl+D` history scroll, and `Alt+Home`/`Alt+End` jump; `Ctrl+D` quit-empty and `Ctrl+P` pop were removed. v0.9 focuses on benchmark cutoff mode, codebase refactor, and broader TUI/CLI polish before local server mode in v0.90.

Implementation note (2026-07-04, v0.87): `Ctrl+A` selects the entire editor or chat input buffer. `Home`/`End` move to the current line; `Alt+Home`/`Alt+End` jump to buffer start/end. `Ctrl+E` is unused in standalone editor mode. Chat TUI `Ctrl+E` copies the last user or assistant message into the input for editing, with `Enter` to save and a bare `Esc` to cancel; escape-sequence parsing during edit no longer treats arrow keys as cancel.

Implementation note (2026-06-29, v0.85): Per-thread model settings add CLI sampling/thinking flags, repeatable `[Model-setting]` presets in `config/pkchat.conf`, TUI `/setting`, `/system`, and `/clone`, provider request fields for `top_k`, `min_p`, penalties, and `thinking_budget`, and SQLite `settings_json` persistence. Unset thread overrides serialize as JSON `null` and use provider defaults; `/setting NAME=NULL` clears an override. `thinking_budget` accepts token counts or verbal labels.

Implementation note (2026-06-28, v0.84): Large monolithic sources were split into `src/app/`, `src/editor/`, and `src/tui/` modules; the benchmark built-in dataset was split by category. SQLite integration tests, Valgrind in CI, and `TESTING.md` were added. Streamed editor AI assist no longer leaks a trailing `</content>` tag across chunk boundaries.

Implementation note (2026-06-28, v0.83): Version metadata moved to `src/version/version.cpp`. Unit tests were split into module directories under `tests/unit/` with a thin `test_runner` driver. Coverage was expanded with Unicode, numeric, file I/O, and network edge cases. Mock helpers were added for slow HTTP timeouts, simulated disk-full (`ENOSPC` via `LD_PRELOAD`), and permission-denied read-only paths. See `README.md` Testing and `docs/decisions.md` for the layout and targets (`make test-unit`, `make test-unit-faults`, `make test-integration`).

## Execution-plan template for agents

For any non-trivial coding task, create or update a short plan using this structure:

```md
## Task: <short name>

### Goal
<What user-visible behavior will exist after this task?>

### Files likely to change
- <file>
- <file>

### Design notes
- <important architecture choices>
- <provider/security/Unicode/error-handling/concurrency/memory implications>

### Steps
- [ ] Inspect existing code and docs.
- [ ] Add or update tests/fixtures.
- [ ] Implement the smallest coherent change.
- [ ] Run relevant build/tests.
- [ ] Run sanitizer/leak checks where practical for touched paths.
- [ ] Update README/TODO/PLANS/docs if behavior changed.

### Acceptance criteria
- [ ] <observable behavior>
- [ ] <test or verification command>
- [ ] <error handling/security/responsiveness requirement>
- [ ] <memory ownership/leak-check requirement>

### Verification performed
- `<command>`: <result>
```

Keep plans short and concrete. The implementation matters more than the plan.

---

# v0.0 - Repository skeleton, build system, and leak-check infrastructure

## Goal

Create a minimal but clean project skeleton that builds a `pkchat` binary, gives future agents obvious places to put code, and establishes from the beginning that memory leaks are not acceptable.

## Tasks

- [ ] Create `README.md` with mission, build instructions, and first examples.
- [ ] Create `TODO.md` with active near-term tasks only.
- [ ] Create `docs/decisions.md`.
- [ ] Create `docs/security.md` stub.
- [ ] Create `docs/api-compatibility.md` stub.
- [ ] Create `docs/web-mode.md` stub.
- [ ] Create `include/pkchat/` for public/internal headers if needed.
- [ ] Create `src/` module directories:
  - [ ] `src/cli/`
  - [ ] `src/config/`
  - [ ] `src/provider/`
  - [ ] `src/http/`
  - [ ] `src/runtime/`
  - [ ] `src/chat/`
  - [ ] `src/tui/`
  - [ ] `src/web/`
  - [ ] `src/unicode/`
  - [ ] `src/security/`
  - [ ] `src/benchmark/`
  - [ ] `src/tools/`
- [ ] Create `tests/` directories:
  - [ ] `tests/unit/`
  - [ ] `tests/integration/`
  - [ ] `tests/fixtures/`
  - [ ] `tests/mock_server/`
- [ ] Add a `Makefile` with at least:
  - [ ] `make`
  - [ ] `make clean`
  - [ ] `make test`
  - [ ] `make sanitize`
  - [ ] `make test-sanitize`
  - [ ] `make leak-check`
  - [ ] `make test-leak`
- [ ] Add compiler warnings:
  - [ ] `-Wall`
  - [ ] `-Wextra`
  - [ ] `-Wpedantic`
- [ ] Add AddressSanitizer/LeakSanitizer flags where supported.
- [ ] Add a documented fallback leak-check command using Valgrind or an equivalent tool where available.
- [ ] Make `pkchat --version` and `pkchat --help` work.

## Memory criteria

- [ ] The first skeleton program exits without leaks under available leak-check tooling.
- [ ] Any wrapper type introduced in v0.0 has deterministic cleanup.
- [ ] No raw owning heap pointer is introduced in C++ code.

## Acceptance criteria

- [ ] `make` builds `pkchat`.
- [ ] `./pkchat --help` prints useful usage text.
- [ ] `./pkchat --version` prints a version string.
- [ ] `make clean` removes build outputs.
- [ ] `make sanitize` or the documented platform equivalent builds successfully where supported.
- [ ] `make leak-check` or the documented platform equivalent can run on the skeleton binary where supported.
- [ ] `README.md`, `TODO.md`, `AGENTS.md`, and `PLANS.md` exist at repo root.

---

# v0.1 - Script-friendly CLI for Chat Completions plus LM Studio profile

## Goal

Implementation note (2026-06-15): v0.3 is now present. The CLI supports model listing, Chat Completions, LM Studio aliases, OpenRouter defaults, text/JSON/NDJSON output, credential lookup, provider shortcuts, a simple REPL, JSON chat persistence, a standalone multiline `--editor` foundation, editor-backed TUI multiline input, a cancellable runtime/job layer, a non-blocking full-screen TUI foundation, a libcurl RAII transport, incremental SSE streaming, mock-server integration tests, sanitizer checks, and Valgrind leak checks. Remaining hardening is tracked in TODO.md: expand JSON handling, add broader error-path and credential-redaction tests, and continue v0.2 persistence work such as XDG chat IDs and chat listing.


Make `pkchat` useful from scripts and shells against `/v1/chat/completions` and `/v1/models`, including local LM Studio at `http://localhost:1234/v1`.

## Required user-facing commands

```sh
pkchat http://localhost:8000 -p "What is the capital of Norway?"
pkchat --base-url http://localhost:8000/v1 -p "Hello"
pkchat --provider openai -m MODEL -p "Hello"
pkchat --provider lm_studio -m MODEL -p "Hello from local LM Studio"
pkchat --provider lmstudio --list-models
pkchat --list-models http://localhost:8000
pkchat --prompt-file prompt.txt --system-file system.txt --format json
```

## CLI options

Implement or reserve these options:

```text
-p, --prompt TEXT
--prompt-file PATH
-s, --system TEXT
--system-file PATH
-m, --model MODEL
-t, --temperature FLOAT
--top-p FLOAT
--max-output-tokens N
--stream
--no-stream
--format text|json|ndjson|jsond
--output PATH
--provider NAME
--profile NAME
--base-url URL
--chat-url URL
--models-url URL
--responses-url URL
--key-env NAME
--key-file PATH
--key-stdin
-k, --key TEXT              discouraged; warn unless --quiet
--header "Name: Value"
--connect-timeout SECONDS
--timeout SECONDS
--proxy URL
--insecure-tls
--quiet
--debug
--trace-http
--version
--help
```

## Core modules

### CLI parser

- [ ] Parse options with clear errors.
- [ ] Reject unknown options.
- [ ] Reject missing option values.
- [ ] Validate numeric options.
- [ ] Validate URL-ish options before transport use.
- [ ] Support stdin input through `--prompt-file -` or a later `--stdin` option.
- [ ] Keep stdout clean for model output by default.
- [ ] Free all parser-owned allocations and temporary strings on success and error.

### Config and credentials

- [ ] Read API keys from provider-specific environment variables:
  - [ ] `OPENAI_API_KEY`
  - [ ] `OPENROUTER_API_KEY`
  - [ ] `LMSTUDIO_API_KEY`
  - [ ] `LM_STUDIO_API_KEY`
  - [ ] `PKCHAT_API_KEY`
- [ ] Support `--key-env`.
- [ ] Support `--key-file`.
- [ ] Support `--key-stdin`.
- [ ] Redact credentials in all debug/error output.
- [ ] Warn when `-k`/`--key` is used.
- [ ] Do not require a key for `--provider lm_studio` unless provided by the user or required by the server.
- [ ] Ensure key file buffers are cleared/released after use where practical.

### HTTP transport

- [ ] Use libcurl.
- [ ] Support GET and POST.
- [ ] Support request/response headers.
- [ ] Support connect timeout and total timeout.
- [ ] Support streaming callbacks.
- [ ] Support cancellation hook even if no UI uses it yet.
- [ ] Return structured errors.
- [ ] Release all `CURL*`, `curl_slist*`, response buffers, callback state, and error buffers on every path.

### Provider: OpenAI-compatible Chat Completions

- [ ] Implement `GET /v1/models`.
- [ ] Implement non-streaming `POST /v1/chat/completions`.
- [ ] Implement streaming `POST /v1/chat/completions`.
- [ ] Keep request JSON generation inside `src/provider/`.
- [ ] Keep response parsing inside `src/provider/`.
- [ ] Handle provider error bodies.
- [ ] Handle missing/unknown fields defensively.
- [ ] Release JSON documents/values and generated request buffers after use.

### Provider profile: LM Studio

Treat LM Studio as a named local OpenAI-compatible profile even though most request/response code can share the generic OpenAI-compatible adapter.

Required behavior:

- [ ] Support `--provider lm_studio`, `--provider lmstudio`, and `--provider lm-studio` aliases.
- [ ] Default base URL: `http://localhost:1234/v1`.
- [ ] Do not require an API key by default.
- [ ] If `LMSTUDIO_API_KEY`, `LM_STUDIO_API_KEY`, `PKCHAT_API_KEY`, `--key-*`, or `--header Authorization: ...` is provided, send the configured credential.
- [ ] Use `GET /v1/models` for model listing.
- [ ] Use `POST /v1/chat/completions` for initial chat support.
- [ ] Support streaming when the server accepts `stream: true`.
- [ ] Surface a clear error if the LM Studio server is not running on `localhost:1234`.
- [ ] Document that LM Studio can be bound to localhost or the LAN, and that LAN exposure needs user-side network security.

### SSE streaming parser

- [ ] Parse server-sent events incrementally.
- [ ] Handle arbitrary chunk boundaries.
- [ ] Handle blank event separators.
- [ ] Handle comments.
- [ ] Handle `data: [DONE]`.
- [ ] Handle malformed JSON with a specific error.
- [ ] Preserve UTF-8 boundaries.
- [ ] Release internal buffers after stream completion, parse failure, or cancellation.

### URL handling

- [ ] If URL path ends with `/v1`, use it.
- [ ] If URL path is empty or `/`, try `/v1`.
- [ ] Support explicit `--base-url`, `--chat-url`, and `--models-url`.
- [ ] Show selected base URL on `stderr` when auto-detected unless `--quiet` is set.
- [ ] Do not mutate user-provided strings in place unless ownership is explicit.

### Output formats

Text mode:

```text
stdout: assistant text
stderr: errors/status
```

JSON mode should include at least:

```json
{
  "model": "...",
  "provider": "...",
  "content": "...",
  "usage": null,
  "timing": {
    "ttft_ms": null,
    "total_ms": 1234
  }
}
```

NDJSON mode should be suitable for streaming events:

```json
{"event":"start","model":"..."}
{"event":"delta","text":"Hel"}
{"event":"delta","text":"lo"}
{"event":"done","usage":null}
```

All output modes must avoid leaking credentials.

## Error cases to test

- [ ] Empty prompt.
- [ ] Overly long prompt.
- [ ] Bad URL.
- [ ] Connection refused.
- [ ] LM Studio default port not listening.
- [ ] DNS failure.
- [ ] TLS failure.
- [ ] Timeout.
- [ ] HTTP 401.
- [ ] HTTP 403.
- [ ] HTTP 404.
- [ ] HTTP 429.
- [ ] HTTP 500.
- [ ] Malformed response JSON.
- [ ] Malformed streaming event.
- [ ] Invalid UTF-8 from server.
- [ ] API key redaction in errors/debug traces.
- [ ] Memory leak checks for successful request, failed request, and cancelled stream.

## Acceptance criteria

- [ ] `pkchat -p "Hello" --provider lm_studio -m MODEL` sends a request to `http://localhost:1234/v1/chat/completions` unless overridden.
- [ ] `pkchat --provider lmstudio --list-models` calls the LM Studio models endpoint.
- [ ] `pkchat http://localhost:8000 -p "Hello"` tries a sensible `/v1` base URL when needed.
- [ ] Streaming works against the mock server.
- [ ] Errors are specific.
- [ ] Credentials are redacted.
- [ ] stdout is clean in text mode.
- [ ] Leak-check tooling reports no leaks for v0.1 success and representative failure paths where supported.

---

# v0.2 - Simple interactive REPL and chat persistence

## Goal

Implementation note (2026-06-14): a first v0.2 slice is implemented. `--repl`/`-i` starts a line-oriented REPL, `--save-chat PATH` and `--load-chat PATH` persist explicit JSON chat files, one-shot mode can continue a saved chat, and the mock integration test covers save/load plus REPL stdout behavior. Remaining v0.2 work is tracked in TODO.md: SQLite-backed local chat threads, automatic save/load, `/list`, `/new`, `/remove`, fuller config/profile support, and schema migration mechanics.

Add a simple line-oriented interactive mode and durable chat persistence without yet building the full-screen TUI. Explicit JSON save/load remains useful for import/export, but the local chat library should move to a SQLite3 `pkchat.db` database in the local profile.

## Commands

Interactive mode should support at least:

```text
/help
/quit
/models
/provider PROVIDER
/model MODEL
/system TEXT
/temperature VALUE
/save
/load PATH
/list
/new
/remove
```

Command-line examples:

```sh
pkchat -i --provider lm_studio
pkchat --chat CHAT_ID
pkchat --resume
pkchat --new
```

## Tasks

- [x] Add internal message model independent from provider JSON.
- [x] Add conversation state object.
- [x] Add line-oriented REPL.
- [x] Add prompt history for the session.
- [x] Add `/save` and `/load`.
- [x] Add `/list` thread listing and picker.
- [x] Add `/new` to create a new chat thread.
- [x] Add `/provider` to switch the current chat thread's provider for future turns.
- [x] Add `/remove` to soft-delete the current chat thread after confirmation.
- [x] Add SQLite3-backed automatic chat persistence in `~/.pkchat/pkchat.db`.
- [x] Add atomic save.
- [x] Add corrupted chat-file handling.
- [ ] Add config/profile support.
- [ ] Add schema migration mechanism, even if only v1 exists.
- [x] Ensure every loaded JSON document, temporary string, file handle, and conversation allocation is released.

## SQLite local chat library

Use `libsqlite3` for automatic local chat persistence. Keep it isolated behind a
small `src/chat/` storage facade so CLI, REPL, TUI, future local server mode, and
JSON import/export do not depend on SQLite details directly.

The default database path is:

```text
~/.pkchat/pkchat.db
```

Create `~/.pkchat` with mode `0700` where supported. Create the database with
user-only permissions where the platform allows it. If `$HOME` is unavailable,
return a specific configuration/storage error instead of inventing a surprising
fallback. Do not store API keys, authorization headers, cookies, key-file paths,
or command-line `--key` values in the database.

Explicit JSON `--save-chat` and `--load-chat` remain import/export and
compatibility commands. The SQLite database is the primary local chat library for
interactive TUI threads.

## SQLite runtime settings

Open the database with clear error mapping for permission denied, missing home
directory, corruption, unsupported schema, migration failure, busy/locked
database, disk-full/IO failure, and memory allocation failure.

Required open-time pragmas:

```sql
PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA busy_timeout = 50;
```

Use short explicit transactions for writes. Set a short busy timeout so the TUI
does not wait indefinitely on a locked database. `/list` may run synchronously in
the TUI loop because it is an indexed local summary query expected to complete
well under 50 ms; if SQLite reports busy/locked, show a status error rather than
blocking. Full thread loads, saves, migrations, imports, exports, and deletes
still need the same responsiveness discipline as other file jobs when they can
touch many rows or slow storage.

Every `sqlite3*`, prepared statement, transaction guard, blob/buffer, and
temporary allocation must be released or rolled back on success, error,
cancellation, and early return. Wrap SQLite handles and statements in RAII
classes; do not leave raw owning SQLite resources in application code.

## Database schema v1

Timestamps are UTC ISO-8601 text generated by the same clock helper used by JSON
chat persistence. Store full message text in one row per message. This is more
work than a single JSON/JSONL transcript blob, but it gives the TUI and future
server/agent modes stable message IDs, fast thread listing, precise
regeneration/removal, message-level usage, attachments, and compaction metadata.

```sql
CREATE TABLE schema_migrations (
    version INTEGER PRIMARY KEY,
    applied_at TEXT NOT NULL
);

CREATE TABLE app_state (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    updated_at TEXT NOT NULL
);

CREATE TABLE threads (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    created_at TEXT NOT NULL,
    modified_at TEXT NOT NULL,
    last_provider TEXT NOT NULL,
    last_base_url TEXT NOT NULL DEFAULT '',
    last_model TEXT NOT NULL,
    settings_json TEXT NOT NULL DEFAULT '{}',
    usage_json TEXT NOT NULL DEFAULT '{}',
    message_count INTEGER NOT NULL DEFAULT 0,
    deleted_at TEXT
);

CREATE TABLE messages (
    id INTEGER PRIMARY KEY,
    thread_id INTEGER NOT NULL REFERENCES threads(id) ON DELETE CASCADE,
    ordinal INTEGER NOT NULL,
    created_at TEXT NOT NULL,
    role TEXT NOT NULL CHECK (role IN ('system', 'user', 'assistant')),
    content TEXT NOT NULL,
    metadata_json TEXT NOT NULL DEFAULT '{}',
    UNIQUE(thread_id, ordinal)
);

CREATE TABLE attachments (
    id INTEGER PRIMARY KEY,
    thread_id INTEGER NOT NULL REFERENCES threads(id) ON DELETE CASCADE,
    message_id INTEGER REFERENCES messages(id) ON DELETE CASCADE,
    ordinal INTEGER NOT NULL DEFAULT 0,
    kind TEXT NOT NULL,
    mime_type TEXT NOT NULL DEFAULT '',
    display_name TEXT NOT NULL DEFAULT '',
    metadata_json TEXT NOT NULL DEFAULT '{}',
    storage_ref TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL
);

CREATE TABLE usage_records (
    id INTEGER PRIMARY KEY,
    thread_id INTEGER NOT NULL REFERENCES threads(id) ON DELETE CASCADE,
    message_id INTEGER REFERENCES messages(id) ON DELETE SET NULL,
    provider TEXT NOT NULL DEFAULT '',
    model TEXT NOT NULL DEFAULT '',
    usage_json TEXT NOT NULL,
    created_at TEXT NOT NULL
);

CREATE TABLE compaction_events (
    id INTEGER PRIMARY KEY,
    thread_id INTEGER NOT NULL REFERENCES threads(id) ON DELETE CASCADE,
    summary_message_id INTEGER REFERENCES messages(id) ON DELETE SET NULL,
    policy TEXT NOT NULL,
    messages_compacted INTEGER NOT NULL,
    original_bytes INTEGER NOT NULL,
    request_bytes INTEGER NOT NULL,
    notice TEXT NOT NULL DEFAULT '',
    metadata_json TEXT NOT NULL DEFAULT '{}',
    created_at TEXT NOT NULL
);
```

Thread `name` is user-facing. Initially derive it from the first non-empty user
message, truncated for display, and allow a later rename command. `modified_at`
must update whenever messages, provider/model metadata, name, attachments, usage,
or compaction state changes. `last_provider`, `last_base_url`, and `last_model`
store the last successful or explicitly selected chat context so a thread can be
resumed with the same provider/model combination. The user can override that
context with `/provider` and `/model`; subsequent saves update the thread row.

Store the currently active thread in:

```text
app_state.key = 'last_thread_id'
```

## Database indexes

Required indexes for fast thread listing and transcript replay:

```sql
CREATE INDEX idx_threads_list
    ON threads(deleted_at, modified_at DESC, id DESC);

CREATE INDEX idx_threads_provider_model
    ON threads(last_provider, last_model, modified_at DESC);

CREATE INDEX idx_messages_thread_ordinal
    ON messages(thread_id, ordinal);

CREATE INDEX idx_messages_thread_created
    ON messages(thread_id, created_at);

CREATE INDEX idx_attachments_thread_message
    ON attachments(thread_id, message_id, ordinal);

CREATE INDEX idx_usage_thread_created
    ON usage_records(thread_id, created_at DESC);

CREATE INDEX idx_usage_message
    ON usage_records(message_id);

CREATE INDEX idx_compaction_thread_created
    ON compaction_events(thread_id, created_at DESC);

CREATE INDEX idx_compaction_summary_message
    ON compaction_events(summary_message_id);
```

The `/list` query should use `idx_threads_list` and fetch only summary fields:

```sql
SELECT id, name, created_at, modified_at, last_provider, last_model, message_count
FROM threads
WHERE deleted_at IS NULL
ORDER BY modified_at DESC, id DESC
LIMIT ?;
```

`LIMIT` should be large enough for normal use but bounded for rendering, for
example 200 rows initially. Add pagination or filtering only when a real need
appears.

## SQLite persistence requirements

- [x] Link against `libsqlite3` from the Makefile without adding a package-manager requirement.
- [x] Add RAII wrappers for SQLite database handles, prepared statements, and transactions.
- [x] Create `~/.pkchat/pkchat.db` with WAL mode enabled.
- [x] Add v1 migrations and record applied schema versions.
- [x] Add indexes for latest-thread listing, provider/model filtering, transcript replay, attachments, usage, and compaction events.
- [x] Automatically create a thread when the first TUI turn is saved and no thread exists.
- [x] Automatically append user/assistant/system messages and update thread metadata after successful turns.
- [x] Persist partial assistant content deliberately only when cancellation keeps the cancelled turn visible.
- [x] Automatically load the last active thread where appropriate.
- [x] Keep deletes deliberate: `/remove` must confirm and then soft-delete the current thread by setting `deleted_at`; hard-delete can be a later maintenance command.
- [ ] Ensure SQLite errors include the database path and operation involved, without leaking credentials or local secrets.
- [ ] Ensure all SQLite statements, handles, transactions, temporary strings, and per-row allocations are finalized or released on success, error, cancellation, and interrupted-stream paths.

## TUI chat thread commands

- [x] `/new [NAME]` creates a new empty chat thread and switches to it.
- [x] `/provider NAME` changes the current thread's provider context for future turns.
- [x] `/model MODEL` continues to change the current thread's model for future turns.
- [x] `/remove` asks for confirmation and soft-deletes the current chat thread.
- [x] `/list` runs the indexed thread-summary query synchronously, newest modified thread first.
- [x] `/list` opens a thread-picker view in the TUI, not a static transcript message.
- [x] In the picker, up/down changes selection, Enter loads the selected thread, and Esc cancels without changing the active chat.
- [x] After selecting a thread or cancelling the picker, the chat screen refreshes fully.
- [x] Selecting a thread restores its messages and last provider/model/base URL context; `/provider` and `/model` may override it before the next turn.

## Acceptance criteria

- [x] Explicit JSON chat files can be saved and loaded for compatibility/import-export.
- [x] TUI local storage opens or creates `~/.pkchat/pkchat.db` with WAL mode enabled.
- [x] Active chat threads are saved automatically after message changes.
- [x] The last active thread can be loaded automatically where appropriate.
- [x] `/new` creates and switches to a new chat thread.
- [x] `/list` lists saved threads newest-first and supports keyboard selection in TUI mode.
- [x] `/list` summary query is indexed and completes synchronously in normal local use without blocking on network or model work.
- [x] `/remove` soft-deletes the current thread after confirmation.
- [x] `/provider` and `/model` can change the provider/model used when resuming a thread.
- [x] Corrupted JSON chat files produce a specific error without crashing.
- [ ] Corrupted SQLite databases produce a specific error and recovery guidance.
- [ ] Permission-denied writes produce a specific error.
- [ ] Disk-full or short-write cases are handled where testable.
- [x] API keys are not saved.
- [ ] Leak-check tooling reports no leaks for SQLite open/save/load/list/remove paths and JSON import/export paths where supported.

---

# v0.3 - Runtime/job layer and non-blocking full-screen TUI foundation

## Goal

Add the runtime/job layer required for responsive UI behavior, then build the first full-screen TUI on top of it.

The full-screen UI must not block while connecting to an endpoint, waiting for a response, streaming output, loading/saving chats, compacting context, or processing an input file.

## Runtime/job tasks

- [ ] Add `src/runtime/` abstraction.
- [ ] Choose and document the runtime model: worker thread pool, single worker thread plus event queue, non-blocking event loop, or another explicit design.
- [ ] Add job IDs.
- [ ] Add cancellation tokens.
- [ ] Add thread-safe event queue.
- [ ] Add event types for:
  - [ ] job started
  - [ ] token/content delta
  - [ ] provider status
  - [ ] warning
  - [ ] error
  - [ ] job completed
  - [ ] job cancelled
  - [ ] progress update
- [ ] Add clean shutdown that cancels or joins workers.
- [ ] Ensure no worker can mutate TUI or chat state directly.
- [ ] Add unit tests for job completion, cancellation, worker error propagation, and shutdown.
- [ ] Add leak tests for completed, failed, and cancelled jobs.

## Target layout

```text
+--------------------------------------------------+
| chat history / scrollback                        |
|                                                  |
| streaming assistant output appears here          |
|                                                  |
+--------------------------------------------------+
| multiline input area, height floor(rows / 5)     |
| minimum 3 lines when possible                    |
+--------------------------------------------------+
| status: /help /quit /model | Alt+Enter sends     |
+--------------------------------------------------+
```

## TUI tasks

- [ ] Use `ncursesw` or equivalent wide-character terminal library.
- [ ] Initialize and restore terminal state safely.
- [ ] Handle terminal resize.
- [ ] Draw chat history.
- [ ] Draw input area.
- [ ] Draw status line.
- [ ] Keep streaming output separate from input editing.
- [ ] Allow cancellation of active generation.
- [ ] Display errors in the status line or an error pane.
- [ ] Keep UI responsive while a mock endpoint delays connection or response.
- [ ] Release all windows, buffers, input history, and terminal resources on normal exit and error exit.

## TUI color and theme tasks

Use colors by default in the full-screen TUI, but keep them strictly optional and semantic.

- [x] Add `--nocolors` to disable all color styling in the TUI.
- [x] Keep cursor movement, alternate-screen, clear-line, and other terminal control sequences working when `--nocolors` is set; only styling/color SGR sequences should be disabled.
- [x] Add built-in `dark` and `light` themes.
- [x] Default to the `dark` theme unless a later config layer provides a user preference.
- [x] Add `/theme` to show the current theme and available theme names.
- [x] Add `/theme dark` and `/theme light` to switch themes at runtime.
- [x] Use semantic style roles rather than hard-coded colors at each draw site:
  - [x] normal text
  - [x] muted text
  - [x] user label, for `You:`
  - [x] assistant label, for `Assistant:`
  - [x] error text
  - [x] status bar foreground/background
  - [x] input label foreground/background
  - [x] background fill
- [x] Draw full-width lines with an explicit theme background when colors are enabled so contrast is predictable.
- [x] Color message labels and visible thinking traces differently from ordinary message content; defer rich markdown coloring.
- [x] Display errors in a distinct error color from ordinary status text.
- [x] Use 24-bit ANSI color escapes initially so contrast is controlled instead of depending on terminal palette mappings.
- [x] Add WCAG 2.1 contrast tests for every foreground/background pair used by the themes.
- [x] Require at least 4.5:1 contrast for all normal text pairs.
- [x] Candidate dark theme:
  - [x] background `#0B0F14`
  - [x] normal text `#E6EDF3`
  - [x] muted text `#9BA7B4`
  - [x] thinking trace text `#A7B8C9`
  - [x] user label `#7DD3FC`
  - [x] assistant label `#86EFAC`
  - [x] error text `#FCA5A5`
  - [x] status background `#1F2937`
  - [x] status foreground `#FFFFFF`
- [x] Candidate light theme:
  - [x] background `#FAFAFA`
  - [x] normal text `#111827`
  - [x] muted text `#4B5563`
  - [x] thinking trace text `#52637A`
  - [x] user label `#075985`
  - [x] assistant label `#166534`
  - [x] error text `#B91C1C`
  - [x] status background `#E5E7EB`
  - [x] status foreground `#111827`
- [x] Add unit tests for `--nocolors` parsing.
- [x] Add unit tests for `/theme` argument handling if theme command parsing is factored out.
- [x] Document default colors, `--nocolors`, and `/theme light|dark` in `README.md`.
- [x] Document the color decision, truecolor ANSI choice, and contrast target in `docs/decisions.md`.

## Initial key bindings

Use portable defaults first:

```text
Enter                insert newline or submit, depending on configured mode
Alt+Enter            send prompt where detectable
Esc then Enter       send prompt fallback
Esc                  cancel active generation
Ctrl+U               undo editor/input edit
Ctrl+R               redo editor/input edit
Ctrl+F               search with a minibuffer prompt in editor mode
Ctrl+H               search and replace with minibuffer prompts in editor mode
F3/Shift+F3          search next/previous in editor mode
Ctrl+Q               quit chat/editor mode
Ctrl+C               copy selection in chat/editor input
Ctrl+X               cut selection in chat/editor input
Ctrl+V               paste in chat/editor input
Ctrl+D               quit when input is empty
Shift+arrows         extend selection in chat/editor input
Shift+PageUp/Down    extend selection in chat/editor input
Shift+Home/End       extend selection in chat/editor input
PageUp/PageDown      scroll the active editor/input window
/help                show help
/quit                quit
/send                send current prompt
```

Do not assume Ctrl+Enter or Shift+Enter are portable. They may be optional advanced bindings later.

## Later editor features

Defer until the base TUI is stable:

```text
insert/overwrite mode
prompt history navigation
read OS clipboard when internal clipboard is empty (e.g. Ctrl+Shift+V style)
mouse support
advanced key protocols
theme persistence/config
```

## Unicode requirements

- [ ] Display Chinese, Arabic, Cyrillic, Ä, Ö, Å correctly when terminal/font support exists.
- [ ] Do not split UTF-8 sequences.
- [ ] Track grapheme clusters for cursor movement.
- [ ] Track terminal cell width.
- [ ] Handle combining marks.
- [ ] Handle invalid UTF-8 with a clear replacement/error policy.

## Basic markdown rendering

Start small:

- [ ] Code blocks.
- [ ] Inline code.
- [ ] Headings.
- [ ] Bullet lists.
- [ ] Links underlined where supported.

Always provide `--no-markdown` or equivalent before markdown rendering becomes complex.

## Responsiveness tests

- [ ] Mock endpoint sleeps before accepting.
- [ ] Mock endpoint accepts but delays first token.
- [ ] Mock endpoint streams slowly.
- [ ] Mock endpoint disconnects mid-stream.
- [ ] Large input file processing job runs while UI scroll/input still works.
- [ ] Chat save/load on a deliberately slow mock filesystem abstraction does not freeze UI.
- [ ] SIGWINCH/resize during streaming does not corrupt the screen.
- [ ] Cancellation while streaming returns UI to an idle usable state.

## Acceptance criteria

- [ ] TUI starts and exits cleanly.
- [ ] Terminal state is restored after normal exit.
- [ ] Streaming output appears without corrupting the input area.
- [ ] User can scroll or open help while a request is pending.
- [ ] User can cancel an in-flight request.
- [ ] Slow endpoint tests demonstrate that the UI event loop is not blocked.
- [ ] Leak-check tooling reports no leaks for TUI startup/exit, cancelled request, and slow-stream scenarios where supported.

---

# v0.4 - Provider adapters, Responses API, LM Studio refinement, and compatibility profiles

## Goal

Expand from a basic OpenAI-compatible Chat Completions client into a provider-profile based client that can support newer OpenAI APIs and common local/third-party dialects.

## Built-in provider registry

Seed the provider registry with these built-in OpenAI-compatible profiles. Keep
`custom_openai_chat` separately for explicit user-supplied endpoints.

```text
[provider.openai]
base_url = https://api.openai.com/v1
chat_path = /chat/completions
models_path = /models
api_key_env = OPENAI_API_KEY
api_key_required = true

[provider.openrouter]
base_url = https://openrouter.ai/api/v1
chat_path = /chat/completions
models_path = /models
api_key_env = OPENROUTER_API_KEY
api_key_required = true

[provider.deepseek]
base_url = https://api.deepseek.com
chat_path = /chat/completions
models_path = /models
api_key_env = DEEPSEEK_API_KEY
api_key_required = true

[provider.gemini]
base_url = https://generativelanguage.googleapis.com/v1beta/openai
chat_path = /chat/completions
models_path = /models
api_key_env = GEMINI_API_KEY
api_key_required = true

[provider.anthropic]
base_url = https://api.anthropic.com/v1
chat_path = /chat/completions
models_path = /models
api_key_env = ANTHROPIC_API_KEY
api_key_required = true
compatibility_warning = OpenAI compatibility layer is mainly for testing/comparison.

[provider.grok]
alias_for = xai

[provider.xai]
base_url = https://api.x.ai/v1
chat_path = /chat/completions
models_path = /models
api_key_env = XAI_API_KEY
api_key_required = true

[provider.moonshot]
base_url = https://api.moonshot.ai/v1
chat_path = /chat/completions
models_path = /models
api_key_env = MOONSHOT_API_KEY
api_key_required = true

[provider.kimi]
alias_for = moonshot

[provider.groq]
base_url = https://api.groq.com/openai/v1
chat_path = /chat/completions
models_path = /models
api_key_env = GROQ_API_KEY
api_key_required = true

[provider.mistral]
base_url = https://api.mistral.ai/v1
chat_path = /chat/completions
models_path = /models
api_key_env = MISTRAL_API_KEY
api_key_required = true

[provider.together]
base_url = https://api.together.ai/v1
chat_path = /chat/completions
models_path = /models
api_key_env = TOGETHER_API_KEY
api_key_required = true

[provider.perplexity]
base_url = https://api.perplexity.ai
chat_path = /chat/completions
models_path = /models
api_key_env = PERPLEXITY_API_KEY
api_key_required = true
compatibility_warning = Perplexity canonical Sonar endpoint is /v1/sonar; /chat/completions is the OpenAI SDK-compatible alias.

[provider.cerebras]
base_url = https://api.cerebras.ai/v1
chat_path = /chat/completions
models_path = /models
api_key_env = CEREBRAS_API_KEY
api_key_required = true

[provider.fireworks]
base_url = https://api.fireworks.ai/inference/v1
chat_path = /chat/completions
models_path = /models
api_key_env = FIREWORKS_API_KEY
api_key_required = true

[provider.deepinfra]
base_url = https://api.deepinfra.com/v1/openai
chat_path = /chat/completions
models_path = /models
api_key_env = DEEPINFRA_API_KEY
api_key_env_alt = DEEPINFRA_TOKEN
api_key_required = true

[provider.nvidia_nim]
base_url = https://integrate.api.nvidia.com/v1
chat_path = /chat/completions
models_path = /models
api_key_env = NVIDIA_NIM_API_KEY
api_key_required = true

[provider.dashscope]
base_url = https://dashscope.aliyuncs.com/compatible-mode/v1
chat_path = /chat/completions
models_path = /models
api_key_env = DASHSCOPE_API_KEY
api_key_required = true

[provider.lmstudio]
base_url = http://localhost:1234/v1
chat_path = /chat/completions
models_path = /models
api_key_required = false

[provider.lm_studio]
alias_for = lmstudio

[provider.ollama]
base_url = http://localhost:11434/v1
chat_path = /chat/completions
models_path = /models
api_key_required = false

[provider.vllm]
base_url = http://localhost:8000/v1
chat_path = /chat/completions
models_path = /models
api_key_required = false
dummy_api_key = token-abc123

[provider.llamacpp]
base_url = http://localhost:8080/v1
chat_path = /chat/completions
models_path = /models
api_key_required = false

[provider.llama.cpp]
alias_for = llamacpp
```

## Provider profile fields

Each profile should define or detect:

```text
name
aliases
default_base_url
default_key_envs
requires_bearer_key
optional_bearer_key
supports_chat_completions
supports_responses_api
supports_streaming
supports_usage_reporting
supports_images
supports_pdfs
supports_file_uploads
supports_file_urls
supports_tool_calls
supports_server_side_context_management
local_endpoint
custom_headers
notes
```

## Tasks

- [x] Add a provider registry.
- [x] Add profile lookup by name and alias.
- [ ] Add capability detection/probing.
- [x] Add OpenAI Responses request generation.
- [x] Add OpenAI Responses streaming parser mapping into internal events.
- [ ] Add provider-specific error normalization.
- [x] Add docs for each supported provider.
- [x] Ensure each adapter frees provider-specific request/response state on all paths through shared RAII HTTP/runtime code.


Implementation note (2026-06-16): The first v0.4 slice is present. `src/provider/` now has a built-in registry for the expanded provider list, alias lookup, endpoint paths, key defaults, compatibility warnings, and client capability flags. `--api responses`, `--responses`, and `--provider openai_responses` select a text-only Responses API adapter that shares the existing HTTP/runtime/cancellation path and maps output text and streaming deltas into the same internal assistant message model as Chat Completions. Capability probing and provider-specific error normalization remain open.

## Compatibility matrix

Maintain `docs/api-compatibility.md` with a table similar to:

```text
provider      chat   responses   streaming   models   key default      local
openai        yes    yes         yes         yes      required         no
lm_studio     yes    detect      yes/detect  yes      optional         yes
openrouter    yes    maybe       yes         yes      required         no
ollama        yes    detect      yes         yes      optional/local   yes
vllm          yes    detect      yes         yes      optional/local   yes
llama_cpp     yes    detect      yes         yes      optional/local   yes
custom        yes    no/detect   detect      detect   user-defined     unknown
```

Do not overstate support. If a capability is detected or version-dependent, say so.

## Acceptance criteria

- [x] Provider registry can resolve all aliases.
- [x] LM Studio remains explicitly supported and documented.
- [x] A provider capability can be reported to CLI/TUI/web code without leaking provider internals.
- [x] OpenAI Chat Completions and Responses paths both map into the internal message/event model.
- [x] Unsupported features return clear `PKCHAT_ERR_UNSUPPORTED_FEATURE` errors.
- [ ] Leak-check tooling reports no leaks for provider registry lookup, capability probing, and failed provider calls where supported.

---

# v0.5 - Context management, attachments, and safe URL fetching

## Goal

Add context-window management, text/file insertion, and safe URL fetching without silently destroying chat history or pretending every provider supports every input type.

## Context management

Policies:

```text
--context-policy error
--context-policy truncate-oldest
--context-policy summarize-oldest
--context-policy summarize-middle
--context-policy provider-auto
```

Rules:

- [x] Preserve full transcript on disk.
- [x] Compact only the provider-bound request context.
- [x] Show a clear notice when compaction happens.
- [x] Record compaction events in chat JSON.
- [x] Distinguish provider token counts from the local text-byte estimate.
- [x] Release temporary compacted-context allocations immediately after request completion.

Example notice:

```text
Context compacted: 42 earlier messages summarized into 1 message. Full transcript preserved on disk.
```

## Attachments

Start with text files:

- [x] `/insert PATH` and `/attach PATH` in REPL for text and images.
- [x] `/insert PATH` and `/attach PATH` in TUI through a cancellable runtime job.
- [x] Repeatable `--attach PATH` for non-interactive CLI prompts.
- [x] Size limit.
- [x] Explicit UTF-8 requirement.
- [x] Clear error for binary files.
- [x] Clear error for unreadable files.
- [x] Clear error for unsupported provider-native attachment types.
- [x] Release raw file buffers after conversion or failure.

Later provider-native inputs:

```text
images
PDFs
file uploads
file URLs
```

Rules:

- [x] Check provider/model image capabilities first.
- [x] Support multiple image attachments in one Chat Completions user turn.
- [x] Do not fake PDF support by blindly dumping binary data into a prompt.
- [ ] If local extraction is added, document the dependency and limitations.
- [x] Use size limits for local attachment reads.
- [x] Clean up temporary extracted text/files on success, failure, and cancellation.

## Safe URL fetching

Feature examples:

```text
summarize https://example.com/article
pkchat --fetch-url https://example.com/article -p "Summarize this"
```

Safety defaults:

- [x] Make fetching explicit or clearly visible for the first extraction mode.
- [x] Limit response size with a hard HTTP body cap.
- [x] Set connect and total timeouts for fetch mode.
- [x] Limit redirects by not following redirects in the first slice.
- [x] Check content type for HTML extraction.
- [x] Block private addresses by default for literal/localhost/common metadata hosts:
  - [x] loopback
  - [x] link-local
  - [x] multicast
  - [x] RFC1918 private ranges
  - [x] metadata-service addresses
- [x] Block DNS-resolved private IPv4/IPv6 addresses at the socket boundary.
- [x] Add override such as `--allow-private-url-fetch` only with clear warnings.
- [x] Show which URL was fetched unless `--quiet` is set.
- [x] Reject unsupported non-UTF-8 encodings clearly; conversion remains future work.
- [x] Release transfer handles, buffers, parsers, and temporary files after success, failure, timeout, and cancellation.


Implementation note (2026-06-17): The first v0.5 slice is present. `src/html/` provides a small C++17 `std::regex` HTML converter with plaintext and Markdown output for simple headings, emphasis, strong text, links, line breaks, and block spacing. `--input` converts supported local `.txt`, `.md`, and `.html` files without contacting a provider. `--html-file` remains a compatibility alias for local HTML. `--fetch-url` uses libcurl to fetch HTML explicitly, applies a response-size cap, a default fetch timeout, content-type checks, no redirect following, and private/loopback/link-local/multicast/common metadata literal-host blocking unless `--allow-private-url-fetch` is set. This slice can inject fetched or local input content into non-interactive chat prompts, but does not yet process JavaScript, convert charsets, check DNS-resolved private addresses, or implement PDF/Word extraction.

Implementation note (2026-06-20): `--input` now classifies supported file endings case-insensitively and accepts PNG, JPEG, and GIF images with a non-interactive prompt. WebP input is intentionally disabled after compatibility tests with common vision models. `src/input/` performs bounded reads, signature checks, and base64 encoding; Chat Completions requests use `image_url` data-URL content parts. Image bytes are temporary and are not persisted in chat JSON. Responses API images remain open.

Implementation note (2026-06-20): Repeatable `--attach PATH` and REPL `/insert PATH` now add converted UTF-8 text, Markdown, or HTML context. Local document reads have a default 1 MiB `--max-input-bytes` cap and reject binary NUL bytes, invalid UTF-8, unreadable files, unsupported types, PDF, and DOCX. Charset conversion remains open; PDF/Markdown and DOCX/Markdown input/output conversion is explicitly deferred in `TODO.md`.

Implementation note (2026-06-20): The remaining core slice adds repeated mixed text/image attachments, conservative provider/model image capability checks, request-only byte-budget context policies with persisted compaction events, socket-level private-address rejection after DNS resolution, and TUI `/insert` through a cancellable runtime file job. Responses API image schema support and charset conversion remain follow-up work.

Implementation note (2026-06-20): Interactive `/insert PATH` and `/attach PATH` now accept text or supported images in REPL/TUI. Images are queued for exactly the next prompt and remain request-only. `/fetch URL` inserts safely fetched Markdown; TUI fetching uses the runtime job and cancellation token. TUI `/help` now renders a persistent UI-only command panel instead of a transient status line.

Implementation note (2026-06-20): Non-interactive `--input stdin` and `--attach stdin` accept bounded UTF-8 plaintext from standard input, with validation preventing multiple options from consuming the same stream. `--output stdout` explicitly selects standard output for pipeline composition.

## Acceptance criteria

- [x] Context compaction never modifies the full saved transcript destructively.
- [x] Text file insertion works for UTF-8 text files in CLI, REPL, and TUI modes.
- [x] Binary/unreadable/too-large files produce clear errors.
- [x] URL fetching refuses private literal, localhost, and DNS-resolved addresses by default.
- [x] URL fetch timeout and max-size limits work for the explicit CLI extraction path.
- [x] TUI remains responsive while file insertion is in progress.
- [x] TUI remains responsive while cancellable URL fetching is in progress.
- [x] Sanitizer leak checks pass for covered attachment and URL-fetch success/failure paths.

---

# v0.6 - System and user TOML-alike configuration files

## Goal

Add predictable system and user configuration files without adding a TOML dependency or claiming full TOML compatibility. A configuration file supplies persistent defaults; command-line arguments and positional provider/URL shortcuts continue to win. Arbitrary extra configuration-file layering is intentionally outside this milestone.

With no configuration files present, behavior must remain identical to v0.55. In particular, the default provider remains `openai`, the default API remains Chat Completions, streaming remains enabled, and the existing size and timeout defaults do not change.

## Architectural fit

- [x] Add a parser, schema validator, XDG path resolver, and layer loader under `src/config/`.
- [x] Keep `cli::Options` as the effective application settings passed to existing CLI, provider, input, context, REPL, and TUI code.
- [x] Keep built-in provider definitions and aliases in `src/provider/`; do not copy OpenAI, LM Studio, Qwen, ZAI, or other registry records into an embedded config string.
- [x] Use the current `cli::Options` initializers as code defaults. Parsed files are typed overrides, not a second source of built-in defaults.
- [x] Apply configuration before the full CLI parse, then parse CLI arguments over the configured `Options` value. An overload such as `parse_args(argc, argv, base_options)` preserves existing parser tests and callers.
- [x] Keep credential resolution in `provider::build_context` after the final provider and `key_env`/`key_file` settings are known. The config parser never reads or retains an API key value.
- [x] Extend `cli::Options` only for settings the current runtime can consume, such as persistent TUI theme/thinking defaults. Do not add unused web, benchmark, agent, PDF, or DOCX settings in this milestone.

Implementation note (2026-06-22): `config/pkchat.conf` is the system-wide template aligned with the v0.6 `cli::Options` defaults. `make install` places it at `${SYSCONFDIR}/xdg/pkchat/config.conf` with mode `0644` and preserves an existing file. `src/config/` provides a bounded parser, complete initial-schema validation, XDG system/user discovery, transactional application, and CLI-last option merging. TUI theme/thinking defaults and URL-fetch private-address policy are wired to effective configuration. `--no-config` skips the user file only, and `--debug` reports file discovery/load state without printing values. Repeatable explicit `--config` layering was rejected as unnecessary complexity.

## File syntax

Call the format "pkchat config" or "TOML-alike" in documentation, not TOML. Use `config.conf` so users do not reasonably expect a general TOML parser.

```text
file         = line*
line         = blank | comment | section | assignment
blank        = horizontal_whitespace*
comment      = horizontal_whitespace* "#" text
section      = horizontal_whitespace* "[" section_name "]" horizontal_whitespace*
assignment   = horizontal_whitespace* key horizontal_whitespace* "=" horizontal_whitespace* value
section_name = name ("." name)*
key          = name
name         = ASCII letter or "_", followed by ASCII letters, digits, "_", or "-"
value        = boolean | integer | float | quoted_string | bare_string
```

Rules:

- [x] Accept LF and CRLF input and an optional UTF-8 BOM at the start of the file.
- [x] Validate the complete file as UTF-8 before parsing.
- [x] Support lowercase `true` and `false`, base-10 signed integers, finite decimal floats, quoted strings, and bare strings.
- [x] Quoted strings support only `\\`, `\"`, `\n`, `\r`, and `\t` escapes. Reject unknown and incomplete escapes.
- [x] Bare strings extend to end of line, preserve `#`, allow UTF-8, and trim ASCII space and tab at both ends. Comments are full-line only in v0.6.
- [x] Permit an empty bare string, although schema validation may reject it for a specific key.
- [x] Reject malformed section headers, trailing text after a section, invalid names, integer overflow, non-finite floats, and unterminated strings with a `PKCHAT_ERR_CONFIG` error.
- [x] Report the source path, line, column, section, and key where applicable.
- [x] Reject duplicate fully qualified keys within one file. A later file may override a key from an earlier layer.
- [x] Reject unknown sections and keys. Silent typo handling is not acceptable for the first version.
- [x] Accept optional `config_version = 1` at the root and reject unsupported versions clearly.

Do not support arrays, repeated keys, dotted assignment keys, inline comments, multiline strings, tables as values, date/time literals, includes, environment interpolation, or expressions in v0.6. Add list syntax only when a concrete implemented setting needs it; current attachment paths and prompts are per-command inputs and must not become persistent lists.

## Initial schema

The first schema maps only to behavior already implemented or explicitly completed as part of v0.6:

```conf
# Example user overrides; these are not new built-in defaults.
config_version = 1
provider = openai
model = gpt-4.1-mini
api = chat

[endpoint]
base_url =
chat_url =
models_url =
responses_url =

[generation]
stream = true
temperature = 0.7
top_p = 0.9
max_output_tokens = 4096

[context]
window_tokens = 64k
max_bytes = 0
policy = error

[network]
connect_timeout_seconds = 10
request_timeout_seconds = 0
proxy =
insecure_tls = false

[credentials]
key_env = OPENAI_API_KEY
key_file =

[output]
format = text
render_format = md

[input]
max_input_bytes = 1048576
max_image_bytes = 20971520
image_capability = auto

[editor]
undo_limit = 5
huge_file_size_warning = 1073741824
file_size_limit = -1

[url_fetch]
max_bytes = 1048576
allow_private_addresses = false

[tui]
colors = true
theme = dark
thinking_traces = false
```

Schema requirements:

- [ ] Use the same enum spellings and numeric ranges as their CLI counterparts: `api`, formats, context policy, image capability, timeouts, byte limits, sampling values, and context-window shorthand must not diverge.
- [x] Omitted `temperature`, `top_p`, and `max_output_tokens` remain unset so the request does not gain parameters merely because config support exists.
- [x] `context.window_tokens` maps to `--context`; accept positive integers plus the existing case-insensitive `k` (1024) and `M` (1000000) suffixes. Omission or `0` disables the TUI estimate.
- [x] `tui.theme` accepts only `dark` or `light` and becomes the initial TUI theme; `/theme` can still change it for the current process.
- [x] `tui.thinking_traces` controls whether the TUI initially displays model thinking traces; `/thinking` can still change it for the current process.
- [x] `network.insecure_tls = true` emits a security warning to `stderr` whenever it is effective.
- [x] Do not accept prompt text, system text, input/attachment paths, fetch URLs, output paths, save/load paths, mode/action flags, `--key`, `--key-stdin`, arbitrary headers, or API key values from persistent configuration.
- [x] Do not add named custom provider profiles in the first slice. Users can select a built-in provider and configure endpoint overrides; dynamic profiles require a later provider-registry extension with separate validation tests.

## Discovery and precedence

Configuration control:

```text
--no-config      Skip the automatically discovered user file; retain system files.
```

Resolve layers from lowest to highest precedence:

1. current C++ defaults in `cli::Options` and the built-in provider registry
2. system files named `pkchat/config.conf` from `$XDG_CONFIG_DIRS`, or `/etc/xdg` when unset
3. `$XDG_CONFIG_HOME/pkchat/config.conf`, or `$HOME/.config/pkchat/config.conf` when unset
4. command-line options and the positional `BASE_URL|PROFILE` shortcut

Implementation details:

- [x] Because `$XDG_CONFIG_DIRS` lists higher-priority directories first, load existing system files in reverse order so the first directory wins after merging.
- [x] Follow XDG path validity rules: ignore relative `$XDG_CONFIG_DIRS` entries, and use the documented fallback when `$XDG_CONFIG_HOME` is empty or relative. If `HOME` is also unavailable, skip automatic user-config discovery without preventing system configuration.
- [x] `--no-config` skips the automatic user file while retaining system configuration.
- [x] Missing automatic files are normal; an existing but unreadable or invalid automatic file is an error.
- [x] Bound each config file to 1 MiB and reject non-regular files with a specific error.
- [x] Use the existing preliminary CLI parse to detect `--no-config`, `--help`, `--version`, `--debug`, and `--quiet`, then run the full parser over configured base options.
- [x] `--help` and `--version` must work without reading config files, including when an automatically discovered file is malformed.
- [x] XDG and `HOME` affect path discovery. Provider-specific environment variables and an explicitly selected `key_env` continue to supply credentials after files are merged. Do not invent a second general `PKCHAT_*` environment-settings layer in this milestone.
- [x] Do not print config status on `stdout`. `--debug` lists loaded, missing, skipped, and failed paths on `stderr` without printing configuration values; `--quiet` suppresses it.

## Merge and validation

- [x] Represent parsed values with owned RAII types and source metadata; no raw owning pointers or parser-global mutable state.
- [x] Merge typed setting overrides by fully qualified schema key. Scalars replace earlier scalars.
- [x] Validate each file completely before applying it so one invalid file cannot partially mutate effective settings.
- [ ] Reuse or factor the CLI's existing numeric, format, context-policy, image-capability, and context-token validation helpers instead of implementing subtly different rules.
- [x] Preserve the CLI's explicit-state semantics (`has_temperature`, `has_top_p`, `has_max_output_tokens`, output format flags, and `stream_explicit`) when config supplies a setting and when CLI later overrides it.
- [ ] Ensure a configured provider is validated by the existing provider registry and that `provider = none` cannot acquire a model endpoint accidentally.
- [ ] Keep config data out of saved chat JSON except for the effective non-secret settings already recorded by chat persistence. Never record the config path, key values, or raw config text.

## Implementation sequence

0. [x] Add the common `config/pkchat.conf` template and install it without overwriting an existing administrator-managed file.
1. [x] Add parser/value/source-location types in `src/config/config.hpp` and `src/config/config.cpp` plus grammar/error unit tests.
2. [x] Add the typed schema mapper and tests for supported keys, representative enums/ranges, and rejected keys.
3. [x] Add XDG discovery and deterministic automatic layer merging with injectable environment/path inputs for tests.
4. [x] Add preliminary CLI handling for `--no-config` and parsing over configured base options; deliberately omit explicit `--config` layering.
5. [x] Wire effective settings into `main.cpp`, provider context construction, input/fetch limits, output formatting, context handling, and the initial TUI theme/thinking mode.
6. [x] Add integration coverage using an isolated `XDG_CONFIG_HOME` fixture and isolated automatic system paths.
7. [x] Update `README.md`, `TODO.md`, `docs/decisions.md`, and `docs/security.md` with syntax, precedence, examples, limitations, and credential guidance.
8. [x] Run normal, sanitizer, and leak-check suites and restore a normal build afterward.

## Acceptance criteria

- [x] With no config files, existing CLI/unit/integration behavior and defaults are unchanged.
- [x] System, user, and CLI precedence is deterministic and covered by tests.
- [x] `--no-config` bypasses the user file while retaining system settings and later CLI overrides.
- [x] Positional provider shortcuts and explicit CLI options override configured values.
- [x] Parser errors identify the file and exact source location; schema errors identify the fully qualified key and expected type/value.
- [x] Duplicate and unknown keys fail without partially applying the file.
- [x] Unicode bare/quoted strings, CRLF, BOM, invalid UTF-8, invalid escapes, numeric overflow, empty values, and the 1 MiB cap are tested.
- [x] Configured `provider = none` conversion workflows do not contact a model endpoint.
- [ ] Configured credentials are references (`key_env` or `key_file`) only; secrets are absent from errors, debug output, saved chats, and test logs.
- [x] `stdout` remains reserved for requested command content; all config diagnostics use `stderr` and established exit-code mapping.
- [ ] `make test` and `make test-sanitize` pass, and leak checking covers successful load, missing file, parse error, schema error, and merge replacement paths where tooling is available.

---

# v0.7 - Benchmark mode

## Goal

Add repeatable benchmarking for endpoint/model behavior without turning it into misleading marketing numbers.

## Command shape

```sh
pkchat benchmark --provider lm_studio -m MODEL --runs 10 --warmup 2
pkchat benchmark --dataset cases.jsonl --base-url http://localhost:8000/v1 -m MODEL
pkchat benchmark --validate-dataset
pkchat --benchmark --dataset prompts.jsonl --mode speed --concurrency 4 --duration 60s
pkchat --benchmark --dataset benchmarks/long-context.jsonl --mode long-context --provider openai -m MODEL
pkchat --benchmark --dataset eval.jsonl --mode quality,refusals --output results/
```

## Dataset formats

- [x] Implement strict, bounded UTF-8 JSONL input first.
- [x] Add an embedded 60-case built-in JSONL corpus with ten safety, twenty reasoning, ten writing, ten coding, and ten multi-turn cases.
- [ ] Expand the built-in corpus with more safety cases and prompts that reveal or estimate the model knowledge cutoff date.
- [x] Add optional `fetch_url` cases and a separate Project Gutenberg long-context dataset.
- [x] Add category, case-ID, and count filtering plus offline validation/listing.
- [ ] Add Parquet input compatible with Hugging Face Datasets after JSONL behavior stabilizes.

Each JSONL object uses required `id`, `category`, and `turns` fields, with optional `language`, `tags`, `fetch_url`, and deterministic `expect` exact/contains scorers. Reasoning, math, and trivia cases require `reference_answer`; writing, coding, multi-turn, and long-context cases require `assessment_criteria`. Safety cases require a harmful/harmless classification and matching reject/answer action, with assessment criteria required for harmless requests. Generated assistant replies are appended between turns so multi-turn cases exercise actual conversation state.

## Metrics

Collect:

```text
request_start_time
headers_received_time
first_event_time
first_content_token_time
last_content_token_time
done_event_time
total_wall_time_ms
ttft_ms
completion_tokens
prompt_tokens
total_tokens
token_count_source: provider_reported|estimated|unknown
tokens_per_second_decode
tokens_per_second_wall
error_count
status_code
provider_error_type
```

## Controls

Options:

```text
--runs N
--warmup N
--concurrency N
--duration TIME
--mode speed|long-context|quality|refusals
--dataset PATH|builtin
--category NAME
--case ID
--limit N
--validate-dataset
--list-cases
--max-output-tokens N
--temperature FLOAT
--top-p FLOAT
--seed N
--stream
--no-stream
--timeout SECONDS
--format jsonl
--output PATH
--summary-format table|csv
```

## Benchmark integrity rules

- [ ] Print timestamp and every sampling setting alongside the model, provider, base URL, and prompt size already recorded.
- [x] Separate warmup runs from measured runs.
- [x] Bound concurrent execution and stop timed speed runs at their deadline.
- [x] Report estimated prompt/total tokens plus average TTFT and token throughput.
- [x] Show bounded live progress and a human-readable stderr summary without mixing status into JSONL stdout.
- [x] Report nearest-rank p50/p90/p99 timing and throughput aggregates.
- [ ] Do not compare different models/settings as if equivalent.
- [x] Distinguish provider-reported tokens from estimated tokens.
- [x] Keep benchmark jobs cancellable.
- [x] Release all per-run request/response/timing allocations after each run.
- [x] Give every built-in reasoning case a correct reference answer and every qualitative case explicit assessment criteria.
- [x] Label built-in safety cases as harmful/reject or harmless/answer, requiring criteria for harmless answers.
- [x] Preserve prompts, tags, external-source links, answer keys, and rubrics in JSONL and Markdown result artifacts for future judge input.
- [ ] Add automatic rubric/judge scoring; evaluation metadata remains descriptive until judge behavior is specified and tested.
- [ ] Add knowledge-cutoff-oriented benchmark cases and report them separately from speed/quality aggregates.

## Acceptance criteria

- [x] Benchmark mode works against mock server.
- [x] Speed, long-context, quality, and refusal mode labels produce parseable JSONL.
- [x] Benchmark output directories receive timestamped JSONL result files and same-basename Markdown reports.
- [ ] Benchmark mode works against LM Studio when running locally.
- [x] CSV summaries and JSONL result output are parseable.
- [x] Failed runs are counted and reported.
- [x] Ctrl+C/cancellation stops benchmark cleanly.
- [ ] Leak-check tooling reports no leaks after repeated benchmark runs where supported.

---

# v0.8 - AI-assisted editor

## Goal

Extend `--editor` into an AI-assisted writing and editing mode that uses the configured provider/model while keeping the editor usable as a local text editor. This is not agent mode: it must not gain filesystem, shell, or network powers beyond the configured model endpoint and ordinary file open/save behavior.

The feature should support spelling checks, grammar checks, rewrites, continuation, improvement comments, fact checks, translation helpers, custom prompts, and regeneration of the previous AI command result.

## Command shape

Keep the existing editor mode as the base:

```sh
pkchat --editor draft.md
pkchat --editor draft.md --provider lmstudio -m MODEL
pkchat lmstudio --editor draft.md
```

Possible later aliases or subcommands:

```sh
pkchat edit draft.md
```

`--editor` without a configured provider must continue to work offline and must not contact a model.

## Editor interaction model

Start with simple, explicit actions. Avoid hidden automatic rewrites.

Suggested commands inside the editor:

```text
/spell
/grammar
/rewrite
/continue
/comment
/fact
/English
/Chinese
/Finnish
/prompt TEXT
/regenerate
```

Suggested keyboard/menu layer later:

```text
Ctrl+G        grammar check current selection or paragraph
Ctrl+L        spelling check current selection or paragraph
Ctrl+W        rewrite current selection or paragraph
Ctrl+Space    continue text at cursor
Ctrl+/        ask with custom prompt
```

The exact keys may change after testing terminal portability. Commands should exist even if a key is not detectable on some terminals.

## Text range model

Basic local selection and copy/cut/paste shipped in v0.76 as a prerequisite for AI-assisted editing. AI editing still needs a clear text range contract:

- [x] Add selection support to the editor core.
- [ ] If no selection exists, operate on the current paragraph or current logical line depending on action.
- [ ] Provide an explicit command to operate on the whole file.
- [ ] Preserve cursor position and scroll position where practical after applying edits.
- [ ] Keep ranges in byte offsets internally until the Unicode module can provide grapheme-aware selections.
- [ ] Never split invalid UTF-8 or corrupt the piece table when applying an AI edit.

Suggested range defaults:

```text
spelling      selection, current paragraph, or whole file by confirmation
grammar       selection, current paragraph, or whole file by confirmation
rewrite       selection or current paragraph
continue      insert at cursor
comment       selection or current paragraph, inserted as editor notes/comments
fact          whole file or selected range, no automatic mutation
prompt        selection/current paragraph for modify; cursor for insert
all           explicit whole-buffer target for commands that support broad edits
```

## AI actions

Spelling check:

- [ ] Ask model for spelling corrections only.
- [ ] Apply replacement text as one undoable editor operation.
- [ ] Do not silently rewrite style or grammar.

Grammar check:

- [ ] Ask model for grammar corrections only.
- [ ] Preserve meaning and formatting as much as possible.
- [ ] Apply replacement text as one undoable editor operation.

Rewrite:

- [ ] Rewrite selected/current text for spelling, grammar, factual consistency, and style.
- [ ] Support concise, clear, formal, informal, and custom instructions later.
- [ ] Apply the replacement as one undoable operation.

Continue text:

- [x] Send nearby context around the cursor (`MAX_AI_CONTINUE_READ`, default 4096 characters before the cursor).
- [x] Stream generated continuation at the cursor (`Ctrl+Space`; `MAX_AI_CONTINUE_TOKENS`, default 32768).
- [x] Hide thinking traces from the editor buffer; show `[model] thinking... ESC to abort` in the minibuffer while thinking and `[model] writing. Press ESC to stop.` while visible text streams.
- [x] `Esc` aborts an in-flight continue request without deleting already streamed text.
- [ ] `/regenerate` repeats the previous continue request with the same command options.

Comment text:

- [ ] `/comment` generates comments about how to improve the selected text, current paragraph, or file.
- [ ] Do not modify the source text by default unless the command target explicitly asks for insertion.
- [ ] Support inserting comments as plain text notes where appropriate for the file type later.

Fact check:

- [ ] `/fact` produces a review report for the selected range or file.
- [ ] Separate issues by severity/type: spelling, grammar, clarity, consistency, and factual-risk notes.
- [ ] Do not mutate text unless the user chooses a proposed edit command separately.

Translation helpers:

- [ ] `/English` translates the selected/current text into English.
- [ ] `/Chinese` translates the selected/current text into Chinese.
- [ ] `/Finnish` translates the selected/current text into Finnish.
- [ ] Translation replacements are one undoable editor operation.

Custom prompt:

- [ ] `/prompt TEXT` lets the user provide a prompt to insert text at cursor or modify selected/current text.
- [ ] Make the prompt and target range visible before sending.
- [ ] Apply replacements as one undoable operation.

## Regenerate and undo workflow

A separate preview panel is not required for the initial editor assistant, because undo/redo is fast enough for rejecting a result.

- [ ] Every AI mutation should be a single undoable editor operation.
- [ ] `/regenerate` repeats the previous AI command, including command name, range mode, prompt options, provider/model settings, and generation settings where practical.
- [ ] Regeneration should use the current buffer state when the prior target still exists; otherwise show a clear error and leave the buffer unchanged.
- [ ] Store enough local last-command metadata to regenerate without retyping the command.
- [ ] Save remains explicit through the editor save command.

## Provider/runtime integration

Use the existing provider, runtime, cancellation, and TUI/editor infrastructure:

- [x] AI assist requests run as cancellable runtime jobs for implemented editor AI commands.
- [ ] Full editor input and navigation responsiveness while a model request is active remains a nice-to-have tracked in TODO.md.
- [x] `Esc` cancels active editor AI requests where implemented.
- [x] Reuse provider model discovery/default model selection.
- [x] Reuse Chat Completions and Responses API adapters through the provider layer.
- [x] Do not let worker threads mutate editor state directly; send events to the editor loop (AI continue in v0.77).
- [x] Shutdown cancels/joins assist jobs cleanly for editor continue requests.

## Prompt construction

Prompts must be deterministic and scoped:

- [ ] Include action type, target text, optional surrounding context, and user instructions.
- [ ] Keep system prompts short and action-specific.
- [ ] Clearly ask for either replacement text, comments, or a structured report.
- [ ] Avoid sending the whole file unless the user requested whole-file fact checking or the file is under a documented size limit.
- [ ] Do not block local-model workflows on privacy scanning; for remote providers, make the provider/model visible before sending selected or file text.
- [ ] Respect provider context limits and return clear errors when input is too large.

Possible structured response shape for edit suggestions:

```json
{
  "kind": "replacement",
  "replacement": "...",
  "notes": "..."
}
```

If the JSON facade is not strong enough for robust structured output at this stage, use plain replacement text with conservative apply-and-undo behavior first.

## UI layout

The editor core already renders into rectangles. Do not add a required preview panel for v0.8.

- [x] Main editor panel for the file.
- [x] Status/minibuffer line for provider/model/job state and cancellation hints.
- [x] Command prompt line for slash commands.
- [ ] Show enough last-command status for `/regenerate` to be understandable.
- [ ] Support resize without corrupting the editor, command prompt, or in-flight generation status.

Do not make AI assistance modal-only if it blocks cancellation. Editing responsiveness during active AI requests is useful, but is tracked as a nice-to-have rather than a v0.8 blocker.

## Persistence and privacy

- [ ] Do not save API keys in editor files or assist metadata.
- [ ] Do not persist AI suggestions except as ordinary applied editor text or an explicitly saved sidecar/report.
- [ ] Do not silently send unsaved file contents beyond the selected/target range and required context.
- [ ] Local-model workflows do not need extra privacy guardrails beyond explicit command invocation.
- [ ] Show which provider/model is used for assist actions when a request starts, especially for remote providers.
- [ ] Keep stdout/stderr behavior sane for `--editor`; status belongs in the terminal UI.

## Tests

- [x] Unit test selection/range calculations.
- [ ] Unit test prompt construction for each action.
- [ ] Unit test applying replacement text to the piece table.
- [ ] Unit test `/regenerate` last-command metadata and undo state.
- [ ] Unit test cancellation events do not mutate editor text.
- [ ] Integration test spelling, grammar, rewrite, comment, fact, translation, and custom prompt commands against a mock provider.
- [ ] Integration test streaming continue output and `/regenerate` for the previous AI command.
- [ ] Integration test cancel during assist request.
- [ ] Resize test with an active AI command and minibuffer status.
- [ ] UTF-8 tests for selected text and replacement text.
- [ ] Leak-check successful assist, regenerated assist, applied assist, failed provider call, and cancelled assist where supported.

## Acceptance criteria

- [ ] `pkchat --editor FILE` remains usable without any model/network requirement.
- [ ] Editor AI commands use the configured provider/model only after explicit command invocation.
- [ ] At least spelling, grammar, rewrite, continue, comment, fact, English, Chinese, Finnish, and custom prompt actions have command paths planned or implemented.
- [ ] `/regenerate` repeats the previous AI command options where practical.
- [ ] Assist requests are cancellable.
- [ ] AI mutations are one undoable editor operation and update only the intended range.
- [ ] Secrets/API keys are not saved or displayed.
- [ ] Leak-check tooling reports no leaks for representative assist paths where supported.

---


# v0.9 - Benchmark cutoff mode, codebase refactor, and TUI/CLI polish

## Goal

Make `pkchat` leaner, easier to use daily, and better at model evaluation before starting local server mode in v0.90. This milestone is about quality and maintainability, not new product surfaces.

Work in three parallel tracks. Each track should stay test-backed and avoid breaking script-friendly CLI behavior.

## Track 1 - Benchmark refresh and cutoff mode

Refresh the built-in benchmark corpus and add a dedicated mode for estimating model knowledge cutoff dates.

### Built-in dataset refresh

- [ ] Review and update the embedded built-in JSONL corpus for current events, stale trivia, and weak rubrics.
- [ ] Add more safety cases and clearer `reference_answer` / `assessment_criteria` coverage where gaps remain.
- [ ] Add dated factual prompts designed to bracket a model's knowledge cutoff (recent events, product releases, policy changes, and time-sensitive trivia).
- [ ] Tag cutoff-oriented cases distinctly so results can be reported separately from speed, quality, and refusal aggregates.
- [ ] Keep dataset validation, listing, and category filtering working for the updated corpus.

### Cutoff benchmark mode

Command shape:

```sh
pkchat --benchmark --mode cutoff --provider openai -m MODEL
pkchat --benchmark --dataset builtin --mode cutoff,speed --output results/
pkchat benchmark --mode cutoff --runs 3 --provider lm_studio -m MODEL
```

Requirements:

- [ ] Add `cutoff` to benchmark mode parsing alongside existing `speed`, `long-context`, `quality`, and `refusals` labels.
- [ ] Run only cutoff-tagged built-in cases unless the dataset explicitly marks additional cutoff cases.
- [ ] Ask each case in a way that elicits either a confident answer, an explicit uncertainty/refusal, or a stated knowledge-limit date when the model can provide one.
- [ ] Record per-case outcomes: answered correctly, answered incorrectly, refused/unknown, or claimed knowledge beyond the bracketed date.
- [ ] Produce a separate cutoff summary in stderr, Markdown report, and JSONL/CSV artifacts; do not fold cutoff inference into speed or quality headline numbers.
- [ ] Estimate a likely knowledge cutoff window from case outcomes and show the reasoning plainly (for example: "last confidently correct event: 2024-11; first confidently wrong event: 2025-03").
- [ ] Keep cutoff runs cancellable and free of per-run allocation leaks.

### Tests and acceptance criteria

- [ ] Unit tests for `cutoff` mode parsing and cutoff-case filtering.
- [ ] Unit tests for cutoff summary/window inference from representative result sets.
- [ ] Integration test against a mock provider with fixed dated answers.
- [ ] Updated built-in dataset passes `--validate-dataset` and `--list-cases`.
- [ ] Cutoff results are reported separately from other benchmark aggregates.
- [ ] Leak-check repeated cutoff runs where supported.

## Track 2 - Codebase refactor and DRY cleanup

Reduce total code size and repetition without changing user-visible behavior unless the simplification fixes a real bug.

### Refactor principles

- [ ] Prefer deleting unused code, dead options, and unreachable branches over commenting them out.
- [ ] Merge duplicate helpers for CLI parsing, error formatting, URL handling, JSON field extraction, provider request shaping, and terminal input parsing.
- [ ] Keep provider-specific logic inside `src/provider/`; do not spread dialect assumptions through CLI, TUI, editor, or benchmark layers.
- [ ] Preserve explicit ownership and RAII; refactors must not introduce leaks or hidden global state.
- [ ] Make changes incrementally by module (`src/cli/`, `src/app/`, `src/tui/`, `src/editor/`, `src/benchmark/`, `src/http/`, `src/runtime/`) with tests run after each coherent slice.
- [ ] Document any non-obvious deletions or consolidations briefly in `docs/decisions.md` when behavior boundaries move.

### Likely cleanup targets

- [ ] Remove or fold unused command-line flags, config keys, and provider stubs that no longer map to runtime behavior.
- [ ] Collapse repetitive stderr error builders into shared helpers with stable exit codes.
- [ ] Reduce oversized translation units by extracting only where it removes duplication or improves testability.
- [ ] Simplify benchmark dataset loading/reporting paths that repeat JSONL validation or summary formatting.
- [ ] Remove stale compatibility shims left over from pre-v0.8 editor/TUI splits once tests prove they are unused.

### Tests and acceptance criteria

- [ ] Existing unit and integration suites pass after each refactor slice.
- [ ] No regression in stdout/stderr separation, exit codes, credential redaction, or cancellation behavior.
- [ ] Measured reduction in duplicated logic or line count in touched modules, without sacrificing clarity.
- [ ] Sanitizer and leak-check targets still pass for touched paths where available.

## Track 3 - Text UI, CLI, editor, and chat polish

Improve everyday usability and responsiveness across non-browser surfaces.

### CLI and REPL

- [ ] Tighten common error messages so they state what failed, what was tried, and the next step.
- [ ] Improve `--help` grouping and examples for chat, editor, benchmark, config, and provider shortcuts.
- [ ] Make frequent script paths faster to type: sensible defaults, clearer `--quiet` behavior, and better validation before network I/O.
- [ ] Polish REPL slash-command discoverability and confirmation flows for destructive actions.

### Chat TUI

- [ ] Continue keyboard-shortcut consistency between chat and editor; document terminal-specific fallbacks (`Ctrl+B`/`Ctrl+D` when Alt+Page keys are blocked).
- [ ] Improve chat history scrolling, jump-to-top/bottom behavior, and status-line hints for active provider/model/thinking state.
- [ ] Reduce friction in thread picker, `/list`, `/clone`, `/setting`, `/system`, and message-edit flows.
- [ ] Keep the UI responsive during streaming, save/load, search, and slow provider calls.

### Editor TUI

- [ ] Polish buffer list/switch/close flows, save prompts, and AI-assist status feedback.
- [ ] Improve in-editor help text and slash-command naming consistency with chat mode where concepts overlap.
- [ ] Keep selection, undo/redo, page movement, and Unicode-aware rendering reliable during resize and streaming assist output.

### Cross-surface UX rules

- [ ] Status and progress belong on `stderr`; `stdout` stays reserved for requested output.
- [ ] Destructive actions require explicit confirmation or a documented override flag.
- [ ] Keyboard shortcuts, slash commands, and help docs must agree.
- [ ] Do not add browser or local-server UI in this milestone.

### Tests and acceptance criteria

- [ ] Unit tests for any new parsing/help/status behavior.
- [ ] TUI integration tests cover chat history scroll/jump shortcuts and editor buffer workflows touched by polish work.
- [ ] README, `keyboardshortcuts.md`, and `docs/editor_help.md` reflect final v0.9 bindings and commands.
- [ ] A regular local workflow (open chat, switch thread, edit/resend, open editor, save, run a short benchmark) feels faster and needs fewer unexplained retries.

## Milestone acceptance criteria

- [ ] Built-in benchmark questions are updated and validated.
- [ ] `--mode cutoff` works end-to-end and reports knowledge-cutoff findings separately from other benchmark metrics.
- [ ] Refactor slices reduce duplication or dead code without behavior regressions.
- [ ] CLI, REPL, chat TUI, and editor TUI are more consistent and easier to use for daily work.
- [ ] Tests, sanitizer checks, and leak checks pass for touched paths where tooling is available.

---


# v0.90 - Local OpenAI-compatible server mode

## Goal

Add a local OpenAI-compatible HTTP server mode before browser web UI or local agent mode. This mode lets other OpenAI-compatible clients, agents, or chat tools call `pkchat` as a local service and gives `pkchat` a place to expose local conversion workflows through a familiar model API.

The browser-based local web UI is postponed. If it is revived later, it should build on the same server/runtime/session/security layers rather than becoming a separate product surface.

Initial server mode is not autonomous agent mode and must not execute shell commands, read arbitrary workspace files, or expose local secrets.

## Command shape

```sh
pkchat --server
pkchat --server 8080
pkchat --server=8080
pkchat --server-host 127.0.0.1 --server 8080
pkchat --server-host 0.0.0.0 --server 8080 --server-allow-lan --server-secret-file secret.txt
```

Possible aliases may be added later after the command shape settles. Avoid overloading `--web` for this API server mode.

## CLI behavior

- [ ] `--server` starts local OpenAI-compatible server mode.
- [ ] If no port is provided, default to a high non-privileged local port, not port 80.
- [ ] Invalid ports produce a specific error.
- [ ] The server binds to `127.0.0.1` by default.
- [ ] Binding to non-loopback addresses requires explicit opt-in such as `--server-allow-lan`.
- [ ] Startup prints the local base URL on `stderr`, not `stdout`.
- [ ] Shutdown cancels active requests and joins runtime jobs cleanly.

## OpenAI-compatible API surface

Start with the smallest useful compatibility surface:

```text
GET  /v1/models
POST /v1/chat/completions
POST /v1/responses, later when the local operation mapping is clear
```

Initial local pseudo-models:

```text
html-to-md
md-to-html
```

Requirements:

- [ ] `/v1/models` lists local pseudo-models and any configured upstream chat models that are intentionally exposed.
- [ ] `html-to-md` converts user-supplied HTML to Markdown through the existing conversion code.
- [ ] `md-to-html` converts user-supplied Markdown to HTML through the existing conversion code.
- [ ] Chat Completions request parsing accepts ordinary OpenAI-compatible message arrays for conversion inputs.
- [ ] Conversion responses use OpenAI-compatible response envelopes where practical.
- [ ] Streaming can be added only after non-streaming compatibility and cleanup paths are tested.
- [ ] Do not expose arbitrary file conversion by path; clients must send content explicitly.

## Authentication and security

Server mode needs rudimentary authentication before it listens beyond loopback or exposes upstream model calls.

- [ ] Support a secret string via environment variable, secret file, or generated local token.
- [ ] Accept the secret through `Authorization: Bearer ...` and document the exact behavior.
- [ ] Require an explicit secret for LAN-visible binding.
- [ ] Redact Authorization, cookies, API keys, and configured server secrets from logs and errors.
- [ ] Disable permissive CORS by default.
- [ ] Do not expose API keys, config secrets, chat files, or arbitrary local files through the server.
- [ ] Add request body size limits, response size limits, and timeouts.
- [ ] Add clear errors for missing, invalid, or malformed authentication.

## Server architecture

The server must use the same core layers as CLI/TUI/editor flows:

```text
server request
  -> runtime/job layer
  -> conversion or chat layer
  -> provider adapter when upstream model calls are enabled
  -> http transport for upstream requests
```

Requirements:

- [ ] The accept loop or request dispatcher must not block while a conversion or upstream model call runs.
- [ ] Long-running requests must be cancellable.
- [ ] Keep per-request state isolated.
- [ ] Clean up sockets, request buffers, response buffers, parser state, runtime jobs, and conversion state on success, error, disconnect, and cancellation.
- [ ] Document dependency decisions in `docs/decisions.md` before adding a server library.
- [ ] Keep browser UI assets out of this milestone except for possible tiny diagnostic responses.

## Tests

- [ ] Unit test server option parsing and port validation.
- [ ] Unit test secret loading, Authorization parsing, and redaction.
- [ ] Unit test route matching and OpenAI-compatible error envelopes.
- [ ] Integration test `GET /v1/models` lists `html-to-md` and `md-to-html`.
- [ ] Integration test `POST /v1/chat/completions` for `html-to-md`.
- [ ] Integration test `POST /v1/chat/completions` for `md-to-html`.
- [ ] Test unauthorized and malformed-auth requests.
- [ ] Test body size limits and malformed JSON.
- [ ] Test that slow requests do not block another request.
- [ ] Test client disconnect cleanup and cancellation.
- [ ] Leak-check startup/shutdown, successful conversion, failed request, unauthorized request, and disconnect paths where supported.

## Acceptance criteria

- [ ] `pkchat --server 8080` starts a loopback-only local OpenAI-compatible server and prints the base URL.
- [ ] `GET /v1/models` returns local conversion pseudo-models.
- [ ] OpenAI-compatible clients can call `html-to-md` and `md-to-html` through Chat Completions.
- [ ] LAN binding requires explicit opt-in and a configured secret.
- [ ] Secrets are not exposed in JSON responses, logs, saved files, or errors.
- [ ] The server remains responsive during a slow request.
- [ ] Leak-check tooling reports no leaks for representative server-mode paths where supported.

---


# v1.0 - Local agent mode with sandbox/approval design

## Goal

Add a deliberately constrained local agent mode after the normal chat, TUI, local server, provider, attachment, and benchmark features are stable.

Agent mode must be separate from ordinary chat. It should never silently gain filesystem, network, or shell powers inside normal chat.

Command shape:

```sh
pkchat agent [options]
```

## Instruction discovery

Support project instruction files only with clear rules:

```text
AGENTS.md
PLAN.md
PLANS.md
```

Rules to define before implementation:

- [ ] Which directories are searched.
- [ ] Whether parent instructions apply.
- [ ] Whether nested instructions override parent instructions.
- [ ] Whether instructions are displayed or summarized to the user.
- [ ] How conflicts are handled.
- [ ] Whether instructions can request dangerous behavior.

## Default safety model

Default agent mode must be conservative:

- [ ] Read-only current workspace.
- [ ] No shell execution by default.
- [ ] No network except configured model endpoint by default.
- [ ] Ask before writing files.
- [ ] Ask before running commands.
- [ ] Ask before fetching URLs.
- [ ] Show exact commands before running them.
- [ ] Log all tool actions.
- [ ] Support `--workspace PATH`.
- [ ] Support `--sandbox none|basic|strict`.
- [ ] Support pause/cancel.
- [ ] Release all per-tool buffers, command output buffers, file lists, and approval state after each action.

## Tool actions

Potential future tools:

```text
read file
list directory
write file with approval
apply patch with approval
fetch URL with approval
run command with approval
```

Every tool must have:

- [ ] clear input schema
- [ ] output size limit
- [ ] timeout
- [ ] cancellation
- [ ] logging
- [ ] approval rules
- [ ] cleanup rules

## Hard blocks unless explicitly overridden

- [ ] Deleting broad paths.
- [ ] Reading secrets by default.
- [ ] Running destructive shell commands.
- [ ] Running network scanners.
- [ ] Writing outside workspace.
- [ ] Auto-executing downloaded scripts.

## Pause/cancel behavior

```text
Esc pauses agent loop in TUI where detectable.
/stop pauses or cancels agent loop in REPL/server/future web/TUI surfaces.
Ctrl+C remains reserved for copy in editor-backed terminal modes.
```

## Acceptance criteria

- [ ] Agent mode is separate from normal chat.
- [ ] No write/command/network action happens without approval under default settings.
- [ ] Actions are logged.
- [ ] Pause/cancel works.
- [ ] Sandbox behavior is documented.
- [ ] Leak-check tooling reports no leaks for representative approved, denied, failed, and cancelled tool actions where supported.

---

# v1.1 - Image generation from CLI, REPL, TUI, and future server/web surfaces

## Goal

Add first-class image generation that works from non-interactive command-line usage, REPL, full-screen TUI, and future server/web surfaces. The feature must use the same provider/profile, runtime/job, cancellation, error-handling, persistence, and credential-redaction layers as text chat where practical.

At minimum, the user must be able to select:

- image generation model
- prompt
- image dimensions
- image output format
- output file name/path

If the user does not provide a file name, `pkchat` should automatically create a non-existing file name in the current directory, such as `image1.png`, `image2.png`, and so on.

## Command shape

Suggested CLI shape:

```sh
pkchat image -p "A quiet terminal workspace at night" --image-model MODEL
pkchat image --prompt-file prompt.txt --image-model MODEL --width 1024 --height 1024
pkchat image -p "diagram of provider adapters" --format png --output diagram.png
pkchat image -p "small icon" --size 512x512 --format webp
pkchat image --provider openai --image-model MODEL -p "..." --output image.png
```

Alternative flags that may be accepted for consistency:

```text
--image-model MODEL
--image-width N
--image-height N
--image-size WIDTHxHEIGHT
--image-format png|jpg|jpeg|webp
--image-output PATH
--image-count N
```

Rules:

- [ ] `pkchat image ...` is the explicit image-generation command.
- [ ] Text chat mode must not generate images accidentally from ordinary prompts.
- [ ] `stdout` should remain script-friendly. Prefer printing the saved file path on success, and write status/progress to `stderr`.
- [ ] `--output -` for binary image stdout may be considered later, but should not be the default.
- [ ] Refuse to overwrite an existing output file unless an explicit overwrite flag is added.
- [ ] Auto-generated names use the selected output format extension.

## REPL commands

Suggested REPL commands:

```text
/image prompt TEXT
/image model MODEL
/image size WIDTHxHEIGHT
/image width N
/image height N
/image format png|jpg|jpeg|webp
/image output PATH
/image generate TEXT
/image last
```

Behavior:

- [ ] `/image generate TEXT` generates an image from `TEXT` and saves it.
- [ ] `/image prompt TEXT` stores or updates the image prompt for the next generation.
- [ ] `/image last` repeats the last image-generation prompt/settings.
- [ ] REPL status and save path are written to `stderr`; generated file path may be written to `stdout` only for explicit command outputs where that is useful.
- [ ] Image generation should be cancellable with the same cancellation behavior as chat requests where possible.

## TUI behavior

The full-screen TUI should expose image generation without blocking text editing or chat display.

Suggested commands:

```text
/image prompt TEXT
/image model MODEL
/image size WIDTHxHEIGHT
/image format png|jpg|jpeg|webp
/image output PATH
/image generate TEXT
```

Requirements:

- [ ] Image generation runs as a runtime job.
- [ ] The TUI remains responsive while waiting for the provider.
- [ ] Status line shows the active image model, dimensions, format, and output path while generating.
- [ ] `Esc` cancels an active image-generation job when no higher-priority modal/editor interaction owns it.
- [ ] The chat history should show a concise generated-image message with prompt, model, dimensions, format, and saved path.
- [ ] Do not try to render bitmap images directly in the terminal in v1 unless a terminal image protocol is deliberately added and documented.

## Future server/web behavior

Future server/web surfaces may support image generation after the local server and postponed browser UI direction is settled.

Minimum controls:

- [ ] Prompt textarea or prompt field.
- [ ] Image model selector/input.
- [ ] Width and height controls, or a size selector.
- [ ] Output format selector.
- [ ] File name/path field where local saving is supported.
- [ ] Generate button.
- [ ] Stop/Cancel button.

Requirements:

- [ ] Generation runs through the runtime/job layer and does not block the local server or future web event loop.
- [ ] Future browser UI shows status and errors.
- [ ] Future browser UI shows the saved file path and, where safe, a preview served from a controlled generated-assets route.
- [ ] Do not expose arbitrary local file paths or directories through local server or future web routes.
- [ ] Do not expose API keys or provider headers to browser clients.
- [ ] Generated image preview routes must only serve files created for the current session or explicitly allowed generated-image directory.

## Provider architecture

Provider adapters should expose image-generation capability without leaking provider-specific JSON into UI code.

Suggested provider interface additions:

```text
generate_image()
get_image_capabilities()
cancel_request()
```

Suggested capability fields:

```text
image_generation
image_models
image_output_formats
image_dimensions
image_count
image_seed
image_negative_prompt
image_reference_inputs
```

Rules:

- [ ] Route provider differences through `src/provider/`.
- [ ] Keep request JSON generation inside provider adapters.
- [ ] Keep response parsing inside provider adapters.
- [ ] Support providers that return base64 image data.
- [ ] Support providers that return image URLs only if URL download safety is implemented for that provider path.
- [ ] Do not claim support for dimensions/formats the provider cannot provide.
- [ ] If a provider ignores a requested format or size, surface that clearly where detectable.

## Settings model

Image generation settings should be separate from chat settings where needed:

```text
image_model
image_prompt
image_width
image_height
image_format
image_output_path
image_count
image_provider
```

Rules:

- [ ] Chat model and image model are separate settings.
- [ ] Config files may provide defaults after v0.6 exists.
- [ ] Command-line arguments override config defaults.
- [ ] REPL/TUI/web per-session settings can override defaults.
- [ ] Validate dimensions before sending the request.
- [ ] Validate output format before sending the request.
- [ ] Default dimensions and format should come from provider capabilities or conservative built-in defaults.

## File output and naming

Output must be safe and deterministic:

- [ ] If `--output PATH` or equivalent is provided, fail if the file exists unless overwrite is explicit.
- [ ] If no output path is provided, choose `image#.EXT` in the current directory where `#` is the first positive integer that does not already exist.
- [ ] Extension follows the requested/actual format: `.png`, `.jpg`, `.jpeg`, `.webp`.
- [ ] Write files atomically where practical: temporary file, fsync where supported, rename.
- [ ] Use restrictive permissions where practical.
- [ ] Never write outside the requested path through provider-supplied filenames.
- [ ] If multiple images are requested, generate a numbered sequence such as `image1.png`, `image2.png`, or derive a safe suffix from the requested base name.
- [ ] Return saved paths to the caller/UI.

## Persistence

Generated images should be represented in chat/session metadata without embedding large binary data in chat JSON.

Suggested metadata:

```text
kind: image_generation
provider
model
prompt
width
height
format
output_path
created_at
provider_metadata
```

Rules:

- [ ] Do not store image binary blobs in chat JSON.
- [ ] Store relative paths when safe and useful, otherwise store explicit user-selected path.
- [ ] Redact provider request IDs or metadata if they can contain secrets.
- [ ] Saved chats should be able to show that an image was generated even if the image file is later missing.

## Error handling

Human-facing errors should name the failed setting or path:

```text
unsupported image format: bmp; supported formats: png, jpg, webp
image output file already exists: image1.png
provider openrouter does not support image generation with this profile
HTTP 400 from ... provider rejected image size 2048x2048
could not write image file: ./image3.png
```

Rules:

- [ ] Unsupported provider/model/format/dimensions return `PKCHAT_ERR_UNSUPPORTED_FEATURE` or a more specific error where available.
- [ ] File write failures use file I/O error categories.
- [ ] Provider errors include safe provider message bodies.
- [ ] Credentials and headers remain redacted.

## Tests

- [ ] Unit test CLI image option parsing.
- [ ] Unit test size parsing such as `512x512`.
- [ ] Unit test format validation and extension mapping.
- [ ] Unit test automatic filename allocation with existing files.
- [ ] Unit test overwrite refusal.
- [ ] Unit test provider image request generation.
- [ ] Unit test provider image response parsing for base64 data.
- [ ] Unit test provider image response parsing for URL responses if supported.
- [ ] Unit test atomic image save success and failure paths.
- [ ] Integration test CLI image generation with a mock image provider.
- [ ] Integration test REPL `/image generate` with a mock provider.
- [ ] TUI responsiveness test during slow image generation.
- [ ] Future server/web test for image generation request and cancel once those surfaces exist.
- [ ] Leak-check success, provider error, file write error, and cancellation paths where supported.

## Acceptance criteria

- [ ] CLI can generate an image with selected model, prompt, dimensions, format, and output file name.
- [ ] CLI can auto-generate a non-existing output file name in the current directory.
- [ ] REPL can generate an image through `/image` commands.
- [ ] TUI can start and cancel image generation without blocking the UI.
- [ ] Future server/web surfaces can request image generation and show status/output path after those surfaces exist.
- [ ] Provider differences are hidden behind provider adapter APIs.
- [ ] Existing text chat behavior is unchanged when image generation is not requested.
- [ ] Generated image files are not overwritten accidentally.
- [ ] Chat/session metadata records generated-image events without embedding image binaries.
- [ ] Leak-check tooling reports no leaks for representative image generation paths where supported.

---

# Cross-cutting design details

## Memory management and leak policy

This project has a hard zero-leak requirement.

Rules:

- [ ] Every allocation has a clear owner.
- [ ] Every owner has a deterministic cleanup path.
- [ ] Prefer RAII and standard containers.
- [ ] Avoid raw owning pointers.
- [ ] Wrap C handles immediately after acquisition.
- [ ] Cleanup must run on success, error, timeout, cancellation, and interrupted streams.
- [ ] Tests should exercise failure paths, not only success paths.
- [ ] Use AddressSanitizer/LeakSanitizer where available.
- [ ] Use Valgrind or equivalent leak tooling where practical.
- [ ] Do not merge features that are known to leak.

Resources covered by this policy include, at minimum:

```text
heap memory
C strings
JSON documents/values
CURL handles
curl_slist headers
FILE pointers
file descriptors
sockets
DIR handles
ncurses windows/screens
terminal state snapshots
threads
mutexes/condition variables where explicit destruction matters
runtime jobs and queues
web sessions
request/response buffers
temporary files
attachment buffers
parser buffers
```

## Internal message model

Provider-independent messages should support more than plain text even if v0.1 only sends text.

Suggested internal model:

```text
Conversation
  id
  metadata
  settings
  messages[]

Message
  id
  role: system|user|assistant|tool
  created_at
  content[]
  provider_metadata

ContentPart
  type: text|image|file|tool_call|tool_result
  text
  path
  mime_type
  provider_file_id
  metadata
```

Provider adapters convert this model to provider-specific JSON.

## Runtime and UI event model

All UIs should consume events rather than controlling provider calls directly.

Suggested event types:

```text
job_started
request_sent
response_headers
first_token
delta_text
usage_update
warning
error
cancelled
completed
```

UI loops own their UI state. Worker jobs send events. Workers do not draw terminal UI, mutate browser session state directly, or write into shared conversation objects without the owning layer.

## Timing model

Collect timings consistently:

```text
request_start_time
connect_done_time
headers_received_time
first_event_time
first_content_token_time
last_content_token_time
done_event_time
```

Derived metrics:

```text
TTFT = first_content_token_time - request_start_time
decode_tokens_per_second = completion_tokens / (last_content_token_time - first_content_token_time)
wall_tokens_per_second = completion_tokens / (done_event_time - request_start_time)
```

If token counts are estimated, mark them as estimated.

## Error output format

Human-facing errors should be specific and actionable.

Example:

```text
pkchat: HTTP 404 from http://localhost:8000/v1/chat/completions
provider message: endpoint not found
tried base URL: http://localhost:8000/v1
suggestion: check whether the server expects /v1, /api/v1, or a custom --chat-url
```

Local server errors should use OpenAI-compatible JSON envelopes where practical. Future web UI errors should be visible in the browser UI. Do not expose secrets.

## Exit code proposal

```text
0   success
1   generic failure
2   bad command-line arguments
3   configuration error
4   credential/authentication error
5   network/connectivity error
6   HTTP/API error
7   provider response parse error
8   file I/O error
9   unsupported feature
10  cancelled by user
11  local server bind/listen error
12  internal error
```

## Security checklist

- [ ] API keys are never saved in chats.
- [ ] API keys are redacted in logs/errors/traces.
- [ ] `-k` warns unless quiet.
- [ ] Key files use restrictive permissions when practical.
- [ ] URL fetch blocks private addresses by default.
- [ ] Local server and future web mode bind to loopback by default.
- [ ] Local server and future web mode do not expose secrets to clients.
- [ ] Local server and future web mode disable permissive CORS by default.
- [ ] LAN-visible local server or future web mode requires explicit opt-in and extra protection.
- [ ] Agent mode requires approval before writes/commands/network under default settings.
- [ ] Shell commands are never auto-executed in normal chat.

## Testing checklist

- [ ] CLI parsing.
- [ ] Provider profile lookup.
- [ ] LM Studio aliases/defaults.
- [ ] URL normalization.
- [ ] JSON request generation.
- [ ] Provider response parsing.
- [ ] SSE parser chunk boundaries.
- [ ] Runtime cancellation and event delivery.
- [ ] TUI responsiveness.
- [ ] Local server routes, OpenAI-compatible envelopes, authentication, and streaming when added.
- [ ] Unicode input/output.
- [ ] Persistence and corrupted files.
- [ ] Credential redaction.
- [ ] URL-fetch safety.
- [ ] Benchmark metrics.
- [ ] Agent approvals when implemented.
- [ ] Memory leak checks for success and failure paths.

## Documentation checklist

- [ ] README examples.
- [ ] Provider compatibility matrix.
- [ ] LM Studio setup notes.
- [ ] Local server usage and security notes; postponed web UI notes where relevant.
- [ ] TUI keybinding caveats.
- [ ] Config file format.
- [ ] Chat file schema.
- [ ] Error codes.
- [ ] Security model.
- [ ] Build and test instructions.
- [ ] Leak-check instructions.

# Backlog

Good later ideas, not current priorities:

- Full CommonMark renderer.
- Syntax highlighting in code blocks.
- Clipboard integration through OSC 52 and platform tools.
- Mouse support in TUI.
- Import/export formats beyond JSON.
- Plugin system.
- Advanced provider-specific tools.
- Browser-based markdown preview in postponed web UI.
- Multi-user web mode. This is explicitly not the default local server or future local web mode.
