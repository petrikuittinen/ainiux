# Ainiux

**Ainiux is a fast, portable AI chat client and AI-powered text editor for the terminal.**

Talk to local and cloud models from one tool. Write and rewrite documents with dozens of AI editor commands. Script chat, convert documents, run benchmarks, and keep your keys and data under your control.

One binary. No Electron. No browser tab. Built in C++17 for Linux on **x86-64** and **ARM64**.

## What it solves

Most people juggle a web chat, a separate code editor, and half a dozen provider dashboards. Ainiux puts the pieces that matter for real work into one local program:

- **Chat** with the model you already run (LM Studio, Ollama, vLLM, llama.cpp, …) or with cloud providers
- **Edit** text and code with AI assist that inserts into your buffer, not into a chat sidebar you have to copy-paste from
- **Script** prompts and pipelines from the shell (`stdout` for model output, `stderr` for status)
- **Evaluate** models with concurrent JSONL benchmarks and optional judge-model grading

You stay on your machine. API keys stay in the environment or key files. Local servers stay local.

## Why Ainiux stands out

| | What you get |
| --- | --- |
| **AI-powered editor** | Standalone `--editor` with **50+ built-in AI commands** (`/spell`, `/grammar`, `/rewrite`, `/summarize`, `/refactor`, translation into many languages, and more). Apply to the selection, whole buffer, insert at cursor, or open a new buffer (including split panes). |
| **Your own commands** | Add or override commands with simple `[command]` blocks in config—no recompile. |
| **Many providers, one UX** | OpenAI, OpenRouter, LM Studio, Ollama, vLLM, DeepSeek, Gemini, xAI, Qwen, and many other OpenAI-compatible endpoints through one registry. Switch with `/provider` and `/model`. |
| **Continue where you are** | `Ctrl+Space` continues prose or code at the cursor with bounded context—not a blank chat. |
| **Real terminal UI** | Full-screen chat TUI that stays responsive while streaming; SQLite thread library; themes and syntax highlighting. |
| **Script-friendly CLI** | One-shot prompts, REPL, document convert (`--input` / `--fetch-url`), benchmarks, and grading. |
| **Local-first** | Works offline with `--provider none` for editing and conversion; optional local models when you want AI. |

> Screenshots and short demos will be added here.

## Naming story

The project started as **pkchat**—a private short name for a personal chat client. As the program grew into a serious editor, benchmark tool, and multi-provider client, that name was already widely used elsewhere.

**Aini** is the name of the author’s youngest child, and it also echoes Chinese **爱你** (*ài nǐ*, “love you”). A short product name built only on *Aini* was attractive but crowded (including other AI assistants and common given names). Longer forms such as **Ainix** were likewise already claimed in overlapping spaces.

**Ainiux** was chosen before going public: personal roots, a distinctive product spelling, and room for a long-term ambition (tool → language → platform). The command-line binary is lowercase **`ainiux`**.

## Install on Ubuntu Linux

Tested on **Ubuntu** for **x86-64** and **ARM64** (including machines such as NVIDIA DGX Spark-class ARM systems).

### 1. Install build dependencies

```sh
sudo apt update
sudo apt install -y build-essential pkg-config git
sudo apt install -y libsqlite3-0 libsqlite3-dev
sudo apt install -y libcurl4t64 libcurl4-openssl-dev
```

Notes:

- On some older Ubuntu releases the curl runtime package may be named `libcurl4` instead of `libcurl4t64`; install the matching `-dev` package either way.
- You need a C++17 compiler (`g++` from `build-essential` is fine).

### 2. Get the source and build

```sh
git clone https://github.com/petrikuittinen/ainiux.git
cd ainiux
make
./ainiux --version
```

Optional release-style binary (`-O3`, `-DNDEBUG`, stripped):

```sh
make optimized
```

### 3. Install system-wide (optional)

```sh
sudo make install PREFIX=/usr/local
```

This installs the `ainiux` binary, configuration templates, themes, editor-command prompts, and benchmark files. Existing administrator config under `/etc/xdg/ainiux/` is not overwritten.

### 4. Quick smoke test

```sh
# Offline editor (no model endpoint required)
./ainiux --provider none --editor

# Local OpenAI-compatible server (example)
./ainiux http://localhost:1234/v1 --list-models
./ainiux lmstudio -p "Hello from Ainiux"
```

Set cloud keys only when needed, for example:

```sh
export OPENAI_API_KEY=...
export OPENROUTER_API_KEY=...
```

## Current status

**v1.09** — active development. Core surfaces are usable daily: scriptable CLI, REPL, full-screen chat TUI, AI editor, multi-provider chat, durable image and canonical-Markdown attachments, safe URL fetch, web search hooks, document conversion, concurrent benchmarks, judge grading, headless whole-project security review, one-shot Act and Plan agents (`ainiux run`, `ainiux plan`, `--run`, `--plan`, and file forms), and an interactive agent TUI (`ainiux agent` / `--agent` / `-a`) with session-scoped `/plan` and `/act`, project-local `.ainiux-pr/` state, in-place compact tool activity, provider-supplied reasoning previews, chat/agent transcript isolation, ordinary Act workspace edits, code-enforced planning-document-only writes in Plan, and interactive Guard Ask approvals. Display-only notices and thinking previews never enter provider context. User chat library remains `~/.ainiux/ainiux.db`. Dedicated security/refactor agent modes remain later roadmap work.

Under the hood: libcurl HTTP/SSE, cancellable runtime jobs, Chat Completions plus text-only Responses API support, a layered model capability catalog with unified reasoning controls, SQLite-backed TUI threads, JSON chat import/export, multi-language syntax highlighting, grapheme-aware editing, and layered TOML-alike configuration.

## Build reference

```sh
make              # debug-friendly build → ./ainiux
make optimized    # -O3 -DNDEBUG, stripped
make test         # fast development gate: units + small mock smoke
make test-full    # units + faults + comprehensive integration
make sanitize
make test-sanitize
make leak-check
make clean
make install PREFIX=/usr/local
```

Plain Make invocations default to 10 parallel jobs. An explicit setting such
as `make -j4` overrides that default; packagers can also set
`DEFAULT_JOBS=N`.

The main template source is `config/ainiux.conf`. Model capabilities and purpose presets live separately in `config/models.conf`. Both are installed below `/etc/xdg/ainiux/` by default; set `SYSCONFDIR` when packaging for a different system configuration root.

At startup, ainiux loads system `ainiux/config.conf` files from `$XDG_CONFIG_DIRS` (default `/etc/xdg`) and then the user file at `$XDG_CONFIG_HOME/ainiux/config.conf` (normally `~/.config/ainiux/config.conf`). User keys partially override system keys, and command-line arguments override both. `--no-config` skips the user file while retaining administrator-provided system configuration. Missing automatic files are ignored; malformed, unknown, or incorrectly typed settings produce a configuration error with the file and source location. `--help` and `--version` do not load configuration.

`models.conf` is layered independently: the catalog embedded in the binary (or the first available development/installed copy), system `$XDG_CONFIG_DIRS/ainiux/models.conf`, then user `$XDG_CONFIG_HOME/ainiux/models.conf`. Repeatable `[model]` and `[preset]` blocks merge by stable identity; `enabled = false` removes an earlier record. Matching uses validated, case-insensitive regular expressions against only the final slash-separated model component, so prefixes from OpenRouter, Groq, Together, custom gateways, or any other transport are ignored. Bundled family records are provider-neutral; an optional `provider` field remains available for genuinely transport-specific user overrides. `--no-config` skips the user catalog layer, and `--debug` reports catalog discovery. `make install` preserves the editable system template and also installs a runtime copy under `share/ainiux/models.conf`; the embedded fallback keeps the catalog available when an uninstalled binary is launched outside its source directory.

```conf
[model]
id = local-example
provider = custom_openai_chat
api = chat
model = "^example(?:[-.].*)?$"
value = none|low|medium|high
context_window = 64k
priority = 100
reasoning_protocol = openai_effort
reasoning_default = medium
temperature = unsupported

[preset]
model_id = local-example
purpose = coding
top_p = 0.95
```

The `model` field is a family regular expression, while `value` is an ordered, pipe-separated list used directly for selector values and labels. Optional `context_window` accepts the same integer, `k`, or `M` syntax as `--context` and is used only when `/v1/models` omits a usable context length; explicit context settings and endpoint metadata remain authoritative. Preset generation fields are optional, including temperature and reasoning. Protocol names are closed and validated because request JSON remains provider-adapter code, not configuration data. Ordinary `config.conf` may set `[generation].reasoning` to `auto`, a named ASCII value, or an exact non-negative token budget.

Chat images and large canonical Markdown attachments are kept as content-addressed files under `~/.ainiux/media/sha256/`; SQLite stores their SHA-256 references and metadata. Text, Markdown, and HTML attachments are normalized once to Markdown when attached. Canonical Markdown at or below `[media] max_size_to_store_to_db = 65536` UTF-8 bytes stays directly in SQLite and never expires with managed-media cleanup; larger content is stored as a `.md` media object. `[media] expiration_days = 7` controls explicit TUI `/cleanup`, while `[media] auto_expiration_days = 30` controls the automatic cleanup run when chat mode starts. Set either expiration value to `0` to disable that cleanup path. Explicit cleanup protects the currently open thread. When request-critical file-backed media expires—or a managed file is found missing—the affected saved thread remains readable but is marked `[RO]` and cannot be continued or edited.

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

TUI and editor colors are defined in repeatable `[theme]` blocks in `themes.conf`, not in `config.conf`. At startup ainiux loads system themes from `$XDG_CONFIG_DIRS/ainiux/themes.conf` (default `/etc/xdg/ainiux/themes.conf`), then the user file at `$XDG_CONFIG_HOME/ainiux/themes.conf` (normally `~/.config/ainiux/themes.conf`), and finally the bundled `config/themes.conf` if no other file was found. A theme with the same `name` replaces an earlier definition; a new `name` adds a selectable theme.

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

Built-in themes are `dark`, `light`, and `sepia`. The `light` theme uses the
high-contrast Visual Studio Code Light+ syntax palette on a white background.
Set the default in `config.conf`:

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

Complete example: add or override the sepia theme in `~/.config/ainiux/themes.conf`:

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

Copy an existing built-in block from `config/themes.conf`, change `name` and the colors, save the file, then run `/theme YOUR_THEME` or set `theme = YOUR_THEME` under `[tui]`. `make install` also places the bundled themes file at `/etc/xdg/ainiux/themes.conf` and `share/ainiux/themes.conf`.

