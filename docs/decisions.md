# Decisions

These records explain implementation rationale. For current usage, start at the [documentation index](README.md).

## C++17 and Makefile

The implementation uses C++17 and a plain Makefile across POSIX-like systems and
the native MSYS2 UCRT64 Windows target. CMake, MSVC, and a package-manager-owned
runtime are deliberately not required.

## Native Windows x64 foundation

Windows 10 1903+ and Windows 11 x64 use an official UCRT64 build because it
retains GCC/GNU Make, produces a native Win32 executable, and allows a portable
ZIP without the MSYS POSIX runtime. `wmain` is the process boundary: UTF-16
arguments and wide environment/path values become internal UTF-8. A manifest
declares long-path and UTF-8 awareness, and portable resources are also searched
under `share/ainiux` beside the executable.

`src/platform/` owns UTF conversion, environment discovery, identity-based path
containment, protected DACL creation, secure temporary siblings, durable atomic
replacement, and native error text. Existing objects are compared by volume/file
identity; new targets are anchored to a canonical existing parent. Path strings
are never globally lowercased. Windows-sensitive operations reject drive-relative
paths, alternate streams, device names, caller NT namespaces, and reparse-point
escapes. Persistent project-relative names keep `/` and enumerated casing so the
SQLite and JSON schemas stay portable.

`src/runtime/subprocess.*` is the common bounded/cancellable process interface.
POSIX uses `fork`/`execve` and process groups. Windows uses `CreateProcessW`, an
explicit inherited-handle list, anonymous pipes, and a kill-on-close Job Object
assigned while the child is suspended. Both return LF-normalized valid UTF-8,
explicit termination reasons, 64-bit exit status, and bounded stdout/stderr.
The headless interrupt guard similarly maps SIGINT to `SetConsoleCtrlHandler`.

Full-screen terminal ownership is one RAII object shared by chat, agent, and
editor. Its POSIX backend retains termios; its Windows backend saves console
modes/code pages and enables virtual-terminal input/output. Windows Terminal and
modern conhost are supported; mintty/non-console full-screen sessions fail with
an actionable error. Native `CF_UNICODETEXT` replaces clipboard helper processes
on Windows. The package and native acceptance policy are documented in
[Native Windows](windows.md).

## HTTP Transport

`src/http/` owns HTTP transport and uses libcurl through RAII wrappers for the easy handle and header list. The Makefile discovers build flags with `pkg-config libcurl`, falling back to `curl-config`. Streaming response bodies are delivered through libcurl write callbacks so provider code can parse SSE incrementally without spawning the `curl` executable.

## JSON Facade

`src/json/` is a small internal JSON facade used for request escaping and provider response parsing. The project should replace or expand it with a reviewed JSON library when dependency installation is available.

## Installed Defaults and User Configuration

The canonical TOML-alike defaults live in `config/ainiux.conf` and the companion
specialized documents. `make install` installs them below the selected prefix's
`share/ainiux/` directory, where upgrades replace the shipped defaults. Runtime
share lookup also considers `$XDG_DATA_HOME/ainiux` and `~/.local/share/ainiux`
so `scripts/install.sh --user` (PREFIX=`~/.local`) is found without relying on a
stale system copy under `/usr/local/share`. Ainiux then applies optional user
files below `~/.config/ainiux/` (or `$XDG_CONFIG_HOME/ainiux/`) and finally CLI
options. There is deliberately no `/etc/xdg` layer: this experimental,
self-managed application should not leave stale administrator-owned copies of
its evolving defaults in the active path. Install and uninstall scripts remove a
legacy `/etc/xdg/ainiux` directory when present.

`src/config/` owns the dependency-free parser, schema mapper, and automatic layer loader. It reads regular files with a 1 MiB default cap and produces an owned map keyed by fully qualified setting name. Boolean, signed 64-bit integer, finite float, quoted string, and bare string values remain typed, and every entry retains its source path and byte-based line/column location. Parsing validates UTF-8 and rejects duplicate keys or malformed syntax without returning a partially populated document.

Each document is schema-validated into a temporary `cli::Options` copy before it replaces the effective options, preventing partial application. The ordinary CLI parser then runs over that configured base so command-line values remain authoritative. TUI theme and thinking-trace visibility are ordinary effective options; provider credentials remain references resolved later by the provider layer.

`--no-config` disables the automatic user files while retaining installed defaults. Arbitrary `--config PATH` layering is intentionally omitted until there is a concrete need. `--debug` reports considered configuration paths and their loaded, missing, skipped, or failed state on `stderr`; it never prints configuration values.

Benchmark judge instructions are a separate runtime configuration document, `benchmarks.conf`, rather than ordinary application defaults or C++ literals. The bundled file (or `AINIUX_BENCHMARKS` override) is applied first, followed by the user file; individual prompt keys merge so either prompt can be overridden independently. `--no-config` skips the user layer. The specialized schema accepts `config_version`, `grading.system_prompt`, and `grading.case_prompt`, retains multiline bytes, and requires the case placeholder exactly once. Missing effective prompts do not disable ordinary modes, but `--grade` reports an actionable configuration error.

Model capabilities and purpose presets are likewise isolated in `models.conf` instead of growing ordinary application configuration or provider conditionals. The bundled source is embedded at build time so an uninstalled binary retains its catalog outside the repository; an available development/installed copy may replace that embedded base before the user layer is applied. Model records merge by `id`, with their ordered reasoning choices stored compactly as `value = none|low|...` and optional documented context fallback as `context_window`; presets merge by `(model_id,purpose)`, and `enabled = off` removes an earlier identity. Explicit context overrides remain authoritative, followed by usable `/models` metadata and then the catalog fallback. Model regular expressions and closed protocol names are validated at load time. A model expression is evaluated case-insensitively against only the final slash-separated component; arbitrary routing prefixes never participate. Bundled family records are provider-neutral, while the optional provider/API scopes and priority remain available for more specific user overrides. The catalog may select only a registered provider protocol—it cannot inject arbitrary JSON paths. Qwen 3.5/3.6 Chat stays on the boolean `qwen_chat` toggle (`chat_template_kwargs.enable_thinking`). Qwen 3.8 uses a dedicated `qwen_chat_effort` protocol because its chat template accepts `reasoning_effort` (`none|low|medium|xhigh`, default `xhigh`) in addition to the same thinking on/off switch; folding that ladder into `qwen_chat` would also break the toggle-only `Ctrl+T` shortcut.


## Provider Registry and API Adapters

v0.4 begins with a data-driven provider registry in `src/provider/`. Built-in profiles carry aliases, default base URLs, endpoint paths, key environment variables, local/remote flags, optional dummy keys, compatibility warnings, and client capability flags. The OpenAI-compatible providers share the same Chat Completions adapter instead of duplicating provider-specific request code.

Responses API support is a sibling adapter. Official `--provider openai` defaults to Responses; `--api chat`, `openai_chat`, user `api = chat`, and custom URLs stay on Chat Completions. `--api responses`, `--responses`, and `openai_responses` still select Responses explicitly. The adapter reuses the existing HTTP transport, cancellation token, timing, redaction, and streaming callback path. It maps Responses output text and SSE text deltas into the same internal assistant message model used by Chat Completions, accepts user `input_image` parts, and can attach catalog-selected hosted `web_search` tools. Files, server-side conversations, and other hosted tools remain disabled until implemented.

Hosted web search is a catalog capability (`web_search = on`, optional `web_search_name`), not another client search provider. When the matched model can emit the family's hosted shape, that tool takes precedence over Tavily/Firecrawl/Exa/Searxng/DuckDuckGo even if those are configured. Agent mode then omits the client `web_search` function. `--no-builtin-web-search` and user `web_search.builtin = off` restore the client path. Kimi `$web_search` is a builtin-function echo: the client returns the model's arguments unchanged. GLM uses a request-level `{type:web_search, web_search:{enable:true}}` wrapper. Claude hosted names are sent on the existing OpenAI-compat Chat adapter. Official Gemini OpenAI-compat Chat rejects `type=google_search`, so that family stays on client search; native Gemini generateContent/Interactions and native Anthropic Messages remain unimplemented.

The `none` provider profile represents an explicit model-offline state. It has no base URL, endpoint paths, credentials, or model capabilities. Provider transport entry points reject model listing and chat before constructing an HTTP request, and endpoint overrides are invalid with this profile. This lets editor, conversion, explicit URL fetching, and local REPL/TUI commands run without inventing a dummy OpenAI-compatible endpoint. URL fetching remains a separate explicit network operation governed by its own safety policy.

Reasoning uses one owned `ReasoningSelection`: Auto, a named value, or an exact token budget. CLI, chat, editor, and the headless security-review request context share its parser, display, catalog resolution, and provider serialization. Auto omits the field; named and numeric values are never approximately converted into each other. Model changes reset the selection to Auto, while thread/editor restoration applies the persisted complete selection. Chat stores it per thread in `settings_json`; editor app state stores provider/model/API/reasoning and is written by a runtime job. Security review does not persist a selection or session.

