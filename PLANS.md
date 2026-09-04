# PLANS.md

Project: `ainiux`

This is the active implementation roadmap. It records current product direction,
unfinished milestones, and enough design detail for the next implementation work.
Current agent constraints live in `AGENTS.md`, user-facing behavior starts in
`README.md` and `docs/README.md`, design rationale lives in `docs/decisions.md`,
release history lives in `docs/version-history.md`, and short open work lives in
`TODO.md`.

This is a living roadmap, not an immutable specification. Git history retains the
superseded long-form milestone plans and implementation checklists removed in the
2026-07-27 roadmap refresh.

## Product direction

Build the best practical command-line and terminal client for OpenAI and
OpenAI-compatible APIs while keeping every surface script-friendly, portable,
responsive, secure with credentials, explicit about failures, and free of memory
leaks.

The same provider, transport, runtime, cancellation, persistence, context, and
error layers should serve:

- one-shot CLI chat and document extraction
- REPL and full-screen chat
- the standalone editor and editor AI assist
- benchmark and judge grading
- local agent Act/Plan workflows
- the local control-API server (`ainiux server`, **v1.30**), with a later OpenAI-compatible `/v1` adapter
- the embedded browser controller and later media capabilities

## Roadmap order

| Milestone | Direction | Status |
| --- | --- | --- |
| v0.0–v0.8 | CLI, persistence, runtime/TUI, providers, context, config, benchmarks, editor | Landed |
| v0.9 | Benchmark calibration, refactor hygiene, TUI/CLI/editor polish | Remaining work continues |
| v0.90 | Unified terminal bindings; this number did not ship a local server | Landed (bindings). Server work shipped in **v1.30** |
| v1.0–v1.15 | Local agent foundation, hardening, and documentation overhaul | Landed through v1.15 |
| v1.16 | Editor dired directory browser (`--dired`, F4, Ctrl+X d) | Landed |
| v1.17 | Mid-turn agent↔editor/dired review, dired history line-diff, agent chrome/tool polish, Apple Silicon builds | Landed |
| v1.18 | Short native tool API without legacy aliases, MCP client/vision bridge, and combined grep filters | Landed |
| v1.19 | Settings widget, Goal-only `goal_met`, and charset conversion to UTF-8 | Landed |
| Native Windows x64 | UCRT64 native target and portable ZIP; all-mode parity gate | Implementation landed; native acceptance pending |
| **v1.1** | **Lightweight definition ranking and index tuning; later `/goal`, `/loop`, and sub-agents** | **Next priority** |
| **v1.2** | **Image generation across CLI and interactive surfaces** | CLI `ainiux image` landed; REPL/TUI remaining |
| **v1.30** | **Remote control API** (`ainiux server`): HTTP `/ainiux/v1`, MCP adapter, remote review/editor/dired, embedded vanilla-JS WUI | **Landed through PR 10** |
| WebUI highlighting | Dependency-free client rendering, then editor and TUI-language parity | Markdown plus the first fenced-code language batch landed; browser editor and remaining TUI languages later |

Each milestone must leave ordinary CLI chat and existing interactive modes usable.
The embedded browser controller completes the v1.30 control-server milestone.

## Current baseline

Implementation status (2026-09-02): **v1.30**.

The shipped product includes:

- script-friendly Chat Completions and text Responses API paths
- built-in provider profiles and OpenAI-compatible custom/local endpoints
- cancellable SSE/HTTP runtime jobs and provider/model selection
- REPL, chat TUI, standalone editor with dired, benchmark, grade, and document conversion
- retained row-diff rendering across chat, agent, and editor terminal surfaces
- SQLite chat persistence plus JSON import/export
- text/Markdown/HTML attachments and supported image input
- safe URL fetch and web search
- syntax highlighting, grapheme-aware editing, multiple editor buffers, and AI assist
- one-shot and interactive local agents with project-local `.ainiux-pr/` state
- reusable project scripts under `scripts/ainiux/` with Smart content-hash trust and optional background `run`
- Act/Plan task modes, Guard approvals, project-contained writes, and tool logging
- mid-turn editor/dired review while an agent turn continues (Ctrl+G / F4; session stays open)
- dired history line-diff tints and n/p changed-block navigation on dirty files
- compact live tool rows, bounded first-thought reasoning previews, request-token chrome,
  and transcript-preserving three-strategy `/compact`: local-only `fast`, loss-aware default
  `smart`, and active-model `summary`, with a universal 75% automatic threshold
