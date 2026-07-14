# Decisions

## C++17 and Makefile

The initial implementation uses C++17 and a plain Makefile to keep the binary portable across POSIX-like systems.

## HTTP Transport

`src/http/` owns HTTP transport and uses libcurl through RAII wrappers for the easy handle and header list. The Makefile discovers build flags with `pkg-config libcurl`, falling back to `curl-config`. Streaming response bodies are delivered through libcurl write callbacks so provider code can parse SSE incrementally without spawning the `curl` executable.

## JSON Facade

`src/json/` is a small internal JSON facade used for request escaping and provider response parsing. The project should replace or expand it with a reviewed JSON library when dependency installation is available.

## System Configuration Template

v0.6 begins with one canonical, system-wide TOML-alike template at `config/pkchat.conf`. It mirrors the existing runtime defaults without duplicating built-in provider registry records or setting optional sampling parameters. `make install` installs it to `${SYSCONFDIR}/xdg/pkchat/config.conf`, normally `/etc/xdg/pkchat/config.conf`, with mode `0644` and does not overwrite an existing administrator-managed file.

`src/config/` owns the dependency-free parser, schema mapper, and automatic layer loader. It reads regular files with a 1 MiB default cap and produces an owned map keyed by fully qualified setting name. Boolean, signed 64-bit integer, finite float, quoted string, and bare string values remain typed, and every entry retains its source path and byte-based line/column location. Parsing validates UTF-8 and rejects duplicate keys or malformed syntax without returning a partially populated document.

Automatic layers follow XDG precedence: system files are applied in reverse `$XDG_CONFIG_DIRS` order, then `$XDG_CONFIG_HOME/pkchat/config.conf` or the `HOME` fallback. Each document is schema-validated into a temporary `cli::Options` copy before it replaces the effective options, preventing partial application. The ordinary CLI parser then runs over that configured base so command-line values remain authoritative. TUI theme and thinking-trace visibility are ordinary effective options; provider credentials remain references resolved later by the provider layer.

`--no-config` narrowly disables the automatic user file while retaining system configuration. This gives users a simple way to troubleshoot or bypass personal defaults without bypassing administrator policy. Arbitrary `--config PATH` layering is intentionally omitted until there is a concrete need. `--debug` reports considered configuration paths and their loaded, missing, skipped, or failed state on `stderr`; it never prints configuration values.


## Provider Registry and API Adapters

v0.4 begins with a data-driven provider registry in `src/provider/`. Built-in profiles carry aliases, default base URLs, endpoint paths, key environment variables, local/remote flags, optional dummy keys, compatibility warnings, and client capability flags. The OpenAI-compatible providers share the same Chat Completions adapter instead of duplicating provider-specific request code.

Responses API support is a sibling adapter selected with `--api responses`, `--responses`, or the `openai_responses` shortcut. It reuses the existing HTTP transport, cancellation token, timing, redaction, and streaming callback path. The first slice is text-only and maps Responses output text and SSE text deltas into the same internal assistant message model used by Chat Completions. Non-text Responses features remain disabled in reported client capabilities until implemented.

The `none` provider profile represents an explicit model-offline state. It has no base URL, endpoint paths, credentials, or model capabilities. Provider transport entry points reject model listing and chat before constructing an HTTP request, and endpoint overrides are invalid with this profile. This lets editor, conversion, explicit URL fetching, and local REPL/TUI commands run without inventing a dummy OpenAI-compatible endpoint. URL fetching remains a separate explicit network operation governed by its own safety policy.

Reasoning and thinking request controls are translated inside `src/provider/` rather than emitted as one generic pair of fields for every endpoint. `--thinking` and `--thinking-budget` remain the user-facing controls, but request serialization maps them to OpenAI Chat `reasoning_effort`, OpenAI Responses `reasoning.effort`, OpenRouter `reasoning`, Gemini OpenAI-compatible `reasoning_effort` or `extra_body.google.thinking_config.thinking_budget`, Qwen/DashScope `enable_thinking` and `thinking_budget`, DeepSeek `thinking.type` plus `reasoning_effort`, and xAI `reasoning_effort`. Local/custom OpenAI-compatible profiles retain the generic `enable_thinking` and `thinking_budget` fallback because those endpoints often accept experimental llama.cpp/Qwen-style fields. Provider capability probing, native Anthropic extended thinking, and preservation of signed/opaque reasoning state for future agentic tool loops are deferred.


