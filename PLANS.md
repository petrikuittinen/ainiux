# PLANS.md

Project: `pkchat`

This file is the implementation roadmap and execution-plan template for coding agents. Work from the earliest incomplete milestone unless the user explicitly asks for something else.

## Product goal

Create the best practical command-line, terminal, and local-web chat client for OpenAI and OpenAI-compatible APIs.

`pkchat` should be:

- Fast and portable.
- Excellent in scripts.
- Useful interactively.
- Careful with credentials.
- Clear when errors happen.
- Robust with streaming output.
- Unicode-aware.
- Provider-adapter/profile based, not hard-coded to one API dialect.
- Friendly to local endpoints, especially LM Studio and llama.cpp-style servers.
- Responsive in full-screen mode and web mode even while waiting for an endpoint, streaming, saving/loading chats, or processing files.
- Free of memory leaks.

## Product non-goals for early versions

Do not implement these before the core CLI, streaming, persistence, provider architecture, runtime/job layer, and memory/leak-check discipline are solid:

- Autonomous local agent mode.
- Full markdown renderer.
- PDF extraction.
- Image attachment support.
- Clipboard integration.
- Complex terminal key protocols.
- Browser automation.
- Plugin system.

Local web server mode is not an early feature, but it must be implemented before autonomous agent mode.

## High-level milestones

```text
v0.0  Repository skeleton, build system, and leak-check infrastructure
v0.1  Script-friendly CLI for Chat Completions plus LM Studio profile
v0.2  Simple interactive REPL and JSON chat persistence
v0.3  Runtime/job layer and non-blocking full-screen TUI foundation
v0.4  Provider adapters, Responses API, LM Studio refinement, and compatibility profiles
v0.5  Context management, attachments, and safe URL fetching
v0.6  Benchmark mode
v0.7  Local web server mode
v0.8  Local agent mode with sandbox/approval design
```

Each milestone should leave the program usable. Do not create a long-lived pile of half-wired features.

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

Create a minimal but clean project skeleton that builds a `pkchat` binary, gives future agents obvious places to put code, and establishes from the beginning that memory leaks are not acceptable.

## Tasks

- [ ] Create `README.md` with mission, build instructions, and first examples.
- [ ] Create `TODO.md` with active near-term tasks only.
- [ ] Create `docs/decisions.md`.
- [ ] Create `docs/security.md` stub.
- [ ] Create `docs/api-compatibility.md` stub.
- [ ] Create `docs/web-mode.md` stub.
- [ ] Create `include/pkchat/` for public/internal headers if needed.
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
- [ ] Make `pkchat --version` and `pkchat --help` work.

## Memory criteria

- [ ] The first skeleton program exits without leaks under available leak-check tooling.
- [ ] Any wrapper type introduced in v0.0 has deterministic cleanup.
- [ ] No raw owning heap pointer is introduced in C++ code.

## Acceptance criteria

- [ ] `make` builds `pkchat`.
- [ ] `./pkchat --help` prints useful usage text.
- [ ] `./pkchat --version` prints a version string.
- [ ] `make clean` removes build outputs.
- [ ] `make sanitize` or the documented platform equivalent builds successfully where supported.
- [ ] `make leak-check` or the documented platform equivalent can run on the skeleton binary where supported.
- [ ] `README.md`, `TODO.md`, `AGENTS.md`, and `PLANS.md` exist at repo root.

---

# v0.1 - Script-friendly CLI for Chat Completions plus LM Studio profile

## Goal

Implementation note (2026-06-14): v0.0 skeleton and a testable v0.1 CLI are now present. The CLI supports model listing, Chat Completions, LM Studio aliases, OpenRouter defaults, text/JSON/NDJSON output, credential lookup, mock-server integration tests, and sanitizer/leak-check targets. Remaining v0.1 hardening is tracked in TODO.md: replace the curl-executable transport fallback with libcurl RAII, expand JSON handling, add broader error-path tests, and add true incremental stream delivery from the HTTP layer.


