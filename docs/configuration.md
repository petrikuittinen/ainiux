# Configuration

Ainiux configuration is a small TOML-like format, not full TOML. Unknown keys, invalid types, and malformed files fail with a source location. User-facing booleans are `on` and `off`.

## Layering

Ainiux loads installed defaults from the first existing `share/ainiux/` copy it
finds, then the user files below `$XDG_CONFIG_HOME/ainiux/` (normally
`~/.config/ainiux/`), and finally command-line options. Later values override
earlier values by key. Share lookup order is: an in-tree development
`config/` file when present, then `share/ainiux/` beside the executable, then `$XDG_DATA_HOME/ainiux/` or
`~/.local/share/ainiux/` (for `install.sh --user`), then
`/usr/local/share/ainiux/`, then `/usr/share/ainiux/`, and finally the
build-time embedded catalog when no file exists. Ainiux deliberately has no
`/etc/xdg` system layer; this keeps upgrades from leaving stale administrator
copies of experimental defaults in the active configuration path. Older installs
that still have `/etc/xdg/ainiux` can remove it; `scripts/install.sh` and
`scripts/uninstall.sh` clean that directory when present. `--no-config`
skips the user files. `--help` and `--version` do not load configuration.
`--debug` prints which configuration paths were loaded, missing, or skipped.

On Windows, `HOME` is initialized from `USERPROFILE` when absent, so the same
`$HOME/.config/ainiux` and `$HOME/.ainiux` layout applies. Portable packages also
look for bundled defaults under `share/ainiux/` beside `ainiux.exe` before the
system-prefix fallbacks.

Bundled templates live in `config/` and are installed by `make install`:

| File | Purpose |
| --- | --- |
| `config.conf` | Provider, endpoint, generation, context, network, credentials, input, agent, media, editor, fetch/search, and TUI defaults |
| `themes.conf` | Named terminal themes and syntax colors |
| `editor-commands.conf` | Built-in and custom editor AI prompts |
| `benchmarks.conf` | Judge grading instructions |
| `models.conf` | Model capabilities, reasoning choices, context metadata, and purpose presets |

The bundled documents are installed beside the binary's other shared data. User
copies can override or extend them; see the comments in the bundled templates for
record syntax.

## A minimal user configuration

```conf
config_version = 1
provider = lm_studio
model =

[generation]
stream = on
reasoning = auto

[tui]
theme = dark
color_mode = auto
highlight = on
thinking_traces = off
```

An empty model lets the interactive selector use `/models`. Explicit CLI settings remain authoritative.

## Credentials

Keep secret values out of configuration. Set `key_env` to an environment variable name or `key_file` to a protected path:

```conf
[credentials]
key_env = OPENAI_API_KEY
key_file =
```

