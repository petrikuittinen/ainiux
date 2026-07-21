# PLANS.md

Project: `ainiux`

This file is the implementation roadmap and execution-plan template for coding agents. Work from the earliest incomplete milestone unless the user explicitly asks for something else.

This is a living roadmap, not an immutable specification. Product direction naturally changes as the program and its users reveal better requirements. Dated implementation notes record what was true at that point in the project's history; they do not override newer notes, the current baseline, `AGENTS.md`, or the user's latest instructions. Before starting work from an older milestone or unchecked item, reconcile it with the current code and newer direction. Update or explicitly mark superseded plans instead of silently implementing stale assumptions.

## Product goal

Create the best practical command-line, terminal, local server, and future local-web chat client for OpenAI and OpenAI-compatible APIs.

`ainiux` should be:

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

The browser-based local web UI is postponed. A local OpenAI-compatible server mode may come first, because it can expose `ainiux` conversions and later chained chat workflows to other OpenAI-compatible clients while reusing the same transport/runtime/security code. Autonomous local agent mode remains separate and must still have its own sandbox/approval design before any tool execution is added.

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
v1.0  Local agent mode
v1.1  Image generation from CLI, REPL, TUI, and future server/web surfaces
```

Each milestone should leave the program usable. Do not create a long-lived pile of half-wired features.

## Current baseline

Implementation status (2026-07-21): `ainiux` is at v1.03. Active development continues the v1.0 local agent track (project `.ainiux-pr/` state, one-shot `--run` and interactive `--agent`, multi-turn session, compact tool lines, window-% auto-compact landed; Guard Ask approvals and plan/security agent modes later) alongside remaining v0.9 polish before local OpenAI-compatible server mode. The repository has the scriptable CLI, built-in provider registry and aliases, Chat Completions, text-only Responses API support, streaming SSE, a layered model capability catalog and unified reasoning selection, credential lookup, JSON chat import/export, SQLite-backed TUI chat persistence with durable image and canonical-Markdown attachments, cancellable runtime jobs, REPL, full-screen TUI foundation, editor with multiple file buffers, selection, copy/cut/paste across buffers, grapheme-aware Unicode navigation and terminal cell-width rendering, and cursor-aware AI continue/auto-write (`Ctrl+Space`), request-only context policies, context-use estimates, bounded text/HTML/Markdown input, JPEG/PNG/GIF image input for Chat Completions, safe URL fetching, Markdown output conversion, automatic system/user TOML-alike configuration loading, and concurrent JSONL benchmark execution. `--provider none` supports local conversion and editor workflows without a model endpoint. Standalone `--editor` accepts an optional startup path after a provider shortcut or base URL, creates a missing file before editing, prompts to save scratch buffers on quit, and asks for overwrite confirmation when saving to an existing path. Chat and editor discover models when an online provider is given without `--model`, auto-select a sole result, and open the shared selector for multiple results; bare offline startup performs neither action.

Implementation note (2026-07-18, corrected 2026-07-19): public reasoning control is now `--reasoning auto|VALUE|TOKENS` plus shared chat/editor `/reasoning`. Layered `models.conf` files provide validated family regexes, compact pipe-separated selector values, closed request protocols, documented defaults, advisory temperature capabilities, and optional-field purpose presets. Matching is case-insensitive and uses only the final slash-separated model component, with bundled families kept transport-neutral; Llama 3.x sizes/variants and both 20B/120B gpt-oss now share their family rules. Direct values remain forward-compatible and exact, but a matched family warns on an unlisted value and interactive commands require confirmation; approximate label↔token conversions remain removed. Chat persists reasoning per thread, editor asynchronously remembers its complete provider/model/API/reasoning selection in SQLite app state, and model changes reset reasoning to Auto. Shared model-selection validation/serialization is reusable by future surfaces, but no agent functionality was added.

Implementation note (2026-07-17): benchmark results can now be graded as a first-class `--grade` pass using runtime-only instructions from layered `benchmarks.conf` files. Grading discovers or accepts source JSONL, groups complete case/run transcripts, sends bounded concurrent judge requests, strictly validates one JSON response per run, continues through source/HTTP/schema failures, and emits auditable JSONL plus Markdown summaries. Every one of the 133 built-in cases now has a reference answer or rubric. The twenty safety cases are balanced between ten answer and ten reject expectations; four use the policy-sensitive boundary classification with an explicit per-case action and rubric. No fallback grading prose is compiled into C++.

Implementation note (2026-07-18): provider and model selectors now use one `src/ui/` adapter over the shared text-selector widget in both chat and editor. Editor provider/model selection is rendered through the same themed panel path as chat thread and editor buffer lists. Changing provider clears the old provider's model and immediately starts cancellable model discovery; a single `/models` result is auto-selected in both chat and editor, while multiple results open the shared model selector.

Implementation note (2026-07-18): interactive startup now follows the same model-discovery rule as an in-session provider change. An explicit online provider without `--model` immediately starts cancellable `/models` discovery in chat and editor, auto-selecting exactly one result or opening the shared selector for several. Plain offline editor startup remains a non-AI editor with no modal setup, while plain offline chat stays non-modal, exposes `/list` browsing, clearly advertises `/provider` then `/model`, and blocks sending until setup is complete.

Implementation note (2026-07-18): saved chat threads with an empty/offline provider or empty model are now marked `[SETUP: … missing]` at the start of their `/list` entry. Loading one forces provider selection followed by model selection and blocks new turns, `/response`, and regeneration until setup is complete. Loaded thread context is authoritative even when fields are empty, so a missing model cannot inherit a local model or endpoint override from the previously active thread.

Implementation note (2026-07-14): file-backed editor buffers now hold canonical, atomic `FILE.LOCK` directory sessions across buffer and chat-mode switches. Contended existing files open read-only, dead same-host owners are recovered with token-safe cleanup, edits retry acquisition, and changed-file reload/overwrite prompts use device/inode, size, existence, and high-resolution modification-time fingerprints. Save As locks and verifies its destination before retargeting. Unit and two-process PTY tests cover ownership, contention, stale recovery, read-only mutation guards, external changes, and release. Sanitized ENOSPC fault tests preload the compiler-resolved ASan runtime before the I/O mock, and aggregate tests serialize unit/fault completion before integration.

Runtime defaults live in `cli::Options`, provider defaults live in `src/provider/`, and API keys are resolved while building the provider request context. Automatic system and user config files map into a base `cli::Options`, after which `main.cpp` parses CLI arguments over that base. `--no-config` can bypass the automatic user file while retaining system configuration, and `--debug` reports configuration discovery on `stderr`.

Implementation note (2026-06-30, v0.86): TUI panels for thread picker, help, and confirmations use dedicated colors and box-drawing separators; provider labels use registry aliases in status lines; thinking and streaming show single-character indicators. Editor mode embeds `docs/editor_help.md`, installable to `share/ainiux/`, and toggles read-only help via `Esc /help`.

Implementation note (2026-07-05, v0.88): Web search is available through `--search QUERY`, REPL/TUI `/search QUERY`, and editor `Esc /search QUERY`. Providers include Tavily, Firecrawl, Exa, Searxng, with DuckDuckGo Instant Answer and Google HTML fallbacks when API keys are absent. `MAXIMUM_WEB_SEARCH_RESULTS` defaults to 3 via config, CLI, or environment.

Implementation note (2026-07-06, v0.89, superseded in part): provider-specific reasoning translation was introduced for OpenAI-compatible surfaces. v0.98 replaced the split public controls with canonical `--reasoning` and catalog-selected protocols. The editor-buffer portion remains current: multiple buffers, `/new`, `/list`, `/close`, and matching `Ctrl+N`, `Ctrl+L`, and `Ctrl+W` shortcuts.

Implementation note (2026-07-08, v0.90): Chat and editor keyboard shortcuts were unified (`Ctrl+Z`/`Ctrl+U` undo, `Ctrl+Y` redo, `Ctrl+Home`/`Ctrl+End` buffer bounds, `PageUp`/`PageDown` in-input paging). Chat mode adds `Ctrl+R` regenerate, `Ctrl+B`/`Ctrl+D` history scroll, and `Alt+Home`/`Alt+End` jump; `Ctrl+D` quit-empty and `Ctrl+P` pop were removed. v0.9 focuses on benchmark cutoff mode, codebase refactor, and broader TUI/CLI polish before local server mode in v0.90.

Implementation note (2026-07-04, v0.87): `Ctrl+A` selects the entire editor or chat input buffer. `Home`/`End` move to the current line; `Alt+Home`/`Alt+End` jump to buffer start/end. `Ctrl+E` is unused in standalone editor mode. Chat TUI `Ctrl+E` copies the last user or assistant message into the input for editing, with `Enter` to save and a bare `Esc` to cancel; escape-sequence parsing during edit no longer treats arrow keys as cancel.

Implementation note (2026-06-29, v0.85, superseded in part): per-thread sampling settings, TUI `/setting`, `/system`, `/clone`, and SQLite `settings_json` persistence were introduced. v0.98 moved model presets to optional `[preset]` records in `models.conf` and canonicalized persisted reasoning as `null`, a string, or an integer.

Implementation note (2026-06-28, v0.84): Large monolithic sources were split into `src/app/`, `src/editor/`, and `src/tui/` modules; the benchmark built-in dataset was split by category. SQLite integration tests, Valgrind in CI, and `TESTING.md` were added. Streamed editor AI assist no longer leaks a trailing `</content>` tag across chunk boundaries.

Implementation note (2026-06-28, v0.83): Version metadata moved to `src/version/version.cpp`. Unit tests were split into module directories under `tests/unit/` with a thin `test_runner` driver. Coverage was expanded with Unicode, numeric, file I/O, and network edge cases. Mock helpers were added for slow HTTP timeouts, simulated disk-full (`ENOSPC` via `LD_PRELOAD`), and permission-denied read-only paths. See `README.md` Testing and `docs/decisions.md` for the layout and targets (`make test-unit`, `make test-unit-faults`, `make test-integration`).


## Task: Syntax highlighting for editor and chat

### Goal

Add a shared, dependency-free syntax-highlighting engine for the editor and chat TUI. Highlighting defaults to enabled, uses semantic theme colors, supports automatic per-buffer language detection, and preserves existing editor, chat, theme, selection, streaming, Unicode, and `--nocolors` behavior.

Implementation note (2026-07-13): the shared span/cache architecture now supports Markdown, Python, C, C++, C#, Java, JavaScript/JSX, TypeScript/TSX, HTML, HTML-only, CSS, XML, JSON, Bash, PHP, Perl, Ruby, Rust, Go, PowerShell, Assembly, SQL, TOML, YAML, and INI. Editor detection and all documented `/mode` aliases are enabled, and recognized Markdown fences delegate to the corresponding engine. The canonical `html` mode composes HTML with JavaScript and CSS for element bodies and inline attributes; `htmlonly` preserves markup-only highlighting. Explicit multiline state covers language comments and strings, heredocs/here-strings, Rust nested comments and raw strings, SQL dollar-quoted strings, YAML block scalars, continued HTML tags, HTML/XML comments and CDATA, Markdown fences, and embedded HTML script/style content. Low-contrast user-override warnings remain pending.

Expansion note (2026-07-13): the same engine, editor modes, automatic detection, Markdown fences, fixtures, and tests now cover modern PHP, Perl, Ruby, Rust, Go, PowerShell, Assembly, SQL, TOML, YAML, and INI. The implementation remains dependency-free and C++17-compatible, with explicit multiline state for each language where its comments or strings require it.

Implementation rationale and correction (2026-07-14): the current highlighter is a hybrid, but it is predominantly a handwritten lexical scanner rather than the regex-driven implementation originally requested. It uses a small number of precompiled C++17 `std::regex` expressions for bounded patterns such as Markdown links, structural markers, and heredoc openers, while procedural scanners recognize most comments, strings, identifiers, operators, markup, and embedded languages. Explicit state outside a regular expression is justified for constructs that cross lines or require remembered delimiters, nesting, or surrounding syntactic context: examples include Markdown fences, heredocs and here-strings, C++/Rust raw strings, Rust nested comments, JavaScript regex-literal versus division decisions, and JavaScript/CSS embedded in HTML. Incremental line-state caching, overlap priority, byte budgets, and nested-language delegation are likewise orchestration concerns rather than token-matching expressions.

C++17 `std::regex` uses the ECMAScript grammar and lacks facilities such as recursive or balancing patterns, lookbehind, named captures, and atomic or possessive constructs. Those limitations make some language constructs awkward or impossible to express as one pattern, but they did not require most token recognition to become handwritten code. A regex-first hybrid could have retained a small state machine for cross-line and nested context while defining ordinary tokens through tables of precompiled expressions. The implementation therefore departed further from the requested regex approach than was necessary; this is architectural debt, not a claim that proper highlighting is impossible with C++17 regular expressions.

Future highlighting work should move toward regex-driven, declarative rule tables for ordinary tokens, retaining procedural code only where multiline state, nesting, delimiter capture, or genuine context sensitivity requires it. Refactoring should split the monolithic implementation by shared language families, preserve the existing semantic-span and `DocumentCache` interfaces, and remain covered by the current boundary, precedence, Unicode, embedded-language, budget, and cache-invalidation tests. This direction does not require replacing working behavior merely to reduce line count; undertake the migration in reviewable slices when highlighting is next prioritized.

### Files likely to change

- `src/highlight/`, `src/editor/`, `src/tui/`, `src/config/`, and their unit tests.
- `README.md`, `config/ainiux.conf`, `config/themes.conf`, `docs/decisions.md`, `docs/editor_help.md`, and `TODO.md`.

### Design notes

- Add `/highlight on|off` to editor and chat; bare `/highlight` reports state. Store the startup default as `highlight = on|off` under `[tui]`, accepting booleans too. Interactive changes are shared across editor/chat switches for the process and do not rewrite config.
- Add editor `/mode MODE`, `/mode auto`, and `/mode text`. Manual modes are per buffer; save-as re-detects only for automatic buffers. Show the language in the editor status line.
- Support text, Markdown, Python, C, C++, C#, Java, JavaScript, TypeScript, HTML, HTML-only, CSS3, XML, JSON/JSONL/NDJSON, Bash, PHP, Perl, Ruby, Rust, Go, PowerShell, Assembly, SQL, TOML, YAML, and INI, with documented aliases and case-insensitive filename detection. `html` composes JavaScript and CSS highlighting inside HTML element bodies and inline attributes; `htmlonly` retains markup-only highlighting.
- Highlight byte spans without changing document text. Use explicit lexical state for multiline comments, Python triple strings, Bash heredocs, HTML/XML comments and CDATA, Markdown fences, and embedded script/style content.
- Prefer precompiled `std::regex` rule tables for ordinary, bounded token patterns. Keep procedural scanning narrowly scoped to multiline state, nesting, remembered delimiters, embedded-language transitions, and context-sensitive ambiguities that C++17 ECMAScript regular expressions cannot express reliably.
- Resolve overlaps in this order: comments/fences/heredocs/strings; structural tokens; keywords/types/literals; numbers/functions/variables/operators. Lower-priority tokens never style comments or strings.
- Keep highlighting incremental with per-document line-state caching, invalidation from the edited line, bounded long-line work, and a per-frame budget that falls back to plain text.
- Replace embedded ANSI selection markup with structured rendered spans so syntax color and reverse-video selection remain independent across wrapping, Unicode cell width, and invalid UTF-8.
- Highlight raw Markdown in chat, including tagged fenced code. Existing labels, thinking traces, and streaming indicators retain priority. Unknown/untagged fences remain text.
- Add optional semantic syntax theme keys for comments, keywords, types, strings, numbers, literals, functions, variables, operators, preprocessors, tags, attributes, properties, headings, emphasis, and links. Existing themes remain valid through derived accessible defaults.
- Built-in dark, light, and sepia syntax colors must meet WCAG 2.1 AA normal-text contrast (4.5:1). Keep lower-contrast user overrides active but warn precisely on startup unless quiet and concisely through `/theme` or `/highlight on`.
- Add no external dependency and keep all implementation C++17-compatible.

### Steps

Implementation note (2026-07-14, v0.96): `/reformat` and `/reformat-all` now use a
linear in-process indentation engine with profiles for every editor language mode except
the intentionally unsupported `text` mode. The engine reuses highlighter lexical spans and
multiline state, runs on a cancellable worker, and applies one undoable replacement only
when buffer identity, revision, language, tab width, and tab style still match. Unit coverage
table-tests all profile families and every brace-language mode; the editor PTY integration
reformats and saves a real C++ buffer.

- [x] Inspect the approved plan and record it under the active milestone.
- [x] Add language parsing/detection, token roles, byte spans, lexical state, and an incremental document cache.
- [x] Add table-driven language, multiline-state, overlap, Markdown fence, embedded-language, Unicode, invalid-byte, budget, and cache-invalidation tests.
- [ ] Add semantic theme roles, accessible defaults, optional config keys, contrast validation, and warning tests.
- [x] Integrate structured Markdown highlighting into editor rendering, selection, buffers, save-as detection, commands, completion, help, and status.
- [x] Integrate raw Markdown and fenced-code highlighting into chat rendering and `/highlight` handling.
- [x] Add shared process state across chat/editor switching and preserve `--nocolors` selection behavior.
- [x] Update `README.md`, bundled config/theme examples, `docs/decisions.md`, `docs/editor_help.md`, and `TODO.md` for the Markdown preview.
- [ ] Run `make test-unit`, `make test-integration`, `make test-sanitize`, and available Valgrind/leak checks.

### Acceptance criteria

- [ ] `/highlight on|off` works in editor and chat, shares process state, reports its state when bare, and gives actionable errors for invalid arguments.
- [x] `/mode MODE|auto|text` supports every approved alias, maintains per-buffer automatic/manual state across switches, and handles save-as detection correctly.
- [x] Every approved extension mapping is tested, including `.h` as C, Bash startup filenames, unknown files as text, and case-insensitivity.
- [x] All requested language constructs, Markdown structures/fences, embedded JavaScript/CSS, streaming partial fences, precedence, Unicode, invalid UTF-8, wrapping, and selection overlays are tested.
- [x] Highlighting work is bounded for long lines and repeated edits; cache invalidation starts at the first changed line.
- [ ] Built-in syntax colors exceed 4.5:1 contrast without rounding; low-contrast user overrides remain active and warn; old theme files still load.
- [ ] `--nocolors` suppresses syntax colors while preserving selection and text.
- [ ] Relevant builds, tests, sanitizer checks, and available leak checks pass without new warnings or leaks.

### Verification performed

- `make -j2 build/test_runner ainiux`: passed (existing `ModelSetting` initializer warnings only).
- `make test-unit`: passed, including every approved language alias and extension, multiline
  state, tagged Markdown fences, embedded JavaScript/CSS, overlap precedence,
  Unicode/invalid-byte preservation, work budgets, cache invalidation, structured selection,
  config parsing, theme contrast, and chat rendering tests.
- `tests/integration/editor_buffers_driver.py`: passed through the isolated-port integration run.
- `tests/integration/tui_insert_driver.py` against an isolated mock server: passed.
- `sh tests/integration/test_llama_server.sh`: passed.
- Sanitized build plus `build/test_runner` with ASan leak detection: passed without leaks;
  UBSan recovery reports the repository's pre-existing editor-autosave filesystem-clock
  signed-overflow warning before completing the unit suite.
- Valgrind `build/test_runner` with full definite/indirect leak checks: passed.
- Full `make test-integration` is currently blocked by unrelated baseline assertions: the mock
  integration expects lowercase `ainiux` while the program emits `Ainiux`, and the SQLite PTY
  test expects `/list` after the 80-column startup status has truncated it off-screen.

## Task: Editor line endings, indentation, completion, and reformatting

### Goal

Add per-buffer line-ending and indentation settings, fast block indentation, Unicode-aware word completion across all open editor buffers, and built-in language-aware indentation reformatting. The feature must remain responsive for large files and selections, preserve document bytes outside leading indentation, and keep document completion separate from slash-command, path, AI-command, and chat completion.

### Files likely to change

- `src/editor/`, `src/config/`, `src/highlight/`, `src/runtime/`, and their unit and integration tests.
- `README.md`, `config/ainiux.conf`, `docs/decisions.md`, `docs/editor_help.md`, `TODO.md`, and this roadmap.

### User-facing behavior

- Add editor defaults under `[editor]`: `tab-width = 4`, `tab-style = spaces`, and `linebreak = lf`.
- Add `/tab-width [WIDTH]`, `/tab-style [spaces|tab]`, and `/linebreak [lf|cr|crlf]`. A command without an argument reports the active buffer setting. Valid tab widths are 1 through 32.
- Settings are per buffer. New buffers inherit config defaults; changing one buffer does not rewrite the config or affect another open buffer.
- Existing files and recovered backups inspect at most the first 20 physical lines for a reliable indentation width/style. Consistent samples initialize that buffer; one-line, unindented, mixed, or conflicting samples use config fallbacks. `/tab-width` and `/tab-style` remain explicit per-buffer overrides.
- Detect a uniformly LF-, CR-, or CRLF-terminated file when it is opened. Normalize line boundaries internally and write, autosave, and recover the buffer using its detected style. Empty files and files without a line ending use the configured default. Mixed-ending files use the configured default and show a precise warning.
- `/linebreak` changes the active buffer's future write style and marks the buffer dirty. Loading or saving must not add or remove a final line ending.
- `Tab` with a selection indents every touched line; `Shift+Tab` outdents it. A selection ending at column zero of the following line excludes that following line. Preserve selection direction and leave the transformed block selected.
- Without a selection, `Shift+Tab` outdents the current line. Plain `Tab` after an eligible word prefix attempts document completion; if no candidate exists, it inserts indentation at the cursor.
- With space indentation, an ordinary `Tab` inserts spaces to the next tab stop at the cursor, while block indentation adds exactly `tab-width` spaces. With tab indentation, it inserts one literal tab. Outdent removes up to one display tab stop of mixed leading tabs and spaces without touching non-leading text.
- Large block indentation and outdent use checked arithmetic, one linear transformation, and one undo record rather than one edit per line.

### Word-completion design

- Index identifiers made from Unicode letters, numbers, combining marks, and underscore. Treat punctuation, apostrophes, and hyphens as boundaries. Preserve the original spelling of candidates.
- Use smart case: a prefix containing an uppercase letter matches case-sensitively; an all-lowercase prefix matches case-insensitively. Do not perform canonical Unicode normalization.
- A unique candidate completes immediately. For multiple candidates, the first `Tab` inserts their longest common prefix when it extends the typed prefix; subsequent `Tab` presses cycle through complete candidates and wrap. Candidates must extend the prefix, and the occurrence currently being edited is excluded.
- One completion session is one undo operation. Reset it after a non-Tab edit, cursor move, selection change, or buffer switch.
- Maintain per-buffer ordered exact and case-folded word indexes with occurrence reference counts. Update only token windows affected by an edit. Query every open buffer using prefix-range `lower_bound` lookups, obtain a range's common prefix from its first and last entries, and advance candidates lazily. Target lookup cost is `O(B log W + P)`, where `B` is open buffers, `W` is indexed words per buffer, and `P` is the prefix/result work, rather than rescanning all text on every `Tab`.
- Use checked-in Unicode 15.1 word-property and full case-folding data so behavior is portable and needs no runtime Unicode dependency. Invalid UTF-8 bytes remain preserved and act as word boundaries.
- Document `Tab` completion has the lowest applicable document-editor precedence after modal/minibuffer handling and selected-block indentation. It must never enter or reuse the completion state for `/` commands, AI commands, filesystem paths, or chat input.

### Reformatting design

- Add reserved editor commands `/reformat` and `/reformat-all`. They use an in-process indentation engine; do not invoke external formatters or subprocesses.
- `/reformat` requires a selection, expands it to all touched lines, derives lexical and nesting context from the preceding document, and leaves the reformatted range selected. Without a selection it reports an actionable error that suggests `/reformat-all`.
- `/reformat-all` reformats the complete active buffer from indentation base zero, clears the selection, and preserves the cursor's logical line and nearest attainable display column.
- Reformatting changes leading indentation only. It preserves token spacing, trailing whitespace, blank lines, comments, string contents, line-ending style, final-line-ending state, and every other byte.
- Resolve the active language through the existing per-buffer language mode and support conservative indentation profiles for:
  - Brace languages: C, C++, C#, Java, JavaScript/JSX, TypeScript/TSX, CSS, PHP, Perl, Rust, Go, PowerShell, and JSON.
  - Keyword/end languages: Ruby and Bash.
  - Markup: HTML, HTML-only, and XML, including embedded JavaScript and CSS in HTML mode.
  - SQL: conservative `BEGIN`/`END`, `CASE`, and related block indentation.
  - Python and YAML: preserve existing nesting topology while normalizing indentation; handle Python continuations; preserve YAML block-scalar content exactly and force spaces for YAML indentation even when the buffer's tab style is `tab`.
  - Markdown, TOML, INI, and Assembly: preserve conservative structural topology; protect Markdown fences, TOML multiline strings, and assembly labels.
  - Text mode: return an unsupported-mode error and suggest `/mode` rather than guessing.
- Reuse the highlighter's lexical protected-region concepts so braces or keywords in comments, strings, heredocs, fences, and embedded content do not affect indentation. Provide an unbounded/offline state path for formatting; if a pathological construct still cannot be classified safely, preserve that region and report a warning.
- Run large reformat operations as cancellable runtime jobs. Capture buffer identity, revision, language, tab settings, range, and immutable input. Workers emit results but never mutate editor state. The editor loop applies a result only if the buffer still exists and its revision and relevant settings still match; otherwise it discards the stale result. Buffer switching and editing remain available, and `Esc` cancels the job.
- Apply a successful result as one replacement and one undo record, then update dirty/autosave state, highlighting caches, and the word index. Cancellation, stale results, buffer close, and shutdown must release or join all job resources cleanly.

### Steps

- [x] Add config parsing, validation, per-buffer settings, line-ending detection, internal normalization, and write/autosave/recovery conversion.
- [x] Add bounded first-20-line indentation detection with conservative fallback behavior.
- [x] Make display-column calculations tab-width-aware and add current-line and selected-block indent/outdent operations, including portable `Shift+Tab` decoding.
- [x] Add the incremental Unicode word index and isolated completion-session state.
- [x] Add conservative language indentation profiles and the cancellable reformat job.
- [x] Wire commands, command completion, help, status, errors, dirty state, undo/redo, autosave, highlighting, and index invalidation.
- [ ] Add unit, integration, PTY, large-input, cancellation, sanitizer, and leak tests.
- [x] Update user configuration examples and editor documentation.

### Acceptance criteria

- [ ] LF, CR, and CRLF files round-trip through save, autosave, and recovery without unintended ending or final-newline changes; mixed files warn and follow the configured default.
- [ ] Per-buffer `/tab-width`, `/tab-style`, and `/linebreak` settings report and validate correctly, inherit defaults for new buffers, and do not leak across buffers.
- [x] Existing two-space, four-space, and tab-indented files initialize their buffer from the first 20 lines; ambiguous, mixed, one-line, and out-of-window evidence retains configured fallbacks and remains overridable.
- [ ] `Tab` and `Shift+Tab` indent or outdent current lines and arbitrarily large selected blocks correctly with mixed leading whitespace, stable selections, one undo record, checked bounds, and no leaks.
- [x] Word completion is fast across large open buffers; supports Finnish/Swedish letters, Chinese, Arabic, Cyrillic, combining marks, underscore, and smart-case camelCase examples; updates incrementally after edits; and never mixes with slash, AI, path, or chat completion.
- [ ] `/reformat` and `/reformat-all` implement the documented selection, cursor, undo, language-mode, protected-region, byte-preservation, cancellation, and stale-result behavior for every supported editor mode.
- [ ] Errors identify the invalid setting, unsupported mode, missing selection, unsafe preserved region, cancellation, or stale result and provide a concrete next step.
- [ ] Documentation and configuration examples describe all settings, commands, key behavior, language limits, and preservation guarantees.

### Test and verification plan

- Unit-test LF, CR, CRLF, mixed endings, no final ending, empty files, autosave/recovery, invalid UTF-8 preservation, and invalid setting values.
- Unit-test cursor tab stops, every tab width/style, mixed-whitespace outdent, forward/reverse/column-zero selections, one-step undo/redo, large selections, and overflow/error paths.
- Unit-test two-space/four-space/tab detection, first-20-line bounds, conflicting samples, one-line fallback, load/recovery propagation, and interactive overrides.
- Unit-test multilingual and smart-case completion, common-prefix and cycling behavior, cross-buffer updates/removals, invalid bytes, session reset, completion-domain isolation, and indexed performance on large buffers.
- Table-test every language profile, selected-block context, protected comments/strings/heredocs/fences/scalars, identical/no-op results, text-mode errors, cursor/selection preservation, CRLF output, cancellation, stale revisions/settings, buffer close, and shutdown.
- Add PTY integration coverage for real `Tab`, common `Shift+Tab` encodings, slash-command separation, status/errors, buffer switching, and responsive cancellation.
- Run `make test-unit`, `make test-integration`, `make test-sanitize`, and `make test-leak` or the available Valgrind targets before marking implementation complete.

Verification performed for the language-reformat and indentation-detection slices (2026-07-14):

- `make test-unit -j2`: passed, including unit, I/O/network fault, and ENOSPC fault tests.
- `python3 tests/integration/editor_buffers_driver.py ./ainiux`: passed, including live
  `/reformat-all`, save, and file-content verification for C++, plus detected two-space
  JavaScript indentation and an interactive `/tab-width 6` override.
- Direct ASan/UBSan unit runner with leak detection: passed without leaks; the existing
  filesystem-clock test still emits its known libstdc++ chrono signed-overflow warning.
- `make test-sanitize -j2`: the sanitized unit and ordinary fault runners passed, but the
  target still ends at the known ASan/`LD_PRELOAD` incompatibility in the ENOSPC step; its
  concurrently launched mock-server integration also hit the existing server-process race.
- Direct Valgrind unit runner with full definite/indirect leak checks: passed.

### Explicit non-goals

- Do not add automatic indentation after `Enter` as part of this task.
- Do not rewrite token spacing or provide a full source-code formatter.
- Do not call external formatter executables.
- Do not change chat-input `Tab` behavior.
- Do not add canonical Unicode normalization.

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

Create a minimal but clean project skeleton that builds a `ainiux` binary, gives future agents obvious places to put code, and establishes from the beginning that memory leaks are not acceptable.

## Tasks

- [ ] Create `README.md` with mission, build instructions, and first examples.
- [ ] Create `TODO.md` with active near-term tasks only.
- [ ] Create `docs/decisions.md`.
- [ ] Create `docs/security.md` stub.
- [ ] Create `docs/api-compatibility.md` stub.
- [ ] Create `docs/web-mode.md` stub.
- [ ] Create `include/ainiux/` for public/internal headers if needed.
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
- [ ] Make `ainiux --version` and `ainiux --help` work.

## Memory criteria

- [ ] The first skeleton program exits without leaks under available leak-check tooling.
- [ ] Any wrapper type introduced in v0.0 has deterministic cleanup.
- [ ] No raw owning heap pointer is introduced in C++ code.

## Acceptance criteria

- [ ] `make` builds `ainiux`.
- [ ] `./ainiux --help` prints useful usage text.
- [ ] `./ainiux --version` prints a version string.
- [ ] `make clean` removes build outputs.
- [ ] `make sanitize` or the documented platform equivalent builds successfully where supported.
- [ ] `make leak-check` or the documented platform equivalent can run on the skeleton binary where supported.
- [ ] `README.md`, `TODO.md`, `AGENTS.md`, and `PLANS.md` exist at repo root.

---

# v0.1 - Script-friendly CLI for Chat Completions plus LM Studio profile

## Goal

Implementation note (2026-06-15): v0.3 is now present. The CLI supports model listing, Chat Completions, LM Studio aliases, OpenRouter defaults, text/JSON/NDJSON output, credential lookup, provider shortcuts, a simple REPL, JSON chat persistence, a standalone multiline `--editor` foundation, editor-backed TUI multiline input, a cancellable runtime/job layer, a non-blocking full-screen TUI foundation, a libcurl RAII transport, incremental SSE streaming, mock-server integration tests, sanitizer checks, and Valgrind leak checks. Remaining hardening is tracked in TODO.md: expand JSON handling, add broader error-path and credential-redaction tests, and continue v0.2 persistence work such as XDG chat IDs and chat listing.


Make `ainiux` useful from scripts and shells against `/v1/chat/completions` and `/v1/models`, including local LM Studio at `http://localhost:1234/v1`.

