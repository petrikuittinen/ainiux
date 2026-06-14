# pkchat

`pkchat` is a fast, script-friendly command-line chat client for OpenAI and OpenAI-compatible APIs.

Current status: v0.1 CLI for `/v1/models` and `/v1/chat/completions`.

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

The current transport uses the installed `curl` executable behind `src/http/` because this environment does not provide libcurl development headers. The transport boundary is isolated for a later libcurl RAII implementation.

## Examples

Local OpenAI-compatible server:

```sh
./pkchat http://localhost:30000 -m "unsloth/Qwen3.6-35B-A3B-MTP-GGUF:UD-Q4_K_XL" -p "Hello"
./pkchat --list-models http://localhost:30000
```

LM Studio profile:

```sh
./pkchat --provider lm_studio -m MODEL -p "Hello from LM Studio"
./pkchat --provider lmstudio --list-models
```

OpenAI:

```sh
OPENAI_API_KEY=... ./pkchat --provider openai -m MODEL -p "Hello"
```

OpenRouter:

```sh
OPENROUTER_API_KEY=... ./pkchat --provider openrouter -m "nvidia/nemotron-3-ultra-550b-a55b:free" -p "Hello"
```

Prompt and system files:

```sh
./pkchat --base-url http://localhost:30000/v1 \
  -m "unsloth/Qwen3.6-35B-A3B-MTP-GGUF:UD-Q4_K_XL" \
  --prompt-file prompt.txt --system-file system.txt --format json
```

## Output Behavior

- `stdout` is model output in text mode.
- `stderr` is used for warnings, status, and errors.
- `--format json` returns one JSON object.
- `--format ndjson` returns streaming-style events.

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

The integration test starts a local mock OpenAI-compatible server and verifies model listing, non-streaming chat, streaming chat, JSON output, and NDJSON output.

For leak and sanitizer checks:

```sh
make test-sanitize
make leak-check
```

If Valgrind is not installed, `make leak-check` falls back to the sanitizer test path.

## Current Limitations

- HTTP is currently isolated behind `src/http/` but implemented through the installed `curl` executable because libcurl development headers were not available in the initial environment.
- Streaming responses are parsed as SSE, but the current transport buffers the HTTP response before provider parsing. True incremental transport delivery is planned next.
- The JSON facade is intentionally small and scoped to the current CLI/provider needs.
