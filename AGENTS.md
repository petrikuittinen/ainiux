# AGENTS.md

Project: `pkchat`

This file contains repository-level instructions for AI coding agents. Treat it as project guidance. The user's latest explicit instruction always wins, but otherwise follow this file and `PLANS.md`.

## Mission

Build `pkchat`: a fast, portable command-line, terminal, and local-web chat client for OpenAI and OpenAI-compatible APIs.

The program must be excellent as a scriptable CLI before it becomes a full-screen terminal UI or browser UI. Keep the core engine independent from the UI so the same request, provider, streaming, persistence, runtime/job, cancellation, memory-management, and error-handling code can be reused by command-line mode, REPL mode, TUI mode, local web mode, benchmark mode, and future agent mode.

## Current implementation stance

- Default implementation language: **C++17**.
- Do not use Rust or Go.
- Keep the code portable across Linux, BSD, macOS, and other POSIX-like systems where practical.
- Use a `Makefile` as the primary build entry point.
- Do not require C++20, C++23, C23, non-portable compiler extensions, or a package manager unless the user explicitly approves it.
- If the user later explicitly changes the project to C, prefer **C17** for practical compiler portability. Do not make C23 mandatory unless a specific feature justifies it.

Performance matters, but correctness, clear errors, safe credential handling, robust streaming, responsive UI behavior, zero memory leaks, and a clean architecture matter more. The main bottlenecks are endpoint latency, model inference, file I/O, and terminal/browser rendering, not C versus C++ overhead.

## Non-negotiable implementation criteria

### No memory leaks

The program must not have memory leaks. Every allocation and acquired resource must be released as soon as it is no longer needed, including on error paths, cancellation paths, timeout paths, interrupted streams, failed JSON parsing, failed file processing, and early returns.

Rules:

- Prefer stack objects, RAII, `std::string`, `std::vector`, and standard containers.
- Use `std::unique_ptr` for owned heap objects. Use `std::shared_ptr` only when shared ownership is truly required and documented.
- Do not use raw owning pointers in new C++ code.
- Wrap C resources in RAII types: `CURL*`, `curl_slist*`, JSON handles, `FILE*`, file descriptors, sockets, `DIR*`, terminal state, allocated buffers, and temporary files.
- When interoperating with C APIs, define ownership at the boundary: who allocates, who frees, and which function frees it.
- Allocation failures must be handled explicitly where applicable.
- Add leak checks to tests with AddressSanitizer/LeakSanitizer where available, and Valgrind or an equivalent leak checker where practical.
- A feature is not done until normal success paths and important failure paths have been checked for leaks.

If the project is converted to C, use explicit ownership comments, `goto cleanup` style cleanup blocks where helpful, and one obvious release path per function. Never duplicate complex cleanup logic across branches.

## Core priorities

Work in this order unless the user explicitly changes priorities:

1. Script-friendly non-interactive CLI.
2. Reliable HTTP transport and streaming parser.
3. Provider adapter/profile architecture.
4. LM Studio as a first-class local provider profile.
5. Clear, structured error handling.
6. Safe credential/config handling.
7. Memory ownership discipline and leak-check infrastructure.
8. JSON chat persistence and simple REPL.
9. Runtime/job layer for cancellable long-running work.
10. Full-screen TUI that never blocks on network, endpoint waits, or file processing.
11. Provider-specific capability detection and additional adapters.
12. Attachments and URL fetching.
13. Benchmark mode.
14. Local web server mode.
15. Local agent mode, only after the above is solid.

Do not start with the autonomous agent feature. It is intentionally late because it has security and sandboxing consequences. Web server mode must be implemented before agent mode.

## Repository layout

Use this layout unless the existing repository already has a better equivalent:

```text
.
├── AGENTS.md
├── PLANS.md
├── TODO.md
├── README.md
├── Makefile
├── include/
│   └── pkchat/
├── src/
│   ├── main.cpp
│   ├── cli/
│   ├── config/
│   ├── provider/
│   ├── http/
│   ├── runtime/
│   ├── chat/
│   ├── tui/
│   ├── web/
│   ├── unicode/
│   ├── tools/
│   ├── benchmark/
│   └── security/
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── fixtures/
│   └── mock_server/
└── docs/
    ├── decisions.md
    ├── api-compatibility.md
    ├── web-mode.md
    └── security.md
```

Keep dependencies isolated behind small wrapper modules so they can be replaced later.

## Dependencies

Use as few external libraries as practical, but do not reimplement complex infrastructure badly.

Acceptable baseline dependencies:

- `libcurl` for HTTP, HTTPS, proxy support, timeouts, and streaming callbacks.
- One JSON library behind a small internal facade. Do not hand-write a JSON parser.
- `ncursesw` for the later full-screen terminal UI.
- A Unicode helper library such as `utf8proc` or `libgrapheme` is acceptable when implementing serious Unicode text editing.
- A small, portable HTTP server library may be considered for local web mode, but only behind `src/web/` and only after documenting the dependency decision. A minimal in-tree local-only server is acceptable if it remains simple, tested, and safe.

Do not add a dependency just because it is convenient. Every dependency must be justified in `docs/decisions.md` or a short comment near the build configuration.

## Architecture rules

### Provider adapters/profiles are mandatory

Do not scatter provider-specific assumptions through the codebase. Route all model/API differences through `src/provider/`.

The internal provider interface should support, at minimum:

```text
list_models()
send_chat_request()
stream_chat_request()
cancel_request()
get_capabilities()
```

Capabilities should include:

```text
chat_completions
responses_api
streaming
usage_reporting
requires_bearer_key
optional_bearer_key
images
pdfs
file_uploads
file_urls
tool_calls
server_side_context_management
custom_headers
local_endpoint
```

Initial adapters/profiles:

```text
openai_chat          /v1/chat/completions
openai_responses     /v1/responses
custom_openai_chat   configurable OpenAI-compatible chat endpoint
lm_studio            local OpenAI-compatible profile, default http://localhost:1234/v1
```

Later adapters/profiles:

```text
openrouter
ollama
vllm
llama_cpp
```

Treat LM Studio as a first-class local compatibility profile, not merely as an undocumented custom URL. It is operationally close to a llama.cpp-style local OpenAI-compatible server, but it has its own defaults, aliases, documentation, and optional authentication behavior.

The UI must not know the difference between OpenAI Chat Completions, OpenAI Responses, OpenRouter, Ollama, vLLM, LM Studio, llama.cpp, or a custom endpoint.

### LM Studio profile rules

Support LM Studio explicitly because it is a popular local OpenAI-compatible endpoint.

Provider/profile names and aliases:

```text
lm_studio
lmstudio
lm-studio
```

Default profile settings:

```text
default_base_url: http://localhost:1234/v1
default_key_env: LMSTUDIO_API_KEY, then LM_STUDIO_API_KEY, then PKCHAT_API_KEY
requires_bearer_key: false by default
local_endpoint: true
```

Required compatibility support:

```text
GET  /v1/models
POST /v1/chat/completions
POST /v1/responses, when capability detection says it works
stream: true for streaming chat/responses, when supported
```

LM Studio authentication is optional. Do not require a key for `--provider lm_studio` unless the user provides one or the server returns an authentication error. If a key is configured, send it as a Bearer token just like other OpenAI-compatible APIs.

Provider quirks to document and test:

- The model identifier should come from `GET /v1/models`; do not assume OpenAI model names.
- The server is usually local and often has no authentication by default. Warn in docs that binding it to a LAN address has security implications.
- Treat capabilities as detected, not assumed. Some LM Studio versions/models may support Responses, tools, images, or structured output differently.
- Keep `lm_studio` separate from `llama_cpp` in profile names, even if much of the adapter code is shared.
- If connection to `localhost:1234` is refused, show a local-server-specific hint to start LM Studio's server and verify the port.

### HTTP and streaming rules

The HTTP layer belongs in `src/http/` and must be usable without the TUI or web UI.

Requirements:

- Use explicit connect timeout and total timeout settings.
- Support cancellation while a stream is active.
- Support proxy configuration where libcurl supports it.
- Redact credentials in debug logs.
- Never assume one network chunk equals one JSON object.
- Implement a real server-sent events parser for streaming.
- Correctly handle `data: [DONE]`, comments, blank event separators, partial chunks, malformed events, and UTF-8 split across chunks.
- Ensure all libcurl handles, header lists, buffers, and callback state are released on success, failure, and cancellation.

### Runtime/job layer rules

Put cancellable long-running work behind `src/runtime/` or an equivalent abstraction before implementing the full-screen TUI or local web UI.

Long-running jobs include:

```text
HTTP connect/request/streaming
model-list request
URL fetch
attachment read/process
text/PDF extraction
chat save/load on slow filesystems
context compaction/summarization
benchmark runs
future web requests that proxy to model calls
future agent tool execution
```

Requirements:

- The UI thread/event loop must never block on network I/O, DNS/TLS waits, endpoint response waits, streaming callbacks, file reads, file writes, URL fetching, attachment processing, PDF/text extraction, compaction, or benchmark jobs.
- Use worker threads, a job queue, non-blocking event loops, or another explicit runtime design. Pick one and document it in `docs/decisions.md`.
- All long-running jobs must support cancellation.
- Use thread-safe event delivery from workers to UI code.
- Workers must not mutate TUI state, web session state, or shared chat state directly. Send events to the owning loop instead.
- The TUI must remain usable while a model request is pending or streaming.
- Local web mode must continue accepting browser/UI requests while a model request, file processing job, or streaming response is in progress.
- Shutdown must cancel or join workers cleanly.
- No worker, queue, cancellation token, mutex, condition variable, or per-job allocation may leak.

### CLI rules

The CLI must be useful in shell scripts.

Default behavior:

- `stdout` is reserved for model output.
- `stderr` is for status, warnings, progress, and errors.
- Exit codes must distinguish success, bad arguments, network failure, API failure, config failure, and internal failure.
- Add `--format text|json|ndjson` before adding fancy output.
- Add `--no-stream` even if streaming is the default.

Required early examples:

```sh
pkchat http://localhost:8000 -p "What is the capital of Norway?"
pkchat --provider openai -m MODEL -p "Hello"
pkchat --provider lm_studio -m MODEL -p "Hello from LM Studio"
pkchat --provider lmstudio --list-models
pkchat --list-models http://localhost:8000
pkchat --prompt-file prompt.txt --system-file system.txt --format json
```

### Local web server mode rules

Local web mode is a normal product mode, but it must be implemented after the CLI, provider architecture, persistence, runtime/job layer, and benchmark mode are solid. It must be implemented before local agent mode.

Command shape:

```sh
pkchat -w
pkchat --web
pkchat --web 8080
pkchat --web=8080
pkchat --web-host 127.0.0.1 --web 8080
```

Rules:

- `-w` and `--web` start a local web server.
- If no port is provided, default to port `80`.
- Because binding to port 80 often requires elevated privileges on Unix-like systems, show a clear permission error and suggest `--web 8080` when binding fails for that reason.
- Bind to `127.0.0.1` by default, not all interfaces.
- Require an explicit option such as `--web-host 0.0.0.0` or `--web-allow-lan` before listening on LAN-visible addresses.
- Do not expose API keys, config secrets, chat files, or arbitrary local files through the web server.
- Disable permissive CORS by default. Do not allow arbitrary websites to control the local server.
- Use standard HTML widgets: `textarea` for input, `button` elements for actions, `select` for model/provider selection, and ordinary form controls for settings.
- Use a monospace font by default.
- Provide a light/dark mode toggle.
- Keep the browser UI visually similar to the text CLI/TUI chat: chat history, input area, status area, settings/model controls, and basic stats.
- Serve local assets only. Do not depend on external CDNs for core functionality.
- Use the same provider, chat, persistence, runtime/job, cancellation, and error-handling layers as CLI/TUI mode.
- Streaming to the browser should use a simple, documented mechanism such as Server-Sent Events or WebSocket. Do not block the HTTP accept loop or browser UI while waiting for model output.
- All per-session allocations, sockets, response buffers, and streaming state must be cleaned up when the session ends, the browser disconnects, or the user cancels generation.

### URL/base URL handling

Be helpful but deterministic.

Rules:

1. If the provided URL path already ends in `/v1`, use it as the base URL.
2. If the path is empty or `/`, try appending `/v1`.
3. Probe `GET /models` and `GET /v1/models` only when needed.
4. Cache or display the selected base URL only after it succeeds.
5. Allow explicit overrides with `--base-url`, `--chat-url`, `--models-url`, and later `--responses-url`.
6. Do not hide surprising URL rewrites. Show them on `stderr` unless `--quiet` is set.

### Error handling rules

Never emit vague errors such as `request failed` when more detail is available.

Use structured internal error categories such as:

```text
PKCHAT_ERR_BAD_ARGS
PKCHAT_ERR_BAD_URL
PKCHAT_ERR_DNS
PKCHAT_ERR_CONNECT
PKCHAT_ERR_TLS
PKCHAT_ERR_TIMEOUT
PKCHAT_ERR_HTTP_STATUS
PKCHAT_ERR_AUTH
PKCHAT_ERR_RATE_LIMIT
PKCHAT_ERR_JSON_PARSE
PKCHAT_ERR_SSE_PARSE
PKCHAT_ERR_PROVIDER_SCHEMA
PKCHAT_ERR_UNSUPPORTED_FEATURE
PKCHAT_ERR_FILE_READ
PKCHAT_ERR_FILE_WRITE
PKCHAT_ERR_CONFIG_CORRUPT
PKCHAT_ERR_WEB_BIND
PKCHAT_ERR_WEB_REQUEST
PKCHAT_ERR_MEMORY
PKCHAT_ERR_INTERNAL
```

