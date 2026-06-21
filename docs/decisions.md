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


## Provider Registry and API Adapters

v0.4 begins with a data-driven provider registry in `src/provider/`. Built-in profiles carry aliases, default base URLs, endpoint paths, key environment variables, local/remote flags, optional dummy keys, compatibility warnings, and client capability flags. The OpenAI-compatible providers share the same Chat Completions adapter instead of duplicating provider-specific request code.

Responses API support is a sibling adapter selected with `--api responses`, `--responses`, or the `openai_responses` shortcut. It reuses the existing HTTP transport, cancellation token, timing, redaction, and streaming callback path. The first slice is text-only and maps Responses output text and SSE text deltas into the same internal assistant message model used by Chat Completions. Non-text Responses features remain disabled in reported client capabilities until implemented.

The `none` provider profile represents an explicit model-offline state. It has no base URL, endpoint paths, credentials, or model capabilities. Provider transport entry points reject model listing and chat before constructing an HTTP request, and endpoint overrides are invalid with this profile. This lets editor, conversion, explicit URL fetching, and local REPL/TUI commands run without inventing a dummy OpenAI-compatible endpoint. URL fetching remains a separate explicit network operation governed by its own safety policy.


## JSON Chat Persistence

v0.2 stores explicit chat files via `--save-chat PATH` and `--load-chat PATH` before adding automatic XDG chat IDs. This keeps the early REPL scriptable and reviewable while still using the target schema fields: `schema_version`, timestamps, provider, base URL, model, settings, messages, attachments, usage, and compaction events. Saves use a temporary file, fsync, rename, and restrictive file permissions.

## Runtime Jobs

v0.3 introduces `src/runtime/` with a small cancellation token, thread-safe event queue, and RAII `JobHandle`. The first users are the full-screen TUI foundation and cancellable provider requests. HTTP requests carry a cancellation token down to libcurl and abort through `CURLOPT_XFERINFOFUNCTION`, returning `PKCHAT_ERR_CANCELLED` instead of a generic transport error.

The initial model is one worker thread per active job. It is intentionally small: the owning UI loop receives events and remains the only code that mutates terminal/session state. This can grow into a queue or pooled runtime when TUI/web workloads need it.

## Full-Screen TUI Foundation

v0.3 adds `--chat` as an alternate-screen terminal UI, with `--tui` retained as a compatibility alias without adding an ncurses dependency yet. The TUI renders chat history, a status line, and a bounded bottom input panel without reserving persistent header rows for endpoint/model details. The input panel embeds `EditorState`, using the editor's rectangular renderer, soft wrap, and visual-row cursor movement so long multiline prompts behave like the standalone editor without taking over the whole screen. Chat requests, model listing, and chat save/load run as runtime jobs so terminal input remains responsive. A bare `Esc` cancels the active model request, while `Ctrl+R` queues regeneration by cancelling any active chat request and resending the last user prompt from the owning event loop. TUI colors use semantic roles and 24-bit ANSI SGR sequences so contrast is controlled instead of depending on terminal palette mappings. The `dark` theme is the default, `/theme dark|light` switches at runtime, and `--nocolors` disables color styling while preserving terminal control sequences. Theme foreground/background pairs are unit-tested against the WCAG 2.1 AA 4.5:1 contrast target, including the subdued tinted role used for visible thinking traces.


## Standalone Editor Foundation

`--editor` is a permanent bonus mode and a controlled test bed for the multiline editing layer now embedded in the chat TUI. The editor core uses a piece table: the original file and appended edit buffer are kept separately while visible text is represented by pieces. This keeps inserts and deletes local to the piece list instead of rewriting the whole buffer on every keystroke, which is a better fit for large files than a single mutable string.

Rendering is split from terminal I/O. `EditorState` renders into a caller-provided `Rect`, so one terminal window can eventually host multiple editor panels or embed the editor in a partial-screen chat layout. Long lines soft-wrap inside that rectangle, preferring whitespace breakpoints and hard-wrapping long words. Vertical movement has two modes: logical hard-line movement for file editing, and visual-row movement that treats wrapped overflow rows as cursor targets for TUI chat input. The current terminal harness uses POSIX `termios` and ANSI escape sequences because `ncursesw` was not available in the build environment; the core renderer is independent of that choice.


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

Image input is Chat Completions-only. Non-interactive repeated `--attach` accepts multiple images alongside text context, while REPL/TUI `/insert` and `/attach` queue images for the next prompt. The default limit is 20 MiB per image and can be lowered with `--max-image-bytes`. Base64 data is temporary request state: it is cleared after the provider call and never written into chat JSON. Conservative capability detection combines provider-registry support with recognized vision-model names; `--image-capability allow` is the explicit override for a verified compatible custom model.

## Bounded Text Attachments

Repeatable `--attach PATH` and REPL `/insert PATH` reuse the same case-insensitive document classifier and HTML/Markdown/plaintext conversion path as `--input`. Each local text file is read incrementally with a default 1 MiB `--max-input-bytes` cap, checked for binary NUL bytes, and validated as UTF-8 before its converted content becomes a visible user-context message. The raw read buffer is released after conversion; the transcript preserves the converted context that was actually sent.

PDF and DOCX remain unsupported binary types. Bidirectional PDF/Markdown and DOCX/Markdown conversion is deferred rather than approximated by inserting binary data or adding an unreviewed document dependency.

The same loader now backs TUI `/insert PATH` and its `/attach PATH` synonym in a runtime file job. Text becomes visible context immediately. Images are capability-checked and queued for exactly the next model turn without entering saved JSON. `/fetch URL` uses the file-job slot and delivers converted Markdown through the event queue. Workers check cancellation and never mutate the chat session directly.

## Request-Only Context Policies

`--max-context-bytes N` enables a conservative text-byte budget. The `error`, `truncate-oldest`, `summarize-oldest`, `summarize-middle`, and `provider-auto` policies operate on a temporary provider request vector. Summaries are bounded deterministic extracts and do not make nested model calls. The full `Session::messages` transcript remains unchanged, while successful compactions append structured `compaction_events` containing the policy, estimated byte counts, affected-message count, timestamp, and notice. This is explicitly a byte estimate and is not presented as an exact provider token count.

## JSONL Benchmark Datasets

Benchmark input starts with JSONL because it is streamable, diffable, Unicode-safe, easy to generate without an added dependency, and maps cleanly onto single- and multi-turn cases. `src/benchmark/` owns strict schema validation and bounded loading. The authoritative 50-case corpus remains the ordinary file `benchmarks/builtin.jsonl`; the Makefile converts that file into a generated C++ raw string so the default `builtin` dataset remains available after moving or installing the executable. The original JSONL and the opt-in long-context dataset are installed as data files for inspection and extension.

Benchmark result output is JSONL. One record is emitted for each measured turn and a final summary reports token estimates, timing/throughput aggregates, and completed, failed, and deadline-cancelled case runs. Warmups use the same execution path but are not emitted or counted as measurements. Multi-turn history includes actual prior assistant output. A bounded worker pool supports concurrent finite runs and duration-driven speed load. Speed is exclusive because combining a timed load test with evaluation labels would make run counts ambiguous; quality and refusal labels may be combined and share one model response per case. Quality/refusal scoring, regex expectations, aggregate percentiles, Ctrl+C cancellation, and Parquet/Hugging Face Datasets input remain explicit follow-up work rather than hidden approximations.
