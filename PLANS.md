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
- a future local control-API server (`ainiux server`, **v1.3**), with a later OpenAI-compatible `/v1` adapter
- postponed browser UI and later media capabilities

## Roadmap order

| Milestone | Direction | Status |
| --- | --- | --- |
| v0.0–v0.8 | CLI, persistence, runtime/TUI, providers, context, config, benchmarks, editor | Landed |
| v0.9 | Benchmark calibration, refactor hygiene, TUI/CLI/editor polish | Remaining work continues |
| v0.90 | Unified terminal bindings; this number did not ship a local server | Landed (bindings). Server work is **v1.3** |
| v1.0–v1.15 | Local agent foundation, hardening, and documentation overhaul | Landed through v1.15 |
| v1.16 | Editor dired directory browser (`--dired`, F4, Ctrl+X d) | Landed |
| v1.17 | Mid-turn agent↔editor/dired review, dired history line-diff, agent chrome/tool polish, Apple Silicon builds | Landed |
| v1.18 | Short native tool API without legacy aliases, MCP client/vision bridge, and combined grep filters | Landed |
| v1.19 | Settings widget, Goal-only `goal_met`, and charset conversion to UTF-8 | Landed |
| Native Windows x64 | UCRT64 native target and portable ZIP; all-mode parity gate | Implementation landed; native acceptance pending |
| **v1.1** | **Lightweight definition ranking and index tuning; later `/goal`, `/loop`, and sub-agents** | **Next priority** |
| **v1.2** | **Image generation across CLI and interactive surfaces** | CLI `ainiux image` landed; REPL/TUI remaining |
| **v1.3** | **Remote control API** (`ainiux server`): HTTP `/ainiux/v1`, MCP adapter, thin editor, remote dired | Specified; **do not implement yet** |

Each milestone must leave ordinary CLI chat and existing interactive modes usable.
Do not begin the postponed browser UI before the v1.3 control-API foundation is
ready. Do not start v1.3 implementation until the user explicitly asks.

## Current baseline

Implementation status (2026-08-18): **v1.19**.

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

The compact v0.0–v1.19 timeline lives in `docs/version-history.md`. Historical
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
superseded: remote control is **v1.3** below. An OpenAI `/v1` adapter remains a
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
- server/browser exposure only after v1.3 listen/auth exists (one-shot `POST /ainiux/v1/image`)

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

# v1.3 - Remote control API (`ainiux server`)

**Specified. Do not implement until the user explicitly asks.**

A WUI, VS Code extension, and other Ainiux agents are **consumers** of this API,
not deliverables of v1.3. The CLI skill for foreign agents to shell out to the
existing one-shot CLI is already at `docs/skills/ainiux-cli/SKILL.md`.

Follows current v1.1 / remaining v1.2 work unless the user reprioritizes it.

## Goal

Run Ainiux as a long-lived headless **server** so a remote host can drive it
over the network:

- one-shot chat, `run` / `plan`, `image`
- interactive agent sessions with live events, cancel, Guard Ask, and a safe
  settings subset
- chat threads on the user sqlite library
- thin editor (paths + AI assist jobs, not a remote piece table)
- **remote dired** (listing, RO preview, dirty/history-diff, file ops)
- MCP server adapter so another Ainiux (already an MCP client) can call this
  workspace

The server owns **provider keys**, **workspace disk**, and **databases**
(`~/.ainiux/` chat library, project `.ainiux-pr/`). The remote host is a
controller, not a file-sync client.

## Why this is not the old OpenAI proxy

An OpenAI-compatible `/v1/models` + `/v1/chat/completions` server would let
SDKs treat Ainiux as OpenAI. That does not give sessions, Guard, cancel,
settings, dired, or workspace tools.

v1.3 is an **Ainiux control plane**. The same listen/auth stack can grow a
`/v1/chat/completions` adapter later. Keep URL namespaces distinct:

| Prefix | Role |
| --- | --- |
| `/ainiux/v1/...` | Control API (this milestone) |
| `/v1/...` | Reserved for a future OpenAI-compatible proxy |
| `/mcp` | MCP Streamable HTTP adapter (PR 5) |

## Product choices

