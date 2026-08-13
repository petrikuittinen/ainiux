# MCP support (agent modes)

Ainiux can use tools from [Model Context Protocol](https://modelcontextprotocol.io) (MCP) servers in **agent modes only**:

| Mode | Entry |
| --- | --- |
| Interactive agent | `-a` / `--agent` / `ainiux agent` |
| One-shot Act | `-r` / `--run` / `ainiux run` |
| One-shot Plan | `plan` / `--plan` / `--plan-file` |

**Not loaded in:** ordinary `--chat`, standalone `--editor`, REPL, or `--security-review`.

MCP does not replace native workspace tools (`read`, `edit`, `run`, …). It adds **external** tools from servers you install.

---

## Quick start

### 1. Install a server (CLI)

Install does **not** need a model API key. It writes the user registry and may probe connectivity.

```sh
# Public catalog (HTTP)
ainiux --add-mcp catalog --mcp-url https://awesome-mcp.tools/mcp

# List
ainiux --list-mcp
ainiux --list-mcp --format json
```

### 2. Use it from agent or one-shot run

```sh
ainiux lmstudio -m MODEL -a
# or
ainiux deepseek -m MODEL -r "Search the catalog for Django-related MCP servers and summarize 3 results."
```

Enabled servers are connected when the agent session **prepares**. Tools appear to the model as:

```text
mcp__<server_name>__<tool_name>
```

Example: installed name `catalog`, tool `search_servers` → `mcp__catalog__search_servers`.

### 3. Local mock (development / tests)

```sh
# Terminal 1
python3 tests/mock_server/mcp_mock.py --host 127.0.0.1 --port 8765 --mode both

# Terminal 2
ainiux --add-mcp mock --mcp-url http://127.0.0.1:8765/mcp --mcp-allow-private
ainiux -r "Call mcp__mock__echo with text hello and report the result." -m MODEL --provider ...
```

Mock tools: `echo`, `add`, `slow`, `fail`. Modes: `legacy`, `stateless`, `both`. Also `--stdio` for process transport.

---

## Registry

Installed servers live in a **user-private** file (not under the project):

```text
~/.ainiux/mcp/registry.json
```

- Restrictive permissions; atomic writes.
- Schema includes name, transport, URL or command/args, env, headers, enable flag, timeouts, optional protocol hint, and last negotiated dialect.
- There is **no project-scoped** MCP registry in this release.

Tests may pass `--mcp-registry PATH` to use a temporary registry file.

---

## CLI management

These flags are exclusive of agent/chat/editor modes (same idea as `--list-models`).

| Flag | Purpose |
| --- | --- |
| `--list-mcp` | List installed servers (human table, or `--format json`) |
| `--add-mcp NAME` / `--install-mcp NAME` | Install or update a server |
| `--remove-mcp NAME` | Remove a server |
| `--enable-mcp NAME` | Enable (tools load on next agent prepare) |
| `--disable-mcp NAME` | Disable (not connected; stays in registry) |
| `--mcp-url URL` | HTTP endpoint (alias: `--url` when installing) |
| `--mcp-transport http\|stdio` | Transport (default: `http` if `--mcp-url`, else need stdio + `--`) |
| `--mcp-header "Name: value"` | Extra HTTP header (repeatable); `${ENV}` / `${ENV:-default}` expanded at connect |
| `--mcp-env NAME=VALUE` | Environment for stdio child (repeatable); same expansion |
| `--mcp-allow-private` | Allow private/loopback HTTP for this server |
| `--mcp-protocol auto\|2026-07-28\|2025-11-25\|2025-03-26` | Negotiate hint |
| `--mcp-registry PATH` | Override registry path (tests) |

### HTTP examples

```sh
ainiux --add-mcp catalog --mcp-url https://awesome-mcp.tools/mcp

ainiux --add-mcp mock --mcp-url http://127.0.0.1:8765/mcp --mcp-allow-private

ainiux --add-mcp gh --mcp-url https://api.githubcopilot.com/mcp/ \
  --mcp-header "Authorization: Bearer ${GITHUB_TOKEN}"
```

### stdio examples

No shell is used: the first token after `--` is the executable.

```sh
ainiux --add-mcp time --mcp-transport stdio -- npx -y @modelcontextprotocol/server-time

ainiux --add-mcp mockstdio --mcp-transport stdio -- \
  python3 /absolute/path/to/ainiux/tests/mock_server/mcp_mock.py --stdio --mode both
```

Prefer **absolute paths** for local scripts so agent cwd does not matter.

Install probes the server when possible and prints dialect + tool count on success. A failed probe still saves the registry entry with a warning so you can fix network/credentials later.

---

## Agent interactive commands

In interactive agent (`-a`), Tab completion includes MCP commands.

| Command | Behavior |
| --- | --- |
| `/list-mcp` | Lists installed servers (history **notice** + short status) |
| `/enable-mcp NAME` | Enable in registry (tools reload on next prepare / new turn) |
| `/disable-mcp NAME` | Disable in registry |
| `/remove-mcp NAME` | Remove from registry |
| `/add-mcp` / `/install-mcp` | **Does not install.** Posts a durable notice with CLI examples (HTTP + stdio) |

Install remains CLI-only so URL, argv after `--`, headers, and private-loopback flags stay explicit and reviewable.

After enable/disable/remove or installing from another terminal, **start a new agent turn or restart `-a`** so prepare reloads MCP tools. The simplest check is `/list-mcp` then ask the model to call a known `mcp__…` tool.

---

## How tools load and run

1. Session prepare loads `~/.ainiux/mcp/registry.json`.
2. Enabled servers connect (HTTP negotiate or stdio spawn).
3. `tools/list` results are mapped to provider function definitions.
4. The model may call `mcp__server__tool` with JSON arguments.
5. Results are wrapped in the native agent envelope (`ok` / `data` / `error`) so success metrics and UI `[ok]`/`[err]` match other tools.
6. Permissions: MCP calls go through the same Guard/permission path as protected actions when Confirm/Smart controls apply; Yolo skips prompts (still logged).

### Protocol dialects

| Dialect | Notes |
| --- | --- |
| **2026-07-28** | Stateless Streamable HTTP: `MCP-Protocol-Version`, `Mcp-Method`, `Mcp-Name`; `server/discover` |
| **2025-11-25 / 2025-03-26** | Streamable HTTP with `initialize` / `notifications/initialized`, optional `Mcp-Session-Id`; JSON or SSE bodies |
| **stdio** | Newline-delimited JSON-RPC on stdin/stdout |

Auto-negotiate tries newer first, then common older. Successful dialect is stored as `last_dialect` on the registry entry.

Connect does **not** pin the agent prepare-job cancellation token onto the live client (prepare jobs cancel their token when they finish). Each tool call uses the **current turn** cancellation token so Esc/cancel still aborts in-flight MCP HTTP or stdio work without poisoning later turns.

---

## Permissions and security

- Servers must be **explicitly installed**; disabled servers never connect.
- **stdio** MCP runs as your user (same class of risk as installing a CLI tool). Argv only—no `sh -c`.
- **HTTP** private/loopback addresses are blocked unless `--mcp-allow-private` (per server) or global `--allow-private-url-fetch`.
- Prefer `${ENV}` in headers/env over pasting secrets into `registry.json`.
- Redact known secrets in logs; treat MCP tool **results as untrusted data** (like web fetch).
- Plan mode can still call remote MCP tools that mutate external state; MCP is not sandboxed per Act/Plan policy.

See also [Security](security.md).

---

## Testing

Automated unit coverage lives in `tests/unit/mcp/` (run via `make test-unit`):

- Registry CRUD
- Tool name qualification
- JSON-RPC / tools list parse
- HTTP mock (stateless + legacy dialects)
- stdio mock
- Prepare-cancel token stickiness regression
- Agent result envelope (`ok: true` / `ok: false`)

Local mock: `tests/mock_server/mcp_mock.py`.

There is no required live dependency on public MCP hosts in CI.

---

## Public servers (manual)

| Server | Example |
| --- | --- |
| [awesome-mcp.tools](https://awesome-mcp.tools/mcp) | `ainiux --add-mcp catalog --mcp-url https://awesome-mcp.tools/mcp` |
| Official Time | `ainiux --add-mcp time --mcp-transport stdio -- npx -y @modelcontextprotocol/server-time` |
| Official Everything | stdio via `npx -y @modelcontextprotocol/server-everything` |
| GitHub (token) | HTTP + `--mcp-header "Authorization: Bearer ${GITHUB_TOKEN}"` |

Catalogs: [mcpservers.org](https://mcpservers.org/), [modelcontextprotocol/servers](https://github.com/modelcontextprotocol/servers).

---

## Local vision bridge MCP

Ainiux ships a stdlib Python helper that exposes a local vision model as an MCP tool. Use it when the **agent model is text-only** (e.g. DeepSeek) but you still want OCR/captions via a **vision-capable** OpenAI-compatible endpoint (llama.cpp, vLLM, LM Studio, …).

Script: [`scripts/image_mcp_server.py`](../scripts/image_mcp_server.py) (Python 3.8+ stdlib core).

Optional large-image downscale uses **Pillow** (if importable) or **`ffmpeg` on PATH** — not linked into the ainiux C++ binary.

### Workflow

```sh
# Terminal 1 — vision LLM already serving Chat Completions (example :30000)
# llama-server / vLLM / LM Studio with a multimodal model

# Terminal 2 — MCP bridge (loopback by default; auto-resize large images)
python3 scripts/image_mcp_server.py http://localhost:30000 --port 8765

# Optional checks (no ainiux install required)
python3 scripts/image_mcp_server.py --self-test-resize tests/image_files/MathAssignment2.png
python3 scripts/image_mcp_server.py http://localhost:30000 \
  --self-test tests/image_files/temperature_meter.jpg

# Terminal 3 — register + use with a blind agent model
ainiux --add-mcp local-image --mcp-url http://127.0.0.1:8765/mcp --mcp-allow-private
ainiux deepseek -m deepseek-v4-flash -r "Describe the attached image." \
  --attach tests/image_files/sea_view.jpg
# Interactive: ainiux deepseek -m deepseek-v4-flash -a
```

The model should call `mcp__local-image__describe_image` (name depends on the install name). Args:

| Arg | Notes |
| --- | --- |
| `path` | Absolute filesystem path (stdio-style local servers) |
| `image_base64` / `image` | Raw base64 or `data:` URL (HTTP MCP rewrite from ainiux often fills this) |
| `mime_type` | Optional when using base64 |
| `prompt` | Question / caption instruction (default: detailed description) |
| `max_tokens` | Optional cap for the vision completion |

### Script flags

```text
image_mcp_server.py BASE_URL [--host 127.0.0.1] [-p|--port 8765] [-m MODEL]
  [--api-key KEY] [--timeout 120] [--max-tokens 1024] [--max-image-bytes N]
  [--resize auto|pillow|ffmpeg|none] [--max-edge 1024] [--soft-bytes 524288]
  [--jpeg-quality 85] [--ffmpeg PATH]
  [--mode legacy|stateless|both] [--enable-thinking]
  [--self-test IMAGE] [--self-test-resize IMAGE]
```

- `BASE_URL` accepts `http://host:port` or `…/v1`; normalized to `{base}/v1/chat/completions`. Not required with `--self-test-resize`.
- `-m` defaults to the first id from `/v1/models`.
- **Thinking is off by default** (`chat_template_kwargs.enable_thinking=false`) so Qwen-style local servers return `message.content` instead of filling only `reasoning_content`. Pass `--enable-thinking` if you want chain-of-thought (raise `--max-tokens` accordingly). Empty `content` still falls back to `reasoning_content` / `reasoning` / `thinking` when present.
- Bind stays on loopback unless you pass a non-loopback `--host` (not recommended).

### Large images and resize (Phase 1)

Ainiux’s default **`--max-image-bytes` (20 MiB)** is a **file-size** cap, not a guarantee the payload fits a provider context window. Base64 expands ~4/3; multi-megabyte screenshots are poor for OCR/VQA and expensive in tokens.

The vision bridge **downscales by default** when:

- raw bytes exceed `--soft-bytes` (default 512 KiB), or
- Pillow can read dimensions and the long edge exceeds `--max-edge` (default 1024)

| `--resize` | Behavior |
| --- | --- |
| `auto` (default) | Pillow if importable, else `ffmpeg` on PATH |
| `pillow` | Require Pillow only |
| `ffmpeg` | Require ffmpeg only |
| `none` | Never resize (full decoded image to the vision endpoint) |

Output of a resize is **JPEG** (first frame of animated GIF/WebP). Formats the bridge accepts for input: **PNG, JPEG, GIF, WebP** (plus whatever ffmpeg/Pillow can open).

Manual preprocess without the bridge:

```sh
ffmpeg -hide_banner -loglevel error -y -i huge.png \
  -vf "scale='min(1024,iw)':-2" -frames:v 1 /tmp/small.jpg
```

**Not in core yet:** optional resize inside the ainiux C++ attach path for multimodal models. See deferred work in `PLANS.md` (image preprocess phases).

### Install reminder

Private/loopback MCP URLs need **`--mcp-allow-private`** (or global private URL fetch). See security notes above.

### Vision quality

Caption quality and language depend entirely on the upstream vision model. Ainiux’s blind agent model only sees the **text** tool result (plus a short `[resized via …]` note when downscale ran).
---

## Non-goals (this release)

- MCP **resources** / **prompts** / sampling / roots as first-class surfaces  
- OAuth browser login / Client ID Metadata Documents  
- Project-scoped (`.ainiux-pr`) MCP registry  
- MCP tools in `--chat` or `--editor`  
- Marketplace UI or auto-install from agent without CLI  
- Built-in C++ MCP **server** inside the ainiux binary (helpers live under `scripts/` / `tests/mock_server/`)  

---

## Related docs

- [Agent workflows](agent.md)  
- [CLI](cli.md)  
- [Security](security.md)  
- [Documentation index](README.md)  
- [Project README](../README.md)

## Images and attachments (agent + MCP)

Ainiux keeps a **turn-scoped attachment bag** for agent modes. Images enter the bag via:

| Source | Behavior |
| --- | --- |
| CLI `ainiux run … --attach photo.png -r "…"` | Image loaded for the one-shot turn |
| Interactive `/attach PATH` (agent) | Queued like chat attach when wired through the turn |
| Native tool `attach` | Always registers in the bag; **vision models** also get pixels on later rounds; **text-only models** get bag-only (no pixel inject) |

There is no separate `attach_file` tool. Use **`attach`** for PNG/JPEG/GIF.

### MCP argument rewrite

Before each `mcp__*` `tools/call`, ainiux may rewrite string arguments that look like image paths:

| Transport | Typical rewrite |
| --- | --- |
| **stdio** (local process) | Normalize to **absolute path** (server reads the file) |
| **HTTP loopback** + `--mcp-allow-private` | Absolute **path** only (local bridge can open the file) |
| **HTTP remote** | Keep absolute `path`, add **`image_base64`** (+ `mime_type` when useful). Path-shaped fields are never replaced with raw base64 (that used to echo multi-MB error text into the model context). |

Also used when the field name suggests binary (`image_base64`, `image`, `data`, …). Caps follow `--max-image-bytes` and a max JSON args size. MCP tool **errors** and successes are size-capped before they re-enter the agent transcript.
**Blind model + local vision bridge example:**

```sh
# Terminal: vision endpoint on :30000, bridge on :8765
python3 scripts/image_mcp_server.py http://localhost:30000 --port 8765

ainiux --add-mcp local-image --mcp-url http://127.0.0.1:8765/mcp --mcp-allow-private
ainiux deepseek -m deepseek-v4-flash -r "Use mcp__local-image__describe_image on the attached photo." \
  --attach tests/image_files/sea_view.jpg
```

Or in agent: `attach` / `/attach`, then call `mcp__local-image__describe_image` with `{"path":"…"}` or let HTTP rewrite supply `image_base64`.

MCP **results** remain text-first (`content[].type == "text"`). Image content blocks from MCP are not injected as vision.

