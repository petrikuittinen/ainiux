# Ainiux control API

The v1.3 control API is versioned under `/ainiux/v1/`. PR 8 provides an
authenticated loopback-by-default listener, optional TLS/direct non-loopback access,
asynchronous one-shot jobs,
interactive agent sessions, read-only workspace review/dired/file routes, and
a separate MCP 2026-07-28 endpoint at `/mcp`, plus revision-safe access to the
existing personal chat library. Filesystem mutations and the WUI remain later slices.
Later routes must use the PR 1 operation/wire contracts rather than exposing
provider, runtime, or terminal-internal structures directly.

## Start and authenticate

```sh
export AINIUX_SERVER_SECRET='use-a-long-random-value'
ainiux server --workspace . --port 8766
curl -H "Authorization: Bearer $AINIUX_SERVER_SECRET" \
  http://127.0.0.1:8766/ainiux/v1/status
```

The listener binds `127.0.0.1` by default. A full-control credential is mandatory and
comes from `AINIUX_SERVER_SECRET` or `--server-secret-file PATH`. An optional,
different MCP-only credential comes from `AINIUX_MCP_SECRET` or
`--mcp-secret-file PATH`; it is accepted only for `/mcp`, which is a separate
MCP-only scope. Secret files must be outside the fixed served
workspace. On POSIX they must grant no group/other permissions. Secret values,
query-string credentials, cookies, and `AINIUX_API_KEY` are not accepted.

Every endpoint requires `Authorization: Bearer TOKEN`, including health.
This keeps all API requests free of ambient loopback authority. The three routes
are:

```text
GET /ainiux/v1/health
GET /ainiux/v1/status
GET /ainiux/v1/capabilities
```

`health` returns only `{"status":"ok"}`. `status` reports the API version,
full-control scope, bind/transport state, and public connection/job
limits. `capabilities` lists the currently routed discovery operations, built-in
provider names without credentials, authentication configuration, and disabled
optional adapters. It advertises MCP as enabled separately from the control
API job operation list.

## TLS and direct non-loopback access

Direct access uses an IPv4 `--bind ADDRESS`. Any non-loopback address requires
both `--tls-cert` and `--tls-key`; only an explicit `--insecure-plain-bind`
allows unencrypted HTTP. OpenSSL is detected at build time. A build without its
development files still supports the default loopback HTTP listener but rejects
TLS startup.

The certificate file is a PEM chain. The matching unencrypted PEM private key
must be a regular, non-symlinked private file outside the served workspace; on
POSIX it may not grant group/other permissions, and on Windows it must have the
same protected current-user/SYSTEM ACL used for other private state. The server
requires TLS 1.2 or newer and never prompts for a key passphrase.

```sh
export AINIUX_SERVER_SECRET='use-a-long-random-value'
ainiux server --workspace . --bind 192.0.2.10 \
  --tls-cert /secure/ainiux-chain.pem --tls-key /secure/ainiux-key.pem
curl --cacert /secure/ca.pem \
  -H "Authorization: Bearer $AINIUX_SERVER_SECRET" \
  https://192.0.2.10:8766/ainiux/v1/status
```

`Host` must match the configured address and listener port. A wildcard
`0.0.0.0` bind accepts only IPv4-literal Host values at that port. If `Origin`
is present, it must exactly match the request scheme and accepted Host; a TLS
listener therefore rejects `http://` origins. DNS names, query-string tokens,
cookies, forwarded-host authority, and cross-origin browser requests do not
expand the allowlist.

Remote interactive sessions run at Confirm/Smart authority by default. An
explicit or project-restored Yolo mode is denied/downgraded unless startup also
includes `--allow-remote-yolo`. This option is deliberately separate from the
persisted project setting.

## Read-only workspace

The server owns the single canonical workspace selected at startup. These
routes accept only GET and never return native absolute paths:

```text
GET /ainiux/v1/workspace/review
GET /ainiux/v1/dired?path=RELATIVE_PATH
GET /ainiux/v1/files?path=RELATIVE_PATH
```

Paths use slash-separated workspace-relative components. The dired response
contains `path`, bounded `entries` (`name`, `path`, `type`, `size`, and
`modified_at`), and `truncated`. Review recursively returns the same entry
shape plus a file/directory/byte summary. File reads return `path`, JSON-safe
`content`, byte `size`, and `truncated:false`; individual reads are capped at
1 MiB. Missing, non-regular, oversized, traversing, or symlink/reparse paths
are rejected. `.ainiux-pr`, `.ainiux`, `.git`, environment/credential names,
and bundled sensitive configuration files are excluded. There are no mutation
routes in PR 6.

## Revision-safe chat threads

PR 7 exposes the existing user-private SQLite chat library through domain
operations rather than database files or tables:

```text
GET  /ainiux/v1/chat/threads
POST /ainiux/v1/chat/threads
GET  /ainiux/v1/chat/threads/:thread_id
POST /ainiux/v1/chat/threads/:thread_id/messages
```