| Topic | Choice |
| --- | --- |
| Surface | HTTP control API + later MCP adapter on the same daemon |
| Process | Headless `ainiux server` / `--server`. Not attach-to-TUI |
| Bind | Default `127.0.0.1:8745`. `--bind 0.0.0.0` is explicit |
| Auth | `Authorization: Bearer` with `AINIUX_SERVER_SECRET` (**not** `AINIUX_API_KEY`) |
| TLS | Required for non-loopback unless `--insecure-plain-bind` |
| Keys / disk | Stay on the server. One `--workspace` (default cwd) |
| Guard Ask | HTTP sessions proxy Ask to the remote host. MCP chaining stays headless (Ask deny) |
| TAB | Remote host draws it. Completions endpoint is later |
| Priority | CLI (skill, landed) → agent → chat → editor → **dired** |

## Architecture

```text
  WUI / VS Code / curl          other Ainiux (MCP client)        bash SKILL
           |                              |                          |
    HTTP control API                 MCP adapter              existing CLI
    /ainiux/v1 + SSE                  /mcp                     (unchanged)
           \                              |                          /
            session hub (jobs, cancel, Guard gate, settings, dired)
            AgentController / SessionRuntime / chat sqlite / image / DiredState
            provider + runtime + redaction (unchanged)
```

Rules:

- UI code (TUI, editor paint) does not speak HTTP. The daemon never owns an
  alternate screen.
- Provider HTTP, SSE parsing, credentials, cancellation, and persistence stay
  in existing modules. The server **drives** them.
- `AgentController` is already surface-neutral (`AgentSurfaceEvent`, including
  `GuardApproval`). The TUI is one subscriber; the control API is another.
- `DiredState` and `src/editor/dired.*` already own listing, hashes, history
  line-diff, and file ops. The server maps those functions to JSON. Do not
  reimplement dired in `src/server/`.
- New listen/auth/router code lives under `src/server/` and
  `src/app/server_mode.cpp`. Do **not** use reserved `src/web/`.
- One authenticated principal per server process (holder of the server secret).
  No multi-user ACLs.

### Session hub

| Session kind | Backing | PR |
| --- | --- | --- |
| One-shot job | In-memory job id + `CancellationSource` | 2 |
| Agent | `AgentController` + project `.ainiux-pr/agent.sqlite` | 3 |
| Chat | existing TUI sqlite thread in `~/.ainiux/ainiux.db` | 4 |
| Editor | buffer list + read/write + AI assist jobs — not a remote piece table | 7 |
| Dired | `ainiux::editor::DiredState` under `--workspace` | 8 |

Constraints:

- Server `--workspace PATH` (default: process cwd). All agent/dired/editor
  paths stay under that root. Per-session arbitrary roots are **denied**.
- At most one in-flight generation per session.
- Global cap on concurrent generations (default **4**, `--max-concurrent-jobs`)
  and sessions (default **32**, `--max-sessions`).
- Disconnect of an SSE subscriber does **not** auto-cancel. Cancel is an
  explicit POST. Optional later: `cancel_on_disconnect=true` per turn.

## CLI entry

```text
ainiux server [options]
ainiux --server [options]
```

Foreground only. No double-fork. systemd/nssm may wrap it. SIGINT/SIGTERM
cancel jobs, finish open agent sessions, join workers, close sockets.

```text
--bind 127.0.0.1          # default; also 0.0.0.0. Later: ::1 / ::
--port 8745
--workspace PATH          # default cwd
--server-secret TOKEN     # or --server-secret-file PATH, or env AINIUX_SERVER_SECRET
--tls-cert PATH --tls-key PATH
--insecure-plain-bind     # extra flag required to serve 0.0.0.0 without TLS
--cors-origin URL         # repeatable; default none
--max-sessions 32
--max-concurrent-jobs 4
```

Exclusive of TUI/editor/repl (same idea as `--list-models` / `--add-mcp`).

`[server]` keys may live in `config.conf` for bind/port/workspace/tls **paths**.
The **secret must not** be stored in `config.conf`. Only flag, env, or a 0600 /
protected-DACL secret file.

## Security

### Server secret vs provider key

Do **not** reuse `AINIUX_API_KEY`. That env var is already a provider
credential. Mixing them would put the OpenAI key on the wire as the bind
password, or block provider auth when the server is up.

Clients send `Authorization: Bearer <secret>`. Constant-time compare. Never log
the secret. Never accept it in query strings. Redact `Authorization` like
existing HTTP client redaction.

- **Loopback:** secret required. If omitted, generate 32 random bytes (hex),
  print **once** on stderr (`server secret: …`).
