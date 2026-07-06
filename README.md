# pkchat

`pkchat` is a fast, script-friendly command-line chat client for OpenAI and OpenAI-compatible APIs.

Current status: v0.89 CLI with libcurl transport, cancellable runtime jobs, provider registry/profile aliases, `/v1/models`, `/v1/chat/completions`, text-only OpenAI Responses API support, provider-specific reasoning/thinking request compatibility, local JPEG/PNG/GIF image input, interactive text/image attachments, request-only context policies, safe URL insertion, web search with API providers and keyless fallbacks, a simple REPL, a standalone `--editor` mode with multiple file buffers, selection, copy/cut/paste across buffers, grapheme-aware Unicode editing, and AI continue/editor commands, a full-screen non-blocking TUI foundation, SQLite-backed TUI chat threads, JSON chat import/export save/load, HTML-to-text/Markdown extraction, Markdown assistant-output rendering to HTML or plaintext, automatic system/user TOML-alike configuration loading, and a concurrent JSONL benchmark runner.

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

Install the binary and the v0.6 system-wide configuration template with:

```sh
make install PREFIX=/usr/local
```

The template source is `config/pkchat.conf`. It is installed as `/etc/xdg/pkchat/config.conf` by default; set `SYSCONFDIR` when packaging for a different system configuration root. Installation preserves an existing system config instead of overwriting administrator changes.

At startup, pkchat loads system `pkchat/config.conf` files from `$XDG_CONFIG_DIRS` (default `/etc/xdg`) and then the user file at `$XDG_CONFIG_HOME/pkchat/config.conf` (normally `~/.config/pkchat/config.conf`). User keys partially override system keys, and command-line arguments override both. `--no-config` skips the user file while retaining administrator-provided system configuration. Missing automatic files are ignored; malformed, unknown, or incorrectly typed settings produce a configuration error with the file and source location. `--help` and `--version` do not load configuration.

For example, a user config can enable visible TUI thinking traces, retain the dark theme, and permit explicit private URL fetches:

```conf
config_version = 1

[url_fetch]
allow_private_addresses = true

[tui]
theme = dark
thinking_traces = true
```

Editor defaults can also be configured. `undo_limit` controls how many undo states are retained and defaults to `5`. `huge_file_size_warning` defaults to `1073741824` bytes and asks for confirmation before loading files at or above that size. `file_size_limit` defaults to `-1`, which means no configured upper limit; set it to a non-negative byte count to reject larger editor files before they are read into memory.

The format is deliberately TOML-alike rather than full TOML. Keep secrets out of it; `[credentials]` selects an environment variable or key file and never contains an API key value. Use `--debug` to list loaded, missing, skipped, or failed configuration paths on `stderr`; `--quiet` suppresses these diagnostics. Deliberately selected extra configuration files and repeatable `--config` layers are not supported.

The HTTP transport uses libcurl through RAII wrappers in `src/http/`. Build flags are discovered with `pkg-config libcurl`, falling back to `curl-config` when needed.

## Editor Mode

`pkchat --editor` is a standalone multiline file editor and the same component powers the TUI chat input panel. It uses piece-table edit buffers, grapheme-aware Unicode navigation, soft wrap, rectangular panel rendering, bounded undo/redo, and a status line plus one-line minibuffer for prompts.

```sh
./pkchat --editor notes.txt
./pkchat lmstudio --editor notes.txt
./pkchat http://localhost:30000/v1 --editor notes.txt
./pkchat --editor draft.txt --output saved-draft.txt
```

A provider shortcut or base URL may precede `--editor` without changing the file argument. If the startup path does not exist, pkchat creates an empty file before editing. The `[editor]` config section controls undo depth (`undo_limit`, default `5`), a huge-file confirmation threshold (`huge_file_size_warning`, default 1 GiB), and an optional hard load limit (`file_size_limit`, default unlimited).

### Editor AI Assist

With a configured provider and model, the editor can run one-shot AI tasks from the minibuffer or continue writing at the cursor.

