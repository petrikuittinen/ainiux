# pkchat

`pkchat` is a fast, script-friendly command-line chat client for OpenAI and OpenAI-compatible APIs.

Current status: v0.40 CLI with libcurl transport, cancellable runtime jobs, provider registry/profile aliases, `/v1/models`, `/v1/chat/completions`, text-only OpenAI Responses API support, a simple REPL, a standalone `--editor` mode, a full-screen non-blocking TUI foundation, and JSON chat save/load.

## Build

```sh
make
```

Useful targets:

```sh
make test
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
./pkchat --tui lmstudio
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

Built-in provider profiles include `openai`, `openrouter`, `deepseek`, `gemini`, `anthropic`, `xai`/`grok`, `moonshot`/`kimi`, `groq`, `mistral`, `together`, `perplexity`, `cerebras`, `fireworks`, `deepinfra`, `nvidia_nim`, `dashscope`, `lm_studio`/`lmstudio`, `ollama`, `vllm`, `llamacpp`/`llama.cpp`, and `custom_openai_chat`. These profiles share the same OpenAI-compatible chat adapter where possible, with endpoint paths and key defaults coming from the registry.

Prompt and system files:

```sh
./pkchat --base-url http://localhost:30000/v1 \
  -m "unsloth/Qwen3.6-35B-A3B-MTP-GGUF:UD-Q4_K_XL" \
  --prompt-file prompt.txt --system-file system.txt --format json
```


Interactive REPL and chat files:

```sh
./pkchat --repl http://localhost:30000 -m MODEL --save-chat chat.json
./pkchat http://localhost:30000 -m MODEL -p "Hello" --save-chat chat.json
./pkchat http://localhost:30000 -p "Hello"
./pkchat --load-chat chat.json -p "Continue from the saved chat"
```

In REPL mode, commands include `/help`, `/quit`, `/save PATH`, `/load PATH`, `/clear`, `/system TEXT`, and `/model MODEL`. Prompts and status are written to `stderr`; assistant replies remain on `stdout`.

Standalone multiline editor:

```sh
./pkchat --editor notes.txt
./pkchat --editor draft.txt --output saved-draft.txt
```

The editor is a permanent bonus mode and the same component now powers the TUI chat input panel. It uses a piece table buffer and a rectangular panel renderer, so the same core can support large files and multiple editor panels in one terminal window. Long lines soft-wrap inside the panel. The standalone file editor keeps logical-line up/down movement by default, while the TUI input uses visual-row movement across soft-wrapped overflow rows. Controls: arrows move, Home/End jump within the line, `Ctrl+A`/`Ctrl+E` jump to the beginning/end of the current line, `Ctrl+K` kills from the cursor to the end of the line and removes the line when it is already empty, Backspace/Delete remove text, `Enter` inserts a newline, `Ctrl+S` saves, and `Ctrl+Q` or `Ctrl+C` exits.

Full-screen TUI foundation:

```sh
./pkchat --tui http://localhost:30000 -m MODEL
./pkchat --tui lmstudio
```

The TUI keeps model requests, `/models`, `/save`, and `/load` behind runtime jobs so the terminal loop stays responsive. The bottom input area embeds the editor component in a fixed-height panel with soft wrap and visual-row cursor movement. Colors are enabled by default with the `dark` theme; use `--nocolors` to disable color styling, and `/theme`, `/theme dark`, or `/theme light` inside the TUI to inspect or switch themes. Thinking traces are hidden by default; use `/thinking trace`, `/thinking notrace`, or `Ctrl+T` to toggle display of `<think>...</think>` blocks; visible thinking traces use a subdued tinted color that is kept WCAG 2.1 AA compliant. Provider reasoning fields such as `reasoning_content`, `reasoning`, and text `reasoning_details` are displayed as `<think>` blocks. `Enter` sends, `Alt+Enter` or `Esc` then `Enter` inserts a newline, `Ctrl+A`/`Ctrl+E` jump to the beginning/end of the current input line, `Ctrl+K` kills from the cursor to the end of the input line and removes the line when it is already empty, and `Ctrl+S` sends the current multiline draft. A bare `Esc` cancels the active model request while keeping the current turn visible. `Ctrl+R` regenerates the last answer by resending the last user prompt. `PageUp` and `PageDown` scroll chat history, `Home` jumps to the beginning of the chat thread, and `End` returns to the live bottom. `Ctrl+C` cancels the active job, or exits when no job is active.

Verbose timing:

```sh
./pkchat -v http://localhost:30000 -m MODEL -p "Hello"
```

`-v`/`--verbose` prints time to first token in milliseconds and token/s to `stderr`. When provider usage is unavailable, token/s uses a lightweight local estimate.

## Output Behavior

- `stdout` is model output in text mode.
- `stderr` is used for warnings, status, and errors.
- Chat startup status prints the chat endpoint and selected model to `stderr` unless `--quiet` is set. `--tui` does not reserve persistent screen rows for endpoint/model details.
- `--format json` returns one JSON object.
- `--format ndjson` returns streaming-style events.
- `--save-chat PATH` writes a JSON chat file atomically with restrictive permissions.
- `--load-chat PATH` loads prior messages before sending the next prompt.

## Credentials

Supported key sources:

- provider environment variables such as `OPENAI_API_KEY`, `OPENROUTER_API_KEY`, `DEEPSEEK_API_KEY`, `GEMINI_API_KEY`, `ANTHROPIC_API_KEY`, `XAI_API_KEY`, `MOONSHOT_API_KEY`, `GROQ_API_KEY`, `MISTRAL_API_KEY`, `TOGETHER_API_KEY`, `PERPLEXITY_API_KEY`, `CEREBRAS_API_KEY`, `FIREWORKS_API_KEY`, `DEEPINFRA_API_KEY`, `DEEPINFRA_TOKEN`, `NVIDIA_NIM_API_KEY`, `DASHSCOPE_API_KEY`, `LMSTUDIO_API_KEY`, `LM_STUDIO_API_KEY`
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

The integration test starts a local mock OpenAI-compatible server and verifies model listing, non-streaming chat, streaming chat, text-only Responses API calls, provider reasoning fields, JSON output, NDJSON output, chat save/load, and REPL mode. Unit tests cover CLI parsing, provider registry aliases, capability reporting, Responses API endpoint selection and unsupported-feature errors, the runtime event queue/job cancellation, `--tui`, `--nocolors`, and `--editor` parsing, editor piece-table edits, rectangular panel rendering, editor word wrapping, editor vertical navigation modes, editor file round-trips, TUI layout sizing, TUI regeneration planning, thinking-trace display filtering, theme parsing, and WCAG contrast checks for TUI themes.

For leak and sanitizer checks:

```sh
make test-sanitize
make leak-check
```

If Valgrind is not installed, `make leak-check` falls back to the sanitizer test path.

## Current Limitations

- Streaming chat and Responses API events are parsed incrementally as SSE through libcurl write callbacks.
- Responses API support is currently text-only; images, files, tools, and provider-side context management remain disabled in client capabilities until implemented.
- Capability probing is not yet implemented; built-in profile capabilities are registry-defined, and `--responses-url` is the explicit override for non-OpenAI Responses endpoints.
- The JSON facade is intentionally small and scoped to the current CLI/provider needs.
- The editor preserves UTF-8 bytes and moves across UTF-8 code units safely, but full grapheme cluster and East Asian cell-width handling still belongs in the planned Unicode module.
- The chat TUI is still a foundation; it now uses the editor component for multiline input, but still needs broader interactive resize, scrollback, and terminal-key coverage.
