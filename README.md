# pkchat

`pkchat` is a fast, script-friendly command-line chat client for OpenAI and OpenAI-compatible APIs.

Current status: v0.96 CLI with libcurl transport, cancellable runtime jobs, provider registry/profile aliases, `/v1/models`, `/v1/chat/completions`, text-only OpenAI Responses API support, provider-specific reasoning/thinking request compatibility, local JPEG/PNG/GIF image input, interactive text/image attachments, request-only context policies, safe URL insertion, web search with API providers and keyless fallbacks, a simple REPL, a standalone `--editor` mode with multiple file buffers, selection, copy/cut/paste across buffers, grapheme-aware Unicode editing, indexed cross-buffer word completion, multi-language syntax highlighting and indentation reformatting, and AI continue/editor commands, a full-screen non-blocking TUI foundation, SQLite-backed TUI chat threads, JSON chat import/export save/load, HTML-to-text/Markdown extraction, Markdown assistant-output rendering to HTML or plaintext, automatic system/user TOML-alike configuration loading, and a concurrent JSONL benchmark runner.

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

Install the binary, system configuration templates, and bundled theme/editor-command files with:

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
highlight = on
```

Editor defaults can also be configured. `undo_limit` controls how many undo states are retained and defaults to `5`. `huge_file_size_warning` defaults to `1073741824` bytes and asks for confirmation before loading files at or above that size. `file_size_limit` defaults to `-1`, which means no configured upper limit; set it to a non-negative byte count to reject larger editor files before they are read into memory. Auto-save settings (`auto-save-mode`, `auto-save-postfix`, `auto-save-threshold`, `auto-save-timeout`, `auto-save-size-limit`) write backup copies such as `notes.txt~` while you edit; defaults are `on`, `~`, `300` bytes changed, `30` idle seconds, and `10M` max buffer size. `tab-width` (1–32, default `4`) and `tab-style` (`spaces` or `tab`, default `spaces`) are fallbacks for new files and existing files without reliable indentation evidence; `linebreak` (`lf`, `cr`, or `crlf`, default `lf`) initializes new buffers and ambiguous line-ending cases.

### Themes

TUI and editor colors are defined in repeatable `[theme]` blocks in `themes.conf`, not in `config.conf`. At startup pkchat loads system themes from `$XDG_CONFIG_DIRS/pkchat/themes.conf` (default `/etc/xdg/pkchat/themes.conf`), then the user file at `$XDG_CONFIG_HOME/pkchat/themes.conf` (normally `~/.config/pkchat/themes.conf`), and finally the bundled `config/themes.conf` if no other file was found. A theme with the same `name` replaces an earlier definition; a new `name` adds a selectable theme.

Each `[theme]` block needs `name` plus every color key below. Colors use `#RRGGBB` (or `0xRRGGBB`).

| Key | Used for |
| --- | --- |
| `background` | Main chat/editor background |
| `text` | Default body text |
| `muted` | Secondary labels and dim text |
| `thinking_trace` | `<think>...</think>` trace text |
| `user_label` | User message labels |
| `assistant_label` | Assistant message labels |
| `error` | Error status text |
| `status_foreground`, `status_background` | Status line and input label bar |
| `thinking_activity`, `streaming_activity` | Thinking/streaming indicators |
| `panel_title`, `panel_border`, `panel_hint`, `panel_highlight`, `panel_body`, `panel_background` | Help, settings, and picker panels |
| `syntax_comment`, `syntax_keyword`, `syntax_type`, `syntax_string`, `syntax_number`, `syntax_literal` | Syntax tokens (optional in custom themes) |
| `syntax_function`, `syntax_variable`, `syntax_operator`, `syntax_preprocessor` | Syntax identifiers and punctuation (optional) |
| `syntax_tag`, `syntax_attribute`, `syntax_property`, `syntax_heading`, `syntax_emphasis`, `syntax_link` | Markup and Markdown tokens (optional) |

Built-in themes are `dark`, `light`, and `sepia`. Set the default in `config.conf`:

```conf
[tui]
theme = sepia
colors = true
```

Switch at runtime in chat TUI or editor command mode:

```text
/theme
/theme sepia
/theme dark
```

`/theme` with no argument lists available themes. Use `--nocolors` to disable 24-bit ANSI styling.