Make `pkchat` useful from scripts and shells against `/v1/chat/completions` and `/v1/models`, including local LM Studio at `http://localhost:1234/v1`.

## Required user-facing commands

```sh
pkchat http://localhost:8000 -p "What is the capital of Norway?"
pkchat --base-url http://localhost:8000/v1 -p "Hello"
pkchat --provider openai -m MODEL -p "Hello"
pkchat --provider lm_studio -m MODEL -p "Hello from local LM Studio"
pkchat --provider lmstudio --list-models
pkchat --list-models http://localhost:8000
pkchat --prompt-file prompt.txt --system-file system.txt --format json
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
--format text|json|ndjson
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
  - [ ] `PKCHAT_API_KEY`
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
- [ ] If `LMSTUDIO_API_KEY`, `LM_STUDIO_API_KEY`, `PKCHAT_API_KEY`, `--key-*`, or `--header Authorization: ...` is provided, send the configured credential.
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

- [ ] `pkchat -p "Hello" --provider lm_studio -m MODEL` sends a request to `http://localhost:1234/v1/chat/completions` unless overridden.
- [ ] `pkchat --provider lmstudio --list-models` calls the LM Studio models endpoint.
- [ ] `pkchat http://localhost:8000 -p "Hello"` tries a sensible `/v1` base URL when needed.
- [ ] Streaming works against the mock server.
- [ ] Errors are specific.
- [ ] Credentials are redacted.
- [ ] stdout is clean in text mode.
- [ ] Leak-check tooling reports no leaks for v0.1 success and representative failure paths where supported.

---

# v0.2 - Simple interactive REPL and JSON chat persistence

## Goal

Add a simple line-oriented interactive mode and durable chat save/load without yet building the full-screen TUI.

## Commands

Interactive mode should support at least:

```text
/help
/quit
/models
/model MODEL
/system TEXT
/temperature VALUE
/save
/load CHAT_ID
/chat
/new
```

Command-line examples:

```sh
pkchat -i --provider lm_studio
pkchat --chat CHAT_ID
pkchat --resume
pkchat --new
```

## Tasks

- [ ] Add internal message model independent from provider JSON.
- [ ] Add conversation state object.
- [ ] Add line-oriented REPL.
- [ ] Add prompt history for the session.
- [ ] Add `/save` and `/load`.
- [ ] Add `/chat` listing.
- [ ] Add atomic save.
- [ ] Add corrupted chat-file handling.
- [ ] Add config/profile support.
- [ ] Add schema migration mechanism, even if only v1 exists.
- [ ] Ensure every loaded JSON document, temporary string, file handle, and conversation allocation is released.

## Chat file schema

Use JSON with a `schema_version` field.

Minimum fields:

```json
{
  "schema_version": 1,
  "id": "...",
  "created_at": "...",
  "updated_at": "...",
  "provider": "lm_studio",
  "base_url": "http://localhost:1234/v1",
  "model": "...",
  "settings": {
    "temperature": 0.7,
    "top_p": null,
    "max_output_tokens": null
  },
  "messages": [
    {
      "role": "user",
      "content": [
        { "type": "text", "text": "Hello" }
      ]
    }
  ],
  "attachments": [],
  "usage": [],
  "compaction_events": []
}
```

Do not save API keys in chat files.

## Storage locations

Use XDG-style paths:

```text
config: $XDG_CONFIG_HOME/pkchat/config.json or ~/.config/pkchat/config.json
state:  $XDG_STATE_HOME/pkchat/chats/ or ~/.local/state/pkchat/chats/
data:   $XDG_DATA_HOME/pkchat/ or ~/.local/share/pkchat/
```

## Atomic save requirements

- [ ] Write to a temporary file in the target directory.
- [ ] Flush and fsync where supported.
- [ ] Rename over the target.
- [ ] fsync parent directory where supported.
- [ ] Preserve or set restrictive permissions.
- [ ] Remove temporary file on failure.
- [ ] Release all temporary path strings and file handles on success or failure.

