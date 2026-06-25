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

## Deferred product work

The CLI, streaming, persistence, provider architecture, runtime/job layer, TUI foundation, and first attachment layer now exist. Keep the following work outside the v0.6 configuration milestone unless it is required to integrate configuration safely:

- Autonomous local agent mode.
- Full rich Markdown rendering in the TUI.
- PDF and DOCX input/output conversion.
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
v0.6  System and user TOML-alike configuration files
v0.7  Benchmark mode
v0.8  AI-assisted editor
v0.9  Local web server mode
v1.0  Local agent mode with sandbox/approval design
v1.1  Image generation from CLI, REPL, TUI, and web chat
```

Each milestone should leave the program usable. Do not create a long-lived pile of half-wired features.

## Current baseline

Implementation status (2026-06-26): `pkchat` is at v0.75. The repository has the scriptable CLI, built-in provider registry and aliases, Chat Completions, text-only Responses API support, streaming SSE, credential lookup, JSON chat persistence, cancellable runtime jobs, REPL, full-screen TUI foundation, editor, request-only context policies, context-use estimates, bounded text/HTML/Markdown input, JPEG/PNG/GIF image input for Chat Completions, safe URL fetching, Markdown output conversion, automatic v0.6 system/user TOML-alike configuration loading, and concurrent JSONL benchmark execution. `--provider none` supports local conversion and editor workflows without a model endpoint.

Runtime defaults live in `cli::Options`, provider defaults live in `src/provider/`, and API keys are resolved while building the provider request context. Automatic system and user config files map into a base `cli::Options`, after which `main.cpp` parses CLI arguments over that base. `--no-config` can bypass the automatic user file while retaining system configuration, and `--debug` reports configuration discovery on `stderr`.

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

Implementation note (2026-06-15): v0.3 is now present. The CLI supports model listing, Chat Completions, LM Studio aliases, OpenRouter defaults, text/JSON/NDJSON output, credential lookup, provider shortcuts, a simple REPL, JSON chat persistence, a standalone multiline `--editor` foundation, editor-backed TUI multiline input, a cancellable runtime/job layer, a non-blocking full-screen TUI foundation, a libcurl RAII transport, incremental SSE streaming, mock-server integration tests, sanitizer checks, and Valgrind leak checks. Remaining hardening is tracked in TODO.md: expand JSON handling, add broader error-path and credential-redaction tests, and continue v0.2 persistence work such as XDG chat IDs and chat listing.


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

Implementation note (2026-06-14): a first v0.2 slice is implemented. `--repl`/`-i` starts a line-oriented REPL, `--save-chat PATH` and `--load-chat PATH` persist explicit JSON chat files, one-shot mode can continue a saved chat, and the mock integration test covers save/load plus REPL stdout behavior. Remaining v0.2 work is tracked in TODO.md: XDG chat IDs, `/chat` listing, `/new`, fuller config/profile support, and schema migration mechanics.

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

- [x] Add internal message model independent from provider JSON.
- [x] Add conversation state object.
- [x] Add line-oriented REPL.
- [x] Add prompt history for the session.
- [x] Add `/save` and `/load`.
- [ ] Add `/chat` listing.
- [x] Add atomic save.
- [x] Add corrupted chat-file handling.
- [ ] Add config/profile support.
- [ ] Add schema migration mechanism, even if only v1 exists.
- [x] Ensure every loaded JSON document, temporary string, file handle, and conversation allocation is released.

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

- [x] A chat can be saved and loaded.
- [x] Corrupted chat files produce a specific error without crashing.
- [ ] Permission-denied writes produce a specific error.
- [ ] Disk-full or short-write cases are handled where testable.
- [ ] Old chats can be listed.
- [x] API keys are not saved.
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
Alt+R                regenerate last answer in chat mode
Ctrl+Q               quit chat/editor mode
Ctrl+C               reserved for future copy support in chat/editor mode
Ctrl+D               quit when input is empty
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
clipboard integration
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
- [x] Unsupported features return clear `PKCHAT_ERR_UNSUPPORTED_FEATURE` errors.
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

- [x] `/insert PATH` and `/attach PATH` in REPL for text and images.
- [x] `/insert PATH` and `/attach PATH` in TUI through a cancellable runtime job.
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
pkchat --fetch-url https://example.com/article -p "Summarize this"
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

Implementation note (2026-06-22): `config/pkchat.conf` is the system-wide template aligned with the v0.6 `cli::Options` defaults. `make install` places it at `${SYSCONFDIR}/xdg/pkchat/config.conf` with mode `0644` and preserves an existing file. `src/config/` provides a bounded parser, complete initial-schema validation, XDG system/user discovery, transactional application, and CLI-last option merging. TUI theme/thinking defaults and URL-fetch private-address policy are wired to effective configuration. `--no-config` skips the user file only, and `--debug` reports file discovery/load state without printing values. Repeatable explicit `--config` layering was rejected as unnecessary complexity.

## File syntax

Call the format "pkchat config" or "TOML-alike" in documentation, not TOML. Use `config.conf` so users do not reasonably expect a general TOML parser.

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
- [x] Reject malformed section headers, trailing text after a section, invalid names, integer overflow, non-finite floats, and unterminated strings with a `PKCHAT_ERR_CONFIG` error.
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
2. system files named `pkchat/config.conf` from `$XDG_CONFIG_DIRS`, or `/etc/xdg` when unset
3. `$XDG_CONFIG_HOME/pkchat/config.conf`, or `$HOME/.config/pkchat/config.conf` when unset
4. command-line options and the positional `BASE_URL|PROFILE` shortcut

Implementation details:

- [x] Because `$XDG_CONFIG_DIRS` lists higher-priority directories first, load existing system files in reverse order so the first directory wins after merging.
- [x] Follow XDG path validity rules: ignore relative `$XDG_CONFIG_DIRS` entries, and use the documented fallback when `$XDG_CONFIG_HOME` is empty or relative. If `HOME` is also unavailable, skip automatic user-config discovery without preventing system configuration.
- [x] `--no-config` skips the automatic user file while retaining system configuration.
- [x] Missing automatic files are normal; an existing but unreadable or invalid automatic file is an error.
- [x] Bound each config file to 1 MiB and reject non-regular files with a specific error.
- [x] Use the existing preliminary CLI parse to detect `--no-config`, `--help`, `--version`, `--debug`, and `--quiet`, then run the full parser over configured base options.
- [x] `--help` and `--version` must work without reading config files, including when an automatically discovered file is malformed.
- [x] XDG and `HOME` affect path discovery. Provider-specific environment variables and an explicitly selected `key_env` continue to supply credentials after files are merged. Do not invent a second general `PKCHAT_*` environment-settings layer in this milestone.
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

0. [x] Add the common `config/pkchat.conf` template and install it without overwriting an existing administrator-managed file.
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
pkchat benchmark --provider lm_studio -m MODEL --runs 10 --warmup 2
pkchat benchmark --dataset cases.jsonl --base-url http://localhost:8000/v1 -m MODEL
pkchat benchmark --validate-dataset
pkchat --benchmark --dataset prompts.jsonl --mode speed --concurrency 4 --duration 60s
pkchat --benchmark --dataset benchmarks/long-context.jsonl --mode long-context --provider openai -m MODEL
pkchat --benchmark --dataset eval.jsonl --mode quality,refusals --output results/
```

