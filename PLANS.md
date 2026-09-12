# PLANS.md

Project: `ainiux`

This is the active product roadmap. It records current workstreams, durable
boundaries, and acceptance criteria. Tactical tasks live in `TODO.md`, current
usage in `README.md` and `docs/README.md`, rationale in `docs/decisions.md`, and
completed releases in `docs/version-history.md` and Git history.

Do not grow this file into a second user guide or release log. When a milestone
lands, keep only any continuing constraint or follow-up and move the result to the
appropriate current guide and version history.

## Product direction

Build a fast, portable command-line and terminal client for OpenAI and
OpenAI-compatible APIs. Every surface must remain script-friendly, responsive,
explicit about failures, safe with credentials and workspace access, cancellable,
and free of resource leaks.

Provider adapters, HTTP/SSE, runtime jobs, persistence, context, Agent/Guard,
filesystem containment, and error handling are shared foundations for:

- one-shot CLI chat, conversion, fetch, and search;
- REPL, full-screen chat, editor, and dired;
- benchmarks and judge grading;
- local Act/Plan/Goal agent workflows;
- CLI and browser image and video generation;
- the authenticated control API, MCP adapter, and embedded browser controller.

Each workstream must preserve existing CLI and interactive behavior unless its
approved specification says otherwise.

## Current foundation

Implementation status (2026-09-12): **v1.34**.

The main product surfaces are shipped. The current foundation includes
Chat Completions and Responses, provider/model catalogs, cancellable streaming,
conversion and common legacy charset support, safe explicit fetch/search, SQLite
and JSON chat persistence, the terminal chat/editor/dired shell, benchmarks,
grading, security review, one-shot and interactive agents, Guard and Act/Plan/Goal
policies, the definitions-only code index, installed MCP clients, and CLI image
generation.

The v1.30 control-server milestone is complete: `/ainiux/v1/` jobs and replay,
the MCP server adapter, interactive remote Agent/Guard sessions, revision-safe
chat and workspace/editor operations, TLS/direct-access gates, and the embedded
same-origin WebUI are implemented. Catalog-driven video generation is in the
headless CLI and browser controller (`fal_queue`, `replicate_predictions`,
`xai_imagine`, `gemini_interactions`, and `gemini_veo`).
v1.31–v1.34 added safe Markdown and full-language
syntax highlighting, model/settings parity, persisted idle agent history,
catalog-driven browser image generation, editor undo/redo, and native-parity
browser indentation/reformatting.

Native Windows x64 implementation is present but unreleased pending the UCRT64
parity gate. The code index intentionally stores definitions and static importance,
not references or graph scores. REPL/TUI image/video generation and the deferred items
listed below remain unimplemented.

## Active workstreams

These tracks may proceed independently when requested. Core reliability and
security constraints apply to all of them; this list does not invent a release
number or strict ordering between otherwise independent work.

### Core reliability and polish

Continue small, test-backed improvements to the shipped product:

- provider capability/error normalization and broader Responses compatibility;
- benchmark cutoff and judge calibration with auditable raw evidence;
- terminal/editor responsiveness, resize behavior, Unicode, and interaction polish;
- cancellation, permission, fault, sanitizer, and leak hardening;
- refactors that remove duplication without destabilizing surface behavior;
- syntax-theme contrast warnings and remaining editor reformat/PTY stress coverage.

Acceptance: each slice has focused regression coverage, preserves documented CLI
stdout/stderr and cancellation behavior, emits actionable errors, and introduces no
new ownership ambiguity or cross-surface duplication.

### Native Windows x64 release gate

Finish release acceptance for Windows 10 1903+/Windows 11 x64 under MSYS2 UCRT64:

- run and retain the manual native build, unit, fault, mock, SQLite, ConPTY,
  sanitizer, packaging, and checksum workflow;
- complete hands-on Windows Terminal and modern conhost checks for every shipped
  mode, streaming/cancellation, Agent/Guard permissions, editor locks/dired,
  indexing, security review, SQLite, PowerShell, and clipboard restoration;
- publish no Windows ZIP until the results are recorded and the parity gate passes.

Windows ARM64, MSVC/CMake, installers, older Windows, and mintty full-screen use
remain out of scope.

### Smarter definitions index