### Syntax highlighting

Syntax highlighting is enabled by default in the standalone editor, chat input, and raw Markdown chat history. The editor supports text, Markdown, Python, C, C++, C#, Java, JavaScript/JSX, TypeScript/TSX, HTML, HTML-only, CSS3, XML, JSON/JSONL, Bash, PHP, Perl, Ruby, Rust, Go, PowerShell, Assembly, SQL, TOML, YAML, and INI. Markdown fenced blocks delegate to these language modes when their info string uses a recognized name or alias; unknown and untagged fences remain plain text.

Inline destinations such as the URL in `[link text](http://example.com)` use the contrasting `syntax_attribute` color, while the link text and Markdown delimiters use `syntax_link`. Both bundled colors meet the same WCAG AA contrast target as the other syntax roles.

Modes are detected case-insensitively from common endings, including `.h` as C, C++ source/header/template endings, `.py`/`.pyw`/`.pyi`, JS/JSX, TS/TSX, HTML, CSS, XML/SVG, JSON variants, Bash scripts/startup files, PHP variants, `.pl`/`.pm`/`.pod`/`.t`, Ruby files and standard build filenames, `.rs`, `.go`, PowerShell module/data files, `.asm`/`.s`, `.sql`, `.toml`, `.yaml`/`.yml`, and `.ini`/`.cfg`. `.html` and `.htm` select `html`; XML-oriented `.xhtml` selects `htmlonly`. Scratch buffers, `.txt`, and unknown endings stay in `text` mode. In editor command mode:

```text
/highlight
/highlight off
/highlight on
/mode
/mode markdown
/mode md
/mode python
/mode cpp
/mode typescript
/mode html
/mode htmlonly
/mode bash
/mode php
/mode rust
/mode powershell
/mode sql
/mode yaml
/mode text
/mode auto
/reformat
/reformat-all
```

Canonical modes are `text`, `markdown`, `python`, `c`, `cpp`, `csharp`, `java`, `javascript`, `typescript`, `html`, `htmlonly`, `css`, `xml`, `json`, `bash`, `php`, `perl`, `ruby`, `rust`, `go`, `powershell`, `assembly`, `sql`, `toml`, `yaml`, and `ini`. Accepted aliases include `md`, `py`, `c++`, `cxx`, `c#`, `cs`, `js`, `ts`, `html5`, `html-multi`, `htmlmulti`, `html-only`, `css3`, `jsonl`, `ndjson`, `sh`, `shell`, `pl`, `rb`, `rs`, `golang`, `pwsh`, `ps1`, `asm`, `yml`, and `dosini`. The default `html` mode highlights markup, JavaScript inside `<script>` and `on*=""` event attributes, and CSS inside `<style>` and `style=""` attributes. It preserves nested multiline state and continued tags. Use `htmlonly` when markup-only highlighting is preferred; embedded code is then treated as strings. A selected mode is a manual per-buffer override. `/mode auto` resumes filename detection; save-as only re-detects while the buffer remains automatic. `/highlight` is process-wide and shared when switching between editor and chat. Chat supports `/highlight [on|off]` but not `/mode`. Interactive commands do not rewrite config.

The editor status line displays the active syntax language and line-ending mode compactly, for example `(html LF)` or `(python CRLF)`. Bare `/mode` reports whether language detection is automatic or manually selected.

`/reformat` reformats the leading indentation of the selected lines according to the active language mode; `/reformat-all` does the whole buffer. Reformatting preserves token spacing, trailing whitespace, blank lines, line endings, and string/comment contents, and applies as one undoable edit. Brace-based languages, Ruby, Bash, HTML/XML, SQL, Python, YAML, Markdown, TOML, INI, and Assembly use conservative built-in profiles. YAML indentation is always spaces. Text mode asks you to select a `/mode` instead of guessing. Large operations run in the background; editing and buffer switching remain available, `Esc` cancels, and a result is discarded if its source buffer or indentation settings changed.

Set the startup default with `highlight = on|off` (or a boolean) under `[tui]`. `--nocolors` suppresses syntax colors while selection remains visible. Existing custom themes remain valid when syntax keys are omitted; accessible colors are derived from their existing semantic colors.

Complete example: add or override the sepia theme in `~/.config/pkchat/themes.conf`:

```conf
[theme]
name = sepia
background = #F4ECD8
text = #5B4636
muted = #7A6A58
thinking_trace = #6E5F4D
user_label = #8B5E34
assistant_label = #4F6F46
error = #9B2C2C
status_foreground = #5B4636
status_background = #E8DCC8
thinking_activity = #8B5E34
streaming_activity = #4F6F46
panel_title = #8B5E34
panel_border = #7A6A58
panel_hint = #6E5F4D
panel_highlight = #B7791F
panel_body = #5B4636
panel_background = #EFE2C8
```

Copy an existing built-in block from `config/themes.conf`, change `name` and the colors, save the file, then run `/theme YOUR_THEME` or set `theme = YOUR_THEME` under `[tui]`. `make install` also places the bundled themes file at `/etc/xdg/pkchat/themes.conf` and `share/pkchat/themes.conf`.

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

A provider shortcut or base URL may precede `--editor` without changing the file argument. If the startup path does not exist, pkchat creates an empty file before editing. The `[editor]` config section controls undo depth (`undo_limit`, default `5`), a huge-file confirmation threshold (`huge_file_size_warning`, default 1 GiB), an optional hard load limit (`file_size_limit`, default unlimited), auto-save backup behavior, and the initial indentation and line-ending settings.

LF, CR, and CRLF files are normalized internally and saved back with their detected line-ending style, including whether the file has a final line ending. Empty files and files without any line ending use the configured default. A mixed-ending file produces a warning and uses the configured `linebreak` style on its next save. When an existing file is opened or recovered, the editor examines at most its first 20 physical lines and adopts a consistent space-indentation width or tab style. One-line, unindented, mixed-style, and inconsistent samples retain the configured `tab-width` and `tab-style` fallbacks. `/linebreak lf|cr|crlf` changes the active buffer’s save style; `/tab-width 1..32` and `/tab-style spaces|tab` override its detected indentation behavior. With no argument, each command reports the active value. These settings are per buffer.

### Editor AI Assist

A provider shortcut or profile may precede `--editor` without a model, matching `--chat` startup: the editor opens immediately and the minibuffer shows **Choose a model with /model**. Use `Esc` then `/model` (or the model picker) to select a model; AI assist stays disabled until then. `pkchat --provider none --editor` and plain `pkchat --editor` run offline as local editors.

With a configured provider and model, the editor can run one-shot AI tasks from the minibuffer or continue writing at the cursor.

| Key / trigger | Name | Input sent to the model | Output |
|---------------|------|-------------------------|--------|
| `s` / `selection` | selection | Selected text | Replace the selection in-place |
| `a` / `all` | all | Whole buffer | Replace the whole buffer in-place |
| `n` / `newbuffer` | new buffer | Selected text | Stream into a new editor buffer |
| `i` / `insert` | insert | Selected text | Stream new text after the cursor |

Built-in commands are `/spell`, `/grammar`, `/continue`, `/fact`, `/comment`, `/rewrite`, `/English`, `/Chinese`, and `/Finnish`. All except `/continue` support `selection`, `all`, `newbuffer`, and `insert`. `/continue` is continue-only and is also bound to `Ctrl+Space`. `/comment` comments on how to improve the text, `/rewrite` rewrites for spelling, grammar, facts, and style, and the language commands translate. Type `Esc` to open the command minibuffer, enter a command such as `/spell`, and pkchat prompts for a mode when one is omitted. `Tab` completes commands and mode variants. `/prompt YOUR TASK` runs a custom one-shot prompt with the same scoped choices: selection (`s`), all (`a`), insert (`i`), or new buffer (`n`). `/regenerate` repeats the previous AI command with the same command options where the current buffer state allows it. `/quit` leaves command mode.

`Ctrl+Space` runs `/continue` in **continue** mode: it sends the tail-before-cursor context, streams visible continuation text at the cursor up to `MAX_AI_CONTINUE_TOKENS` (default 32768), hides thinking traces from the buffer, and shows `[MODEL] thinking... ESC to abort` / `[MODEL] writing. Press ESC to stop.` / `[MODEL] stopped and ready` in the minibuffer. `Esc` cancels an in-flight request but keeps any text already streamed into the buffer. Editor mode does not auto-select a model at startup; choose one with `/model` after `pkchat openrouter --editor`, `pkchat lmstudio --editor`, or similar. Provider/model pickers are also available through `Esc` then `/provider` or `/model`.

