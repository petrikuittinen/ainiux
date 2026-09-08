# AGENTS.md

Project: `ainiux`

Repository-level guidance for AI coding agents. The user's latest explicit
instruction wins. Use `PLANS.md` for direction and milestone acceptance,
`TODO.md` for open tasks, `docs/version-history.md` for releases,
`docs/decisions.md` for rationale, and `README.md` / `docs/README.md` for
current user-facing behavior.

## Mission

Build and maintain `ainiux`: a fast, portable command-line and terminal client
for OpenAI and OpenAI-compatible APIs, with a standalone editor, document
conversion, benchmarks and judge grading, local agent workflows, image
generation, an authenticated control API, and an embedded browser controller.

Keep the program excellent as a scriptable CLI. Provider requests, streaming,
persistence, jobs, cancellation, memory management, and errors belong in shared
core modules rather than being reimplemented by each UI or protocol surface.

## Current baseline

Current release: **v1.33**. Linux and other POSIX-like source builds are the
primary supported path, Apple Silicon macOS source builds are supported, and a
native Windows 10 1903+/Windows 11 x64 MSYS2 UCRT64 implementation is present.
Windows remains unreleased until the parity gate in `docs/windows.md` passes.

Implemented entry points include:

| Surface | Entry |
| --- | --- |
| One-shot chat | default plus `-p` / `--prompt-file` |
| Conversion, fetch, and search | `--input`, `--fetch-url`, `--search` |
| Model and MCP management | `--list-models`, `--list-mcp`, MCP install/enable/disable/remove options |
| REPL | `-i` / `--repl` |
| Chat TUI | `-c` / `--chat` (`--tui` alias) |
| Standalone editor and dired | `-e` / `--editor`, `-d` / `--dired` |
| Benchmark and judge | `benchmark` / `--benchmark`, `--grade` |
| Code index and security review | `--index-code`, `--print-index`, `--clear-index`, `--security-review` |
| Interactive agent | `agent` / `-a` / `--agent` |
| One-shot Act and Plan | `run` / `-r` / `--run`, `plan` / `--plan` |
| Image generation | `image` / `--image` |
| Control API and browser | `server` / `--server`, `webserver`, `server --webui` |

The control server, revision-safe chat/workspace/editor operations, MCP server
adapter, interactive remote Agent/Guard sessions, and embedded dependency-free
WebUI are shipped. The WebUI includes chat, agent, image generation, dired/file
review, live editing, syntax highlighting, undo/redo, and indentation controls.
CLI and WebUI image generation use the layered `images.conf` catalog and the
compiled `openai_images`, `replicate_predictions`, `fal_queue`, and
`gemini_interactions` adapters.

### Product boundaries

- Ordinary chat never receives workspace tools. Agent mode is explicit and
  separate, although chat, editor, and agent share terminal presentation.
- Agent Act is the default task policy. Fresh projects use Smart permissions;
  Confirm and Yolo are explicit choices. Plan permits research but limits writes
  to planning documents. `/goal` and `goal_met` are implemented.
- Project agent state stays under `.ainiux-pr/`; user chat data and installed MCP
  configuration stay under `~/.ainiux/`.
- The code index is an optional definitions-only navigation hint. Agent queries
  use short-lived lazy read-only SQLite access; security review deliberately uses
  an immutable loaded snapshot. Current source must still be verified before edits.
- OpenAI Chat Completions and text Responses output are supported. Image-capable
  Responses requests can carry user images. Provider capabilities still vary.
- The control API is Ainiux-native under `/ainiux/v1/`; `/v1/` remains reserved
  for a later OpenAI-compatible adapter.

Do not pretend these exist: REPL/TUI image-generation jobs, batch or streaming
image output, multi-turn image editing, PDF/DOCX conversion, `/loop`, sub-agents,
a native Anthropic Messages adapter, multi-workspace server routing, or an
ncurses UI. MCP tools do not imply MCP sub-agents or interactive MCP Guard
chaining.

## Implementation stance

- Default language: **C++17**. Do not use Rust or Go.
- Use the `Makefile` as the primary build entry point.
- Preserve portability across Linux, BSD, macOS, other POSIX-like systems, and
  the official native Windows UCRT64 target.
- Do not require C++20/23, C23, package managers, or non-portable extensions
  without explicit approval.