- compact native agent tool schemas with short industry-aligned names and no legacy execute aliases
- explicitly installed HTTP/stdio MCP tools in agent, run, and plan modes, including the bundled local vision bridge
- shared list pickers with `/` multi-character find and `.` alphabetical sort
- OpenRouter, OpenAI, and DeepSeek credit display when the selected key can query it
- a fast project-local symbol index with incremental discovery and lightweight scanners
- Apple Silicon macOS 15+ source builds
- the authenticated loopback-by-default control API, MCP adapter, TLS/direct-access gates,
  revision-safe remote chat/workspace/editor operations, and embedded responsive vanilla-JS WUI
- safe client-side Markdown rendering for WebUI Chat and Agent prose, with semantic headings,
  responsive tables, clickable HTTP(S) links, and TUI-derived light/dark colors
- dependency-free fenced-code highlighting for JavaScript, TypeScript, Python, C, C++, HTML,
  CSS, and Bash, including embedded CSS/JavaScript within HTML fences

The current v1.1 index stores files and definitions for every supported scanner
language, plus static declaration importance. It intentionally has no reference
graph or automatic model-context hints.

The unreleased native Windows target supports Windows 10 1903+/Windows 11 x64
through MSYS2 UCRT64, with a portable ZIP, wide path/process boundaries, Win32
terminal/clipboard/persistence, Job Object process trees, and platform test/CI
paths. It is not a released platform until every implemented product mode passes
the native checklist in `docs/windows.md`; Windows ARM64, MSVC/CMake, installers,
older Windows, and mintty full-screen operation are explicit non-goals.

## Release history

The compact v0.0–v1.30 timeline lives in `docs/version-history.md`. Historical
implementation details remain available in Git history and `docs/decisions.md`.

# v1.1 - Lightweight smarter agent indexing

## Current implementation

The project index remains a fast, optional definitions-only navigation aid across every v1.00 scanner language. SQLite stores metadata, files, and symbols with a compact 0–100 static importance score computed during the existing definition scan from declaration kind, visibility, and scope. It stores no references, evidence, call edges, caller counts, or graph scores.

Lexical relevance is authoritative: full-name matches rank above exact identifier components, which rank above component-prefix matches. Multi-token coverage is preserved, importance orders only otherwise comparable hits, and path/line/ID ties are deterministic. `index`, `symbol`, and `outline` expose index summaries and definitions; `read` verifies live source and supports `items` batching; `edit` supports `replace_symbol` while indexing is enabled. Legacy long names and index-management tools are absent; caller/callee tools and automatic provider-context hints are also absent.

Agent navigation prefers one `read` call with `items` whenever two or more independent paths or ranges are known, including with native parallel tool calling; `path` remains the single-target form. Batched items have independent 128 KiB default limits, line-numbered content and hashes, explicit continuation metadata when truncated, within the 256 KiB aggregate cap.

Agent startup performs a cheap read-only index probe and never prompts, creates an index, loads a full snapshot, or waits for freshness. Missing/corrupt indexes become ready with live filesystem tools and a display-only `/index-code` hint. Existing completed indexes are queried lazily through short-lived read-only SQLite transactions and refreshed in the background only after readiness. `/index-code` explicitly creates/enables an index, while `/show-index` refreshes and appends the compact totals table without adding it to model context. Native mutations keep a touched-path overlay until their coalesced SQLite revision completes; task completion runs a full-tree freshness pass that reparses only added or changed files. Security review alone retains an eager immutable authorization snapshot. Query/refresh failures preserve the previous completed database and fall back to live tools where applicable.

Schema versions 1–3 migrate transactionally by rescanning definitions, adding importance, and dropping `refs` and `symbol_scores`. Cancellation before commit preserves the previous completed snapshot. A cancellable post-commit SQLite compaction reclaims obsolete graph storage; compaction failure leaves the migrated index valid and emits a warning.

## Next tuning

- benchmark fresh and no-change refresh time and peak memory on representative projects
- tune declaration visibility inference without adding parser or language-server dependencies
- enrich text search with enclosing definitions where cheap
- retain live filesystem, compiler, and test verification because indexed locations are hints
- `/goal` + `goal_met` landed for interactive agent; keep `/loop` and sub-agent behavior reserved until separately specified

## Remaining v0.9 polish

Continue in small, test-backed slices:

- benchmark cutoff/judge calibration and stronger assessment data
- provider/API error and compatibility hardening
- TUI/editor responsiveness, Unicode, resize, and interaction polish
- refactor hygiene where it removes duplication without destabilizing behavior
- focused cancellation, permission, sanitizer, and leak hardening
- syntax-theme contrast warnings and remaining editor reformat/PTY coverage

These are maintenance tracks, not a reason to delay v1.1 definitions-index tuning indefinitely.

## Deferred: large-image preprocess for attach / MCP vision