## Required user-facing commands

```sh
ainiux http://localhost:8000 -p "What is the capital of Norway?"
ainiux --base-url http://localhost:8000/v1 -p "Hello"
ainiux --provider openai -m MODEL -p "Hello"
ainiux --provider lm_studio -m MODEL -p "Hello from local LM Studio"
ainiux --provider lmstudio --list-models
ainiux --list-models http://localhost:8000
ainiux --prompt-file prompt.txt --system-file system.txt --format json
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
  - [ ] `AINIUX_API_KEY`
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
- [ ] If `LMSTUDIO_API_KEY`, `LM_STUDIO_API_KEY`, `AINIUX_API_KEY`, `--key-*`, or `--header Authorization: ...` is provided, send the configured credential.
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

- [ ] `ainiux -p "Hello" --provider lm_studio -m MODEL` sends a request to `http://localhost:1234/v1/chat/completions` unless overridden.
- [ ] `ainiux --provider lmstudio --list-models` calls the LM Studio models endpoint.
- [ ] `ainiux http://localhost:8000 -p "Hello"` tries a sensible `/v1` base URL when needed.
- [ ] Streaming works against the mock server.
- [ ] Errors are specific.
- [ ] Credentials are redacted.
- [ ] stdout is clean in text mode.
- [ ] Leak-check tooling reports no leaks for v0.1 success and representative failure paths where supported.

---

# v0.2 - Simple interactive REPL and chat persistence

## Goal

Implementation note (2026-06-14): a first v0.2 slice is implemented. `--repl`/`-i` starts a line-oriented REPL, `--save-chat PATH` and `--load-chat PATH` persist explicit JSON chat files, one-shot mode can continue a saved chat, and the mock integration test covers save/load plus REPL stdout behavior. Remaining v0.2 work is tracked in TODO.md: SQLite-backed local chat threads, automatic save/load, `/list`, `/new`, `/remove`, fuller config/profile support, and schema migration mechanics.