## Dataset formats

- [x] Implement strict, bounded UTF-8 JSONL input first.
- [x] Add an embedded 60-case built-in JSONL corpus with ten safety, twenty reasoning, ten writing, ten coding, and ten multi-turn cases.
- [x] Add optional `fetch_url` cases and a separate Project Gutenberg long-context dataset.
- [x] Add category, case-ID, and count filtering plus offline validation/listing.
- [ ] Add Parquet input compatible with Hugging Face Datasets after JSONL behavior stabilizes.

Each JSONL object uses required `id`, `category`, and `turns` fields, with optional `language`, `tags`, `fetch_url`, and deterministic `expect` exact/contains scorers. Reasoning, math, and trivia cases require `reference_answer`; writing, coding, multi-turn, and long-context cases require `assessment_criteria`. Safety cases require a harmful/harmless classification and matching reject/answer action, with assessment criteria required for harmless requests. Generated assistant replies are appended between turns so multi-turn cases exercise actual conversation state.

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
- [x] Label built-in safety cases as harmful/reject or harmless/answer, requiring criteria for harmless answers.
- [x] Preserve prompts, tags, external-source links, answer keys, and rubrics in JSONL and Markdown result artifacts for future judge input.
- [ ] Add automatic rubric/judge scoring; evaluation metadata remains descriptive until judge behavior is specified and tested.

## Acceptance criteria

- [x] Benchmark mode works against mock server.
- [x] Speed, long-context, quality, and refusal mode labels produce parseable JSONL.
- [x] Benchmark output directories receive timestamped JSONL result files and same-basename Markdown reports.
- [ ] Benchmark mode works against LM Studio when running locally.
- [x] CSV summaries and JSONL result output are parseable.
- [x] Failed runs are counted and reported.
- [x] Ctrl+C/cancellation stops benchmark cleanly.
- [ ] Leak-check tooling reports no leaks after repeated benchmark runs where supported.

---

# v0.8 - AI-assisted editor

## Goal

Extend `--editor` into an AI-assisted writing and editing mode that uses the configured provider/model while keeping the editor usable as a local text editor. This is not agent mode: it must not gain filesystem, shell, or network powers beyond the configured model endpoint and ordinary file open/save behavior.