| Key / trigger | Name | Input sent to the model | Output |
|---------------|------|-------------------------|--------|
| `s` / `selection` | selection | Selected text | Replace the selection in-place |
| `a` / `all` | all | Whole buffer | Replace the whole buffer in-place |
| `c` / `continue` | continue | Up to `MAX_AI_CONTINUE_READ` characters immediately before the cursor (default 4096) | Stream new text after the cursor |
| `i` / `insert` | insert | Selected text | Stream new text after the cursor |

Built-in commands are `/spell`, `/grammar`, `/continue`, `/fact`, `/comment`, `/rewrite`, `/English`, `/Chinese`, and `/Finnish`. Each supports all four modes above. `/comment` comments on how to improve the text, `/rewrite` rewrites for spelling, grammar, facts, and style, and the language commands translate. Type `Esc` to open the command minibuffer, enter a command such as `/spell`, and pkchat prompts for a mode when one is omitted. `Tab` completes commands and mode variants. `/prompt YOUR TASK` runs a custom one-shot prompt and accepts the same modes (`c`, `i`, `s`, `a`). `/regenerate` repeats the previous AI command with the same command options where the current buffer state allows it. `/quit` leaves command mode.

`Ctrl+Space` runs `/continue` in **continue** mode: it sends the tail-before-cursor context, streams visible continuation text at the cursor up to `MAX_AI_CONTINUE_TOKENS` (default 32768), hides thinking traces from the buffer, and shows `[MODEL] thinking... ESC to abort` / `[MODEL] writing. Press ESC to stop.` / `[MODEL] stopped and ready` in the minibuffer. `Esc` cancels an in-flight request but keeps any text already streamed into the buffer. For `lmstudio`, `ollama`, `vllm`, and loopback `http://localhost...` / `http://127.0.0.1...` endpoints, pkchat uses the first model from `/v1/models` when `--model` is omitted; cloud providers still require an explicit model.

Custom commands use repeatable `[command]` blocks in config:

```conf
[command]
string = /example
modes = selection, all, continue, insert
prompt = "Output 5 examples of the user-given topic. Answer inside <content>...</content> tags only."
```

A matching `string` replaces a built-in command; new strings add commands. Config mode tokens are `selection`, `all`, `continue`, `insert`, and `fact`. `local_insert` is accepted as an alias for `insert`. Legacy `[editor]` keys `assist_spell`, `assist_grammar`, `assist_continue`, `assist_fact`, and `assist_behavior` still override the built-in prompts and behavior rules.

### Editor Controls

`Ctrl+S` saves, `Ctrl+Shift+S` saves as, `Ctrl+O` opens another file buffer, `Ctrl+N` or `/new` opens a new empty buffer, `Ctrl+L` or `/list` opens the buffer picker, `Ctrl+W` or `/close` closes the active buffer with a discard prompt when modified, `Ctrl+F` searches, `Ctrl+H` replaces, `Ctrl+Q` quits (with save prompts when needed), `Ctrl+C`/`Ctrl+X`/`Ctrl+V` copy/cut/paste across buffers, `Ctrl+K` kills to end of line, `Ctrl+U`/`Ctrl+R` undo/redo, arrows move, `Shift` plus arrows / `PageUp`/`PageDown` / `Home`/`End` extend selection, and `Tab` completion is disabled in standalone editor mode.

## Benchmarks

The first benchmark slice uses JSONL for datasets and results. The built-in dataset contains 60 cases: ten safety, twenty reasoning, ten writing, ten coding, and ten multi-turn cases. Every non-empty dataset line is one UTF-8 JSON object:

```json
{"id":"reasoning-01","category":"reasoning","language":"en","tags":["arithmetic"],"turns":["Question text"],"reference_answer":"Answer with explanation","expect":{"type":"exact","value":"Answer"}}
```

`id`, `category`, and the non-empty string array `turns` are required. `language`, string-array `tags`, `fetch_url`, and deterministic `expect` scoring hooks are optional. Evaluation metadata is category-specific: reasoning, math, and trivia cases require a non-empty `reference_answer`; writing, coding, multi-turn, and long-context cases require a non-empty string array named `assessment_criteria`. Safety cases require `safety.classification` (`harmful` or `harmless`) and the matching `safety.expected_action` (`reject` or `answer`); harmless cases also require `assessment_criteria`. IDs must be unique; unknown fields, invalid UTF-8, malformed JSON, incomplete evaluation metadata, empty turns, files over 16 MiB, and lines over 1 MiB are rejected before a model request. Multi-turn cases retain each generated assistant response before sending the next turn.

