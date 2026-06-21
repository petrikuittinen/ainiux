# pkchat

`pkchat` is a fast, script-friendly command-line chat client for OpenAI and OpenAI-compatible APIs.

Current status: v0.54 CLI with libcurl transport, cancellable runtime jobs, provider registry/profile aliases, `/v1/models`, `/v1/chat/completions`, text-only OpenAI Responses API support, local JPEG/PNG/GIF image input, interactive text/image attachments, request-only context policies, safe URL insertion, a simple REPL, a standalone `--editor` mode, a full-screen non-blocking TUI foundation, JSON chat save/load, HTML-to-text/Markdown extraction, and Markdown assistant-output rendering to HTML or plaintext.

## Build

```sh
make
```

For a smaller release-style executable compiled with `-O3`, `-DNDEBUG`, and stripped symbols:

```sh
make optimized
```

Useful targets:

```sh
make test
make optimized
make sanitize
make test-sanitize
make leak-check
make clean
```

The HTTP transport uses libcurl through RAII wrappers in `src/http/`. Build flags are discovered with `pkg-config libcurl`, falling back to `curl-config` when needed.

## Examples

Local OpenAI-compatible server:

```sh
./pkchat http://localhost:30000 -m "unsloth/Qwen3.6-35B-A3B-MTP-GGUF:UD-Q4_K_XL" -p "Hello"
./pkchat http://localhost:30000 -p "Hello"
./pkchat --list-models http://localhost:30000
```

LM Studio profile:

```sh
./pkchat lmstudio -i
./pkchat --chat lmstudio
./pkchat --provider lm_studio -m MODEL -p "Hello from LM Studio"
./pkchat --provider lmstudio --list-models
```

When no model is provided, `pkchat` calls `/v1/models` and uses the first returned model id. If the models endpoint returns no ids, the request omits the model field and startup status shows `Model: unknown`.

`lmstudio -i` uses `http://localhost:1234/v1` and does not require an API key.

OpenAI:

```sh
OPENAI_API_KEY=... ./pkchat --provider openai -m MODEL -p "Hello"
OPENAI_API_KEY=... ./pkchat --provider openai --api responses -m MODEL -p "Hello through Responses"
OPENAI_API_KEY=... ./pkchat --provider openai_responses -m MODEL -p "Hello through Responses"
```

`--api responses` and `--responses` use `/v1/responses` and currently support text chat only. Providers without a built-in Responses endpoint return an unsupported-feature error unless `--responses-url URL` is supplied explicitly.

OpenRouter:

```sh
OPENROUTER_API_KEY=... ./pkchat openrouter -model "nvidia/nemotron-3-ultra-550b-a55b:free" -i
OPENROUTER_API_KEY=... ./pkchat --provider openrouter -m "nvidia/nemotron-3-ultra-550b-a55b:free" -p "Hello"
```

Z.AI and Qwen:

```sh
ZAI_API_KEY=... ./pkchat --provider zai -m glm-5 -p "Hello"
DASHSCOPE_API_KEY=... ./pkchat --provider qwen -m qwen-plus -p "Hello"
DASHSCOPE_API_KEY=... ./pkchat --provider qwen --list-models
```

Z.AI does not publish an OpenAI-compatible model-list endpoint, so its profile requires `--model`. The Qwen profile uses Alibaba Cloud Model Studio's global Singapore endpoint; use `dashscope` for the China (Beijing) endpoint or override `--base-url` for another region.