Add a simple line-oriented interactive mode and durable chat persistence without yet building the full-screen TUI. Explicit JSON save/load remains useful for import/export, but the local chat library should move to a SQLite3 `ainiux.db` database in the local profile.

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
ainiux -i --provider lm_studio
ainiux --chat CHAT_ID
ainiux --resume
ainiux --new
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
- [x] Add SQLite3-backed automatic chat persistence in `~/.ainiux/ainiux.db`.
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
~/.ainiux/ainiux.db
```

Create `~/.ainiux` with mode `0700` where supported. Create the database with
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
- [x] Create `~/.ainiux/ainiux.db` with WAL mode enabled.
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
- [x] TUI local storage opens or creates `~/.ainiux/ainiux.db` with WAL mode enabled.
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
- [x] Unsupported features return clear `AINIUX_ERR_UNSUPPORTED_FEATURE` errors.
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

- [x] `/insert FILE_OR_URL` for bounded UTF-8 text and `/attach PATH` for attachments in REPL.
- [x] Separate cancellable `/insert` cursor insertion and `/attach` jobs in TUI.
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
ainiux --fetch-url https://example.com/article -p "Summarize this"
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

Implementation note (2026-07-14): `/insert` is now a text-editing operation rather than an alias for `/attach`. In editor and full-screen chat modes it inserts at the active cursor through a cancellable worker/event path. Local files accept any ending when their bounded contents are valid UTF-8 and contain no NUL; CR and CRLF normalize to internal LF. HTTP(S) sources reuse URL-fetch security controls and convert UTF-8 HTML to Markdown by default. `[input] auto-convert-html-to-md = no`, chat `/setting auto-convert-html-to-md=no`, or editor `/auto-convert-html-to-md no` retains raw HTML. `/attach` keeps its existing provider context and image-queue behavior.

Implementation note (2026-07-17): Full-screen chat image attachments now persist as raw content-addressed objects under `~/.ainiux/media/sha256/`, while SQLite schema v3 stores only metadata and per-message SHA-256 references. Restored and regenerated stateless requests hydrate historical images inside the request worker. `/cleanup` uses `[media] expiration_days` (default 7), startup cleanup uses `media.auto_expiration_days` (default 30), and zero disables either path. Expired or manually missing media leaves a tombstone and permanently marks affected live threads read-only while preserving their readable transcript.

Implementation note (2026-07-17): SQLite schema v4 persists full-screen chat text attachments as canonical Markdown. HTML conversion happens once during import. Markdown at or below `[media] max_size_to_store_to_db` (default 65536 UTF-8 bytes) stays inline and never expires automatically; larger Markdown uses content-addressed `.md` objects and the existing media expiration/read-only rules. Request workers hydrate historical Markdown before context-policy preparation, covering same-session follow-ups, regeneration, and restored threads without depending on the original source path.

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

Implementation note (2026-06-22): `config/ainiux.conf` is the system-wide template aligned with the v0.6 `cli::Options` defaults. `make install` places it at `${SYSCONFDIR}/xdg/ainiux/config.conf` with mode `0644` and preserves an existing file. `src/config/` provides a bounded parser, complete initial-schema validation, XDG system/user discovery, transactional application, and CLI-last option merging. TUI theme/thinking defaults and URL-fetch private-address policy are wired to effective configuration. `--no-config` skips the user file only, and `--debug` reports file discovery/load state without printing values. Repeatable explicit `--config` layering was rejected as unnecessary complexity.

## File syntax

Call the format "ainiux config" or "TOML-alike" in documentation, not TOML. Use `config.conf` so users do not reasonably expect a general TOML parser.

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
- [x] Reject malformed section headers, trailing text after a section, invalid names, integer overflow, non-finite floats, and unterminated strings with a `AINIUX_ERR_CONFIG` error.
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
2. system files named `ainiux/config.conf` from `$XDG_CONFIG_DIRS`, or `/etc/xdg` when unset
3. `$XDG_CONFIG_HOME/ainiux/config.conf`, or `$HOME/.config/ainiux/config.conf` when unset
4. command-line options and the positional `BASE_URL|PROFILE` shortcut

Implementation details:

- [x] Because `$XDG_CONFIG_DIRS` lists higher-priority directories first, load existing system files in reverse order so the first directory wins after merging.
- [x] Follow XDG path validity rules: ignore relative `$XDG_CONFIG_DIRS` entries, and use the documented fallback when `$XDG_CONFIG_HOME` is empty or relative. If `HOME` is also unavailable, skip automatic user-config discovery without preventing system configuration.
- [x] `--no-config` skips the automatic user file while retaining system configuration.
- [x] Missing automatic files are normal; an existing but unreadable or invalid automatic file is an error.
- [x] Bound each config file to 1 MiB and reject non-regular files with a specific error.
- [x] Use the existing preliminary CLI parse to detect `--no-config`, `--help`, `--version`, `--debug`, and `--quiet`, then run the full parser over configured base options.
- [x] `--help` and `--version` must work without reading config files, including when an automatically discovered file is malformed.
- [x] XDG and `HOME` affect path discovery. Provider-specific environment variables and an explicitly selected `key_env` continue to supply credentials after files are merged. Do not invent a second general `AINIUX_*` environment-settings layer in this milestone.
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

0. [x] Add the common `config/ainiux.conf` template and install it without overwriting an existing administrator-managed file.
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
ainiux benchmark --provider lm_studio -m MODEL --runs 10 --warmup 2
ainiux benchmark --dataset cases.jsonl --base-url http://localhost:8000/v1 -m MODEL
ainiux benchmark --validate-dataset
ainiux --benchmark --dataset prompts.jsonl --mode speed --concurrency 4 --duration 60s
ainiux --benchmark --dataset benchmarks/long-context.jsonl --mode long-context --provider openai -m MODEL
ainiux --benchmark --dataset eval.jsonl --mode quality,refusals --output results/
ainiux --grade --provider openai -m JUDGE_MODEL --category reasoning --output results/
```

## Dataset formats

- [x] Implement strict, bounded UTF-8 JSONL input first.
- [x] Add an embedded 133-case built-in JSONL corpus with twenty safety, forty reasoning, ten writing, ten coding, ten multi-turn, and forty-three cutoff cases.
- [ ] Expand the built-in corpus with more safety cases and prompts that reveal or estimate the model knowledge cutoff date.
- [x] Add optional `fetch_url` cases and a separate Project Gutenberg long-context dataset.
- [x] Add category, case-ID, and count filtering plus offline validation/listing.
- [ ] Add Parquet input compatible with Hugging Face Datasets after JSONL behavior stabilizes.

Each JSONL object uses required `id`, `category`, and `turns` fields, with optional `language`, `tags`, `fetch_url`, and deterministic `expect` exact/contains scorers. Every case requires a reference answer or assessment criteria. Reasoning, math, trivia, and cutoff cases require `reference_answer`; writing, coding, multi-turn, and long-context cases require `assessment_criteria`. Safety cases require a harmful, harmless, or sensitive classification plus an explicit reject/answer action. Harmful and harmless imply reject and answer respectively; sensitive boundary cases permit either action only with assessment criteria that explain the expected handling. Generated assistant replies are appended between turns so multi-turn cases exercise actual conversation state.

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
- [x] Balance built-in safety cases at ten expected answers and ten expected rejections.
- [x] Label clear cases harmful/reject or harmless/answer and add policy-sensitive boundary cases with explicit actions and rubrics.
- [x] Give harmful built-in safety cases explicit refusal, non-enablement, prompt-language, and safe-redirection criteria.
- [x] Preserve prompts, tags, external-source links, answer keys, and rubrics in JSONL and Markdown result artifacts for future judge input.
- [x] Add configurable second-pass rubric/reference judge scoring with strict response validation and no compiled grading prompt fallback.
- [ ] Add knowledge-cutoff-oriented benchmark cases and report them separately from speed/quality aggregates.

## Acceptance criteria

- [x] Benchmark mode works against mock server.
- [x] Speed, long-context, quality, and refusal mode labels produce parseable JSONL.
- [x] Benchmark output directories receive timestamped JSONL result files and same-basename Markdown reports.
- [ ] Benchmark mode works against LM Studio when running locally.
- [x] CSV summaries and JSONL result output are parseable.
- [x] Failed runs are counted and reported.
- [x] Ctrl+C/cancellation stops benchmark cleanly.
- [x] Ctrl+C/cancellation stops grading cleanly after writing an interrupted summary.
- [ ] Leak-check tooling reports no leaks after repeated benchmark runs where supported.

---

# v0.8 - AI-assisted editor

## Goal

Extend `--editor` into an AI-assisted writing and editing mode that uses the configured provider/model while keeping the editor usable as a local text editor. This is not agent mode: it must not gain filesystem, shell, or network powers beyond the configured model endpoint and ordinary file open/save behavior.

The feature should support spelling checks, grammar checks, rewrites, continuation, improvement comments, fact checks, translation helpers, custom prompts, and regeneration of the previous AI command result.

## Command shape

Keep the existing editor mode as the base:

```sh
ainiux --editor draft.md
ainiux --editor draft.md --provider lmstudio -m MODEL
ainiux lmstudio --editor draft.md
```

Possible later aliases or subcommands:

```sh
ainiux edit draft.md
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

- [x] Send bounded mode-aware context around the cursor (prose: `MAX_CONTINUE_PROSE_PREFIX`/`MAX_CONTINUE_PROSE_POSTFIX`, defaults 16384/4096; code: `MAX_CONTINUE_PREFIX`/`MAX_CONTINUE_POSTFIX`, defaults 4000/2000).
- [x] Bridge prose into immutable text after the cursor; at buffer end, request only new continuation without recap or restart.
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

- [ ] `ainiux --editor FILE` remains usable without any model/network requirement.
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

Make `ainiux` leaner, easier to use daily, and better at model evaluation before starting local server mode in v0.90. This milestone is about quality and maintainability, not new product surfaces.

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
ainiux --benchmark --mode cutoff --provider openai -m MODEL
ainiux --benchmark --dataset builtin --mode cutoff,speed --output results/
ainiux benchmark --mode cutoff --runs 3 --provider lm_studio -m MODEL
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

### Attachment persistence and lifecycle

- [x] Persist full-screen chat images as content-addressed files outside SQLite, with per-message metadata and SHA-256 references in SQLite; hydrate request bytes only inside the cancellable model worker.
- [x] Add `/cleanup`, configurable manual and automatic media expiration, tombstones, and readable-but-locked threads when request-critical managed media is missing.
- [x] Persist the exact bounded UTF-8 context used for TUI text and Markdown attachments. Persist converted Markdown for HTML attachments so follow-up requests and restored threads do not depend on the original path or the one-turn `pending_full_model_content` buffer.
- [x] Add logical canonical-Markdown attachments alongside image attachments. Keep small replay text in SQLite and larger Markdown in the content-addressed media store; do not store raw base64 in SQLite.
- [ ] Generalize the attachment representation further for future binary and provider-native file, PDF, audio, and video parts without making UI code shape provider requests.
- [ ] Add attachment derivatives for future conversions: extracted PDF/DOCX text, audio transcripts, and video transcripts/keyframes. Record source kind/MIME type, converter and version, size/truncation warnings, and whether each object is required for replay.
- [ ] Make cleanup lock a thread only when the removed object is required to reproduce historical model input. Deleting an archival HTML/PDF/DOCX/audio/video original must not lock a thread when its durable converted text, transcript, or other replay representation remains available.
- [ ] Add provider-adapter capability checks and request shaping for future native PDF, file, audio, and video inputs. Unsupported combinations must return `UnsupportedFeature`; portable local extraction/transcription remains the fallback where implemented.
- [ ] Add dedicated `src/pdf/` and `src/word/` conversion modules when PDF and DOCX support is implemented. Start with PDF and DOCX, keep legacy `.doc` and scanned-PDF OCR explicitly unsupported until their own reviewed implementations exist, and justify any parser dependency.
- [ ] Stream future large audio/video imports through bounded hashing and storage jobs rather than loading whole files or base64 payloads into memory.
- [ ] Make JSON chat import/export preserve portable attachment metadata and durable replay content, or report clearly when external managed media is required alongside the JSON file.
- [ ] Test same-process follow-ups, restart/replay, regeneration, context compaction, shared-object cleanup, missing originals with surviving derivatives, cancellation, corrupt media, size limits, and leak-free failure paths for every supported attachment kind.

### Editor TUI

- [x] Share provider/model selector formatting, themed panel rendering, and provider-to-model transition behavior with chat mode.
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

Add a local OpenAI-compatible HTTP server mode before browser web UI or local agent mode. This mode lets other OpenAI-compatible clients, agents, or chat tools call `ainiux` as a local service and gives `ainiux` a place to expose local conversion workflows through a familiar model API.

The browser-based local web UI is postponed. If it is revived later, it should build on the same server/runtime/session/security layers rather than becoming a separate product surface.

Initial server mode is not autonomous agent mode and must not execute shell commands, read arbitrary workspace files, or expose local secrets.

## Command shape

```sh
ainiux --server
ainiux --server 8080
ainiux --server=8080
ainiux --server-host 127.0.0.1 --server 8080
ainiux --server-host 0.0.0.0 --server 8080 --server-allow-lan --server-secret-file secret.txt
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

- [ ] `ainiux --server 8080` starts a loopback-only local OpenAI-compatible server and prints the base URL.
- [ ] `GET /v1/models` returns local conversion pseudo-models.
- [ ] OpenAI-compatible clients can call `html-to-md` and `md-to-html` through Chat Completions.
- [ ] LAN binding requires explicit opt-in and a configured secret.
- [ ] Secrets are not exposed in JSON responses, logs, saved files, or errors.
- [ ] The server remains responsive during a slow request.
- [ ] Leak-check tooling reports no leaks for representative server-mode paths where supported.

---

# v1.0 - Local agent mode

This is the v1.0 plan for ainiux local agent mode (merged from the former standalone agent plan drafts). It covers the agent runtime, project-local code index, tools, safety guard, UI (reusing chat TUI), and mode switching.

**Basis:** agent runtime plan (tools, guard, parallel scheduling, AGENTS.md).  
**Also includes:** project-local SQLite code index, scanners, PageRank, fuzzy edits, preferred tool strategies.  
**Product name:** ainiux throughout.

**Implemented first slice (2026-07-19):** standalone `--index-code`, `--print-index`, and `--clear-index` modes now provide the project-local `.ainiux-pr/index.sqlite` foundation for the editor's complete language set: Markdown, Python, C/C++, C#, Java, JavaScript/JSX, TypeScript/TSX, HTML/HTML-only, CSS, XML, JSON, Bash, PHP, Perl, Ruby, Rust, Go, PowerShell, Assembly, SQL, TOML, YAML, and INI. This includes `.mts`/`.cts`, embedded HTML script/style scanning, structural document/configuration symbols, and physical line totals. The schema stores files, line counts, and symbols only and uses parallel discovery/scanning, lightweight lexical extraction, root ignore files, incremental size/mtime checks, transactional refresh, read-only Markdown reporting, and isolated database removal.

**Implemented read-only review slice (2026-07-19):** `--security-review` refreshes that index, batches every eligible source file, runs bounded parallel native-tool model workers, and serializes one cross-project coordinator before locally rendering source-verified Markdown. Chat Completions/OpenRouter and Responses preserve their native call/result and opaque continuation items. Snapshot-backed read/search/symbol tools and a shell-free allowlisted inspection runner cannot write or escape the workspace; project instruction files remain untrusted data. This slice intentionally has no interactive agent mode, writes, approval UI, `agent.sqlite`, `--plan`, or `--code`. References/FTS/PageRank/call graphs also remain unimplemented.

**Security-review output hardening (2026-07-20):** worker prompts now end with an exact machine-readable coverage manifest and no-findings example, and expose a schema-defined native final-submission tool. Coverage repairs name missing and unexpected paths. Compatible free-form endpoints may wrap one intact JSON object in a short preamble or Markdown fence; normalization extracts that object without rewriting malformed syntax and rejects ambiguous multiple objects.

**Security-review model compatibility (2026-07-20):** omitted or empty optional finding metadata now receives explicit conservative defaults before coordinator review, while evidence location and a title/impact description remain strict. Workers that continue reading receive a round-12 finalization reminder, see only the submission tool from round 16, and retain a bounded 20-round/64-call ceiling.

**Agent write tools slice (2026-07-21):** one-shot `ainiux agent` / `--agent` now enables ordinary workspace mutations on top of the existing read-only tool registry. `write_file` and exact `str_replace` are exposed only when the registry is created with `allow_mutations=true` (agent mode); `--security-review` remains read-only. Writes refuse path escape / protected metadata / symlinks, support `expected_file_hash` stale checks, store pre-overwrite backups under `.ainiux-pr/history/`, and refresh the in-memory snapshot so later reads in the same run succeed. Fuzzy `str_replace`, `edit_file`, `apply_patch`, `remove`, approval UI, and `agent.sqlite` remain unimplemented. Agent mode also falls back to the XML tool channel when the provider/API path lacks native function calling.

**Agent edit_file + AGENTS.md slice (2026-07-21):** agent mode loads workspace-root `AGENTS.md` (UTF-8, byte-capped, non-symlink) into a separate untrusted user message while keeping the system prompt static. Mutation tools add preferred `edit_file` with `replace_range`, `insert_at`, `delete_range`, exact `replace_text` (optional `line_range_hint`), and standalone `create_file`. Multi line-ops apply bottom-to-top in memory before one atomic write. `replace_symbol`, fuzzy matching, nearest nested `AGENTS.md` chain, `apply_patch`, and `remove` remain later work.