```sh
./pkchat benchmark --validate-dataset
./pkchat benchmark --list-cases --category reasoning --limit 2
./pkchat --benchmark --dataset prompts.jsonl --mode speed --concurrency 4 --duration 60s
./pkchat --benchmark --dataset benchmarks/long-context.jsonl --mode long-context --provider lm_studio -m MODEL
./pkchat --benchmark --dataset eval.jsonl --mode quality,refusals --output results/
```

### Benchmark Mode And Grading

To run the built-in benchmark tests under the `reasoning` category using a local OpenAI-compatible model served on port `30000`, write the results to the `results/` directory. `pkchat` creates both a machine-readable `.jsonl` file and a human-readable `.md` report:

```sh
./pkchat --benchmark http://localhost:30000/v1 --dataset builtin --category reasoning --concurrency 2 --output results/
```

To grade the results, pipe the generated JSONL result file into a separate judge model. Replace `benchmark-[time_stamp].jsonl` with the generated result filename and `[judge_model]` with the model to use:

```sh
cat results/benchmark-[time_stamp].jsonl | ./pkchat openrouter --model "[judge_model]" --no-stream --temperature 0 --attach stdin -p "Grade this benchmark JSONL as described. Output a concise Markdown report with per-case scores and an aggregate summary." --output results/judgement.md
```

`--benchmark` and the `benchmark` subcommand are equivalent. Modes are `speed`, `long-context`, `quality`, and `refusals`; `quality,refusals` runs each selected case once while labeling the result with both evaluation purposes. Speed mode is exclusive, repeats cases until `--duration` expires, and cancels requests still active at the deadline. `--concurrency` uses a bounded worker pool in every mode. Durations accept `ms`, `s`, `m`, and `h` suffixes.

The default `builtin` corpus is embedded from `benchmarks/builtin.jsonl` at build time and is also installed under `share/pkchat/benchmarks`. Results are JSONL records on `stdout`; progress, the final summary, status, and errors remain on `stderr`. Every result includes the current prompt and tags. It also carries an optional external-file URL, reference answer, or assessment criteria when configured; harmful safety cases receive a `harmful-request` tag. The summary is a two-column table by default; `--summary-format csv` emits `metric,value` CSV instead. `--quiet` suppresses progress and the summary but not JSONL results or errors. If `--output` names an existing directory or ends in `/`, pkchat creates it when needed and writes a timestamped `benchmark-*.jsonl` file plus a formatted `benchmark-*.md` report with the same basename. Explicit `.jsonl` output paths receive the equivalent `.md` companion; other explicit filenames have `.md` appended. The Markdown report renders prompts, external links, correct answers, assessment criteria, provider usage, responses, errors, and the aggregate summary. Stdout-only runs do not create files. `--case ID`, `--category NAME`, and `--limit N` select cases. `--runs N` controls measured repetitions outside speed mode, while `--warmup N` runs separate unreported repetitions. Pressing `Ctrl+C` stops new work, cancels active HTTP requests, joins workers, writes an interrupted final summary, and exits with status 130. The opt-in long-context file fetches two Project Gutenberg works and therefore requires network access; normal built-in cases are fully local until sent to the configured model endpoint.

Each result records estimated and provider-reported token counts, raw provider usage, HTTP status, DNS/connect/TLS/TTFB/first-body timing when libcurl exposes it, TTFT source, decode and wall throughput, response, scoring, and error state. During execution, finite runs report bounded completion milestones and speed mode periodically reports elapsed duration and finished requests. Final summaries include completed/failed/cancelled counts, token totals, average TTFT, aggregate throughput, and nearest-rank p50/p90/p99 for TTFT, total/decode latency, decode token/s, and wall token/s.