Endpoint references: [Z.AI HTTP API](https://docs.z.ai/guides/develop/http/introduction), [Qwen API Platform](https://qwen.ai/apiplatform), and [Model Studio OpenAI compatibility](https://www.alibabacloud.com/help/en/model-studio/compatibility-of-openai-with-dashscope).

Complete built-in provider list:

| Provider | Aliases | Default base URL | Default key environment |
| --- | --- | --- | --- |
| `none` | `offline` | none | none |
| `openai` | `openai_chat`, `openai_responses` | `https://api.openai.com/v1` | `OPENAI_API_KEY` |
| `openrouter` | | `https://openrouter.ai/api/v1` | `OPENROUTER_API_KEY` |
| `deepseek` | | `https://api.deepseek.com` | `DEEPSEEK_API_KEY` |
| `gemini` | | `https://generativelanguage.googleapis.com/v1beta/openai` | `GEMINI_API_KEY` |
| `anthropic` | | `https://api.anthropic.com/v1` | `ANTHROPIC_API_KEY` |
| `xai` | `grok` | `https://api.x.ai/v1` | `XAI_API_KEY` |
| `moonshot` | `kimi` | `https://api.moonshot.ai/v1` | `MOONSHOT_API_KEY` |
| `groq` | | `https://api.groq.com/openai/v1` | `GROQ_API_KEY` |
| `mistral` | | `https://api.mistral.ai/v1` | `MISTRAL_API_KEY` |
| `together` | | `https://api.together.ai/v1` | `TOGETHER_API_KEY` |
| `perplexity` | | `https://api.perplexity.ai` | `PERPLEXITY_API_KEY` |
| `cerebras` | | `https://api.cerebras.ai/v1` | `CEREBRAS_API_KEY` |
| `fireworks` | | `https://api.fireworks.ai/inference/v1` | `FIREWORKS_API_KEY` |
| `deepinfra` | | `https://api.deepinfra.com/v1/openai` | `DEEPINFRA_API_KEY` |
| `nvidia_nim` | | `https://integrate.api.nvidia.com/v1` | `NVIDIA_NIM_API_KEY` |
| `zai` | `z.ai`, `z_ai` | `https://api.z.ai/api/paas/v4` | `ZAI_API_KEY` |
| `qwen` | `dashscope_intl` | `https://dashscope-intl.aliyuncs.com/compatible-mode/v1` | `DASHSCOPE_API_KEY` |
| `dashscope` | | `https://dashscope.aliyuncs.com/compatible-mode/v1` | `DASHSCOPE_API_KEY` |
| `lm_studio` | `lmstudio`, `lm-studio` | `http://localhost:1234/v1` | optional |
| `ollama` | | `http://localhost:11434/v1` | none |
| `vllm` | | `http://localhost:8000/v1` | `token-abc123` |
| `llamacpp` | `llama_cpp`, `llama.cpp` | `http://localhost:8080/v1` | none |
| `custom_openai_chat` | `custom` | user supplied | `PKCHAT_API_KEY` (optional) |

The model-backed profiles share the same OpenAI-compatible chat adapter where possible, with endpoint paths and key defaults coming from the registry. Provider names and aliases are case-insensitive; hyphens and underscores are interchangeable.

Offline mode uses the `none` provider (alias `offline`) and requires no model endpoint or API key:

```sh
./pkchat --provider none --editor notes.txt
./pkchat --provider none --input page.html --output-format md
./pkchat --provider none --input notes.md --output-format html --output notes.html
./pkchat --provider none --fetch-url https://example.com/article --output-format md
printf '/quit\n' | ./pkchat --provider none --repl --quiet
```

The `none` provider never sends model requests or lists models. REPL and TUI modes can still run local commands such as `/insert`, `/fetch`, `/save`, and `/load`, but entering a chat prompt returns an unsupported-feature error until an OpenAI-compatible provider is selected. Model endpoint overrides are rejected with `--provider none` so offline mode cannot accidentally contact one. Explicit `--fetch-url` and `/fetch` operations still access their requested URL and retain the normal URL-fetch safety checks.

Prompt and system files:

```sh
./pkchat --base-url http://localhost:30000/v1 \
  -m "unsloth/Qwen3.6-35B-A3B-MTP-GGUF:UD-Q4_K_XL" \
  --prompt-file prompt.txt --system-file system.txt --format json
```

Rendered assistant output:

```sh
./pkchat http://localhost:30000 -p "Write a short report" --output-format html
./pkchat http://localhost:30000 -p "Write a short report" --output-format html --output report.html
./pkchat http://localhost:30000 -p "Write a short report" --output-format plaintext
./pkchat http://localhost:30000 -p "Write a short report" --output-format jsond
```

`--output-format md|html|plaintext` controls how assistant Markdown is written in text mode. `md` is the default and preserves the existing streaming behavior. `html` and `plaintext` render after the full assistant reply is received; when `html` is combined with `--output PATH`, the file contains a complete HTML document with doctype, charset, viewport, head, and body. `--output-format json|jsond|ndjson` is accepted as an alias for machine-readable JSON or newline-delimited JSON output. HTML fragments and raw HTML blocks in model output are preserved; this is a renderer, not an HTML sanitizer.

Input extraction and URL context:

```sh
./pkchat --input page.html --output-format md
./pkchat --input page.html --output-format plaintext --output page.txt
./pkchat --input notes.md --output-format html --output notes.html
./pkchat --input notes.txt --output-format jsond
./pkchat --fetch-url https://example.com/article --output-format md
./pkchat http://localhost:30000 -p "Tee yhteenveto" --fetch-url https://yle.fi/uutiset/lyhyesti/74-20232138
./pkchat http://localhost:30000 -s "Vastaa suomeksi" -p "Tee yhteenveto" --input page.html
./pkchat http://localhost:30000 -p "Describe this image" --input photo.png
./pkchat http://localhost:30000 -p "Compare these notes" --attach one.md --attach two.txt
./pkchat http://localhost:30000 -p "Compare these images" --attach one.png --attach two.jpg
printf 'pipeline output\n' | ./pkchat http://localhost:30000 -p "Summarize this" --attach stdin
generate-report | ./pkchat --input stdin --output-format html --output stdout
./pkchat http://localhost:30000 -p "Continue" --load-chat chat.json --context-policy truncate-oldest --max-context-bytes 65536
```

`--input PATH` classifies extensions case-insensitively. It reads local `.txt`, `.md`/`.markdown`, and `.html`/`.htm` documents, or attaches `.png`, `.jpg`, `.jpeg`, and `.gif` images. Document inputs can be extracted without a model; image inputs require `-p`/`--prompt` and non-interactive Chat Completions mode. Images are signature-checked, capped at 20 MiB by default (`--max-image-bytes N`), base64-encoded into an OpenAI-compatible `image_url` data URL, and released after the request. Saved chat JSON keeps the prompt but does not embed image bytes. Provider profiles and recognized vision-model names are checked in the default `--image-capability auto` mode. Compatible unknown/custom models require `--image-capability allow`; `deny` disables image input. WebP input is disabled because common tested vision models do not decode it reliably, and `.webm` is a video container rather than an image.

`--input` and `--fetch-url` by themselves are explicit document extraction modes: they print converted content to `stdout` and do not contact a model. In standalone extraction, `--output-format md|html|plaintext|json|jsond|ndjson` controls the output; `html` writes a fragment to `stdout` or a complete HTML document with `--output PATH`. When a document input is combined with `-p`/`--prompt` or `--prompt-file` in non-interactive CLI mode, `pkchat` sends the extracted input as a separate user-context message before the final prompt, while any `-s`/`--system` or `--system-file` remains the system prompt. The older `--html-file` option remains accepted as a compatibility alias for local HTML input.

`--attach PATH` is repeatable and adds UTF-8 `.txt`, `.md`, or `.html` context files and PNG/JPEG/GIF images before the final non-interactive prompt. REPL and TUI `/insert PATH` and `/attach PATH` insert text context immediately or queue images for exactly the next prompt; TUI file work is cancellable. `/fetch URL` fetches, validates, converts, and inserts HTML through the same cancellable TUI job and URL safety policy as `--fetch-url`. Local document reads default to a 1 MiB per-file limit; change it with `--max-input-bytes N`. Oversized, unreadable, binary, invalid UTF-8, PDF, DOCX, and unsupported-extension inputs fail with specific errors. PDF and MS Word input/output conversion is deferred.

For pipelines, `--input stdin` and `--attach stdin` read bounded UTF-8 plaintext from standard input. A command may select stdin only once, so these cannot be combined with another stdin-consuming option such as `--prompt-file -` or `--key-stdin`. `--output stdout` writes to standard output and is equivalent to omitting `--output`; status and errors remain on standard error.

Context control is opt-in through `--max-context-bytes N`. `--context-policy error` is the default; `truncate-oldest`, `summarize-oldest`, and `summarize-middle` create a bounded request copy, while `provider-auto` sends the full transcript for provider-side handling. Local summaries are deterministic extracts, not extra model calls. Saved chat messages are never compacted: each compaction is recorded in `compaction_events`, and notices state that the full transcript remains on disk. The byte estimate is a transport-independent guard, not a provider token count.

`--context TOKENS` supplies the model's context-window size for the TUI usage estimate. A `k` suffix uses 1024 (`64k` is 65536), while `M` uses 1000000 (`1M` is one million); suffixes are case-insensitive. After a response, `Context used: ESTIMATED/TOTAL (PERCENT%)` shows estimated consumption and can exceed 100%. The estimate conservatively uses the higher of provider-reported total usage, when available, and a Unicode-aware transcript estimate. Retained `<think>...</think>` reasoning is included. This display setting is separate from the byte-based compaction limit.

The first HTML parser lives in `src/html/` and handles simple text extraction, including `h1`, `h2`, `strong`/`b`, `em`/`i`/`italic`, and `a href` links in Markdown output. Fetching uses libcurl, browser-style `User-Agent`/`Accept` headers, a default 1 MiB body limit, a default 30 second total timeout for this mode, HTML content-type checks, and no redirect following. Private, loopback, link-local, multicast, and common metadata-service literal hosts and resolved socket addresses are blocked by default; use `--allow-private-url-fetch` only for explicit local testing. A proxy also requires that override because its target DNS resolution cannot be verified by the client. Input must be UTF-8; legacy charsets such as Windows-1251 or GBK are rejected with a clear error until charset conversion is implemented. JavaScript-rendered pages are not supported.

Interactive REPL and chat files:

```sh
./pkchat --repl http://localhost:30000 -m MODEL --save-chat chat.json
./pkchat http://localhost:30000 -m MODEL -p "Hello" --save-chat chat.json
./pkchat http://localhost:30000 -p "Hello"
./pkchat --load-chat chat.json -p "Continue from the saved chat"
```

In REPL mode, commands include `/help`, `/quit`, `/save PATH`, `/load PATH`, `/insert PATH`, `/attach PATH`, `/fetch URL`, `/clear`, `/system TEXT`, and `/model MODEL`. Prompts and status are written to `stderr`; assistant replies remain on `stdout`.

Standalone multiline editor:

```sh
./pkchat --editor notes.txt
./pkchat --editor draft.txt --output saved-draft.txt
```

The editor is a permanent bonus mode and the same component now powers the TUI chat input panel. It uses a piece table buffer and a rectangular panel renderer, so the same core can support large files and multiple editor panels in one terminal window. Long lines soft-wrap inside the panel. The standalone file editor keeps logical-line up/down movement by default, while the TUI input uses visual-row movement across soft-wrapped overflow rows. `Tab` completes the whitespace-delimited file path at the cursor. A unique match is completed immediately; multiple matches first complete their common prefix, then repeated `Tab` presses cycle through the sorted choices. Controls: arrows move, Home/End jump within the line, `Ctrl+A`/`Ctrl+E` jump to the beginning/end of the current line, `Ctrl+K` kills from the cursor to the end of the line and removes the line when it is already empty, Backspace/Delete remove text, `Enter` inserts a newline, `Ctrl+S` saves, and `Ctrl+Q` or `Ctrl+C` exits.

Full-screen chat TUI foundation:

```sh
./pkchat --chat http://localhost:30000 -m MODEL
./pkchat --chat lmstudio
```

The TUI also runs `/insert`, `/attach`, and `/fetch` through cancellable runtime jobs. `/help` toggles a persistent, scrollable command panel that is not sent to the provider or saved.

In non-interactive `-p`/`--prompt` mode, model thinking traces are written only to standard error. Standard output contains only the visible answer, including with streaming, JSON, NDJSON, rendered output, and `--output stdout`, so it is safe to pipe into another command. Saved chat files retain the full assistant response, including thinking traces.

The TUI keeps model requests, `/models`, `/save`, and `/load` behind runtime jobs so the terminal loop stays responsive. Its initial status is `Pkchat vVERSION ready`; after a completed streaming response, the status shows time to first token and token/s, marked as estimated when provider token usage is unavailable. With `--context`, that same line also shows estimated context usage. Non-streaming responses show total response latency because true first-token timing is not observable. The bottom input area embeds the editor component in a fixed-height panel with soft wrap and visual-row cursor movement. `Tab` uses the same file-path completion and repeated-choice cycling as standalone editor mode, including paths supplied to `/insert`, `/attach`, `/save`, and `/load`. Colors are enabled by default with the `dark` theme; use `--nocolors` to disable color styling, and `/theme`, `/theme dark`, or `/theme light` inside the TUI to inspect or switch themes. Thinking traces are hidden by default; use `/thinking trace`, `/thinking notrace`, or `Ctrl+T` to toggle display of `<think>...</think>` blocks; visible thinking traces use a subdued tinted color that is kept WCAG 2.1 AA compliant. Provider reasoning fields such as `reasoning_content`, `reasoning`, and text `reasoning_details` are displayed as `<think>` blocks. `Enter` sends, `Alt+Enter` or `Esc` then `Enter` inserts a newline, `Ctrl+A`/`Ctrl+E` jump to the beginning/end of the current input line, `Ctrl+K` kills from the cursor to the end of the input line and removes the line when it is already empty, and `Ctrl+S` sends the current multiline draft. A bare `Esc` cancels the active model request while keeping the current turn visible. `Ctrl+R` regenerates the last answer by resending the last user prompt. `PageUp` and `PageDown` scroll chat history, `Home` jumps to the beginning of the chat thread, and `End` returns to the live bottom. `Ctrl+C` cancels the active job, or exits when no job is active.

Verbose timing:

```sh
./pkchat -v http://localhost:30000 -m MODEL -p "Hello"
```

`-v`/`--verbose` prints time to first token in milliseconds and token/s to `stderr`. When provider usage is unavailable, token/s uses a lightweight local estimate.

## Output Behavior

- `stdout` is model output in text mode.
- `--output-format md|html|plaintext` renders assistant Markdown in text mode; `html` writes a fragment to `stdout` or a complete page with `--output PATH`. `--output-format json|jsond|ndjson` selects machine-readable response output.
- `stderr` is used for warnings, status, and errors.
- Chat startup status prints the chat endpoint and selected model to `stderr` unless `--quiet` is set. `--chat` does not reserve persistent screen rows for endpoint/model details.
- `--input` and `--fetch-url` reserve `stdout` for converted text/Markdown/HTML/JSON in extraction mode; fetch status is written to `stderr` unless `--quiet` is set.
- `--format json` returns one JSON object.
- `--format ndjson` and `--output-format jsond` return streaming-style events.
- `--save-chat PATH` writes a JSON chat file atomically with restrictive permissions.
- `--load-chat PATH` loads prior messages before sending the next prompt.

## Credentials

Supported key sources:

- provider environment variables such as `OPENAI_API_KEY`, `OPENROUTER_API_KEY`, `DEEPSEEK_API_KEY`, `GEMINI_API_KEY`, `ANTHROPIC_API_KEY`, `XAI_API_KEY`, `MOONSHOT_API_KEY`, `GROQ_API_KEY`, `MISTRAL_API_KEY`, `TOGETHER_API_KEY`, `PERPLEXITY_API_KEY`, `CEREBRAS_API_KEY`, `FIREWORKS_API_KEY`, `DEEPINFRA_API_KEY`, `DEEPINFRA_TOKEN`, `NVIDIA_NIM_API_KEY`, `ZAI_API_KEY`, `DASHSCOPE_API_KEY`, `QWEN_API_KEY`, `LMSTUDIO_API_KEY`, `LM_STUDIO_API_KEY`
- `PKCHAT_API_KEY`
- `--key-env NAME`
- `--key-file PATH`
- `--key-stdin`
- `--header "Authorization: Bearer ..."`

`-k`/`--key` exists for testing but warns because command-line arguments can be visible to other local users.

## Testing

Run the full local suite:

```sh
make test
```

The integration test starts a local mock OpenAI-compatible server and verifies model listing, non-streaming chat, streaming chat, text-only Responses API calls, provider reasoning fields, JSON output, NDJSON output, Markdown-to-HTML/plaintext assistant rendering, complete HTML file output, chat save/load, REPL mode, explicit local input and HTML URL extraction with private-address blocking, input/fetched URL prompt context with system prompts, complete HTML file output from input Markdown, JSOND output aliases, and non-UTF-8 HTML rejection. Unit tests cover CLI parsing, provider registry aliases, capability reporting, Responses API endpoint selection and unsupported-feature errors, HTML conversion including malformed documents and UTF-8 validation, Markdown output rendering, the runtime event queue/job cancellation, `--chat`, legacy `--tui`, `--nocolors`, and `--editor` parsing, editor piece-table edits, rectangular panel rendering, editor word wrapping, editor vertical navigation modes, editor file round-trips, TUI layout sizing, TUI regeneration planning, thinking-trace display filtering, theme parsing, and WCAG contrast checks for TUI themes.

For leak and sanitizer checks:

```sh
make test-sanitize
make leak-check
```

If Valgrind is not installed, `make leak-check` falls back to the sanitizer test path.

## Current Limitations

- Streaming chat and Responses API events are parsed incrementally as SSE through libcurl write callbacks.
- Responses API support is currently text-only. Local image input currently uses the Chat Completions `image_url` content-part schema only.
- Capabilities start from the provider registry. Chat Completions image input additionally recognizes common vision model names; use `--image-capability allow` only after verifying an unknown/custom model.
- The JSON facade is intentionally small and scoped to the current CLI/provider needs.
- HTML extraction is intentionally simple: no JavaScript execution, no full DOM implementation, and no charset conversion yet. Non-UTF-8 input is rejected instead of transcoded.
- The editor preserves UTF-8 bytes and moves across UTF-8 code units safely, but full grapheme cluster and East Asian cell-width handling still belongs in the planned Unicode module.
- The chat TUI is still a foundation; it now uses the editor component for multiline input, but still needs broader interactive resize, scrollback, and terminal-key coverage.
