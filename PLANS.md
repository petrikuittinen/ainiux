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
| v1.0–v1.09 | Local agent foundation and hardening | Landed through v1.09 |
| **v1.1** | **Approximate caller graph, smarter indexing, automatic code-index hints, later `/goal`, `/loop`, and sub-agents** | **Next priority** |
| **v1.2** | **Image generation across CLI and interactive surfaces** | Planned after v1.1 |

Each milestone must leave ordinary CLI chat and existing interactive modes usable.
Do not begin the postponed browser UI before the local server/runtime foundation is
ready.

## Current baseline

Implementation status (2026-07-27): **v1.09**.

The shipped product includes:

- script-friendly Chat Completions and text Responses API paths
- built-in provider profiles and OpenAI-compatible custom/local endpoints
- cancellable SSE/HTTP runtime jobs and provider/model selection
- REPL, chat TUI, standalone editor, benchmark, grade, and document conversion
- SQLite chat persistence plus JSON import/export
- text/Markdown/HTML attachments and supported image input
- safe URL fetch and web search
- syntax highlighting, grapheme-aware editing, multiple editor buffers, and AI assist
- one-shot and interactive local agents with project-local `.ainiux-pr/` state
- Act/Plan task modes, Guard approvals, project-contained writes, and tool logging
- compact live tool rows, bounded reasoning previews, and transcript-preserving `/compact`
- OpenRouter, OpenAI, and DeepSeek credit display when the selected key can query it
- a fast project-local symbol index with incremental discovery and lightweight scanners

The current code index stores files, physical line totals, symbols, ranges, signatures,
documentation hints, and hashes. It does **not** yet store references, a call graph,
PageRank, or automatic task-specific context hints. `find_callers` and `find_callees`
are not implemented.

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

Historical implementation details remain available in Git history and the dated
notes in `README.md` and `docs/decisions.md`.

# v1.1 - Smarter agent indexing and graph-guided context

## Goal

Make the local agent reach useful source code with fewer model rounds, fewer tool
calls, and less context. Extend the existing fast symbol index with an approximate
reference graph, then automatically inject a tiny task-aware orientation block before
the model begins tool work.

This is deliberately an 80/20 system. It must remain fast, local, dependency-light,
and honest about uncertainty. Compiler-grade parsing and complete dynamic-language
resolution are not required.

## Why this is next

`glob`, `search_text`, and `read_file` are already fast locally. The expensive part is
often the sequence of model round trips required to discover the same relationships.
A compact local hint can eliminate several search/read turns, while reverse callers
provide change-impact information that raw text search does not organize.

Success is not “the model used more index tools.” Success is:

- fewer model/tool rounds before the first relevant read or edit
- fewer full-file reads and fewer prompt tokens
- better discovery of callers and likely affected tests
- no meaningful regression in final correctness

## Current foundation to reuse

Keep and extend the existing `src/agent/index/` scanners, `.ainiux-pr/index.sqlite`,
`index::Snapshot`, agent runtime, tool registry, cancellation layer, and guarded
mutation paths.

Current behavior already:

- incrementally refreshes the persistent index at agent startup
- avoids reopening unchanged size/mtime matches
- scans changed files through a bounded worker pool
- commits a completed refresh transactionally
- rescans a file in the live in-memory snapshot after native agent mutations
- treats symbol ranges and names as hints that must be verified against current source

Do not introduce a language-server, compiler frontend, embedding service, or model call
into indexing.

## Reference graph

Extend the index schema with confidence-scored references. A reference should retain:

```text
source file
source/enclosing symbol when known
target spelling
resolved target symbol when known
kind
source line
confidence
optional receiver/type/import evidence
```

Initial edge kinds:

```text
call
import
include
inherit
instantiate
use
```

Unresolved references remain useful records. Do not force a target when a common name,
overload, dynamic dispatch, or missing import makes resolution ambiguous.

Suggested confidence interpretation:

- exact qualified or unique imported resolution: high confidence
- unique same-file or same-scope name: medium/high confidence
- simple inferred receiver type: medium confidence
- ambiguous lexical call-like occurrence: low confidence or unresolved

Every graph consumer must expose that the result is approximate.

## Scanner priorities

First acceptance set:

