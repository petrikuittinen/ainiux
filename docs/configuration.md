# Configuration

Ainiux configuration is a small TOML-like format, not full TOML. Unknown keys, invalid types, and malformed files fail with a source location. User-facing booleans are `on` and `off`.

## Layering

For `config.conf`, Ainiux loads:

1. system files below each `$XDG_CONFIG_DIRS/ainiux/` entry in reverse directory order (default `/etc/xdg/ainiux/config.conf`);
2. the user file at `$XDG_CONFIG_HOME/ainiux/config.conf` (normally `~/.config/ainiux/config.conf`);
3. command-line options.

Later values override earlier values by key. `--no-config` skips the user file only; administrator system configuration remains active. `--help` and `--version` do not load configuration.

Bundled templates live in `config/` and are installed by `make install`:

| File | Purpose |
| --- | --- |
| `config.conf` | Provider, endpoint, generation, context, network, credentials, input, agent, media, editor, fetch/search, and TUI defaults |
| `themes.conf` | Named terminal themes and syntax colors |
| `editor-commands.conf` | Built-in and custom editor AI prompts |
| `benchmarks.conf` | Judge grading instructions |
| `models.conf` | Model capabilities, reasoning choices, context metadata, and purpose presets |

The four specialized documents have their own system and user layers. See the comments in the bundled templates for record syntax.

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
- `[agent]` controls logs, backups, automatic compaction, strategy, and optional command-output display. The universal derived compaction threshold is 75% when `compact_limit` is unset.
- `[media]` controls SQLite versus file-backed attachment size and cleanup ages.
- `[editor]` controls undo, file size warnings, auto-save, indentation, line endings, alignment width, and AI continuation limits.
- `[url_fetch]` controls byte limits and private-address permission.
- `[web_search]` controls result count, provider, key-variable names, and optional endpoints.
- `[tui]` controls colors, theme, highlighting, thinking display, agent input height, and reasoning-preview length.

## Themes

Themes are repeatable `[theme]` records in `themes.conf`. The built-ins are `dark`, `light`, and `sepia`. Each complete custom record defines semantic body, status, panel, activity, and optional syntax colors. A later record with the same name replaces an earlier one.

Use `/theme` to inspect themes, `/theme NAME` to select one, and `/theme off` to disable color styling. `--nocolors`, `[tui] colors = off`, and `[tui] theme = off` also start without palette styling.

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
