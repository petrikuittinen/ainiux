# AGENTS.md

Project: `ainiux`

Repository-level instructions for AI coding agents. Treat this as current project guidance. The user's latest explicit instruction always wins. For milestones and history use `PLANS.md`; for user-facing usage use `README.md`; for design rationale use `docs/decisions.md`; for open work use `TODO.md`.

## Mission

Build and maintain `ainiux`: a fast, portable command-line and terminal chat client for OpenAI and OpenAI-compatible APIs, with a first-class standalone editor, document conversion, benchmarks, and judge grading.

The program must stay excellent as a scriptable CLI. Keep the core engine independent from UI surfaces so the same request, provider, streaming, persistence, runtime/job, cancellation, memory-management, and error-handling code is reused by:

- non-interactive CLI chat
- document extraction / conversion
- REPL
- full-screen chat TUI
- standalone editor (with optional AI assist)
- benchmark runner
- grade (judge) pass
- future local OpenAI-compatible server mode
- postponed browser web UI
- local agent mode with session-scoped Act/Plan task modes

## Current product snapshot

Status: **v1.12** (see `README.md` and `PLANS.md` implementation notes). One-shot (`run` / `--run` / `-r`) and interactive (`agent` / `--agent` / `-a`) local agent modes are landed with workspace writes, multi-turn project sessions (`.ainiux-pr/`), compact live tool activity, provider-supplied reasoning previews in interactive agent history, three-strategy transcript-preserving compaction, retained row-diff terminal rendering, punctuation-aware Markdown highlighting, chat↔editor↔agent cycling, project-persisted Confirm/Smart/Yolo permissions, OpenRouter/OpenAI/DeepSeek credit display, interactive Guard approvals, and session-scoped Act/Plan task modes. Live tool rows update in place, while display-only `notice` and `thinking` rows remain outside provider context. One-shot planning is available through `plan`, `--plan`, and `--plan-file`; Plan retains research tools but code-enforces planning-document-only writes. User profile stays `~/.ainiux/` (chat DB/media). The **v1.1** code index is a lightweight definitions-only index across all scanner languages, with static declaration importance and mutation-aware persistence. Local server mode is deferred behind v1.1, image generation moves to v1.2, and browser web UI remains postponed.

### Implemented modes

| Mode | Entry | Notes |
| --- | --- | --- |
| One-shot CLI chat | default + `-p` / `--prompt-file` | stdout = model output; stderr = status/errors |
| Document extract | `--input` / `--fetch-url` without chat prompt | text/Markdown/HTML conversion; image input for chat path |
| List models | `--list-models` | provider `/models` |
| REPL | `--repl` / `-i` | line-oriented interactive |
| Chat TUI | `--chat` (`--tui` alias) | non-blocking alternate-screen UI; SQLite threads |
| Standalone editor | `--editor [path]` | multi-buffer piece-table editor; optional AI assist |
| Benchmark | `benchmark` / `--benchmark` | concurrent JSONL dataset runner |
| Grade | `--grade` | second-pass judge scoring of benchmark results (not combined with `--benchmark`) |
| Interactive agent | `agent` / `--agent` / `-a` | **Separate mode from `--chat`**. Project-local `.ainiux-pr/`; Act by default; `/plan` and `/act` switch session task policy; shared TUI shell + selectors |
| One-shot agent | `run` / `--run` / `-r` / `--run-file` | headless Act mode; compact tool lines on stderr; stdout = final answer |
| One-shot plan | `plan` / `--plan` / `--plan-file` | headless Plan mode; read/research tools plus planning-document-only writes |
| Security review | `--security-review` | headless read-only whole-project review |
| Code index | `--index-code` / `--print-index` / `--clear-index` | project-local `.ainiux-pr/index.sqlite` |

### Implemented capabilities agents must respect