## Native tools and the headless security-review boundary

The first agent-like runtime is deliberately a single explicit non-interactive mode, `--security-review`, rather than the future general agent surface. Provider-neutral function definitions/calls/results live beside provider request types. Chat Completions replays native assistant `tool_calls`, tool-role results, and opaque reasoning fields; Responses replays original output items and `function_call_output` by `call_id`. This keeps protocol state in the provider adapter and avoids a textual pseudo-tool format for security-review itself. A shared argument pipeline in `src/agent/tool_args.*` now normalizes tool argument text for the read registry (empty → `{}`, Markdown fence strip, strict JSON, single-object extraction, one-pass repair, schema coercion, exact/case/snake-camel name repair) and exposes an XML-alike channel parser for the future general agent loop; security-review still prefers native `tool_calls` only.

`src/agent/agent_loop.*` is the reusable v1.0 agent turn engine. `src/agent/session_runtime.*` owns a prepared multi-turn workspace session (index refresh, mutation tools, `agent.sqlite`, AGENTS.md, tool conversation). Headless one-shot entry is `ainiux run` / `--run` / `-r` via `src/app/agent_mode.cpp` (`run_agent_goal` → one prepared turn); interactive entry is `ainiux agent` / `--agent` / `-a` via `InteractiveMode::Agent` reusing the same runtime across user messages. It owns history hygiene (invalid tool `arguments` are stored locally for error results but rewritten to `"{}"` in provider continuation items; dangling call ids get synthetic `cancelled` results), separate transport retry budgets (3 attempts with 1s/2s/4s backoff; immediate fail on 400/401/403/404 and deterministic schema/auth errors; never auto re-run tools), loop limits (identical-call soft notice at 3 / hard abort at 5; consecutive all-failed abort at 3 for non-identical thrashing; 50-turn scripted cap with interactive continue), and native→XML protocol downgrade after two consecutive leaked `<tool_call>` markup turns on the native channel. Mid-session protocol downgrade injects a user notice and does not rewrite the system prompt, so provider-side prompt caching stays valid.

## User-initiated shell vs agent `run`

Interactive chat, agent TUI, and REPL offer user shell for the human operator via
`src/app/user_shell.*`: `/bin/sh -c` on POSIX, or built-in Windows PowerShell 5.1
with a no-profile encoded UTF-16 command and UTF-8 output on Windows. Both use
closed stdin, bounded pipes, timeout, cancellation, and a sanitized environment:

- `/shell` / `!` → display-only `notice` (filtered from provider payloads and chat SQLite).
- `/shell-stdout` / `!!` → pure redacted stdout replaces the TUI input draft; the user must submit explicitly (Enter/Ctrl+S). Safer than auto-injecting shell output as a user message.

This is intentionally **not** the agent tool path. Agent `run`
(`src/agent/process.*`) is direct argv execution with structural argument safety
and a **denylist / Guard** for dangerous forms. POSIX uses fixed-PATH `execve`;
Windows resolves absolute executables from inherited absolute PATH entries,
filters PATHEXT, and runs safe `.bat`/`.cmd` files only through resolved
`cmd.exe`. Security-review keeps a strict inspection allowlist. Agent mode
deliberately does **not** grow per-command option allowlists—that does not scale
across platform toolchains.

## Guard Ask approvals (interactive only)

Destructive-command Guard returns Allow, Deny, or Ask. Hard Deny (shell wrappers, sudo, disk destroyers, `find -delete`) is never elevatable. Ask is for high-risk-but-sometimes-legitimate actions (`git reset --hard`, force push, recursive `rm`, database-file deletion, **creating missing parent directories** via `create_dirs`).

Interactive Agent layers a project-persisted `confirm`/`smart`/`yolo` permission policy above Guard. The registry validates a complete tool call before requesting one consolidated decision, then revalidates canonical external targets before mutation. Native exact-path tools are preferred over command equivalents because they are structured, bounded, cancellable, and easier to validate. External changes intentionally receive no project history/index entry. `run` stays fixed-PATH direct argv execution; approval never enables shell composition.

Provider account-credit display is likewise registry-driven and optional. A profile may expose one official authenticated credit URL; the provider adapter owns its response schema and normalized currency/amount result, while a cancellable runtime job delivers that result to the TUI event queue. The UI stores only the formatted in-memory label, silently omits unavailable balances, and never persists raw billing responses.

- Headless `run` maps Ask → Deny (no self-approval).
- Interactive agent blocks the tool worker on an `ApprovalGate`; the TUI shows a **y/n** panel (not Enter). Decisions are one-shot. Outcomes are written to `.ainiux-pr/agent.sqlite` `approvals` and a short transcript `notice`.
- Agent git policy is broader than security-review so a user-approved Ask can actually run (still no shell, still common path/safety checks).

## Workspace path containment for agent writes

Agent file tools only accept **project-relative** paths. On POSIX, `~/…` is *not*
absolute—it is a relative component—so rejecting only
`fs::path::is_absolute()` is insufficient: `~/code/x` would otherwise create
`$workspace/~/code/x`. Paths with `~`, `~user`, `$…`, `..`, protected metadata
dirs, or absolutes are rejected. Identity-based containment anchors a new target
to its canonical existing parent and verifies existing targets by filesystem
identity. Windows additionally applies its lexical path rules and rejects any
link/reparse component in sensitive operations. `mkdir` never
replaces existing non-directories and never deletes trees.

## Chat vs interactive agent (shared shell, separate modes)

`--chat` / `-c` and `--agent` / `-a` are **separate product modes**. Ordinary chat must never silently gain workspace tools, shell, or agent system prompts. Interactive agent must never be entered merely by opening chat.

They **may and should share** the full-screen terminal shell and selector infrastructure:

- history panel, status line, bottom multiline input (`EditorState`)
- `/provider`, `/model`, `/reasoning` (and related) pickers in `src/ui/` and `src/tui/`
- themes, layout, job/event loop, cancel/regenerate UX patterns

Shared code lives under `src/tui/` and `src/ui/`; mode identity is `InteractiveMode::{Chat,Agent}` plus `options.agent` for generation. Generation branches: Chat uses `provider::send_chat_messages`; Agent uses `AgentSessionRuntime`. Mode cycling is an explicit handoff via `InteractiveUiTarget`.

Reusable agent helpers live under **`scripts/ainiux/`** as ordinary workspace files (indexed, greppable, git-visible). The earlier private `.ainiux-pr/scripts/` store was removed: it hid helpers from discovery, blocked ordinary path tools, and models preferred `python3 -c` instead of reuse. Confirm/Smart now deny long or wrapping `python3 -c` and `nohup`; Act `run` can set `background=true` for servers. Confirm does not paste script bytes into the Guard panel (path, args, size, short hash only); the panel is a top-aligned overlay so the start of the request stays on screen and can be scrolled. Script-hash trust is project-local, not session-only.

**Agent is project-centric:** one workspace tree, one project state dir **`.ainiux-pr/`** (parent/nested project markers refused). User profile remains **`~/.ainiux/`** (chat library, media) and is never treated as a project root. Durable agent/index state is only under `.ainiux-pr/` (`agent.sqlite` schema v2 singleton thread, `history/` with one backup slot per path, size/TTL config, logs). Auto-compact uses estimated request tokens vs context window × `agent.compact_limit`; an unset limit resolves to 75% for every known context-window size. `agent.auto_compact=false` disables it. Agent tool rounds default to 250 per user turn and are configurable up to 500 through `agent.max_turns`; interactive cap continuation and headless termination remain distinct. `max_parallel_agents` is reserved for security-review workers until sub-agent semantics are specified. Compact tool lines (`N: name(args) → ok|error`) are preferred for TUI and `--run` stderr.

Trusted prompts: agent sessions use the bounded `resources/prompts/agent_prompt.md` plus a static native/XML appendix. The initial Act/Plan state is a separate Ainiux control message; later switches append controls and, only when changed, refreshed framed root `AGENTS.md` instructions. They never rewrite the earlier model-visible prefix. Compaction rebuilds the stable base with only the active mode. Native Act and Plan requests advertise the same ordered tool superset, while `MutationPolicy` enforces mode authority. `master_prompt.md` and `security_prompt.md` are retained exclusively for security review and still compose with the historical exact `master + "\n" + security` byte sequence. Security-review keeps its own bounded finalization loop and retry helper so its acceptance behavior stays stable. Diagnostic JSONL logging is shared (`ReviewLogger`) with a run-kind parameter so security-review and agent logs live under separate directories with the same live-flush / finalize semantics.