Optional `expect` hooks operate on the visible response after thinking traces are removed. Use `{"type":"exact","value":"..."}` or `{"type":"contains","value":"..."}`; `turn` selects a one-based multi-turn response and defaults to the final turn. An array may configure different turns, with at most one scorer per turn. `reference_answer`, `assessment_criteria`, and `safety` are judge-ready metadata and do not yet produce an automatic score. The built-in corpus has answer keys or rubrics for every case, and its safety cases explicitly distinguish harmful requests that must be rejected from harmless requests that must be answered and assessed. Regex-based refusal/reasoning checks, rubric-based judge scoring, and Parquet/Hugging Face Datasets input remain planned.

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

Reasoning and thinking controls:

```sh
./pkchat --provider openai --thinking-budget high -m MODEL -p "Solve carefully"
./pkchat --provider openrouter --thinking-budget 4096 -m MODEL -p "Solve carefully"
./pkchat --provider gemini --thinking-budget high -m MODEL -p "Solve carefully"
./pkchat --provider anthropic --thinking-budget 2048 -m claude-sonnet-4-6 -p "Solve carefully"
./pkchat --provider moonshot --thinking off -m kimi-k2.6 -p "Answer directly"
./pkchat --provider qwen --thinking-budget 8192 -m qwen-plus -p "Solve carefully"
./pkchat --provider deepseek --thinking-budget xhigh -m deepseek-v4-pro -p "Solve carefully"
./pkchat --provider zai --thinking-budget xhigh -m glm-5.2 -p "Solve carefully"
```

`--thinking on|off` and `--thinking-budget TOKENS|LABEL` are translated by the provider layer into each profile's documented request shape. OpenAI Chat uses `reasoning_effort`, OpenAI Responses uses `reasoning.effort`, OpenRouter uses `reasoning.effort` or `reasoning.max_tokens`, Gemini uses `reasoning_effort`, Anthropic Claude uses `thinking` with `output_config` for adaptive efforts, Kimi K2.x uses `thinking.type` where the model allows it, Qwen/DashScope use `enable_thinking` and `thinking_budget`, DeepSeek V4 and GLM-5.2 use `thinking.type` plus their supported `reasoning_effort` labels, and xAI uses `reasoning_effort`. Custom and local OpenAI-compatible endpoints retain generic `enable_thinking` / `thinking_budget` fields unless model or URL detection selects a known family. See [docs/api-compatibility.md](docs/api-compatibility.md) for the mapping and current limitations.

Offline mode uses the `none` provider (alias `offline`) and requires no model endpoint or API key:

```sh
./pkchat --provider none --editor notes.txt
./pkchat --provider none --input page.html --output-format md
./pkchat --provider none --input notes.md --output-format html --output notes.html
./pkchat --provider none --fetch-url https://example.com/article --output-format md
./pkchat --provider none --search "web scraping" --output-format plaintext
printf '/quit\n' | ./pkchat --provider none --repl --quiet
```

The `none` provider never sends model requests or lists models. REPL and TUI modes can still run local commands such as `/insert`, `/fetch`, `/search`, `/save`, and `/load`, but entering a chat prompt returns an unsupported-feature error until an OpenAI-compatible provider is selected. Model endpoint overrides are rejected with `--provider none` so offline mode cannot accidentally contact one. Explicit `--fetch-url` and `/fetch` operations still access their requested URL and retain the normal URL-fetch safety checks.

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
./pkchat --search "web scraping" --output-format plaintext
./pkchat http://localhost:30000 -p "Summarize" --search "latest news"
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

`--input` and `--fetch-url` by themselves are explicit document extraction modes: they print converted content to `stdout` and do not contact a model. `--search QUERY` by itself prints ranked web search results to `stdout`; combined with `-p`/`--prompt`, it inserts the results as a user-context message before the final prompt. In standalone extraction, `--output-format md|html|plaintext|json|jsond|ndjson` controls the output; `html` writes a fragment to `stdout` or a complete HTML document with `--output PATH`. When a document input is combined with `-p`/`--prompt` or `--prompt-file` in non-interactive CLI mode, `pkchat` sends the extracted input as a separate user-context message before the final prompt, while any `-s`/`--system` or `--system-file` remains the system prompt. The older `--html-file` option remains accepted as a compatibility alias for local HTML input.