- Built-in provider registry and aliases; Chat Completions; text-only OpenAI Responses API (`--api responses`, `--responses`, `openai_responses`)
- Catalog-selected provider reasoning mapping (`--reasoning auto|VALUE|TOKENS`)
- `--provider none` offline profile for conversion/editor without a model endpoint
- Credential lookup from env / key file / stdin; redaction in logs and artifacts
- JSON chat import/export (`--save-chat` / `--load-chat`); SQLite-backed TUI chat library at `~/.ainiux/ainiux.db`
- Cancellable runtime jobs; libcurl HTTP + incremental SSE streaming
- Request-only context policies; full transcript preserved on disk
- Bounded text/HTML/Markdown attachments; JPEG/PNG/GIF image input (Chat Completions)
- Safe URL fetching; web search (`--search`, `/search`) with API providers and keyless fallbacks
- Automatic system/user TOML-alike configuration (`config.conf`), plus `themes.conf`, `editor-commands.conf`, `benchmarks.conf`, and `models.conf`
- Shared syntax highlighting for editor and chat; grapheme-aware editor navigation
- Editor AI assist (`Ctrl+Space`, slash commands); window splits; file locking sessions
- Concurrent JSONL benchmarks and configurable judge grading with runtime prompts from `benchmarks.conf`

### Not implemented yet (do not pretend they exist)

- Local OpenAI-compatible **server** mode (`--server` in `PLANS.md` v0.90)
- Browser local web UI (`src/web/` reserved; `docs/web-mode.md` is still a stub plan)
- Reference extraction beyond the landed Python/C/C++ v1.1 review slice; JavaScript/TypeScript, Java/C#, Go, Rust, and other languages still have definitions-only indexes
- `/goal`, `/loop`, and sub-agents. Their names are reserved for v1.1, but their behavior is not specified or implementation-ready
- Image generation (`ainiux image` / `/image`; moved to v1.2)
- Agent session resume/list UI and richer tool-call transcript chrome; Guard Ask y/n in interactive agent is landed (headless Ask still denies); one-shot `ainiux run` / `--run` and interactive `ainiux agent` / `--agent` with multi-turn tools + mode cycling are landed
- PDF / DOCX conversion modules
- Native Anthropic Messages adapter; full live capability probing for all models
- ncurses-based TUI (current UI uses POSIX `termios` + ANSI)

## Implementation stance

- Default language: **C++17**.
- Do not use Rust or Go.
- Keep the code portable across Linux, BSD, macOS, and other POSIX-like systems where practical.
- Use a `Makefile` as the primary build entry point.
- Do not require C++20, C++23, C23, non-portable compiler extensions, or a package manager unless the user explicitly approves it.
- Prefer stack objects, RAII, `std::string`, `std::vector`, and standard containers.

Performance matters, but correctness, clear errors, safe credential handling, robust streaming, responsive UI behavior, zero memory leaks, and clean architecture matter more.

## Non-negotiable: no memory leaks

Every allocation and acquired resource must be released as soon as it is no longer needed, including on error, cancellation, timeout, interrupted stream, failed JSON parse, failed file processing, and early return paths.

Rules:

- Prefer stack objects and RAII. Use `std::unique_ptr` for owned heap objects. Use `std::shared_ptr` only when shared ownership is truly required and documented.
- Do not introduce raw owning pointers in new C++ code.
- Wrap C resources in RAII: `CURL*`, `curl_slist*`, `sqlite3*`, `FILE*`, file descriptors, sockets, `DIR*`, terminal state, temporary files, and allocated buffers.
- At C API boundaries, define who allocates, who frees, and which free function is used.
- Changes that affect allocation, resource ownership, cancellation, or cleanup must add focused coverage for normal and important failure paths. Full sanitizer and Valgrind suites follow the slow-test policy below; they are not required after every minor change unless the user explicitly requests them.

## Active priorities

Work in this order unless the user explicitly changes priorities:

1. Keep script-friendly CLI, provider, HTTP/SSE, and error behavior solid.
2. Finish remaining v0.9 polish: benchmark cutoff/grade calibration, TUI/CLI polish, refactor hygiene, leak and cancellation hardening (see `TODO.md` / `PLANS.md`).
3. Tune v1.1 lightweight definition importance, lexical ranking, and mutation-aware persistent refresh.
4. Add `/goal`, `/loop`, and sub-agents only after the user supplies their detailed specifications; reuse the landed Guard, workspace-containment, cancellation, and logging design.
5. Local OpenAI-compatible **server** mode (v0.90), reusing provider/runtime/security layers.
6. Image generation (v1.2), then only later consider revived browser UI on the server/runtime foundation.

Do not infer `/goal`, `/loop`, or sub-agent semantics from their names. Do not rewrite or expand the built-in agent system prompt as part of v1.1 indexing work; the user plans a separate prompt-optimization pass for small local models. Do not treat postponed browser web UI as the next major surface.

## Repository layout

This is the **authoritative** layout. Put new code in the matching module. Do not invent a parallel tree.

```text
.
├── AGENTS.md
├── PLANS.md
├── TODO.md
├── README.md
├── TESTING.md
├── Makefile
├── LICENSE
├── config/                      # bundled install templates
│   ├── ainiux.conf
│   ├── themes.conf
│   ├── editor-commands.conf
│   ├── benchmarks.conf
│   └── models.conf
├── benchmarks/                  # JSONL datasets (builtin parts + long-context)
├── include/ainiux/
│   ├── version.hpp
│   └── model_setting.hpp
├── src/
│   ├── main.cpp                 # mode dispatch
│   ├── common.{hpp,cpp}
│   ├── app/                     # mode runners and session orchestration
│   ├── cli/                     # argument parsing and option values
│   ├── config/                  # TOML-alike config loading/schema
│   ├── provider/                # registry, adapters, model list jobs
│   ├── http/                    # libcurl transport + SSE
│   ├── runtime/                 # jobs, cancellation, event queues
│   ├── chat/                    # session, settings, SQLite, JSON chat I/O
│   ├── context/                 # request context policies / estimates
│   ├── editor/                  # piece table, buffers, AI assist, locks, splits
│   ├── tui/                     # full-screen chat UI
│   ├── ui/                      # shared confirmation / text selector
│   ├── benchmark/               # dataset, run, scoring, grading, report
│   ├── fetch/                   # safe URL fetch
│   ├── search/                  # web search providers and fallbacks
│   ├── input/                   # local text/image classification and read
│   ├── html/                    # HTML → text/Markdown
│   ├── markdown/                # Markdown → HTML/plaintext
│   ├── highlight/               # shared syntax highlighter
│   ├── json/                    # internal JSON facade
│   ├── output/                  # thinking/trace helpers
│   ├── security/                # credential redaction
│   ├── version/
│   ├── web/                     # reserved (browser UI postponed; empty)
│   ├── tools/                   # reserved / thin
│   └── unicode/                 # reserved (Unicode data lives under editor/detail)
├── tests/
│   ├── unit/<module>/           # mirrors src modules; test_runner driver
│   ├── integration/
│   ├── mock_server/
│   ├── mock/                    # LD_PRELOAD helpers (e.g. ENOSPC)
│   ├── fixtures/
│   ├── highlight/
│   ├── image_files/
│   └── text_files/
├── tools/                       # generate_editor_unicode_data.py, run_enospc_test.sh
└── docs/
    ├── decisions.md
    ├── api-compatibility.md
    ├── security.md
    ├── editor_help.md
    ├── web-mode.md
    └── unicode-license.txt
```

Headers for a module usually live next to their `.cpp` files under `src/<module>/` (not only under `include/`). Keep dependencies isolated behind small wrapper modules so they can be replaced later.

## Dependencies

Use as few external libraries as practical. Every new dependency must be justified in `docs/decisions.md` or next to the build configuration.

Current baseline:

- **libcurl** — HTTP/HTTPS, proxies, timeouts, streaming callbacks (`src/http/`).
- **libsqlite3** — TUI chat library (`src/chat/sqlite_store.*`, `~/.ainiux/ainiux.db`).
- **In-tree JSON facade** (`src/json/`) — request escaping and response parsing; expand or vendor a reviewed library only with an explicit decision.
- **POSIX termios + ANSI** — TUI and editor terminal I/O. **No ncurses dependency** today. The editor renderer is independent of the terminal harness.
- **Generated Unicode tables** — editor word completion / properties under `src/editor/detail/` (see `tools/generate_editor_unicode_data.py`). Full `utf8proc` / `libgrapheme` integration is future work, not required for ordinary changes.

Do not add a dependency only because it is convenient.

## Architecture rules

### Mode dispatch

`src/main.cpp` validates option combinations and dispatches to `src/app/`:

- `run_benchmark_mode`, `run_grade_mode`
- document extract via document helpers
- interactive session for `--editor`, `--chat` / `--tui`, and REPL

UI code must not reimplement provider HTTP, SSE parsing, or credential resolution. Long-running work goes through `src/runtime/` and delivers events to the owning loop.

### Provider adapters/profiles are mandatory

Route model/API differences through `src/provider/`. Do not scatter provider-specific JSON shape or auth assumptions through TUI, editor, or CLI formatters.

The registry is data-driven. Profiles include aliases, default base URLs, endpoint paths, key environment variables, local/remote flags, optional dummy keys, compatibility warnings, and capability flags. OpenAI-compatible providers share Chat Completions request code; Responses is a sibling adapter.

Built-in profiles include at least:

```text
none / offline
openai (aliases: openai_chat, openai_responses)
openrouter
deepseek, gemini, anthropic, xai (grok), moonshot (kimi)
llamacpp, lm_studio (lmstudio), ollama, vllm, sglang
groq, mistral, together, perplexity, cerebras, fireworks,
deepinfra, nvidia_nim, zai, qwen, dashscope
custom_openai_chat (custom)
```

Treat LM Studio as a first-class local profile (default `http://localhost:1234/v1`, optional key via `LMSTUDIO_API_KEY` / `LM_STUDIO_API_KEY` / `AINIUX_API_KEY`). Keep `lm_studio` separate from `llamacpp` in profile names even if adapters share code.

`--provider none` has no base URL or model transport. Use it for local conversion and editor workflows that must not invent a dummy endpoint. URL fetch and web search remain separate explicit network operations with their own safety rules.

The UI must not hard-code the difference between OpenAI Chat Completions, Responses, OpenRouter, Ollama, vLLM, LM Studio, or a custom URL beyond selecting a profile and showing status.

### Code index and v1.1 ranking

The project-local code index lives at `.ainiux-pr/index.sqlite` and remains a fast hint source, never ground truth. It stores metadata, files, and definitions with a compact static importance score. It does not store references, evidence, edges, caller counts, graph scores, or automatic request-context hints.

When extending v1.1:

- Extend `src/agent/index/`; do not create a parallel index or use the user chat database.
- Keep lightweight lexical definition scanners and avoid compiler-grade parsers or new language-server dependencies.
- Keep full-name, exact-component, and component-prefix relevance ahead of importance; preserve deterministic ties and multi-token coverage.
- Require the model and tools to verify indexed locations against current source before editing. Preserve `glob`, `search_text`/`grep`, `read_file`, compiler, and test fallbacks.
- Native mutations must immediately update the live touched-file snapshot. Persist affected definitions through a cancellable coalescing job; potentially mutating commands trigger an incremental check; task completion performs a full-tree freshness pass that reparses only changed files.
- Publish snapshots atomically. Cancellation or failure preserves the previous completed database state.
- Full/multi-file discovery and scanning use `floor(online_cores × 0.80)`, bounded by available work and with at least one worker when work exists. Zero work uses zero workers, and a single-file scan runs inline without creating a scanner worker thread.
- Do not rewrite the built-in agent system prompt during this milestone; prompt/tool-selection optimization is a separate user-directed pass.

### HTTP and streaming

`src/http/` owns transport and must remain usable without TUI or editor.