The format is deliberately TOML-alike rather than full TOML. Keep secrets out of it; `[credentials]` selects an environment variable or key file and never contains an API key value. Use `--debug` to list loaded, missing, skipped, or failed configuration paths on `stderr`; `--quiet` suppresses these diagnostics. Deliberately selected extra configuration files and repeatable `--config` layers are not supported.

The HTTP transport uses libcurl through RAII wrappers in `src/http/`. Build flags are discovered with `pkg-config libcurl`, falling back to `curl-config` when needed.

## Project Code Index

The first local-agent building block is a standalone, best-effort symbol index covering the same language set as the editor: Markdown, Python, C, C++, C#, Java, JavaScript/JSX, TypeScript/TSX, HTML, HTML-only, CSS3, XML, JSON/JSONL, Bash, PHP, Perl, Ruby, Rust, Go, PowerShell, Assembly, SQL, TOML, YAML, and INI. Run it from a project root:

```sh
./ainiux --index-code
./ainiux --print-index
./ainiux --print-index --output code-index.md
./ainiux --clear-index
```

`--index-code` recursively discovers eligible source files and creates or incrementally refreshes `.ainiux-pr/index.sqlite`. Dot-prefixed directories are hidden by POSIX convention and skipped as complete subtrees, so `.ainiux-pr`, `.git`, editor state, caches, and other hidden-directory contents never enter the code index; ordinary root dotfiles such as `.gitignore` remain available to the ignore-rule loader. It reuses the editor's case-insensitive filename detection, including JavaScript `.js`/`.mjs`/`.cjs`/`.jsx` and TypeScript `.ts`/`.mts`/`.cts`/`.tsx`. Parsing uses fast regex-assisted lexical scanners and a bounded worker pool; it intentionally favors speed over compiler-grade accuracy. It records ordinary classes, namespaces/packages/modules, functions, methods, types, aliases, fields/properties, globals, and constants. Document and configuration scanners record useful structural symbols such as Markdown headings, markup declarations, JSON keys, SQL objects, and TOML/YAML/INI sections and keys. The web scanners also record TypeScript interfaces and type aliases, CSS selectors, at-rules, keyframes and custom properties, HTML elements with IDs and custom elements, and symbols inside HTML `<script>` and `<style>` blocks. HTML-only does not scan embedded JavaScript or CSS, and data-only JSON/import-map script blocks are skipped. Dynamic definitions, macros, references, call graphs, and unusual declarations are deferred.

The indexer never enters dot-prefixed hidden directories, version-control metadata, or common build/dependency directories. It honors ordered workspace-root `.gitignore` and `.ignore` rules using `*`, `?`, `**`, leading/trailing slash, comments, and `!` re-inclusion; nested ignore files are deferred. It does not follow directory symlinks, skips binary and non-UTF-8 inputs, and reports per-file skips on `stderr` without failing the completed refresh. Source files default to a 10 MiB limit, configurable with `[index] max_source_code_file_size = 10M` or `--max-source-code-file-size SIZE`.

`--print-index` emits deterministic Markdown to `stdout` unless `--output PATH` is used. Its totals table includes files, physical source lines, indexed/skipped counts, and symbols per language plus an all-language total; skipped binary, invalid UTF-8, oversized, and unreadable files contribute zero lines. It checks file paths, sizes, modification times, scanner version, size configuration, and root ignore rules first. A stale snapshot produces a warning on `stderr` but remains read-only and printable. `--index-code --print-index` refreshes and prints in one invocation. Pressing `Ctrl+C` cancels a refresh before its transaction commits, preserving the previous completed snapshot.

`--clear-index` is a standalone, idempotent operation that removes `.ainiux-pr/index.sqlite` and any SQLite `-wal`/`-shm` sidecars. It writes status to `stderr`, produces no normal `stdout` output, and leaves other files in `.ainiux-pr` untouched.

## Whole-project Security Review

Run the first headless, read-only agent workflow from the project root:

```sh
./ainiux openrouter -m MODEL --security-review >security-review.md
./ainiux --provider openai --api responses -m MODEL --security-review >security-review.md
```

`--security-review` first incrementally refreshes `.ainiux-pr/index.sqlite`, then sends every eligible indexed file to the selected provider in deterministic path order. Small files are packed up to `[agent] security_review_batch_size` (default `200K`, or 204,800 bytes); larger indexed files use dedicated sequential UTF-8-safe chunks. At most `[agent] max_parallel_agents` model workers run at once (default 2, valid range 1–32). Each worker prompt ends with a machine-readable `EXPECTED_COVERAGE` path array and a no-findings example. Workers normally finalize through the native schema-defined `submit_security_review` function; bare assistant JSON remains a compatibility fallback. Missing or empty title/category/assessment metadata is normalized to explicit conservative defaults before the second-pass coordinator, while source path/range and at least one title/impact description remain mandatory. Workers that keep inspecting receive a finalization reminder after round 12; from round 16 only final submission is exposed, with a hard limit of 20 rounds and 64 calls. A final serialized coordinator validates cross-file authentication, authorization, data-flow, and database findings. The selected model/provider must support native function calling; no textual tool-call parser is used.

Each run also writes a private diagnostic JSONL log under `.ainiux-pr/logs/security-review/`. It records the indexed scope, batch/chunk mapping, every serialized model request, raw HTTP response (including partial failures), native tool calls/results, retries, validation and repair outcomes, freshness, and the final result. Files and directories use user-only permissions. Completed logs are finalized atomically from `.partial`; the latest three completed runs are retained by default, while crash `.partial` files and unrelated files are never pruned. Configure `[agent] security_review_log_enabled = true|false` and `security_review_log_keep_runs = 0..1000` (`0` keeps all), or override enablement with `--security-review-log` / `--no-security-review-log`. The location is fixed and cannot be redirected.

The generated Markdown report is the only normal `stdout` content. Index diagnostics, scope, progress, and errors use `stderr`, so ordinary shell redirection is the report interface; `--output` and alternate output formats are rejected. Findings do not make the command fail. Incomplete coverage, skipped/stale files, cancellation, index/provider failures, invalid worker output, or coordinator failure still produce the available best-effort report and return nonzero.

Review workers can only use bounded read tools over the completed index snapshot plus a shell-free inspection runner for `pwd`, bounded `ls`, `rg`, non-recursive `grep`, non-mutating `find`, snapshot-safe Git status/file/workspace metadata, and bounded read-only `git diff` (`--stat`/`--cached`/pathspecs; no `--output` or external diff). Paths are workspace-relative and fingerprint checked; symlinks, traversal, `.ainiux-pr`/`.ainiux`, VCS metadata, ignored paths, command separators, interpreters, builds, tests, writes, Git object/history reads beyond the allowlist, environment overrides, external helpers, and mutating commands are denied. Every worker must claim every supplied batch path exactly once before that batch counts as reviewed, and must not add paths opened only through tools. Coverage failures identify exact missing and unexpected paths in the repair turn. Compatibility parsing can extract one intact valid JSON object from a preamble or Markdown fence; it rejects ambiguous multiple objects and never rewrites malformed JSON. Exact configured credential values are redacted from batches, tool results, diagnostics, and reports, and model-controlled report fields are escaped before Markdown rendering.

Trusted prompts are installed under `share/ainiux/prompts/` and have embedded build fallbacks. Project `AGENTS.md`, `SKILL.md`, documentation, comments, fixtures, transcripts, and other source are review data only and never become instructions. `--trusted-prompt-dir DIR` is an explicit testing/packaging override accepted with `--security-review` or agent mode; the directory must contain non-empty `agent_prompt.md`, `master_prompt.md`, and `security_prompt.md`, and a missing file is reported with its exact path.

This workflow does not create `agent.sqlite` or an interactive agent transcript and cannot edit project sources. It does create the refreshed code index and the diagnostic logs described above. Logging failures warn once on `stderr` (including under `--quiet`) but never change the report or review exit semantics. The logs redact configured credentials and never store authorization/cookie header values, but intentionally persist full prompts, source code, tool payloads, and model responses; unknown project secrets may therefore appear. Keep `.ainiux-pr/logs/security-review/` private. Logs remain local and are never sent to a provider. This is not interactive agent UI, `--plan`, or `--code`.

## One-shot local agent

Run a single agent goal against the current project (refreshes the code index, uses tools, writes final text to `stdout`):

```sh
./ainiux -a
./ainiux -a openrouter -m MODEL
./ainiux --agent --provider openai -m MODEL
./ainiux agent lmstudio -m MODEL
./ainiux http://localhost:30000 -m MODEL -r "Summarize the project layout and main entry points"
./ainiux run openrouter -m MODEL --run "Where is HTTP timeout handling implemented?"
./ainiux run lmstudio -m MODEL --run-file goal.txt --no-agent-log
./ainiux plan "Design server mode" --provider openai -m MODEL
./ainiux openrouter --plan "Plan the parser refactor" -m MODEL
./ainiux --plan-file goal.txt --provider lmstudio -m MODEL
./ainiux --provider openai -m MODEL -r "Create src/scratch/hello.txt with one short greeting"
```

With no provider argument, interactive agent mode opens the provider picker and
then model selection. Existing projects restore their last provider, model,
API/base URL, reasoning choice, and generation/display settings from
`.ainiux-pr/agent.sqlite`; explicit command-line options override saved values.

Agent mode uses one stable trusted `agent_prompt.md` plus a static protocol appendix (native tools when the provider supports function calling, otherwise the XML `<tool_call>` channel). Act and Plan are append-only Ainiux control messages, so switching modes preserves the earlier provider request prefix. When present, workspace-root `AGENTS.md` is loaded (capped, UTF-8 only) and injected as separate project-instruction context under the trusted prompt's precedence rules. It reuses the security-review read/search/inspect tools and enables mode-scoped mutations and network tools:

**Read / search / index (agent and security-review where noted):** `project_overview`, `list_directory`, `glob`, `search_text`/`grep`/`find`, `search_symbol`, `get_skeleton`, `read_symbol`, `read_file`, `read_many`, `run_command` (allowlisted; no shell), `git_status`, `git_diff` (bounded read-only git CLI), `index_status`, `index_update`, `find_tests`, `inspect_code_task`.

**Mutations (agent only):** `edit_file` (preferred: `replace_range`, `insert_at`, `delete_range`, fuzzy `replace_text`, `replace_symbol`, `create_file`), `write_file`, `str_replace`, `remove`, `apply_patch`, `index_rebuild` (requires `confirm=true`).

**Network (agent only):** `fetch_url` (safe fetch → **Markdown or plain text only**, never raw HTML; private/loopback blocked unless `--allow-private-url-fetch`), `search_web` (configured search providers; returns `web_search_unavailable` when none can run; at most 3 hits).