Listing returns at most 200 newest summaries with `id`, `revision`, `name`,
timestamps, provider/model labels, message count, and read-only state. Loading
a thread returns its transcript. Loads are bounded to the newest 512 messages
and 4 MiB of message content; `message_count`, per-message `ordinal`, and
`messages_truncated` tell a client whether older content was omitted. Remote
loads do not change the TUI's last-active-thread selection.

Create a thread with revision zero:

```json
{"revision":0,"name":"Remote chat","provider":"openai","model":"MODEL"}
```

`name`, `provider`, and `model` are optional bounded metadata strings. Creation
returns `201` and revision `1`. Append one through 64 transcript messages with
the last revision observed by the client:

```json
{
  "revision": 1,
  "messages": [
    {"role":"user","content":"Hello"},
    {"role":"assistant","content":"Hi"}
  ]
}
```

Roles are `system`, `user`, or `assistant`. This persistence operation does not
start a model request; use the asynchronous chat job route for provider work.
Successful appends return the new revision and message count. Existing TUI
saves advance the same SQLite revision, so a stale API append returns
`revision_conflict` (409) with `details.current_revision`; no stale messages are
written. Read-only threads return `thread_read_only` (409).

Thread responses expose at most 64 attachments per message, with kind, MIME
type, display name, and byte size only; `attachments_truncated` reports an
omission. Managed-media identifiers, inline attachment bodies, original
source references, database paths, base URLs, usage internals, and provider
credentials are omitted. PR 7 does not accept remote attachment input or
delete/rename threads.

From a source checkout, run `scripts/test-control-server.sh --build` for a
self-contained curl smoke test of the listener, authentication scopes, discovery,
revision-safe chat persistence, jobs, SSE replay, and Host/Origin/method/body
rejection. Use `--port PORT` when 18766 is
already occupied.

For the complete repeatable PR 4 check—including `make test`, optional fault
tests, a live bundled mock provider, real chat completion, idempotency replay and
conflict, SSE reconnect, and cancellation—run
`scripts/test-control-server-pr3.sh`. Use `--no-build`, `--no-faults`, or
`--binary PATH` to shorten or redirect the run.

## Operation boundary

Reusable operations accept explicit request data, a cancellation token, and an
optional typed event sink. They return structured results and never select an
output file, write to stdout or stderr, own terminal state, or install process
signal handlers. CLI adapters retain their existing output behavior while HTTP,
MCP, and later browser adapters translate the same operation results.

The extracted operations cover ordinary chat and one-shot image generation.
Agent run/plan use the existing headless `run_agent_goal` boundary and the
server's fixed canonical workspace.

Internal operation event enums are not public protocol values. The server wire
boundary explicitly converts them to lowercase event names and stable JSON DTOs.

## Error envelope

Every future API failure uses this shape:

```json
{
  "error": {
    "code": "invalid_request",
    "message": "model is required",
    "details": {"field": "model"},
    "request_id": "req_..."
  }
}
```

Public codes are stable strings and are mapped explicitly from `ErrorCode`.
Notable mappings are `invalid_request` (400), `authentication_failed` (401),
`conflict` and `cancelled` (409), `unsupported_feature` (422), `rate_limited`
(429), `upstream_failure`/`upstream_schema`/`upstream_stream` (502), `timeout`
(504), and `internal` (500). Internal enum spellings are never serialized.
`details` must be a JSON object; malformed internal detail data becomes `{}`.

## Job and event DTOs

Job states are `queued`, `running`, `succeeded`, `failed`, and `cancelled`.
Events use monotonically increasing integer IDs and this stable envelope:

```json
{
  "id": 42,
  "type": "progress",
  "created_at": "2026-09-01T12:00:00Z",
  "job_id": "job_...",
  "session_id": null,
  "turn_id": null,
  "data": {}
}
```

Submit a JSON object to one of these routes:

```text
POST /ainiux/v1/jobs/chat   {provider?, model?, api?, messages:[{role,content}]}
POST /ainiux/v1/jobs/run    {provider?, model?, api?, goal}
POST /ainiux/v1/jobs/plan   {provider?, model?, api?, goal}
POST /ainiux/v1/jobs/image  {provider?, model?, api?, prompt, size?, aspect?, quality?, format?}
GET  /ainiux/v1/jobs/:job_id
GET  /ainiux/v1/jobs/:job_id/events
POST /ainiux/v1/jobs/:job_id/cancel
```

Submission normally returns `202`; `Idempotency-Key` reuse with identical input
returns the retained job with `200`, while changed input returns the typed
`idempotency_conflict` response. Run and plan reserve the single workspace agent
lane at submission and return `agent_lane_busy` (409) instead of waiting.

The events route uses `text/event-stream`. Each `data:` value is the full event
DTO above. Reconnect with `Last-Event-ID`; ordered retained events are replayed.
An evicted cursor returns `replay_expired` (410), requiring a fresh job snapshot.
Event count and bytes are bounded per job, terminal jobs are bounded by
`--max-jobs`, and none of this state survives a server restart.