The feature should support spelling checks, grammar checks, rewrites, continuation, comments, proof checks, and user-provided prompts that insert or modify text.

## Command shape

Keep the existing editor mode as the base:

```sh
pkchat --editor draft.md
pkchat --editor draft.md --provider lmstudio -m MODEL
pkchat --editor draft.md --assist
pkchat --editor draft.md --assist --provider openai -m MODEL
pkchat --editor draft.md --assist-prompt "Make this more concise"
```

Possible later aliases or subcommands:

```sh
pkchat edit draft.md
pkchat edit draft.md --assist
```

`--editor` without assist options must continue to work offline and must not contact a model.

## Editor interaction model

Start with simple, explicit actions. Avoid hidden automatic rewrites.

Suggested commands inside the editor:

```text
/assist spelling
/assist grammar
/assist rewrite
/assist continue
/assist comment
/assist proof
/assist prompt TEXT
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

AI editing needs a clear text range contract:

- [ ] Add selection support to the editor core.
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
proof         whole file or selected range, no automatic mutation
prompt        selection/current paragraph for modify; cursor for insert
```

## AI actions

Spelling check:

- [ ] Ask model for spelling corrections only.
- [ ] Return a patch-like preview or replacement suggestions.
- [ ] Do not silently rewrite style or grammar.

Grammar check:

- [ ] Ask model for grammar corrections only.
- [ ] Preserve meaning and formatting as much as possible.
- [ ] Show preview before applying.

Rewrite:

- [ ] Rewrite selected/current text according to default or user-specified style.
- [ ] Support concise, clear, formal, informal, and custom instructions later.
- [ ] Show before/after preview.

Continue text:

- [ ] Send nearby context around the cursor.
- [ ] Insert generated continuation at the cursor only after user approval.
- [ ] Let user retry/regenerate before applying.

Comment text:

- [ ] Generate feedback comments without modifying the source text by default.
- [ ] Support inserting comments as plain text notes where appropriate for the file type later.

Proof check:

- [ ] Produce a review report for the selected range or file.
- [ ] Separate issues by severity/type: spelling, grammar, clarity, consistency, factual-risk notes.
- [ ] Do not mutate text unless the user chooses a proposed edit.

Custom prompt:

- [ ] Let the user provide a prompt to insert text at cursor or modify selected/current text.
- [ ] Make the prompt and target range visible before sending.
- [ ] Show preview before applying any replacement.

## Preview and apply workflow

AI changes should be reviewable before mutation:

- [ ] Stream model output into a preview panel or temporary assistant buffer.
- [ ] Support accept, reject, regenerate, and edit-before-apply.
- [ ] Applying a replacement should be one undoable editor operation once undo exists.
- [ ] Store enough local state to reject or revert a pending suggestion without reloading the file.
- [ ] Mark the buffer dirty only after a suggestion is applied.
- [ ] Save remains explicit through the editor save command.

Minimal first implementation can use a split-screen preview with these commands:

```text
/apply
/reject
/regenerate
```

## Provider/runtime integration

Use the existing provider, runtime, cancellation, and TUI/editor infrastructure:

- [ ] AI assist requests run as cancellable runtime jobs.
- [ ] Editor input and navigation remain responsive while the model is thinking or streaming.
- [ ] `Esc` or another documented key cancels an active assist request.
- [ ] Reuse provider model discovery/default model selection.
- [ ] Reuse Chat Completions and Responses API adapters through the provider layer.
- [ ] Do not let worker threads mutate editor state directly; send events to the editor loop.
- [ ] Shutdown cancels/joins assist jobs cleanly.

## Prompt construction

Prompts must be deterministic and scoped:

- [ ] Include action type, target text, optional surrounding context, and user instructions.
- [ ] Keep system prompts short and action-specific.
- [ ] Clearly ask for either replacement text, comments, or a structured report.
- [ ] Avoid sending the whole file unless the user requested whole-file proofing or the file is under a documented size limit.
- [ ] Redact or warn about potential secrets before sending selected text when feasible.
- [ ] Respect provider context limits and return clear errors when input is too large.

Possible structured response shape for edit suggestions:

```json
{
  "kind": "replacement",
  "replacement": "...",
  "notes": "..."
}
```

If the JSON facade is not strong enough for robust structured output at this stage, use plain replacement text with a conservative preview first.

## UI layout

The editor core already renders into rectangles. Use that for assist panels:

- [ ] Main editor panel for the file.
- [ ] Preview/result panel for suggestions or comments.
- [ ] Status line for provider/model/job state.
- [ ] Optional command prompt line for `/assist prompt ...`.
- [ ] Support resize without corrupting the editor or preview.

Do not make the AI panel modal-only if it blocks basic editing for long requests. The editor should remain cancellable and responsive.

## Persistence and privacy

