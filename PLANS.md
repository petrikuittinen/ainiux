# PLANS.md

Project: `ainiux`

This is the active implementation roadmap. It records current product direction,
unfinished milestones, and enough design detail for the next implementation work.
Current agent constraints live in `AGENTS.md`, user-facing behavior in `README.md`,
design rationale in `docs/decisions.md`, and short open work in `TODO.md`.

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
- a future local OpenAI-compatible server
- postponed browser UI and later media capabilities

## Roadmap order

| Milestone | Direction | Status |
| --- | --- | --- |
| v0.0–v0.8 | CLI, persistence, runtime/TUI, providers, context, config, benchmarks, editor | Landed |
| v0.9 | Benchmark calibration, refactor hygiene, TUI/CLI/editor polish | Remaining work continues |
| v0.90 | Local OpenAI-compatible server | Deferred behind v1.1 |
| v1.0–v1.13 | Local agent foundation and hardening | Landed through v1.13 |
| **v1.1** | **Lightweight definition ranking and index tuning; later `/goal`, `/loop`, and sub-agents** | **Next priority** |
| **v1.2** | **Image generation across CLI and interactive surfaces** | Planned after v1.1 |

Each milestone must leave ordinary CLI chat and existing interactive modes usable.
Do not begin the postponed browser UI before the local server/runtime foundation is
ready.

## Current baseline

Implementation status (2026-07-30): **v1.13**.

The shipped product includes:

- script-friendly Chat Completions and text Responses API paths
- built-in provider profiles and OpenAI-compatible custom/local endpoints
- cancellable SSE/HTTP runtime jobs and provider/model selection
- REPL, chat TUI, standalone editor, benchmark, grade, and document conversion
- retained row-diff rendering across chat, agent, and editor terminal surfaces
- SQLite chat persistence plus JSON import/export
- text/Markdown/HTML attachments and supported image input
- safe URL fetch and web search
- syntax highlighting, grapheme-aware editing, multiple editor buffers, and AI assist
- one-shot and interactive local agents with project-local `.ainiux-pr/` state
- Act/Plan task modes, Guard approvals, project-contained writes, and tool logging
- compact live tool rows, bounded reasoning previews, and transcript-preserving
  three-strategy `/compact`: local-only `fast`, loss-aware default `smart`, and
  active-model `summary`, with a universal 75% automatic threshold
- OpenRouter, OpenAI, and DeepSeek credit display when the selected key can query it
- a fast project-local symbol index with incremental discovery and lightweight scanners

The current v1.1 index stores files and definitions for every supported scanner
language, plus static declaration importance. It intentionally has no reference
graph or automatic model-context hints.

## Compact release history

| Version | Main result |
| --- | --- |
| v0.0 | Repository/build/test and leak-check foundation |
| v0.1 | Script-friendly OpenAI-compatible CLI and LM Studio profile |
| v0.2 | REPL and persistence foundations |
| v0.3 | Cancellable runtime jobs and full-screen TUI |
| v0.4 | Provider registry, compatibility profiles, and Responses API |
| v0.5 | Context policies, attachments, and safe URL fetching |
| v0.6 | Layered TOML-like configuration |
| v0.7 | Concurrent benchmark runner |
| v0.8 | Standalone AI-assisted editor |
| v0.81–v0.98 | TUI/editor/provider/search/highlighting/model-catalog hardening |
| v0.99 | Read-only whole-project security review |
| v1.01 | One-shot local agent |
| v1.02 | Ordinary workspace mutations |
| v1.03 | Project-local agent sessions and history |
| v1.04 | Interactive agent and live tool activity |
| v1.05 | Agent chrome and command guard |
| v1.06 | Interactive Guard approvals and clipboard/UI polish |
| v1.07 | Session-scoped Act/Plan modes |
| v1.08 | Provider reasoning previews and in-place activity rows |
| v1.09 | Stable prompt caching/accounting, Smart read-only commands, and context polish |
| v1.10 | Lightweight definition importance, optional index startup, mutation-aware refresh, and one-shot tool metrics |
| v1.11 | Three-strategy agent context compaction with visible progress and result reporting |
| v1.12 | Retained terminal row rendering and punctuation-aware Markdown highlighting |
| v1.13 | First-run index build offer, index chrome marker, timed index summary preface, and index progress lifetime fix |