- **Non-loopback (`0.0.0.0`):** `--server-secret` or secret-file is mandatory
  (no auto-generate). TLS is required unless `--insecure-plain-bind` is also
  set (loud stderr banner, still requires the secret).

### TLS

Plaintext + secret on `0.0.0.0` is MITM-able. That is a test escape hatch, not
the supported WAN mode.

Listen/auth ships HTTP/1.1 first. TLS is PR 6 (`--tls-cert` / `--tls-key`):

- Prefer OpenSSL via pkg-config when present. Justify in `docs/decisions.md`.
  If devel headers are missing, the binary still serves HTTP; non-loopback
  without `--insecure-plain-bind` fails with a rebuild-or-Caddy message.
- Native Windows can use UCRT64 OpenSSL or a later SChannel path; do not block
  the POSIX server on SChannel.
- Until TLS ships, the supported remote pattern is SSH/Tailscale to loopback.

CORS: default deny. A browser WUI on another origin must pass `--cors-origin`.
Never `Access-Control-Allow-Origin: *` when a secret is in use. Do not add
cookie auth (loopback CSRF).

### Never expose

- Provider keys, `--header` values, secret files
- Raw chat DB / `agent.sqlite` files
- Paths outside `--workspace` (and existing agent Guard rules inside it)
- Unredacted tool logs

`GET /health` may be unauthenticated and returns only `{"ok":true}`.
Authenticated `GET /ainiux/v1/status` may show version, bind, workspace,
session counts.

## HTTP control API

HTTP/1.1, in-tree. No extra HTTP library for listen. Bounded request bodies.
`Host` must match bind. RAII sockets (POSIX `close`, Win32 `closesocket` +
existing Winsock init). Workers post events; they do not mutate sockets from
random threads without a queue. JSON via `src/json/`.

Errors use existing `ErrorCode` mapping plus HTTP status (`400` bad args,
`401` auth, `404` session, `409` turn already running, `429` cap, `500`
internal). Body shape:

```json
{"error":{"code":"cancelled","message":"…"}}
```

### One-shot (PR 2)

```text
GET  /health
GET  /ainiux/v1/status
POST /ainiux/v1/chat
POST /ainiux/v1/run
POST /ainiux/v1/plan
POST /ainiux/v1/image
POST /ainiux/v1/jobs/:id/cancel
GET  /ainiux/v1/jobs/:id
```

Reuse `app::run_agent_goal`, CLI chat, and image mode. Optional SSE when
`Accept: text/event-stream`. Headless Guard Ask remains deny (same as CLI
`run`).

### Interactive agent (PR 3)

```text
POST   /ainiux/v1/sessions                  { "kind":"agent", "permission_mode"?:… }
GET    /ainiux/v1/sessions
GET    /ainiux/v1/sessions/:id
DELETE /ainiux/v1/sessions/:id

POST   /ainiux/v1/sessions/:id/turns        { "content":"…" }
POST   /ainiux/v1/sessions/:id/cancel
GET    /ainiux/v1/sessions/:id/events       SSE of AgentSurfaceEvent

POST   /ainiux/v1/sessions/:id/approval     { "decision":"allow"|"deny" }
GET    /ainiux/v1/sessions/:id/settings
PUT    /ainiux/v1/sessions/:id/settings
GET    /ainiux/v1/sessions/:id/review-file  bounded UTF-8 when Guard has review_path
```

SSE names map from `AgentSurfaceEvent::Type`: `progress`, `phase`, `prepare`,
`index`, `delta`, `tool`, `notice`, `thinking`, `guard`, `turn_done`,
`turn_error`. Thinking/notice stay display-only.

Guard: worker blocks in `ApprovalGate::request`; SSE emits `guard`; remote POST
`allow`/`deny` calls `resolve`. Turn cancel → `Cancelled`. The remote host
draws y/n/review; the server does not. Full dired review of `review_path` is
PR 8.

Settings subset: model, provider, api, reasoning, temperature, max_tokens,
permission_mode, task_mode, goal text, thinking preview cap, cmd-out.
Never settable: keys, extra auth headers, server secret, bind, workspace root,
`--insecure-tls`, MCP stdio argv.

### Chat (PR 4)

`kind: "chat"`. Persistence is `~/.ainiux/ainiux.db`. No workspace tools.
Cancel in-flight provider stream.

### Thin editor (PR 7)