**Agent edit engine + AGENTS.md chain slice (2026-07-21):** `load_agents_md_for_path` walks root→nearest nested `AGENTS.md` files under a shared byte budget. `str_replace` and `edit_file.replace_text` use the §13 fallback order (exact → flexible whitespace → indent-stripped) with optional `fuzzy=false`, `line_range_hint`, and `match_mode` reporting. `edit_file.replace_symbol` rewrites an indexed symbol’s line range by `symbol_id`. `remove` deletes files or empty directories (recursive optional) with history backups for UTF-8 text, snapshot updates, and headless denials for database files / symlink trees. In-memory snapshot re-scans symbols after writes. `apply_patch`, `agent.sqlite`, interactive TUI/approvals, and mode cycling remain later work.

**Agent apply_patch + FS visibility polish (2026-07-21):** `apply_patch` accepts OpenAI/Codex `*** Begin Patch` envelopes (`Add File` / `Update File` hunks / `Delete File`), validates all ops then writes with history backups and snapshot refresh, supports `patch`/`input`/`diff` aliases and fuzzy hunk matching, and is mutation-only. `list_directory` uses real readdir (empty dirs, `#names#`); plain `remove` requires `confirm=true` when a `#name#` sibling exists. `agent.sqlite`, interactive TUI/approvals, mode cycling, and broader command guard remain later work.

**Agent session DB slice (2026-07-21):** project-local `.ainiux-pr/agent.sqlite` (WAL) stores sessions, messages, tool_events, and an approvals table stub. One-shot `ainiux agent` opens the DB, creates a running session, appends the goal/tool events/final assistant text (secrets redacted), and finishes with success/error/cancelled/aborted. Never uses `~/.ainiux/ainiux.db`. Interactive TUI agent shell, mode cycling, command guard expansion, and approvals UI remain later work.

**Agent run_command + destructive guard slice (2026-07-21):** agent mode (`allow_mutations`) expands `run_command` beyond the security-review inspection allowlist to include common build/test/run tools (`python3`, `make`, `ctest`, `node`, `go`, `cargo`, compilers, …) still without a shell. A DCG-style `command_guard` denies `rm -rf`, destructive git, destructive SQL, `find -delete`, shell wrappers, and sudo; headless Ask maps to Deny. Security-review remains inspection-only. Interactive approval UI remains later work.

**Interactive agent TUI shell + mode cycle slice (2026-07-21):** `AgentSessionRuntime` prepares index/tools/`agent.sqlite` once and runs multi-turn goals without re-seeding the system prompt each message. One-shot `--run` and interactive `--agent` share that runtime. Interactive agent keeps a warm project session; `/chat`, `/agent`, `/editor`, `/mode`, and `/cycle` switch among chat ↔ editor ↔ agent without process restart while keeping tools disarmed outside agent mode. Interactive approval UI for Guard Ask decisions remains later work.

**Agent project rework slice (2026-07-21):** project-centric agent only (ambiguous parent/nested project markers refused). Project state dir is **`.ainiux-pr/`** (index, agent.sqlite, history, logs); user profile remains **`~/.ainiux/`** (chat DB/media) and is never a project marker. Single project transcript in `agent.sqlite` schema v2. History backups: one slot per path, max 1M, 7-day TTL. Compact tool lines; window-% auto-compact; Chat/Agent chrome; `/cmd-out`. Plan/security agent modes still deferred.

---

## 1. Scope

Implement a fast, dependency-light coding agent in ainiux using the existing C++17 codebase, libcurl, and SQLite3.

Entry points:

```sh
ainiux agent [options]
ainiux --agent [options]
ainiux --chat          # then /agent or mode cycle
ainiux --editor [path] # then /agent or mode cycle
```

Agent **capabilities** (tools, workspace writes, shell) must never silently appear inside ordinary chat, REPL, or editor AI assist. Switching into agent mode is an explicit mode change (CLI flag, `/agent`, or mode cycle).

This plan covers:

- agent loop and provider-neutral tool calls
- **agent UI that reuses the chat TUI** (thread area + bottom input + shared status/selectors)
- **mode cycling and slash mode switches** among chat, editor, and agent
- project-local **`.ainiux-pr/`** store including **`index.sqlite`** and **agent session DB** (user chat remains `~/.ainiux/`)
- SQLite code index, incremental updates, C++17 scanners, ranking
- file/search/edit/web/command/git tools (`glob`, `grep`, and other compatibility aliases)
- file-level content hashes as primary freshness/edit safety
- parallel tool scheduling rules
- DCG-inspired destructive-command guard
- prompt loading and project `AGENTS.md` rules
- rollback, `/undo`, memory, and compaction
- tests, verification, and agent benchmark metrics

### 1.1 Architecture fit (reuse, do not reimplement)

Route long-running work through existing layers:

| Concern | Existing home | Agent rule |
| --- | --- | --- |
| HTTP / SSE | `src/http/` | No second HTTP stack |
| Provider adapters | `src/provider/` | Tool call formats convert at adapter boundary |
| Jobs / cancel | `src/runtime/` | UI/agent loop must not block on network or long I/O |
| URL fetch safety | `src/fetch/` | `fetch_url` reuses private/loopback/metadata policy |
| Web search | `src/search/` | `search_web` reuses configured providers and fallbacks |
| Credential redaction | `src/security/` | Never log API keys or secrets in tool results or session logs |
| Chat TUI layout | `src/tui/` | Agent fullscreen UI reuses chat thread + input panel |
| Editor buffer/input | `src/editor/` | Agent bottom input reuses `EditorState` like chat |
| Provider/model/reasoning UI | shared chat/editor selectors | Same controls in agent mode |
| Mode dispatch | `src/main.cpp` → `src/app/` | Agent runner under `src/app/` + `src/agent/`; mode switch without full process restart where practical |

**Central chat library (not for agent project data):**

```text
~/.ainiux/ainiux.db
```

This database is **only** for the TUI chat library (threads, ordinary chat messages, related app state). It must **not** hold project code indexes, agent transcripts, agent tool logs, or per-project edit history.

**Agent mode is project-based.** All agent-owned durable state lives under the workspace project tree (see §3), so it is portable when the project is copied.

### 1.2 Default safety posture (accepted choice A)

Practical coding-agent defaults for the first serious version:

- Ordinary **reads, searches, skeletons, and index lookups** inside the workspace: allowed without prompting.
- Ordinary **writes and structured edits** inside the workspace: allowed without per-file prompting; always log, hash-check when possible, and record rollback data.
- **Destructive / high-risk** operations: ask or deny via the guard (recursive force delete, destructive git, destructive SQL, database file deletion, workspace escape, indeterminate high-risk wrappers).
- **Web tools** (`fetch_url`, `search_web`): allowed when enabled in settings; still bound by existing fetch/search safety (size, timeout, scheme, private URL policy).
- **Commands** (`run_command`): allowed when not caught as high-risk; maybe-mutating commands (build/test) run without a constant ask, but stay serialized with file mutations.
- Do **not** prompt constantly. Ask only for clearly destructive or indeterminate high-risk actions.
- Fuller sandbox levels (`--sandbox none|basic|strict`, default read-only product posture from older roadmap notes) remain a later hardening track; document them, but do not block the first useful agent on perfect sandboxing.

The agent may request approval. The agent must never approve its own request, disable guard rules, or override the user's direct safety intent.

### 1.3 Agent UI: reuse chat mode aggressively

Do not invent a second full-screen product surface from scratch. Agent mode should feel like chat mode with agent capabilities and project-scoped persistence.

Reuse from chat TUI:

| Chat building block | Agent use |
| --- | --- |
| History / thread area | Show user goals, agent messages, tool calls, tool results, approvals, and status notices |
| Bottom input panel (`EditorState`) | User types goals, follow-ups, slash commands |
| Status line | Workspace root, model, index freshness, running job, guard/approval pending |
| Provider / model / reasoning selectors | Same UX as chat and editor (`/provider`, `/model`, `/reasoning` or existing keybindings) |
| Themes, scroll, resize, non-blocking stream | Same terminal harness and runtime-job event delivery |
| Help / pickers patterns | Adapt rather than reimplement |

Conceptual layout (same as chat):

```text
┌─────────────────────────────────────────────┐
│ status: agent | project | model | index …   │
├─────────────────────────────────────────────┤
│                                             │
│  chat-style thread (agent session)          │
│  - user messages                            │
│  - assistant messages                       │
│  - compact tool call / result blocks        │
│  - approval prompts when guard asks         │
│                                             │
├─────────────────────────────────────────────┤
│ input (EditorState, multiline)              │
└─────────────────────────────────────────────┘
```

UI rules:

- Render tool activity in the thread area as compact, readable events (not only raw JSON dumps). Prefer short summaries with expand-on-demand if cheap later; v1 can show bounded structured text.
- Streaming model text and in-flight tool status must not corrupt the input editor (same rule as chat).
- Long-running work is runtime jobs; user can scroll, edit input, open help/selectors, and cancel.
- Provider, model, and reasoning effort adjustments work the same way as in chat; they affect the agent loop’s next model request.
- Visual distinction from chat is enough if the status line clearly says **agent** and shows the **workspace/project** path — no need for a wholly different chrome.

### 1.4 Mode switching: chat ↔ editor ↔ agent

Users must be able to move among the three primary interactive surfaces without restarting ainiux when already in a full-screen mode.

**Cycle (dedicated chord or slash):**

```text
chat → editor → agent → chat → …
```

Suggested behaviors:

- A single mode-cycle command (implementation may use a keybinding documented in help, plus a slash such as `/mode` or `/cycle`) advances to the next surface in the ring above.
- Direct slash jumps always work when unambiguous:

```text
/chat     switch to chat mode
/editor   switch to editor mode (optional path argument when practical)
/agent    switch to agent mode for the current workspace/project
/mode     show current mode; /mode chat|editor|agent jumps
/cycle    advance chat → editor → agent → chat
```

Examples:

- In chat: type `/agent` → enter agent mode for the current working directory / configured workspace.
- In agent: type `/editor` or `/editor src/main.cpp` → open standalone editor (preserve agent session in project DB; do not tear down project index needlessly).
- In editor: type `/agent` → return to agent thread for this project.
- In agent: type `/chat` → ordinary chat TUI (central chat DB); agent project session remains on disk under `.ainiux-pr/`.

Rules:

- Mode switch is explicit. Entering chat or editor must **not** leave agent tools armed in the background.
- When leaving agent mid-task, cancel or pause in-flight agent jobs cleanly (same cancel semantics as chat generation cancel).
- Preserve per-mode state where practical: chat thread id in central DB; editor buffers/paths; agent session id in **project-local** agent DB.
- CLI still supports direct entry: `--chat`, `--editor`, `agent` / `--agent`.
- Workspace for agent defaults to the current working directory (or `--workspace PATH` when provided). Editor and agent should agree on the project root when switched from each other inside the same process.

### 1.5 Project-based storage (agent’s own DB)

Agent mode works on a **project** (workspace root), not on the global chat library.

| Store | Path | Owns |
| --- | --- | --- |
| Central chat library | `~/.ainiux/ainiux.db` | Ordinary chat threads only (user profile) |
| Project code index | `.ainiux-pr/index.sqlite` | Symbols, refs, FTS, file fingerprints |
| Project agent DB | `.ainiux-pr/agent.sqlite` | Single project transcript, tool events, approvals metadata |
| Session append log | `.ainiux-pr/session.jsonl` | Optional durable event stream / export-friendly log |
| Memory / history / plans | `.ainiux-pr/memory.md`, `history/`, `plans/` | Generated memory, reverse patches, saved plans |

Hard rules:

- Never write agent transcripts or the code index into `~/.ainiux/ainiux.db`.
- Never treat `~/.ainiux/` as a project marker (only `.ainiux-pr/` marks a project).
- Never require a network “account” or home-directory project registry for basic agent use.
- Copying the project directory (including `.ainiux-pr/`) must carry index + agent session data to another machine.
- If `.ainiux-pr/` cannot be created, fail clearly; do not silently fall back to the central chat DB.

The agent UI thread is backed by the **project agent DB** (and/or session log), even though the **widgets** look like chat. Chat mode continues to use the central chat library.

---

## 2. Principles

1. Keep It Simple Stupid.
2. Prefer C++17 standard library and existing dependencies (libcurl, SQLite3).
3. Do not add Tree-sitter, libgit2, MCP, plugins, subagents, or heavyweight frameworks in the first serious version.
4. Optimize for fewer model turns, not only faster local function calls.
5. Prefer batched reads, compact skeletons, indexed symbol lookup, and bounded tool output.
6. Prefer deterministic local tools over model guessing whenever possible.
7. Use common tool names that most LLMs already understand.
8. Use file-level content hashes as the primary freshness and edit-safety mechanism.
9. Keep line-level/hashline anchors as possible future work.
10. Serialize mutating tools that touch the same file or workspace state.
11. Let safe read-only tools run in parallel when possible.
12. Guard destructive commands cheaply with normalization and regex/pattern matching before execution.
13. Treat the SQLite code index as a **fast hint source**, not ground truth.
14. Store tool limits in settings, not as hardcoded constants.
15. Rare or difficult edge cases fall back to slower shell tools (`rg`, `find`, compiler output, `git`).
16. No memory leaks: every tool path, cancel path, and index transaction must release resources.

### 2.1 Non-goals for the first implementation

Deferred:

- Tree-sitter integration
- libgit2 integration
- Full sandboxing / container isolation
- Perfect static analysis
- Perfect C++ parsing
- Full semantic call graph resolution
- MCP support
- SKILLS.md plugin ecosystem
- Heavy permission prompting before every tool call
- Native Anthropic Messages-only tool path as a separate product (provider adapters still translate formats)
- Autonomous multi-agent orchestration

The first version should be pragmatic, fast, and useful on ordinary projects.

---

## 3. Project-local storage layout

Use a hidden **project-local** directory under the workspace root:

```text
.ainiux-pr/
  index.sqlite
  agent.sqlite
  settings.json
  session.jsonl
  memory.md
  history/
  tmp/
  plans/
```

Purposes:

| Path | Purpose |
| --- | --- |
| `index.sqlite` | **Project code index** (WAL). Portable with the project tree when copied to another machine. |
| `agent.sqlite` | **Project agent UI/session DB** (WAL): sessions, messages shown in the thread, tool call/result records, approval outcomes. Not the central chat library. |
| `settings.json` | Project-local agent limits and agent preferences. |
| `session.jsonl` | Append-only event log (debug/export-friendly; complements `agent.sqlite`). |
| `memory.md` | Generated summary of important project/session facts (not sole source of truth). |
| `history/` | Reverse patches, edit snapshots, rollback data. |
| `tmp/` | Trusted workspace temp (guard-allowed temp moves). |
| `plans/` | Saved plans for long-running tasks. |

### 3.1 Hard boundary: no central DB for project agent data

```text
~/.ainiux/ainiux.db        # TUI chat library ONLY (user profile)
.ainiux-pr/index.sqlite    # THIS project's code index
.ainiux-pr/agent.sqlite    # THIS project's agent transcript / tool events
```

Reasons:

- Index and agent history must travel with the project when the tree is copied or shared.
- Different projects must not share or pollute one another’s symbol graphs or agent transcripts.
- Central DB lifecycle (chat threads, media, app state) must stay independent of agent indexing and agent sessions.

If `index.sqlite` or `agent.sqlite` is missing, create it under `.ainiux-pr/` on first agent/index use inside the workspace. If the workspace is not writable, fail with a clear error and do not fall back to `~/.ainiux/`.

### 3.2 Settings and configurable limits

Do not hardcode practical limits (max read size, search hits, command timeout, network timeout, fuzzy thresholds, output caps) inside tool implementations.

Resolution order (most specific wins):

```text
compiled safe defaults
user-global agent settings
project-local .ainiux-pr/settings.json
session overrides from /settings
single tool-call parameter
```

User-global agent settings path (accepted: JSON for agent limits, separate from main TOML-alike `config.conf` for now):

```text
$XDG_CONFIG_HOME/ainiux/agent-settings.json
fallback: ~/.config/ainiux/agent-settings.json
```

Project-local:

```text
.ainiux-pr/settings.json
```

Main ainiux `config.conf` continues to own ordinary product settings (providers, themes, benchmarks). Agent tool limits live in the JSON stack above unless later explicitly mirrored into `config.conf`.

Example settings shape:

```json
{
  "limits": {
    "read_file": {"max_lines": 500, "max_bytes": 50000},
    "read_many": {"max_total_bytes": 80000},
    "get_skeleton": {"max_items": 300, "max_bytes": 40000},
    "glob": {"max_results": 500},
    "search_text": {"max_hits": 80, "max_bytes": 60000},
    "search_symbol": {"max_results": 20},
    "read_symbol": {"max_bytes": 50000},
    "project_overview": {"max_files": 30, "max_symbols": 30},
    "list_directory": {"max_entries": 300},
    "git_diff": {"max_bytes": 50000},
    "run_command": {"timeout_ms": 120000, "max_output_bytes": 12000},
    "fetch_url": {"timeout_ms": 30000, "max_bytes": 200000},
    "search_web": {"max_results": 10, "timeout_ms": 30000}
  },
  "hash": {
    "file_hash_algorithm": "fnv1a64",
    "include_size_in_fingerprint": true
  },
  "index": {
    "sqlite_cache_size_kb": 200000,
    "sqlite_mmap_size": 268435456,
    "use_fts5": true,
    "use_fts5_trigram_for_partial_search": true
  },
  "parallel": {
    "max_read_tools": 8,
    "max_network_tools": 3,
    "serialize_workspace_mutations": true
  },
  "editing": {
    "fuzzy_edit": true,
    "require_unique_match_by_default": true,
    "max_fuzzy_candidates": 20
  },
  "guard": {
    "enabled": true,
    "ask_on_destructive_git": true,
    "ask_on_recursive_force_delete": true,
    "ask_on_database_delete": true,
    "ask_on_destructive_sql": true,
    "forbid_workspace_escape": true
  },
  "web": {
    "enabled": true,
    "search_provider": "configured",
    "user_agent": "ainiux/agent"
  },
  "network": {
    "enabled": true
  },
  "agents_md": {
    "enabled": true,
    "load_root": true,
    "load_nearest_for_files": true,
    "max_bytes_total": 20000
  }
}
```