- C and C++
- Python
- JavaScript and TypeScript
- Java and C#
- Go
- Rust

Add inexpensive rules for PHP, Perl, Ruby, Bash, and other existing programming
scanners where they are reliable. Markdown, markup, stylesheet, data, and
configuration scanners may remain definitions-only unless a relationship is both
cheap and useful.

Common cases to recognize:

- direct global and namespace-qualified calls
- imports/includes/use statements and package/module relationships
- constructors and simple inheritance declarations
- `this.method()`, `self.method()`, and static `Class.method()` calls
- simple lexical receiver inference, for example:

```javascript
const spaceShip = new Sprite(...);
spaceShip.draw();
```

The scanner may associate `spaceShip.draw()` with `Sprite::draw` when scope and naming
make that a reasonable hint. It must not attempt complete dynamic data-flow,
reflection, prototype-chain, macro, or runtime dependency resolution.

## Resolution and stable graph updates

Separate source extraction from graph resolution:

1. A changed file is read and scanned once.
2. Its definitions and raw references replace the previous rows atomically.
3. A resolver uses all stored definitions/import evidence to update affected edges.
4. Incoming and outgoing graph views are published only as a complete snapshot.

Do not leave dangling references when symbol rows are replaced. Use a stable symbol
identity/key during resolution or rebuild affected edges in the same committed update;
database row IDs alone must not be assumed stable across rescans.

Changing a definition can affect references stored in untouched files. Re-resolve the
affected target-name/import buckets without reparsing those files. A global lightweight
resolver pass is acceptable when cheaper than maintaining exact dependency buckets.

## Ranking

Keep these concepts distinct:

- **distinct caller count:** transparent change-impact signal
- **global PageRank:** weak architectural/centrality prior
- **task match:** symbol, qualified-name, path, and identifier-token relevance
- **graph proximity:** callers, callees, imports, owners, and likely tests near task seeds

Global PageRank must not be the primary task ranking. It tends to favor low-level
utilities and framework plumbing while under-ranking entry points and dynamically
selected code.

Task ranking should combine:

```text
symbol/path task match
graph proximity from the strongest task matches
distinct caller count
small global PageRank contribution
entry-point/export/test signals
recently touched signal
```

Use PageRank internally; the model-facing result normally needs caller counts and a
short reason, not an opaque floating-point rank.

## Automatic context hints

Before the first provider request of **every agent user turn**, compute a bounded local
orientation block:

```text
[Approximate code-index hints; verify before editing]
Task matches:
src/provider/provider.cpp: parse_credit_summary (lines 691-766; 4 callers)
src/tui/run.cpp: start_credit_lookup (lines 1320-1371; 2 callers)

Related/high-impact:
tests/unit/provider/test_provider.cpp: test_credit_summary (lines 410-468)
```

Requirements:

- make no provider/model call to produce the block
- use the current user request as the task-ranking seed
- prefer task-relevant symbols, direct graph neighbors, and likely tests
- fall back to a few global architectural anchors only when task matching is weak
- use a small fixed line/byte budget and deterministic ordering
- omit the block when the index has no useful result
- label it exactly as approximate and instruct the model to verify before editing
- include path, qualified symbol name, line range, and caller count where available
- never include raw PageRank values merely because they exist

The hint is request-only context for the active turn:

- it is available before the model chooses its first tool
- it remains available through that turn's tool rounds
- it is replaced for a later user turn rather than accumulated
- it is not written as a durable user/assistant transcript message
- it is excluded from transcript display and compaction history
- it can be regenerated after reopen or `/compact` from the current index

Do not expand or rewrite the built-in agent system prompt as part of this milestone.
The user will separately redesign prompt/tool-selection guidance for small local
models.

## Tool integration

Add:

- `find_callers`
- `find_callees`

Improve existing tools:

- `search_symbol`: include caller count and graph-aware tie-breaking
- `read_symbol`: include a bounded caller/callee impact summary
- `inspect_code_task`: use task matches plus graph proximity rather than token matching alone
- `project_overview`: include a tiny central/entry-point summary
- `search_text`: where cheap, identify the enclosing indexed symbol for each match

The automatic hint is the primary delivery mechanism. New tools remain useful for
deeper inspection but the model must not be required to call them to receive basic
orientation.