Do **not** serialize the piece table, splits, or `FILE.LOCK` over the wire.
VS Code already edits files on the shared disk. If needed:

- list/open/save paths under workspace
- cancellable AI assist job (existing editor assist)

### Remote dired (PR 8)

Do **not** remote the TUI key map, cheat sheet, or styled listing paint. The
remote host implements the widget (WUI, VS Code tree, another TUI). The server
exposes `DiredState` and the existing operations in `src/editor/dired.hpp`.

This is the remote equivalent of `ainiux -d` / F4 / Guard **Review**: browse
the server workspace, preview files, see content-hash dirty markers and
`.ainiux-pr/history` line-diff, and run the same light file ops.

Reuse, do not fork:

- `dired_open` / `dired_refresh` / `dired_sync_live`
- `dired_set_sort` / selection
- `dired_activate_selection` / `dired_go_parent` / `dired_go_deeper`
- RO view + `view_changed_lines` / `dired_goto_next_changed_block`
- `dired_toggle_pass_selected`
- `dired_rename_selected` / `dired_copy_selected` / `dired_delete_selected` /
  `dired_touch_selected` / `dired_create_file` / `dired_create_directory`

```text
POST /ainiux/v1/sessions                    { "kind":"dired", "path"?: "." }
GET  /ainiux/v1/sessions/:id/dired          listing snapshot
POST /ainiux/v1/sessions/:id/dired/open     { "path_or_glob" }
POST /ainiux/v1/sessions/:id/dired/refresh
POST /ainiux/v1/sessions/:id/dired/sync     # dired_sync_live (agent writes)
POST /ainiux/v1/sessions/:id/dired/sort     { "key":"name"|"size"|"date", "ascending": true }
POST /ainiux/v1/sessions/:id/dired/select   { "index" } or { "name" }
POST /ainiux/v1/sessions/:id/dired/enter
POST /ainiux/v1/sessions/:id/dired/parent
POST /ainiux/v1/sessions/:id/dired/deeper
GET  /ainiux/v1/sessions/:id/dired/view     RO preview + changed_lines + hashes
POST /ainiux/v1/sessions/:id/dired/view     { "path"? }  open RO view
POST /ainiux/v1/sessions/:id/dired/view/close
POST /ainiux/v1/sessions/:id/dired/pass     toggle dirty/reviewed on selected file
POST /ainiux/v1/sessions/:id/dired/rename   { "to":"…", "overwrite": false }
POST /ainiux/v1/sessions/:id/dired/copy     { "to":"…", "overwrite": false }
POST /ainiux/v1/sessions/:id/dired/delete   { "recursive": false }
POST /ainiux/v1/sessions/:id/dired/touch
POST /ainiux/v1/sessions/:id/dired/create-file { "name":"…" }
POST /ainiux/v1/sessions/:id/dired/create-dir  { "name":"…" }
```

Listing JSON is a snapshot of `DiredEntry` fields (name, path, directory,
parent, symlink, hidden, executable, size, mtime, POSIX mode/owner/group when
non-Windows, content_hash, dirty) plus directory, glob, sort, focus, selected
index. Do not send `tui::StyledLine`.

RO view JSON: path, UTF-8 content (existing editor/dired size caps),
`content_hash`, `changed_lines` (bool per source line, or a compact run-length
form), `has_history_baseline`, optional `[diff N]`. This is the same
poor-man's history line review as local dired, not git.

**Containment:** every path is resolved then compared to `--workspace`. Reject
escape, `~user`, mid-path `~`, and writes into `.ainiux-pr` except **reading**
history backups for the line-diff. Same symlink/reparse refusal as local dired
and agent tools.

**Mutations** are user-initiated (authenticated remote host), same as local
dired: they do **not** go through agent Guard. They still cannot leave the
workspace. Overwrite and recursive-delete flags are explicit in the JSON body
(local dired prompts in the minibuffer; the remote host prompts, then sends
the confirmed request).

**Live agent review:** if an agent session is turning and a dired session is
open on the same workspace, call `dired_sync_live` and optionally emit SSE
`dired_changed` (listing and/or view hash changed). Guard `review_path` may
open or reuse a dired session focused on that file (`dired_return_to_guard`
semantics: after preview, the remote still POSTs `allow`/`deny` on the agent
session). Mid-turn F4-style review must not cancel the agent turn
(`AgentController` already preserves the turn).