## Acceptance criteria

- [ ] A chat can be saved and loaded.
- [ ] Corrupted chat files produce a specific error without crashing.
- [ ] Permission-denied writes produce a specific error.
- [ ] Disk-full or short-write cases are handled where testable.
- [ ] Old chats can be listed.
- [ ] API keys are not saved.
- [ ] Leak-check tooling reports no leaks for save/load success, corrupted file, and failed write paths where supported.

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

## Initial key bindings

Use portable defaults first:

```text
Enter                insert newline or submit, depending on configured mode
Alt+Enter            send prompt where detectable
Esc then Enter       send prompt fallback
Ctrl+C               cancel active generation, then interrupt/exit when idle
Ctrl+D               quit when input is empty
PageUp/PageDown      scroll chat
/help                show help
/quit                quit
/send                send current prompt
```

Do not assume Ctrl+Enter or Shift+Enter are portable. They may be optional advanced bindings later.

## Later editor features

Defer until the base TUI is stable:

```text
insert/overwrite mode
undo stack
prompt history navigation
clipboard integration
mouse support
advanced key protocols
theme support
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

## Adapters/profiles

Implement or refine:

```text
openai_chat
openai_responses
custom_openai_chat
lm_studio
openrouter
ollama
vllm
llama_cpp
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

- [ ] Add a provider registry.
- [ ] Add profile lookup by name and alias.
- [ ] Add capability detection/probing.
- [ ] Add OpenAI Responses request generation.
- [ ] Add OpenAI Responses streaming parser mapping into internal events.
- [ ] Add provider-specific error normalization.
- [ ] Add docs for each supported provider.
- [ ] Ensure each adapter frees provider-specific request/response state on all paths.

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

- [ ] Provider registry can resolve all aliases.
- [ ] LM Studio remains explicitly supported and documented.
- [ ] A provider capability can be reported to CLI/TUI/web code without leaking provider internals.
- [ ] OpenAI Chat Completions and Responses paths both map into the internal message/event model.
- [ ] Unsupported features return clear `PKCHAT_ERR_UNSUPPORTED_FEATURE` errors.
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

- [ ] Preserve full transcript on disk.
- [ ] Compact only the provider-bound request context.
- [ ] Show a clear notice when compaction happens.
- [ ] Record compaction events in chat JSON.
- [ ] Distinguish exact provider-reported token counts from estimates.
- [ ] Release temporary compacted-context allocations immediately after request completion.

Example notice:

```text
Context compacted: 42 earlier messages summarized into 1 message. Full transcript preserved on disk.
```

## Attachments

Start with text files:

- [ ] `/insert PATH` in REPL/TUI.
- [ ] `--attach PATH` for CLI, if useful.
- [ ] Size limit.
- [ ] Encoding detection or explicit UTF-8 requirement.
- [ ] Clear error for binary files.
- [ ] Clear error for unreadable files.
- [ ] Clear error for unsupported provider-native attachment types.
- [ ] Release file buffers after the provider request is built or after failure.

Later provider-native inputs:

```text
images
PDFs
file uploads
file URLs
```

Rules:

- [ ] Check provider capabilities first.
- [ ] Do not fake PDF support by blindly dumping binary data into a prompt.
- [ ] If local extraction is added, document the dependency and limitations.
- [ ] Use size limits and timeouts.
- [ ] Clean up temporary extracted text/files on success, failure, and cancellation.

## Safe URL fetching

Feature examples:

```text
summarize https://example.com/article
pkchat --fetch-url https://example.com/article -p "Summarize this"
```

Safety defaults:

- [ ] Make fetching explicit or clearly visible.
- [ ] Limit response size.
- [ ] Set connect and total timeouts.
- [ ] Limit redirects.
- [ ] Check content type.
- [ ] Block private addresses by default:
  - [ ] loopback
  - [ ] link-local
  - [ ] multicast
  - [ ] RFC1918 private ranges
  - [ ] metadata-service addresses