## JSON Chat Persistence

v0.2 stores explicit chat files via `--save-chat PATH` and `--load-chat PATH` before adding automatic XDG chat IDs. This keeps the early REPL scriptable and reviewable while still using the target schema fields: `schema_version`, timestamps, provider, base URL, model, settings, messages, attachments, usage, and compaction events. Saves use a temporary file, fsync, rename, and restrictive file permissions.

## SQLite Chat Persistence

The automatic local TUI chat library uses `libsqlite3` and stores its database at `~/.pkchat/pkchat.db`. SQLite is a standard system library on the target POSIX-like platforms and keeps the dependency smaller than adding a bespoke storage engine. The schema stores one row per message rather than one JSON transcript blob so thread listing, transcript replay, regeneration, attachments, usage records, and compaction events have stable identifiers and indexes. WAL mode is enabled for the local database, with short transactions and a short busy timeout so indexed `/list` queries can run synchronously without waiting indefinitely on a lock.

## Runtime Jobs

v0.3 introduces `src/runtime/` with a small cancellation token, thread-safe event queue, and RAII `JobHandle`. The first users are the full-screen TUI foundation and cancellable provider requests. HTTP requests carry a cancellation token down to libcurl and abort through `CURLOPT_XFERINFOFUNCTION`, returning `PKCHAT_ERR_CANCELLED` instead of a generic transport error.

The initial model is one worker thread per active job. It is intentionally small: the owning UI loop receives events and remains the only code that mutates terminal/session state. This can grow into a queue or pooled runtime when TUI/web workloads need it.

## Full-Screen TUI Foundation

v0.3 adds `--chat` as an alternate-screen terminal UI, with `--tui` retained as a compatibility alias without adding an ncurses dependency yet. The TUI renders chat history, a status line, and a bounded bottom input panel without reserving persistent header rows for endpoint/model details. The input panel embeds `EditorState`, using the editor's rectangular renderer, soft wrap, visual-row cursor movement, undo/redo stack, page movement, and exact substring search so long multiline prompts behave like the standalone editor without taking over the whole screen. Chat requests, model listing, and chat save/load run as runtime jobs so terminal input remains responsive. A bare `Esc` cancels the active model request, while `Ctrl+R` triggers regeneration (resending the last user prompt) by cancelling any active chat request from the owning event loop. `Ctrl+U` is undo and `Ctrl+Y` is redo. TUI colors use semantic roles and 24-bit ANSI SGR sequences so contrast is controlled instead of depending on terminal palette mappings. The `dark` theme is the default, `/theme dark|light` switches at runtime, and `--nocolors` disables color styling while preserving terminal control sequences. Theme foreground/background pairs are unit-tested against the WCAG 2.1 AA 4.5:1 contrast target, including the subdued tinted role used for visible thinking traces.


## Shared syntax highlighting engine

Syntax highlighting lives in `src/highlight/` and produces byte-based semantic spans without terminal escapes. `text` remains the safe fallback for scratch buffers, unknown extensions, and manual opt-out. The dependency-free engine supports Markdown, Python, C, C++, C#, Java, JavaScript/JSX, TypeScript/TSX, HTML, HTML-only, CSS, XML, JSON, Bash, PHP, Perl, Ruby, Rust, Go, PowerShell, Assembly, SQL, TOML, YAML, and INI. The canonical `html` mode composes the HTML, JavaScript, and CSS lexers for script/style element bodies and inline event/style attributes; `.html`/`.htm` select it automatically. `htmlonly` preserves the markup-focused implementation and is selected for XML-oriented `.xhtml` files. The former `html-multi` and `htmlmulti` names remain compatibility aliases for `html`. The engine keeps explicit line state for fenced blocks, continued HTML tags, comments, strings, heredocs, CDATA, PHP/Perl/Ruby heredocs, Rust raw strings and nested comments, Go raw strings, PowerShell here-strings, SQL dollar-quoted strings, TOML multiline strings, YAML block scalars, and embedded script/style content. It resolves nested-language tokens over fallback spans, caps pathological lines, and provides an incremental document cache invalidated from before the changed line so multiline state remains correct.

