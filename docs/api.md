# Ainiux control API

The v1.30 control API is versioned under `/ainiux/v1/`. PR 10 provides an
authenticated loopback-by-default listener, optional TLS/direct non-loopback access,
asynchronous one-shot jobs,
interactive agent sessions, revision-safe workspace review/edit routes, and
a separate MCP 2026-07-28 endpoint at `/mcp`, plus revision-safe access to the
existing personal chat library. It also serves the embedded same-origin browser
controller from `/ui/`. Later routes must use the PR 1 operation/wire contracts rather than exposing
provider, runtime, or terminal-internal structures directly.

## Start and authenticate

```sh
export AINIUX_SERVER_SECRET='use-a-long-random-value'
ainiux server --workspace . --port 8766
curl -H "Authorization: Bearer $AINIUX_SERVER_SECRET" \
  http://127.0.0.1:8766/ainiux/v1/status
```

The listener binds `127.0.0.1` by default. Full-control credential precedence is
`--server-secret-file PATH`, then `AINIUX_SERVER_SECRET`, then the stable
per-user managed secret at `~/.ainiux/server-secret`. The managed secret is
atomically created with private platform permissions when absent. Plain server
mode never prints it. An optional,
different MCP-only credential comes from `AINIUX_MCP_SECRET` or
`--mcp-secret-file PATH`; it is accepted only for `/mcp`, which is a separate
MCP-only scope. Operator-supplied secret files must be outside the fixed served
workspace. The managed file remains protected under the excluded user-profile
directory. On POSIX secret files grant no group/other permissions. Secret values,
query-string credentials, cookies, and `AINIUX_API_KEY` are not accepted.

Every API and MCP endpoint requires `Authorization: Bearer TOKEN`, including
health. This keeps control requests free of ambient loopback authority. Static,
non-secret WUI boot assets are the only exception; the browser supplies the
token on every API request and event stream. The three discovery routes are:

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

## Embedded browser controller

`GET /ui/` loads the controller without a bearer token so a browser can show
the connection form. `/ui` and `/ui/index.html` return the same no-store HTML
shell. Its exact versioned stylesheet and ES-module paths live below
`/ui/assets/` and use immutable caching; unknown asset paths, query strings,
bodies, directory requests, and non-GET methods are rejected. Assets are
compiled into the executable rather than read from the served workspace.

Browser responses set `nosniff`, a same-origin CSP, no-referrer, frame denial,
same-origin resource/opener policy, and a permissions policy that disables
camera, microphone, geolocation, payment, and USB. The CSP permits only
same-origin scripts/styles/connections and `data:` images returned by image
jobs. CORS remains disabled.

The browser saves a successfully validated controller token in origin-scoped
`localStorage`, clears it on a 401 or explicit sign-out, and retains it through
network/server outages while reconnecting. It is never accepted through a query
string, cookie, or ambient browser credential. See [the browser guide](web-mode.md).

## TLS and direct non-loopback access

`ainiux webserver` and `ainiux server --webui` are browser-oriented entry points.
Without an explicit `--bind`, they listen on `0.0.0.0`, print loopback and active
IPv4 interface `/ui/` links, warn about plaintext exposure, and make a best-effort
local browser launch. This web-mode choice is the explicit acknowledgement for
plaintext wildcard access. Add `--bind 127.0.0.1` for local-only use or configure
TLS for access across an untrusted network. Plain `ainiux server` retains the
stricter policy below.

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

## Revision-safe workspace editing

The server owns the single canonical workspace selected at startup. No route
accepts a filesystem root, native absolute path, symlink/reparse traversal, or
overwrite-by-default destination. Reads never return native absolute paths:

```text
GET /ainiux/v1/workspace/review
GET /ainiux/v1/dired?path=RELATIVE_PATH
GET /ainiux/v1/files?path=RELATIVE_PATH
```

Paths use slash-separated workspace-relative components. The dired response
contains `path`, an opaque directory `revision`, bounded `entries` (`name`,
`path`, `type`, opaque `revision`, `size`, `modified_at`, `mutable`, and
`executable`), and
`truncated`. Review recursively returns the same entry shape plus a
file/directory/byte summary. File reads return `path`, opaque `revision`,
JSON-safe `content`, byte `size`, and `truncated:false`; individual remote
editing reads are capped at 1 MiB. Missing, non-regular, oversized, traversing,
or symlink/reparse paths
are rejected. `.ainiux-pr`, `.ainiux`, `.git`, environment/credential names,
and bundled sensitive configuration files are excluded.

Browser clients percent-encode individual query path components. The server
strictly decodes those values so spaces and UTF-8 filenames work, while route
paths themselves still reject percent encoding and workspace validation still
rejects traversal, backslashes, invalid components, links, and out-of-root
targets.

PR 9 adds explicit-target mutations and atomic text saves:

```text
POST /ainiux/v1/dired/mutations
POST /ainiux/v1/files
PUT  /ainiux/v1/files?path=RELATIVE_PATH
POST /ainiux/v1/jobs/editor-assist
```

