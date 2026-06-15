# pkchat

`pkchat` is a fast, script-friendly command-line chat client for OpenAI and OpenAI-compatible APIs.

Current status: v0.3 CLI with libcurl transport, cancellable runtime jobs, `/v1/models`, `/v1/chat/completions`, a simple REPL, a full-screen non-blocking TUI foundation, and JSON chat save/load.

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
```

OpenRouter:

```sh
OPENROUTER_API_KEY=... ./pkchat openrouter -model "nvidia/nemotron-3-ultra-550b-a55b:free" -i
OPENROUTER_API_KEY=... ./pkchat --provider openrouter -m "nvidia/nemotron-3-ultra-550b-a55b:free" -p "Hello"
```

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

Full-screen TUI foundation:

```sh
./pkchat --tui http://localhost:30000 -m MODEL
./pkchat --tui lmstudio
```

The TUI keeps model requests, `/models`, `/save`, and `/load` behind runtime jobs so the terminal loop stays responsive. The input area is multi-line: `Enter` sends, `Alt+Enter` or `Esc` then `Enter` inserts a newline, and `Ctrl+S` sends the current multiline draft. `Ctrl+C` cancels the active job, or exits when no job is active. The first screen shows the chat endpoint and selected model.

Verbose timing:

```sh
./pkchat -v http://localhost:30000 -m MODEL -p "Hello"
```

`-v`/`--verbose` prints time to first token in milliseconds and token/s to `stderr`. When provider usage is unavailable, token/s uses a lightweight local estimate.

## Output Behavior

- `stdout` is model output in text mode.
- `stderr` is used for warnings, status, and errors.
- Chat startup status prints the chat endpoint and selected model to `stderr` unless `--quiet` is set. In `--tui`, the same information is shown in the full-screen header.
- `--format json` returns one JSON object.
- `--format ndjson` returns streaming-style events.
- `--save-chat PATH` writes a JSON chat file atomically with restrictive permissions.
- `--load-chat PATH` loads prior messages before sending the next prompt.

## Credentials

Supported key sources:

- provider environment variables such as `OPENAI_API_KEY`, `OPENROUTER_API_KEY`, `LMSTUDIO_API_KEY`, `LM_STUDIO_API_KEY`
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

The integration test starts a local mock OpenAI-compatible server and verifies model listing, non-streaming chat, streaming chat, JSON output, NDJSON output, chat save/load, and REPL mode. Unit tests cover the runtime event queue/job cancellation and `--tui` parsing.

For leak and sanitizer checks:

```sh
make test-sanitize
make leak-check
```

If Valgrind is not installed, `make leak-check` falls back to the sanitizer test path.

## Current Limitations

- Streaming responses are parsed incrementally as SSE through libcurl write callbacks.
- The JSON facade is intentionally small and scoped to the current CLI/provider needs.
