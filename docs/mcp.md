# MCP support (agent modes)

Ainiux can use tools from [Model Context Protocol](https://modelcontextprotocol.io) (MCP) servers in **agent modes only**:

| Mode | Entry |
| --- | --- |
| Interactive agent | `-a` / `--agent` / `ainiux agent` |
| One-shot Act | `-r` / `--run` / `ainiux run` |
| One-shot Plan | `plan` / `--plan` / `--plan-file` |

**Not loaded in:** ordinary `--chat`, standalone `--editor`, REPL, or `--security-review`.

MCP does not replace native workspace tools (`read_file`, `edit_file`, `run_command`, …). It adds **external** tools from servers you install.

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

## Non-goals (this release)

- MCP **resources** / **prompts** / sampling / roots as first-class surfaces  
- OAuth browser login / Client ID Metadata Documents  
- Project-scoped (`.ainiux-pr`) MCP registry  
- MCP tools in `--chat` or `--editor`  
- Marketplace UI or auto-install from agent without CLI  
- Shipping a production MCP **server** inside ainiux (mock only)  

---

## Related docs

- [Agent workflows](agent.md)  
- [CLI](cli.md)  
- [Security](security.md)  
- [Documentation index](README.md)  
- [Project README](../README.md)  