`dired/mutations` accepts `{"operations":[...]}` with 1–32 entries and
returns one result per target. Operations are `mkdir`, `move` (or `rename`),
`copy`, and `delete`. Every existing source requires the opaque `revision`
last returned for that exact path. Creation destinations require
`parent_revision` or `destination_parent_revision`; destinations must not
already exist. Delete additionally requires `confirmation` to exactly equal
`delete RELATIVE_PATH`, and non-empty directories require `recursive:true`.
Directory copy/delete walks are capped at 512 entries and 8 MiB, with each file
capped at 1 MiB. A failed item does not suppress results for other items.

`POST /files` creates bounded UTF-8 text from `path`, `content`, and
`parent_revision`. `PUT /files?path=...` replaces an existing file from
`content` and its reviewed `revision`. Saves use the shared atomic
temp-write/flush/replace primitive and preserve the old file on write failure.
A stale save returns HTTP 409 `revision_conflict` with `current_revision` for
conflict UI. Reads and mutations serialize inside the service; identity-bearing
revisions also detect same-content file replacement between review and action.

Editor assist is an asynchronous job with `path`, `revision`, `instruction`,
the ordinary optional `provider`/`model`/`api` fields, and optional paired byte
offsets `selection_start`/`selection_end`. Without a selection it operates on
the whole reviewed file. It reuses the standalone editor's assist prompt and
provider path, verifies the file revision before provider work, and returns a
proposed `{start,length,replacement}` plus the source revision. It never saves
automatically; clients apply the proposal through the revision-checked PUT.

## Revision-safe chat threads

PR 7 exposes the existing user-private SQLite chat library through domain
operations rather than database files or tables:

```text
GET  /ainiux/v1/chat/threads
POST /ainiux/v1/chat/threads
GET  /ainiux/v1/chat/threads/:thread_id
POST /ainiux/v1/chat/threads/:thread_id/messages
POST /ainiux/v1/chat/threads/:thread_id/regenerate
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

`name`, `provider`, and `model` are optional bounded metadata strings. When
`name` is omitted or left blank, the first non-empty user-message line becomes
the title (bounded to the store's title limit). Creation returns `201` and
revision `1`. Append one through 64 transcript messages with
the last revision observed by the client:

```json
{
  "revision": 1,
  "provider": "deepseek",
  "model": "deepseek-chat",
  "messages": [
    {"role":"user","content":"Hello"},
    {"role":"assistant","content":"Hi"}
  ]
}
```

Roles are `system`, `user`, or `assistant`. Optional bounded `provider` and
`model` values update the thread metadata in the same revision-checked
transaction, so a resumed browser or TUI chat keeps its effective routing.
This persistence operation does not
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

Regenerate accepts `{"revision":N}`. It atomically removes messages after the
latest user prompt and returns that prompt plus the advanced thread revision;
the client then submits a normal streaming chat job and appends its replacement
answer. A stale revision cannot discard newer TUI or API messages.

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
POST /ainiux/v1/jobs/chat   {provider?, model?, api?, reasoning?, messages:[{role,content}]}
POST /ainiux/v1/jobs/models {provider?, api?}
POST /ainiux/v1/jobs/run    {provider?, model?, api?, goal}
POST /ainiux/v1/jobs/plan   {provider?, model?, api?, goal}
GET  /ainiux/v1/images/catalog
POST /ainiux/v1/images/inputs  (raw image/png or image/jpeg)
DELETE /ainiux/v1/images/inputs/:input_id
POST /ainiux/v1/jobs/image  {provider?, model?, api?, prompt, size?, aspect?, quality?, format?, input_image_ids?:[]}
POST /ainiux/v1/jobs/editor-assist {provider?, model?, api?, path, revision, instruction, selection_start?, selection_end?}
GET  /ainiux/v1/jobs/:job_id
GET  /ainiux/v1/jobs/:job_id/events
POST /ainiux/v1/jobs/:job_id/cancel
```

The `models` job calls the selected provider's existing model-list operation
and returns `{"provider":"...","models":["..."],"reasoning_options":[...]}`.
Catalog-matched models include their model-aware reasoning values and labels in
`reasoning_options`; unmatched models are omitted from that metadata. The chat
job's optional `reasoning` field accepts the same `auto|off|VALUE|TOKENS` syntax
as the CLI. Model requests use the same
provider concurrency limit, cancellation, authentication, error redaction,
retention, and SSE lifecycle as other jobs. Clients should keep manual model
entry available when a provider does not advertise model listing or its
endpoint is unavailable.

Successful chat results identify the effective `provider` and `model`. A
successful image result contains the encoded image plus a workspace-relative
`server_path` such as `image2.png`. The server creates the first available
`imageN.ext` atomically and never overwrites an existing workspace file.