**Landed (docs + vision bridge):** `scripts/image_mcp_server.py` optional downscale
(`--resize auto|pillow|ffmpeg|none`, `--max-edge` default 1024, `--soft-bytes`) using
**Pillow** if importable or **ffmpeg** on PATH. Input formats for the bridge: PNG, JPEG,
GIF, WebP (first frame when animated). Core ainiux attach remains PNG/JPEG/GIF only;
WebP stays disabled in C++ until a separate capability decision. See `docs/mcp.md`.

**Not started — Phase 2 (core optional preprocess):** optional shell-free spawn of
ffmpeg/ImageMagick from `src/input/` (or similar) before base64 for Chat Completions
vision inject, agent `attach`, and remote HTTP MCP `image_base64` rewrite. Prefer
loopback absolute paths without base64 when possible. Zero new link-time image libraries.
Config knobs such as `image.resize_backend` / `--image-max-edge`. Store originals in
TUI media; resize only at request time. Fixed argv, timeouts, RAII temp cleanup.

**Not started — Phase 3 (polish):** cache resized variants by
`(sha256, max_edge, quality)`; `ffprobe`/identify for dimension-only decisions; consider
agent-only soft defaults; ImageMagick as secondary auto backend; optional PNG retention
when alpha must be preserved.

Do not implement full JPEG/PNG/GIF/WebP codecs inside the ainiux binary for this work.

## v0.90 (historical)

The v0.90 release shipped unified terminal bindings. It did **not** ship a local
server. The old sketch of an OpenAI-only `/v1/chat/completions` proxy is
superseded: remote control is **v1.30** below. An OpenAI `/v1` adapter remains a
later optional layer on that daemon, not a substitute for the control API.

# v1.2 - Image generation

## Current implementation

CLI-only single-turn `ainiux image` / `--image` is landed. Image **models** are `[image]` records in `images.conf`; image **protocols** are compiled adapters. `openai_images`, `replicate_predictions`, `fal_queue`, and `gemini_interactions` are implemented. The bundled catalog defaults `--provider openai` to `gpt-image-2`, `--provider replicate` to `prunaai/z-image-turbo`, `--provider fal` to `fal-ai/flux/schnell`, and `--provider gemini` to `gemini-3.1-flash-image`. `--size` / `--ar` / `--quality` / `--format` are mapped from the matched record. Further image or video models for those protocols should be catalog-only.

## Goal

Finish first-class provider-backed image generation after v1.1, reusing provider profiles,
runtime jobs, cancellation, error handling, persistence, and credential redaction.

Remaining surfaces:

- REPL `/image` commands
- cancellable TUI image-generation job
- server/browser exposure through the landed v1.30 listener (one-shot `POST /ainiux/v1/image`)

Text chat must never generate images merely because an ordinary prompt asks for one.

## User controls

Support provider/model-appropriate forms of:

```text
prompt
image model/provider
width and height or named size
output format
output path
image count
```

Example direction:

```sh
ainiux image -p "A quiet terminal workspace at night" -m gpt-image-2
ainiux image --prompt-file prompt.txt --size 1024x1024 --format png
ainiux image -p "diagram of provider adapters" --output diagram.png
ainiux image --provider replicate -m p-image -p "a cute chubby cat"
ainiux image --provider fal -m fal-ai/flux/schnell -p "a cute cat" --ar 1:1
ainiux image --provider gemini -p "a ramen shop at night"
```

Keep stdout script-friendly: print intentional output such as the saved path, while
status and errors use stderr.

## Provider and runtime architecture

- Keep image request/response JSON inside `src/provider/`.
- Expose explicit image capability metadata instead of assuming every provider/model
  supports every size or format.
- Support bounded base64 responses and safely downloaded provider image URLs where the
  adapter requires them.
- Run generation/download/save as cancellable runtime work.
- Keep TUI/editor loops responsive and release every buffer/handle on failure or cancel.
- Do not add terminal bitmap protocols in the first slice.

## Safe output

- Refuse accidental overwrite unless explicitly authorized.
- When no path is supplied, allocate the first unused `imageN.EXT` name.
- Derive extensions from the actual output format.
- Use atomic restrictive writes where practical.
- Ignore provider-supplied filenames as local paths.
- For multiple images, allocate deterministic non-conflicting names.

## Persistence and security

Persist metadata, not image blobs, in chat/session records:

```text
provider
model
prompt
dimensions
format
saved path
created_at
safe provider metadata
```

Never expose provider keys or arbitrary local files through future preview routes.
Generated-image routes, if later added, may serve only controlled generated assets.

## Tests and acceptance criteria