The existing project-local index is optional, definitions-only, mutation-aware,
and queried lazily by Agent. Continue tuning it as a navigation aid:

- benchmark startup, no-change and incremental refresh, peak memory, model rounds,
  tool calls, time to first useful edit, and correctness on representative projects;
- tune declaration importance and visibility across scanner languages;
- enrich text search with the enclosing indexed definition where it is cheap;
- stress generated files, renames/removals, coalescing, cancellation, and shutdown;
- measure tool execution before considering bounded read-only parallelism.

Acceptance: lexical relevance remains ahead of importance, results are deterministic,
mutations immediately update the live touched-file view, persistent publication is
atomic, cancellation preserves the previous completed database, and live filesystem,
compiler, and test verification remain available.

Do not add compiler-grade parsers, language-server dependencies, reference/call
graphs, or automatic request-context hints. Security review keeps its immutable
authorization snapshot. Built-in agent prompt optimization, agent skills, `/loop`,
and sub-agents require separate specifications.

### Interactive image generation

CLI `ainiux image` and one-shot WebUI image generation are shipped using
catalog-selected `openai_images`, `replicate_predictions`, `fal_queue`, and
`gemini_interactions` adapters. Complete the interactive terminal surfaces without
duplicating those adapters:

- add an explicit REPL `/image` command;
- add a cancellable TUI image-generation job with responsive input/navigation;
- reuse `images.conf` capabilities, validation, output naming, overwrite refusal,
  bounded reference inputs, downloads, atomic writes, and credential redaction;
- persist safe metadata and saved paths, never image blobs or provider credentials.

Acceptance: ordinary text prompts never trigger image generation implicitly; status
uses stderr where applicable; cancellation and provider/write failures release all
buffers, handles, temporary files, and workers; existing CLI/WebUI flows remain
compatible.

Batch `n>1`, streaming partials, terminal bitmap protocols, and multi-turn image
editing remain deferred until separately scoped. New models supported by an existing
protocol should normally be `images.conf` records rather than new C++ code.

### Control API and WebUI maintenance

Treat the shipped v1.30–v1.34 server/browser stack as a maintained compatibility
surface, not an unfinished milestone:

- keep `/ainiux/v1/` DTOs, jobs, replay, revisions, Guard correlation, cancellation,
  and authentication scopes stable and capability-detectable;
- retain one fixed workspace, one serialized Agent mutation lane, bounded HTTP/SSE,
  exact relative-path containment, and revision-safe mutations;
- keep the browser same-origin, dependency-free, CSP-safe, accessible, responsive,
  and served only through exact no-store asset routes;
- preserve server-side credentials and prevent absolute paths, databases, project
  private state, environment data, or TLS material from crossing the wire.

Acceptance: focused server and WebUI tests cover any changed behavior, stale clients
fail clearly, slow/disconnected clients cannot block producers or leak resources, and
plain CLI/server behavior remains unchanged.

## Deferred work

These items are intentional future work, not implied current behavior:

- an OpenAI-compatible `/v1` adapter layered beside—not replacing—the Ainiux
  control API;
- a native Anthropic Messages adapter and broader live capability probing;
- richer DOCX support beyond normalized canonical-Markdown conversion (media,
  story parts, lossless edits, remote fetch, transcript/WebUI export), encrypted
  PDF read/write, and broader PDF font/emoji support;
- `/loop`, sub-agents, and a separately designed Agent skill/custom-command system;
- optional core image downscaling/caching before vision requests, using shell-free
  bounded subprocesses and no mandatory image-codec dependency;
- richer interactive image workflows beyond the active REPL/TUI first slice;
- multi-workspace or multi-user server operation.

See `TODO.md` for smaller provider, benchmark, editor, search, persistence,
cancellation, and conversion tasks. Adding an item there does not override the
security, portability, dependency, or surface-separation rules in `AGENTS.md`.

## Planning new work

For a non-trivial implementation slice, record a concise plan with:

```md
# Task title

## Goal and current foundation
## User-visible behavior and interfaces
## Implementation and ownership changes
## Failure, cancellation, and security behavior
## Tests and acceptance criteria
## Explicit non-goals
```

Prefer current decisions and observable acceptance criteria. Do not repeat shipped
implementation detail already owned by user guides, API documentation, decisions,
version history, or tests.