Requirements:

- Explicit connect and total timeouts.
- Cancellation while a stream is active.
- Proxy support where libcurl supports it.
- Credential redaction in debug logs.
- Real SSE parsing: never assume one network chunk equals one JSON object.
- Handle `data: [DONE]`, comments, blank separators, partial chunks, malformed events, and UTF-8 split across chunks.
- Release all libcurl handles, header lists, buffers, and callback state on success, failure, and cancellation.

### Runtime/job layer

Cancellable long-running work belongs in `src/runtime/` (cancellation token, thread-safe event queue, RAII job handle). Current model: one worker thread per active job; the owning UI loop alone mutates terminal/session state (see `docs/decisions.md`).

Jobs include HTTP connect/request/stream, model list, URL fetch, attachment/document processing, chat save/load on slow filesystems, editor AI assist, reformat jobs, benchmark/grade work, and future server request handling.

Rules:

- UI threads must never block on network, DNS/TLS, endpoint waits, streaming callbacks, large file I/O, fetch, or compaction.
- Workers must not mutate TUI, editor, or chat state directly; they send events.
- All jobs must support cancellation; shutdown cancels or joins cleanly.
- No leaks of workers, queues, tokens, mutexes, or per-job buffers.

### CLI rules

The CLI must remain useful in shell scripts.

- `stdout` is reserved for model output (and intentional convert/list output).
- `stderr` is for status, warnings, progress, and errors.
- Exit codes distinguish success, bad arguments, network failure, API failure, config failure, cancellation, and internal failure via `ErrorCode` and `src/app/exit_codes.cpp`.
- Support `--format text|json|ndjson` and `--no-stream` for scripted chat paths.

Examples:

```sh
ainiux http://localhost:8000 -p "What is the capital of Norway?"
ainiux --provider openai -m MODEL -p "Hello"
ainiux --provider lmstudio --list-models
ainiux --editor notes.md
ainiux --chat
ainiux benchmark --provider openai -m MODEL
ainiux --grade --provider openai -m JUDGE_MODEL --grade-input results.jsonl
ainiux --input page.html --output-format md
```

### Configuration

Configuration is TOML-alike, not JSON.

```text
system: $XDG_CONFIG_DIRS/ainiux/config.conf (default /etc/xdg/...)
user:   $XDG_CONFIG_HOME/ainiux/config.conf or ~/.config/ainiux/config.conf
```

Layering: system files (reverse `XDG_CONFIG_DIRS` order), then user file, then CLI (authoritative). `--no-config` skips the user file only. Separate documents:

- `themes.conf` — TUI/editor themes
- `editor-commands.conf` — editor AI slash commands
- `benchmarks.conf` — judge grading prompts (no compiled fallback grading prose)
- `models.conf` — model capabilities, reasoning selector options, and optional purpose presets

Bundled templates live in `config/` and install via `make install`. See `docs/decisions.md`.

### Persistence

- **SQLite TUI library:** `~/.ainiux/ainiux.db` (WAL, per-message rows). Primary local chat library for `--chat`.
- **JSON chat files:** explicit `--save-chat` / `--load-chat` import/export. Include schema version, timestamps, provider, base URL, model, settings, messages, attachments, usage, compaction events as applicable.
- Atomic file saves where files are used: write temp → fsync where supported → rename → fsync parent where supported.
- Restrictive permissions for files that may contain prompts, history, URLs, or secrets.
- Release every allocation, open DB handle, temporary file, and JSON object on success and failure.

### Error handling

Never emit vague errors such as `request failed` when more detail is available.

Internal codes live in `ainiux::ErrorCode` (`src/common.hpp`), including:

```text
Ok, BadArgs, BadUrl, Dns, Connect, Tls, Timeout,
HttpStatus, Auth, RateLimit, JsonParse, SseParse,
ProviderSchema, UnsupportedFeature, FileRead, FileWrite,
FileLock, Config, Cancelled, StreamComplete, Internal
```