- CLI and config parsing for model, size, format, count, and output
- capability validation and actionable unsupported-feature errors
- base64 and URL response parsing
- filename allocation, overwrite refusal, and atomic-save failure paths
- mock-provider CLI/REPL integration
- TUI responsiveness and cancellation
- credential redaction and bounded-memory behavior
- cleanup/leak coverage for success, provider error, write error, and cancellation
- existing text chat behavior unchanged without explicit image generation

# v1.30 - Remote control API (`ainiux server`)

Status: **Landed through PR 10.** The authenticated control API, bounded jobs
and replay, MCP adapter, interactive agent/Guard sessions, revision-safe chat
and workspace/editor operations, TLS/direct-access policy, and embedded WUI are
implemented.

## Goal

Add a local Ainiux control server that lets trusted clients drive Ainiux without
remoting terminal keys or reimplementing provider behavior. The server exposes
an Ainiux-native HTTP API under `/ainiux/v1/`, then a scoped MCP adapter, and
finally an embedded browser controller written in vanilla JavaScript.

This is not an OpenAI-compatible proxy. Keep `/v1/` reserved for a later,
separate OpenAI adapter. The control API must preserve Ainiux concepts that an
OpenAI-only endpoint cannot represent: jobs, sessions, cancellation, Guard
approvals, task modes, settings, review, dired, and editor operations.

The v1.30 release is complete with the embedded WUI shipped as the final PR over
the stable control API.

## Product decisions

- Entry points: `ainiux server` and `--server`; do not overload `--web`.
- Default bind: `127.0.0.1`. Initial server PRs are loopback-only.
- One server owns one workspace, selected at startup with `--workspace`
  (default: current directory). Remote requests cannot choose another root.
- One persistent interactive-agent lane exists per workspace. Concurrent
  requests that would mutate that agent session fail clearly instead of racing
  or silently creating a second controller.
- HTTP job control and the MCP adapter provide the first useful remote surface.
- The WUI is an Ainiux-served, same-origin, full controller implemented with
  vanilla HTML, CSS, and JavaScript ES modules. Its assets are embedded in the
  executable.
- Provider credentials, the controller secret, chat databases, project state
  databases, absolute filesystem paths, and arbitrary local files are never
  exposed through the API or WUI.

## Architecture boundary

Before adding sockets, extract surface-neutral operations from current CLI,
agent, chat, image, editor, and dired runners. A service operation must accept
explicit input plus a cancellation token and publish typed progress/result
events. It must not own terminal state, process signals, stdout/stderr policy,
or output-file selection.

Reuse the existing provider registry, HTTP/SSE transport, runtime jobs,
`AgentController`, `ApprovalGate`, `DiredState`, chat persistence,
configuration, path containment, and credential redaction. UI and protocol
adapters translate their inputs into the same operations; they do not duplicate
provider requests, agent loops, permission decisions, or filesystem logic.

The public wire model is versioned independently from internal C++ event types.
Do not serialize `AgentSurfaceEvent` or other internal enums directly. Define
stable DTOs for requests, job state, events, errors, approvals, directory
entries, revisions, and capabilities, with explicit conversion at the server
boundary.

Planned modules:

```text
src/server/
  server.{hpp,cpp}          lifecycle and shutdown
  listener.{hpp,cpp}        socket ownership and connection limits
  http_parser.{hpp,cpp}     bounded HTTP/1.1 subset
  router.{hpp,cpp}          /ainiux/v1 routing
  auth.{hpp,cpp}            full-control and MCP-only credentials
  wire.{hpp,cpp}            stable JSON DTOs and error envelopes
  job_registry.{hpp,cpp}    jobs, retention, cancellation, idempotency
  event_broker.{hpp,cpp}    ordered bounded replay and subscribers
  session_hub.{hpp,cpp}     chat/agent/dired/editor session ownership
  mcp_adapter.{hpp,cpp}     MCP 2026-07-28 transport and tools
  embedded_assets.*         generated WUI asset table
src/web/
  index.html
  css/
  js/                       vanilla ES modules
```

Keep browser source in the reserved `src/web/` tree. The build may generate an
embedded resource table, but it must not require Node.js, npm, a runtime
bundler, a CDN, or external scripts.

## Concurrency and ownership

- Listener threads parse and authenticate bounded requests, then dispatch work
  through the runtime/job layer.
- Provider chat and image jobs obey an explicit global concurrency cap.
- Agent `run`, `plan`, and interactive turns share one workspace mutation
  lane and one persistent `AgentController`. At most one such operation runs
  at a time.
- Conflicting agent work returns a typed conflict response (HTTP 409) with the
  active job/session identifier; it is not silently queued.