## Initial bounded HTTP contract

The strict parser and PR 4 job broker enforce these constants:

| Limit | Initial value |
| --- | ---: |
| Request line | 8 KiB |
| All request headers | 32 KiB |
| Header count | 100 |
| JSON request body | 1 MiB |
| Upload body | 20 MiB |
| Requests per keep-alive connection | 100 |
| Default simultaneous connections | 64 |
| Default retained/in-flight jobs | 128 |
| Default interactive sessions | 32 |
| Default provider-operation concurrency | 4 |
| Workspace agent mutation lanes | 1 |
| Retained events per job | 256 |
| Retained event bytes per job | 1 MiB |
| SSE heartbeat interval | 15 seconds |
| Header read timeout | 10 seconds |
| Body read timeout | 30 seconds |
| Idle keep-alive timeout | 60 seconds |
| Total non-job request timeout | 120 seconds |

Only exact, single `Content-Length` framing is accepted. Transfer-Encoding,
multiple or conflicting lengths, request bodies on bodyless routes, ambiguous
encoded paths, traversal, obsolete folded headers, and unsupported methods will
be rejected. Long provider and agent work becomes asynchronous jobs rather than
extending the request timeout.

## Concurrency ownership

Provider chat and image operations share a bounded global pool. Run, plan, and
interactive-agent mutations share one workspace lane; a conflict returns 409
instead of queueing behind another agent. The owning session loop alone mutates
agent, approval, dired, editor, or chat session state. Cancellation belongs to
one operation/job and cannot cancel unrelated session work.

No other route listed in the v1.3 roadmap is callable until its implementation
slice lands. Clients must use the authenticated capabilities endpoint rather
than infer support from this contract document.

## Interactive agent sessions

Create and inspect a persistent project-local agent session:

```text
POST   /ainiux/v1/sessions/agent
GET    /ainiux/v1/sessions
GET    /ainiux/v1/sessions/:session_id
GET    /ainiux/v1/sessions/:session_id/events
POST   /ainiux/v1/sessions/:session_id/turns
POST   /ainiux/v1/sessions/:session_id/turns/:turn_id/cancel
POST   /ainiux/v1/sessions/:session_id/approvals/:approval_id
GET    /ainiux/v1/sessions/:session_id/approvals/:approval_id/review-file
DELETE /ainiux/v1/sessions/:session_id
```

Session creation accepts `kind` (`agent`), `provider`, `model`, `api`,
`permission_mode` (`confirm`, `smart`, or `yolo`), and `task_mode`
(`act` or `plan`). Preparation is asynchronous and returns `202` with an
opaque session ID. A turn body is `{"text":"..."}` and returns `202`
with a server-generated turn ID. Only one turn may be active per session; a
concurrent turn returns `409`.

Session SSE events use the same bounded ordered replay contract as jobs and
include the session and turn IDs. Guard Ask produces an `approval_required`
event with a server-generated approval ID. Resolve it with
`{"decision":"allow"}`, `{"decision":"deny"}`, or
`{"decision":"cancelled"}`. Approval IDs are single-use and tied to the
active turn. Review-file returns only the bounded workspace-relative file
requested by the pending Guard approval; absolute and out-of-root paths are
rejected. Closing a session cancels active work, finishes its project session,
closes its event stream, and releases retained state. Sessions are bounded by
`--max-sessions` (default 32) and are not restored after a server restart.

## MCP 2026-07-28 endpoint

`/mcp` uses stateless Streamable HTTP. Every request is a new authenticated POST
using the MCP-only bearer token, `Content-Type: application/json`, and an
`Accept` header containing both `application/json` and `text/event-stream`.
Requests must carry matching `MCP-Protocol-Version`, `Mcp-Method`, and (for
tool/task calls) `Mcp-Name` headers plus the per-request `_meta` protocol,
client-info, and client-capabilities fields. The server does not implement the
legacy initialize/session/GET-SSE transport.

Supported RPCs are `server/discover`, `tools/list`, `tools/call`, `tasks/get`,
`tasks/update`, and `tasks/cancel`. The deterministic tools are
`ainiux_chat`, `ainiux_run`, `ainiux_plan`, `ainiux_image`,
`ainiux_job_get`, and `ainiux_job_cancel`. Job execution reuses the control
API's bounded `JobService`; no provider credentials or filesystem paths are
exposed.

Clients that advertise `io.modelcontextprotocol/tasks` may receive an opaque
task handle from `tools/call` and poll it with `tasks/get`, or cancel it with
`tasks/cancel`. Task handles are in-memory, bounded, and expire when retained
server state is evicted or the server restarts. Clients without the extension
receive a completed MCP tool result containing the asynchronous Ainiux job
snapshot and can use `ainiux_job_get` with its opaque handle.