## Freshness and background work

Use mutation-aware refresh rather than checking the filesystem after every pure read:

- Native agent writes/edits/removals immediately update or invalidate the exact touched
  files in the live snapshot so subsequent tools observe the new content.
- Queue the persistent definition/reference/graph update for those exact paths through
  one cancellable, coalescing index job.
- After a command capable of modifying or generating source, run an incremental
  filesystem freshness check.
- Before graph-backed queries, consume a completed newer snapshot when available and
  otherwise report bounded staleness rather than racing shared mutable state.
- At task completion, run a full-tree incremental freshness pass and wait for its final
  transaction; only changed files are reparsed.
- Keep forced full rebuild for schema/scanner-version changes, corruption recovery, or
  explicit user action. Do not force a full reparse merely because a project is below a
  line-count threshold.

Full/multi-file scans may use at most approximately 75% of available hardware threads,
with at least one worker and a conservative safety cap. Single-file refreshes should
avoid unnecessary thread creation. Index jobs must be cancellable and owned through
RAII; shutdown cancels or joins them without leaking workers, SQLite handles, or
snapshots.

## Measurement and tuning

Create repeatable before/after agent tasks across small, medium, and large repositories.
Record:

- model rounds before first relevant source read
- model rounds before first edit
- number and type of tool calls
- full-file versus range/symbol reads
- request bytes and estimated/provider-reported tokens
- wall time to first useful edit and final result
- relevant callers/tests missed
- index startup, touched-file, resolver, PageRank, and final freshness latency
- false-positive graph hints and fallback use
- final test/task correctness

Tune output caps and ranking only from representative measurements. A graph feature that
adds context without reducing navigation work is not successful.

## Tests

Add focused coverage for:

- schema migration from the current files/symbols-only database
- extraction, resolution confidence, and unresolved references
- direct calls, imports, inheritance, instantiation, and simple receiver inference
- duplicate names, overloads, shadowing, ambiguous receivers, and dynamic-call fallback
- callers/callees across files and likely-test relationships
- deterministic PageRank and task ranking
- bounded/deterministic automatic hint formatting
- one hint per user turn, replacement without transcript persistence, reopen, and compact
- touched-file updates, removals, renames, command-generated changes, and final freshness
- cancellation before commit and preservation of the previous complete snapshot
- concurrency limits, coalescing, shutdown/join, and SQLite/resource cleanup
- agent comparisons showing fewer navigation calls without correctness regression

Follow the repository slow-test policy. Ordinary implementation slices should run the
nearest unit coverage; integration, sanitizer, and leak suites remain opt-in unless
directly relevant or explicitly requested.

## Acceptance criteria

- Common static callers/callees resolve usefully across the first acceptance languages.
- Ambiguous/dynamic cases remain confidence-scored hints and never masquerade as proof.
- Every agent user turn can receive a small task-aware index block before its first
  provider request without an additional model call.
- Hints are request-only, bounded, deterministic, and do not accumulate in the durable
  transcript.
- Native edits reindex only affected files; command/external changes are detected by an
  incremental pass; task completion leaves SQLite current.
- Graph publication is atomic and cancellation preserves the last complete snapshot.
- `find_callers`/`find_callees` and enriched existing tools verify against current source.
- Representative tasks use fewer navigation rounds/tokens without reducing final
  correctness.
- Existing grep/glob/read and compiler/test fallbacks remain available.

## Reserved v1.1 agent tracks

The following names reserve intended v1.1 product work, but their behavior is **not yet
specified and is not implementation-ready**:

- `/goal`
- `/loop`
- sub-agents

Do not infer command syntax, persistence, concurrency, safety, context sharing, budgets,
permissions, UI, or acceptance criteria from these names. Add their detailed plans only
after the user supplies the specifications. They must reuse the existing runtime,
workspace containment, Guard, cancellation, logging, and credential protections.

## Explicit non-goals

- compiler-grade parsing or complete call graphs
- executing code to discover dynamic edges
- model-generated summaries during indexing
- embeddings or a new indexing dependency
- mandatory index-tool calls in the system prompt
- automatic edits based only on graph rank
- concurrent workspace mutations
- implementing `/goal`, `/loop`, or sub-agents before their specifications exist

# Deferred active tracks

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