- The owning session loop alone mutates session, approval, dired, editor, or
  terminal-independent controller state.
- Every worker, socket, subscriber, timer, job, and session has RAII ownership.
  Shutdown stops accepts, rejects new work, cancels active jobs, closes SSE
  streams, joins workers, and releases listeners and TLS state.
- Slow clients cannot block producers. Each subscriber has a bounded queue;
  overflow closes that stream with a resumable error.

## Command-line contract

Initial loopback options:

```text
ainiux server
ainiux --server
  --workspace PATH
  --port PORT
  --server-secret-file PATH
  --mcp-secret-file PATH
  --max-connections N
  --max-jobs N
  --max-sessions N
```

The browser-startup amendment adds `ainiux webserver` and
`ainiux server --webui`. Plain server mode retains loopback defaults and no
browser side effects. Browser mode defaults to wildcard IPv4 only when no bind
was supplied, prints concrete `/ui/` interface links, and makes a best-effort
platform browser launch.

Credential environment variables:

```text
AINIUX_SERVER_SECRET   full controller/API access
AINIUX_MCP_SECRET      MCP-only access to /mcp
```

Never reuse `AINIUX_API_KEY`. Literal secret arguments, if retained for
compatibility, warn that argv may be visible; environment variables or
permission-restricted files are preferred. Secrets and TLS keys must live
outside the served workspace.

Non-loopback options land only with the TLS PR:

```text
  --bind ADDRESS
  --tls-cert PATH
  --tls-key PATH
  --insecure-plain-bind
  --allow-remote-yolo
```

## Network and authentication policy

Loopback is the safe default and the only supported bind during the first
server slices. Remote users may put an authenticated tunnel or reverse proxy in
front of that listener.

Plain-server direct non-loopback binding requires all of the following. The
explicit browser-startup mode described above instead permits its wildcard
plaintext default with prominent warnings:

- a full-control server secret;
- TLS, unless `--insecure-plain-bind` is explicitly supplied;
- strict `Host` and `Origin` allowlists;
- startup warnings that identify the exposed workspace and security mode.

Remote Yolo permissions are denied unless the server was started with
`--allow-remote-yolo`. Existing project permission state must not silently
enable it over the network.

Use bearer authentication over TLS for direct remote access. Compare secrets in
constant time where practical and redact them from logs, errors, metrics, URLs,
and persisted artifacts. The full-control token may use all enabled
`/ainiux/v1/` routes. The MCP token is accepted only at `/mcp` and receives
only the MCP tool scope. A credential valid for one scope must fail in the
other.

Browser assets may be fetched without embedding credentials, but every API
request and event stream is authenticated. Do not use query-string tokens,
authentication cookies, or implicit ambient authority.

## Bounded HTTP/1.1 contract

Implement and document a deliberately small HTTP/1.1 subset:

- request line and header size limits;
- JSON body and upload size limits per route;
- header-read, body-read, idle, and total request timeouts;
- connection and in-flight job caps;
- exact `Content-Length` handling and a documented decision on supported
  transfer encodings;
- rejection of conflicting framing, request smuggling patterns, invalid header
  syntax, unexpected bodies, and unsupported methods;
- safe keep-alive rules and bounded requests per connection;
- normalized paths with rejection of traversal and ambiguous encodings;
- explicit content types, `Cache-Control`, `X-Content-Type-Options`, and CSP
  for browser responses.

CORS is disabled by default. The embedded WUI is same-origin and needs no CORS.
If a later configuration enables another origin, it must use an exact allowlist
and must not combine wildcard origins with credentials.

## Wire errors

All failures use one stable JSON envelope:

```json
{
  "error": {
    "code": "conflict",
    "message": "an agent operation is already active",
    "details": {},
    "request_id": "..."
  }
}
```

Map internal `ainiux::ErrorCode` values consistently to HTTP status and public
codes. Provider bodies are included only when safe. Validation errors identify
the field, accepted shape, and actionable correction without exposing secrets
or local absolute paths.

## Jobs, events, cancellation, and replay

Long-running calls are asynchronous jobs. A successful submission normally
returns `202 Accepted` with a job resource. Job states are:

```text
queued -> running -> succeeded | failed | cancelled
```

Core routes:

```text
POST /ainiux/v1/jobs/chat
POST /ainiux/v1/jobs/run
POST /ainiux/v1/jobs/plan
POST /ainiux/v1/jobs/image
GET  /ainiux/v1/jobs/:job_id
GET  /ainiux/v1/jobs/:job_id/events
POST /ainiux/v1/jobs/:job_id/cancel
```