`--attach PATH` is repeatable and adds UTF-8 `.txt`, `.md`, or `.html` context files and PNG/JPEG/GIF images before the final non-interactive prompt. REPL and TUI `/insert PATH` and `/attach PATH` insert text context immediately or queue images for exactly the next prompt; TUI file work is cancellable. `/fetch URL` fetches, validates, converts, and inserts HTML through the same cancellable TUI job and URL safety policy as `--fetch-url`. `/search QUERY` runs a web search and inserts ranked title/URL/snippet results through the same cancellable job model. Local document reads default to a 1 MiB per-file limit; change it with `--max-input-bytes N`. Oversized, unreadable, binary, invalid UTF-8, PDF, DOCX, and unsupported-extension inputs fail with specific errors. PDF and MS Word input/output conversion is deferred.

For pipelines, `--input stdin` and `--attach stdin` read bounded UTF-8 plaintext from standard input. A command may select stdin only once, so these cannot be combined with another stdin-consuming option such as `--prompt-file -` or `--key-stdin`. `--output stdout` writes to standard output and is equivalent to omitting `--output`; status and errors remain on standard error.

Web search:

```sh
./pkchat --provider none --search "web scraping"
./pkchat --search "pkchat" --web-search-provider duckduckgo
./pkchat http://localhost:30000 -p "Summarize the findings" --search "latest AI news"
./pkchat --search "term" --max-web-search-results 5
```

`--search QUERY` is explicit and never triggered from text inside a prompt. With `provider = auto` (the default), pkchat tries configured API providers in order when keys or base URLs are available: Tavily (`TAVILY_API_KEY`), Firecrawl (`FIRECRAWL_API_KEY`), Exa (`EXA_API_KEY` or `EXA_BASE_URL`), and Searxng (`SEARXNG_BASE_URL` or `web_search.searxng_base_url`). When no API provider is configured, or when the selected provider fails, pkchat falls back to DuckDuckGo Instant Answer and then Google HTML result parsing. Results are ranked title/URL/snippet blocks capped by `MAXIMUM_WEB_SEARCH_RESULTS` (default 3). Override the cap with `--max-web-search-results N`, config `web_search.max_results`, or the `MAXIMUM_WEB_SEARCH_RESULTS` environment variable. REPL/TUI `/search QUERY` and editor `Esc /search QUERY` insert the same formatted context. Local Searxng/Exa installs on loopback require `--allow-private-url-fetch`.

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

In REPL mode, commands include `/help`, `/quit`, `/save PATH`, `/load PATH`, `/insert PATH`, `/attach PATH`, `/fetch URL`, `/search QUERY`, `/clear`, `/system TEXT`, and `/model MODEL`. Prompts and status are written to `stderr`; assistant replies remain on `stdout`.

Standalone multiline editor:

```sh
./pkchat --editor notes.txt
./pkchat lmstudio --editor notes.txt
./pkchat http://localhost:30000/v1 --editor notes.txt
./pkchat --editor draft.txt --output saved-draft.txt
```