Editor rendering now carries syntax and selection as independent structured overlays. Terminal adapters apply semantic theme colors and reverse video at the final output boundary, so wrapping, grapheme/cell-width calculations, invalid UTF-8 replacement, selection, and `--nocolors` do not depend on embedded ANSI bytes. Chat history consumes the same Markdown spans; labels, thinking traces, and activity indicators remain higher-priority UI styles. No external dependency was added, and regular expressions are compiled once with `std::regex::optimize`.

The UI exposes `/highlight` in editor/chat and per-buffer `/mode MODE|auto` in the editor. The process-wide toggle is shared across editor/chat switches but is not persisted. Automatic mode follows filename endings, while manual mode survives buffer switches and save-as. Recognized Markdown fence tags delegate to the same language engine; unknown tags stay plain.

## Standalone Editor Foundation

`--editor` is a permanent bonus mode and a controlled test bed for the multiline editing layer now embedded in the chat TUI. The editor core uses a piece table: the original file and appended edit buffer are kept separately while visible text is represented by pieces. This keeps inserts and deletes local to the piece list instead of rewriting the whole buffer on every keystroke, which is a better fit for large files than a single mutable string.

Rendering is split from terminal I/O. `EditorState` renders into a caller-provided `Rect`, so one terminal window can eventually host multiple editor panels or embed the editor in a partial-screen chat layout. Long lines soft-wrap inside that rectangle, preferring whitespace breakpoints and hard-wrapping long words. Vertical movement has two modes: logical hard-line movement for file editing, and visual-row movement that treats wrapped overflow rows as cursor targets for TUI chat input. The current terminal harness uses POSIX `termios` and ANSI escape sequences because `ncursesw` was not available in the build environment; the core renderer is independent of that choice.

The standalone editor reserves the bottom two terminal rows for editor-owned UI: a reverse-video status line and a one-line minibuffer. The main editing screen uses all remaining rows. The status line reports path, dirty state, the compact syntax language and line-ending mode as `(mode LF)`, `(mode CRLF)`, or `(mode CR)`, and cursor position; it omits redundant editor and automatic/manual labels to preserve space on 80-column terminals. The minibuffer handles prompts such as save path, load path, huge-file load confirmation, exact substring search, exact substring replacement, overwrite confirmation, scratch-buffer save-on-quit, and unsaved-exit confirmation. Replacement mode is deliberately modal after the search/replacement prompts: each match is replaced, skipped, or all remaining matches are replaced to the end of the buffer. The editor keeps a bounded undo/redo history with a configurable default depth of five entries. Editor file loads are unlimited by default except by address space and available memory, but config can set a hard byte limit and a separate warning threshold. Document `Tab` first queries the document word-completion domain and falls back to indentation; selected blocks always indent. Command and path completion remain isolated in the command minibuffer, while chat-TUI completion keeps its existing context rules.

Editor buffers use LF as their only internal line-boundary representation. File loading detects uniform LF, CR, or CRLF input while normalizing boundaries; file save and auto-save stream the internal LF boundaries using the buffer's detected or explicitly selected output style. This keeps piece-table line indexing and editor operations independent from external encodings, preserves the presence or absence of a final line ending, and makes mixed endings explicit: they warn and use the configured default instead of silently choosing the first ending encountered. Tab width, tab style, and output line ending are buffer-owned settings initialized from config defaults.

File loading also performs a bounded indentation probe over the first 20 physical lines. It compares nonblank leading-space depths and accepts a width only when one candidate explains at least two thirds of observed depth changes without an equally strong candidate; a single clean block transition is sufficient. Consistently space- or tab-indented samples select that style, while mixed prefixes/styles, conflicting steps, unindented input, and one-line input retain configuration fallbacks. Detection happens for normal opens and autosave recovery before the text moves into the piece table, does not scan the whole file, and remains a per-buffer initial value that `/tab-width` or `/tab-style` can override.