Job creation supports an idempotency key. Reusing a key with the same operation
and payload returns the existing job; reusing it with different input is a
conflict. Terminal jobs and replay events have bounded, server-configured
in-memory retention and are not promised to survive a restart.

SSE events use monotonically increasing IDs and a stable envelope:

```json
{
  "id": 42,
  "type": "progress",
  "created_at": "...",
  "job_id": "...",
  "session_id": null,
  "turn_id": null,
  "data": {}
}
```

Clients reconnect with `Last-Event-ID`. The event broker replays retained
events in order. If the requested event was evicted, return an explicit
replay-expired response and require a fresh job/session snapshot. Heartbeats,
disconnect detection, bounded queues, and cancellation must not leak threads or
retain completed request buffers.

Cancellation is idempotent. A terminal job remains terminal, and cancelling one
job must not cancel unrelated session work.

## Status and capabilities

```text
GET /ainiux/v1/health
GET /ainiux/v1/status
GET /ainiux/v1/capabilities
```

`health` is minimal and reveals no workspace details. Authenticated status and
capabilities describe the public API version, supported operations, enabled
authentication scope, bind security, job limits, provider availability without
credentials, and optional adapters. Clients must capability-detect instead of
assuming every provider or mode supports every operation.

## Interactive agent protocol

Session routes:

```text
POST   /ainiux/v1/sessions/agent
GET    /ainiux/v1/sessions/:session_id
GET    /ainiux/v1/sessions/:session_id/events
POST   /ainiux/v1/sessions/:session_id/turns
POST   /ainiux/v1/sessions/:session_id/turns/:turn_id/cancel
POST   /ainiux/v1/sessions/:session_id/approvals/:approval_id
GET    /ainiux/v1/sessions/:session_id/approvals/:approval_id/review-file
DELETE /ainiux/v1/sessions/:session_id
```

Each turn has a server-generated `turn_id`; every Guard request has an
`approval_id` tied to that turn. Approval replies must include the matching
identifiers. Reject late, duplicate, expired, or cross-turn approvals. A session
event stream supports ordered replay and a current-state snapshot so reconnects
cannot silently lose an approval.

HTTP interactive agent uses the existing Guard Ask flow. Headless MCP tool
chaining remains Ask-deny. Closing a session disarms tools and ends the active
project session but does not erase durable history.

Do not promise incremental assistant-answer deltas for agent turns until the
provider tool-round layer exposes them without changing context or tool
semantics. Tool activity, thinking notices, approval requests, state changes,
and the final result can ship first.

## MCP adapter

Implement MCP after the job API is stable, targeting the 2026-07-28 MCP
protocol. Include server discovery and current required protocol headers.
Expose a small deterministic tool set backed by the same job operations, for
example:

```text
ainiux_chat
ainiux_run
ainiux_plan
ainiux_image
ainiux_job_get
ainiux_job_cancel
```

Long operations return task/job handles rather than holding an unbounded HTTP
request open. MCP calls are stateless with respect to interactive Guard
prompts; they cannot answer approvals or acquire the full controller scope.
Tool schemas, errors, cancellation, and capability discovery must pass protocol
conformance tests.

## Chat and persistence

Remote chat uses revision-safe thread operations over the existing chat store,
not direct SQLite exposure:

```text
GET  /ainiux/v1/chat/threads
POST /ainiux/v1/chat/threads
GET  /ainiux/v1/chat/threads/:thread_id
POST /ainiux/v1/chat/threads/:thread_id/messages
```

Mutating requests include the last observed revision. Stale revisions fail with
a conflict and return enough metadata to reload. Never return database paths,
raw tables, provider secrets, or unrestricted attachment paths.

## Review, dired, and editor

Remote filesystem work is JSON over existing domain operations, not TUI
keystrokes, terminal paint, or a second browser.

First ship read-only review and dired:

```text
GET /ainiux/v1/workspace/review
GET /ainiux/v1/dired?path=RELATIVE_PATH
GET /ainiux/v1/files?path=RELATIVE_PATH
```

Wire paths are workspace-relative slash-separated paths. The server resolves
them beneath its fixed workspace and never returns native absolute paths.
Project-private `.ainiux-pr/` state and sensitive configuration paths are
excluded.

Later mutation routes must use root-aware filesystem primitives, exact file
identity, optimistic concurrency tokens, and atomic saves. They must not map a
remote row selection directly onto existing selection-based dired mutation
methods. A client supplies explicit target identities and the revision it
reviewed; stale, replaced, symlink-swapped, or out-of-root targets fail safely.

Planned later operations include create directory, rename/move, copy, delete
with explicit confirmation policy, file save, and editor AI assist. Bulk
actions are bounded and report per-target results. No endpoint accepts a remote
workspace root or arbitrary host path.