- Prefer stack objects, RAII, `std::string`, `std::vector`, standard containers,
  and `std::unique_ptr`. Use shared ownership only when it is truly required and
  documented. Do not introduce raw owning pointers.
- Correctness, actionable errors, credential safety, robust streaming,
  responsive UIs, and leak-free cleanup outrank cleverness or micro-optimization.

### Resource ownership is non-negotiable

Every allocation and acquired resource must be released on success, error,
cancellation, timeout, interrupted stream, parse failure, and early return.
Wrap libcurl handles/header lists, SQLite handles/statements, files, descriptors,
sockets, directories, terminal state, TLS objects, subprocesses, threads, and
temporary data in RAII. At C API boundaries, document allocator, owner, and
matching release function. Lifetime-sensitive changes require focused normal and
failure-path tests.

## Repository map

Put code in the existing matching module; do not create a parallel architecture.

```text
.
├── README.md, PLANS.md, TODO.md, TESTING.md
├── config/                  installed configuration templates
├── benchmarks/              builtin JSONL datasets
├── include/ainiux/          small public headers and version metadata
├── scripts/                 install, test, and optional MCP helper scripts
├── src/
│   ├── main.cpp             option validation and top-level dispatch
│   ├── app/                 mode runners and surface-neutral operations
│   ├── agent/               Agent/Guard/session/tools/index implementation
│   ├── benchmark/           dataset, runner, scoring, grading, reports
│   ├── chat/                sessions, settings, media, SQLite persistence
│   ├── cli/                 argument parsing and option values
│   ├── config/              TOML-like config and model/image catalogs
│   ├── context/             request budgets and compaction policies
│   ├── editor/              piece table, dired, assist, locks, reformat
│   ├── encoding/            built-in and allowlisted iconv conversion
│   ├── fetch/, search/      explicit network retrieval
│   ├── highlight/           shared terminal syntax highlighter
│   ├── html/, markdown/     conversion and display formatting
│   ├── http/                libcurl transport and SSE
│   ├── input/               bounded text/image classification and reads
│   ├── json/                in-tree JSON facade
│   ├── mcp/                 installed MCP client and tool bridge
│   ├── platform/            portable filesystem/environment/Windows UTF
│   ├── provider/            profiles and protocol adapters
│   ├── runtime/             cancellable jobs, events, subprocesses
│   ├── security/            credential redaction
│   ├── server/              control API, auth, jobs, sessions, TLS, assets
│   ├── tui/, ui/            terminal shell and shared widgets
│   ├── version/             runtime version
│   └── web/                 embedded vanilla HTML/CSS/JavaScript sources
├── tests/                   unit, fault, integration, browser, fixtures
├── tools/                   project maintenance helpers
└── docs/                    current guides, decisions, history, audits
```

Headers normally live beside their implementation under `src/<module>/`.
Keep replaceable dependencies behind small module boundaries.

## Dependencies

Use as few external libraries as practical. New dependencies require an explicit
decision in `docs/decisions.md` or next to the build configuration.

- **libcurl**: HTTP/HTTPS, proxies, timeouts, streaming.
- **libsqlite3**: chat, project agent state, and code indexes.
- **OpenSSL (optional)**: direct TLS for the control server. Builds without it
  retain loopback/plain-HTTP support subject to server policy.
- **In-tree JSON facade**: request/response JSON boundaries.
- **POSIX termios or native Win32 console plus ANSI/VT**: terminal ownership;
  Windows Terminal and modern conhost are supported, not mintty full-screen use.
- **Generated Unicode tables**: editor properties under `src/editor/detail/`.

Do not add a runtime WebUI framework, Node/npm dependency, CDN, hosted font, or
external script. Optional browser tests may use an already installed Chromium.

## Architecture rules

### Shared services and mode dispatch

`src/main.cpp` validates combinations and dispatches to `src/app/` or the owning
server runner. Surface-neutral operations accept explicit inputs and cancellation
and publish typed results/events. They do not own terminal state, process signals,
or CLI stdout/stderr policy.

UI and protocol adapters must reuse provider, runtime, persistence, Agent/Guard,
filesystem containment, and redaction code. Do not duplicate provider HTTP/SSE,
agent loops, approval decisions, or filesystem mutation logic in a UI.

### Provider, HTTP, and runtime

- Route model/API differences through `src/provider/`. Keep the registry
  data-driven and keep local/custom profiles distinct even when adapters share code.