Human-facing errors should include what failed, which URL/path/model/option was involved, HTTP status and safe provider body when available, what was tried, and a concrete next step when possible.

### Credential handling

Prefer environment variables, key files, or stdin. Do not encourage command-line API keys.

Supported patterns include provider-specific env vars (`OPENAI_API_KEY`, `OPENROUTER_API_KEY`, `LMSTUDIO_API_KEY`, `LM_STUDIO_API_KEY`, …), `AINIUX_API_KEY`, `--key-env`, `--key-file`, `--key-stdin`, and `--header`.

If `-k` / `--key` is used, warn that argv may be visible to other local users unless `--quiet`.

Always redact from logs, errors, traces, saved chats, and debug output:

```text
Authorization, api-key, x-api-key, cookie, set-cookie
```

### URL / base URL handling

Be helpful but deterministic:

1. If the path already ends in `/v1`, use it as the base URL.
2. If the path is empty or `/`, try appending `/v1`.
3. Probe models endpoints only when needed.
4. Allow overrides such as `--base-url`, `--chat-url`, `--models-url`, `--responses-url`.
5. Do not hide surprising rewrites; show them on stderr unless `--quiet`.

### URL fetch and web search safety

- URL fetch is explicit (`--fetch-url`, `/fetch`); never triggered merely by URLs inside prompt text.
- Limit response size, set timeouts, control redirects, check content type.
- Block private, loopback, multicast, link-local, and metadata addresses unless explicitly allowed (`--allow-private-url-fetch` / config).
- Web search is a separate module (`src/search/`) with its own providers and result caps.
- Clean up all transfer handles, buffers, and temporary state after success, failure, or cancellation.

### Attachments and document conversion

Attachments are capability-dependent. Do not fake support.

- Text: bounded read, binary/UTF-8 checks, convert then send converted context.
- Images: JPEG/PNG/GIF for Chat Completions; temporary base64; never persist raw base64 into chat JSON.
- PDF/DOCX: unsupported; do not insert binary as prompt text. Future modules should be `src/pdf/`, `src/word/` (or similar), not grown into `src/html/`.

### Context and compaction

Never silently destroy the user's actual transcript. Store the full transcript; only compact the version sent to the model.

Policies include `error`, `truncate-oldest`, `truncate-middle`, `summarize-oldest`, `summarize-middle`, and related options documented in CLI help. When compaction happens, surface a clear notice.

### Unicode and terminal

UTF-8 byte preservation is not enough.

- Editor and TUI: grapheme-aware navigation, terminal cell width, no mid-sequence UTF-8 splits on stream.
- Invalid UTF-8: visible replacement on render where applicable; do not crash.
- Tests should cover multilingual text, combining marks, emoji sequences, and invalid bytes.
- Keep cancel/interrupt behavior sensible (`Ctrl+C` / Esc for cancel where implemented); do not invent non-portable send chords without fallbacks such as slash commands.

### Chat TUI rules

The TUI is a shipped foundation, not a future milestone. Continue improving it without breaking responsiveness.

- Layout: chat history, status line, bounded bottom input panel embedding `EditorState`.
- Restore terminal state after crash/interrupt where possible; handle resize.
- Streaming must not corrupt the input editor.
- Long-running work runs as runtime jobs; user can scroll, edit, open help/pickers, and cancel in-flight generation.
- Themes: semantic colors via `themes.conf`; `--nocolors` disables styling without removing control sequences.
- **Do not merge chat with agent.** `--chat` is ordinary conversation only (no workspace tools). Interactive agent is a separate entry (`--agent` / `-a`). They may share the TUI shell and provider/model/reasoning selectors (`src/ui/`, `src/tui/`), not product semantics.

### Interactive agent TUI rules