In the chat TUI, the same built-in editor AI commands are available as slash commands. A bare command such as `/Chinese` submits that command's prompt as a normal chat turn. `/Chinese n` (or `newbuffer`) with selected input text switches to the editor and runs the command in **new buffer** mode there.

Custom commands use repeatable `[command]` blocks in config:

```conf
[command]
string = /example
modes = selection, all, newbuffer, insert
prompt = "Output 5 examples of the user-given topic. Answer inside <content>...</content> tags only."
```

A matching `string` replaces a built-in command; new strings add commands. Config mode tokens are `selection`, `all`, `newbuffer` (`new` or `n`), `continue`, `insert`, and `fact`. `local_insert` is accepted as an alias for `insert`. Legacy `[editor]` keys `assist_spell`, `assist_grammar`, `assist_continue`, `assist_fact`, and `assist_behavior` still override the built-in prompts and behavior rules.

### Editor Controls

`Ctrl+S` saves, `Ctrl+Shift+S` saves as, `Ctrl+O` opens another file buffer, `Ctrl+N` or `/new` opens a new empty buffer, `Ctrl+L` or `/list` opens the buffer picker, `Ctrl+W` or `/close` closes the active buffer with a discard prompt when modified, `Ctrl+F` searches, `Ctrl+H` replaces, `Ctrl+Q` quits (with save prompts when needed), `Ctrl+C`/`Ctrl+X`/`Ctrl+V` copy/cut/paste across buffers, `Ctrl+K` kills to end of line, `Ctrl+Z`/`Ctrl+U` undo and `Ctrl+Y` redo, `Home`/`End` move to the current line, `Ctrl+Home`/`Ctrl+End` jump to buffer bounds, arrows move, and `Shift` plus arrows / `PageUp`/`PageDown` / `Home`/`End` extend selection. After a word or symbol prefix, `Tab` completes from words in every open editor buffer. With multiple matches it first inserts their common prefix, then repeated `Tab` presses rotate through full candidates; the whole completion session is one undoable edit. Lowercase prefixes match with Unicode case folding, while a prefix containing uppercase letters is case-sensitive. If no word matches, `Tab` inserts indentation. With a selection it indents every touched line instead. `Shift+Tab` outdents the current line or selected block. Document completion is separate from command/path completion in the `Esc` minibuffer and from chat-input completion.

## Benchmarks

The first benchmark slice uses JSONL for datasets and results. The built-in dataset contains 103 cases: ten safety, twenty reasoning, ten writing, ten coding, ten multi-turn, and forty-three cutoff cases. Cutoff cases ask one dated factual question per month from January 2023 through July 2026 to help estimate a model's knowledge cutoff; run them with `--category cutoff`. Every non-empty dataset line is one UTF-8 JSON object:

```json
{"id":"reasoning-01","category":"reasoning","language":"en","tags":["arithmetic"],"turns":["Question text"],"reference_answer":"Answer with explanation","expect":{"type":"exact","value":"Answer"}}
```

`id`, `category`, and the non-empty string array `turns` are required. `language`, string-array `tags`, `fetch_url`, and deterministic `expect` scoring hooks are optional. Evaluation metadata is category-specific: reasoning, math, trivia, and cutoff cases require a non-empty `reference_answer`; writing, coding, multi-turn, and long-context cases require a non-empty string array named `assessment_criteria`. Safety cases require `safety.classification` (`harmful` or `harmless`) and the matching `safety.expected_action` (`reject` or `answer`); harmless cases also require `assessment_criteria`. IDs must be unique; unknown fields, invalid UTF-8, malformed JSON, incomplete evaluation metadata, empty turns, files over 16 MiB, and lines over 1 MiB are rejected before a model request. Multi-turn cases retain each generated assistant response before sending the next turn.

```sh
./pkchat benchmark --validate-dataset
./pkchat benchmark --list-cases --category reasoning --limit 2
./pkchat --benchmark --dataset prompts.jsonl --mode speed --concurrency 4 --duration 60s
./pkchat --benchmark --dataset benchmarks/long-context.jsonl --mode long-context --provider lm_studio -m MODEL
./pkchat --benchmark --dataset eval.jsonl --mode quality,refusals --output results/
```

