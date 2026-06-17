# Decisions

## C++17 and Makefile

The initial implementation uses C++17 and a plain Makefile to keep the binary portable across POSIX-like systems.

## HTTP Transport

`src/http/` owns HTTP transport and uses libcurl through RAII wrappers for the easy handle and header list. The Makefile discovers build flags with `pkg-config libcurl`, falling back to `curl-config`. Streaming response bodies are delivered through libcurl write callbacks so provider code can parse SSE incrementally without spawning the `curl` executable.

## JSON Facade

`src/json/` is a small internal JSON facade used for request escaping and provider response parsing. The project should replace or expand it with a reviewed JSON library when dependency installation is available.


## Provider Registry and API Adapters

v0.4 begins with a data-driven provider registry in `src/provider/`. Built-in profiles carry aliases, default base URLs, endpoint paths, key environment variables, local/remote flags, optional dummy keys, compatibility warnings, and client capability flags. The OpenAI-compatible providers share the same Chat Completions adapter instead of duplicating provider-specific request code.

Responses API support is a sibling adapter selected with `--api responses`, `--responses`, or the `openai_responses` shortcut. It reuses the existing HTTP transport, cancellation token, timing, redaction, and streaming callback path. The first slice is text-only and maps Responses output text and SSE text deltas into the same internal assistant message model used by Chat Completions. Non-text Responses features remain disabled in reported client capabilities until implemented.


## JSON Chat Persistence

v0.2 stores explicit chat files via `--save-chat PATH` and `--load-chat PATH` before adding automatic XDG chat IDs. This keeps the early REPL scriptable and reviewable while still using the target schema fields: `schema_version`, timestamps, provider, base URL, model, settings, messages, attachments, usage, and compaction events. Saves use a temporary file, fsync, rename, and restrictive file permissions.

## Runtime Jobs

v0.3 introduces `src/runtime/` with a small cancellation token, thread-safe event queue, and RAII `JobHandle`. The first users are the full-screen TUI foundation and cancellable provider requests. HTTP requests carry a cancellation token down to libcurl and abort through `CURLOPT_XFERINFOFUNCTION`, returning `PKCHAT_ERR_CANCELLED` instead of a generic transport error.

The initial model is one worker thread per active job. It is intentionally small: the owning UI loop receives events and remains the only code that mutates terminal/session state. This can grow into a queue or pooled runtime when TUI/web workloads need it.

## Full-Screen TUI Foundation

v0.3 adds `--tui` as an alternate-screen terminal UI without adding an ncurses dependency yet. The TUI renders chat history, a status line, and a bounded bottom input panel without reserving persistent header rows for endpoint/model details. The input panel embeds `EditorState`, using the editor's rectangular renderer, soft wrap, and visual-row cursor movement so long multiline prompts behave like the standalone editor without taking over the whole screen. Chat requests, model listing, and chat save/load run as runtime jobs so terminal input remains responsive. A bare `Esc` cancels the active model request, while `Ctrl+R` queues regeneration by cancelling any active chat request and resending the last user prompt from the owning event loop. TUI colors use semantic roles and 24-bit ANSI SGR sequences so contrast is controlled instead of depending on terminal palette mappings. The `dark` theme is the default, `/theme dark|light` switches at runtime, and `--nocolors` disables color styling while preserving terminal control sequences. Theme foreground/background pairs are unit-tested against the WCAG 2.1 AA 4.5:1 contrast target, including the subdued tinted role used for visible thinking traces.


## Standalone Editor Foundation

`--editor` is a permanent bonus mode and a controlled test bed for the multiline editing layer now embedded in the chat TUI. The editor core uses a piece table: the original file and appended edit buffer are kept separately while visible text is represented by pieces. This keeps inserts and deletes local to the piece list instead of rewriting the whole buffer on every keystroke, which is a better fit for large files than a single mutable string.

Rendering is split from terminal I/O. `EditorState` renders into a caller-provided `Rect`, so one terminal window can eventually host multiple editor panels or embed the editor in a partial-screen chat layout. Long lines soft-wrap inside that rectangle, preferring whitespace breakpoints and hard-wrapping long words. Vertical movement has two modes: logical hard-line movement for file editing, and visual-row movement that treats wrapped overflow rows as cursor targets for TUI chat input. The current terminal harness uses POSIX `termios` and ANSI escape sequences because `ncursesw` was not available in the build environment; the core renderer is independent of that choice.


## Document Extraction Modules

v0.5 starts document extraction with a separate `src/html/` module. The HTML converter is intentionally small, uses C++17 `std::regex` for simple tag and attribute matching, and converts easy page content to plaintext or Markdown without adding another parser dependency. Script/style/noscript removal uses a linear scanner instead of a broad regular expression so large real pages do not trigger catastrophic regex recursion. The first slice handles headings, bold/strong, emphasis/italic, links, line breaks, and simple block spacing. It tolerates common malformed HTML by ignoring unknown or broken tags while preserving text. Input is validated as UTF-8 at the extraction boundary; legacy charset conversion is deferred, so invalid bytes produce an explicit unsupported-feature error instead of mojibake or crashes. It does not execute JavaScript or attempt to be a browser-grade HTML parser.

Future PDF and Word extraction should live in separate modules such as `src/pdf/` and `src/word/` rather than growing the HTML module.

## URL Fetching First Slice

`--fetch-url` is an explicit extraction mode, not hidden prompt injection. The CLI path uses the existing libcurl RAII wrapper, sends browser-style `User-Agent`, `Accept`, `Accept-Language`, and `Upgrade-Insecure-Requests` headers, captures `Content-Type`, applies a hard `max_body_bytes` cap in the HTTP write callback before appending oversized chunks, sets a fetch-mode total timeout when the user did not provide one, and refuses private/loopback/link-local/multicast/metadata literal hosts unless `--allow-private-url-fetch` is provided. Redirect following remains disabled in this slice.

This is not the final URL safety model. DNS-resolved private-address blocking, charset conversion, allow/block domain configuration, and runtime-job integration for TUI/web fetches remain v0.5 follow-up work.