Agent tool authorization uses `MutationPolicy::{Disabled,PlanningDocuments,Full}`. `run` has three distinct policies: security-review retains its narrow index-snapshot inspection allowlist, Plan permits only complete argv forms accepted by a conservative built-in read-only classifier, and Act retains the broad Guard-controlled direct runner. Smart reuses the Plan classifier solely as an approval exemption after Guard and canonical command-aware path validation; Confirm still asks for every executable command and Yolo is unchanged. Unknown commands/options therefore ask in Act/Smart but cannot be elevated in Plan. Redirects, substitutions, pipes, and other shell syntax remain structural errors rather than inputs to a partial shell/output-path parser. Plan retains configured network tools and preflights every edit/patch destination before mutation. Its only writable destinations are root `PLANS.md`, `PLAN.md`, `TODO.md`, `AGENTS.md`, and case-sensitive `.md` files under an existing `docs/plans/` tree.

Review workers receive an explicit per-batch `EXPECTED_COVERAGE` array and normally terminate with a schema-defined `submit_security_review` call. The loop has a bounded finalization phase: it reminds an over-exploring model after round 12, exposes only submission from round 16, and stops after round 20 or 64 calls. Free-form final content is retained for compatible endpoints, but normalization only extracts one already-valid JSON object from common preamble/fence framing; it never repairs syntax and rejects multiple objects.

Security-review diagnostic logs append each JSONL event to a live `*.jsonl.partial` path and flush after every record so operators can `tail -f` progress. The path is printed on `stderr` at start; graceful completion renames the file to the final `*.jsonl` name.

Security-review read tools are views over narrow index snapshot records, not SQLite handles. Index file/symbol rows remain hints; review source ranges are accepted only after reading the real file once and matching its indexed hash. The virtual snapshot tree is the security-review authorization list, so ignored, unsupported, generated, VCS, state, traversal, and symlink paths do not become review-readable. Agent mode keeps index/search/symbol tools on that snapshot but routes exact-path `read` calls, including `items` batches, through the same validated live-filesystem layer as its native mutation tools, allowing safe unindexed project files without broadening protected metadata or symlink access. A shared native process runner exists because inspection command lifecycle, process trees, bounded pipes, cancellation, and Git hardening are reusable concerns; the security-review policy is a conservative read-only allowlist and never uses `system`, `popen`, or an unrestricted shell.

The security-review command runner post-filters path-listing/search stdout against its index snapshot. Agent Act/Plan does not apply that output filter: after command and canonical path validation, stdout represents the live project filesystem in every interactive permission mode. Besides being the intended live-filesystem behavior, this avoids incorrectly treating formatted rows such as `ls -l` output as literal filenames.

Review batching is based on raw indexed file size and deterministic relative-path order. Files larger than one batch are one logical job with sequential UTF-8-safe chunks; jobs, not chunks, consume the bounded parallel-worker slots. A finding still requires a snapshot-valid path/range plus a title or impact description. Missing or empty presentation and assessment metadata is conservatively normalized so a minor schema omission does not discard the batch before the final single coordinator sees normalized stable finding IDs and coverage. Markdown is rendered locally from validated paths/ranges and freshly verified evidence. The design intentionally creates no `agent.sqlite`, session log, write/edit tool, or approval state.

All wire construction remains inside registered protocols in `src/provider/`. The catalog chooses a protocol and advertised selector values. A syntactically valid direct value remains forward-compatible, but a matching family with a nonmatching `value` list produces a reusable advisory: non-interactive CLI warns and proceeds, while REPL/chat/editor commands require explicit confirmation. A family with no advertised values does not invent a closed list. Temperature support is metadata, not a serialization gate for explicit values: automatic presets skip known-incompatible temperature fields, while an explicit override is sent with a warning. Live capability probing, native Anthropic Messages, and preservation of signed/opaque provider reasoning output for future tool loops remain deferred.


## JSON Chat Persistence

v0.2 stores explicit chat files via `--save-chat PATH` and `--load-chat PATH` before adding automatic XDG chat IDs. This keeps the early REPL scriptable and reviewable while still using the target schema fields: `schema_version`, timestamps, provider, base URL, model, settings, messages, attachments, usage, and compaction events. Saves use a private temporary sibling, durable flush, and atomic replacement: POSIX mode/fsync/rename or a protected Windows DACL with `FlushFileBuffers` and write-through replacement.

## SQLite Chat Persistence

The automatic local TUI chat library uses `libsqlite3` and stores its database at `~/.ainiux/ainiux.db`. SQLite is available on the target POSIX-like systems and UCRT64 and keeps the dependency smaller than adding a bespoke storage engine. The schema stores one row per message rather than one JSON transcript blob so thread listing, transcript replay, regeneration, attachments, usage records, and compaction events have stable identifiers and indexes. WAL mode is enabled for the local database, with short transactions and a short busy timeout so indexed `/list` queries can run synchronously without waiting indefinitely on a lock. Provider, base URL, and model fields are restored as one authoritative thread context: empty persisted fields clear prior in-memory values instead of inheriting them. An incomplete provider/model pair is treated as recoverable setup state rather than a valid request configuration; the TUI labels it, requires provider-then-model selection, and prevents sends until repaired.

## Runtime Jobs

v0.3 introduces `src/runtime/` with a small cancellation token, thread-safe event queue, and RAII `JobHandle`. The first users are the full-screen TUI foundation and cancellable provider requests. HTTP requests carry a cancellation token down to libcurl and abort through `CURLOPT_XFERINFOFUNCTION`, returning `AINIUX_ERR_CANCELLED` instead of a generic transport error.

The initial model is one worker thread per active job. It is intentionally small: the owning UI loop receives events and remains the only code that mutates terminal/session state. This can grow into a queue or pooled runtime when TUI/web workloads need it.

## Background agent across editor hops

Interactive agent turns already ran on a worker thread, but product-surface ownership was exclusive: mode cycle required an idle job, and leaving agent finished the project session. That blocked the natural workflow of reviewing dirty files in the editor/dired while a long turn continues.

`AgentController` (`src/agent/agent_controller.*`) is owned by `InteractiveSession` and holds the prepared `AgentSessionRuntime`, Guard `ApprovalGate`, turn `JobHandle`, and a surface-neutral event queue. Temporary hops to the editor (`Ctrl+G` / `/cycle` / `/editor`) detach the agent TUI without cancelling the turn or calling `finish_session`. Returning reattaches and reloads the durable display transcript when needed. Guard Ask is answerable from the editor so a detached worker cannot stall forever. Switching to chat or process quit still shuts the controller down. Dual concurrent editor-assist + agent streams remain a follow-up; file-lock awareness for agent mutations is planned as a safety hardening pass.

## Theme registry ownership (not themes.conf install)

`themes.conf` is installed with the other share configs and loaded by `config::load_automatic` (bundled + user layers), with `default_theme_registry()` as the seed and fallback when no file is found. The recurring “no colors / no dired changed-line tint after mode hop” bug was **not** missing install or failed load: `EditorSettings::themes` is a **non-owning** `const ThemeRegistry*` that defaults to null. Agent→editor hops updated surface flags without always rebinding that pointer to the session-owned `Options::tui_themes`, so paint saw `themes == nullptr` and skipped styled backgrounds even when history-diff marks were correct.

Hardening: (1) `rebind_editor_theme_settings()` on every interactive editor entry; (2) TUI leave-to-editor rebinds themes to the session registry after context assignment; (3) paint uses `tui::resolve_theme_registry()` which never returns empty—prefer settings/options, else process-lifetime `builtin_theme_registry()`. Do not store theme data only behind a nullable pointer without a paint-time fallback.

## Full-Screen TUI Foundation

v0.3 adds `--chat` as an alternate-screen terminal UI, with `--tui` retained as a compatibility alias without adding an ncurses dependency yet. The TUI renders chat history, a status line, and a bounded bottom input panel without reserving persistent header rows for endpoint/model details. The input panel embeds `EditorState`, using the editor's rectangular renderer, soft wrap, visual-row cursor movement, undo/redo stack, page movement, and exact substring search so long multiline prompts behave like the standalone editor without taking over the whole screen. Chat requests, model listing, and chat save/load run as runtime jobs so terminal input remains responsive. A bare `Esc` cancels the active model request, while `Ctrl+R` triggers regeneration (resending the last user prompt) by cancelling any active chat request from the owning event loop. `Ctrl+U` is undo and `Ctrl+Y` is redo. TUI colors use semantic roles and 24-bit ANSI SGR sequences so contrast is controlled instead of depending on terminal palette mappings. The `dark` theme is the default; `/theme off|dark|light|sepia` changes the process-wide runtime color state, and `--nocolors` disables color styling while preserving terminal control sequences. `off` is reserved and cannot be a custom theme name. Theme foreground/background pairs are unit-tested against the WCAG 2.1 AA 4.5:1 contrast target, including the subdued tinted role used for visible thinking traces.

Full-screen chat, agent, and editor sessions enable xterm button-event tracking plus SGR mouse coordinates, with legacy X10 decoding as a fallback. This gives deterministic wheel events instead of relying on alternate-screen arrow-key translation: each wheel report moves one rendered visual row in the content region under the pointer. The tradeoff is that native terminal selection may require `Shift+drag` in terminals that reserve unmodified mouse input for applications. RAII terminal restoration disables both reporting modes on every normal mode switch and exit path.