## Embedded vanilla-JavaScript WUI

The final v1.30 PR adds a same-origin browser application at `/ui/`. It is a
full remote controller for the stable API, not an alternate implementation of
Ainiux logic.

Technical constraints:

- vanilla HTML, CSS, and JavaScript ES modules;
- no framework, Node.js runtime, npm runtime dependency, runtime bundler, CDN,
  third-party hosted font, or external script;
- source assets under `src/web/`, generated into an embedded binary resource
  table at build time;
- content-hashed or versioned static asset URLs, correct MIME types, cache
  policy, CSP, and no directory serving;
- `fetch` for JSON operations and a fetch-based SSE parser so the
  `Authorization` header and `Last-Event-ID` can be controlled;
- controller token persisted after successful validation in origin-scoped
  `localStorage`, removed on HTTP 401 or explicit sign-out, and retained across
  network/timeout/5xx failures; never store it in cookies, URLs, logs, or
  rendered DOM;
- safe rendering with DOM construction and `textContent`; model/tool output is
  never assigned as raw HTML;
- responsive CSS Grid/Flex layouts for desktop, tablet, and narrow mobile
  screens;
- keyboard, pointer, and touch operation; visible focus; semantic controls;
  labelled dialogs; reduced-motion support; sensible contrast and live-region
  announcements.

The WUI includes:

- connection/authentication and capability status;
- active/completed jobs, progress, results, cancellation, and reconnect/replay;
- ordinary chat and revision conflict recovery;
- interactive agent turns, task mode, Guard review/approve/deny, and stale
  approval handling;
- image job submission and result display;
- workspace review and read-only dired, followed by revision-safe mutations
  when those API routes exist;
- file editor load/save with optimistic concurrency and explicit conflict UI;
- a safe settings view limited to server-exposed non-secret settings.

The browser receives neither provider credentials nor the server's stored
secret. It receives only the user-supplied controller token in JS memory and
sanitized API data. Server responses must not reveal raw databases, absolute
paths, environment variables, TLS material, or hidden project state.

## Testing and validation

Add focused unit and integration coverage with each implementation PR. The
v1.30 release matrix includes:

- fragmented request lines/headers/bodies, malformed framing, conflicting
  lengths, unsupported transfer encodings, timeouts, keep-alive limits, and
  connection exhaustion;
- authentication scope separation, constant-time comparison behavior where
  testable, Host/Origin rejection, redaction, and loopback/non-loopback startup
  policy;
- job state transitions, idempotency collisions, cancellation races, bounded
  retention, event order, replay, replay expiry, slow subscribers, disconnects,
  and clean shutdown;
- one-agent-lane conflicts, turn/approval correlation, stale approval rejection,
  reconnect during Guard Ask, and cancellation without cross-session effects;
- workspace containment, encoded traversal, symlink/reparse races, hidden-state
  exclusion, exact identity, optimistic concurrency, and atomic save failure
  paths;
- MCP 2026-07-28 discovery, headers, schemas, task handles, errors,
  cancellation, scope enforcement, and conformance;
- chat revision conflicts and database failure paths;
- WUI API flows against the mock server: authentication, capability fallback,
  SSE fragmentation/reconnect/replay, Guard review, cancellation, safe text
  rendering, editor conflicts, and error recovery;
- responsive desktop/tablet/mobile layouts; keyboard-only, pointer, and touch
  flows; focus management; reduced motion; CSP; absence of external resources;
  and absence of secret/path leakage;
- sanitizer/leak and fault-injection coverage for listener, TLS, job registry,
  event broker, session shutdown, and embedded-asset failure paths.

Use a controllable mock provider and raw-socket HTTP fixtures. Browser tests may
use a small documented development dependency only if explicitly approved;
the shipped WUI and normal Ainiux runtime remain dependency-free beyond the
project baseline.

## Documentation deliverables

Update, as the corresponding slices land:

- `README.md` and `docs/README.md` with startup, local use, authentication,
  WUI access, cancellation, and examples;
- `docs/api.md` with schemas, status codes, event ordering/replay, limits,
  idempotency, and compatibility rules;
- `docs/security.md` with bind policy, scopes, TLS, tunnels, browser token
  handling, remote Yolo, workspace containment, and threat model;
- `docs/mcp.md` with discovery, configuration, tools, scopes, and examples;
- `docs/web-mode.md` rewritten from postponed snapshot to the shipped WUI
  architecture and accessibility contract;
- `docs/decisions.md` with service boundaries, HTTP subset, concurrency,
  event DTOs, asset embedding, and any approved new dependency.

## Non-goals for v1.30