Historical implementation details remain available in Git history and the dated
notes in `README.md` and `docs/decisions.md`.

# v1.1 - Lightweight smarter agent indexing

## Current implementation

The project index remains a fast, optional definitions-only navigation aid across every v1.00 scanner language. SQLite stores metadata, files, and symbols with a compact 0–100 static importance score computed during the existing definition scan from declaration kind, visibility, and scope. It stores no references, evidence, call edges, caller counts, or graph scores.

Lexical relevance is authoritative: full-name matches rank above exact identifier components, which rank above component-prefix matches. Multi-token coverage is preserved, importance orders only otherwise comparable hits, and path/line/ID ties are deterministic. `project_overview`, `search_symbol`, `get_skeleton`, `read_symbol`, `find_tests`, `inspect_code_task`, index management, and `replace_symbol` remain available; caller/callee tools and automatic provider-context hints are absent.

Agent navigation prefers one `read_many` call whenever two or more independent paths or ranges are known, including with native parallel tool calling; `read_file` remains the single-target fallback. Batched items have independent 64 KiB default limits, line-numbered content and hashes, within the 256 KiB aggregate cap.

Agent startup performs a cheap read-only index probe and never prompts, creates an index, loads a full snapshot, or waits for freshness. Missing/corrupt indexes become ready with live filesystem tools and a display-only `/index-code` hint. Existing completed indexes are queried lazily through short-lived read-only SQLite transactions and refreshed in the background only after readiness. `/index-code` explicitly creates/enables an index, while `/show-index` refreshes and appends the compact totals table without adding it to model context. Native mutations keep a touched-path overlay until their coalesced SQLite revision completes; task completion runs a full-tree freshness pass that reparses only added or changed files. Security review alone retains an eager immutable authorization snapshot. Query/refresh failures preserve the previous completed database and fall back to live tools where applicable.

Schema versions 1–3 migrate transactionally by rescanning definitions, adding importance, and dropping `refs` and `symbol_scores`. Cancellation before commit preserves the previous completed snapshot. A cancellable post-commit SQLite compaction reclaims obsolete graph storage; compaction failure leaves the migrated index valid and emits a warning.

## Next tuning

- benchmark fresh and no-change refresh time and peak memory on representative projects
- tune declaration visibility inference without adding parser or language-server dependencies
- enrich text search with enclosing definitions where cheap
- retain live filesystem, compiler, and test verification because indexed locations are hints
- keep `/goal`, `/loop`, and sub-agent behavior reserved until separately specified

## Remaining v0.9 polish

Continue in small, test-backed slices:

- benchmark cutoff/judge calibration and stronger assessment data
- provider/API error and compatibility hardening
- TUI/editor responsiveness, Unicode, resize, and interaction polish
- refactor hygiene where it removes duplication without destabilizing behavior
- focused cancellation, permission, sanitizer, and leak hardening
- syntax-theme contrast warnings and remaining editor reformat/PTY coverage

These are maintenance tracks, not a reason to delay v1.1 graph work indefinitely.

## v0.90 local OpenAI-compatible server

Server mode remains planned but follows v1.1 and precedes v1.2 in the current priority
order unless the user reprioritizes it.

Minimum direction:

- dedicated `--server` entry
- loopback bind by default; explicit authenticated opt-in for LAN
- initial `/v1/models` and non-streaming `/v1/chat/completions`
- reuse provider/runtime/cancellation/redaction layers
- no arbitrary local-file access or agent tools
- bounded requests, clean disconnect cancellation, and OpenAI-compatible errors

Browser UI remains postponed and should build on the server/runtime foundation if
revived.

# v1.2 - Image generation

## Goal

Add first-class provider-backed image generation after v1.1, reusing provider profiles,
runtime jobs, cancellation, error handling, persistence, and credential redaction.

Initial surfaces:

- explicit non-interactive `ainiux image` command
- REPL `/image` commands
- cancellable TUI image-generation job
- future server/browser exposure only after those surfaces exist securely

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
ainiux image -p "A quiet terminal workspace at night" --image-model MODEL
ainiux image --prompt-file prompt.txt --size 1024x1024 --format png
ainiux image -p "diagram of provider adapters" --output diagram.png
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
implementation details to `README.md`, `docs/decisions.md`, or Git history rather than
allowing this roadmap to become another source of stale product truth.