Provider/model picker data and labels live in `src/ui/provider_model_selector.*`, layered on the generic `ui::TextSelector`; they are not owned by chat or editor. Chat threads, editor buffers, providers, and models all feed that text-selector representation through the same semantic panel styling. A provider change invalidates the previous model id, then starts the existing cancellable `/models` job. Exactly one returned model is selected automatically; multiple results stay explicit and open the selector. This keeps provider-specific model ids from leaking across provider changes and makes the transition consistent across chat and editor.


## Shared syntax highlighting engine

Syntax highlighting lives in `src/highlight/` and produces byte-based semantic spans without terminal escapes. `text` remains the safe fallback for scratch buffers, unknown extensions, and manual opt-out. The dependency-free engine supports Markdown, Python, C, C++, C#, Java, JavaScript/JSX, TypeScript/TSX, HTML, HTML-only, CSS, XML, JSON, Bash, PHP, Perl, Ruby, Rust, Go, PowerShell, Assembly, SQL, TOML, YAML, and INI. The canonical `html` mode composes the HTML, JavaScript, and CSS lexers for script/style element bodies and inline event/style attributes; `.html`/`.htm` select it automatically. `htmlonly` preserves the markup-focused implementation and is selected for XML-oriented `.xhtml` files. The former `html-multi` and `htmlmulti` names remain compatibility aliases for `html`. The engine keeps explicit line state for fenced blocks, continued HTML tags, comments, strings, heredocs, CDATA, PHP/Perl/Ruby heredocs, Rust raw strings and nested comments, Go raw strings, PowerShell here-strings, SQL dollar-quoted strings, TOML multiline strings, YAML block scalars, and embedded script/style content. It resolves nested-language tokens over fallback spans, caps pathological lines, and provides an incremental document cache invalidated from before the changed line so multiline state remains correct.

Editor rendering now carries syntax and selection as independent structured overlays. Terminal adapters apply semantic theme colors and reverse video at the final output boundary, so wrapping, grapheme/cell-width calculations, invalid UTF-8 replacement, selection, and `--nocolors` do not depend on embedded ANSI bytes. Chat history consumes the same Markdown spans; labels, thinking traces, and activity indicators remain higher-priority UI styles. No external dependency was added, and regular expressions are compiled once with `std::regex::optimize`.

The UI exposes `/highlight` in editor/chat and per-buffer `/mode MODE|auto` in the editor. The process-wide toggle is shared across editor/chat switches but is not persisted. Automatic mode follows filename endings, while manual mode survives buffer switches and save-as. Recognized Markdown fence tags delegate to the same language engine; unknown tags stay plain.

## Standalone Editor Foundation

`--editor` is a permanent bonus mode and a controlled test bed for the multiline editing layer now embedded in the chat TUI. The editor core uses a piece table: the original file and appended edit buffer are kept separately while visible text is represented by pieces. This keeps inserts and deletes local to the piece list instead of rewriting the whole buffer on every keystroke, which is a better fit for large files than a single mutable string.

Rendering is split from terminal I/O. `EditorState` renders into a caller-provided `Rect`, so one terminal window can eventually host multiple editor panels or embed the editor in a partial-screen chat layout. Long lines soft-wrap inside that rectangle, preferring whitespace breakpoints and hard-wrapping long words. Vertical movement has two modes: logical hard-line movement for file editing, and visual-row movement that treats wrapped overflow rows as cursor targets for TUI chat input. The terminal harness uses the shared RAII POSIX-termios/Win32-console backend and ANSI/VT escape sequences; the core renderer remains independent of terminal ownership and no ncurses dependency is added.

The standalone editor reserves the bottom two terminal rows for editor-owned UI: a reverse-video status line and a one-line minibuffer. The main editing screen uses all remaining rows. The status line reports path, dirty state, the compact syntax language and line-ending mode as `(mode LF)`, `(mode CRLF)`, or `(mode CR)`, and cursor position; it omits redundant editor and automatic/manual labels to preserve space on 80-column terminals. The minibuffer handles prompts such as save path, load path, huge-file load confirmation, exact substring search, exact substring replacement, overwrite confirmation, scratch-buffer save-on-quit, and unsaved-exit confirmation. Replacement mode is deliberately modal after the search/replacement prompts: each match is replaced, skipped, or all remaining matches are replaced to the end of the buffer. The editor keeps a bounded undo/redo history with a configurable default depth of five entries. Editor file loads are unlimited by default except by address space and available memory, but config can set a hard byte limit and a separate warning threshold. Document `Tab` first queries the document word-completion domain and falls back to indentation; selected blocks always indent. Command and path completion remain isolated in the command minibuffer, while chat-TUI completion keeps its existing context rules.

Editor buffers use LF as their only internal line-boundary representation. File loading detects uniform LF, CR, or CRLF input while normalizing boundaries; file save and auto-save stream the internal LF boundaries using the buffer's detected or explicitly selected output style. This keeps piece-table line indexing and editor operations independent from external encodings, preserves the presence or absence of a final line ending, and makes mixed endings explicit: they warn and use the configured default instead of silently choosing the first ending encountered. Tab width, tab style, and output line ending are buffer-owned settings initialized from config defaults.

File loading also performs a bounded indentation probe over the first 20 physical lines. It compares nonblank leading-space depths and accepts a width only when one candidate explains at least two thirds of observed depth changes without an equally strong candidate; a single clean block transition is sufficient. Consistently space- or tab-indented samples select that style, while mixed prefixes/styles, conflicting steps, unindented input, and one-line input retain configuration fallbacks. Detection happens for normal opens and autosave recovery before the text moves into the piece table, does not scan the whole file, and remains a per-buffer initial value that `/tab-width` or `/tab-style` can override.

Document completion keeps ordered exact and full-case-folded maps with occurrence counts in each open buffer. Prefix lookup starts with `lower_bound`; ordinary edits remove and re-index only the token window joined or split by the edit, while bulk/stream replacements invalidate and rebuild lazily. The index cache uses documented copy-on-write shared ownership because the editor loop temporarily copies the active `EditorState` when handing it to and from the open-buffer list; this keeps those handoffs cheap while edits detach before mutating an index. Unicode letters, numbers, marks, uppercase properties, and full default case folding come from generated, checked-in Unicode 15.1 UCD tables, so completion has no locale or runtime Unicode-library dependency. Invalid UTF-8 bytes remain buffer data but delimit indexed words. Completion candidates and cycling state are separate from command, path, AI-command, and chat completion, and cycling reuses the first replacement's undo record.

Language indentation reformatting lives in `src/editor/reformat.*` and is deliberately narrower than a source formatter: it replaces leading whitespace only. It reuses highlighter spans and multiline lexical state to ignore structural tokens in comments, strings, heredocs, fences, and scalars. Conservative profiles cover brace, keyword/end, markup, SQL, indentation-topology, and assembly-label languages. Formatting is a cancellable runtime job over immutable input. Each editor buffer has a stable process-local identity and monotonic text revision; the UI applies a worker result only when identity, revision, language, tab width, and tab style still match, then records the range replacement as one undo operation. The event queue outlives and is destroyed after its worker handle, preventing callbacks into released queue state during cancellation or shutdown.

v0.76 adds byte-offset selection with Shift+movement keys, reverse-video highlighting, and internal copy/cut/paste shared between standalone editor mode and the chat TUI input. Bracketed paste is enabled so terminal paste events can be distinguished from typed input.