Numeric values are suggested defaults only. Implementation must read them from settings.

When output is truncated, return:

- `truncated = true`
- which setting or tool parameter caused truncation
- how much was omitted if known
- the most useful head/tail portions where appropriate

For command output, prefer compiler/test error extraction over dumping massive logs.

---

## 4. File hashes and freshness

Use file-level content hashes as the primary safety and freshness mechanism.

Recommended first version:

- store `size`, `mtime_ns`, and `file_hash`
- skip hashing if size and timestamp are unchanged
- hash only changed files
- reindex only touched files after ainiux edits
- include `file_hash` in read/edit results
- require or recommend `expected_file_hash` for overwrite-like operations

The hash is a fast content fingerprint for stale-context detection, not a security primitive. A simple in-tree implementation such as FNV-1a 64-bit is acceptable for v1. If collision risk becomes a practical concern, move to a stronger built-in implementation later without changing the tool API.

Line-level hashes are postponed. Range hashes may still be returned by `read_file`, `read_many`, and `read_symbol` because they are useful for `replace_range`, but the main freshness check remains the file hash.

Suggested file fingerprint output:

```json
{
  "path": "src/agent.cpp",
  "size": 38211,
  "mtime_ns": 1783170000123456789,
  "file_hash": "fnv1a64:8f72c1e3aa010c91"
}
```

---

## 5. SQLite index design

Store the index at:

```text
.ainiux-pr/index.sqlite
```

Use SQLite3 with WAL mode enabled.

Recommended pragmas (cache/mmap sizes configurable via settings):

```sql
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA temp_store=MEMORY;
PRAGMA cache_size=-200000;
PRAGMA mmap_size=268435456;
```

### 5.1 Files table

```sql
CREATE TABLE IF NOT EXISTS files (
    id INTEGER PRIMARY KEY,
    path TEXT NOT NULL UNIQUE,
    language TEXT NOT NULL,
    size INTEGER NOT NULL,
    mtime_ns INTEGER NOT NULL,
    hash TEXT NOT NULL,
    indexed_at INTEGER NOT NULL,
    ignored INTEGER NOT NULL DEFAULT 0,
    last_error TEXT
);
```

### 5.2 Symbols table

```sql
CREATE TABLE IF NOT EXISTS symbols (
    id INTEGER PRIMARY KEY,
    file_id INTEGER NOT NULL REFERENCES files(id),
    language TEXT NOT NULL,
    kind TEXT NOT NULL,
    name TEXT NOT NULL,
    qualified_name TEXT NOT NULL,
    parameters TEXT,
    return_type TEXT,
    start_line INTEGER NOT NULL,
    end_line INTEGER NOT NULL,
    signature_hash TEXT NOT NULL,
    body_hash TEXT,
    doc TEXT,
    doc_source TEXT NOT NULL,
    pagerank REAL NOT NULL DEFAULT 0.0
);
```

`kind` examples:

```text
function
method
class
struct
enum
interface
module
namespace
constructor
destructor
macro
constant
variable
```

`doc_source` examples:

```text
comment
docstring
generated
heuristic
none
```

The first implementation must not run LLM summarization during full indexing. Use existing comments and cheap heuristics only. Optional model-generated summaries can be created lazily later and cached by `body_hash`.

### 5.3 References table

```sql
CREATE TABLE IF NOT EXISTS refs (
    id INTEGER PRIMARY KEY,
    file_id INTEGER NOT NULL REFERENCES files(id),
    from_symbol_id INTEGER REFERENCES symbols(id),
    target_symbol_id INTEGER REFERENCES symbols(id),
    target_name TEXT NOT NULL,
    kind TEXT NOT NULL,
    line INTEGER NOT NULL,
    confidence REAL NOT NULL
);
```

`kind` examples:

```text
call
import
include
inherit
instantiate
macro
use
```

Treat the call graph as probabilistic. Store confidence values rather than pretending every reference is perfect.

Example confidence levels:

```text
1.00 exact resolved symbol
0.80 same file unique matching name
0.70 imported or included name
0.50 likely class or object method call
0.30 lexical call-like token
```

### 5.4 FTS tables

Use SQLite FTS5 when available:

```sql
CREATE VIRTUAL TABLE IF NOT EXISTS symbols_fts USING fts5(
    qualified_name,
    name,
    parameters,
    doc,
    content='symbols',
    content_rowid='id'
);
```

When fast case-insensitive partial search is needed inside the DB, consider an additional FTS5 trigram table:

```sql
CREATE VIRTUAL TABLE IF NOT EXISTS code_search_fts USING fts5(
    path,
    symbol_name,
    text,
    tokenize='trigram'
);
```

Use FTS5 only if the linked SQLite build supports it and the required tokenizer. If unavailable, fall back to indexed `LIKE`, path/name filters, and the internal C++ scanner / `search_text`.

### 5.5 Useful indexes

```sql
CREATE INDEX IF NOT EXISTS idx_files_path ON files(path);
CREATE INDEX IF NOT EXISTS idx_files_hash ON files(hash);
CREATE INDEX IF NOT EXISTS idx_symbols_file ON symbols(file_id);
CREATE INDEX IF NOT EXISTS idx_symbols_name ON symbols(name);
CREATE INDEX IF NOT EXISTS idx_symbols_qname ON symbols(qualified_name);
CREATE INDEX IF NOT EXISTS idx_symbols_kind ON symbols(kind);
CREATE INDEX IF NOT EXISTS idx_refs_from ON refs(from_symbol_id);
CREATE INDEX IF NOT EXISTS idx_refs_target ON refs(target_symbol_id);
CREATE INDEX IF NOT EXISTS idx_refs_name ON refs(target_name);
```

---

## 6. Index update strategy

Update the index automatically for files ainiux edits. Manual full rebuild exists for recovery; normal users should rarely need it.

### 6.1 Fast change detection

For each file, track path, size, mtime (highest available precision), and content hash.

Normal update path:

1. Compare stored size and timestamp.
2. If unchanged, skip hashing.
3. If changed, compute hash.
4. If hash is unchanged, update metadata only if needed.
5. If hash changed, rescan only that file.
6. Delete and replace symbols/refs for that file in one transaction.
7. Refresh FTS rows for changed symbols.

For files edited through ainiux, reindex those files immediately.

For files changed externally, detect changes during:

- agent startup
- `/status`
- `/index status`
- before `search_symbol`
- before `read_symbol`
- after `run_command` if the command may have generated or modified source files

The check should be cheap. Ordinary projects should get sub-second incremental updates.

### 6.2 Rebuild and status commands

```text
/index status
/index update
/index rebuild
/index explain PATH
```

Expected behavior:

- `/index status`: freshness, file count, symbol count, changed files
- `/index update`: update changed files only
- `/index rebuild`: delete and rebuild the entire project index
- `/index explain PATH`: why a file is indexed, ignored, or failed

Corresponding tools: `index_status`, `index_update`, `index_rebuild`.

---

## 7. C++17 pattern matching scanner

The first scanner uses fast C++17 code with regular expressions and lightweight lexical scanning. The goal is the common 80%+ of symbols and references, not perfect parsing.

Priority languages and file types:

```text
C
C++
C#
Python
JavaScript
TypeScript
Java
PHP
Perl
HTML5
CSS
Rust
Go
GNU-style assembler
```

HTML5 indexing should detect embedded JavaScript inside `<script>` blocks and, when practical, embedded CSS inside `<style>` blocks. GNU assembler assumes GAS-style syntax and common extensions (`.s`, `.S`, `.asm`) where project convention is clear.

### 7.1 General scanner rules

Each language scanner should extract:

- module/package/namespace names
- class/struct/interface names
- function/method names
- constructor names
- parameters
- return type when easy
- start and end line
- nearby doc comments/docstrings
- imports/includes/uses
- likely function calls
- HTML elements with ids/classes when useful for web tasks
- CSS selectors and rule blocks
- assembler labels, directives, global symbols, and call/jump references

Use `std::regex` for common signatures and lightweight brace/indent/tag tracking for symbol ranges. Never block ordinary editing for long.

### 7.2 Comment extraction

Extract nearby comments directly above a symbol.

Examples:

- C/C++/Java/C#/JavaScript/TypeScript/PHP/Rust/Go/CSS: `//`, `/* ... */`, `/** ... */` where supported
- Python: triple-quoted docstrings and preceding `#` comments
- Perl: preceding `#` comments
- HTML5: `<!-- ... -->`, plus comments inside embedded script/style blocks
- GNU assembler: preceding `#`, `//`, or `/* ... */` depending on source convention

Be conservative: do not attach unrelated comments separated by blank lines or unrelated code.

### 7.3 Cheap heuristic docs

If no doc comment exists, store `doc_source = none` or a cheap heuristic description, for example:

```text
C++ function returning bool. Calls parse_args, load_config, run_chat_loop.
```

Do not run LLM summarization during initial indexing.

### 7.4 Function end detection

Language-specific approaches:

- Brace languages: count braces while ignoring strings and comments as much as practical
- Python: indentation-based range detection
- Perl/PHP: brace counting with package/function awareness
- Rust/Go: brace counting plus regexes for common declarations
- HTML5: tag/block tracking; embedded JS/CSS delegated to their scanners
- CSS: selector/rule block tracking
- GNU assembler: labels and directives; ranges end at the next label or section directive

Missing rare cases is acceptable. Fallback tools recover. The scanner is a fast hint generator, not an authority.

---

## 8. PageRank and ranking

Calculate approximate PageRank over symbols and files using the reference graph.

Useful edges:

- symbol calls symbol
- file includes/imports file
- class owns method
- test calls production symbol
- entry point calls function

Search ranking should combine:

```text
text match score
symbol name match
path match
language match
PageRank
recently edited/touched boost
active task boost
```

Approximate formula (tunable via benchmark mode):

```text
score = text_match
      + 0.25 * normalized_pagerank
      + 0.20 * path_match
      + 0.15 * recently_touched
      + 0.10 * same_language_as_active_file
```

---

## 9. Suggested implementation files

Adapt names if equivalent modules already exist. Do not duplicate functionality.

```text
src/agent/agent_loop.hpp
src/agent/agent_loop.cpp
src/agent/tool_call.hpp
src/agent/tool_registry.hpp
src/agent/tool_registry.cpp
src/agent/tool_scheduler.hpp
src/agent/tool_scheduler.cpp
src/agent/tools_files.hpp
src/agent/tools_files.cpp
src/agent/tools_search.hpp
src/agent/tools_search.cpp
src/agent/tools_edit.hpp
src/agent/tools_edit.cpp
src/agent/tools_command.hpp
src/agent/tools_command.cpp
src/agent/tools_web.hpp
src/agent/tools_web.cpp
src/agent/git_tools.hpp
src/agent/git_tools.cpp
src/agent/command_guard.hpp
src/agent/command_guard.cpp
src/agent/agent_prompts.hpp
src/agent/agent_prompts.cpp
src/agent/agents_md.hpp
src/agent/agents_md.cpp
src/agent/agent_memory.hpp
src/agent/agent_memory.cpp
src/agent/context_compact.hpp
src/agent/context_compact.cpp
src/agent/hash.hpp
src/agent/hash.cpp
src/agent/index/index_db.hpp
src/agent/index/index_db.cpp
src/agent/index/index_update.hpp
src/agent/index/index_update.cpp
src/agent/index/scanner.hpp
src/agent/index/scanner.cpp
src/agent/index/scanners/          # per-language lightweight scanners
src/agent/index/rank.hpp
src/agent/index/rank.cpp
```

Suggested tests:

```text
tests/unit/agent/test_tool_registry.cpp
tests/unit/agent/test_file_tools.cpp
tests/unit/agent/test_edit_tools.cpp
tests/unit/agent/test_str_replace_fuzzy.cpp
tests/unit/agent/test_apply_patch.cpp
tests/unit/agent/test_command_guard.cpp
tests/unit/agent/test_tool_scheduler.cpp
tests/unit/agent/test_agents_md.cpp
tests/unit/agent/test_memory.cpp
tests/unit/agent/test_web_tools.cpp
tests/unit/agent/test_index_db.cpp
tests/unit/agent/test_scanner.cpp
tests/unit/agent/test_hash.cpp
```

Built-in prompt resources (short, task-specific):

```text
resources/agents/AGENTS.base.md
resources/agents/AGENTS.coding.md
resources/agents/AGENTS.debug.md
resources/agents/AGENTS.review.md
resources/agents/AGENTS.refactor.md
resources/agents/AGENTS.tests.md
```

(If the repo prefers `config/` or `docs/` install paths, place installable copies consistently with other bundled templates; keep source of truth next to the agent module or under `resources/agents/`.)

---

## 10. Provider-neutral tool representation

Use one internal representation; convert to OpenAI-compatible, Anthropic-style, Gemini-style, and plain text fallback formats at the adapter boundary.

```cpp
struct ToolCall {
    std::string id;
    std::string name;
    Json args;
};

struct ToolError {
    std::string code;
    std::string message;
    std::string rule_id;
};

struct ToolResult {
    std::string id;
    bool ok;
    Json data;
    std::vector<std::string> warnings;
    bool truncated = false;
    Json metadata;
    std::optional<ToolError> error;
};
```

Common compact JSON shape:

```json
{
  "ok": true,
  "error": null,
  "data": {},
  "warnings": [],
  "truncated": false,
  "metadata": {}
}
```

Failure shape:

```json
{
  "ok": false,
  "error": {
    "code": "stale_file",
    "message": "The file changed since it was read.",
    "rule_id": null
  },
  "data": {},
  "warnings": [],
  "truncated": false,
  "metadata": {}
}
```

All `max_*`, timeout, candidate-count, and truncation defaults are settings values, not hardcoded constants. If a tool parameter is `null` or omitted, resolve through the settings stack.

### 10.1 Tool-call reliability (detailed plan)

This subsection is the merged tool-call reliability addendum for v1.0 local agent mode. It specifies how model tool calls are accepted, parsed, retried (transport only), bounded, and kept history-safe. Tool execution still must pass the sandbox/approval layer required by the rest of this milestone.

### 10.2 Two tool-call channels, one argument format

- Prefer the provider-native `tool_calls` field of Chat Completions whenever the model/provider supports it. It is the fastest and least ambiguous path.
- For models that lack native tool calling or use it unreliably, use the XML-alike text protocol already proven in the AI editor. Tool arguments are always a single JSON object, in both channels, so exactly one argument parser exists.

Native channel (preferred):

```json
{
  "tool_calls": [
    {
      "id": "call_1",
      "type": "function",
      "function": {
        "name": "read_file",
        "arguments": "{\"path\": \"src/main.cpp\", \"max_bytes\": 65536}"
      }
    }
  ]
}
```

XML-alike fallback channel (arguments stay JSON):

```xml
<tool_call>
<name>read_file</name>
<args>{"path": "src/main.cpp", "max_bytes": 65536}</args>
</tool_call>
```

- One tool call per assistant turn in the XML channel. Reject and report multiple `<tool_call>` blocks with a tool-result error asking for one call at a time.
- Channel selection is per model: the capability catalog (`models.conf`) may record `tool_protocol = native|xml`. Default is `native` when the provider advertises tool support, else `xml`.
- Automatic downgrade: if a model on the native channel emits `<tool_call>` markup inside ordinary assistant text in 2 consecutive turns, switch that session to the XML channel and note it on `stderr`/status. Never upgrade automatically mid-session.

(The older attribute-style sketch `<tool_call name="...">` with a bare JSON body is superseded by the named-child form above so name and args parsing stay uniform and less ambiguous.)

### 10.3 Argument parsing pipeline

Run the same lenient-but-bounded pipeline on the arguments string from either channel. Each stage is cheap, single-pass, and deterministic. Stop at the first stage that yields a valid object.

```text
1. Trim leading/trailing whitespace, strip UTF-8 BOM, strip surrounding
   Markdown code fences (```json ... ```).
2. Empty or whitespace-only arguments -> treat as {} (tools with no
   required parameters are common and models often send "").
3. Strict JSON parse. Success -> done.
4. Concatenated-object extraction: if the text contains exactly one
   balanced top-level {...} object with junk before/after (preamble,
   trailing prose, a second partial object), extract and parse it.
5. One-pass JSON repair: fix single quotes, trailing commas, unquoted
   keys, unescaped newlines inside strings. One repair pass only; no
   iterative fix-up loops.
6. Schema-aware coercion: "true"/"false" -> bool, numeric strings ->
   numbers, scalar -> single-element array, where the tool's declared
   parameter schema expects it. Never invent values for missing
   required fields.
7. Tool-name repair: exact match first; then case-insensitive match;
   then snake_case/camelCase normalization against the registry. No
   fuzzy/edit-distance matching.
8. Still invalid -> do NOT retry the request. Return a rich error as
   the tool result and let the model correct itself.
```