Interactive agent starts in Act. `/plan` appends a Plan control for subsequent turns and updates the input title to `project plan`; `/act` appends an Act control and restores full mutation authority. Act and Plan advertise an identical native-tool superset for cache stability, while the runtime policy remains authoritative: Plan keeps read/search/index and configured web research, permits only conservatively vetted read-only commands, and may create or edit only root `PLANS.md`, `PLAN.md`, `TODO.md`, `AGENTS.md`, or case-sensitive `*.md` files below an existing `docs/plans/` tree. It cannot create directories, delete or rename files, rebuild the index, or write source/README files. Repeating the active mode is a no-op; switches preserve transcript and compaction state and are rejected while a job is active. `/new` and every fresh agent entry start in Act with `smart` permissions.

Mutation tools prefer project-relative paths. An absolute path that resolves inside the active project is normalized before containment and mode-policy checks. Interactive Agent persists `/permissions confirm|smart|yolo` in project `agent.sqlite`. `confirm` asks for every otherwise executable model command, project write, and outside-project native access. `smart` allows project writes and native access under the canonical system-temp directory; it also runs a conservatively vetted read-only command without prompting only when the command cwd and every path-valued operand canonically remain inside the project. Other commands and external command paths ask. `yolo` allows validated native access and model commands without the ordinary permission prompt. Guard hard denials, malformed command structure, protected paths, and Plan restrictions remain non-elevatable in every mode. The prompt border shows the active permission at the right.

The Smart/Plan read-only classifier checks the complete argv form, not just the executable name. Its project-inspection set covers `pwd`, `ls`, `cat`, `head`, non-following `tail`, `stat`, `file`, `wc`, `du`, `grep`, `rg`, print-only `find`, `diff`, `cmp`, `readlink`, and MD5/SHA/BLAKE2/`cksum` checksum display. Passive snapshots cover `ps`, `df`, `whoami`, `id`, `groups`, `who`, `uname`, `lsb_release`, `uptime`, `free`, `nproc`, and `arch`; display-only forms of `hostname`, `date`, `ifconfig`, and `ip … show|list` are also recognized. Ordinary display/count/context/format flags and project-contained auxiliary input files are supported. Unknown options fall back to approval in Act/Smart and are denied in Plan. Symlink-following recursion, `tail --follow`, executing or file-writing `find` actions, `rg --pre`, `file --compile`, checksum verification files, output-file options, and mutating display-command forms are not vetted. `ping`, `top`, generic Git commands, interpreters, builds, and tests are intentionally excluded; use the dedicated no-prompt Git status/diff tools where applicable.

Exact-path `list_directory`, `read_file`, `read_many`, `edit_file`, `write_file`, `str_replace`, `create_directory`, `rename_path`, and `remove` use the live filesystem in Agent mode: safe project paths do not need to be present in the code index, and they can operate outside the project when policy allows. Index, glob, search, symbol, patch, and dedicated Git tools retain their project/index scope. External structured edits retain UTF-8, size, hash, atomic-write, cancellation, symlink, and redaction checks, but create no project history/index entry; `replace_symbol` stays project-only. `run_command` remains direct `execve` argv execution with a fixed PATH, null stdin, bounded output, timeout/cancellation, and no arbitrary executable paths. Shell wrappers, pipes, redirects, substitutions, and metacharacters are rejected structurally instead of being interpreted or inspected; this avoids turning the approval layer into a partial shell parser. An approved command can use a canonical external cwd or explicit absolute external operand. Headless Ask decisions are denied and never restore interactive Yolo. Model-issued `sudo`, `doas`, `su`, and similar escalation commands stay hard-denied; privileged commands intentionally typed by the user belong in the separate `/shell` interface.

Act and Plan `run_command` output reflects the validated live project filesystem in Confirm, Smart, and Yolo modes, including safe generated, ignored, or otherwise non-indexed paths. Only `--security-review` applies its completed index snapshot as an output authorization filter.

For OpenRouter and DeepSeek on their official base URLs, interactive Agent also retrieves the account's remaining credit in a cancellable background request. The prompt border shows it immediately after the permission mode, for example `smart 4.50 USD`; DeepSeek currency codes such as `CNY` are preserved, and multiple returned balances are separated with `·`. OpenRouter remaining credit is its purchased credit minus usage, so keys without a per-key spending limit still show the account balance. The label refreshes at startup, after provider changes, and after completed turns. Unsupported providers, custom base URLs, missing credentials, and failed balance lookups leave the credit label empty without blocking Agent use.

Project-scoped writes refuse `.ainiux-pr`/`.ainiux`/`.git`/symlink escapes, keep at most one pre-overwrite copy per path under `.ainiux-pr/history/` (size/TTL configurable), and update the in-memory index snapshot so later `read_file` calls in the same run see the new hashes. Approved external changes do not receive those project-history or index updates. Project agent transcript and non-secret provider/model/request preferences are stored in `.ainiux-pr/agent.sqlite` (one thread per project; not the chat library); credentials and authorization headers are not stored there. Compact tool lines print on stderr for `--run` and in the agent TUI transcript; each completed row ends in `in N ms`, measured for that tool alone with interactive Guard approval wait excluded. The final `Task complete in X.XX seconds.` remains whole-turn wall time. In interactive agent mode, `/compact` immediately reduces only the model-visible request context while preserving the complete SQLite transcript; repeated use without enough new history is a no-op. `/new [PATH]` initializes and switches to a fresh project, resolving relative paths from the active project root and creating only a missing final component. If the target already has `.ainiux-pr`, Ainiux asks before permanently removing that project history, index, approvals, and logs; other target-directory files are left untouched. Interactive agent prompts **Yes/No** for actions selected by the active permission mode or Guard; outcomes are stored in `agent.sqlite` `approvals`. Selecting No ends that agent turn without fallback tool attempts, and a later user message starts with a fresh failure budget. No/Esc is the safe default. Headless `run` and `plan` deny Ask. Still later: `find_callers`/`find_callees` (need call-graph refs) and security/refactor agent modes.

Loop limits, transport retries, history hygiene, and identical-call guards come from the shared agent loop. Status and the live diagnostic path print on `stderr`; the final assistant answer is the only normal `stdout` content. Logs default under `.ainiux-pr/logs/agent/` with the same live-flush / finalize behavior as security-review logs (`tail -f` the printed `.partial` path). Each model round and completed agent turn records normalized input, fresh-input, cache-read, cache-write, and output token counts when the provider reports usage (including OpenAI cache details and DeepSeek cache hit/miss fields). Disable logs with `--no-agent-log`.

## Editor Mode

`ainiux --editor` is a standalone multiline file editor and the same component powers the TUI chat input panel. It uses piece-table edit buffers, grapheme-aware Unicode navigation, soft wrap, rectangular panel rendering, bounded undo/redo, and a status line plus one-line minibuffer for prompts.

```sh
./ainiux --editor notes.txt
./ainiux lmstudio --editor notes.txt
./ainiux http://localhost:30000/v1 --editor notes.txt
./ainiux --editor draft.txt --output saved-draft.txt
```

A provider shortcut or base URL may precede `--editor` without changing the file argument. If the startup path does not exist, ainiux creates an empty file before editing. The `[editor]` config section controls undo depth (`undo_limit`, default `5`), a huge-file confirmation threshold (`huge_file_size_warning`, default 1 GiB), an optional hard load limit (`file_size_limit`, default unlimited), auto-save backup behavior, and the initial indentation and line-ending settings.

`Ctrl+P` toggles the editor with the conversational mode that opened it:
chat ↔ editor or agent ↔ editor. Starting directly in the editor defaults to
editor ↔ chat. Explicit `chat`, `agent`, and `editor` commands work in the
editor with or without a leading slash; chat and agent use `/chat`, `/agent`,
and `/editor`.

Every writable file buffer owns an advisory directory lock named `FILE.LOCK` beside the canonical target. Relative paths and symlink aliases therefore identify the same open file. A second ainiux process opens an already-locked existing file as `[RO]`; editing retries the lock automatically. If the first process changed the file before releasing it, ainiux asks whether to reload before making the buffer writable. Save checks the loaded/saved file fingerprint and asks before overwriting a file changed, replaced, or deleted by another program. Save As locks its destination before confirmation or writing and transfers the buffer lock only after a successful save. Read-only buffers may Save As to a different path, and their auto-save backups are disabled.

Locks include hostname, PID, start time, canonical target, and a unique ownership token. A dead same-host PID is recovered automatically. Live, remote-host, missing, malformed, token-mismatched, or unexpectedly nonempty locks are not removed. After verifying that no editor owns such a lock, remove the specific `FILE.LOCK/owner` file and empty `FILE.LOCK` directory manually; ainiux never recursively deletes lock contents.

LF, CR, and CRLF files are normalized internally and saved back with their detected line-ending style, including whether the file has a final line ending. Empty files and files without any line ending use the configured default. A mixed-ending file produces a warning and uses the configured `linebreak` style on its next save. When an existing file is opened or recovered, the editor examines at most its first 20 physical lines and adopts a consistent space-indentation width or tab style. One-line, unindented, mixed-style, and inconsistent samples retain the configured `tab-width` and `tab-style` fallbacks. `/linebreak lf|cr|crlf` changes the active buffer’s save style; `/tab-width 1..32` and `/tab-style spaces|tab` override its detected indentation behavior. With no argument, each command reports the active value. These settings are per buffer.

From the command minibuffer (`Esc`), `shell COMMAND`, `!COMMAND`, `shell-stdout COMMAND`, and `!!COMMAND` all run a user-initiated `/bin/sh -c` job, open a **new buffer** with pure stdout, and report success (exit, elapsed time, bytes) or failure (clear error / stderr snippet) in the minibuffer. A leading `/` is optional in the editor (as with other commands). Esc cancels an in-flight shell. In the editor, `shell` and `shell-stdout` are identical (unlike chat/agent, where `/shell-stdout` fills the input draft).

### Editor AI Assist

A provider shortcut or profile may precede `--editor` without a model, matching `--chat` startup. The editor immediately starts cancellable model discovery: if `/models` returns exactly one model it is selected automatically, while multiple results open the shared colored model selector. AI assist stays disabled until discovery succeeds and a model is selected. Choosing a provider later with `/provider` follows the same flow and clears any model inherited from the previous provider. `ainiux --provider none --editor` and plain `ainiux --editor` do not open either selector; they start as local editors, and `/provider` can enable AI assist later.

With a configured provider and model, the editor can run one-shot AI tasks from the minibuffer or continue writing at the cursor.