- [ ] Add override such as `--allow-private-url-fetch` only with clear warnings.
- [ ] Show which URL was fetched.
- [ ] Handle charset conversion or reject unsupported encodings clearly.
- [ ] Release transfer handles, buffers, parsers, and temporary files after success, failure, timeout, and cancellation.

## Acceptance criteria

- [ ] Context compaction never modifies the full saved transcript destructively.
- [ ] Text file insertion works for UTF-8 text files.
- [ ] Binary/unreadable/too-large files produce clear errors.
- [ ] URL fetching refuses private addresses by default.
- [ ] URL fetch timeout and max-size limits work.
- [ ] TUI remains responsive while file insertion or URL fetching is in progress.
- [ ] Leak-check tooling reports no leaks for attachment and URL-fetch success/failure/cancellation paths where supported.

---

# v0.6 - Benchmark mode

## Goal

Add repeatable benchmarking for endpoint/model behavior without turning it into misleading marketing numbers.

## Command shape

```sh
pkchat benchmark --provider lm_studio -m MODEL --prompt-file bench.txt --runs 10 --warmup 2
pkchat benchmark --base-url http://localhost:8000/v1 -m MODEL --format csv
pkchat benchmark --provider openai -m MODEL --concurrency 4 --runs 20 --format json
```

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
--prompt-file PATH
--prompt TEXT
--max-output-tokens N
--temperature FLOAT
--top-p FLOAT
--seed N
--stream
--no-stream
--timeout SECONDS
--format table|json|csv|ndjson
--output PATH
```

## Benchmark integrity rules

- [ ] Print model, provider, base URL, prompt size, settings, and timestamp.
- [ ] Separate warmup runs from measured runs.
- [ ] Report p50/p90/p99 where enough samples exist.
- [ ] Do not compare different models/settings as if equivalent.
- [ ] Distinguish provider-reported tokens from estimated tokens.
- [ ] Keep benchmark jobs cancellable.
- [ ] Release all per-run request/response/timing allocations after each run.

## Acceptance criteria

- [ ] Benchmark mode works against mock server.
- [ ] Benchmark mode works against LM Studio when running locally.
- [ ] CSV and JSON output are parseable.
- [ ] Failed runs are counted and reported.
- [ ] Ctrl+C/cancellation stops benchmark cleanly.
- [ ] Leak-check tooling reports no leaks after repeated benchmark runs where supported.

---

# v0.7 - Local web server mode

## Goal

Add a local browser UI for `pkchat` without compromising the CLI/TUI architecture or blocking behavior.

`pkchat -w` or `pkchat --web 8080` should launch a local web server at the designated port. If no port is provided, use port `80`. The browser GUI should resemble the text CLI/TUI chat but use standard HTML widgets.

## Command shape

```sh
pkchat -w
pkchat --web
pkchat --web 8080
pkchat --web=8080
pkchat --web-host 127.0.0.1 --web 8080
pkchat --web-host 0.0.0.0 --web 8080 --web-allow-lan
```

## CLI behavior

- [ ] `-w` starts web mode.
- [ ] `--web` starts web mode on default port `80`.
- [ ] `--web PORT` and `--web=PORT` start web mode on the selected port.
- [ ] Invalid ports produce a specific error.
- [ ] Permission failure on port 80 produces a clear message and suggests `--web 8080`.
- [ ] The server binds to `127.0.0.1` by default.
- [ ] Binding to non-loopback addresses requires an explicit opt-in such as `--web-allow-lan`.
- [ ] Startup prints the local URL on `stderr`, not `stdout`, so script output remains clean where relevant.

## Web UI requirements

Use simple, durable HTML first. Do not start with a large frontend framework.

Required UI elements:

- [ ] Chat history area.
- [ ] `textarea` for input.
- [ ] `button` for Send.
- [ ] `button` for Stop/Cancel.
- [ ] `button` or link for New chat.
- [ ] `select` or equivalent for model selection when models are available.
- [ ] Provider/base URL/settings controls.
- [ ] Status/error area.
- [ ] Basic stats area for TTFT, token counts, and token/s when available.
- [ ] Light/dark mode toggle.
- [ ] Monospace font by default.

Visual rule: the browser UI should feel like the text UI moved into a page, not like a separate unrelated product.

## HTML/CSS/JS rules

- [ ] Use standard HTML widgets.
- [ ] Use semantic labels for form controls.
- [ ] Use a monospace font stack.
- [ ] Support light and dark mode.
- [ ] Persist theme preference in browser-local storage if small client-side JS exists.
- [ ] Do not depend on external CDNs for core functionality.
- [ ] Keep JavaScript small and auditable.
- [ ] Escape model output before inserting into the DOM.
- [ ] Do not render arbitrary HTML from model output as trusted content.

## Server architecture

The web server must use the same core layers as the CLI/TUI:

```text
web request/session
  -> runtime/job layer
  -> chat/conversation layer
  -> provider adapter
  -> http transport