- [ ] Do not save API keys in editor files or assist metadata.
- [ ] Do not persist AI suggestions unless the user applies them or explicitly saves a sidecar/report.
- [ ] Do not silently send unsaved file contents beyond the selected/target range and required context.
- [ ] Show which provider/model is used for assist actions when a request starts.
- [ ] Keep stdout/stderr behavior sane for `--editor`; status belongs in the terminal UI.

## Tests

- [ ] Unit test selection/range calculations.
- [ ] Unit test prompt construction for each action.
- [ ] Unit test applying replacement text to the piece table.
- [ ] Unit test reject/regenerate state transitions.
- [ ] Unit test cancellation events do not mutate editor text.
- [ ] Integration test spelling/grammar/rewrite against mock provider.
- [ ] Integration test streaming preview output.
- [ ] Integration test cancel during assist request.
- [ ] Resize test with active preview panel.
- [ ] UTF-8 tests for selected text and replacement text.
- [ ] Leak-check successful assist, rejected assist, applied assist, failed provider call, and cancelled assist where supported.

## Acceptance criteria

- [ ] `pkchat --editor FILE --assist` opens the editor and can call the configured model explicitly.
- [ ] Existing `pkchat --editor FILE` remains usable without any model/network requirement.
- [ ] At least spelling, grammar, rewrite, continue, comment, proof, and custom prompt actions have command paths planned or implemented.
- [ ] The editor remains responsive during an assist request.
- [ ] Assist requests are cancellable.
- [ ] Suggestions are previewed before modifying the buffer.
- [ ] Applying a suggestion updates only the intended range.
- [ ] Rejected suggestions leave the buffer unchanged.
- [ ] Secrets/API keys are not saved or displayed.
- [ ] Leak-check tooling reports no leaks for representative assist paths where supported.

---

# v0.9 - Local web server mode

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

# v1.0 - Local agent mode with sandbox/approval design

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
Ctrl+C remains reserved for copy in editor-backed terminal modes.
```

## Acceptance criteria

- [ ] Agent mode is separate from normal chat.
- [ ] No write/command/network action happens without approval under default settings.
- [ ] Actions are logged.
- [ ] Pause/cancel works.
- [ ] Sandbox behavior is documented.
- [ ] Leak-check tooling reports no leaks for representative approved, denied, failed, and cancelled tool actions where supported.

---

# v1.1 - Image generation from CLI, REPL, TUI, and web chat

## Goal

Add first-class image generation that works from non-interactive command-line usage, REPL, full-screen TUI, and local web chat. The feature must use the same provider/profile, runtime/job, cancellation, error-handling, persistence, and credential-redaction layers as text chat where practical.

At minimum, the user must be able to select:

- image generation model
- prompt
- image dimensions
- image output format
- output file name/path

If the user does not provide a file name, `pkchat` should automatically create a non-existing file name in the current directory, such as `image1.png`, `image2.png`, and so on.

## Command shape

Suggested CLI shape:

```sh
pkchat image -p "A quiet terminal workspace at night" --image-model MODEL
pkchat image --prompt-file prompt.txt --image-model MODEL --width 1024 --height 1024
pkchat image -p "diagram of provider adapters" --format png --output diagram.png
pkchat image -p "small icon" --size 512x512 --format webp
pkchat image --provider openai --image-model MODEL -p "..." --output image.png
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

- [ ] `pkchat image ...` is the explicit image-generation command.
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

## Web chat behavior

The local web UI should support image generation once web mode exists.

Minimum controls:

- [ ] Prompt textarea or prompt field.
- [ ] Image model selector/input.
- [ ] Width and height controls, or a size selector.
- [ ] Output format selector.
- [ ] File name/path field where local saving is supported.
- [ ] Generate button.
- [ ] Stop/Cancel button.

Requirements:

- [ ] Generation runs through the runtime/job layer and does not block the web server event loop.
- [ ] Browser UI shows status and errors.
- [ ] Browser UI shows the saved file path and, where safe, a preview served from a controlled generated-assets route.
- [ ] Do not expose arbitrary local file paths or directories through the web server.
- [ ] Do not expose API keys or provider headers to the browser.
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

- [ ] Unsupported provider/model/format/dimensions return `PKCHAT_ERR_UNSUPPORTED_FEATURE` or a more specific error where available.
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
- [ ] Web test for image generation request and cancel once web mode exists.
- [ ] Leak-check success, provider error, file write error, and cancellation paths where supported.

## Acceptance criteria

- [ ] CLI can generate an image with selected model, prompt, dimensions, format, and output file name.
- [ ] CLI can auto-generate a non-existing output file name in the current directory.
- [ ] REPL can generate an image through `/image` commands.
- [ ] TUI can start and cancel image generation without blocking the UI.
- [ ] Web chat can request image generation and show status/output path after web mode exists.
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