Document completion keeps ordered exact and full-case-folded maps with occurrence counts in each open buffer. Prefix lookup starts with `lower_bound`; ordinary edits remove and re-index only the token window joined or split by the edit, while bulk/stream replacements invalidate and rebuild lazily. The index cache uses documented copy-on-write shared ownership because the editor loop temporarily copies the active `EditorState` when handing it to and from the open-buffer list; this keeps those handoffs cheap while edits detach before mutating an index. Unicode letters, numbers, marks, uppercase properties, and full default case folding come from generated, checked-in Unicode 15.1 UCD tables, so completion has no locale or runtime Unicode-library dependency. Invalid UTF-8 bytes remain buffer data but delimit indexed words. Completion candidates and cycling state are separate from command, path, AI-command, and chat completion, and cycling reuses the first replacement's undo record.

Language indentation reformatting lives in `src/editor/reformat.*` and is deliberately narrower than a source formatter: it replaces leading whitespace only. It reuses highlighter spans and multiline lexical state to ignore structural tokens in comments, strings, heredocs, fences, and scalars. Conservative profiles cover brace, keyword/end, markup, SQL, indentation-topology, and assembly-label languages. Formatting is a cancellable runtime job over immutable input. Each editor buffer has a stable process-local identity and monotonic text revision; the UI applies a worker result only when identity, revision, language, tab width, and tab style still match, then records the range replacement as one undo operation. The event queue outlives and is destroyed after its worker handle, preventing callbacks into released queue state during cancellation or shutdown.

v0.76 adds byte-offset selection with Shift+movement keys, reverse-video highlighting, and internal copy/cut/paste shared between standalone editor mode and the chat TUI input. Bracketed paste is enabled so terminal paste events can be distinguished from typed input; paste prefers the internal clipboard when it is non-empty and otherwise falls back to the terminal payload. Copy publishes through OSC 52 where supported, but local editing does not depend on the OS clipboard for Ctrl+C/X/V round-trips.

v0.78 adds grapheme-aware Unicode handling to the standalone editor and shared editor core. Navigation, backspace/delete, display-column math, rendering, and soft wrap now operate on grapheme clusters instead of raw UTF-8 bytes. Terminal cell width uses a simplified East Asian Wide and emoji range table with zero-width combining marks, variation selectors, skin-tone modifiers, and ZWJ sequences. Invalid UTF-8 is replaced with a visible placeholder during render. Full UAX #29 conformance and `utf8proc`/`libgrapheme` integration remain future work.

v0.77 adds the first editor AI feature: continue/auto-write on `Ctrl+Space`. The editor sends up to `MAX_AI_CONTINUE_READ` characters before the cursor to the configured provider, streams visible continuation text at the cursor up to `MAX_AI_CONTINUE_TOKENS`, hides thinking traces from the buffer, and reports thinking/writing/stopped states in the minibuffer. Continue runs on cancellable runtime jobs with events delivered to the editor loop; SSE completion is detected through `finish_reason` and `[DONE]`, then the HTTP stream is aborted so the editor can return to idle without a keypress. For `lmstudio`, `ollama`, `vllm`, and loopback custom base URLs, editor startup auto-selects the first `/v1/models` entry when `--model` is omitted.

v0.79 unifies editor AI assist modes around four scoped behaviors: `selection` and `all` replace text in-place; `continue` streams after the cursor using tail-before-cursor context; `insert` streams after the cursor using the current selection as input. The older start-to-cursor `insert` mode was removed in favor of `continue` semantics. Config token `local_insert` remains an alias for `insert`. Built-in `/spell`, `/grammar`, `/continue`, `/fact`, `/comment`, `/rewrite`, `/English`, `/Chinese`, and `/Finnish` expose all four scoped modes by default, and `Ctrl+Space` uses `continue`.


## Document Extraction Modules

v0.5 starts document extraction with a separate `src/html/` module. The HTML converter is intentionally small, uses C++17 `std::regex` for simple tag and attribute matching, and converts easy page content to plaintext or Markdown without adding another parser dependency. Script/style/noscript removal uses a linear scanner instead of a broad regular expression so large real pages do not trigger catastrophic regex recursion. The first slice handles headings, bold/strong, emphasis/italic, links, line breaks, and simple block spacing. It tolerates common malformed HTML by ignoring unknown or broken tags while preserving text. Input is validated as UTF-8 at the extraction boundary; legacy charset conversion is deferred, so invalid bytes produce an explicit unsupported-feature error instead of mojibake or crashes. It does not execute JavaScript or attempt to be a browser-grade HTML parser.