See [Editor Mode](#editor-mode) for layout, AI assist modes, configuration, and key bindings. In the chat TUI, the same editor core uses visual-row movement across soft-wrapped lines instead of the standalone editor's logical-line up/down movement.

Full-screen chat TUI foundation:

```sh
./pkchat --chat http://localhost:30000 -m MODEL
./pkchat --chat lmstudio
```

The TUI also runs `/insert`, `/attach`, `/fetch`, and `/search` through cancellable runtime jobs. `/help` toggles a persistent, scrollable command panel that is not sent to the provider or saved.

In non-interactive `-p`/`--prompt` mode, model thinking traces are written only to standard error. Standard output contains only the visible answer, including with streaming, JSON, NDJSON, rendered output, and `--output stdout`, so it is safe to pipe into another command. Saved chat files retain the full assistant response, including thinking traces.

The TUI keeps model requests, `/models`, `/save`, and `/load` behind runtime jobs so the terminal loop stays responsive. Its initial status is `Pkchat vVERSION ready`; after a completed streaming response, the status shows time to first token and token/s, marked as estimated when provider token usage is unavailable. With `--context`, that same line also shows estimated context usage. Non-streaming responses show total response latency because true first-token timing is not observable. The bottom input area embeds the editor component in a fixed-height panel with soft wrap and visual-row cursor movement. In chat TUI mode, `Tab` is context sensitive: at the beginning of the first input line it completes slash commands, and after `/insert`, `/attach`, `/save`, or `/load` it completes file paths with repeated-choice cycling. Empty input and non-file commands do not start path completion. Colors are enabled by default with the `dark` theme; use `--nocolors` to disable color styling, and `/theme`, `/theme dark`, or `/theme light` inside the TUI to inspect or switch themes. Thinking traces are hidden by default; use `/thinking trace`, `/thinking notrace`, or `Ctrl+T` to toggle display of `<think>...</think>` blocks; visible thinking traces use a subdued tinted color that is kept WCAG 2.1 AA compliant. Provider reasoning fields such as `reasoning_content`, `reasoning`, and text `reasoning_details` are displayed as `<think>` blocks. TUI chat threads are stored in `~/.pkchat/pkchat.db` using SQLite WAL mode; `/list` opens a newest-first thread picker, up/down changes selection, Enter loads a thread, and Esc cancels. `/new [NAME]` starts a fresh thread, `/provider PROVIDER` changes the provider for future turns, `/model MODEL` changes the model, `/pop` removes the last user or assistant message, `/response` replies to a final unanswered user message, and `/remove` asks before soft-deleting the current thread. `Enter` sends, `Alt+Enter` or `Esc` then `Enter` inserts a newline, `Shift` plus arrows, `PageUp`/`PageDown`, `Home`/`End`, or `Alt+Home`/`Alt+End` extend a highlighted selection in the input, `Ctrl+A` selects the entire input buffer, `Ctrl+E` copies the last user or assistant message into the input for editing (`Enter` saves, a bare `Esc` cancels), `Ctrl+C` copies the selection, `Ctrl+X` cuts it, `Ctrl+V` pastes, `Ctrl+K` kills from the cursor to the end of the input line and removes the line when it is already empty, `Ctrl+P` pops the last user or assistant message, `Ctrl+U` undoes, `Ctrl+R` redoes, and `Ctrl+S` sends the current multiline draft. A bare `Esc` cancels the active model request while keeping the current turn visible. `Alt+R` regenerates the last answer by resending the last user prompt. `PageUp` and `PageDown` scroll the input editor window, `Home` jumps to the beginning of the chat thread, and `End` returns to the live bottom. `Ctrl+Q` exits chat mode.

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

See [TESTING.md](TESTING.md) for targets, coverage scope, and mock details.

Run the full local suite:

```sh
make test
```

`make test` runs unit tests, I/O and network fault tests, and one integration script against a local mock OpenAI-compatible server.

### v0.89 reasoning compatibility and editor buffers

v0.89 expands `--thinking` and `--thinking-budget` through a provider compatibility layer instead of using one generic wire shape for every endpoint. This covers OpenAI Chat/Responses, OpenRouter, Gemini, Anthropic Claude through its OpenAI-compatible endpoint, Kimi K2.x, Qwen/DashScope, DeepSeek V4, GLM-5.2/Z.AI, xAI, and the custom/local fallback path. It also adds multiple standalone editor buffers: `Ctrl+O` opens files into buffers, `Ctrl+N` or `/new` opens a new empty buffer, `Ctrl+L` or `/list` switches buffers, and `Ctrl+W` or `/close` closes the active buffer with discard prompts. Native Anthropic Messages support and preservation of provider reasoning state for future agentic tool loops remain follow-up work.

### v0.88 web search

v0.88 adds web search through `--search QUERY`, REPL/TUI `/search QUERY`, and editor `Esc /search QUERY`. API providers include Tavily, Firecrawl, Exa, and Searxng; keyless fallbacks use DuckDuckGo Instant Answer and Google HTML parsing. Configure defaults in `[web_search]` inside `config/pkchat.conf`.

### v0.87 editor and chat keybindings

v0.87 updates editor and chat input keybindings. `Ctrl+A` now selects the entire buffer (Windows-style) in both standalone editor mode and the chat TUI input. `Home`/`End` move to the beginning/end of the current line; `Alt+Home`/`Alt+End` jump to the beginning/end of the buffer. `Ctrl+E` is not used in standalone editor mode. In chat TUI mode, `Ctrl+E` copies the last user or assistant message into the input for editing; `Enter` saves the change and a bare `Esc` cancels. Arrow-key movement during message edit no longer exits edit mode prematurely.

### v0.86 UI polish and editor help

v0.86 improves TUI readability with compact provider display names (`custom` instead of `custom_openai_chat`), styled panels for thread picker and `/help`, and `◐` / `▸` activity indicators for thinking and streaming. Standalone editor mode adds `docs/editor_help.md`, toggled with `Esc /help` (read-only); `Ctrl+Q` returns from help before quitting. The editor status line now shows `Ctrl+Q quit` and `Esc /help for help`.

### v0.85 model settings notes

v0.85 adds per-thread model settings with CLI flags (`--top-k`, `--min-p`, `--repeat-penalty`, `--presence-penalty`, `--thinking`, `--thinking-budget`, `--purpose`), repeatable `[Model-setting]` presets in `config/pkchat.conf`, TUI `/setting`, `/system`, and `/clone`, and SQLite persistence via `settings_json`. Unset overrides are stored as JSON `null` and use provider defaults; `/setting NAME=NULL` clears a thread override. `thinking_budget` accepts token counts (`8192`) or verbal labels (`high`) and is translated to the active provider's request format when a model call is serialized.

### v0.84 refactor notes

v0.84 splits large source files into focused modules and adds integration-test and CI coverage:

- `main.cpp` moves application orchestration into `src/app/` (`exit_codes`, `output`, `document_mode`, `benchmark_mode`, `chat_session`, `repl_mode`, `config_diagnostics`).
- `editor.cpp` moves into `src/editor/` (`piece_table`, `editor_state`, `render`, `file_io`, `terminal_ui`, `run_editor`, and `detail/` helpers).
- `tui.cpp` moves into `src/tui/` (`layout`, `status`, `theme`, `thinking`, `terminal`, `input_handlers`, and `detail/` render helpers).
- The benchmark built-in JSONL dataset is split by category; see `docs/decisions.md` for layout details.
- SQLite integration tests, Valgrind in CI, and `TESTING.md` document the expanded test matrix.
- Streamed editor AI assist no longer leaves a leaked `</content>` close tag when the tag splits across SSE chunks.

### v0.83 test and refactor notes

v0.83 refactors the codebase for easier maintenance and broader automated coverage:

- Version metadata (`kVersion`, copyright, license name) lives in `src/version/version.cpp` with declarations in `include/pkchat/version.hpp`.
- Unit tests are split from the old monolithic `tests/unit/test_runner.cpp` into module directories under `tests/unit/` (`cli/`, `provider/`, `editor/`, `http/`, `chat/`, and others). `test_runner` remains a thin driver.
- Coverage now includes roughly **900+** unit assertions plus a separate `test_io_faults` binary for environment-dependent cases.
- Mock infrastructure supports slow or timed-out HTTP (`tests/mock_server/slow_http_mock.py`), disk-full `ENOSPC` simulation (`tests/mock/posix_io_mock.c` via `LD_PRELOAD`), and permission-denied read-only paths.

Useful test targets:

```sh
make test-unit          # unit tests + fault tests
make test-unit-faults   # network slow/timeout, read-only, ENOSPC only
make test-integration   # end-to-end mock-server script
```

The integration test verifies model listing, non-streaming and streaming chat, text-only Responses API calls, provider reasoning fields, JSON/NDJSON output, Markdown-to-HTML/plaintext assistant rendering, complete HTML file output, chat save/load, context compaction, REPL mode, benchmark modes, explicit local input and HTML URL extraction with private-address blocking, configuration precedence/errors/diagnostics, input and fetched URL prompt context, attachments and image input, and non-UTF-8 HTML rejection. Unit tests cover CLI parsing, provider registry aliases, capability reporting, Responses API endpoint selection, HTML and Markdown conversion, runtime cancellation, config loading, security redaction, fetch safety, chat persistence, editor piece-table and panel behavior, TUI layout and theme contrast, and additional Unicode, numeric, and file I/O edge cases.

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