**Not in PR 8** (same as local dired today): multi-file marks, trash, git
status colors, side-by-side diffs, recursive `**` globs, theme paint, opening
a writable editor buffer (`o` / `n` locally leave dired — remotely that is PR 7
or the host’s own editor on a GET of the file).

## MCP adapter (PR 5)

Ainiux is an MCP **client** today. Serving Streamable HTTP on `/mcp` is the
agent-to-agent path. Reuse `src/mcp/protocol.cpp` JSON-RPC helpers; add a
server dispatcher.

| Tool | Maps to |
| --- | --- |
| `run` | one-shot Act on **this** server workspace |
| `plan` | one-shot Plan |
| `chat` | one-shot chat |
| `image` | one-shot image |
| `status` | authenticated status |

Do **not** expose `read`/`edit`/`bash` of the server disk as MCP tools
(chaining means “ask the other Ainiux to work on its workspace”). Guard for
MCP tools is headless Ask-deny. Humans use the HTTP control API.

stdio MCP (`ainiux server --mcp-stdio`) is optional later.

## Listen implementation

| File | Role |
| --- | --- |
| `src/server/listen.*` | RAII listen/accept, IPv4, loopback vs any, port 0 for tests |
| `src/server/http1.*` | Request parse, response writer, SSE writer |
| `src/server/auth.*` | Bearer secret, bind policy, generate-on-loopback |
| `src/server/router.*` | Path table |
| `src/server/hub.*` | Session/job table, caps, cancel |
| `src/server/events.*` | `AgentSurfaceEvent` → JSON |
| `src/server/dired_api.*` | JSON map over `DiredState` (PR 8) |
| `src/server/tls.*` | Later OpenSSL wrap of accepted fds |
| `src/app/server_mode.cpp` | Accept loop (main thread mutates hub) |

Tests: `tests/unit/server/` parse, auth, bind policy, router, JSON errors,
loopback libcurl against port 0, Guard allow/deny, dired containment and
rename/delete refusal outside workspace. Default loop: `make` + `make test-unit`
plus targeted server tests. Slow suites only if requested.

## Documentation when code ships

`docs/server.md` (new), `docs/cli.md`, `docs/security.md`, `docs/decisions.md`,
`docs/dired-mode.md` (remote pointer), `docs/mcp.md` (when adapter ships),
`README.md`, `AGENTS.md`, this file, `TODO.md`, `docs/version-history.md`.
Do not pretend a WUI exists.

## Non-goals

- Implementing v1.3 until the user asks
- Shipping a WUI or VS Code extension
- Attach/control a running TUI session
- OpenAI `/v1/chat/completions` proxy in the first PR series
- Remote piece-table, splits, or `FILE.LOCK`
- File sync / workspace upload
- Multi-tenant users, OAuth, cookies
- `/loop` and sub-agents
- Reusing `AINIUX_API_KEY` as the bind password
- Storing the server secret in `config.conf`
- MCP `read`/`edit` of the server disk
- Auto-cancel on SSE drop
- HTTP/2 or WebSockets (SSE + POST is enough)
- New package manager; no Rust/Go
- Remoting dired keybindings or styled TUI rows

## Key decisions

1. Control API is native; MCP is an adapter; OpenAI `/v1` is a later adapter.
2. Headless daemon, not TUI attach.
3. `/ainiux/v1` prefix so a future OpenAI proxy can own `/v1`.
4. `AINIUX_SERVER_SECRET`, not `AINIUX_API_KEY`.
5. Loopback default; `0.0.0.0` is explicit; TLS required for non-loopback
   unless `--insecure-plain-bind`.
6. In-tree HTTP/1.1. New dependency only for optional TLS (OpenSSL).
7. Reuse `AgentController`, `ApprovalGate`, and `DiredState`.
8. Single workspace per server process. No remote-supplied roots.
9. HTTP Guard proxy for humans; MCP Ask-deny for agents.
10. Provider keys never leave the server.
11. Phase 0 skill is documentation of the existing CLI (landed).
12. Thin editor is path I/O + assist, not a remote TUI.
13. Remote dired is JSON over existing `dired_*` operations, not a second
    directory browser.

## PR plan

### PR 0 — CLI skill for external agents (landed)

- **Title:** Document existing one-shot CLI as `SKILL.md`
- **Files:** `docs/skills/ainiux-cli/SKILL.md` (installed under
  `share/ainiux/skills/ainiux-cli/`)