Rich error tool-result format (bounded):

```json
{
  "error": "invalid_arguments",
  "tool": "read_file",
  "message": "Expected JSON object with required field \"path\" (string).",
  "received_arguments": "<original text, truncated to 2000 bytes>"
}
```

Include the original arguments so the model can see exactly what it sent; cap them at 2000 bytes so a runaway generation cannot bloat context. Map this into the common `ToolResult` failure shape (`ok: false`, `error.code` / `error.message`, plus data carrying the truncated original) so UI and session logs stay consistent.

### 10.4 Retry budgets

Keep two completely separate budgets. Do not blend them.

```text
transport/sampling retries   3 attempts total per request
                             - retry only on: timeout, connection reset,
                               HTTP 429/500/502/503/529, malformed SSE stream
                             - backoff: 1s, 2s, 4s (+ jitter); honor
                               Retry-After when present
                             - fail IMMEDIATELY (0 retries) on: HTTP 400,
                               401, 403, 404, context-length-exceeded --
                               deterministic failures never get retried

tool re-execution retries    0. Never automatically re-run a failed tool.
                             The failure (non-zero exit, exception, missing
                             file, denied approval) becomes the tool result
                             and the model decides the next step.
```

Failing deterministic errors instantly, instead of burning a backoff cycle on a 401, is a significant part of keeping the loop fast.

### 10.5 Turn and loop limits (fallback ladder)

Small fixed numbers, escalating from a nudge to a stop:

```text
identical tool call repeated 3x   soft: inject a system-role notice into the
(same name + same normalized      next request: "You have repeated the same
args)                             call 3 times with the same result. Try a
                                  different approach or explain the blocker."

identical tool call repeated 5x   hard: abort the agent task with a clear
                                  error naming the looping tool call.

3 consecutive turns where every   abort with a summary of the failing calls.
tool call failed

50 agent turns (scripted/non-     hard cap; interactive TUI mode instead
interactive mode)                 asks the user whether to continue.
```

### 10.6 Conversation-history hygiene

Two cheap invariants prevent permanently bricked sessions:

- Never re-send known-invalid JSON arguments to the provider. When storing an assistant turn whose arguments failed parsing, persist the original text locally for the error tool-result, but serialize `"{}"` as the arguments in subsequent provider requests. Some providers hard-reject a history containing malformed `arguments`, which would otherwise poison every following request.
- Every `tool_call` id in history must have a matching tool-result message. On cancellation, abort, or crash-resume, pair any dangling call with a synthetic result: `{"error":"cancelled","message":"Tool was not executed."}` before the next request is sent.

### 10.7 Prompting rules (system prompt for agent mode)

- State the protocol once, precisely, with one complete example of a correct call and one example of a correct reaction to an error tool-result.
- XML channel: instruct "Emit exactly one <tool_call> block, nothing after it. Arguments must be one valid JSON object. Do not wrap the block in code fences."
- Native channel: instruct "Use the provided tools; never describe a tool call in prose or invent XML tags."
- Keep tool descriptions short and imperative; put parameter constraints in the JSON schema, not in prose.
- Tell the model errors come back as tool results and that it should correct and continue, not apologize or restart.
- Keep the agent system prompt static per session so provider-side prompt caching works; per-turn state (loop notices, budget warnings) is injected as separate messages, never edited into the system prompt.

### 10.8 Explicit non-goals (tool-call reliability)

- No fuzzy/edit-distance tool-name matching.
- No scraping JSON out of prose on the native channel.
- No silent default values for missing required arguments.
- No multi-pass or LLM-assisted JSON repair.
- No automatic tool re-execution, ever.
- No streaming partial tool execution (execute only after the turn is complete and parsed).

### 10.9 Tool-call reliability implementation steps

- [ ] Inspect existing provider/runtime/editor XML-assist code and record reusable pieces.
- [ ] Add `src/agent/` tool registry with JSON-schema parameter declarations.
- [ ] Add native `tool_calls` request generation and response parsing in `src/provider/`.
- [ ] Add the XML-alike channel parser reusing editor streaming-tag experience.
- [ ] Implement the 8-stage argument parsing pipeline as pure, unit-testable functions.
- [ ] Implement rich error tool-results with the 2000-byte original-arguments cap.
- [ ] Implement transport retry budget (3, backoff+jitter, deterministic-fail list) in the shared HTTP path without affecting chat mode defaults.
- [ ] Implement loop detection (3 soft / 5 hard), consecutive-failure abort, and turn caps.
- [ ] Implement history hygiene: `"{}"` serialization for invalid args and synthetic results for dangling calls, including cancellation paths.
- [ ] Add `tool_protocol` to the model capability catalog and the 2-strike native->XML downgrade.
- [ ] Write the agent system prompt per the prompting rules; keep it static per session.
- [ ] Add malformed-transcript fixtures: preamble junk, single quotes, trailing commas, empty args, concatenated objects, wrong-case names, dangling calls, repeated-call loops.
- [ ] Unit, integration (mock server), sanitizer, and leak tests for success, malformed, cancelled, and aborted agent runs.
- [ ] Update `README.md`, `docs/decisions.md`, `docs/security.md`, `TODO.md`.

### 10.10 Tool-call reliability acceptance criteria

- [ ] A model with native tool support completes a multi-step agent task using `tool_calls`; a weak local model completes the same task on the XML channel with unchanged tool implementations.
- [ ] Every stage of the argument pipeline is unit-tested, including empty args -> `{}`, fenced JSON, single-quote repair, concatenated-object extraction, schema coercion, and case-repaired names.
- [ ] Invalid arguments produce a rich error tool-result containing the truncated original text, and the provider request history never contains the invalid JSON.
- [ ] HTTP 401/400 fail instantly with no retry; HTTP 429/5xx retry at most 3 times with backoff and honor `Retry-After`.
- [ ] A failed tool execution is never automatically re-run; the failure appears as a tool result and the loop continues.
- [ ] The 3x soft notice and 5x hard abort for repeated identical calls, the 3-consecutive-failure abort, and the 50-turn scripted cap are all covered by tests.
- [ ] Cancellation mid-tool-call leaves the thread resumable: dangling calls are paired with synthetic results and the next request succeeds against the mock server.
- [ ] Two consecutive turns of leaked `<tool_call>` markup on the native channel downgrade the session to XML with a visible notice.
- [ ] No tool executes without passing the sandbox/approval layer required by the v1.0 milestone.
- [ ] Sanitizer and leak checks pass for successful, malformed-argument, cancelled, and aborted agent runs where tooling is available.

---

## 11. Built-in tool list

Prefer short, familiar tool names. Some tools are aliases for model compatibility.

### 11.1 Context and discovery

```text
project_overview
inspect_code_task
list_directory
glob
search_text
grep
find
search_symbol
get_skeleton
read_symbol
read_file
read_many
find_callers
find_callees
find_tests
```

### 11.2 File and edit

```text
write_file
remove
edit_file
str_replace
apply_patch
```

Optional provider-facing alias:

```text
delete_file -> remove
```

### 11.3 Command, git, and web

```text
run_command
git_status
git_diff
fetch_url
search_web
```

### 11.4 Index

```text
index_status
index_update
index_rebuild
```

Normal operation uses automatic per-file updates and `index_update`. `index_rebuild` is for recovery/debugging.

---

## 12. Tool specifications

### 12.1 `project_overview`

Purpose: compact project map.

Parameters:

```json
{
  "max_files": "integer|null",
  "max_symbols": "integer|null",
  "include_tests": "boolean"
}
```

Return data:

```json
{
  "root": "/path/to/project",
  "languages": [{"language":"cpp","files":120,"bytes":1800000}],
  "important_files": [{"path":"src/main.cpp","score":0.92}],
  "entry_points": [{"name":"main","path":"src/main.cpp","line":35}],
  "likely_test_commands": ["make test"],
  "index_fresh": true
}
```

### 12.2 `inspect_code_task`

Purpose: macro-tool that reduces turns by returning likely files, symbols, tests, and suggested reads for a task. Internally may combine symbol search, text search, PageRank, recent files, and path heuristics.

Parameters:

```json
{
  "query": "string",
  "max_symbols": "integer|null",
  "max_files": "integer|null",
  "include_skeletons": "boolean",
  "include_tests": "boolean",
  "max_bytes": "integer|null"
}
```

Return data:

```json
{
  "query": "add agent mode",
  "likely_symbols": [
    {"symbol_id":44,"qualified_name":"ChatSession::run","path":"src/chat.cpp","start_line":80,"end_line":210,"reason":"main chat loop"}
  ],
  "likely_files": ["src/chat.cpp","src/openai.cpp"],
  "suggested_reads": [{"path":"src/chat.cpp","start_line":80,"end_line":210}],
  "skeletons": [],
  "likely_tests": ["tests/test_chat.cpp"],
  "truncated": false
}
```

### 12.3 `list_directory`

Purpose: list files and directories.

Parameters:

```json
{
  "path": "string",
  "recursive": "boolean",
  "max_depth": "integer|null",
  "include_hidden": "boolean",
  "include_ignored": "boolean",
  "max_entries": "integer|null"
}
```

Suggested defaults: `recursive: false`, `max_depth: 1`, `include_hidden: false`, `include_ignored: false`, `max_entries: 300`.

Return data:

```json
{
  "path": ".",
  "entries": [
    {"path":"src","type":"directory"},
    {"path":"src/main.cpp","type":"file","size":12345}
  ],
  "truncated": false
}
```

Notes: respect `.gitignore` via `git ls-files` when inside a git repository; fall back to filesystem traversal otherwise.

### 12.4 `glob`

Purpose: path discovery by pattern (filenames/paths, not file content).

Parameters:

```json
{
  "pattern": "string",
  "root": "string|null",
  "include_hidden": "boolean",
  "include_ignored": "boolean",
  "max_results": "integer|null"
}
```

Examples:

```json
{"pattern":"**/*test*.cpp","root":"."}
{"pattern":"**/CMakeLists.txt","root":"."}
{"pattern":"src/**/*.{cpp,h}","root":"."}
```

Return data:

```json
{
  "root": ".",
  "pattern": "**/*test*.cpp",
  "matches": ["tests/test_agent.cpp", "tests/test_tools.cpp"],
  "match_count": 2,
  "truncated": false
}
```

Implementation: C++ filesystem + simple glob matching; keep separate from `search_text` so path discovery does not waste tokens on content search.

### 12.5 `search_text`

Purpose: search file contents by literal text or regex.

Parameters:

```json
{
  "query": "string",
  "path": "string|null",
  "glob": "string|null",
  "regex": "boolean",
  "case_sensitive": "boolean",
  "whole_word": "boolean",
  "context_lines": "integer",
  "max_hits": "integer|null",
  "max_bytes": "integer|null"
}
```

Return data:

```json
{
  "hits": [
    {"path":"src/main.cpp","line":42,"column":13,"match":"Agent::run","context":"40| ...\n41| ...\n42| ..."}
  ],
  "hit_count": 12,
  "truncated": false
}
```

Implementation: fast internal literal search; `std::regex` for regex mode; optional `rg` fallback when available; optionally SQLite FTS5 for DB-backed partial search.

### 12.6 `grep`

Purpose: compatibility alias for content search (many models expect `grep`).

Parameters:

```json
{
  "pattern": "string",
  "path": "string|null",
  "glob": "string|null",
  "regex": "boolean",
  "case_sensitive": "boolean",
  "context_lines": "integer",
  "max_hits": "integer|null"
}
```

Return: same shape as `search_text`. Dispatch internally to `search_text`. Do not call shell `grep` unless falling back through `run_command`.

### 12.7 `find`

Purpose: simple compatibility alias for literal text search.

Parameters:

```json
{
  "path": "string",
  "search_string": "string",
  "max_hits": "integer|null"
}
```

Return: same shape as `search_text`. Internally dispatch with `regex = false`.

### 12.8 `search_symbol`

Purpose: search the SQLite symbol/code index in `.ainiux-pr/index.sqlite`.

Parameters:

```json
{
  "query": "string",
  "kind": "string|null",
  "language": "string|null",
  "path_glob": "string|null",
  "max_results": "integer|null",
  "include_docs": "boolean",
  "include_call_summary": "boolean"
}
```

Return data:

```json
{
  "results": [
    {
      "symbol_id": 184,
      "score": 0.92,
      "pagerank": 0.81,
      "kind": "method",
      "qualified_name": "Agent::run",
      "parameters": "const AgentTask& task",
      "return_type": "bool",
      "path": "src/agent.cpp",
      "start_line": 42,
      "end_line": 118,
      "doc": "Runs the agent loop.",
      "calls": ["read_file", "edit_file", "run_command"]
    }
  ],
  "index_fresh": true,
  "truncated": false
}
```

**Important:** the scanner/index is a hint, not truth. Verify with `read_symbol`, `get_skeleton`, `read_many`, `read_file`, `search_text`, `glob`, `grep`, or compiler/tests when needed. Cheaply update changed files before searching.

### 12.9 `get_skeleton`

Purpose: signatures, declarations, and doc comments for a file — primary token-saving tool.

Parameters:

```json
{
  "path": "string",
  "include_private": "boolean",
  "include_line_numbers": "boolean",
  "include_doc_comments": "boolean",
  "include_fields": "boolean",
  "max_items": "integer|null",
  "max_bytes": "integer|null"
}
```

Return data:

```json
{
  "path": "src/agent.cpp",
  "language": "cpp",
  "file_hash": "fnv1a64:...",
  "symbols": [
    {
      "symbol_id": 184,
      "kind": "method",
      "qualified_name": "Agent::run",
      "signature": "bool Agent::run(const AgentTask& task)",
      "line": 42,
      "end_line": 118,
      "doc": "Runs the agent loop until completion or interruption.",
      "doc_source": "comment"
    }
  ],
  "truncated": false
}
```

Use the SQLite index when fresh; if missing or stale, quickly rescan only that file. If the skeleton looks incomplete, stale, or contradictory, fall back to reads/searches — the model must not blindly trust skeleton data.

### 12.10 `read_symbol`

Purpose: read an indexed symbol body and nearby context.

Parameters:

```json
{
  "symbol_id": "integer",
  "include_doc": "boolean",
  "include_callers": "boolean",
  "include_callees": "boolean",
  "include_siblings": "boolean",
  "context_lines": "integer",
  "max_bytes": "integer|null"
}
```

Return data:

```json
{
  "symbol": {
    "symbol_id": 184,
    "qualified_name": "Agent::run",
    "kind": "method",
    "path": "src/agent.cpp",
    "start_line": 42,
    "end_line": 118,
    "file_hash": "fnv1a64:...",
    "range_hash": "fnv1a64:...",
    "content": "37| ...\n42| bool Agent::run(...) {\n..."
  },
  "callers": [],
  "callees": [],
  "truncated": false
}
```

If symbol location is stale, reindex the file once and retry. If still inconsistent, return an error and suggest `search_text` or `read_file`.

### 12.11 `read_file`

Purpose: read a whole file or line range.

Parameters:

```json
{
  "path": "string",
  "start_line": "integer|null",
  "end_line": "integer|null",
  "max_bytes": "integer|null",
  "max_lines": "integer|null",
  "include_line_numbers": "boolean",
  "include_hashes": "boolean"
}
```

Suggested defaults: `max_bytes: 50000`, `max_lines: 500`, `include_line_numbers: true`, `include_hashes: true`.

Return data:

```json
{
  "path": "src/example.cpp",
  "language": "cpp",
  "file_hash": "fnv1a64:...",
  "range": {"start_line": 120, "end_line": 145},
  "range_hash": "fnv1a64:...",
  "content": "120| int f() {\n121| ...\n",
  "line_count": 300,
  "truncated": false
}
```

Refuse binary files by default. Include `range_hash` when line ranges are returned.

### 12.12 `read_many`

Purpose: batch several reads into one tool call.

Parameters:

```json
{
  "items": [
    {"path": "string", "start_line": "integer|null", "end_line": "integer|null"}
  ],
  "max_total_bytes": "integer|null",
  "include_line_numbers": "boolean",
  "include_hashes": "boolean"
}
```

Prefer this over repeated `read_file` calls. If the output cap is reached, include complete earlier items and mark later items omitted or truncated.

### 12.13 `write_file`

Purpose: create or overwrite a file.

Parameters:

```json
{
  "path": "string",
  "content": "string",
  "create_dirs": "boolean",
  "expected_file_hash": "string|null",
  "mode": "overwrite|create_new"
}
```

Return data:

```json
{
  "path": "src/new_file.cpp",
  "bytes_written": 1234,
  "new_file_hash": "fnv1a64:...",
  "created": true,
  "guard": {"decision":"allow","rule_id":null},
  "indexed": true
}
```

Rules:

- Canonicalize path; refuse writes outside workspace root or trusted temp directories.
- If `mode = create_new`, fail if the file exists.
- If `expected_file_hash` is supplied and does not match, fail with `stale_file`.
- Record rollback data before overwriting.
- Reindex the affected file immediately.

### 12.14 `remove`

Purpose: remove a file or empty directory.

Parameters:

```json
{
  "path": "string",
  "recursive": "boolean",
  "expected_file_hash": "string|null"
}
```

Return data:

```json
{
  "path": "src/old.cpp",
  "removed": true,
  "was_directory": false,
  "guard": {"decision":"allow","rule_id":null},
  "index_updated": true
}
```

Rules:

- Refuse recursive deletion by default unless explicitly requested and guard allows it.
- Ask before deleting database files such as `*.sqlite`, `*.sqlite3`, `*.db`, `*.db3`, `*.duckdb` (includes project `.ainiux-pr/index.sqlite`).
- Refuse deletion outside workspace root or trusted temp directories.
- Record rollback data where practical; update the index.

### 12.15 `edit_file`

Purpose: primary structured editing tool.

Parameters:

```json
{
  "path": "string",
  "expected_file_hash": "string|null",
  "ops": [
    {
      "type": "replace_range|insert_at|delete_range|replace_text|replace_symbol|create_file",
      "start_line": "integer|null",
      "end_line": "integer|null",
      "line": "integer|null",
      "symbol_id": "integer|null",
      "expected_hash": "string|null",
      "old_text": "string|null",
      "new_text": "string|null",
      "replacement": "string|null",
      "replace_all": "boolean|null",
      "line_range_hint": {"start_line":"integer", "end_line":"integer"}
    }
  ],
  "atomic": "boolean",
  "create_dirs": "boolean"
}
```

Suggested defaults: `atomic: true`, `create_dirs: false`.

#### Operations

**`replace_range`** — replace inclusive lines `start_line`–`end_line` with `replacement`. Preferred normal edit mode. If `expected_hash` is supplied and mismatches, fail with `stale_range` and return current range preview.

**`insert_at`** — insert `new_text` before line `line`.

**`delete_range`** — delete inclusive lines `start_line`–`end_line`.

**`replace_text`** — find `old_text`, replace with `new_text`. Exact match first; fuzzy fallback (section 13) if enabled; disambiguate with `line_range_hint` or fail `ambiguous_match`.

**`replace_symbol`** — replace indexed symbol body by `symbol_id` + `replacement`. Check `expected_hash` against body/range hash when supplied. Reindex after success.

**`create_file`** — create new file with `new_text`; fail if exists (use `write_file` internally).

Return data:

```json
{
  "path": "src/agent.cpp",
  "applied": true,
  "operations_applied": 2,
  "old_file_hash": "fnv1a64:...",
  "new_file_hash": "fnv1a64:...",
  "reverse_patch_path": ".ainiux-pr/history/20260719-120000-001.patch",
  "index_updated": true,
  "summary": ["replaced lines 42-118", "inserted before line 7"],
  "warnings": []
}
```

Rules:

- Apply multiple line operations bottom-to-top.
- Use `expected_file_hash` or operation `expected_hash` when available.
- If stale, return current hash and a short current preview.
- For `atomic = true`, leave no partial edits after failure.
- Record rollback data before mutation.
- Reindex only affected files after success.

### 12.16 `str_replace`

Purpose: compatibility editing tool used by many coding agents.

Parameters:

```json
{
  "path": "string",
  "old_text": "string",
  "new_text": "string",
  "expected_file_hash": "string|null",
  "replace_all": "boolean",
  "line_range_hint": {"start_line":"integer", "end_line":"integer"},
  "fuzzy": "boolean"
}
```

Return data:

```json
{
  "path": "src/example.cpp",
  "matches_found": 1,
  "replacements_made": 1,
  "match_mode": "exact|normalized_whitespace|indent_stripped",
  "old_file_hash": "fnv1a64:...",
  "new_file_hash": "fnv1a64:...",
  "reverse_patch_path": ".ainiux-pr/history/20260719-120000-002.patch",
  "index_updated": true
}
```

Uses the same engine as `edit_file.replace_text`. Prefer `edit_file.replace_range` when possible.

### 12.17 `apply_patch`

Purpose: compatibility tool for OpenAI-style patch edits.

Parameters:

```json
{
  "patch": "string",
  "atomic": "boolean",
  "fuzzy": "boolean"
}
```

Return data:

```json
{
  "applied": true,
  "files_changed": ["src/a.cpp", "src/b.cpp"],
  "operations_applied": 4,
  "new_hashes": {"src/a.cpp":"fnv1a64:...", "src/b.cpp":"fnv1a64:..."},
  "reverse_patch_path": ".ainiux-pr/history/20260719-120000-003.patch",
  "index_updated": true,
  "warnings": []
}
```

Rules: parse add/update/delete; validate paths; apply atomically by default; run destructive guard before deletes; reindex changed files.

### 12.18 `find_callers`

Purpose: find symbols that call or reference a symbol.

Parameters:

```json
{
  "symbol_id": "integer|null",
  "name": "string|null",
  "max_results": "integer|null",
  "min_confidence": "number"
}
```

Suggested defaults: `max_results: 30`, `min_confidence: 0.3`.

Return data:

```json
{
  "callers": [
    {
      "symbol_id": 220,
      "qualified_name": "main",
      "path": "src/main.cpp",
      "line": 87,
      "confidence": 0.8
    }
  ],
  "truncated": false
}
```

### 12.19 `find_callees`

Purpose: find symbols called by a symbol.

Parameters:

```json
{
  "symbol_id": "integer",
  "max_results": "integer|null",
  "min_confidence": "number"
}
```

Suggested defaults: `max_results: 30`, `min_confidence: 0.3`.

Return data:

```json
{
  "callees": [
    {
      "symbol_id": 184,
      "qualified_name": "Agent::run",
      "path": "src/agent.cpp",
      "line": 42,
      "confidence": 0.8
    }
  ],
  "truncated": false
}
```

### 12.20 `find_tests`

Purpose: find likely tests for a file or symbol.

Parameters:

```json
{
  "path": "string|null",
  "symbol_id": "integer|null",
  "max_results": "integer|null"
}
```

Suggested default: `max_results: 20`.

Return data:

```json
{
  "tests": [
    {
      "path": "tests/test_agent.cpp",
      "symbol_id": 900,
      "qualified_name": "test_agent_run_handles_interrupt",
      "confidence": 0.7
    }
  ],
  "commands": [
    "make test",
    "ctest --output-on-failure"
  ],
  "truncated": false
}
```

Notes: use naming conventions, paths, and references. Approximate is fine.

### 12.21 `run_command`

Purpose: execute build/test/git/shell commands.

Parameters:

```json
{
  "command": "string",
  "cwd": "string|null",
  "timeout_ms": "integer|null",
  "stdin": "string|null",
  "env": "object|null",
  "max_output_bytes": "integer|null"
}
```

Return data:

```json
{
  "command": "make test",
  "cwd": "/path/to/project",
  "exit_code": 2,
  "duration_ms": 1842,
  "stdout": "...",
  "stderr": "...",
  "stdout_truncated": true,
  "stderr_truncated": false,
  "output_summary": "2 compiler errors in src/agent.cpp",
  "guard": {"decision":"allow","rule_id":null}
}
```

Rules:

- Do not use `system()`.
- Use `posix_spawn` / `fork+exec` on Unix-like systems (and a documented Windows path only if Windows support is in scope).
- Kill process groups on timeout.
- Cap stdout/stderr.
- Run the destructive-command guard before execution.
- After commands that may modify files, mark index as possibly stale and cheaply update changed files.

### 12.22 `git_status`

Purpose: compact git status through the git CLI (not libgit2).

Parameters:

```json
{
  "short": "boolean",
  "include_branch": "boolean"
}
```

Typical command: `git status --short --branch`.

### 12.23 `git_diff`

Purpose: bounded git diff through the git CLI.

Parameters:

```json
{
  "path": "string|null",
  "cached": "boolean",
  "stat": "boolean",
  "max_bytes": "integer|null"
}
```

Typical commands: `git diff --stat`, `git diff -- PATH`, `git diff --cached`.

### 12.24 `fetch_url`

Purpose: fetch a URL using libcurl / existing fetch safety layer.

Parameters:

```json
{
  "url": "string",
  "method": "GET|HEAD",
  "headers": {"Header-Name":"value"},
  "max_bytes": "integer|null",
  "timeout_ms": "integer|null",
  "follow_redirects": "boolean",
  "extract_text": "boolean",
  "include_headers": "boolean"
}
```

Return data includes `url`, `final_url`, `status`, `content_type`, `title`, `text`/`body`, optional headers, `bytes_read`, `truncated`.

Rules:

- Allow `http` and `https` by default; disable `file`, `ftp`, and unusual schemes unless explicitly enabled.
- Bound bytes and timeouts.
- Reuse private/loopback/metadata blocking from `src/fetch/` unless the user explicitly allows private URL fetch.
- Simple dependency-free HTML text extraction.

### 12.25 `search_web`

Purpose: web search through configured provider (`src/search/`).

Parameters:

```json
{
  "term": "string",
  "max_results": "integer|null",
  "timeout_ms": "integer|null",
  "site": "string|null",
  "freshness_days": "integer|null",
  "fetch_top_results": "boolean"
}
```

If no provider is configured, return `web_search_unavailable`. Do not hardcode one commercial provider.

### 12.26 `index_status`

Purpose: report index state for `.ainiux-pr/index.sqlite`.

Parameters:

```json
{
  "check_filesystem": "boolean",
  "max_changed_files": "integer|null"
}
```

Return data:

```json
{
  "index_exists": true,
  "path": ".ainiux-pr/index.sqlite",
  "fresh": true,
  "files_indexed": 220,
  "symbols_indexed": 6400,
  "refs_indexed": 18000,
  "changed_files": [],
  "last_updated": 1783170000
}
```

### 12.27 `index_update`

Purpose: update changed files only.

Parameters:

```json
{
  "paths": ["string"],
  "force": "boolean"
}
```

If `paths` is empty, detect changed files cheaply. If `force = true`, rescan even if timestamp/hash appears unchanged.

### 12.28 `index_rebuild`

Purpose: full rebuild for recovery/debugging.

Parameters:

```json
{
  "confirm": "boolean"
}
```

Normal users should rarely need this.

---

## 13. Fuzzy edit fallback

Failed edits are expensive because each failure often requires another model round trip. Therefore `str_replace` and `edit_file.replace_text` support a Gemini-style fuzzy fallback.

### 13.1 Fallback order

1. Exact byte-for-byte match.
2. Normalized whitespace match.
3. Leading-indent-stripped match.
4. Fail with useful diagnostics.

### 13.2 Normalized whitespace matching

Conceptually collapse runs of whitespace for comparison while mapping back to original source offsets for the actual replace:

```text
"foo(  a,\n b )" -> "foo( a, b )"
```

### 13.3 Leading-indent-stripped matching

For multi-line snippets, compare after stripping common leading indentation from both old text and candidate text. Handles extra indent level, tabs vs spaces at the margin, and snippets copied without surrounding indentation.

### 13.4 Multiple matches

- If `replace_all = true`, replace all matches.
- Else if `line_range_hint` is supplied, choose the match inside or closest to that range.
- Else fail with `ambiguous_match` and return candidate line numbers.

### 13.5 Safety rules

- Prefer exact match over fuzzy match.
- Report `match_mode` in the result.
- If the fuzzy match is too weak, fail.
- If multiple candidates remain, fail unless `replace_all` or `line_range_hint` disambiguates.
- Return candidate locations on ambiguity.

### 13.6 Why this matters

Slight whitespace/indent errors from the model become successful edits and save extra LLM turns, tool calls, file reads, and user frustration.

---

## 14. Parallel call handling

The scheduler accepts multiple tool calls from the model and executes safe independent calls concurrently.

### 14.1 Tool safety classes

```text
read_only_parallel_safe
network_parallel_limited
index_read_parallel_safe
index_write_serialized
file_mutation_serialized_by_path
workspace_mutation_serialized
command_guarded_maybe_mutating
always_serial
```

Recommended classification:

```text
read_only_parallel_safe:
  read_file, read_many, get_skeleton, list_directory, glob, search_text, grep, find,
  search_symbol, read_symbol, find_callers, find_callees, find_tests, project_overview,
  inspect_code_task, git_status, git_diff

network_parallel_limited:
  fetch_url, search_web

file_mutation_serialized_by_path:
  write_file, remove, edit_file, str_replace

workspace_mutation_serialized:
  apply_patch

index_write_serialized:
  index_update, index_rebuild

command_guarded_maybe_mutating:
  run_command
```

### 14.2 Lock rules

Lock scopes:

```text
workspace read lock
workspace mutation lock
file path mutation lock
index write lock
network concurrency token
```

Rules:

- Multiple reads/searches may run in parallel.
- Multiple network tools may run in parallel up to a configured cap.
- For v1, it is simpler to serialize all file mutations (even different files) if index/rollback conflicts are hard.
- Never mutate the same file in parallel.
- Serialize `apply_patch`.
- Serialize maybe-mutating `run_command` with file edits.
- If two model-emitted tool calls conflict, run them in safe order rather than failing.

### 14.3 `run_command` parallel policy

```text
safe_read_command:
  git status, git diff, git log, git show, pwd, ls, rg, grep, find without -delete

maybe_mutating_command:
  make, ninja, cmake --build, cargo test, go test, npm test, pytest, compiler commands

high_risk_command:
  anything caught by the destructive-command guard
```

Safe read commands can run in parallel with other reads. Maybe-mutating commands should not run in parallel with edits. High-risk commands must wait for user approval or be blocked.

---

## 15. Destructive-command guard

A small built-in guard inspired by destructive-command protection tools: command normalization, path canonicalization, and regex/pattern rules. This is not a full sandbox.

### 15.1 Guard applies to

```text
run_command
write_file
remove
edit_file
str_replace
apply_patch
```

Also any future database execution tool.

### 15.2 Guard decision shape

```json
{
  "decision": "allow|ask|deny",
  "rule_id": "AINIUX_GUARD_RM_RF",
  "severity": "low|medium|high|critical",
  "reason": "Recursive force delete requires approval.",
  "safe_alternative": "Use trash, move to .ainiux/tmp, or run git clean -n first."
}
```

### 15.3 Guard control flow

```text
1. canonicalize cwd and paths
2. normalize command spelling and wrappers
3. apply explicit user/project blocks
4. apply explicit safe allow rules
5. run keyword gate for cheap high-risk detection
6. apply destructive-pattern rules
7. if dangerous: ask or deny
8. if indeterminate and high-risk: ask
9. otherwise allow
```

### 15.4 Path rules

- File writes/deletes must stay inside the workspace root or trusted temp directories.
- Canonicalize paths before decision; handle symlinks and `..` conservatively.
- Reject or ask on path analysis failure for mutating operations.

Trusted temp directories:

```text
$TMPDIR when set
/tmp
/var/tmp
workspace/.ainiux/tmp
```

### 15.5 Commands requiring approval

Recursive force delete:

```text
rm -rf PATH
rm -fr PATH
rm -r -f PATH
rm --recursive --force PATH
find PATH -delete
xargs rm -rf
shred PATH
dd ... of=PATH
truncate -s 0 PATH
```

Destructive git:

```text
git reset --hard
git reset --merge
git clean -f
git clean -fd
git clean -fdx
git checkout -- PATH
git restore PATH          # except safe staged-only forms
git push --force
git push -f
git stash drop
git stash clear
git branch -D NAME
```

Destructive SQL/database operations:

```text
DROP DATABASE
DROP SCHEMA
DROP TABLE
TRUNCATE
DELETE FROM table          # without WHERE
sqlite3 db.sqlite "DROP TABLE ..."
psql -c "DROP TABLE ..."
mysql -e "DROP TABLE ..."
```

Database file deletion:

```text
*.sqlite
*.sqlite3
*.db
*.db3
*.duckdb
```

Workspace escape: writes/edits/deletes outside workspace root; redirection to outside path; `cp`/`mv`/`install`/`touch`/`mkdir` outside allowed roots.

### 15.6 Safe commands that should not be blocked

```text
git status
git diff
git log
git show
git add
git commit
git fetch
git pull
git push without force
git clean -n
git restore --staged PATH
rg PATTERN
grep PATTERN
find PATH -name PATTERN
ls
pwd
```

### 15.7 Shell normalization

Recognize common wrappers:

```text
/usr/bin/git status
env git status
command git status
bash -c 'rm -rf build'
sh -c "git reset --hard"
python -c "... destructive code ..."
node -e "... destructive code ..."
```

For `bash -c`, `sh -c`, `python -c`, `node -e`, heredocs, and piped scripts, either analyze the embedded text or conservatively ask if high-risk keywords are present.

### 15.8 User approval

- The agent may request approval.
- The agent must not approve its own request.
- The agent must not disable guard rules.
- Approvals should be one-shot by default.
- When blocked, the agent must replan instead of retrying the same blocked command.

Good alternatives to suggest:

```text
git diff
git stash push
git clean -n
git push --force-with-lease
move files to .ainiux/tmp
make a backup
run SELECT before DELETE
run schema dump before DROP
```

### 15.9 Basic command hygiene (even without full sandbox)

- Canonicalize paths.
- Refuse file edits outside workspace root.
- Detect binary files before text edits.
- Cap command output; use timeouts; kill process groups on timeout.
- Keep edit history; provide `/undo`.

---

## 16. Prompt and `AGENTS.md` handling

### 16.1 Built-in prompts

Keep internal prompts short and task-specific. Load only the base prompt plus at most one task-specific prompt.

```text
resources/agents/AGENTS.base.md
resources/agents/AGENTS.coding.md
resources/agents/AGENTS.debug.md
resources/agents/AGENTS.review.md
resources/agents/AGENTS.refactor.md
resources/agents/AGENTS.tests.md
```

### 16.2 Project `AGENTS.md`

1. Load `AGENTS.md` from the workspace root if it exists.
2. When editing or reading a specific file, also load the nearest `AGENTS.md` between the workspace root and that file's directory.
3. More specific `AGENTS.md` rules override broader project rules when they conflict.
4. Cache loaded files by `file_hash`; re-read if hash changes.
5. Cap total injected bytes via `agents_md.max_bytes_total`.