- `--provider none` is the offline profile; it has no invented model endpoint.
- Keep transport in `src/http/`. Apply connect/total timeouts, proxy policy,
  active cancellation, credential redaction, body bounds, and correct incremental
  SSE parsing across arbitrary network chunks and UTF-8 boundaries.
- Long-running provider calls, model listing, fetch, file work, editor assist and
  reformat, benchmarks, indexing, and server jobs use cancellable runtime work.
  Workers publish events; the owning UI/session thread mutates presentation state.
- Shutdown must stop new work, cancel or finish active work as designed, close
  streams/sockets, join workers, and release every retained buffer and handle.

### CLI, errors, and configuration

- Reserve stdout for requested model/conversion/list output. Send status,
  warnings, progress, and errors to stderr.
- Preserve `--format text|json|ndjson` and `--no-stream` scripted behavior.
- Use `ainiux::ErrorCode` and include the failed operation, safe URL/path/model or
  option context, status/provider detail when safe, and an actionable correction.
- Never encourage command-line API keys. Prefer provider env vars, `AINIUX_API_KEY`,
  key files, stdin, or named key environments. Warn for `-k` unless quiet.
- Redact authorization, API-key, cookie, and set-cookie values from logs, errors,
  saved data, traces, server responses, and UI output.
- Configuration is TOML-like: installed defaults, then the user file, then CLI.
  `--no-config` skips only the user layer. Keep themes, editor commands,
  benchmarks, models, and images in their existing separate config documents.

### Persistence and local data

- TUI chat uses `~/.ainiux/ainiux.db`; explicit JSON save/load remains import/export.
- Agent sessions, settings, logs, history, and index data use project-local
  `.ainiux-pr/`, never the user chat database.
- Use restrictive permissions and atomic replacement for sensitive or durable
  files. Preserve the prior generation on failed or cancelled index/database work.
- On Windows, use protected current-user/SYSTEM ACLs, reject unsafe reparse paths,
  flush durable writes, and use native atomic replacement primitives.
- Never persist raw base64 image bodies in chat/session JSON.

### Inputs, fetching, and context

- Bound text, HTML/Markdown, and image reads. Reject unsupported/binary documents
  rather than inserting bytes into prompts.
- Chat input supports PNG/JPEG/GIF where the selected protocol/model permits it;
  image-generation reference inputs are PNG/JPEG and catalog capability-dependent.
- URL fetch is always explicit. Apply redirect, size, timeout, content-type, and
  resolved-address checks; private, loopback, link-local, multicast, and metadata
  destinations require the explicit override. Search remains a separate module.
- Keep the full transcript on disk. Context compaction changes only the request
  sent to the model and must produce a visible notice.

### Terminal, editor, and chat

- Preserve grapheme-aware navigation, terminal cell widths, resize behavior, and
  valid UTF-8 rendering. Invalid bytes must not crash the UI.
- The terminal/event owner alone draws or mutates chat/editor/agent state.
  Streaming and background work must not corrupt input or block interaction.
- Chat is ordinary conversation only. Switching to agent/editor is explicit.
- The standalone editor remains a multi-buffer piece-table editor with splits,
  advisory file sessions, external-change checks, dired, autosave, syntax modes,
  indentation/reformat, and optional model-only AI assist.
- Editor assist has no arbitrary workspace-agent tools. `/insert` edits text;
  `/attach` supplies request context or images.

### Agent and code index

- Interactive agent owns one session-scoped `AgentController`. Temporary editor or
  dired hops may leave a turn running; leaving for chat or quitting finishes the
  project session and disarms tools.
- Guard, permission mode, task policy, workspace containment, approvals, and tool
  logs remain independent enforcement layers. Headless Ask decisions deny.
- Native tools use the shipped short names. Removed long aliases stay unknown.
  Installed MCP tools are qualified and available only to agent/run/plan.
- Extend indexing only under `src/agent/index/`. Keep lightweight lexical scanners,
  deterministic ranking, and definitions-only storage; do not add compiler/LSP
  dependencies, reference graphs, or automatic request-context injection.
- Exact/full-component lexical relevance precedes static importance. Mutations keep
  live touched-file results coherent and coalesce persistent refresh. Cancellation
  or failure preserves the previous completed database.
- Do not redesign `/loop`, sub-agents, agent skills, or the built-in agent prompt
  without a separate user specification.

