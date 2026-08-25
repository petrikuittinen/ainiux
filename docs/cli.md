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

Text, Markdown, and HTML are supported. HTML conversion is intentionally lightweight: it does not execute JavaScript or implement a browser DOM. UTF-8 is accepted as-is. UTF-16 (BOM or a strong no-BOM heuristic) is converted automatically. Declared HTML/HTTP charsets and `--encoding NAME` convert Windows-1250/1251/1252, ISO-8859-1/2, KOI8-R/U, and (via `iconv` when installed) CJK names such as `gbk` or `big5`. Unlabeled 8-bit files fail with a hint to pass `--encoding`. PDF and DOCX are rejected rather than inserted as binary prompt text.

```sh
ainiux --input letter.txt --encoding cp1251 --output-format plaintext
ainiux --input export.txt --encoding utf-16 --output-format md
```

## Prompt context and attachments

Combine a prompt with converted input, repeatable attachments, fetches, or search:

```sh
ainiux lmstudio -p "Summarize" --attach notes.md
ainiux openai -m MODEL -p "Describe this" --input photo.png
ainiux deepseek -m deepseek-v4-flash-vision-exp -p "Describe this" \
  --input tests/image_files/China_EV_sales_March_2024.png
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

Override individual paths with `--base-url`, `--chat-url`, `--models-url`, or `--responses-url`. Official `--provider openai` defaults to Responses. `--api chat`, `openai_chat`, and a user `api = chat` setting stay on Chat Completions. `--api responses` and `--responses` still select Responses explicitly. Custom URLs stay on Chat Completions. Selecting a chat-only provider such as Gemini uses Chat Completions even if the previous provider used Responses. Endpoint normalization is deterministic and reports surprising rewrites unless `--quiet` is set.

`--search QUERY` with a model request uses hosted provider `web_search` when `models.conf` marks the model `web_search=on`. `--no-builtin-web-search` forces the client Tavily/Firecrawl/Exa/Searxng/DuckDuckGo path. Standalone `--search` without a model request still uses client search.

## Script reliability

Use `--quiet` to suppress ordinary status and `-v` for timing. `--debug` prints configuration diagnostics with credentials redacted. Network, API, configuration, cancellation, and argument failures map to distinct exit codes. Errors include a safe provider response and next step where available.

Avoid `-k`: keys in process arguments may be visible locally. Prefer environment variables, `--key-env NAME`, `--key-file PATH`, or `--key-stdin`.

Native Windows uses the same stdout/stderr and exit-code contract. UCRT64 build,
portable package, PowerShell, profile paths, and supported console details are in
[Native Windows](windows.md).

Related documentation: [documentation index](README.md), [getting started](getting-started.md), [API compatibility](api-compatibility.md), [security](security.md).

## MCP server management

Install and manage Model Context Protocol servers for agent modes (not used by plain chat). Registry: `~/.ainiux/mcp/registry.json`.

```sh
ainiux --list-mcp
ainiux --add-mcp catalog --mcp-url https://awesome-mcp.tools/mcp
ainiux --add-mcp mock --mcp-url http://127.0.0.1:8765/mcp --mcp-allow-private
ainiux --add-mcp time --mcp-transport stdio -- npx -y @modelcontextprotocol/server-time
ainiux --enable-mcp NAME
ainiux --disable-mcp NAME
ainiux --remove-mcp NAME
```

These flags cannot be combined with `--agent` / `--run` / chat / editor. After install, run agent or `-r` so tools load as `mcp__<name>__<tool>`.

Full guide: [MCP servers](mcp.md).

## Image generation

`ainiux image` (or `--image`) generates **one** image from a prompt. The catalog in `images.conf` selects the protocol and model. OpenAI defaults to `gpt-image-2` (`openai_images`). `--provider replicate` uses official Replicate predictions (`replicate_predictions`); the default model is `prunaai/z-image-turbo`. This is a single-turn CLI mode: there is no TUI/REPL `/image`, no batch `n>1`, and no multi-turn editing.

```sh
ainiux image -p "a quiet terminal at night" --size 1536x1024 --output night.png
ainiux image -p "gift basket from these items" --attach lotion.png --attach soap.jpg --size 2k --ar 16:9
ainiux image -p "otter" --format webp --output stdout > otter.webp
ainiux image --provider replicate -m prunaai/z-image-turbo -p "a red cube"
ainiux image --provider replicate -m google/nano-banana-2 -p "make it night" --attach photo.png --size 1k --ar 1:1
```

- Prompt: `-p` / `--prompt` / `--prompt-file` (required)
- Input images: repeatable `--attach` PNG or JPEG when the matched record sets `edits = on`. OpenAI uses `/v1/images/edits`; Replicate puts data URLs in the model’s image array field (`image_input`, `input_images`, or `images`).
Image models and size/quality/format limits come from layered `images.conf` (not `models.conf`). Unknown `-m` values fail before HTTP. Replicate matching uses the final slash component, so `-m z-image-turbo` and `-m prunaai/z-image-turbo` both work. Credentials are `REPLICATE_API_KEY` or `REPLICATE_API_TOKEN` (not `AINIUX_API_KEY`).

- `--size`: `WIDTHxHEIGHT`, or `1k` / `2k` / `4k` / `auto`. OpenAI maps `2k`+`16:9` to `2048x1152`. Replicate enum models send the catalog token (`2K`, `1 MP`) and keep `--ar` separate.
- `--ar W:H` (and named values such as `match_input_image` when listed)
- `--quality low|medium|high|auto` (default auto; OpenAI Images)
- `--format png|jpeg|webp|auto` (codec; default from the catalog record)
- `--output PATH` writes that file; `stdout` writes raw bytes. If omitted, the first unused `imageN.EXT` in the current directory is used.
- `--force` overwrites an existing `--output` file.

Stdout prints the saved path (or raw bytes for `--output stdout`). Status goes to stderr. GPT Image models may require OpenAI organization verification. Ordinary chat never generates images from a text prompt.