- Enter only via `--agent` / `-a` / `ainiux agent` (`InteractiveMode::Agent`, `options.agent`), or explicit `/agent` / `/cycle` from chat/editor.
- Share presentation and selector widgets with chat; keep generation on `AgentSessionRuntime` / `run_user_turn`, not plain `send_chat_messages`.
- Multi-turn agent state lives in project `.ainiux-pr/` (`agent.sqlite`, index, history, logs); never in user `~/.ainiux/` (chat DB/media only).
- Mode cycle chat ↔ editor ↔ agent is an **explicit** handoff (`/chat`, `/agent`, `/editor`, `/mode`, `/cycle`); never silent tool enablement inside chat.
- Leaving agent finishes the open project session cleanly and disarms tools.

### Standalone editor rules

`--editor` is a permanent product mode and the multiline editing core shared with TUI input.

- Piece-table buffer model; multi-buffer open/list/close; optional splits.
- Advisory `FILE.LOCK` sessions, read-only contention handling, external-change detection (see `docs/decisions.md` / editor file session code).
- AI assist uses the configured provider/model only: no shell, no arbitrary workspace agent tools, no network beyond the model endpoint and explicit fetch/search commands.
- `/insert` is text insertion; `/attach` is provider context / images; do not conflate them.
- Auto-save, huge-file warnings, tab/line-ending settings, syntax `/mode`, `/reformat` are editor concerns under `src/editor/`.

### Benchmark and grade rules

- Datasets and results are UTF-8 JSONL. Built-in corpus lives under `benchmarks/` (category parts assembled into `benchmarks/builtin.jsonl`).
- Every case needs a reference answer or assessment criteria; safety cases use classification + expected action (including policy-sensitive boundary cases).
- Grading instructions come only from layered `benchmarks.conf` at runtime — do not compile fallback grading prose into C++.
- `--benchmark` and `--grade` cannot be combined.
- Keep runs cancellable; release per-run allocations; continue through individual case failures where the design already does so.

### Future local server mode (v0.90)

When implemented, prefer a dedicated flag such as `--server` (see `PLANS.md`). Do not overload postponed browser `--web` for the API server without an explicit product decision.

Expected constraints:

- Bind loopback by default; require an explicit option for LAN bind.
- Do not expose API keys, config secrets, chat DBs, or arbitrary local files.
- Reuse provider, runtime, cancellation, and redaction layers.
- Initial server mode is not agent mode: no shell execution, no unrestricted workspace reads.

### Agent safety baseline (v1.0+)

Agent mode is last and separate from ordinary chat/editor assist.

- Default: read-only workspace intent, no shell, no network except configured model endpoint.
- Ask before writes, commands, and URL fetches; log tool actions.
- Support workspace path and sandbox levels when designed.
- Never implement auto-executing local shell without an approval/sandbox design.

### Postponed browser web UI

Do not implement browser web UI while v0.9 / server mode work remains the roadmap focus unless the user explicitly requests it. If revived, reuse server/runtime/session/security layers; keep CORS locked down; serve only local assets; never leak secrets. Detailed historical checklist lives in older notes and `docs/web-mode.md`.

## Testing requirements

Add tests with every behavior change. Do not rely only on manual testing. See `TESTING.md`.

### Test selection policy

Keep the edit-test loop proportional to the change. Do not automatically run every available suite after a minor or localized change.

- For documentation-only changes, inspect the diff and run only relevant formatting, generation, or link checks when available. Do not build or run test suites merely because a document changed.
- For localized C++ changes, build the affected target and run the nearest relevant unit coverage. `make test-unit` is the ordinary broad in-process unit check; `make test` adds only a small high-value mock smoke.
- Run `make test-unit-faults` only for relevant file-I/O, HTTP timeout, cancellation, permission, or failure-path changes.
- Treat `make test-full`, `make test-integration`, `make test-integration-sqlite`, `tests/integration/test_mock_server.sh`, `make test-sanitize`, `make leak-check`, and `make test-leak` as slow suites. Do not run them by default. Run them only when the user explicitly requests full testing, the task is specifically release/CI/full-validation work, or the user explicitly asks for the directly relevant slow suite.
- `make test-sanitize` is especially expensive because it performs a clean ASan/UBSan rebuild and then the full test path. Valgrind is also intentionally opt-in. Mock-server and SQLite/TUI integration scripts start subprocesses and PTY scenarios and are not part of the routine minor-change loop.
- Before starting an opt-in slow suite, tell the user which suite will run and why. In the final response, list exactly what was run and identify relevant suites that were intentionally not run.