- **Status:** In tree. No daemon.

### PR 1 — `ainiux server` listen, HTTP/1.1, auth, health

- **Files:** `src/server/listen.*`, `http1.*`, `auth.*`, `router.*`,
  `src/app/server_mode.cpp`, `src/main.cpp`, `src/cli/args.*`,
  `tests/unit/server/`, `docs/server.md` stub, `docs/decisions.md`,
  `docs/security.md`
- **Depends on:** nothing (PR 0 is independent)
- **Changes:** Bind default 127.0.0.1:8745; secret policy; `GET /health`;
  `GET /ainiux/v1/status`; reject unauthenticated product routes; graceful
  SIGINT. No model calls. `0.0.0.0` without secret refused; without TLS
  requires `--insecure-plain-bind`.

### PR 2 — One-shot control endpoints + job cancel

- **Depends on:** PR 1
- **Changes:** `POST /ainiux/v1/{chat,run,plan,image}`; job ids;
  `POST .../cancel` via existing `CancellationToken`. Optional SSE. Caps.
  Workspace = `--workspace`. Headless Ask remains deny.

### PR 3 — Interactive agent sessions

- **Depends on:** PR 2
- **Changes:** Hub wrapping `AgentController`. SSE of `AgentSurfaceEvent`.
  Guard POST allow/deny. Review-file GET. Safe settings subset in
  `agent.sqlite`. Delete session → `shutdown(true)`.

### PR 4 — Chat sessions

- **Depends on:** PR 3 (session/SSE/cancel)
- **Changes:** `kind:chat` on the user sqlite library. No tools.

### PR 5 — MCP server adapter

- **Depends on:** PR 2 (one-shot). PR 3 not required.
- **Changes:** `/mcp` Streamable HTTP. Tools `run`/`plan`/`chat`/`image`/`status`.
  Ask-deny. Same Bearer secret. Other Ainiux installs the URL with
  `--add-mcp --mcp-allow-private` when loopback.

### PR 6 — TLS for non-loopback

- **Depends on:** PR 1 (can land after PR 2+)
- **Changes:** `--tls-cert`/`--tls-key`. Clear error if OpenSSL missing at
  build. Caddy-in-front remains documented.

### PR 7 — Thin editor

- **Depends on:** PR 1 (workspace containment). Chat (PR 4) is not required.
- **Changes:** list/read/write paths under workspace + cancellable AI assist.
  No piece table on the wire.

### PR 8 — Remote dired

- **Depends on:** PR 1. Guard-review integration and live `dired_changed`
  need PR 3. Can ship listing/preview/file-ops after PR 1 alone, then hook
  agent review.
- **Files:** `src/server/dired_api.*`, hub `kind:dired`, tests for
  containment and overwrite/recursive flags, `docs/server.md`, pointer from
  `docs/dired-mode.md`
- **Changes:** JSON snapshots of `DiredState`; map POST verbs onto existing
  `dired_*` functions; RO view includes `changed_lines`; pass/rename/copy/
  delete/touch/mkdir/create-file; path containment under `--workspace`;
  optional SSE while an agent turn mutates files; Guard `review_path` can
  open this session without cancelling the turn.
- **Not in this PR:** remoting keys/paint, multi-mark, git, writable `o`/`n`
  into a piece-table buffer (that is PR 7 or the host editor).

### Suggested implementation order (when asked)

1. PR 1 + PR 2 — usable remote one-shot
2. PR 3 — interactive agent (product priority after CLI)
3. PR 8 — remote dired (agent review surface for a WUI)
4. PR 5 when chaining is needed (can precede chat)
5. PR 4 chat, PR 6 TLS, PR 7 thin editor

## Definition of done (whole v1.3, not one PR)

- `ainiux server` listens on loopback with a secret and does not leak provider keys
- Remote can one-shot chat/run/plan/image and cancel
- Remote can run an interactive agent turn, see tool rows, cancel the provider
  call, and answer Guard Ask
- Remote can browse the workspace with dired semantics (list, preview, dirty,
  history line-diff, contained file ops) without a TUI
- MCP adapter designed so a second Ainiux can `--add-mcp` this server
- CLI skill remains valid for bash agents without the daemon
- Tests cover auth, bind policy, cancel, Guard allow/deny, dired containment,
  and error-path cleanup
- Docs match: this is the Ainiux server, not a WUI and not an OpenAI-only proxy

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