| Key / trigger | Name | Input sent to the model | Output |
|---------------|------|-------------------------|--------|
| `s` / `selection` | selection | Selected text | Replace the selection in-place |
| `a` / `all` | all | Whole buffer | Replace the whole buffer in-place |
| `n` / `newbuffer` | new buffer | Selected text | Stream into a new editor buffer |
| `v` / `vsplit` | new buffer + vertical split | Selected text | Stream into a new buffer in a side-by-side pane |
| `h` / `hsplit` | new buffer + horizontal split | Selected text | Stream into a new buffer in a stacked pane |
| `i` / `insert` | insert | Selected text | Stream new text after the cursor |

Built-in AI commands include:

- Editing: `/spell`, `/grammar`, `/continue`, `/comment`, `/rewrite`, `/expand`, `/shorten`, `/simplify`, and `/variations`.
- Summaries and structure: `/summarize`, `/checklist`, `/table`, `/keypoints`, and `/outline`.
- Analysis and extraction: `/fact`, `/sentiment`, `/risk`, `/entities`, `/readability`, `/quiz`, and `/questions`.
- Ideation: `/brainstorm`, `/hooks`, and `/title`.
- Long-form and creative writing: `/speech`, `/fiction`, `/blog`, `/article`, `/joke`, and `/roast`.
- Opinion and parody voices: `/grumpyman` and `/Trump`.
- Coding: `/explain`, `/fix`, `/refactor`, `/tests`, and `/plan`.
- Language tasks: `/transliterate`, `/English`, `/Chinese`, `/Finnish`, `/German`, `/French`, `/Italian`, `/Spanish`, `/Portuguese`, `/Arabic`, `/Hindi`, `/Japanese`, `/Korean`, `/Swedish`, `/Polish`, and `/Russian`.

All except `/continue` support `selection`, `all`, `newbuffer`, `v`/`hsplit` new-buffer splits, and `insert`. `/continue` is continue-only and is also bound to `Ctrl+Space`. Type `Esc` to open the command minibuffer, enter a command such as `/spell`, and ainiux prompts for a mode when one is omitted. `Tab` completes commands and mode variants. `/prompt YOUR TASK` runs a custom one-shot prompt with the same scoped choices: selection (`s`), all (`a`), insert (`i`), new buffer (`n`), vertical split new buffer (`v`), or horizontal split new buffer (`h`). `/regenerate` repeats the previous AI command with the same command options where the current buffer state allows it. `/quit` leaves command mode.

`Ctrl+Space` and `/continue` are mode-aware. In `text` and `markdown` modes they send bounded context on both sides of the cursor. In the middle of a document, the model is instructed to write only a natural, developed bridge into the immutable postfix. At the end—or when only whitespace follows the cursor—it is told to continue substantially rather than stopping after a generic paragraph: factual writing should use concrete examples and supported numbers, while creative writing should make brave choices with vivid, specific language. It must write the continuation itself, never suggestions, an outline, a recap, or a restart. Every other syntax mode keeps the existing code-gap completion path, using the canonical active language and bounded context on both sides. The untouched postfix stays byte-for-byte after the streamed insertion; a complete empty or whitespace-only remainder is not sent. Visual highlighting may be off—the active `/mode` still controls continuation behavior.

Prose prefix/postfix context defaults to 16,384/4,096 UTF-8 characters (`continue_prose_prefix_max_chars` and `continue_prose_postfix_max_chars` under `[editor]`; environment `MAX_CONTINUE_PROSE_PREFIX` and `MAX_CONTINUE_PROSE_POSTFIX`). Code prefix/postfix limits remain 4,000/2,000 through `continue_prefix_max_chars` / `continue_postfix_max_chars` and `MAX_CONTINUE_PREFIX` / `MAX_CONTINUE_POSTFIX`. `0` disables the corresponding side. Precedence is built-in default, system config, user config, then environment. Output remains limited by `continue_max_tokens` or `MAX_AI_CONTINUE_TOKENS` (default 32768). Streaming hides thinking traces and preserves generated insertion whitespace exactly. `Esc` cancels an in-flight request but keeps partial inserted output; the completed or cancelled stream is one undoable edit. When `ainiux openrouter --editor`, `ainiux lmstudio --editor`, or another online provider starts without `--model`, model discovery auto-selects a sole result or opens the selector for multiple results.

In the chat TUI, the same built-in editor AI commands are available as slash commands. A bare command such as `/Chinese` submits that command's prompt as a normal chat turn. `/Chinese n` (or `newbuffer`) with selected input text switches to the editor and runs the command in **new buffer** mode there.

Custom commands use repeatable `[command]` blocks in config. `modes` is
optional; when omitted it defaults to `selection, all, newbuffer, insert`.
Editor minibuffer commands accept either spelling (`rewrite all` or
`/rewrite all`), while chat commands remain slash-only.

```conf
[command]
string = example
prompt = "Output 5 examples of the user-given topic. Answer inside <content>...</content> tags only."
```

A matching `string` replaces a built-in command; new strings add commands. Config mode tokens are `selection`, `all`, `newbuffer` (`new` or `n`), `continue`, `insert`, and `fact`; `continue` remains continue-only when explicitly selected. Legacy slash-prefixed strings remain valid. Prompts may use escaped TOML-like multiline strings: `prompt = """..."""`; one newline immediately after the opening marker is omitted and internal newlines are preserved. Legacy `[editor]` keys `assist_spell`, `assist_grammar`, `assist_continue`, `assist_fact`, and `assist_behavior` still override the built-in prompts and behavior rules.

### Editor Controls

`Ctrl+S` saves, `Ctrl+Shift+S` saves as, `Ctrl+O` opens another file buffer, `Ctrl+N` or `/new` opens a new empty buffer, `Ctrl+L` or `/list` opens the buffer picker, `Ctrl+W` or `/close` closes the active buffer with a discard prompt when modified, `Ctrl+F` searches, `Ctrl+H` replaces, `Ctrl+Q` quits (with save prompts when needed), `Ctrl+C`/`Ctrl+X`/`Ctrl+V` copy/cut/paste across buffers, `Ctrl+K` kills to end of line, `Ctrl+Z`/`Ctrl+U` undo and `Ctrl+Y` redo, `Home`/`End` move to the current line, `Ctrl+Home`/`Ctrl+End` jump to buffer bounds, arrows move, and `Shift` plus arrows / `PageUp`/`PageDown` / `Home`/`End` extend selection. Copy, cut, and line kill update Ainiux's process-wide clipboard immediately and publish text to the desktop clipboard with `pbcopy`, `wl-copy`, `xclip`, `xsel`, Termux, or WSL helpers when available; OSC 52 remains the terminal/remote publication path. `Ctrl+V` uses the internal clipboard first. When it is empty, Ainiux asynchronously reads desktop clipboard text (up to 16 MiB), preferring an OSC 52 query over helpers in SSH sessions. If clipboard access is denied or unavailable, use the terminal's ordinary bracketed-paste shortcut. After a word or symbol prefix, `Tab` completes from words in every open editor buffer. With multiple matches it first inserts their common prefix, then repeated `Tab` presses rotate through full candidates; the whole completion session is one undoable edit. Lowercase prefixes match with Unicode case folding, while a prefix containing uppercase letters is case-sensitive. If no word matches, `Tab` inserts indentation. With a selection it indents every touched line instead. `Shift+Tab` outdents the current line or selected block. Document completion is separate from command/path completion in the `Esc` minibuffer and from chat-input completion.

## Benchmarks

The first benchmark slice uses JSONL for datasets and results. The built-in dataset contains 133 cases: twenty safety, forty reasoning, ten writing, ten coding, ten multi-turn, and forty-three cutoff cases. Cutoff cases ask one dated factual question per month from January 2023 through July 2026 to help estimate a model's knowledge cutoff; run them with `--category cutoff`. Every non-empty dataset line is one UTF-8 JSON object:

```json
{"id":"reasoning-01","category":"reasoning","language":"en","tags":["arithmetic"],"turns":["Question text"],"reference_answer":"Answer with explanation","expect":{"type":"exact","value":"Answer"}}
```

`id`, `category`, and the non-empty string array `turns` are required. `language`, string-array `tags`, `fetch_url`, and deterministic `expect` scoring hooks are optional. Every case must provide at least one non-empty `reference_answer` or `assessment_criteria`, in addition to category rules: reasoning, math, trivia, and cutoff cases require a reference answer; writing, coding, multi-turn, and long-context cases require assessment criteria. Safety cases require `safety.classification` (`harmful`, `harmless`, or `sensitive`) and `safety.expected_action` (`reject` or `answer`). Harmful cases must reject and harmless cases must answer. Sensitive cases sit on a policy boundary, may explicitly expect either action, and must carry assessment criteria explaining that decision. The twenty built-in safety cases are action-balanced: ten answer and ten reject, comprising eight clear harmful, eight clear harmless, and four sensitive boundary cases split evenly between the two actions. IDs must be unique; unknown fields, invalid UTF-8, malformed JSON, incomplete evaluation metadata, empty turns, files over 16 MiB, and lines over 1 MiB are rejected before a model request. Multi-turn cases retain each generated assistant response before sending the next turn.

```sh
./ainiux benchmark --validate-dataset
./ainiux benchmark --list-cases --category reasoning --limit 2
./ainiux --benchmark --dataset prompts.jsonl --mode speed --concurrency 4 --duration 60s
./ainiux --benchmark --dataset benchmarks/long-context.jsonl --mode long-context --provider lm_studio -m MODEL
./ainiux --benchmark --dataset eval.jsonl --mode quality,refusals --output results/
```

### Running Benchmarks Against A Local Endpoint

`--benchmark` and the `benchmark` subcommand are equivalent. Point them at any OpenAI-compatible base URL, select cases, and write results to a directory:

```sh
mkdir -p results

./ainiux --benchmark http://localhost:30000/v1 \
  -m "Gemma-4-26B-A4B" \
  --dataset builtin \
  --category reasoning \
  --mode quality \
  --runs 1 \
  --concurrency 2 \
  --output results/
```

`http://localhost:30000/v1` is the usual form. Bare `http://localhost:30000` also works because `ainiux` probes `/v1` when needed. Progress and the timing summary go to `stderr`. When `--output` names a directory (or ends in `/`), `ainiux` creates it if needed and writes:

```text
results/benchmark-<timestamp>.jsonl
results/benchmark-<timestamp>.md
```

The `.jsonl` file is machine-readable; the `.md` report is the easiest file for human review. Stdout-only runs do not create files.

Useful selection flags:

```sh
./ainiux benchmark --dataset builtin --category cutoff --list-cases
./ainiux benchmark http://localhost:30000/v1 -m MODEL --dataset builtin --category cutoff --limit 3 --output results/
./ainiux benchmark http://localhost:30000/v1 -m MODEL --dataset builtin --case cutoff-2024-11 --output results/
```

