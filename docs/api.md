# Ainiux control API

The v1.3 control API is versioned under `/ainiux/v1/`. PR 2 provides a
loopback-only listener and authenticated discovery routes. Job submission,
sessions, MCP serving, filesystem operations, and the WUI remain later slices.
Later routes must use the PR 1 operation/wire contracts rather than exposing
provider, runtime, or terminal-internal structures directly.

## Start and authenticate

```sh
export AINIUX_SERVER_SECRET='use-a-long-random-value'
ainiux server --workspace . --port 8766
curl -H "Authorization: Bearer $AINIUX_SERVER_SECRET" \
  http://127.0.0.1:8766/ainiux/v1/status
```

The listener binds only `127.0.0.1`. A full-control credential is mandatory and
comes from `AINIUX_SERVER_SECRET` or `--server-secret-file PATH`. An optional,
different MCP-only credential comes from `AINIUX_MCP_SECRET` or
`--mcp-secret-file PATH`; it is accepted only for `/mcp`, which still returns
404 until the MCP adapter lands. Secret files must be outside the fixed served
workspace. On POSIX they must grant no group/other permissions. Secret values,
query-string credentials, cookies, and `AINIUX_API_KEY` are not accepted.

Every PR 2 endpoint requires `Authorization: Bearer TOKEN`, including health.
This keeps all API requests free of ambient loopback authority. The three routes
are:

```text
GET /ainiux/v1/health
GET /ainiux/v1/status
GET /ainiux/v1/capabilities
```

`health` returns only `{"status":"ok"}`. `status` reports the API version,
full-control scope, loopback/plain-HTTP bind state, and public connection/job
limits. `capabilities` lists the currently routed discovery operations, built-in
provider names without credentials, authentication configuration, and disabled
optional adapters. It does not claim that jobs or MCP are available.

From a source checkout, run `scripts/test-control-server.sh --build` for a
self-contained curl smoke test of the listener, authentication scopes, discovery
routes, and Host/Origin/method/body rejection. Use `--port PORT` when 18766 is
already occupied.

## Operation boundary

Reusable operations accept explicit request data, a cancellation token, and an
optional typed event sink. They return structured results and never select an
output file, write to stdout or stderr, own terminal state, or install process
signal handlers. CLI adapters retain their existing output behavior while HTTP,
MCP, and later browser adapters translate the same operation results.

The first extracted operations cover ordinary chat and one-shot image
generation. Agent run/plan continue through the existing surface-neutral
`run_agent_goal` boundary until the serialized agent lane lands with the job API.

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

## Initial bounded HTTP contract

The PR 2 strict parser enforces these constants:

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
| Default provider-operation concurrency | 4 |
| Workspace agent mutation lanes | 1 |
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