### Running Benchmarks Against A Local Endpoint

`--benchmark` and the `benchmark` subcommand are equivalent. Point them at any OpenAI-compatible base URL, select cases, and write results to a directory:

```sh
mkdir -p results

./pkchat --benchmark http://localhost:30000/v1 \
  -m "Gemma-4-26B-A4B" \
  --dataset builtin \
  --category reasoning \
  --mode quality \
  --runs 1 \
  --concurrency 2 \
  --output results/
```

`http://localhost:30000/v1` is the usual form. Bare `http://localhost:30000` also works because `pkchat` probes `/v1` when needed. Progress and the timing summary go to `stderr`. When `--output` names a directory (or ends in `/`), `pkchat` creates it if needed and writes:

```text
results/benchmark-<timestamp>.jsonl
results/benchmark-<timestamp>.md
```

The `.jsonl` file is machine-readable; the `.md` report is the easiest file for human review. Stdout-only runs do not create files.

Useful selection flags:

```sh
./pkchat benchmark --dataset builtin --category cutoff --list-cases
./pkchat benchmark http://localhost:30000/v1 -m MODEL --dataset builtin --category cutoff --limit 3 --output results/
./pkchat benchmark http://localhost:30000/v1 -m MODEL --dataset builtin --case cutoff-2024-11 --output results/
```

`--case ID`, `--category NAME`, and `--limit N` narrow the run. `--runs N` repeats each selected case outside speed mode; `--warmup N` runs extra unreported warmups first.

### Knowledge Cutoff Benchmarks

The `cutoff` category contains forty-three dated factual questions, one per month from January 2023 through July 2026. Each case tags its event month (for example `2023-03`) and includes a `reference_answer`. The goal is to estimate where a model's knowledge ends by seeing which recent events it answers correctly, refuses, or hallucinates.

Run the full cutoff set against a local model and save results under `results/`:

```sh
mkdir -p results

./pkchat --benchmark http://localhost:30000/v1 \
  -m "Gemma-4-26B-A4B" \
  --dataset builtin \
  --category cutoff \
  --mode quality \
  --runs 1 \
  --concurrency 2 \
  --output results/
```

Start with `--limit 3` if you want a quick smoke test before all forty-three cases. Read cases in chronological order by month tag when grading.

To estimate a cutoff window manually, walk the `.md` report from oldest to newest month and note:

- **Correct or close enough** — the model likely knows events through that month.
- **Wrong but confident** — the model may be past its cutoff or hallucinating.
- **Refusal or uncertainty** — the cutoff may be before that event.
- **Vague or hedged** — treat separately; do not force a hard pass/fail.

A practical cutoff estimate is the range from the last month with reliably correct answers to the first month with clearly wrong or fabricated answers.

Automatic `--mode cutoff` inference and separate cutoff summaries are planned; today use the steps below.

### Grading Benchmark Results

Automatic scoring is limited. `pkchat` only auto-scores cases that define an `expect` hook such as `{"type":"contains","value":"March 14, 2023"}`. Most built-in cases, including all cutoff cases, carry `reference_answer` metadata for review but leave `"score": null` unless `expect` is present. That is expected.

**Option 1: read the Markdown report**

Open `results/benchmark-<timestamp>.md`. Each case shows the prompt, correct answer, model response, timing, and token usage side by side.

**Option 2: judge with another model**

Pipe the JSONL results to a stronger model:

```sh
RESULT=$(ls -t results/benchmark-*.jsonl | head -1)

cat "$RESULT" | ./pkchat openrouter \
  --model "your-judge-model" \
  --no-stream \
  --temperature 0 \
  --attach stdin \
  -p 'You are grading a knowledge-cutoff benchmark.

For each JSONL record with category "cutoff":
1. Compare "response" to "reference_answer".
2. Classify as: correct, partially_correct, incorrect, refused_or_unknown, or hallucinated_beyond_cutoff.
3. Use the month tag (for example 2024-11) as the event month.
4. At the end, estimate the model knowledge cutoff window:
   - last_month_confidently_correct
   - first_month_clearly_wrong_or_hallucinated
   - brief reasoning

Output Markdown with a per-case table and a final cutoff summary.' \
  --output results/cutoff-judgement.md
```

