# CLI and scripting

Ainiux reserves `stdout` for intentional model or conversion output. Status, warnings, progress, and errors use `stderr`. This makes the default one-shot mode suitable for pipes and command substitution.

## One-shot chat and REPL

```sh
ainiux lmstudio -p "What is RAII?"
ainiux openai -m MODEL --prompt-file prompt.txt
ainiux openrouter -m MODEL -i
```

Use `-s` for a system prompt, `-t` for temperature, `--reasoning` for provider-mapped reasoning selection, and `--no-stream` for a complete response. `--purpose general|coding|instruct|creative` applies catalog presets where available. Run `ainiux --help` for all generation controls.

`--save-chat PATH` writes a JSON transcript after a successful response. `--load-chat PATH` loads prior messages before the next prompt. These explicit files are separate from the SQLite thread library used by `-c`.

## Output formats

- `--format text` emits response text.
- `--format json` emits one response object.
- `--format ndjson` or `jsonl` emits event records.
- `--output-format md|html|plaintext` renders assistant Markdown for text output.
- `--output-format json|jsond|ndjson` selects machine-readable response output.
- `--output PATH` writes intentional output to a file; `stdout` is accepted explicitly.

HTML written to a file is a complete document. HTML on `stdout` is a fragment. Model-generated HTML is not sanitized for hostile browser contexts.

## Local conversion

Without a chat prompt, `--input`, `--fetch-url`, and `--search` perform extraction or conversion:

```sh
ainiux --input article.html --output-format md
ainiux --input notes.md --output-format plaintext
ainiux --fetch-url https://example.com --output-format md
ainiux --search "portable C++ terminal UI" --output-format json
printf 'plain text' | ainiux --input stdin --output stdout
```

Text, Markdown, and HTML are supported. HTML conversion is intentionally lightweight: it does not execute JavaScript, implement a browser DOM, or transcode legacy encodings. PDF and DOCX are rejected rather than inserted as binary prompt text.

## Prompt context and attachments

Combine a prompt with converted input, repeatable attachments, fetches, or search:

```sh
ainiux lmstudio -p "Summarize" --attach notes.md
ainiux openai -m MODEL -p "Describe this" --input photo.png
ainiux lmstudio -p "Compare the sources" --fetch-url https://example.com --search "related topic"
```

Text inputs are bounded and validated. PNG, JPEG, and GIF are supported only when the selected Chat Completions model accepts image content. Raw base64 is not persisted in chat JSON. Use `--image-capability allow` only after verifying an unknown custom model.

Fetching and searching are explicit. Prompt URLs are never fetched automatically. URL fetch applies byte and timeout limits and blocks private, loopback, link-local, multicast, and metadata addresses unless `--allow-private-url-fetch` is set. Search provider details are in [Configuration](configuration.md#web-search).

## Context management

`--context TOKENS` supplies a model context-window size. `--max-context-bytes` sets a request text budget, and `--context-policy` chooses `error`, `truncate-oldest`, `truncate-middle`, `summarize-oldest`, `summarize-middle`, or `provider-auto`.

Compaction changes only the request sent to the model; saved transcripts remain complete. The CLI reports compaction rather than silently rewriting history. Agent compaction is a separate workflow described in [Agent workflows](agent.md#compaction).

## Providers and endpoints

A provider shortcut or raw URL may be positional:

```sh
ainiux deepseek -m MODEL -p "Hello"
ainiux http://localhost:8000/v1 -m MODEL -p "Hello"
```

Override individual paths with `--base-url`, `--chat-url`, `--models-url`, or `--responses-url`. `--api responses` and `--responses` select the text-only Responses adapter. Endpoint normalization is deterministic and reports surprising rewrites unless `--quiet` is set.

## Script reliability

Use `--quiet` to suppress ordinary status and `-v` for timing. `--debug` prints configuration diagnostics with credentials redacted. Network, API, configuration, cancellation, and argument failures map to distinct exit codes. Errors include a safe provider response and next step where available.

Avoid `-k`: keys in process arguments may be visible locally. Prefer environment variables, `--key-env NAME`, `--key-file PATH`, or `--key-stdin`.

Related documentation: [documentation index](README.md), [getting started](getting-started.md), [API compatibility](api-compatibility.md), [security](security.md).