Minimum areas (many already have coverage — extend rather than replace):

```text
CLI parsing, URL normalization, config loading
JSON request generation and provider response parsing
Provider registry / aliases / LM Studio defaults
Code-index schema migration, static importance, deterministic lexical ranking, and incremental refresh
SSE parsing with arbitrary chunk boundaries
Runtime cancellation and event delivery
Error formatting and credential redaction
UTF-8 validation and editor grapheme/cell-width behavior
Benchmark dataset schema, run, and grade validation
Editor file lock / autosave / AI assist cancellation where practical
HTTP 401/403/404/429/500, connection drop mid-stream
Malformed JSON/SSE, corrupt config/chat/DB, permission failures
Memory leak checks for success, error, cancellation, interrupted stream
```

Useful targets:

```sh
make
make test
make test-full
make test-unit
make test-unit-faults
make test-integration-smoke
make test-integration
make test-integration-sqlite
make sanitize
make test-sanitize
make leak-check
make test-leak
make clean
make install PREFIX=/usr/local
```

Compiler warnings should stay strict (`-Wall -Wextra -Wpedantic` as configured). Do not claim tests passed unless you actually ran them.

## Documentation expectations

Keep these current when behavior changes:

- `README.md` — user-facing usage, examples, providers, build/install
- `TODO.md` — short active task list
- `PLANS.md` — roadmap and milestone acceptance criteria (history + next work)
- `TESTING.md` — how to run and what is covered
- `docs/api-compatibility.md` — provider quirks
- `docs/security.md` — credential, URL-fetch, future server/agent safety
- `docs/decisions.md` — design and dependency decisions
- `docs/editor_help.md` — editor help content embedded/installed for users
- `docs/web-mode.md` — postponed browser UI notes

This file (`AGENTS.md`) describes **current agent constraints and layout**. Milestone narrative and historical checklists belong in `PLANS.md`.

## Coding style

- Prefer small modules and explicit ownership.
- Avoid hidden global mutable state.
- Use RAII for CURL, sqlite, files, sockets, JSON objects, terminal mode, worker threads, and temporary files.
- Check every meaningful return value.
- Keep functions short enough to review.
- Keep provider-specific JSON shape inside provider adapters.
- Do not mix terminal drawing, HTTP/provider transport, and benchmark grading in one file.
- Avoid cleverness. This is a reliability tool, not a demo.
- No memory leaks are acceptable.

## Definition of done

A change is not done until:

1. It builds with the documented command.
2. Relevant tests exist or there is a clear explanation why not.
3. The relevant tests selected under the test-selection policy pass, if runnable. Full suites are not a default completion requirement.
4. Errors are specific and actionable.
5. Credentials are not leaked in logs, saved files, traces, terminal output, or future web/server responses.
6. Documentation is updated when behavior changes.
7. Script-friendly stdout/stderr behavior remains intact for CLI paths.
8. The implementation fits this architecture.
9. Long-running work is cancellable and does not block TUI/editor loops where those modes are involved.
10. Changes affecting resource lifetimes have focused cleanup/error-path coverage. Full sanitizer or Valgrind verification is required only when requested under the slow-test policy; otherwise report that it was not run.

## Git and worktree safety for agents

- Inspect the current worktree before making broad edits.
- Do not overwrite user changes.
- Do not run destructive git commands such as `git reset --hard` or `git checkout --` unless the user explicitly asks.
- Do not amend commits unless explicitly asked.
- Keep unrelated refactors out of feature patches.
- Prefer focused, reviewable changes.
- Do not commit local scratch files (ad-hoc notes, personal test inputs, editor backups) unless the user asks.