Replace `openrouter` and `your-judge-model` with whichever judge endpoint and model you use. The same pattern works for `reasoning`, `writing`, and other categories: ask the judge to compare `response` against `reference_answer` or `assessment_criteria`.

**Option 3: quick JSONL scan**

```sh
jq -r 'select(.type=="result") | [.id, .tags[1], .reference_answer, .response] | @tsv' \
  results/benchmark-*.jsonl | less
```

**CSV summary on stderr**

```sh
./pkchat benchmark http://localhost:30000/v1 -m MODEL \
  --dataset builtin --category cutoff \
  --summary-format csv \
  --output results/ 2> results/cutoff-summary.csv
```

Modes are `speed`, `long-context`, `quality`, and `refusals`; `quality,refusals` runs each selected case once while labeling the result with both evaluation purposes. Speed mode is exclusive, repeats cases until `--duration` expires, and cancels requests still active at the deadline. `--concurrency` uses a bounded worker pool in every mode. Durations accept `ms`, `s`, `m`, and `h` suffixes.

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

`--attach PATH` is repeatable and adds UTF-8 `.txt`, `.md`, or `.html` context files and PNG/JPEG/GIF images before the final non-interactive prompt. Interactive `/attach PATH` retains the attachment path: text becomes context and supported images are queued for exactly the next prompt. `/insert FILE_OR_URL` is separate: it accepts any local file ending when the contents are bounded UTF-8 text and inserts those contents at the active cursor in the editor or full-screen chat draft. An HTTP(S) URL is fetched with the normal URL safety policy and converted from UTF-8 HTML to Markdown by default. Set `[input] auto-convert-html-to-md = no`, use chat `/setting auto-convert-html-to-md=no`, or use editor `/auto-convert-html-to-md no` to insert raw HTML instead. Local and URL insertion work is cancellable. `/fetch URL` remains the command for adding converted URL context to chat history, while `/search QUERY` adds ranked search context. Local reads default to a 1 MiB limit; change it with `--max-input-bytes N`. Oversized, unreadable, binary, and invalid UTF-8 insertion inputs fail with specific errors. PDF and MS Word attachment conversion remains deferred.

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

In REPL mode, commands include `/help`, `/quit`, `/save PATH`, `/load PATH`, `/insert FILE_OR_URL`, `/attach PATH`, `/fetch URL`, `/search QUERY`, `/clear`, `/system TEXT`, and `/model MODEL`. Because the line-oriented REPL has no editable draft cursor, inserted text is added as visible context; the full-screen chat and editor modes insert directly at the cursor. Prompts and status are written to `stderr`; assistant replies remain on `stdout`.

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

The TUI also runs `/insert`, `/attach`, `/fetch`, and `/search` through cancellable runtime jobs. `/insert FILE_OR_URL` places text in the chat input at its cursor; `/attach PATH` continues to prepare provider context or an image for the next turn. `/help` toggles a persistent, scrollable command panel that is not sent to the provider or saved.

In non-interactive `-p`/`--prompt` mode, model thinking traces are written only to standard error. Standard output contains only the visible answer, including with streaming, JSON, NDJSON, rendered output, and `--output stdout`, so it is safe to pipe into another command. Saved chat files retain the full assistant response, including thinking traces.