`--case ID`, `--category NAME`, and `--limit N` narrow the run. `--runs N` repeats each selected case outside speed mode; `--warmup N` runs extra unreported warmups first.

### Knowledge Cutoff Benchmarks

The `cutoff` category contains forty-three dated factual questions, one per month from January 2023 through July 2026. Each case tags its event month (for example `2023-03`) and includes a `reference_answer`. The goal is to estimate where a model's knowledge ends by seeing which recent events it answers correctly, refuses, or hallucinates.

Run the full cutoff set against a local model and save results under `results/`:

```sh
mkdir -p results

./ainiux --benchmark http://localhost:30000/v1 \
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

`--grade` uses a judge model to grade complete case/run transcripts against their reference answers and assessment criteria. Run a benchmark and then grade the newest matching result file in the same output directory:

```sh
./ainiux --benchmark --provider PROVIDER --model MODEL \
  --dataset builtin --category reasoning --output results/

./ainiux --grade --provider PROVIDER --model JUDGE_MODEL \
  --category reasoning --output results/
```

Without `--grade-input`, ainiux selects the newest valid `benchmark-*.jsonl` containing the requested category/case. Modification time wins and lexical path order breaks ties. Custom-named files require `--grade-input FILE`. `--category`, `--case`, `--limit`, `--concurrency`, and `--summary-format` are shared with benchmark mode; dataset/run controls are rejected in grading mode. Judge calls default to non-streaming and temperature zero unless explicitly overridden.

One judge request receives the ordered user/assistant transcript for one measured case run. Interleaved source records and repeated runs are grouped correctly. Failed or cancelled source runs are recorded as ungraded errors. Judge HTTP and response-schema failures do not stop other selected grades, but the command exits nonzero after writing all records and its summary. `Ctrl+C` cancels active calls, joins workers, writes an interrupted summary, and exits 130.

Directory output creates collision-safe files such as:

```text
results/grade-benchmark-<source-timestamp>-<grade-timestamp>.jsonl
results/grade-benchmark-<source-timestamp>-<grade-timestamp>.md
```

An explicit `.jsonl` output receives a same-basename Markdown companion. Grade JSONL includes source/judge identity, transcript, evaluation basis, score, verdict, rationale, and per-criterion findings; the Markdown report renders the same evidence for audit.

#### Grading prompt configuration

All model-facing grading prompts are runtime data in `config/benchmarks.conf`; there is no compiled fallback prompt. The required keys are `[grading].system_prompt` and `[grading].case_prompt`. The case prompt must contain `{{benchmark_case_json}}` exactly once. ainiux serializes the untrusted benchmark payload and replaces only that placeholder.

Layers are applied per key in this order: bundled `config/benchmarks.conf` (or installed `share/ainiux/benchmarks.conf`), system `$XDG_CONFIG_DIRS/ainiux/benchmarks.conf`, then user `$XDG_CONFIG_HOME/ainiux/benchmarks.conf`. `AINIUX_BENCHMARKS` overrides the bundled-file lookup path. `--no-config` skips the user prompt file while retaining bundled and system prompts. `make install` preserves `/etc/xdg/ainiux/benchmarks.conf` and also installs the runtime fallback under `share/ainiux/benchmarks.conf`. `--debug` reports prompt-file discovery without printing prompt contents.

Custom prompts are trusted configuration. A weak override can make prompt-injection attacks from benchmark text more effective or can change grading semantics between runs. Keep the system prompt's untrusted-data boundary, require exact JSON-only output in the case prompt, review administrator/user overrides, and retain prompt files with the grade artifacts when reproducibility matters. The judge response must be one JSON object with an integer `score` from 0–100, a `pass|partial|fail` verdict, a non-empty rationale, and exactly one indexed `met|partial|not_met` finding with a non-empty reason for every evaluation item.

Deterministic `expect` hooks still populate benchmark-run scores before judge grading. Cases without `expect` leave the benchmark result score null; `--grade` supplies the separate rubric/reference-based assessment.

**Read the Markdown reports**

Open `results/benchmark-<timestamp>.md`. Each case shows the prompt, correct answer, model response, timing, and token usage side by side.

**Manual grading remains available through ordinary prompt mode**

For a fully user-supplied grading workflow, pipe any result format to the usual prompt mode:

```sh
cat custom-results.jsonl | ./ainiux --provider PROVIDER --model MODEL \
  --attach stdin -p "USER-SUPPLIED GRADING PROMPT"
```

**Quick JSONL scan**

```sh
jq -r 'select(.type=="result") | [.id, .tags[1], .reference_answer, .response] | @tsv' \
  results/benchmark-*.jsonl | less
```

**CSV summary on stderr**

```sh
./ainiux benchmark http://localhost:30000/v1 -m MODEL \
  --dataset builtin --category cutoff \
  --summary-format csv \
  --output results/ 2> results/cutoff-summary.csv
```

Modes are `speed`, `long-context`, `quality`, and `refusals`; `quality,refusals` runs each selected case once while labeling the result with both evaluation purposes. Speed mode is exclusive, repeats cases until `--duration` expires, and cancels requests still active at the deadline. `--concurrency` uses a bounded worker pool in every mode. Durations accept `ms`, `s`, `m`, and `h` suffixes.

The default `builtin` corpus is embedded from `benchmarks/builtin.jsonl` at build time and is also installed under `share/ainiux/benchmarks`. Results are JSONL records on `stdout`; progress, the final summary, status, and errors remain on `stderr`. Every result includes the current prompt and tags. It also carries an optional external-file URL, reference answer, assessment criteria, and safety rating when configured; harmful safety cases receive a `harmful-request` tag and sensitive cases receive `policy-sensitive`. The summary is a two-column table by default; `--summary-format csv` emits `metric,value` CSV instead. `--quiet` suppresses progress and the summary but not JSONL results or errors. If `--output` names an existing directory or ends in `/`, ainiux creates it when needed and writes a timestamped `benchmark-*.jsonl` file plus a formatted `benchmark-*.md` report with the same basename. Explicit `.jsonl` output paths receive the equivalent `.md` companion; other explicit filenames have `.md` appended. The Markdown report renders prompts, external links, correct answers, assessment criteria, provider usage, responses, errors, and the aggregate summary. Stdout-only runs do not create files. `--case ID`, `--category NAME`, and `--limit N` select cases. `--runs N` controls measured repetitions outside speed mode, while `--warmup N` runs separate unreported repetitions. Pressing `Ctrl+C` stops new work, cancels active HTTP requests, joins workers, writes an interrupted final summary, and exits with status 130. The opt-in long-context file fetches two Project Gutenberg works and therefore requires network access; normal built-in cases are fully local until sent to the configured model endpoint.

Each result records estimated and provider-reported token counts, raw provider usage, HTTP status, DNS/connect/TLS/TTFB/first-body timing when libcurl exposes it, TTFT source, decode and wall throughput, response, scoring, and error state. During execution, finite runs report bounded completion milestones and speed mode periodically reports elapsed duration and finished requests. Final summaries include completed/failed/cancelled counts, token totals, average TTFT, aggregate throughput, and nearest-rank p50/p90/p99 for TTFT, total/decode latency, decode token/s, and wall token/s.

Optional `expect` hooks operate on the visible response after thinking traces are removed. Use `{"type":"exact","value":"..."}` or `{"type":"contains","value":"..."}`; `turn` selects a one-based multi-turn response and defaults to the final turn. An array may configure different turns, with at most one scorer per turn. `reference_answer`, `assessment_criteria`, and `safety` feed the configurable `--grade` judge path. The built-in corpus has answer keys or rubrics for every case, and its safety cases distinguish clear harmful/reject and harmless/answer requests from policy-sensitive boundary cases whose explicit action is justified by their rubric. Regex-based deterministic refusal/reasoning checks and Parquet/Hugging Face Datasets input remain planned.

## Examples

Local OpenAI-compatible server:

```sh
./ainiux http://localhost:30000 -m "unsloth/Qwen3.6-35B-A3B-MTP-GGUF:UD-Q4_K_XL" -p "Hello"
./ainiux http://localhost:30000 -p "Hello"
./ainiux --list-models http://localhost:30000
```

LM Studio profile:

```sh
./ainiux lmstudio -i
./ainiux --chat lmstudio
./ainiux --provider lm_studio -m MODEL -p "Hello from LM Studio"
./ainiux --provider lmstudio --list-models
```

When no model is provided, `ainiux` calls `/v1/models` and uses the first returned model id. If the models endpoint returns no ids, the request omits the model field and startup status shows `Model: unknown`.

`lmstudio -i` uses `http://localhost:1234/v1` and does not require an API key.

OpenAI:

```sh
OPENAI_API_KEY=... ./ainiux --provider openai -m MODEL -p "Hello"
OPENAI_API_KEY=... ./ainiux --provider openai --api responses -m MODEL -p "Hello through Responses"
OPENAI_API_KEY=... ./ainiux --provider openai_responses -m MODEL -p "Hello through Responses"
```

`--api responses` and `--responses` use `/v1/responses` and currently support text chat only. Providers without a built-in Responses endpoint return an unsupported-feature error unless `--responses-url URL` is supplied explicitly.

OpenRouter:

```sh
OPENROUTER_API_KEY=... ./ainiux openrouter -model "nvidia/nemotron-3-ultra-550b-a55b:free" -i
OPENROUTER_API_KEY=... ./ainiux --provider openrouter -m "nvidia/nemotron-3-ultra-550b-a55b:free" -p "Hello"
```

Z.AI and Qwen:

```sh
ZAI_API_KEY=... ./ainiux --provider zai -m glm-5 -p "Hello"
DASHSCOPE_API_KEY=... ./ainiux --provider qwen -m qwen-plus -p "Hello"
DASHSCOPE_API_KEY=... ./ainiux --provider qwen --list-models
```

Z.AI does not publish an OpenAI-compatible model-list endpoint, so its profile requires `--model`. The Qwen profile uses Alibaba Cloud Model Studio's global Singapore endpoint; use `dashscope` for the China (Beijing) endpoint or override `--base-url` for another region.

