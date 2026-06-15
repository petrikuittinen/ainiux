# Decisions

## C++17 and Makefile

The initial implementation uses C++17 and a plain Makefile to keep the binary portable across POSIX-like systems.

## HTTP Transport

`src/http/` owns HTTP transport and uses libcurl through RAII wrappers for the easy handle and header list. The Makefile discovers build flags with `pkg-config libcurl`, falling back to `curl-config`. Streaming response bodies are delivered through libcurl write callbacks so provider code can parse SSE incrementally without spawning the `curl` executable.

## JSON Facade

`src/json/` is a small internal JSON facade used for request escaping and provider response parsing. The project should replace or expand it with a reviewed JSON library when dependency installation is available.


## JSON Chat Persistence

v0.2 stores explicit chat files via `--save-chat PATH` and `--load-chat PATH` before adding automatic XDG chat IDs. This keeps the early REPL scriptable and reviewable while still using the target schema fields: `schema_version`, timestamps, provider, base URL, model, settings, messages, attachments, usage, and compaction events. Saves use a temporary file, fsync, rename, and restrictive file permissions.

## Runtime Jobs

v0.3 introduces `src/runtime/` with a small cancellation token, thread-safe event queue, and RAII `JobHandle`. The first users are the full-screen TUI foundation and cancellable provider requests. HTTP requests carry a cancellation token down to libcurl and abort through `CURLOPT_XFERINFOFUNCTION`, returning `PKCHAT_ERR_CANCELLED` instead of a generic transport error.

The initial model is one worker thread per active job. It is intentionally small: the owning UI loop receives events and remains the only code that mutates terminal/session state. This can grow into a queue or pooled runtime when TUI/web workloads need it.

## Full-Screen TUI Foundation

v0.3 adds `--tui` as an alternate-screen terminal UI without adding an ncurses dependency yet. The TUI renders endpoint/model status, chat history, a status line, and a bounded bottom input panel. The input panel embeds `EditorState`, using the editor's rectangular renderer, soft wrap, and visual-row cursor movement so long multiline prompts behave like the standalone editor without taking over the whole screen. Chat requests, model listing, and chat save/load run as runtime jobs so terminal input remains responsive, and `Ctrl+C` cancels the active job.


## Standalone Editor Foundation

`--editor` is a permanent bonus mode and a controlled test bed for the multiline editing layer now embedded in the chat TUI. The editor core uses a piece table: the original file and appended edit buffer are kept separately while visible text is represented by pieces. This keeps inserts and deletes local to the piece list instead of rewriting the whole buffer on every keystroke, which is a better fit for large files than a single mutable string.

Rendering is split from terminal I/O. `EditorState` renders into a caller-provided `Rect`, so one terminal window can eventually host multiple editor panels or embed the editor in a partial-screen chat layout. Long lines soft-wrap inside that rectangle, preferring whitespace breakpoints and hard-wrapping long words. Vertical movement has two modes: logical hard-line movement for file editing, and visual-row movement that treats wrapped overflow rows as cursor targets for TUI chat input. The current terminal harness uses POSIX `termios` and ANSI escape sequences because `ncursesw` was not available in the build environment; the core renderer is independent of that choice.