Human-facing errors should include:

- What failed.
- Which URL, path, file, model, option, socket, or port was involved.
- HTTP status and provider error body when safe to show.
- What was tried.
- A concrete next step when possible.

Example:

```text
HTTP 404 from http://localhost:8000/v1/chat/completions: endpoint not found.
Tried base URL: http://localhost:8000/v1
Suggestion: check whether the server expects /v1, /api/v1, or a custom --chat-url.
```

### Credential handling rules

Prefer credentials from environment variables, key files, or stdin. Do not encourage command-line API keys.

Supported methods should include:

```text
OPENAI_API_KEY
OPENROUTER_API_KEY
LMSTUDIO_API_KEY
LM_STUDIO_API_KEY
PKCHAT_API_KEY
--key-env NAME
--key-file PATH
--key-stdin
--header "Name: Value"
```

If `-k` or `--key` is implemented, warn that command-line arguments may be visible to other local users unless `--quiet` is set.

Always redact these from logs, errors, traces, saved chats, web responses, and debug output:

```text
Authorization
api-key
x-api-key
cookie
set-cookie
```

### Persistence rules

Use XDG-style locations on Unix-like systems:

```text
config: $XDG_CONFIG_HOME/pkchat/config.json or ~/.config/pkchat/config.json
state:  $XDG_STATE_HOME/pkchat/chats/ or ~/.local/state/pkchat/chats/
data:   $XDG_DATA_HOME/pkchat/ or ~/.local/share/pkchat/
```

Chat files must include at least:

```text
schema_version
created_at
updated_at
provider
base_url
model
settings
messages
attachments
usage
compaction_events
```

Save chat/config files atomically:

```text
write temp file
fsync temp file where supported
rename temp file over destination
fsync parent directory where supported
```

Use restrictive permissions for files that may contain prompts, chat history, URLs, or secrets.

Every allocation, open file, temporary file, JSON object, and directory handle used during load/save must be released on success and failure.

### Unicode and terminal rules

UTF-8 byte preservation is not enough. Treat Unicode handling as a real feature.

Tests must include:

```text
Chinese: 你好
Arabic: مرحبا
Cyrillic: Привет
Finnish/Swedish characters: Ä Ö Å ä ö å
Combining marks: é
Emoji sequence: 👨‍👩‍👧‍👦
Invalid UTF-8 byte sequences
```

For the TUI/editor:

- Track grapheme clusters, not just bytes.
- Track terminal cell width.
- Do not split a UTF-8 sequence during streaming output.
- Do not assume Ctrl+Enter or Shift+Enter are portable.
- Prefer configurable key bindings.
- Default send key should be something realistically detectable, such as `Alt+Enter` or `Esc` followed by `Enter`, with fallback commands like `/send`.
- Keep `Ctrl+C` as cancel/interrupt by default, not copy.

### TUI rules

The TUI is not v1. Build it after CLI, simple REPL, runtime/job infrastructure, and persistence are reliable.

Target layout:

```text
chat history: remaining screen height
input area: floor(height / 5), minimum 3 lines when possible
status line: one line
```

Required behavior before visual polish:

- Terminal state is restored after crash or interrupt where possible.
- Window resize is handled.
- Streaming output does not corrupt the input editor.
- Raw mode/cbreak mode cleanup is robust.
- `/help`, `/quit`, `/model`, `/system`, `/temperature`, `/save`, `/load`, and `/models` work.
- The UI remains responsive while connecting, waiting for the endpoint, streaming output, loading/saving chats, processing attachments, fetching URLs, compacting context, or cancelling a job.
- The user can scroll, edit the current input, open help/options, and cancel an in-flight generation while a request is pending.

### Context and compaction rules

Never silently destroy the user's actual transcript.

Store the full transcript on disk. Only compact the version sent to the model.

Supported policies should eventually include:

```text
--context-policy error
--context-policy truncate-oldest
--context-policy summarize-oldest
--context-policy summarize-middle
--context-policy provider-auto
```

When compaction happens, show a clear notice such as:

```text
Context compacted: 42 earlier messages summarized into 1 message. Full transcript preserved on disk.
```

### Attachments and URL fetching rules

Attachments are provider-capability dependent. Do not fake support.

Text files may be inlined after size and encoding checks. Images, PDFs, and provider-native files require capability checks.

URL fetching must be explicit or clearly visible. Protect users from accidental local-network probing.

Default URL-fetching safety rules:

- Limit response size.
- Set a timeout.
- Limit redirects.
- Check content type.
- Block private, loopback, multicast, link-local, and metadata-service addresses unless explicitly allowed.
- Show which URL was fetched.
- Clean up all transfer handles, buffers, temporary files, parser objects, and attachment state after success, failure, or cancellation.

### Local agent mode rules

Agent mode is late-stage and must be separate from ordinary chat. Implement web server mode before implementing agent mode.

Command shape should be similar to:

```sh
pkchat agent [options]
```

Default safety model:

- Read-only current workspace.
- No shell execution by default.
- No network except the configured model endpoint by default.
- Ask before writing files.
- Ask before running commands.
- Ask before fetching URLs.
- Show exact commands before running them.
- Log all tool actions.
- Support `--workspace PATH` and `--sandbox none|basic|strict`.

Do not implement auto-executing local shell commands without an approval/sandbox design.

## Testing requirements

Add tests with every behavior change. Do not rely only on manual testing.

Minimum test categories:

```text
unit tests for CLI parsing
unit tests for URL normalization
unit tests for JSON request generation
unit tests for provider response parsing
unit tests for LM Studio profile defaults and aliases
unit tests for SSE parsing
unit tests for runtime cancellation and event delivery
unit tests for error formatting
unit tests for UTF-8 validation and boundary handling
unit tests for web-mode port parsing and route handling
integration tests with a mock OpenAI-compatible server
integration tests with a mock LM Studio-compatible server
streaming tests with arbitrary chunk boundaries
TUI responsiveness tests with slow mock endpoints
web UI responsiveness tests with slow mock endpoints and slow file jobs
HTTP 401, 403, 404, 429, 500 tests
connection drop mid-stream
malformed JSON
malformed SSE
empty input
overly long input
corrupted config and chat files
permission-denied file writes
permission-denied web bind to port 80
API key redaction tests
memory leak tests for success, error, cancellation, and interrupted-stream paths
```

When practical, add sanitizer and leak-check targets:

```sh
make sanitize
make test-sanitize
make leak-check
make test-leak
```

Do not claim tests passed unless you actually ran them.

## Build expectations

The Makefile should eventually support:

```sh
make
make test
make test-integration
make sanitize
make test-sanitize
make leak-check
make test-leak
make clean
make install PREFIX=/usr/local
```

Compiler warnings should be strict. Use at least:

```text
-Wall -Wextra -Wpedantic
```

Treat warnings as errors in CI or sanitizer builds when practical, but avoid making uncommon platform warnings block all users unnecessarily.

## Documentation expectations

Keep these files current:

- `README.md`: user-facing usage, examples, supported providers, build/install.
- `TODO.md`: short active task list.
- `PLANS.md`: roadmap, milestone acceptance criteria, implementation plan.
- `docs/api-compatibility.md`: provider quirks and supported endpoints.
- `docs/security.md`: credential, URL-fetching, web-mode, and agent-mode safety model.
- `docs/web-mode.md`: local web server usage, security model, endpoints, and UI behavior.
- `docs/decisions.md`: notable design decisions and dependency choices.

## Coding style

- Prefer small modules and explicit ownership.
- Avoid hidden global mutable state.
- Use RAII for resources such as CURL handles, files, sockets, JSON objects, terminal mode, web sessions, worker threads, and temporary files.
- Check every meaningful return value.
- Do not ignore errors from file I/O, socket I/O, terminal operations, JSON parsing, memory allocation, thread creation, locks, condition variables, or libcurl callbacks.
- Keep functions short enough to review.
- Keep provider-specific JSON shape code inside provider adapters.
- Do not mix terminal drawing code, web-server code, and HTTP/model-provider code.
- Avoid cleverness. The project is a reliability tool, not a demo.
- No memory leaks are acceptable. New code must have a clear ownership model.

## Definition of done

A change is not done until:

1. It builds with the documented command.
2. Relevant tests exist or there is a clear explanation why not.
3. Existing tests pass, if runnable.
4. Errors are specific and actionable.
5. Credentials are not leaked in logs, saved files, traces, terminal output, or web responses.
6. Documentation is updated when behavior changes.
7. The change does not break script-friendly stdout/stderr behavior.
8. The implementation fits the architecture in this file.
9. Long-running work is cancellable and does not block TUI/web UI loops where those modes are involved.
10. Leak checks pass for the touched code path where leak-check tooling is available.

## Git and worktree safety for agents

- Inspect the current worktree before making broad edits.
- Do not overwrite user changes.
- Do not run destructive git commands such as `git reset --hard` or `git checkout --` unless the user explicitly asks.
- Do not amend commits unless explicitly asked.
- Keep unrelated refactors out of feature patches.
- Prefer focused, reviewable changes.