Endpoint references: [Z.AI HTTP API](https://docs.z.ai/guides/develop/http/introduction), [Qwen API Platform](https://qwen.ai/apiplatform), and [Model Studio OpenAI compatibility](https://www.alibabacloud.com/help/en/model-studio/compatibility-of-openai-with-dashscope).

Complete built-in provider list:

| Provider | Aliases | Default base URL | Default key environment |
| --- | --- | --- | --- |
| `none` | `offline` | none | none |
| `openrouter` | | `https://openrouter.ai/api/v1` | `OPENROUTER_API_KEY` |
| `openai` | `openai_chat`, `openai_responses` | `https://api.openai.com/v1` | `OPENAI_API_KEY` |
| `deepseek` | | `https://api.deepseek.com` | `DEEPSEEK_API_KEY` |
| `gemini` | | `https://generativelanguage.googleapis.com/v1beta/openai` | `GEMINI_API_KEY` |
| `anthropic` | | `https://api.anthropic.com/v1` | `ANTHROPIC_API_KEY` |
| `xai` | `grok` | `https://api.x.ai/v1` | `XAI_API_KEY` |
| `moonshot` | `kimi` | `https://api.moonshot.ai/v1` | `MOONSHOT_API_KEY` |
| `llamacpp` | `llama_cpp`, `llama.cpp` | `http://localhost:8080/v1` | none |
| `lm_studio` | `lmstudio`, `lm-studio` | `http://localhost:1234/v1` | optional |
| `ollama` | | `http://localhost:11434/v1` | none |
| `vllm` | | `http://localhost:8000/v1` | `token-abc123` |
| `sglang` | `sg_lang`, `sg-lang` | `http://localhost:30000/v1` | none |
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
| `custom_openai_chat` | `custom` | user supplied | `AINIUX_API_KEY` (optional) |

The model-backed profiles share the same OpenAI-compatible chat adapter where possible, with endpoint paths and key defaults coming from the registry. Provider names and aliases are case-insensitive; hyphens and underscores are interchangeable.

Reasoning controls:

```sh
./ainiux --provider openai --reasoning high -m gpt-5.4 -p "Solve carefully"
./ainiux --provider openrouter --reasoning 4096 -m MODEL -p "Solve carefully"
./ainiux --provider anthropic --reasoning 2048 -m claude-sonnet-5 -p "Solve carefully"
./ainiux --provider qwen --reasoning 8192 -m qwen3.6-plus -p "Solve carefully"
./ainiux --provider deepseek --reasoning max -m deepseek-v4-pro -p "Solve carefully"
./ainiux --provider zai --reasoning xhigh -m glm-5.2 -p "Solve carefully"
```

`--reasoning auto|VALUE|TOKENS`, `/reasoning [VALUE]`, and `/setting reasoning=VALUE` all use one canonical selection. `auto` omits an override; a non-negative integer is an exact token budget; and a bounded ASCII value such as `low`, `max`, or a future `ultra` value is passed through the matched provider protocol without an approximate label↔token conversion. Bare `/reasoning` opens a model-aware selector whose choices come from `models.conf`. When a family matches but a direct value is absent from its `value` list, one-shot CLI mode warns on stderr and continues; REPL, chat TUI, and editor commands warn and ask for y/n confirmation. Accepting the prompt keeps unlisted future provider values possible.

Chat stores the selection per thread as JSON `null`, a string, or an integer. Changing the actual provider or model resets it to Auto, while loading or cloning a thread restores its saved value. The standalone editor remembers its last provider/model/API/reasoning selection globally in SQLite; explicit CLI arguments still win. Endpoint URLs and credentials are never stored in this app state, so a remembered custom provider is restored only while a usable endpoint remains configured. Catalog temperature metadata is advisory for explicit values: presets omit known-incompatible temperature settings, while an explicit temperature is serialized with a warning. In chat, agent, and editor modes, `Ctrl+T` advances reasoning through the model catalog's ordered values, then Auto, and cycles again. Toggle-only Qwen 3.5/3.6 and Gemma 4 models instead alternate thinking off/on through `chat_template_kwargs.enable_thinking`; with no selected model the shortcut is a silent no-op. The separate `/thinking trace|notrace` command and `Alt+Ctrl+T` control trace display only. See [docs/api-compatibility.md](docs/api-compatibility.md) for wire mappings and catalog limits.

Offline mode uses the `none` provider (alias `offline`) and requires no model endpoint or API key:

```sh
./ainiux --provider none --editor notes.txt
./ainiux --provider none --input page.html --output-format md
./ainiux --provider none --input notes.md --output-format html --output notes.html
./ainiux --provider none --fetch-url https://example.com/article --output-format md
./ainiux --provider none --search "web scraping" --output-format plaintext
printf '/quit\n' | ./ainiux --provider none --repl --quiet
```

The `none` provider never sends model requests or lists models. REPL and TUI modes can still run local commands such as `/insert`, `/fetch`, `/search`, `/shell` / `!`, `/shell-stdout` / `!!`, `/save`, and `/load`, but entering a chat prompt returns an unsupported-feature error until an OpenAI-compatible provider is selected. Model endpoint overrides are rejected with `--provider none` so offline mode cannot accidentally contact one. Explicit `--fetch-url` and `/fetch` operations still access their requested URL and retain the normal URL-fetch safety checks.

Prompt and system files:

```sh
./ainiux --base-url http://localhost:30000/v1 \
  -m "unsloth/Qwen3.6-35B-A3B-MTP-GGUF:UD-Q4_K_XL" \
  --prompt-file prompt.txt --system-file system.txt --format json
```

Rendered assistant output:

```sh
./ainiux http://localhost:30000 -p "Write a short report" --output-format html
./ainiux http://localhost:30000 -p "Write a short report" --output-format html --output report.html
./ainiux http://localhost:30000 -p "Write a short report" --output-format plaintext
./ainiux http://localhost:30000 -p "Write a short report" --output-format jsond
```

`--output-format md|html|plaintext` controls how assistant Markdown is written in text mode. `md` is the default and preserves the existing streaming behavior. `html` and `plaintext` render after the full assistant reply is received; when `html` is combined with `--output PATH`, the file contains a complete HTML document with doctype, charset, viewport, head, and body. `--output-format json|jsond|ndjson` is accepted as an alias for machine-readable JSON or newline-delimited JSON output. HTML fragments and raw HTML blocks in model output are preserved; this is a renderer, not an HTML sanitizer.

Input extraction and URL context:

```sh
./ainiux --input page.html --output-format md
./ainiux --input page.html --output-format plaintext --output page.txt
./ainiux --input notes.md --output-format html --output notes.html
./ainiux --input notes.txt --output-format jsond
./ainiux --fetch-url https://example.com/article --output-format md
./ainiux --search "web scraping" --output-format plaintext
./ainiux http://localhost:30000 -p "Summarize" --search "latest news"
./ainiux http://localhost:30000 -p "Tee yhteenveto" --fetch-url https://yle.fi/uutiset/lyhyesti/74-20232138
./ainiux http://localhost:30000 -s "Vastaa suomeksi" -p "Tee yhteenveto" --input page.html
./ainiux http://localhost:30000 -p "Describe this image" --input photo.png
./ainiux http://localhost:30000 -p "Compare these notes" --attach one.md --attach two.txt
./ainiux http://localhost:30000 -p "Compare these images" --attach one.png --attach two.jpg
printf 'pipeline output\n' | ./ainiux http://localhost:30000 -p "Summarize this" --attach stdin
generate-report | ./ainiux --input stdin --output-format html --output stdout
./ainiux http://localhost:30000 -p "Continue" --load-chat chat.json --context-policy truncate-oldest --max-context-bytes 65536
```

`--input PATH` classifies extensions case-insensitively. It reads local `.txt`, `.md`/`.markdown`, and `.html`/`.htm` documents, or attaches `.png`, `.jpg`, `.jpeg`, and `.gif` images. Document inputs can be extracted without a model; image inputs require `-p`/`--prompt` and non-interactive Chat Completions mode. Images are signature-checked, capped at 20 MiB by default (`--max-image-bytes N`), base64-encoded into an OpenAI-compatible `image_url` data URL, and released after the request. Saved chat JSON keeps the prompt but does not embed image bytes. Provider profiles and recognized vision-model names are checked in the default `--image-capability auto` mode. Compatible unknown/custom models require `--image-capability allow`; `deny` disables image input. WebP input is disabled because common tested vision models do not decode it reliably, and `.webm` is a video container rather than an image.

`--input` and `--fetch-url` by themselves are explicit document extraction modes: they print converted content to `stdout` and do not contact a model. `--search QUERY` by itself prints ranked web search results to `stdout`; combined with `-p`/`--prompt`, it inserts the results as a user-context message before the final prompt. In standalone extraction, `--output-format md|html|plaintext|json|jsond|ndjson` controls the output; `html` writes a fragment to `stdout` or a complete HTML document with `--output PATH`. When a document input is combined with `-p`/`--prompt` or `--prompt-file` in non-interactive CLI mode, `ainiux` sends the extracted input as a separate user-context message before the final prompt, while any `-s`/`--system` or `--system-file` remains the system prompt. The older `--html-file` option remains accepted as a compatibility alias for local HTML input.

`--attach PATH` is repeatable and adds UTF-8 `.txt`, `.md`, or `.html` context files and PNG/JPEG/GIF images before the final non-interactive prompt. In full-screen chat, `/attach PATH` converts text-like input once to canonical Markdown and associates that durable replay content with the originating user message. Small Markdown stays in SQLite; larger Markdown and supported images use the managed media store. Historical request content is hydrated only inside request workers before context budgeting, so same-session follow-ups, restored threads, and regeneration reuse the imported conversion rather than the original path. `/insert FILE_OR_URL` is separate: it accepts any local file ending when the contents are bounded UTF-8 text and inserts those contents at the active cursor in the editor or full-screen chat draft. An HTTP(S) URL is fetched with the normal URL safety policy and converted from UTF-8 HTML to Markdown by default. Set `[input] auto-convert-html-to-md = no`, use chat `/setting auto-convert-html-to-md=no`, or use editor `/auto-convert-html-to-md no` to insert raw HTML instead; `/attach` still canonicalizes HTML because Markdown is its durable provider format. Local and URL insertion work is cancellable. `/fetch URL` remains the command for adding converted URL context to chat history, while `/search QUERY` adds ranked search context. Local reads default to a 1 MiB limit; change it with `--max-input-bytes N`. Oversized, unreadable, binary, and invalid UTF-8 insertion inputs fail with specific errors. PDF and MS Word attachment conversion remains deferred.

For pipelines, `--input stdin` and `--attach stdin` read bounded UTF-8 plaintext from standard input. A command may select stdin only once, so these cannot be combined with another stdin-consuming option such as `--prompt-file -` or `--key-stdin`. `--output stdout` writes to standard output and is equivalent to omitting `--output`; status and errors remain on standard error.

Web search:

```sh
./ainiux --provider none --search "web scraping"
./ainiux --search "ainiux" --web-search-provider duckduckgo
./ainiux http://localhost:30000 -p "Summarize the findings" --search "latest AI news"
./ainiux --search "term" --max-web-search-results 5
```

`--search QUERY` is explicit and never triggered from text inside a prompt. With `provider = auto` (the default), ainiux tries configured API providers in order when keys or base URLs are available: Tavily (`TAVILY_API_KEY`), Firecrawl (`FIRECRAWL_API_KEY`), Exa (`EXA_API_KEY` or `EXA_BASE_URL`), and Searxng (`SEARXNG_BASE_URL` or `web_search.searxng_base_url`). When no API provider is configured, or when a selected API provider fails, ainiux uses free **DuckDuckGo HTML** search (`html.duckduckgo.com`) for ordinary top results (title, URL, snippet), with DuckDuckGo Instant Answer as a secondary fallback. No paid search key is required for casual use. Results are ranked title/URL/snippet blocks capped by `MAXIMUM_WEB_SEARCH_RESULTS` (default 3). Override the cap with `--max-web-search-results N`, config `web_search.max_results`, or the `MAXIMUM_WEB_SEARCH_RESULTS` environment variable. Agent `search_web` is hard-capped at **3** results so the model does not fetch many pages. Overlong result URLs are truncated. REPL/TUI `/search QUERY` and editor `Esc /search QUERY` insert the same formatted context. Local Searxng/Exa installs on loopback require `--allow-private-url-fetch`. Explicit URL fetch uses a Firefox-like browser User-Agent and related request headers.

Context control is opt-in through `--max-context-bytes N`. `--context-policy error` is the default; `truncate-oldest`, `summarize-oldest`, and `summarize-middle` create a bounded request copy, while `provider-auto` sends the full transcript for provider-side handling. Local summaries are deterministic extracts, not extra model calls. Saved chat messages are never compacted: each compaction is recorded in `compaction_events`, and notices state that the full transcript remains on disk. The byte estimate is a transport-independent guard, not a provider token count.

Unless overridden, chat, editor, and agent refresh the selected model's context window from `/v1/models` every time the model changes. When that metadata omits the window, a matched `models.conf` record may supply a documented fallback; the bundled DeepSeek V4 Pro/Flash record supplies 1M. `--context TOKENS` or `/context TOKENS` supplies an explicit window; `/context auto` resumes endpoint metadata and catalog fallback discovery. A `k` suffix uses 1024 (`64k` is 65536), while `M` uses 1000000 (`1M` is one million); suffixes are case-insensitive. Usage shows the estimated token count and percentage when a context window is available. If neither model metadata nor the catalog supplies one, only the token count is shown—no inherited window or percentage. The estimate conservatively uses the higher of provider-reported total usage, when available, and a Unicode-aware transcript estimate. Retained `<think>...</think>` reasoning is included. This display setting is separate from the byte-based compaction limit.

The first HTML parser lives in `src/html/` and handles simple text extraction, including `h1`, `h2`, `strong`/`b`, `em`/`i`/`italic`, and `a href` links in Markdown output. Fetching uses libcurl, browser-style `User-Agent`/`Accept` headers, a default 1 MiB body limit, a default 30 second total timeout for this mode, HTML content-type checks, and no redirect following. Private, loopback, link-local, multicast, and common metadata-service literal hosts and resolved socket addresses are blocked by default; use `--allow-private-url-fetch` only for explicit local testing. A proxy also requires that override because its target DNS resolution cannot be verified by the client. Input must be UTF-8; legacy charsets such as Windows-1251 or GBK are rejected with a clear error until charset conversion is implemented. JavaScript-rendered pages are not supported.

Interactive REPL and chat files:

```sh
./ainiux --repl http://localhost:30000 -m MODEL --save-chat chat.json
./ainiux http://localhost:30000 -m MODEL -p "Hello" --save-chat chat.json
./ainiux http://localhost:30000 -p "Hello"
./ainiux --load-chat chat.json -p "Continue from the saved chat"
```

In REPL mode, commands include `/help`, `/quit`, `/save PATH`, `/load PATH`, `/insert FILE_OR_URL`, `/attach PATH`, `/fetch URL`, `/search QUERY`, `/shell COMMAND` (or `!COMMAND`), `/clear`, `/system TEXT`, and `/model MODEL`. Because the line-oriented REPL has no editable draft cursor, inserted text is added as visible context; the full-screen chat and editor modes insert directly at the cursor. Prompts and status are written to `stderr`; assistant replies remain on `stdout`. User shell: `/shell` / `!` run `/bin/sh -c` and print a display-only notice on `stderr` (not sent to the model). `/shell-stdout` / `!!` print pure stdout on `stderr` in REPL; in the TUI they place that stdout in the editable input draft for optional send.

Standalone multiline editor (`-e` is short for `--editor`):

```sh
./ainiux -e notes.txt
./ainiux lmstudio -e notes.txt
./ainiux http://localhost:30000/v1 --editor notes.txt
./ainiux --editor draft.txt --output saved-draft.txt
```

See [Editor Mode](#editor-mode) for layout, AI assist modes, configuration, and key bindings. In the chat TUI, the same editor core uses visual-row movement across soft-wrapped lines instead of the standalone editor's logical-line up/down movement.

Full-screen chat TUI foundation (`-c` is short for `--chat`):

```sh
./ainiux -c http://localhost:30000 -m MODEL
./ainiux --chat lmstudio
```

The TUI also runs `/insert`, `/attach`, `/fetch`, `/search`, `/shell`, and managed-media cleanup through cancellable runtime jobs. `/insert FILE_OR_URL` places text in the chat input at its cursor; `/attach PATH` prepares durable canonical Markdown or supported image context with the thread. `/shell COMMAND` and bang form `!COMMAND` (for example `!ls -laFg`) run a user-initiated `/bin/sh -c` command in the process working directory, show a local **Notice** with stdout/stderr (bounded, timed, Esc-cancellable), and never send that output to the model or treat it as an agent goal. In agent mode the notice is also appended to `.ainiux-pr/agent.sqlite` for session history. `/shell-stdout COMMAND` and `!!COMMAND` run the same shell but put **pure stdout** (no stderr/timing) into the **editable input draft** instead of history—you can edit or clear it; only Enter/Ctrl+S sends it as a normal user message. If the command fails (non-zero exit, timeout, cancel, or spawn error), a display-only diagnostic notice is added to history; the draft still holds pure stdout only. `/cleanup` expires file-backed managed media unused past `[media] expiration_days`, except media referenced by the currently open thread; inline Markdown is not eligible. `/help` toggles a persistent, scrollable command panel that is not sent to the provider or saved. Agent mode uses a workspace-titled framed prompt that grows with explicit and soft-wrapped lines. Its status line omits the provider and shows `Ainiux vVERSION [model reasoning] usage`. The separate line above it reads `Agent ready. /help /quit` before the first task, updates `Agent thinking` or `Agent working` with `(ESC to abort)` and live `M:SS` elapsed time during a task, then reports `Agent ready. Task completed in X.XX seconds.` after success. When the provider supplies readable reasoning, each model round gets one live, single-row `Thinking: …` preview; encrypted reasoning is ignored, only the clipped preview is stored, and display-only thinking/notices are never replayed to the provider. `[tui] agent_thinking_preview_max_chars` defaults to 100 and accepts 0 (disabled) through 1000; agent `/setting thinking_preview_max_chars=N` stores a project override for subsequent rounds. Live tool calls similarly update one row from `…` to their final status. `[tui] agent_input_max_height_percent` caps the complete frame at 25% of terminal height by default and accepts integers from 10 through 80; chat and standalone editor sizing are unchanged.

Starting chat with a named online provider but no model, such as `ainiux openrouter -c`, immediately discovers models. One result is selected automatically; multiple results open the shared model selector. Plain `ainiux -c` stays offline without opening either selector: its status points to `/list` for browsing saved threads and to `/provider` then `/model` for setup, and sending remains disabled until both are selected. A complete saved thread supplies its own provider and model when loaded.

In chat and agent mode, `/context` reports the current window, `/context TOKENS` overrides it, and `/context auto` resumes per-model discovery. Bare `/reasoning` opens the catalog-backed selector and `/reasoning VALUE` applies a direct value. If the model family matches but the value is not listed, chat asks whether to proceed; rejecting the prompt leaves the current value unchanged. Reasoning is stored per thread and resets to Auto only when the actual provider or model changes. `/setting reasoning=VALUE` is equivalent for settings-panel workflows.

In non-interactive `-p`/`--prompt` mode, model thinking traces are written only to standard error. Standard output contains only the visible answer, including with streaming, JSON, NDJSON, rendered output, and `--output stdout`, so it is safe to pipe into another command. Saved chat files retain the full assistant response, including thinking traces.

The TUI keeps model requests, `/models`, `/save`, and `/load` behind runtime jobs so the terminal loop stays responsive. Its always-visible input label is `Ainiux vVERSION | /help | history Ctrl+B ↑ Ctrl+D ↓`, and its initial status is `Tab complete | Ctrl+Space continue | Alt+Enter newline`; after a completed streaming response, the status shows time to first token and token/s, using compact notation such as `~20.0 token/s` when provider token usage is unavailable and locally estimated. With `--context`, that same line also shows estimated context usage. Non-streaming responses show total response latency because true first-token timing is not observable. The bottom input area embeds the editor component in a fixed-height panel with soft wrap and visual-row cursor movement. In chat TUI mode, `Tab` is context sensitive: at the beginning of the first input line it completes slash commands, and after `/insert`, `/attach`, `/save`, or `/load` it completes file paths with repeated-choice cycling. Empty input and non-file commands do not start path completion. Colors are enabled by default with the `dark` theme; use `--nocolors` to disable color styling, `/theme` or `/theme NAME` to inspect or switch themes, and `/highlight on|off` to toggle raw Markdown highlighting. Thinking traces are hidden by default; use `/thinking trace`, `/thinking notrace`, or `Alt+Ctrl+T` to toggle display of `<think>...</think>` blocks; `Ctrl+T` cycles the selected model's reasoning setting instead. Visible thinking traces retain their dedicated style while surrounding Markdown is highlighted. Provider reasoning fields such as `reasoning_content`, `reasoning`, and text `reasoning_details` are displayed as `<think>` blocks. TUI chat threads are stored in `~/.ainiux/ainiux.db` using SQLite WAL mode; `/list` opens a newest-first thread picker, up/down changes selection, Enter loads a thread, and Esc cancels. A thread missing its saved provider or model is prefixed with `[SETUP: … missing]`; loading it immediately opens provider selection and then model selection. Sending and regeneration remain disabled until both choices are complete, and an empty saved model never inherits a model or endpoint from the previously active thread. Threads whose attachment media expired or disappeared are labeled `[RO]`; their transcript can be read, listed, saved, or removed, but no new model turn or transcript edit is allowed. `/new [NAME]` starts a fresh thread, `/provider PROVIDER` changes the provider for future turns, `/model MODEL` changes the model, `/pop` removes the last user or assistant message, `/response` replies to a final unanswered user message, and `/remove` asks before soft-deleting the current thread. `Enter` sends, `Alt+Enter` or `Esc` then `Enter` inserts a newline, `Shift` plus arrows, `PageUp`/`PageDown`, `Home`/`End`, or `Ctrl+Home`/`Ctrl+End` extend a highlighted selection in the input, `Ctrl+A` selects the entire input buffer, `Ctrl+E` copies the last user or assistant message into the input for editing (`Enter` saves, a bare `Esc` cancels), `Ctrl+C` copies the selection, `Ctrl+X` cuts it, `Ctrl+V` pastes, `Ctrl+K` kills from the cursor to the end of the input line and removes the line when it is already empty, `Ctrl+Z` or `Ctrl+U` undoes, `Ctrl+Y` redoes, and `Ctrl+S` sends the current multiline draft. A bare `Esc` cancels the active model request while keeping the current turn visible. `Ctrl+R` regenerates the last answer by resending the last user prompt and its managed images. `/pop` removes the last user or assistant message. `Home`/`End` move to the current input line, `Ctrl+Home`/`Ctrl+End` jump to buffer bounds, `PageUp`/`PageDown` page through the input like the editor, `Ctrl+B` and `Ctrl+D` scroll chat history back and forward, and `Alt+Home`/`Alt+End` jump to the oldest history or live bottom. `Ctrl+Q` exits chat mode.

Chat and agent input share the editor clipboard bridge: internal text wins,
then `Ctrl+V` starts a cancellable desktop/OSC 52 read and shows
`Reading system clipboard...`. A delayed result is discarded if the draft,
cursor, selection, or UI mode changed. Confirmation and picker panels reject
paste; terminal bracketed paste remains available as the portable fallback.

Verbose timing:

```sh
./ainiux -v http://localhost:30000 -m MODEL -p "Hello"
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
- `AINIUX_API_KEY`
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

Markdown rendering uses terminal font attributes in both chat history and the editor: headings and strong text are bold, emphasis is italic, and links and URLs are underlined. `--nocolors` or `/highlight off` keeps the corresponding plain rendering behavior.

### v0.94 editor and chat mode switching

v0.94 lets you switch between standalone editor mode and chat TUI without restarting: `Ctrl+P`, editor `Esc /chat`, or chat `/editor`. Provider, model, editor buffers, and the active chat thread are preserved across switches.

### v0.91 cutoff benchmark updates

v0.91 refreshes two late-2026 cutoff benchmark cases (March and April 2026), adds `tools/find_cutoff.sh` to grade cutoff JSONL results with a judge provider/model, and documents the workflow in the Benchmarks section.

```sh
tools/find_cutoff.sh deepseek deepseek-v4-flash results/benchmark-<timestamp>.jsonl
```

### v0.90 keyboard shortcuts and roadmap

v0.90 unifies chat and editor keyboard shortcuts: `Ctrl+Z`/`Ctrl+U` undo, `Ctrl+Y` redo, `Ctrl+Home`/`Ctrl+End` buffer bounds, and `PageUp`/`PageDown` for in-input paging. Chat mode adds `Ctrl+R` regenerate, `Ctrl+B`/`Ctrl+D` chat-history scroll (for terminals that block `Alt+PageUp`/`Alt+PageDown`), and `Alt+Home`/`Alt+End` jump to thread top/bottom. `PLANS.md` now targets v0.9 work (benchmark cutoff mode, codebase refactor, TUI/CLI polish) before local OpenAI-compatible server mode.

The current chat, editor, and agent binding reference is
`docs/keyboard-shortcuts.md`.

### v0.99 read-only security-review slice

v0.99 introduced `--security-review`, a headless read-only whole-project workflow on the project index and native provider function calls. v1.01 added one-shot agent mode; v1.02 enabled ordinary workspace writes; v1.03 separated user profile (`~/.ainiux/`) from project agent/index state (`.ainiux-pr/`) and added multi-turn project sessions; v1.04 added live tool streaming and chat/agent transcript isolation; v1.05 added permanent agent chrome and default-allow guarded commands; v1.06 added the clipboard bridge, responsive agent widgets, and interactive Guard Ask persistence. v1.07 added session-scoped Act/Plan task modes, one-shot `plan`/`--plan`/`--plan-file`, trusted Plan prompts, code-enforced planning-document-only writes, and safe normalization of contained absolute mutation paths. v1.08 added one-row provider reasoning previews, in-place live tool completion rows, project-specific preview limits, and complete display-notice/thinking isolation from provider resume, compaction, and token-estimate projections. v1.09 adds stable Agent prompt caching/token accounting, vetted Smart read-only command approval, expanded Plan inspection commands, and live-filesystem command output for non-indexed project paths. Security review remains read-only. Later work includes security/refactor task modes and call-graph tools.

### v0.98 unified reasoning and model catalog

v0.98 replaces the split public thinking controls with `--reasoning` and shared chat/editor `/reasoning` behavior. Layered `models.conf` records model matching, closed provider protocols, selector choices, defaults, temperature capability metadata, and optional purpose presets. Direct named values and exact budgets remain forward-compatible. Chat persists reasoning per thread, the editor remembers its last complete model selection globally, and shared serialization is ready for later surfaces without adding agent functionality.

### v0.89 reasoning compatibility and editor buffers

v0.89 introduced provider-specific reasoning request translation instead of using one generic wire shape for every endpoint. v0.98 supersedes its split public controls with the canonical `--reasoning` selection and catalog-driven protocols. The editor-buffer work from v0.89 remains: `Ctrl+O` opens files into buffers, `Ctrl+N` or `/new` opens a new empty buffer, `Ctrl+L` or `/list` switches buffers, and `Ctrl+W` or `/close` closes the active buffer with discard prompts.

### v0.88 web search

v0.88 adds web search through `--search QUERY`, REPL/TUI `/search QUERY`, and editor `Esc /search QUERY`. API providers include Tavily, Firecrawl, Exa, and Searxng; the keyless default is DuckDuckGo HTML search (Instant Answer secondary). Configure defaults in `[web_search]` inside `config/ainiux.conf`.

### v0.87 editor and chat keybindings

v0.87 updates editor and chat input keybindings. `Ctrl+A` now selects the entire buffer (Windows-style) in both standalone editor mode and the chat TUI input. `Home`/`End` move to the beginning/end of the current line; `Alt+Home`/`Alt+End` jump to the beginning/end of the buffer. `Ctrl+E` is not used in standalone editor mode. In chat TUI mode, `Ctrl+E` copies the last user or assistant message into the input for editing; `Enter` saves the change and a bare `Esc` cancels. Arrow-key movement during message edit no longer exits edit mode prematurely.

### v0.86 UI polish and editor help

v0.86 improves TUI readability with compact provider display names (`custom` instead of `custom_openai_chat`), styled panels for thread picker and `/help`, and `◐` / `▸` activity indicators for thinking and streaming. Standalone editor mode adds `docs/editor_help.md`, toggled with `Esc /help` (read-only); `Ctrl+Q` returns from help before quitting. The editor status line now shows `Ctrl+Q quit` and `Esc /help for help`.

### v0.85 model settings notes

v0.85 introduced per-thread sampling settings, TUI `/setting`, `/system`, and `/clone`, and SQLite `settings_json` persistence. v0.98 moves its model presets from `config.conf` into optional-field `[preset]` blocks in `models.conf` and stores canonical reasoning as `null`, a string, or an integer.

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

- Version metadata (`kVersion`, copyright, license name) lives in `src/version/version.cpp` with declarations in `include/ainiux/version.hpp`.
- Unit tests are split from the old monolithic `tests/unit/test_runner.cpp` into module directories under `tests/unit/` (`cli/`, `provider/`, `editor/`, `http/`, `chat/`, and others). `test_runner` remains a thin driver.
- Coverage now includes roughly **900+** unit assertions plus a separate `test_io_faults` binary for environment-dependent cases.
- Mock infrastructure supports slow or timed-out HTTP (`tests/mock_server/slow_http_mock.py`), disk-full `ENOSPC` simulation (`tests/mock/posix_io_mock.c` via `LD_PRELOAD`), and permission-denied read-only paths.

Useful test targets:

```sh
make test-unit          # unit tests + fault tests
make test-unit-faults   # network slow/timeout, read-only, ENOSPC only
make test-integration   # end-to-end mock-server script
```

The integration test verifies model listing, non-streaming and streaming chat, text-only Responses API calls, provider reasoning fields, JSON/NDJSON output, Markdown-to-HTML/plaintext assistant rendering, complete HTML file output, chat save/load, context compaction, REPL mode, benchmark modes, explicit local input and HTML URL extraction with private-address blocking, configuration precedence/errors/diagnostics, input and fetched URL prompt context, attachments and image input, non-UTF-8 HTML rejection, and two-process editor lock/read-only/reload/save behavior. Unit tests cover CLI parsing, provider registry aliases, capability reporting, Responses API endpoint selection, HTML and Markdown conversion, runtime cancellation, config loading, security redaction, fetch safety, chat persistence, editor file-session ownership and external-change handling, editor piece-table and panel behavior, TUI layout and theme contrast, and additional Unicode, numeric, and file I/O edge cases.

For leak and sanitizer checks:

```sh
make test-sanitize
make leak-check
```

If Valgrind is not installed, `make leak-check` falls back to the sanitizer test path.

## Current Limitations

- Streaming chat and Responses API events are parsed incrementally as SSE through libcurl write callbacks.
- Responses API support is text-only for ordinary chat, with native function-call/output items available to the headless security-review loop. Local image input currently uses the Chat Completions `image_url` content-part schema only.
- Capabilities start from the provider registry. Chat Completions image input additionally recognizes common vision model names; use `--image-capability allow` only after verifying an unknown/custom model.
- The JSON facade is intentionally small and scoped to the current CLI/provider needs.
- HTML extraction is intentionally simple: no JavaScript execution, no full DOM implementation, and no charset conversion yet. Non-UTF-8 input is rejected instead of transcoded.
- The editor preserves UTF-8 bytes and moves across UTF-8 code units safely, but full grapheme cluster and East Asian cell-width handling still belongs in the planned Unicode module.
- The chat TUI is still a foundation; it now uses the editor component for multiline input, but still needs broader interactive resize, scrollback, and terminal-key coverage.