Provider profiles already know their normal variables and share `AINIUX_API_KEY` as a fallback where applicable. LM Studio accepts both `LMSTUDIO_API_KEY` and `LM_STUDIO_API_KEY`; DeepInfra also accepts `DEEPINFRA_TOKEN`; Qwen accepts `QWEN_API_KEY` or `DASHSCOPE_API_KEY`. The full table is in the [README](../README.md#provider-profiles-and-credentials).

At invocation time, `--key-env`, `--key-file`, and `--key-stdin` provide generic alternatives. Avoid storing keys in `-k` or headers on the command line because local process listings may expose arguments.

## Major settings

- `[endpoint]` overrides base, chat, model-list, and Responses URLs.
- `[generation]` controls streaming and optional sampling/reasoning defaults.
- `[context]` supplies a window, request byte budget, and compaction policy.
- `[network]` controls connect/request timeouts, proxy, and TLS verification.
- `[input]` bounds text and image input and controls HTML-to-Markdown insertion.
- `[agent]` controls logs, backups, automatic compaction, strategy, optional command-output display, and the LLM HTTP response body cap (`max_response_bytes`, default `32M`; `0` is unlimited). The universal derived compaction threshold is 75% when `compact_limit` is unset.
- `[media]` controls SQLite versus file-backed attachment size and cleanup ages.
- `[editor]` controls undo, file size warnings, auto-save, indentation, line endings, alignment width, and AI continuation limits.
- `[url_fetch]` controls byte limits and private-address permission.
- `[web_search]` controls result count, provider, key-variable names, and optional endpoints.
- `[tui]` controls colors, theme, color wire format (`color_mode`), highlighting, thinking display, agent input height, reasoning-preview body length (`agent_thinking_preview_max_chars`, default `120`, not counting the `💭 ` prefix), when the opening thinking row freezes if it has not already filled that budget (`agent_thinking_idle_preview_seconds`, default `30`; `0` freezes the opening clip as soon as it is complete), and how often the agent context chrome refreshes an in-flight reasoning token estimate during long thinks (`agent_thinking_token_refresh_seconds`, default `1`; `0` disables). Long thinks keep at most those two rows: the frozen opening clip and a live tail of the last ~max_chars of the think, frozen with the same `💭 ` prefix when reasoning ends.

## Themes

Themes are repeatable `[theme]` records in `themes.conf`. The built-ins are `dark`, `light`, and `sepia`. Each complete custom record defines semantic body, status, panel, activity, and optional syntax colors. A later record with the same name replaces an earlier one.

Use `/theme` to inspect themes, `/theme NAME` to select one, and `/theme off` to disable color styling. CLI `--theme NAME` selects a palette at startup; `--theme off` and `--nocolors` disable color styling (same as `/theme off`). Configuration can also start without palette styling via `[tui] colors = off` or `[tui] theme = off`.

### Color mode (truecolor / 256 / 16)

Theme palettes are stored as RGB. How those colors are sent to the terminal is controlled by `color_mode` / `--color-mode`:

| Value | Emission |
| --- | --- |
| `auto` (default) | `COLORTERM=truecolor`/`24bit` → 24-bit; common `TERM` values (`*256color*`, `xterm*`, …) → 256-color; otherwise 24-bit |
| `truecolor` | 24-bit SGR with **colon** subparameters (`38:2:R:G:B`) |
| `256` | xterm 256-color indexes (`38;5;N`) |
| `16` | classic 16 ANSI colors |

OpenSSH usually does **not** forward `COLORTERM`, so remote sessions often resolve `auto` to 256-color. That avoids a class of broken truecolor paths (for example some Windows Terminal + PowerShell + SSH setups) where semicolon truecolor parameters were misread as classic SGR codes—producing pure red status bars and unreadable dark blue text for the default dark theme.

If colors look wrong over SSH from Windows Terminal, try `--color-mode 256` or set `[tui] color_mode = 256`. Force full RGB with `--color-mode truecolor` when the terminal supports it.

## Model catalog

`models.conf` contains repeatable `[model]` and `[preset]` records. Model matching uses validated case-insensitive regular expressions against the final slash-separated model component. Records can describe API, context window, reasoning protocol and choices, temperature support, and priority. Purpose presets supply optional generation fields.

Endpoint metadata and explicit CLI values outrank catalog fallbacks. Protocol names are closed because request JSON stays in provider adapter code rather than configuration.

## Custom editor commands

Editor AI commands use repeatable `[command]` blocks in `editor-commands.conf` or supported configuration layers. A command identifies its invocation string, applicable modes, and prompt. It can override a built-in command by stable identity or add a new one without recompiling.

The command minibuffer opens with `Ctrl+E`, `Esc`, or `Alt+X`; `Tab` completes commands. AI commands require a provider and model. Local layout and cleanup commands work with provider `none`. See [Editor help](editor_help.md) for invocation, scopes, and output destinations.

## Web search

`provider = auto` tries configured API providers and keyless fallbacks according to the implementation. Supported names are `tavily`, `firecrawl`, `exa`, `searxng`, and `duckduckgo`.

```conf
[web_search]
max_results = 3
provider = auto
tavily_key_env = TAVILY_API_KEY
firecrawl_key_env = FIRECRAWL_API_KEY
exa_key_env = EXA_API_KEY
exa_base_url =
searxng_base_url =
```

Environment endpoint alternatives are `EXA_BASE_URL` and `SEARXNG_BASE_URL`. DuckDuckGo HTML and its Instant Answer fallback require no key. Search is separate from provider credentials and URL-fetch permission.

Related documentation: [documentation index](README.md), [CLI and scripting](cli.md), [API compatibility](api-compatibility.md), [benchmarks](benchmarks.md), [security](security.md).
