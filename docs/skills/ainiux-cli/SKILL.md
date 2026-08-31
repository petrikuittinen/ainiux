---
name: ainiux-cli
description: >
  Invoke the Ainiux CLI from a shell for one-shot chat, agent run/plan,
  image generation, document conversion, fetch, and search. Use when another
  agent should call `ainiux` via bash instead of a TUI. Triggers: ainiux,
  ainiux run, ainiux plan, ainiux image, script-friendly OpenAI-compatible CLI.
---

# Call Ainiux from a shell

This skill is for **other agents** (and humans driving them) that need to run
Ainiux as a subprocess. Full option lists live in `ainiux --help` and
[CLI and scripting](../cli.md). Do not copy flags from memory; if a flag is
uncertain, run `ainiux --help` or read that guide.

Ainiux is a C++17 CLI/TUI client for OpenAI-compatible APIs. It is **not** an
OpenAI-compatible server. There is no `ainiux server` in the current release.

## When to use this

Use the one-shot CLI when the caller should own the conversation and only needs
a result on stdout.

Do **not** start interactive surfaces from a foreign agent:

| Flag / verb | What it does | From a subprocess |
| --- | --- | --- |
| `-p` / default chat | One-shot model reply | Yes |
| `run` / `-r` | One-shot Act agent (workspace tools) | Yes |
| `plan` / `--plan` | One-shot Plan agent | Yes |
| `image` / `--image` | One-shot image file | Yes |
| `--input` / `--fetch-url` / `--search` without `-p` | Convert / fetch / search | Yes |
| `--list-models` | Print provider models | Yes |
| `-i` / `--repl` | Line-oriented REPL | No |
| `-c` / `--chat` | Full-screen chat TUI | No |
| `-e` / `--editor`, `-d` / `--dired` | Full-screen editor | No |
| `-a` / `--agent` | Interactive agent TUI | No |

Headless Guard **Ask is denied**. `run` / `plan` will not pause for `y`/`n`.
Do not expect a TTY approval dialog.

## I/O contract

- **stdout** — model text, conversion output, image path (or raw bytes with
  `--output stdout`), JSON/NDJSON when requested.
- **stderr** — status, warnings, progress, errors. Never parse stderr as the
  answer.
- **stdin** — only when you pass `--input stdin` / `--output stdout` for
  conversion, or `--key-stdin` for a key.

Prefer `--quiet` unless you need progress. Prefer `--no-stream` when the caller
must wait for a complete reply (scripts, command substitution).

Working directory is the workspace. Agent modes write project state under
`.ainiux-pr/` in that tree. The user chat library `~/.ainiux/ainiux.db` is
separate and is not used by `run` / `plan`.

## Credentials

Never pass `-k` / `--key` (argv is visible to other local users).

Set a provider env var, or `AINIUX_API_KEY`, or `--key-file PATH` /
`--key-stdin` / `--key-env NAME`. Typical names: `OPENAI_API_KEY`,
`OPENROUTER_API_KEY`, `LMSTUDIO_API_KEY`, `GEMINI_API_KEY`. Image-only hosts
use `REPLICATE_API_KEY` / `REPLICATE_API_TOKEN` or `FAL_API_KEY` / `FAL_KEY`
and **not** `AINIUX_API_KEY`.

## Pick a mode

1. **Need workspace tools** (read/edit/run in the current directory) → `ainiux run` (Act) or `ainiux plan` (planning documents only).
2. **Need one image file** → `ainiux image`.
3. **Need HTML/Markdown/text conversion, URL fetch, or search without a model** → `--input` / `--fetch-url` / `--search` and no `-p`.
4. **Otherwise** → one-shot chat with `-p` or `--prompt-file`.

Provider is a positional profile or URL (`lmstudio`, `openai`, `deepseek`,
`http://127.0.0.1:1234/v1`, …) or `--provider NAME`. Pass `-m MODEL` when the
profile has more than one model.

## Recipes

One-shot chat:

```sh
ainiux lmstudio -p "Explain RAII in three sentences." --no-stream --quiet
ainiux openai -m MODEL --prompt-file prompt.txt --format json --no-stream
```

`--format text` (default) prints the reply. `--format json` prints one object.
`--format ndjson` / `jsonl` prints events. `--output-format md|html|plaintext`
renders assistant Markdown; that is not the same as `--format`.

Act / Plan (final answer on stdout; metrics on stderr unless `--quiet`):

```sh
ainiux lmstudio -m MODEL -r "add focused tests for the parser"
ainiux plan "design the retry policy" --provider openai -m MODEL
```

Image (stdout is the saved path unless `--output stdout`):

```sh
ainiux image -p "a quiet terminal at night" --size 1536x1024 --output night.png
```

Catalog, size, and `--attach` rules: [CLI image generation](../cli.md#image-generation).

Conversion / fetch / search:

```sh
ainiux --input page.html --output-format md
ainiux --fetch-url https://example.com --output-format md
ainiux --search "portable C++ terminal UI" --output-format json
printf 'plain text' | ainiux --input stdin --output stdout
```

List models:

```sh
ainiux --provider lmstudio --list-models
```

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Success |
| 2 | Bad arguments or URL |
| 3 | DNS, connect, TLS, or timeout |
| 4 | HTTP/auth/rate-limit/parse/provider schema |
| 5 | File or config |
| 6 | Unsupported feature or internal |
| 130 | Cancelled |

Treat nonzero as failure. The error line on stderr includes an `ErrorCode` name
and a next step when one exists.

## Attachments and safety

`--attach` is bounded text or PNG/JPEG/GIF for capable Chat Completions models.
`--input` on an image is the chat vision path, not `ainiux image`. PDF and DOCX
are rejected.

`--fetch-url` and `--search` are explicit. A URL inside `-p` is not fetched.
Private/loopback fetch needs `--allow-private-url-fetch`.

`run` / `plan` may mutate the current workspace. They do not get a y/n prompt
in this headless path; destructive Guard Ask is denied. Prefer `plan` when the
caller only wants a document. Do not point `run` at a directory you do not
intend to change.

## Windows

Same stdout/stderr and exit codes. Full-screen modes need Windows Terminal or
modern conhost, not mintty. Details: [Native Windows](../windows.md).

## Further reading

- [CLI and scripting](../cli.md) — flags, context policy, image catalog
- [Agent workflows](../agent.md) — Act/Plan, Guard, `.ainiux-pr/`
- [MCP servers](../mcp.md) — `--add-mcp` / `--list-mcp` (management is
  CLI-only; tools load on the next `run` / `plan` / `-a`)
- [Security](../security.md) — keys, fetch, agent boundaries
- [Documentation index](../README.md)