```

Requirements:

- [ ] The HTTP accept loop or request dispatcher must not block while a model call is pending.
- [ ] File processing from the web UI must run as a cancellable runtime job.
- [ ] Streaming output must be delivered incrementally to the browser.
- [ ] Use Server-Sent Events or WebSocket for streaming; document the choice in `docs/decisions.md`.
- [ ] Support cancellation from the browser Stop button.
- [ ] Keep per-session state isolated.
- [ ] Clean up sessions after browser disconnect or timeout.
- [ ] Cleanly close sockets on shutdown.
- [ ] Release all request buffers, response buffers, session objects, route state, sockets, and streaming state on success, error, disconnect, and cancellation.

## Suggested internal routes

The exact route names may change, but keep the API small and documented.

```text
GET  /                         HTML UI
GET  /app.css                  CSS, if not embedded
GET  /app.js                   JS, if not embedded
GET  /api/models               list models
GET  /api/chats                list saved chats
POST /api/chat                 submit a prompt
POST /api/cancel               cancel active generation
POST /api/settings             update model/system/temperature/etc.
GET  /api/events?session=ID    streaming events if using SSE
```

## Security rules

- [ ] Bind to loopback by default.
- [ ] Refuse or strongly warn before binding to LAN-visible addresses.
- [ ] Disable permissive CORS by default.
- [ ] Do not expose arbitrary filesystem paths.
- [ ] Do not expose API keys or Authorization headers to the browser.
- [ ] Redact secrets in server logs.
- [ ] Use a generated local session token or equivalent protection if listening beyond loopback.
- [ ] Add basic security headers where practical.
- [ ] Do not add agent/tool execution to web mode until agent mode has its own sandbox/approval design.

## Tests

- [ ] Unit test web option parsing.
- [ ] Unit test port validation.
- [ ] Unit test route matching.
- [ ] Unit test HTML escaping.
- [ ] Unit test theme toggle assets if client JS is testable.
- [ ] Integration test `GET /` returns usable HTML containing textarea, buttons, and theme toggle.
- [ ] Integration test `GET /api/models` with mock provider.
- [ ] Integration test `POST /api/chat` starts a runtime job.
- [ ] Integration test streaming events reach the browser client incrementally.
- [ ] Integration test Stop/Cancel cancels the provider request.
- [ ] Test that slow provider response does not block serving another static/UI request.
- [ ] Test that slow file processing does not block the web server event loop.
- [ ] Test browser disconnect cleanup.
- [ ] Test bind failure and permission-denied errors.
- [ ] Leak-check server startup/shutdown, one successful chat, one cancelled chat, and one browser disconnect path where supported.

## Acceptance criteria

- [ ] `pkchat --web 8080` starts a local server and prints the URL.
- [ ] `pkchat -w` attempts port 80 by default and gives a clear actionable error if binding is not permitted.
- [ ] The UI contains a chat history, textarea, Send button, Stop button, status area, and light/dark toggle.
- [ ] The UI uses a monospace font.
- [ ] A prompt can be sent from the browser to a mock provider and streamed back.
- [ ] The browser Stop button cancels an active generation.
- [ ] The server remains responsive while waiting for the provider or processing an input file.
- [ ] Secrets are not exposed in HTML, JS, JSON responses, logs, or saved chat files.
- [ ] Leak-check tooling reports no leaks for representative web-mode paths where supported.

---

# v0.8 - Local agent mode with sandbox/approval design

## Goal

Add a deliberately constrained local agent mode after the normal chat, TUI, web, provider, attachment, and benchmark features are stable.

Agent mode must be separate from ordinary chat. It should never silently gain filesystem, network, or shell powers inside normal chat.

Command shape:

```sh
pkchat agent [options]
```

## Instruction discovery

Support project instruction files only with clear rules:

```text
AGENTS.md
PLAN.md
PLANS.md
```

Rules to define before implementation:

- [ ] Which directories are searched.
- [ ] Whether parent instructions apply.
- [ ] Whether nested instructions override parent instructions.
- [ ] Whether instructions are displayed or summarized to the user.
- [ ] How conflicts are handled.
- [ ] Whether instructions can request dangerous behavior.

## Default safety model

Default agent mode must be conservative:

- [ ] Read-only current workspace.
- [ ] No shell execution by default.
- [ ] No network except configured model endpoint by default.
- [ ] Ask before writing files.
- [ ] Ask before running commands.
- [ ] Ask before fetching URLs.
- [ ] Show exact commands before running them.
- [ ] Log all tool actions.
- [ ] Support `--workspace PATH`.
- [ ] Support `--sandbox none|basic|strict`.
- [ ] Support pause/cancel.
- [ ] Release all per-tool buffers, command output buffers, file lists, and approval state after each action.

## Tool actions

Potential future tools:

```text
read file
list directory
write file with approval
apply patch with approval
fetch URL with approval
run command with approval
```

Every tool must have:

- [ ] clear input schema
- [ ] output size limit
- [ ] timeout
- [ ] cancellation
- [ ] logging
- [ ] approval rules
- [ ] cleanup rules

## Hard blocks unless explicitly overridden

- [ ] Deleting broad paths.
- [ ] Reading secrets by default.
- [ ] Running destructive shell commands.
- [ ] Running network scanners.
- [ ] Writing outside workspace.
- [ ] Auto-executing downloaded scripts.

## Pause/cancel behavior

```text
Esc pauses agent loop in TUI where detectable.
/stop pauses or cancels agent loop in REPL/web/TUI.
Ctrl+C cancels current action first.
```

## Acceptance criteria

- [ ] Agent mode is separate from normal chat.
- [ ] No write/command/network action happens without approval under default settings.
- [ ] Actions are logged.
- [ ] Pause/cancel works.
- [ ] Sandbox behavior is documented.
- [ ] Leak-check tooling reports no leaks for representative approved, denied, failed, and cancelled tool actions where supported.

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
pkchat: HTTP 404 from http://localhost:8000/v1/chat/completions
provider message: endpoint not found
tried base URL: http://localhost:8000/v1
suggestion: check whether the server expects /v1, /api/v1, or a custom --chat-url
```

Web-mode errors should be returned as JSON for API routes and as visible status messages in the browser UI. Do not expose secrets.

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
11  web server bind/listen error
12  internal error
```

## Security checklist

- [ ] API keys are never saved in chats.
- [ ] API keys are redacted in logs/errors/traces.
- [ ] `-k` warns unless quiet.
- [ ] Key files use restrictive permissions when practical.
- [ ] URL fetch blocks private addresses by default.
- [ ] Web mode binds to loopback by default.
- [ ] Web mode does not expose secrets to the browser.
- [ ] Web mode disables permissive CORS by default.
- [ ] LAN web mode requires explicit opt-in and extra protection.
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
- [ ] Web server routes and streaming.
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
- [ ] Local web mode usage and security notes.
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
- Browser-based markdown preview in web mode.
- Multi-user web mode. This is explicitly not the default local web mode.