Future PDF and Word extraction should live in separate modules such as `src/pdf/` and `src/word/` rather than growing the HTML module.

## Markdown Output Rendering

Assistant output rendering lives in a separate `src/markdown/` module so provider code, HTML extraction, and UI code do not grow Markdown-specific branches. The first writer is intentionally line-oriented and dependency-free: it supports ATX headings, paragraphs, fenced and indented code blocks, bold, italic, `++underline++`, Markdown links, ordered/unordered nested lists, simple pipe tables, raw HTML blocks, HTML fragments, and plaintext stripping. `--output-format md` preserves the existing raw Markdown streaming path. `--output-format html` and `--output-format plaintext` render after the full assistant reply is available because a correct fragment/document needs complete block context. HTML written to `stdout` is a fragment; HTML written through `--output PATH` is wrapped as a complete document with doctype, charset, viewport, head, and body.

## URL Fetching First Slice

`--input PATH` is the local-file input path for `.txt`, `.md`/`.markdown`, and `.html`/`.htm`; the parser is selected from the path ending. `--fetch-url` is explicit and never triggered by URLs found inside prompt text. Used alone, either option is an extraction mode controlled by `--output-format md|html|plaintext|json|jsond|ndjson`. Combined with `-p`/`--prompt` or `--prompt-file` in non-interactive CLI mode, the converted input is inserted as a separate user-context message before the final user prompt, so saved transcripts show exactly what was sent. The older `--html-file` remains a compatibility alias for local HTML input. The CLI URL path uses the existing libcurl RAII wrapper, sends browser-style `User-Agent`, `Accept`, `Accept-Language`, and `Upgrade-Insecure-Requests` headers, captures `Content-Type`, applies a hard `max_body_bytes` cap in the HTTP write callback before appending oversized chunks, sets a fetch-mode total timeout when the user did not provide one, and refuses private/loopback/link-local/multicast/metadata literal hosts unless `--allow-private-url-fetch` is provided. Redirect following remains disabled in this slice.

The follow-up URL safety layer checks the actual IPv4/IPv6 address passed to libcurl's socket-open callback and refuses private, loopback, link-local, multicast, and metadata ranges before creating the socket. Proxy URL fetching requires `--allow-private-url-fetch` because proxy-side target DNS cannot be verified locally. The same module backs CLI `--fetch-url`, REPL `/fetch`, and cancellable TUI `/fetch`; charset conversion, allow/block domain configuration, and future web integration remain follow-up work.

## Local Image Input First Slice

The first image-input slice extends `--input PATH` rather than adding a second overlapping file option. A small `src/input/` module classifies file endings case-insensitively and owns bounded binary reading, signature validation, and base64 encoding. PNG, JPEG, and GIF inputs become OpenAI-compatible Chat Completions content parts with `type: image_url` and an in-memory data URL. WebP input is intentionally disabled after compatibility testing showed that common vision endpoints do not decode it reliably. WebM is not accepted because it is a video container.

Image input is Chat Completions-only. Non-interactive repeated `--attach` and interactive `/attach` accept supported images for the next prompt. `/insert` is deliberately not an image alias: it is a UTF-8 text insertion command. The default limit is 20 MiB per image and can be lowered with `--max-image-bytes`. Base64 data is temporary request state: it is cleared after the provider call and never written into chat JSON. Conservative capability detection combines provider-registry support with recognized vision-model names; `--image-capability allow` is the explicit override for a verified compatible custom model.

## Bounded Text Attachments

Repeatable `--attach PATH` and interactive `/attach PATH` reuse the same case-insensitive document classifier and HTML/Markdown/plaintext conversion path as `--input`. Each local text attachment is read incrementally with a default 1 MiB `--max-input-bytes` cap, checked for binary NUL bytes, and validated as UTF-8 before its converted content becomes visible provider context. The raw read buffer is released after conversion; the transcript preserves the converted context that was actually sent.