### Control server and WebUI

- The control API is `/ainiux/v1/` plus the scoped `/mcp` adapter. Do not substitute
  an OpenAI-only endpoint for jobs, sessions, cancellation, Guard, or revisions.
- One server owns one fixed workspace. Wire paths are relative; reject traversal,
  unsafe symlink/reparse paths, protected state, stale revisions, and arbitrary roots.
- Plain `ainiux server` defaults to `127.0.0.1`. Browser-oriented `webserver` /
  `server --webui` defaults to `0.0.0.0` with prominent plaintext warnings and can
  be constrained to loopback. Direct non-loopback plain server mode requires TLS
  unless the explicit insecure override is supplied.
- Keep full-control and MCP-only secrets separate; never reuse provider keys.
  Enforce Host/Origin, TLS, remote-Yolo, authentication-scope, and bounded HTTP rules.
- Browser source stays dependency-free under `src/web/`. Serve only exact versioned
  asset routes with immutable caching, strict CSP, no CORS, and correct MIME types.
- Persist a controller token only after validation in origin-scoped localStorage;
  clear it on 401 or sign-out. Never put it in cookies, URLs, logs, or rendered DOM.
- Build dynamic content with DOM APIs and `textContent`; raw model/tool HTML remains
  inert. Preserve desktop/mobile, keyboard/touch, visible focus, reduced-motion,
  light/dark, bounded-work, undo/redo, and highlight/editor synchronization behavior.
- Never use `alert()`, `confirm()`, or `prompt()` in the WebUI. Blocking choices
  use in-page `<dialog>` elements (or the same custom overlay pattern), never
  the browser's built-in dialogs.

### Benchmark and grade

- Datasets/results are UTF-8 JSONL. Every case needs a reference answer or explicit
  assessment criteria; safety cases include an expected classification/action.
- Grading prompts come only from layered `benchmarks.conf`; do not compile fallback
  grading prose into C++.
- Benchmark and grade modes remain mutually exclusive, cancellable, and resilient
  to individual case failures where designed.

## Testing requirements

Add or update automated tests for behavior changes. Select the smallest meaningful
set and report exactly what ran; never claim an unrun test passed. See `TESTING.md`.

- Documentation-only changes: inspect the diff and run relevant link/generation or
  formatting checks; do not build merely because prose changed.
- Localized C++ changes: build the affected target and run the nearest units.
  `make test-unit` is the normal broad unit check; `make test` adds a small mock smoke.
- WebUI changes: run `make test-web-js`; set `AINIUX_TEST_BROWSER` only when an
  installed Chromium browser should join the optional test.
- Run fault tests for relevant I/O, HTTP, cancellation, or permission changes.
- `make test-full`, comprehensive integration, sanitizer, and Valgrind targets are
  opt-in slow suites for release/full-validation work or explicit requests. Announce
  them before starting and identify intentionally omitted relevant slow suites.

Compiler warnings remain strict (`-Wall -Wextra -Wpedantic` as configured).

## Documentation ownership

- `README.md` and `docs/README.md`: landing page and documentation index.
- `docs/getting-started.md`, `docs/cli.md`, `docs/chat.md`, `docs/agent.md`,
  `docs/editor_help.md`, `docs/web-mode.md`: current user behavior.
- `docs/api.md`, `docs/api-compatibility.md`, `docs/security.md`: wire/provider/
  security contracts.
- `TESTING.md`: test selection and coverage.
- `PLANS.md`: active direction and acceptance criteria; `TODO.md`: open tasks.
- `docs/version-history.md`: compact releases; `docs/decisions.md`: rationale.

Do not put completed milestone checklists in `AGENTS.md` or `PLANS.md`.

## Definition of done

A change is done when it fits the architecture, builds where applicable, has and
passes proportionate tests, preserves script-friendly output, reports specific
errors, does not expose credentials or forbidden paths, keeps long work cancellable,
updates relevant documentation, and has focused cleanup/error coverage when resource
lifetimes change. Full sanitizer/Valgrind runs are required only by the test-selection
policy or explicit request.

## Git and worktree safety

- Inspect the worktree and preserve user changes and untracked scratch files.
- Do not use destructive reset/checkout commands or amend commits unless asked.
- Keep patches focused; do not mix unrelated refactors or commit local scratch data.