The authenticated image catalog is a safe projection of the effective layered
`images.conf`. It exposes provider/model identifiers, defaults, edit support,
input counts, selector values, custom-dimension constraints, and upload limits;
protocol details, field mappings, regular expressions, defaults JSON, and config
paths stay server-side. Uploads use a raw body with an exact `image/png` or
`image/jpeg` content type. The response contains an opaque `id`, MIME type, byte
size, and expiry. Inputs are memory-only, expire after one hour, and can be
deleted early. Limits are 20 MiB per file, 40 MiB combined per image job, 16
inputs globally per job (with lower catalog model limits), and 160 MiB of live
server upload buffers. `input_image_ids` preserves array order. Missing or
expired IDs are rejected before a job starts.

Successful chat, run, and plan results include an additive `metrics` object.
Interactive agent completion/failure events include the same object, and the
latest value is retained as `last_turn_metrics` in the session snapshot:

```json
{
  "context_used_tokens": 1200,
  "context_window_tokens": 128000,
  "input_tokens": 900,
  "fresh_input_tokens": 700,
  "cache_read_tokens": 200,
  "cache_write_tokens": 0,
  "output_tokens": 300,
  "total_tokens": 1200,
  "input_tokens_estimated": false,
  "output_tokens_estimated": false,
  "elapsed_ms": 2450,
  "ttft_ms": 310,
  "output_tokens_per_second": 42.5
}
```

Unknown measurements are `null`; zero is a measured zero. Estimation flags
apply independently to input and output totals. Session snapshots also expose
`context.{used_tokens,window_tokens}` and `active_elapsed_ms` so a controller
can display live turn time without polling faster than its own UI clock.

Submission normally returns `202`; `Idempotency-Key` reuse with identical input
returns the retained job with `200`, while changed input returns the typed
`idempotency_conflict` response. Run and plan reserve the single workspace agent
lane at submission and return `agent_lane_busy` (409) instead of waiting.

The events route uses `text/event-stream`. Each `data:` value is the full event
DTO above. Reconnect with `Last-Event-ID`; ordered retained events are replayed.
Chat and editor-assist jobs emit `delta` events while model text arrives. The
server enables provider streaming for browser-created chat, run, plan,
editor-assist, and interactive-agent work by default.
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

Provider model-list, chat, and image operations share a bounded global pool. Run, plan, and
interactive-agent mutations share one workspace lane; a conflict returns 409
instead of queueing behind another agent. The owning session loop alone mutates
agent, approval, dired, editor, or chat session state. Cancellation belongs to
one operation/job and cannot cancel unrelated session work.

Clients must use the authenticated capabilities endpoint rather than infer
support from this contract document. The WUI follows that rule and disables
controls for operations absent from an older or reduced server.

## Interactive agent sessions

Create and inspect a persistent project-local agent session:

```text
POST   /ainiux/v1/sessions/agent
GET    /ainiux/v1/sessions
GET    /ainiux/v1/sessions/:session_id
GET    /ainiux/v1/sessions/:session_id/events
POST   /ainiux/v1/sessions/:session_id/reasoning
POST   /ainiux/v1/sessions/:session_id/settings
POST   /ainiux/v1/sessions/:session_id/turns
POST   /ainiux/v1/sessions/:session_id/turns/:turn_id/cancel
POST   /ainiux/v1/sessions/:session_id/approvals/:approval_id
GET    /ainiux/v1/sessions/:session_id/approvals/:approval_id/review-file
DELETE /ainiux/v1/sessions/:session_id
```

Session creation accepts `kind` (`agent`), `provider`, `model`, `api`, `reasoning`,
`permission_mode` (`confirm`, `smart`, or `yolo`), and `task_mode`
(`act` or `plan`). Preparation is asynchronous and returns `202` with an
opaque session ID. A turn body is `{"text":"..."}` and returns `202`
with a server-generated turn ID. Only one turn may be active per session; a
concurrent turn returns `409`.

An idle session's reasoning selector can be changed with
`{"reasoning":"auto|off|VALUE|TOKENS"}`. Snapshots include the effective
`reasoning` value and catalog-derived `reasoning_options`.

The settings route accepts exactly one of `provider`, `model`, `task_mode`, or
`permission_mode` per request and only while the session is idle. It returns
the updated snapshot and emits `settings_changed`. Provider changes use that
profile's configured API default; the browser does not choose Chat Completions
or Responses itself.

Session SSE events use the same bounded ordered replay contract as jobs and
include the session and turn IDs. Streaming agent rounds publish `activity`
events with append/upsert/commit/discard actions for live assistant text,
reasoning, tools, and notices. Guard Ask produces an `approval_required`
event with a server-generated approval ID. Resolve it with
`{"decision":"allow"}`, `{"decision":"deny"}`, or
`{"decision":"cancelled"}`. Approval IDs are single-use and tied to the
active turn. Review-file returns only the bounded workspace-relative file
requested by the pending Guard approval; absolute and out-of-root paths are
rejected. Closing a session cancels active work, finishes its project session,
closes its event stream, and releases retained state. Sessions are bounded by
`--max-sessions` (default 32). In-memory controllers and event streams are not
restored after a server restart. A newly created controller does restore the
workspace's persisted `.ainiux-pr` provider, model, API, reasoning, and
permission settings before applying explicit creation fields.

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