PDF and DOCX remain unsupported binary types. Bidirectional PDF/Markdown and DOCX/Markdown conversion is deferred rather than approximated by inserting binary data or adding an unreviewed document dependency.

Interactive `/insert FILE_OR_URL` and `/attach PATH` use separate runtime paths. `/attach` retains the provider-context/image behavior. `/insert` performs a bounded UTF-8 read for any local file ending or an explicitly requested safe HTTP(S) fetch, then sends text through the owning UI event queue for one cursor insertion. HTML-to-Markdown conversion defaults on and can be disabled to retain raw HTML. Editor insertion records the target buffer identity, revision, and cursor, and discards stale results rather than applying them to changed or closed buffers. `/fetch URL` remains the chat-history context command. Workers check cancellation and never mutate UI or chat state directly.

## Web Search First Slice

v0.88 adds a separate `src/search/` module for explicit web search. Search is never inferred from prompt text. CLI `--search QUERY` can run standalone (printing ranked results) or insert a user-context message before `-p`/`--prompt`, matching the fetch/input context pattern. REPL `/search`, TUI `/search`, and editor `Esc /search` reuse the same formatter and provider chain.

Provider selection is client-side and independent from LLM provider profiles. Configured API providers are tried when credentials or base URLs exist: Tavily, Firecrawl, Exa, and Searxng. `provider = auto` tries configured API providers first, then falls back to DuckDuckGo Instant Answer and Google HTML parsing when keys are absent or a provider fails. Results are capped by `web_search.max_results` (default 3), overridable through `--max-web-search-results` or `MAXIMUM_WEB_SEARCH_RESULTS`.

The module reuses the existing libcurl HTTP wrapper. Google HTML parsing is intentionally fragile and treated as a best-effort fallback. DuckDuckGo may return only related topics when the instant abstract is empty. Follow-up work may add richer page fetching, provider capability probing, and tighter HTML-parser hardening.

## Request-Only Context Policies

`--max-context-bytes N` enables a conservative text-byte budget. The `error`, `truncate-oldest`, `summarize-oldest`, `summarize-middle`, and `provider-auto` policies operate on a temporary provider request vector. Summaries are bounded deterministic extracts and do not make nested model calls. The full `Session::messages` transcript remains unchanged, while successful compactions append structured `compaction_events` containing the policy, estimated byte counts, affected-message count, timestamp, and notice. This is explicitly a byte estimate and is not presented as an exact provider token count.

## JSONL Benchmark Datasets

Benchmark input starts with JSONL because it is streamable, diffable, Unicode-safe, easy to generate without an added dependency, and maps cleanly onto single- and multi-turn cases. `src/benchmark/` owns strict schema validation and bounded loading. The authoritative 50-case corpus remains the ordinary file `benchmarks/builtin.jsonl`; the Makefile converts that file into a generated C++ raw string so the default `builtin` dataset remains available after moving or installing the executable. The original JSONL and the opt-in long-context dataset are installed as data files for inspection and extension.

Benchmark result output is JSONL. One record is emitted for each measured turn and a final summary reports token estimates, provider usage, timing/throughput aggregates, nearest-rank percentiles, scoring, and completed, failed, and cancelled case runs. File-backed output is closed and parsed line-by-line into a same-basename Markdown companion containing the same summary and result information; stdout-only execution does not create a report. This keeps JSONL authoritative and avoids retaining an unbounded speed-run result set in memory. Warmups use the same execution path but are not emitted or counted as measurements. Multi-turn history includes actual prior assistant output. A bounded worker pool supports concurrent finite runs and duration-driven speed load. Speed is exclusive because combining a timed load test with evaluation labels would make run counts ambiguous; quality and refusal labels may be combined and share one model response per case.

`Ctrl+C` is handled by an async-signal-safe flag. A normal monitor thread converts that flag into the shared runtime cancellation token, so active libcurl calls use the existing cancellation path and all worker/timer threads are joined before exit. Human summaries remain on `stderr`, with table and CSV renderers sharing the same aggregate rows so JSONL `stdout` stays pipeline-safe. Optional dataset `expect` hooks support only byte-deterministic exact and substring checks after thinking-trace removal. Regex refusal/reasoning checks remain deferred until matching and false-positive semantics are specified; Parquet/Hugging Face Datasets input also remains deferred.