- OpenAI-compatible `/v1/chat/completions` or Responses proxy.
- Multi-workspace routing in one server.
- Remoting terminal keys, ANSI frames, TUI paint, or editor internals.
- Exposing arbitrary filesystem paths, chat databases, provider keys, config
  secrets, project-private state, or unrestricted shell access.
- Multiple simultaneous interactive agents in one workspace.
- MCP sub-agents or interactive MCP Guard chaining.
- A framework-based SPA, hosted cloud dashboard, extension marketplace, or
  VS Code extension.
- Internet-facing multi-user tenancy, accounts, roles, billing, or untrusted
  shared hosting.

## Implementation order

Each PR must preserve CLI behavior and include its nearest tests and
documentation.

### PR 0 - Existing CLI skill

No behavior change. Record current CLI chat, run, plan, image, agent, Guard,
dired, editor, persistence, cancellation, and error behavior as the compatibility
baseline.

### PR 1 - Surface-neutral operations and wire contract — Landed

Extract reusable operation boundaries, define public DTOs/error mappings,
document concurrency and HTTP limits, and add tests proving CLI output and
cancellation remain unchanged. No listener yet.

### PR 2 - Loopback listener, strict parser, auth, and status — Landed

Add lifecycle, bounded HTTP/1.1 parsing, full-control and MCP-only authentication,
`health`, `status`, `capabilities`, connection caps, and clean shutdown.
Bind loopback only.

### PR 3 - One-shot jobs and serialized agent lane — Landed

Add job registry, idempotency, status, SSE replay, cancellation, and chat/run/
plan/image operations. Enforce the one-workspace agent lane and typed conflicts.

### PR 4 - MCP 2026-07-28 adapter — Landed

Added discovery, protocol headers, deterministic tools, task/job handles,
MCP-only scope, focused conformance coverage, and documentation.

### PR 5 - Interactive agent and Guard — Landed

Add the persistent session controller, correlated turns/approvals, session
snapshots, replay, cancel, review-file, stale-response rejection, and shutdown
semantics.

### PR 6 - Read-only review and dired

Landed. Expose workspace-relative review, directory listing, and bounded file
reads over the existing contained filesystem logic. Project-private state,
sensitive configuration names, special files, and symlink/reparse paths stay
out of the response. Mutations remain deferred.

### PR 7 - Revision-safe chat threads

Landed. Add thread listing/loading/creation/messages with optimistic revision
checks and no raw database exposure. Local TUI saves and remote appends advance
one SQLite revision; bounded remote loads return the newest 512 messages and
attachment metadata without managed-media or source references.

### PR 8 - TLS and direct non-loopback access — Landed

Add the optional TLS dependency and RAII wrappers, strict Host/Origin policy,
remote startup gates, remote-Yolo opt-in, certificate tests, and security docs.

### PR 9 - Revision-safe dired/editor mutations

Landed. Add root-aware explicit-target mutations, file editing and assist, optimistic
concurrency, atomic saves, conflict UI data, and focused race/failure tests.

### PR 10 - Embedded vanilla-JavaScript WUI (v1.30 release gate) — Landed

Added the embedded same-origin responsive controller at `/ui/`, covering jobs,
chat, agent/Guard, image, review/dired, editor, and safe settings. Complete
browser security, reconnect, accessibility, responsive-layout, and end-to-end
tests. Update `docs/web-mode.md` and release documentation.

## Definition of done

v1.30 is done when:

- existing CLI, REPL, TUI, editor, agent, benchmark, grade, and image behavior
  remains intact;
- the stable `/ainiux/v1/` job/session API and MCP adapter share the same core
  operations and cancellation semantics;
- one workspace cannot acquire competing interactive-agent controllers;
- authentication scopes, loopback/TLS policy, Guard correlation, replay,
  backpressure, path containment, revision conflicts, and clean shutdown are
  tested;
- the embedded vanilla-JavaScript WUI is functional and responsive on desktop,
  tablet, and mobile, and passes the security/accessibility matrix;
- no secret, absolute local path, raw database, hidden project state, worker,
  socket, subscriber, TLS handle, or temporary buffer leaks on success, error,
  cancellation, disconnect, or shutdown.

# Execution-plan template

For non-trivial implementation work, add or update a short plan containing:

```md
# Task title

## Goal

## Current foundation and constraints

## User-visible behavior and interfaces

## Implementation changes

## Failure and cancellation behavior

## Tests and acceptance criteria

## Explicit non-goals
```

Prefer current behavior and concise decisions over historical narration. Move completed
implementation details to the appropriate guide under `docs/`, `docs/decisions.md`, or Git history rather than
allowing this roadmap to become another source of stale product truth.