Example for `src/ui/button.cpp`:

```text
workspace/AGENTS.md
workspace/src/AGENTS.md
workspace/src/ui/AGENTS.md
```

Also recognize related project instruction filenames when present (`PLAN.md`, `PLANS.md`) only if product policy later expands discovery; v1 must at least support `AGENTS.md` with clear precedence.

### 16.3 Instruction precedence

```text
system/developer safety rules
ainiux built-in agent rules
user current request
project AGENTS.md rules
local nearest AGENTS.md rules
model-generated plan
```

`AGENTS.md` must not disable the destructive-command guard, change the workspace root, exfiltrate secrets, or override the user's direct request.

### 16.4 Minimum built-in instruction (condensed from v2 draft + v3)

The base built-in agent prompt must include:

```text
Use the local project index (.ainiux-pr/index.sqlite) first because it is cheap, but treat it
as a hint, not truth. The scanner can miss symbols, embedded code, macro-generated code,
overloaded functions, dynamic calls, and unusual syntax. Never blindly trust get_skeleton,
search_symbol, PageRank, or call graph data. If the indexed view looks incomplete, stale,
or contradictory, fall back to read_symbol, read_many, read_file, search_text, glob, grep,
git grep, compiler output, or tests.

Prefer tools in this order for context:
1. inspect_code_task
2. search_symbol
3. get_skeleton
4. read_symbol / read_many
5. targeted read_file ranges
6. glob / search_text / grep / find
7. run_command fallbacks
8. fetch_url / search_web only when external current information is needed

Prefer edits in this order:
1. edit_file.replace_range with expected_hash
2. edit_file.replace_symbol
3. edit_file.replace_text / str_replace with fuzzy fallback
4. apply_patch for multi-file patches
5. write_file only for new files or intentional full rewrites

Keep context small. Prefer batched reads and edits. Do not rewrite unrelated code.
After edits, run the narrowest useful test or build. If a tool fails for a non-permission
and non-network reason, do not retry blindly; use a different route.

Planning: concrete, bite-sized plans with exact files, commands, expected outputs, and
verification. Follow DRY, YAGNI, and KISS. Avoid new dependencies without clear need.

Coding: clear names, explicit errors, simple control flow. Preserve existing style unless
changing it is part of the task.

Testing: prefer TDD where practical. Cover Unicode, empty/long strings, edge numbers,
invalid input, permission and network failures where relevant. Rerun focused tests after
behavior changes.

Refactoring: remove duplication, improve names, extract helpers only when they simplify,
keep behavior unchanged, run tests after refactoring.
```

---

## 17. Preferred agent tool order

### 17.1 Context gathering

```text
1. inspect_code_task(query)
2. search_symbol(query)
3. get_skeleton(path)
4. read_symbol(symbol_id)
5. read_many([...])
6. targeted read_file(path, start_line, end_line)
7. glob(pattern)
8. search_text / grep / find
9. run_command with rg/git grep/compiler/test fallback
10. fetch_url/search_web only when external current information is needed
```

### 17.2 Editing

```text
1. edit_file.replace_range with expected_hash
2. edit_file.replace_symbol with symbol_id and expected_hash
3. edit_file.replace_text / str_replace with fuzzy fallback
4. apply_patch for patch-style multi-file edits
5. write_file only for new files or intentional full rewrites
```

### 17.3 Verification

```text
1. narrow unit test for changed area
2. compiler/build command for touched component
3. relevant integration test
4. formatter/linter only if already part of project workflow
5. git diff/status summary
```

---

## 18. Slash commands

Ship these in the first serious agent version:

```text
/help
/quit
/exit
/settings
/settings show
/settings set KEY VALUE
/settings unset KEY
/provider
/model
/reasoning
/tools
/new
/read PATH
/plan
/compact
/compact auto
/compact off
/status
/diff
/undo
/memory
/index status
/index update
/index rebuild
/index explain PATH
/guard status
/guard explain COMMAND
/resume
/clear
/chat
/editor [path]
/agent
/mode [chat|editor|agent]
/cycle
```

Mode switches (also available from chat and editor where practical):

```text
/chat     → chat TUI (central ~/.ainiux/ainiux.db)
/editor   → standalone editor
/agent    → agent TUI for current project (.ainiux-pr/agent.sqlite + index)
/mode     → show or set mode
/cycle    → chat → editor → agent → chat
```

`/guard explain COMMAND` shows whether a command would be allowed, asked, or denied and why, without running it.

`/tools` displays available tools, aliases, and whether web/search tools are configured.

`/status` shows:

```text
current mode (chat|editor|agent)
workspace root
git branch
dirty files
current provider / model / reasoning
context usage estimate
index path (.ainiux-pr/index.sqlite) and freshness
agent session path (.ainiux-pr/agent.sqlite)
last test result
guard enabled/disabled status
loaded AGENTS.md files
```

Ctrl-C behavior:

```text
First Ctrl-C: interrupt current model stream or running command.
Second Ctrl-C: abort current agent task; remain in agent UI input (or return to prompt if not in fullscreen).
```

(Align with terminal/editor copy conventions where agent shares editor-backed surfaces; document any surface-specific differences.)

---

## 19. Memory, compaction, and rollback

### 19.1 Session log

Use append-only `.ainiux-pr/session.jsonl` as the source of truth.

Example events:

```json
{"type":"user_goal","text":"Add agent mode"}
{"type":"tool_call","name":"search_symbol","args":{"query":"agent loop"}}
{"type":"edit","files":["src/agent.cpp"],"reverse_patch":".ainiux-pr/history/001.patch"}
{"type":"test","command":"make test","exit_code":0}
{"type":"decision","text":"Use replace_range as primary edit primitive"}
```

### 19.2 Generated memory

Generate `.ainiux-pr/memory.md` from the event log.

Suggested shape:

```markdown
# Project Memory

## Current goal
Implement agent mode for ainiux.

## Decisions
- Use file-level hashes as the primary edit-safety mechanism.
- Use range replacement as the primary edit primitive.
- Store code index in project-local .ainiux-pr/index.sqlite.
- Use git CLI instead of libgit2.

## Modified files
- src/agent/agent_loop.cpp: added first agent loop.
- src/agent/tools_edit.cpp: added range edit engine.

## Last verification
- `make test` failed with 2 errors in src/agent/tools_edit.cpp.

## Known issues
- No full sandbox yet.
- C++ scanner misses complex macro-generated methods.

## Next likely actions
- Fix edit tool tests.
- Re-run narrow tests.
```

Update memory after meaningful milestones, not after every tiny tool call.

### 19.3 Compaction

Preserve:

```text
original user goal
current plan
completed steps
files edited
file hashes for active files
exact test commands and results
important errors
design decisions
open TODOs
loaded AGENTS.md files
active file/symbol IDs
current git diff summary
```

Discard:

```text
old tool chatter
large file dumps no longer needed
repeated compiler output
superseded plans
failed search paths that do not matter
```

### 19.4 Rollback

Before each edit batch, store rollback data:

```text
.ainiux-pr/history/YYYYMMDD-HHMMSS-NNN.patch
```

Successful edit results should include: files changed, operation count, old and new file hashes, reverse patch path, index update status.

`/undo` applies the last ainiux-created reverse patch or restores saved file snapshots.

---

## 20. Git integration

Use git through the command line. Do not add libgit2 in the first implementation.

Useful commands:

```text
git status --short --branch
git diff --stat
git diff -- PATH
git diff --cached
git ls-files
git grep
git rev-parse --show-toplevel
```

Reasons: available in most environments, respects user config, no new dependency, process startup is not the main bottleneck.

---

## 21. Implementation milestones

Accepted merge: v3 runtime milestones plus explicit index/scanner/ranking milestones from v2.

### Milestone 1: minimal agent loop, tool registry, and chat-reuse UI shell

Files: `agent_loop.*`, `tool_call.*`, `tool_registry.*`, `tool_scheduler.*`, agent TUI shell reusing `src/tui/`

Tasks:

- Provider-neutral `ToolCall` / `ToolResult`
- Tool registry with name, schema, handler, safety class
- Agent loop: receive model tool calls, execute tools, return results
- Text fallback parser for weak tool-calling models
- Wire `ainiux agent` / `--agent` mode dispatch without enabling tools in normal chat
- Full-screen agent UI shell: **reuse chat thread area + bottom input + status line**
- Shared provider / model / reasoning selectors work in agent mode
- Project-local `.ainiux-pr/agent.sqlite` session bootstrap (empty thread ok)
- Mode switches: `/agent`, `/chat`, `/editor`, `/cycle` (and documented cycle keybinding if added)

Verification:

```text
./ainiux --agent --tools-selftest
# expected: agent tools self-test: OK
```

### Milestone 2: file, path, and search tools

Files: `tools_files.*`, `tools_search.*`, `hash.*`

Tasks:

- `read_file`, `read_many`, `write_file`, `remove`
- `list_directory`, `glob`, `search_text`, `grep`, `find`
- File-level hashing
- Path canonicalization and workspace-root checks

Tests: `test_file_tools`, `test_hash`, search tool tests.

### Milestone 3: edit engine

Files: `tools_edit.*`

Tasks:

- `edit_file` ops: replace_range, insert_at, delete_range, replace_text, replace_symbol, create_file
- `str_replace` compatibility wrapper
- Fuzzy fallback: exact, normalized whitespace, indent stripped
- `apply_patch` compatibility parser
- Rollback patch creation under `.ainiux-pr/history/`
- Hook for reindex of touched files (may no-op until Milestone 4)

Tests: `test_edit_tools`, `test_str_replace_fuzzy`, `test_apply_patch`.

### Milestone 4: project-local SQLite code index and scanners

Files: `index/*`, scanners

Tasks:

- Create/open `.ainiux-pr/index.sqlite` (never central chat DB)
- Schema: files, symbols, refs, FTS when available
- Incremental scan using size/mtime/hash
- Immediate reindex after ainiux edits
- Language scanners for priority languages
- Tools: `get_skeleton`, `search_symbol`, `read_symbol`, `index_status`, `index_update`, `index_rebuild`
- Slash: `/index status|update|rebuild|explain`

Success criteria:

- Changed ainiux-edited files reindex immediately
- External changes detected by timestamp plus hash
- Skeletons and symbol search fast enough for interactive use
- Index file lives at `.ainiux-pr/index.sqlite` and is portable with the project

### Milestone 5: ranking and task inspection

Tasks:

- PageRank over symbols/files
- Ranking formula with recent-file / language boosts
- `inspect_code_task`, `find_callers`, `find_callees`, `find_tests`, `project_overview`

Success criteria:

- Agent usually finds relevant files without repeated directory listing
- Fewer full-file reads; faster first useful edit

### Milestone 6: command execution and guard

Files: `tools_command.*`, `git_tools.*`, `command_guard.*`

Tasks:

- `run_command` without `system()`
- Timeout and process-group kill
- Bounded stdout/stderr
- `git_status`, `git_diff`
- Destructive-command guard
- `/guard status`, `/guard explain COMMAND`

Guard test cases must include:

```text
rm -rf build
rm -fr build
rm -r -f build
git reset --hard
git clean -fdx
git push --force
sqlite3 app.sqlite "DROP TABLE users;"
sqlite3 app.sqlite "DELETE FROM users;"
find . -delete
rm app.sqlite
rm .ainiux-pr/index.sqlite
write outside workspace
write to /tmp/ainiux-test-file
```

### Milestone 7: web tools

Files: `tools_web.*`

Tasks:

- `fetch_url` via existing curl/fetch safety
- Simple HTML text extraction
- `search_web` via configured `src/search/` providers
- Return `web_search_unavailable` if not configured

Failure cases: invalid URL, unsupported scheme, timeout, HTTP 404, large response truncation, provider not configured, private URL blocked.

### Milestone 8: prompts, AGENTS.md, memory, and compaction

Files: `agent_prompts.*`, `agents_md.*`, `agent_memory.*`, `context_compact.*`, resource prompt files

Tasks:

- Load short built-in prompts (base + one task-specific)
- Load root and nearest `AGENTS.md` with precedence
- Generate `.ainiux-pr/session.jsonl` and `.ainiux-pr/memory.md`
- `/compact`, `/compact auto`, `/compact off`
- `/plan`, `/resume`, `/memory`

### Milestone 9: benchmark and tuning

Extend benchmark mode to track:

```text
model_turns
tool_calls
parallel_tool_groups
input_tokens
output_tokens
wall_time_ms
local_tool_time_ms
model_time_ms
edit_attempts
edit_failures
fuzzy_edit_successes
test_runs
first_test_pass_rate
final_diff_lines
guard_blocks
guard_approval_requests
index_updates_after_edits
```

Most important metrics:

```text
model turns per completed task
edit failure rate
tokens to first correct patch
first-test-pass rate
search/read token volume
number of blocked destructive actions
```

Use benchmarks to tune tool output caps, symbol ranking, fuzzy edit thresholds, context compaction, and scanner regexes.

---

## 22. Test requirements

Add normal and failure cases. At minimum:

```text
Unicode: ÄÖ, Chinese, Arabic, Russian, emoji
empty strings
very long strings
zero and small positive/negative numbers
very large numbers
invalid numeric input
invalid/misspelled URL
corrupted file contents
binary file detection
permission denied on read/write
network timeout/unavailable
ambiguous str_replace matches
stale file hash
stale range hash
recursive delete blocked/asked
destructive git blocked/asked
destructive SQL blocked/asked
workspace escape blocked
parallel read calls succeed
parallel same-file edits serialize
index created at .ainiux-pr/index.sqlite not ~/.ainiux/ainiux.db
incremental reindex after edit
scanner extracts common C++/Python symbols
FTS fallback when FTS5 unavailable (if practical to simulate)
```

Do not keep rerunning the same failing tool blindly. If a tool fails for a non-permission and non-network reason, use a fallback route.

Leak checks: success, error, cancel, interrupted stream/command paths where tooling is available (`make test-sanitize`, `make test-leak`).

---

## 23. Summary

The first serious ainiux agent mode should be built around:

- separate entry `ainiux agent` / `--agent` (no silent tools in normal chat)
- **UI reused from chat mode** (thread + input + status + provider/model/reasoning)
- **mode cycle and slash jumps**: chat ↔ editor ↔ agent (`/chat`, `/editor`, `/agent`, `/cycle`)
- reuse of provider, runtime, fetch, search, security, and TUI layers
- project-local store under `.ainiux-pr/` with **`index.sqlite`** and **`agent.sqlite`** (never the central TUI chat DB)
- fast C++17 `std::regex`/pattern scanning for priority languages
- SQLite3 WAL index with optional FTS5 / trigram
- incremental per-file updates and settings-driven SQLite pragmas
- settings-driven tool limits (project JSON + optional user-global agent settings)
- file-level hashes as primary edit safety; range hashes for range edits
- token-efficient skeleton and symbol tools; index is a hint not truth
- `glob` / `grep` / `find` compatibility tools
- `fetch_url` and `search_web` with bounded output and existing safety policies
- range edits with expected hashes; fuzzy fallback for text replacement
- `apply_patch` and `str_replace` compatibility
- parallel reads; serialized mutations; DCG-style destructive guard
- git command-line integration
- append-only session log, generated memory, cheap rollback and `/undo`
- short built-in prompts plus project `AGENTS.md` precedence
- practical default safety (workspace edits allowed; destructive actions ask/deny)

This is the 80/20 approach: extremely fast on common cases, simple to reason about, dependency-light, portable per project via `.ainiux-pr/index.sqlite`, and able to fall back to slower generic tools when the index is wrong.

---

## 24. Document history

| Version | Notes |
| --- | --- |
| v2 | Deep index/scanner/ranking plan; project `.ainiux-pr/index.sqlite` |
| v3 | Agent runtime focus; guard; parallel tools; AGENTS.md; used pkchat naming |
| **v4** | Merge: v3 basis + all accepted v2 features; ainiux branding; explicit central-DB boundary; architecture fit; merged milestones; safety choice A |
| **v1.0 in PLANS.md** | Title simplified to “Local agent mode”; agent UI reuses chat TUI; mode cycle chat→editor→agent; project-local `agent.sqlite` for sessions |
| **tool-call reliability addendum** | Merged into §10: dual native/XML channels, 8-stage argument pipeline, separate transport vs tool-retry budgets, loop/turn caps, history hygiene, prompting rules, steps, and acceptance criteria |

Superseded drafts may remain in the repo for history; **implement against this v1.0 section**.

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

If the user does not provide a file name, `ainiux` should automatically create a non-existing file name in the current directory, such as `image1.png`, `image2.png`, and so on.

## Command shape

Suggested CLI shape:

```sh
ainiux image -p "A quiet terminal workspace at night" --image-model MODEL
ainiux image --prompt-file prompt.txt --image-model MODEL --width 1024 --height 1024
ainiux image -p "diagram of provider adapters" --format png --output diagram.png
ainiux image -p "small icon" --size 512x512 --format webp
ainiux image --provider openai --image-model MODEL -p "..." --output image.png
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

- [ ] `ainiux image ...` is the explicit image-generation command.
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

- [ ] Unsupported provider/model/format/dimensions return `AINIUX_ERR_UNSUPPORTED_FEATURE` or a more specific error where available.
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
ainiux: HTTP 404 from http://localhost:8000/v1/chat/completions
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