## Application Module Split (v0.84)

v0.84 splits the largest application sources into focused directories so CLI, editor, and TUI code can evolve independently while sharing the same provider, runtime, and chat layers:

- `src/app/` owns non-interactive CLI orchestration formerly in `main.cpp` (`exit_codes`, `output`, `document_mode`, `benchmark_mode`, `chat_session`, `repl_mode`, `config_diagnostics`). `src/main.cpp` remains a thin entry point.
- `src/editor/` owns the standalone `--editor` mode (`piece_table`, `editor_state`, `render`, `file_io`, `terminal_ui`, `run_editor`, `editor_assist`, and `detail/` Unicode helpers).
- `src/tui/` owns the full-screen TUI (`layout`, `status`, `theme`, `thinking`, `terminal`, `input_handlers`, `run`, and `detail/` frame rendering).

The benchmark built-in JSONL corpus is split into category files under `benchmarks/builtin/` (`coding.jsonl`, `cutoff.jsonl`, `multi-turn.jsonl`, `reasoning.jsonl`, `safety.jsonl`, `writing.jsonl`) so individual prompt sets can be maintained without editing one large file. The `cutoff` category contains one dated factual question per month (January 2023 through July 2026) sourced from `llm_knowledge_cutoff_test_dataset_v2.md`; each case tags its event month and carries a `reference_answer` for knowledge-cutoff evaluation.

## Version Metadata and Test Layout (v0.83)

v0.83 moves version constants out of headers into `src/version/version.cpp` so `kVersion`, copyright, and license strings have one owned definition. `include/pkchat/version.hpp` keeps only the extern declarations.

The unit-test driver was refactored from one large `tests/unit/test_runner.cpp` into per-module files under `tests/unit/<module>/`, each exposing a `run_all()` entry point. `test_runner` now only dispatches those suites. This keeps new tests close to the code they exercise and makes failures easier to locate.

v0.83 also expands automated coverage and adds small mocks for faults that are awkward to reproduce portably:

- `tests/mock_server/slow_http_mock.py` plus `tests/unit/mock/slow_server.cpp` for real local slow-response and chunked-body timeout tests.
- `tests/mock/posix_io_mock.c`, preloaded with `LD_PRELOAD`, to simulate `ENOSPC` on write paths tagged with `mock-enospc`.
- `tests/unit/io/test_io_faults.cpp` and `tests/unit/test_io_faults.cpp` for read-only permission failures (`chmod`) and the network/disk fault cases above.

Environment-dependent fault tests run in a separate `build/test_io_faults` binary so the main `test_runner` stays fast and deterministic on every platform.

## Advisory Editor File Sessions

File-backed editor buffers use `src/editor/file_session.*` to canonicalize identity, fingerprint disk state, and own an atomic `FILE.LOCK` directory. `EditorState` copies share the RAII lock through `std::shared_ptr` because active buffers are copied while switching buffers and temporarily moving between editor and chat modes; the underlying lock object remains uniquely responsible for token-checked cleanup. Scratch buffers have no lock. The lock is held for the entire buffer lifetime and Save As is a destination-locked transaction that retargets only after a successful write.

Lock metadata uses a bounded, versioned, hex-encoded line format so arbitrary canonical path bytes cannot alter its structure. Automatic stale recovery is deliberately limited to a valid same-host owner whose PID is proven absent with `kill(pid, 0)`. Cleanup names only `owner` and the lock directory and restores owner metadata if an unexpected entry prevents `rmdir`; recursive deletion is forbidden. Because locks are advisory, a separate fingerprint check protects saves from external modification, inode replacement, and deletion, and overwrite confirmation is valid only for the fingerprint observed by that prompt.

The ENOSPC fault launcher preserves the ordinary `posix_io_mock.so` preload for normal binaries. For sanitizer binaries it detects the ASan dependency, resolves the compiler's runtime, and places that runtime first in `LD_PRELOAD`; an unresolved sanitizer runtime is an explicit unsupported-toolchain failure. The aggregate `test` target invokes integration tests only after unit and fault tests complete, including under parallel make.