The TUI keeps model requests, `/models`, `/save`, and `/load` behind runtime jobs so the terminal loop stays responsive. Its initial status is `Pkchat vVERSION ready`; after a completed streaming response, the status shows time to first token and token/s, marked as estimated when provider token usage is unavailable. With `--context`, that same line also shows estimated context usage. Non-streaming responses show total response latency because true first-token timing is not observable. The bottom input area embeds the editor component in a fixed-height panel with soft wrap and visual-row cursor movement. In chat TUI mode, `Tab` is context sensitive: at the beginning of the first input line it completes slash commands, and after `/insert`, `/attach`, `/save`, or `/load` it completes file paths with repeated-choice cycling. Empty input and non-file commands do not start path completion. Colors are enabled by default with the `dark` theme; use `--nocolors` to disable color styling, `/theme` or `/theme NAME` to inspect or switch themes, and `/highlight on|off` to toggle raw Markdown highlighting. Thinking traces are hidden by default; use `/thinking trace`, `/thinking notrace`, or `Ctrl+T` to toggle display of `<think>...</think>` blocks; visible thinking traces retain their dedicated style while surrounding Markdown is highlighted. Provider reasoning fields such as `reasoning_content`, `reasoning`, and text `reasoning_details` are displayed as `<think>` blocks. TUI chat threads are stored in `~/.pkchat/pkchat.db` using SQLite WAL mode; `/list` opens a newest-first thread picker, up/down changes selection, Enter loads a thread, and Esc cancels. `/new [NAME]` starts a fresh thread, `/provider PROVIDER` changes the provider for future turns, `/model MODEL` changes the model, `/pop` removes the last user or assistant message, `/response` replies to a final unanswered user message, and `/remove` asks before soft-deleting the current thread. `Enter` sends, `Alt+Enter` or `Esc` then `Enter` inserts a newline, `Shift` plus arrows, `PageUp`/`PageDown`, `Home`/`End`, or `Ctrl+Home`/`Ctrl+End` extend a highlighted selection in the input, `Ctrl+A` selects the entire input buffer, `Ctrl+E` copies the last user or assistant message into the input for editing (`Enter` saves, a bare `Esc` cancels), `Ctrl+C` copies the selection, `Ctrl+X` cuts it, `Ctrl+V` pastes, `Ctrl+K` kills from the cursor to the end of the input line and removes the line when it is already empty, `Ctrl+Z` or `Ctrl+U` undoes, `Ctrl+Y` redoes, and `Ctrl+S` sends the current multiline draft. A bare `Esc` cancels the active model request while keeping the current turn visible. `Ctrl+R`, `Alt+R`, or `Esc` then `R` regenerates the last answer by resending the last user prompt. `/pop` removes the last user or assistant message. `Home`/`End` move to the current input line, `Ctrl+Home`/`Ctrl+End` jump to buffer bounds, `PageUp`/`PageDown` page through the input like the editor, `Ctrl+B` and `Ctrl+D` scroll chat history back and forward, and `Alt+Home`/`Alt+End` jump to the oldest history or live bottom. `Ctrl+Q` exits chat mode.

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

### v0.95 multi-language syntax highlighting

v0.95 expands the shared editor/chat highlighter from Markdown to Python, C, C++, C#, Java, JavaScript/JSX, TypeScript/TSX, HTML, HTML-only, CSS, XML, JSON/JSONL, and Bash. The editor detects common filename endings, supports manual per-buffer `/mode` overrides and automatic re-detection, and highlights recognized languages inside Markdown fences. The default `html` mode delegates script/style elements and inline event/style attributes to JavaScript and CSS; `htmlonly` retains markup-only highlighting. Multiline comments, strings, Bash heredocs, XML CDATA, HTML tags, and embedded script/style blocks retain state across lines.

### v0.94 editor and chat mode switching

v0.94 lets you switch between standalone editor mode and chat TUI without restarting: `Ctrl+P`, editor `Esc /chat`, or chat `/editor`. Provider, model, editor buffers, and the active chat thread are preserved across switches.

### v0.91 cutoff benchmark updates

v0.91 refreshes two late-2026 cutoff benchmark cases (March and April 2026), adds `find_cutoff.sh` to grade cutoff JSONL results with a judge provider/model, and documents the workflow in the Benchmarks section.

```sh
./find_cutoff.sh deepseek deepseek-v4-flash results/benchmark-<timestamp>.jsonl
```

### v0.90 keyboard shortcuts and roadmap

v0.90 unifies chat and editor keyboard shortcuts: `Ctrl+Z`/`Ctrl+U` undo, `Ctrl+Y` redo, `Ctrl+Home`/`Ctrl+End` buffer bounds, and `PageUp`/`PageDown` for in-input paging. Chat mode adds `Ctrl+R` regenerate, `Ctrl+B`/`Ctrl+D` chat-history scroll (for terminals that block `Alt+PageUp`/`Alt+PageDown`), and `Alt+Home`/`Alt+End` jump to thread top/bottom. `PLANS.md` now targets v0.9 work (benchmark cutoff mode, codebase refactor, TUI/CLI polish) before local OpenAI-compatible server mode.

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