The later external clipboard bridge keeps that process-wide buffer authoritative and preserves internal-first paste behavior. Native integration is shell-free and dependency-free at link time: fixed-argument `pbcopy`/`pbpaste`, `wl-copy`/`wl-paste`, `xclip`/`xsel`, Termux, and WSL helpers are resolved only from absolute `PATH` entries and communicate only through pipes. Helpers run in cancellable runtime jobs with a two-second deadline; child processes and descriptors are owned and reaped on every exit path. Reads are UTF-8 text-only and limited to 16 MiB. OSC 52 remains the terminal and remote transport; SSH paste queries the terminal first, while local sessions prefer native readers. Clipboard queries remain permission- and terminal-dependent, consistent with [kitty's clipboard controls](https://sw.kovidgoyal.net/kitty/conf/#opt-kitty.clipboard_control) and [tmux's remote clipboard limitations](https://github.com/tmux/tmux/wiki/Clipboard). A delayed read is one undoable edit only when its captured buffer identity, revision, cursor, selection, and UI mode still match. External reads do not refill the internal clipboard, so an empty-buffer paste reflects the current desktop selection each time.

v0.78 adds grapheme-aware Unicode handling to the standalone editor and shared editor core. Navigation, backspace/delete, display-column math, rendering, and soft wrap now operate on grapheme clusters instead of raw UTF-8 bytes. Terminal cell width uses a simplified East Asian Wide and emoji range table with zero-width combining marks, variation selectors, skin-tone modifiers, and ZWJ sequences. Invalid UTF-8 is replaced with a visible placeholder during render. Full UAX #29 conformance and `utf8proc`/`libgrapheme` integration remain future work.

v0.77 adds the first editor AI feature: continue/auto-write on `Ctrl+Space`. Cursor-aware prose continuation now gives text and Markdown independent 16,384/4,096-character prefix/postfix limits and byte-length-delimited gap framing. Middle insertions explicitly bridge into immutable suffix text, while end insertions prohibit recap/restart behavior. A prose-specific streaming filter accepts raw text or an optional `<content>` wrapper without trimming body bytes. Every structured syntax mode retains its original language-aware code-gap framing, prompts, filter, and 4,000/2,000 limits. All context is sliced by UTF-8 characters (invalid bytes are preserved as one unit), and zero disables a context side. Continue runs on cancellable runtime jobs with one undo record; cancellation keeps partial output. SSE completion is detected through `finish_reason` and `[DONE]`, then the HTTP stream is aborted so the editor can return to idle without a keypress. Current startup policy is provider-agnostic: chat and editor start cancellable `/models` discovery whenever an online provider is explicit and `--model` is absent, auto-select exactly one result, and open the shared selector for multiple results. A bare offline chat or editor performs no discovery and opens no setup picker; editor remains fully usable locally, while chat permits thread browsing but blocks sending until provider and model are configured.

v0.79 unifies editor AI assist modes around four scoped behaviors: `selection` and `all` replace text in-place; `continue` streams after the cursor using tail-before-cursor context; `insert` streams after the cursor using the current selection as input. The older start-to-cursor `insert` mode was removed in favor of `continue` semantics. Config token `local_insert` remains an alias for `insert`. Built-in `/spell`, `/grammar`, `/continue`, `/fact`, `/comment`, `/rewrite`, `/English`, `/Chinese`, and `/Finnish` expose all four scoped modes by default, and `Ctrl+Space` uses `continue`.


## Charset conversion

`src/encoding/` converts inbound text to UTF-8 without linking ICU or libiconv. Small single-byte sets (Windows-1250/1251/1252, ISO-8859-1/2, KOI8-R/U) and UTF-16 live as in-tree tables. CJK names are allowlisted and converted by a shell-free `iconv` child on POSIX or `MultiByteToWideChar` on Windows. Fetch uses declared Content-Type / HTML meta charsets; HTML `iso-8859-1` follows the WHATWG Windows-1252 map. Headless `--input`/`--attach` do not guess unlabeled 8-bit files; `--encoding` or the editor picker is required. Converted editor buffers save as UTF-8.

## Document Extraction Modules

v0.5 starts document extraction with a separate `src/html/` module. The HTML converter is intentionally small, uses C++17 `std::regex` for simple tag and attribute matching, and converts easy page content to plaintext or Markdown without adding another parser dependency. Script/style/noscript removal uses a linear scanner instead of a broad regular expression so large real pages do not trigger catastrophic regex recursion. The first slice handles headings, bold/strong, emphasis/italic, links, line breaks, and simple block spacing. It tolerates common malformed HTML by ignoring unknown or broken tags while preserving text. Input is decoded to UTF-8 at the extraction boundary by `src/encoding/`: built-in maps cover UTF-16 and the small European/Cyrillic sets, declared HTML/HTTP charsets drive fetch conversion, and unlabeled 8-bit local files fail or ask rather than guessing. CJK names go through an allowlisted `iconv` subprocess (Windows: `MultiByteToWideChar`) so the tree does not vendor large mapping tables or link ICU. It does not execute JavaScript or attempt to be a browser-grade HTML parser.

Future PDF and Word extraction should live in separate modules such as `src/pdf/` and `src/word/` rather than growing the HTML module.

## Markdown Output Rendering

Assistant output rendering lives in a separate `src/markdown/` module so provider code, HTML extraction, and UI code do not grow Markdown-specific branches. The first writer is intentionally line-oriented and dependency-free: it supports ATX headings, paragraphs, fenced and indented code blocks, bold, italic, `++underline++`, Markdown links, ordered/unordered nested lists, simple pipe tables, raw HTML blocks, HTML fragments, and plaintext stripping. `--output-format md` preserves the existing raw Markdown streaming path. `--output-format html` and `--output-format plaintext` render after the full assistant reply is available because a correct fragment/document needs complete block context. HTML written to `stdout` is a fragment; HTML written through `--output PATH` is wrapped as a complete document with doctype, charset, viewport, head, and body.

## Editor text alignment and line cleanup

The standalone editor provides local, offline text transforms for reflow and line hygiene: `/left-align`, `/right-align`, `/center-align`, `/justify`, `/remove-blank-lines`, `/remove-duplicate-blank-lines`, and `/remove-duplicate-lines`. They apply to the selection when one exists (expanded to complete physical lines), otherwise the whole buffer, and each successful edit is one undo step. Alignment WIDTH must be greater than 20 and at most 1000; any whitespace-delimited word wider than WIDTH fails without mutating the buffer. Blank lines separate paragraphs during reflow. Omitting WIDTH prompts in the minibuffer with the configured default prefilled (`Enter width for the text-alignment (N default):`). The default lives in `[editor] alignment-width` (78) and can be changed for the session with `/alignment-width`. Layout logic lives in `src/editor/text_layout.*` so indentation reformat stays separate.

## Chat/agent history display alignment

Chat and interactive agent TUI history deliberately **omit** `/width` and left/right/center/justify prose reflow. Those commands stay **editor-only** (`src/editor/text_layout.*` / minibuffer) so chat and agent transcripts are not mangled by incomplete history layout. History display uses terminal soft wrap only; pretty Markdown tables are capped to the history content column via `pretty_format_tables(..., max_width)`. SQLite and JSON transcripts remain raw.

## Pretty tables for human display

GFM pipe tables are easy for models and generators to emit, but unpadded columns are hard to read in a terminal. `src/markdown/table_format.*` owns shared layout: column widths use a simplified UTF-8 display-cell measure, honor left/center/right alignment from separator cells, and render either Unicode box borders (`┌─┬─┐` / `│` / `└─┴─┘`) or padded GFM pipes. The default is Unicode when the process locale looks UTF-8 capable; otherwise padded GFM.

Chat and agent TUI history apply this **display-only** before Markdown highlighting. Stored transcripts and model-visible context keep the original raw GFM (or any other bytes the model streamed). While a reply is still streaming, tables with a header and separator reformat on each frame from currently known row widths so columns can grow without waiting for the full table. Fenced code blocks that contain pipes are left alone; already-boxed Unicode tables pass through unchanged so generators stay idempotent with the display path.

Structured human reports call the same formatter at the source: `/show-index` and first-run index completion (`compact_totals_markdown`), `--print-index` totals, and benchmark/grade companion Markdown tables. Streaming CLI `--output-format md` remains model-faithful and is not rewritten.

## URL Fetching First Slice

`--input PATH` is the local-file input path for `.txt`, `.md`/`.markdown`, and `.html`/`.htm`; the parser is selected from the path ending. `--fetch-url` is explicit and never triggered by URLs found inside prompt text. Used alone, either option is an extraction mode controlled by `--output-format md|html|plaintext|json|jsond|ndjson`. Combined with `-p`/`--prompt` or `--prompt-file` in non-interactive CLI mode, the converted input is inserted as a separate user-context message before the final user prompt, so saved transcripts show exactly what was sent. The older `--html-file` remains a compatibility alias for local HTML input. The CLI URL path uses the existing libcurl RAII wrapper, sends browser-style `User-Agent`, `Accept`, `Accept-Language`, and `Upgrade-Insecure-Requests` headers, captures `Content-Type`, applies a hard `max_body_bytes` cap in the HTTP write callback before appending oversized chunks, sets a fetch-mode total timeout when the user did not provide one, and refuses private/loopback/link-local/multicast/metadata literal hosts unless `--allow-private-url-fetch` is provided. Redirect following remains disabled in this slice.

The follow-up URL safety layer checks the actual IPv4/IPv6 address passed to libcurl's socket-open callback and refuses private, loopback, link-local, multicast, and metadata ranges before creating the socket. Proxy URL fetching requires `--allow-private-url-fetch` because proxy-side target DNS cannot be verified locally. The same module backs CLI `--fetch-url`, REPL `/fetch`, and cancellable TUI `/fetch`. Charset conversion now lives in `src/encoding/` (see the HTML extraction note above); allow/block domain configuration and future web integration remain follow-up work.

## Local Image Input First Slice

The first image-input slice extends `--input PATH` rather than adding a second overlapping file option. A small `src/input/` module classifies file endings case-insensitively and owns bounded binary reading, signature validation, and base64 encoding. PNG, JPEG, and GIF inputs become OpenAI-compatible Chat Completions content parts with `type: image_url` and an in-memory data URL. WebP input is intentionally disabled after compatibility testing showed that common vision endpoints do not decode it reliably. WebM is not accepted because it is a video container.

## CLI image generation (v1.2 first slice)

One-shot `ainiux image` / `--image` uses the OpenAI **Images API** (`/v1/images/generations` and `/v1/images/edits`) with `model: gpt-image-2`. The Responses `image_generation` tool is not used: that path requires a mainline chat model, extra tokens, and is aimed at multi-turn editing. JSON `images[].image_url` data URLs avoid multipart. `--format` in this mode is the image codec (`png` default), not chat `text|json|ndjson`. Output is one file (or raw stdout bytes); REPL/TUI `/image` remains later work.

Image **protocols** (HTTP conversation shapes) are compiled C++. Image **models** live in `images.conf`, analogous to `models.conf` reasoning protocols. Adding another OpenAI Images, Replicate, fal, or Gemini Interactions model is a catalog record (`api_model`, field names, `defaults_json`). `openai_images`, `replicate_predictions`, `fal_queue`, and `gemini_interactions` are implemented. `models.conf` `images = on` stays vision input and is not reused for generation. The Replicate and fal profiles are image-only (`REPLICATE_API_KEY` / `REPLICATE_API_TOKEN`, `FAL_API_KEY` / `FAL_KEY`) and do not appear in chat provider pickers. fal uses `Authorization: Key`, not Bearer. Gemini image generation reuses `--provider gemini` (`GEMINI_API_KEY`) and posts to the Interactions API with `x-goog-api-key`, not the OpenAI-compat chat base.

Image input is Chat Completions and Responses. Non-interactive repeated `--attach` remains request-local; full-screen chat `/attach` associates supported images with the originating user message so stateless providers can receive them again on later and restored turns. `/insert` is deliberately not an image alias: it is a UTF-8 text insertion command. The default limit is 20 MiB per image and can be lowered with `--max-image-bytes`. Base64 data remains temporary request state and is never stored in SQLite. TUI images are raw, SHA-256-addressed objects under `~/.ainiux/media/sha256/`; SQLite schema v3 stores media metadata and message references. This avoids base64 expansion and SQLite/WAL rewrite amplification while permitting deduplication. Writes use restrictive permissions and atomic installation; request workers verify size and hash before encoding. Conservative capability detection combines provider-registry support, optional `models.conf` `images = on|off` (text-image-to-text vs text-to-text), and recognized vision-model names; `--image-capability allow` is the explicit override for a verified compatible custom model. The DeepSeek profile advertises image parts so `deepseek-v4-flash-vision-exp` can send pixels; older DeepSeek V4 records stay `images = off`.

### Agent mode vision attachments (request-local)

Interactive agent (`ainiux agent` / `--agent`) reuses the same `/attach` chrome as chat, but image (and new text) attachments are **request-local for that user turn only**. Bytes are base64-encoded in RAM, placed on the tool-conversation user message for every model/tool round of the turn, then stripped when the turn ends. They are never written to `.ainiux-pr/agent.sqlite`, never imported into `~/.ainiux/media`, and do not survive `/compact` or session restart—the user re-attaches from disk if the model needs the image again. Durable transcript keeps only text provenance (paths / “Attached images” lines). Chat Completions and Responses capability gates (`validate_image_input`) still apply.

The agent also exposes `attach` so the model can opt into vision only when needed: load one local PNG/JPEG/GIF under the project (or an approved external path), queue it, and inject a multimodal user item after that tool round for subsequent rounds of the same turn. A per-turn cap (default 4) and `--max-image-bytes` bound context growth. Paths mentioned in free text are **not** auto-attached—that would easily overfill context with large screenshots. Models must not use Python/PIL to open images; `read` on an image returns a clear redirect to `attach` or user `/attach`.

Managed-media expiration uses the media object's last successful thread-save time. TUI `/cleanup` applies `[media] expiration_days` and protects the currently open thread; chat startup applies the longer `[media] auto_expiration_days`. Zero disables the corresponding path. Expiration tombstones the media object instead of breaking database foreign keys, marks every affected live thread read-only, then removes the raw file. A missing file discovered during thread loading applies the same read-only lock. Read-only threads remain viewable and exportable but cannot be continued or have their transcript mutated. Content-addressed objects without a message reference, such as an attachment abandoned before prompt submission, become eligible by the same age rule.

## Bounded Text Attachments

Repeatable `--attach PATH` and interactive `/attach PATH` reuse the same case-insensitive document classifier as `--input`. Each local text attachment is read incrementally with a default 1 MiB `--max-input-bytes` cap, checked for binary NUL bytes, and validated as UTF-8. Markdown is the canonical durable representation: plaintext is already valid Markdown, Markdown is retained, and HTML is converted exactly once in the attachment worker. The original path is provenance, not a replay dependency.

SQLite schema v4 adds inline attachment content. Canonical Markdown no larger than `[media] max_size_to_store_to_db` (default 65536 UTF-8 bytes) remains directly in SQLite and is not subject to expiration. Larger Markdown is stored as a SHA-256-addressed `.md` object beside image media, leaving only its digest, size, source, and display metadata in SQLite. The threshold is byte-based because its purpose is to bound database and WAL growth; a character-based threshold could consume four times as much storage for valid UTF-8. Request workers verify and hydrate every retained text attachment before context policy preparation, so compaction accounts for the actual provider text and follow-up/restart requests reuse the one-time conversion.

PDF and DOCX remain unsupported binary types. Bidirectional PDF/Markdown and DOCX/Markdown conversion is deferred rather than approximated by inserting binary data or adding an unreviewed document dependency.

Interactive `/insert FILE_OR_URL` and `/attach PATH` use separate runtime paths. `/attach` retains the provider-context/image behavior. `/insert` performs a bounded UTF-8 read for any local file ending or an explicitly requested safe HTTP(S) fetch, then sends text through the owning UI event queue for one cursor insertion. HTML-to-Markdown conversion defaults on and can be disabled to retain raw HTML. Editor insertion records the target buffer identity, revision, and cursor, and discards stale results rather than applying them to changed or closed buffers. `/fetch URL` remains the chat-history context command. Workers check cancellation and never mutate UI or chat state directly.

## Web Search First Slice

v0.88 adds a separate `src/search/` module for explicit web search. Search is never inferred from prompt text. CLI `--search QUERY` can run standalone (printing ranked results) or insert a user-context message before `-p`/`--prompt`, matching the fetch/input context pattern. REPL `/search`, TUI `/search`, and editor `Esc /search` reuse the same formatter and provider chain.

Provider selection is client-side and independent from LLM provider profiles. Configured API providers are tried when credentials or base URLs exist: Tavily, Firecrawl, Exa, and Searxng. `provider = auto` tries those first, then free **DuckDuckGo HTML** (`html.duckduckgo.com`) for ordinary top results; DuckDuckGo Instant Answer is a secondary keyless fallback for entity-style queries when the HTML SERP fails. Results are capped by `web_search.max_results` (default 3), overridable through `--max-web-search-results` or `MAXIMUM_WEB_SEARCH_RESULTS`.

The module reuses the existing libcurl HTTP wrapper. Google HTML scraping was removed: modern Google search pages return JavaScript-only shells to non-browser clients, so free Google SERP access is not reliable without a browser stack or a paid API. DuckDuckGo HTML is the supported keyless path (title, URL, snippet). Result URLs longer than 512 bytes are truncated (prefer dropping query/fragment). Agent-mode `web_search` hard-caps at 3 results so models do not fetch large SERPs. URL fetch sends a desktop Firefox User-Agent plus browser-like Accept/Sec-Fetch headers. A more reliable free search provider remains open work (see TODO.md).

Agent `fetch` always converts HTML to Markdown (or keeps `text/plain`) via `fetch_text` + `src/html/`. Raw HTML is never returned in tool results: full pages carry scripts/styles/hidden markup (prompt-injection risk) and waste tokens. CLI `--fetch-url` may still print HTML for local export. Legacy `extract_text=false` is ignored if a model still sends it. Tool `max_bytes` caps the **Markdown output** the model sees; the raw download uses a larger ceiling so HTML→MD is not aborted on bloated pages. HTTP redirects are followed (bounded) with the same private-address socket checks on each hop—trailing-slash 301s from WordPress-style hosts are common and previously failed as bare “HTTP 301”.

## Request-Only Context Policies

`--max-context-bytes N` enables a conservative text-byte budget. The `error`, `truncate-oldest`, `summarize-oldest`, `summarize-middle`, and `provider-auto` policies operate on a temporary provider request vector. Summaries are bounded deterministic extracts and do not make nested model calls. The full `Session::messages` transcript remains unchanged, while successful compactions append structured `compaction_events` containing the policy, estimated byte counts, affected-message count, timestamp, and notice. This is explicitly a byte estimate and is not presented as an exact provider token count.

## JSONL Benchmark Datasets

Benchmark input starts with JSONL because it is streamable, diffable, Unicode-safe, easy to generate without an added dependency, and maps cleanly onto single- and multi-turn cases. `src/benchmark/` owns strict schema validation and bounded loading. The authoritative 133-case corpus remains the ordinary file `benchmarks/builtin.jsonl`; the Makefile converts each record into a bounded C++ raw string literal, and the loader reconstructs the JSONL when `builtin` is selected so the dataset remains available after moving or installing the executable without requiring one compiler-sized corpus literal. Every built-in or custom case must include a reference answer or assessment criteria, while category-specific and deterministic scorer validation remains in force. Safety classification and expected action are kept separate: clear `harmful` and `harmless` cases imply reject and answer, while `sensitive` marks a policy-boundary case whose answer/reject expectation must be selected and justified explicitly by its rubric. This avoids using the subjective label `controversial` and avoids treating all sexual, hateful, or insulting content as one uniform decision. The original JSONL and the opt-in long-context dataset are installed as data files for inspection and extension.

Benchmark result output is JSONL. One record is emitted for each measured turn and a final summary reports token estimates, provider usage, timing/throughput aggregates, nearest-rank percentiles, scoring, and completed, failed, and cancelled case runs. File-backed output is closed and parsed line-by-line into a same-basename Markdown companion containing the same summary and result information; stdout-only execution does not create a report. This keeps JSONL authoritative and avoids retaining an unbounded speed-run result set in memory. Warmups use the same execution path but are not emitted or counted as measurements. Multi-turn history includes actual prior assistant output. A bounded worker pool supports concurrent finite runs and duration-driven speed load. Speed is exclusive because combining a timed load test with evaluation labels would make run counts ambiguous; quality and refusal labels may be combined and share one model response per case.

`Ctrl+C` is handled by an async-signal-safe flag. A normal monitor thread converts that flag into the shared runtime cancellation token, so active libcurl calls use the existing cancellation path and all worker/timer threads are joined before exit. Human summaries remain on `stderr`, with table and CSV renderers sharing the same aggregate rows so JSONL `stdout` stays pipeline-safe. Optional dataset `expect` hooks support only byte-deterministic exact and substring checks after thinking-trace removal. Regex refusal/reasoning checks remain deferred until matching and false-positive semantics are specified; Parquet/Hugging Face Datasets input also remains deferred.

`--grade` is deliberately a second pass over benchmark result JSONL. Source records are grouped by case id and measured run, turns are ordered, and one judge call receives the complete transcript plus a structured evaluation basis. Reference-only cases use a structured `reference_answer_semantic_agreement` evaluation item, avoiding a hidden prose instruction in the executable. C++ serializes the payload and replaces the configured placeholder; it does not append a fallback rubric or response instruction. Judge output must be exactly one validated JSON object with a bounded integer score, enumerated verdicts, a rationale, and unique complete criterion findings. HTTP/schema failures produce per-run error records and do not stop the worker pool; the final exit is nonzero if any selected grade failed. File output uses collision-safe JSONL and same-basename Markdown reports. This provides auditability, not an assertion that judge-model scores are objective. Custom prompt overrides are trusted configuration and can weaken the untrusted-data boundary, so the bundled prompt explicitly treats benchmark transcripts as data.

## Application Module Split (v0.84)

v0.84 splits the largest application sources into focused directories so CLI, editor, and TUI code can evolve independently while sharing the same provider, runtime, and chat layers:

- `src/app/` owns non-interactive CLI orchestration formerly in `main.cpp` (`exit_codes`, `output`, `document_mode`, `benchmark_mode`, `grade_mode`, `chat_session`, `repl_mode`, `config_diagnostics`). `src/main.cpp` remains a thin entry point.
- `src/editor/` owns the standalone `--editor` mode (`piece_table`, `editor_state`, `render`, `file_io`, `terminal_ui`, `run_editor`, `editor_assist`, and `detail/` Unicode helpers).
- `src/tui/` owns the full-screen TUI (`layout`, `status`, `theme`, `thinking`, `terminal`, `input_handlers`, `run`, and `detail/` frame rendering).

The benchmark built-in JSONL corpus is split into category files under `benchmarks/builtin/` (`coding.jsonl`, `cutoff.jsonl`, `multi-turn.jsonl`, `reasoning.jsonl`, `safety.jsonl`, `writing.jsonl`) so individual prompt sets can be maintained without editing one large file. The `cutoff` category contains one dated factual question per month (January 2023 through July 2026) sourced from `llm_knowledge_cutoff_test_dataset_v2.md`; each case tags its event month and carries a `reference_answer` for knowledge-cutoff evaluation.

## Version Metadata and Test Layout (v0.83)

v0.83 moves version constants out of headers into `src/version/version.cpp` so `kVersion`, copyright, and license strings have one owned definition. `include/ainiux/version.hpp` keeps only the extern declarations.

The unit-test driver was refactored from one large `tests/unit/test_runner.cpp` into per-module files under `tests/unit/<module>/`, each exposing a `run_all()` entry point. `test_runner` now only dispatches those suites. This keeps new tests close to the code they exercise and makes failures easier to locate.

v0.83 also expands automated coverage and adds small mocks for faults that are awkward to reproduce portably:

- `tests/mock_server/slow_http_mock.py` plus `tests/unit/mock/slow_server.cpp` for real local slow-response and chunked-body timeout tests.
- `tests/mock/posix_io_mock.c`, preloaded with `LD_PRELOAD`, to simulate `ENOSPC` on write paths tagged with `mock-enospc`.
- `tests/unit/io/test_io_faults.cpp` and `tests/unit/test_io_faults.cpp` for read-only permission failures (`chmod`) and the network/disk fault cases above.

Environment-dependent fault tests run in a separate `build/test_io_faults` binary so the main `test_runner` stays fast and deterministic on every platform.

## Project-Local Code Index First Slice

The v1.0 agent groundwork begins with an isolated, non-agent index mode under `src/agent/index/`. Its SQLite database is always `<workspace>/.ainiux-pr/index.sqlite`; it is deliberately separate from the user chat library (`~/.ainiux/ainiux.db`). SQLite WAL, foreign-key cascades, a single transactional writer, and bounded parallel readers/scanners provide inexpensive incremental refreshes without adding a dependency.

The scanner set mirrors every non-text editor language: Markdown, Python, C/C++, C#, Java, JavaScript/TypeScript, HTML/HTML-only, CSS, XML, JSON, Bash, PHP, Perl, Ruby, Rust, Go, PowerShell, Assembly, SQL, TOML, YAML, and INI. It reuses the editor's case-insensitive extension detector, including `.jsx`, `.tsx`, `.mts`, and `.cts`. Dedicated scanners are grouped by lexical family under `src/agent/index/`; they combine comment/string masking, precompiled regular expressions, and indentation/brace/block scope tracking rather than compiler front ends or language servers. HTML scanning recognizes identified/custom elements and delegates embedded script and style blocks to the JavaScript/TypeScript and CSS scanners while ignoring data-only script types; HTML-only intentionally avoids embedded-language extraction. Document/data/configuration formats yield structural symbols rather than pretending to have functions or classes. This trades edge-case accuracy for startup speed and portability and makes the index a hint, never source-of-truth. Unchanged size/mtime pairs are not opened or hashed. Changed files are read once and scanned in a bounded worker pool, while all database replacement happens in one final transaction so cancellation preserves the previous snapshot. `--clear-index` removes only the project index database and its SQLite sidecars, refuses a symlinked `.ainiux-pr` directory, and leaves other project-local state untouched.

The lightweight schema stores only metadata, files, and definitions. Each definition receives a 0–100 static importance score during its existing scan from declaration kind, visibility, and scope. Symbol and task searches keep lexical match tiers primary and use importance only as a tie-breaker. Schema versions 1–3 migrate transactionally by rescanning definitions, adding importance, and dropping superseded graph tables; a cancellable post-commit SQLite compaction reclaims their storage without invalidating a successful migration.

Agent startup probes for a completed index but never refreshes or loads it before readiness. Agent Act/Plan uses short-lived read-only SQLite queries as non-authoritative hints; security review alone retains an eager immutable snapshot as an authorization boundary. Agent-native mutations rescan only touched files into a revision-tagged overlay and enqueue exact paths into one RAII-owned coalescing worker. Lazy queries merge that overlay until the corresponding SQLite generation completes. Potentially mutating commands enqueue a full-tree incremental check, and successful task completion waits for one final incremental pass. Full/multi-file discovery and scanning use `floor(online_cores × 0.80)`, bounded by work items and with at least one worker when work exists; zero work uses none and a single-file scan runs inline.

Workspace discovery has fixed safety exclusions for project state, VCS, generated output, dependencies, caches, and virtual environments. It reads only root `.gitignore` and `.ignore` files using a documented practical wildcard/negation subset and never follows directory symlinks. Binary, invalid UTF-8, oversized, and individually unreadable source files are recorded as skips; structural traversal and database failures remain fatal. Markdown reporting opens the index read-only, streams rows in deterministic source order, and warns rather than refreshing when lightweight freshness metadata differs.

## Advisory Editor File Sessions

File-backed editor buffers use `src/editor/file_session.*` to canonicalize identity, fingerprint disk state, and own an atomic `FILE.LOCK` directory. `EditorState` copies share the RAII lock through `std::shared_ptr` because active buffers are copied while switching buffers and temporarily moving between editor and chat modes; the underlying lock object remains uniquely responsible for token-checked cleanup. Scratch buffers have no lock. The lock is held for the entire buffer lifetime and Save As is a destination-locked transaction that retargets only after a successful write.

Lock metadata uses a bounded, versioned, hex-encoded line format so arbitrary canonical path bytes cannot alter its structure. Automatic stale recovery is deliberately limited to a valid same-host owner whose PID is proven absent with `kill(pid, 0)`. Cleanup names only `owner` and the lock directory and restores owner metadata if an unexpected entry prevents `rmdir`; recursive deletion is forbidden. Because locks are advisory, a separate fingerprint check protects saves from external modification, inode replacement, and deletion, and overwrite confirmation is valid only for the fingerprint observed by that prompt.

The ENOSPC fault launcher preserves the ordinary `posix_io_mock.so` preload for normal binaries. For sanitizer binaries it detects the ASan dependency, resolves the compiler's runtime, and places that runtime first in `LD_PRELOAD`; an unresolved sanitizer runtime is an explicit unsupported-toolchain failure. The aggregate `test` target invokes integration tests only after unit and fault tests complete, including under parallel make.

## Compact native agent tool schemas (v1.18)

Native tool definitions are sent on every tool-capable model request and were
exceeding ~4k tokens for a full Act session. The registry was slimmed by:

1. **Removing unused tools** from the advertised set: `index_status`,
   `index_update`, `index_rebuild`, `find_tests`, `inspect_code_task` (index
   lifecycle remains CLI/mutation-driven).
2. **Renaming for industry alignment:** the final advertised native set uses
   `index`, `ls`, `glob`, `grep`, `symbol`, `outline`, `read`, `run`, `fetch`,
   `web_search`, `goal_met`, `attach`, `edit`, `write`, `mkdir`, `mv`, `rm`,
   and `apply_patch`, subject to session policy.
3. **Compacting tool descriptions** while preserving wire schemas and critical
   compatibility cues for `edit` (flat ops), `apply_patch` (Codex markers),
   and `run` (shell-free argv).
4. **Removing execute-time aliases.** Old names are not advertised or accepted;
   compaction alone can still recognize them in transcripts created by older releases.

## Surface-neutral control operations and wire DTOs (v1.3 PR 1)

The control API begins with an application boundary rather than a socket. Reusable
chat and image operations accept explicit requests, shared cancellation tokens, and
typed event sinks; they return structured provider results without choosing files,
writing terminal streams, installing signal handlers, or owning UI state. Existing
CLI adapters keep signal monitoring and stdout/stderr formatting, which makes their
current behavior the compatibility baseline. The existing `run_agent_goal` remains
the headless agent boundary until the single serialized agent lane is introduced
with the job registry.

Public wire types live under `src/server/` even before the listener exists. They are
versioned independently from internal runtime and operation enums: conversion maps
internal events to stable lowercase strings and maps every `ErrorCode` to a public
HTTP status/code pair. Arbitrary detail/data fragments are admitted only when they
parse as JSON objects. This prevents future refactors of internal enums from silently
breaking API clients and prevents invalid fragments from corrupting envelopes.

The initial parser and concurrency constants are frozen in `src/server/limits.hpp`
and documented in `docs/api.md`. Long work will be submitted as jobs, so request
timeouts remain bounded. Chat/image provider work shares a small global pool while
run, plan, and interactive-agent mutations share one workspace lane. A competing
agent operation is a typed conflict, not a hidden queue. PR 1 intentionally adds no
listener, authentication surface, `--server` option, or network-reachable route.

## Loopback control listener and strict HTTP subset (v1.3 PR 2)

The first network slice is intentionally IPv4 loopback-only and uses a small
in-tree HTTP/1.1 parser instead of adding a server framework. It accepts
origin-form ASCII targets, CRLF framing, one Host header, at most one exact
Content-Length, bounded headers/body, and bounded keep-alive requests. It rejects
transfer encodings, duplicate headers, folded headers, encoded/relative traversal,
non-loopback Host values, and cross-origin requests before dispatch. Per-connection
workers own their sockets; the listener tracks a hard active cap, reaps completed
workers, shuts active sockets down on cancellation, and joins every worker.

The full-control secret is separate from provider credentials and from the
path-bound MCP-only secret. Both are compared without content-dependent early
exit. All API routes, including minimal health, require a bearer credential so a
local webpage cannot rely on ambient loopback access. PR 2 routes only health,
status, and capabilities; capabilities explicitly marks jobs, MCP, and the
OpenAI adapter unavailable. This preserves capability detection without
prematurely exposing the PR 1 chat/image operations over the network.
The initial default port is 8766, adjacent to but distinct from the documented
8765 local MCP development examples; `--port` remains authoritative.

## Bounded one-shot jobs and replay broker (v1.3 PR 3)

HTTP requests validate a small operation-specific JSON schema and submit work to
a surface-neutral registry. Provider chat/image jobs share four execution slots;
run and plan atomically reserve the one workspace agent lane at submission and
return a typed conflict instead of accumulating mutation work. Agent execution
receives the server's canonical workspace as an explicit argument, avoiding a
process-wide current-directory change.

Each job owns a cancellation source and a bounded event broker. Producers never
wait for subscribers. The broker retains at most 256 events and 1 MiB per job,
assigns monotonic IDs, and detects cursors overtaken by eviction. SSE connections
read by ID, emit periodic heartbeats, close after the terminal event, and abandon
their socket on a bounded send timeout. Job workers are owned and joined by the
registry—not by the job object—so terminal eviction cannot make a worker join
itself. Retention is process-local, bounded by `--max-jobs`, and intentionally
does not promise restart persistence.

Idempotency keys are scoped across the registry: the same canonical JSON and
operation return the retained job; any changed operation or payload conflicts.
Remote schemas exclude key/header/base-URL/path fields. Provider credentials and
endpoints come only from server startup configuration, and public job errors
redact resolved keys and replace server-side file paths with generic messages.

## Stateless MCP adapter and task handles (v1.3 PR 4)

The server exposes MCP at `/mcp` with a separate, path-bound MCP-only bearer
credential. The adapter implements the 2026-07-28 stateless Streamable HTTP
shape: every request is a POST with per-request metadata and matching
`MCP-Protocol-Version`, `Mcp-Method`, and applicable `Mcp-Name` headers. It
does not mint protocol sessions, serve a GET SSE stream, or accept the full
controller credential.

MCP tools are a deterministic, deliberately small projection of the existing
`JobService`: chat, run, plan, image, and opaque job inspection/cancellation.
Tool calls never duplicate provider or agent execution. Clients that advertise
`io.modelcontextprotocol/tasks` receive an opaque high-entropy task alias that
maps to the bounded in-memory `JobRegistry`; `tasks/get`, `tasks/update`, and
`tasks/cancel` operate on that alias. The alias map is bounded with the server
job limit and is discarded on restart. Clients without task extension support
receive a valid completed tool result containing a job snapshot, preserving a
usable fallback without holding an HTTP request open.

Only JSON responses are used in this slice. Existing control-API SSE replay
remains the richer event surface; MCP task polling is intentionally separate so
the stateless protocol does not inherit controller session or replay semantics.

## Revision-safe remote chat persistence (v1.3 PR 7)

Remote chat threads reuse `chat::SqliteStore`; the control API never opens raw
tables for clients or creates a second persistence model. Schema version 5 adds
a monotonic revision to each thread. New threads start at one; full saves from
the existing TUI and API message appends both compare the observed revision and
increment it under `BEGIN IMMEDIATE`. This makes a stale client or TUI snapshot
conflict with changes from either surface across separate SQLite connections,
while rollback preserves the previous transcript on every validation, write,
or commit failure.

The server owns one lazily opened `ChatService` and serializes calls over its
full-mutex SQLite connection. Laziness avoids touching the personal chat library
unless a chat-thread route is used. Remote creation and append schemas are
strict and intentionally persistence-only: appending messages does not start a
provider request, and attachment input is deferred until a contained upload
contract exists.

Remote reads expose only stable thread/message DTOs. They omit database paths,
provider base URLs, usage records, compaction internals, attachment bodies,
managed-media digests, and original source references. A separate metadata-only
query avoids materializing legacy inline payload/source columns and caps output
at 64 attachments per message. Loads keep the newest 512 messages under a 4 MiB
content cap and report the total count, original ordinals, and truncation;
listing keeps the newest 200 summaries. A remote GET does not update the TUI's last-thread app
state. Store failures cross the wire only as path-free typed errors.
