#!/usr/bin/env bash
# Smoke-test the v1.30 PR 10 control server and embedded WUI with curl.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

BINARY="${REPO_ROOT}/ainiux"
PORT=18766
BUILD=0
SERVER_PID=""
TEMP_DIR=""
RESPONSE_FILE=""
FULL_SECRET="ainiux-control-smoke-${$}-full"
MCP_SECRET="ainiux-control-smoke-${$}-mcp"

usage() {
    cat <<'EOF'
Smoke-test the Ainiux v1.30 PR 10 control server and embedded WUI.

Usage: scripts/test-control-server.sh [options]

Options:
  --binary PATH   Ainiux executable (default: ./ainiux)
  --port PORT     Test listener port (default: 18766)
  --build         Build ./ainiux before testing
  -h, --help      Show this help

The script starts a temporary loopback server, tests authentication, discovery,
embedded WUI assets and browser headers, job submission/status/events/cancel/idempotency,
revision-safe chat/workspace discovery, Host/Origin policy, and scoped MCP credentials, then stops the server. It uses
provider "none" and never contacts a provider endpoint.
EOF
}

die() {
    echo "FAIL: $*" >&2
    if [ -n "${TEMP_DIR}" ] && [ -s "${TEMP_DIR}/server.log" ]; then
        echo "--- server log ---" >&2
        sed -n '1,160p' "${TEMP_DIR}/server.log" >&2
    fi
    exit 1
}

cleanup() {
    local exit_code=$?
    set +e
    trap - EXIT INT TERM
    if [ -n "${SERVER_PID}" ] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill -INT "${SERVER_PID}" 2>/dev/null
        for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
            kill -0 "${SERVER_PID}" 2>/dev/null || break
            sleep 0.05
        done
        if kill -0 "${SERVER_PID}" 2>/dev/null; then
            kill -TERM "${SERVER_PID}" 2>/dev/null
        fi
        wait "${SERVER_PID}" 2>/dev/null
    fi
    if [ -n "${TEMP_DIR}" ] && [ -d "${TEMP_DIR}" ]; then
        rm -rf -- "${TEMP_DIR}"
    fi
    exit "${exit_code}"
}

trap cleanup EXIT
trap 'exit 130' INT TERM

while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary)
            BINARY="${2:?--binary requires a path}"
            shift 2
            ;;
        --port)
            PORT="${2:?--port requires a number}"
            shift 2
            ;;
        --build)
            BUILD=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

case "${PORT}" in
    ''|*[!0-9]*) die "--port must be an integer from 1 through 65535" ;;
esac
if [ "${PORT}" -lt 1 ] || [ "${PORT}" -gt 65535 ]; then
    die "--port must be an integer from 1 through 65535"
fi

command -v curl >/dev/null 2>&1 || die "curl is required"

if [ "${BUILD}" -eq 1 ]; then
    echo "==> Building ainiux"
    make -C "${REPO_ROOT}" ainiux
fi

if [ ! -x "${BINARY}" ]; then
    die "Ainiux executable not found: ${BINARY} (run make or pass --build)"
fi

TEMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ainiux-control-test.XXXXXX")"
RESPONSE_FILE="${TEMP_DIR}/response.json"
mkdir -p "${TEMP_DIR}/home"

echo "==> Starting ${BINARY} on 127.0.0.1:${PORT}"
HOME="${TEMP_DIR}/home" \
AINIUX_SERVER_SECRET="${FULL_SECRET}" \
AINIUX_MCP_SECRET="${MCP_SECRET}" \
    "${BINARY}" server --workspace "${REPO_ROOT}" --port "${PORT}" \
    >"${TEMP_DIR}/server.stdout" 2>"${TEMP_DIR}/server.log" &
SERVER_PID=$!

ready=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
    if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
        wait "${SERVER_PID}" || true
        die "server exited before becoming ready (the port may already be in use)"
    fi
    code="$(curl --silent --show-error --output "${RESPONSE_FILE}" --write-out '%{http_code}' \
        --header "Authorization: Bearer ${FULL_SECRET}" \
        "http://127.0.0.1:${PORT}/ainiux/v1/health" 2>/dev/null || true)"
    if [ "${code}" = "200" ]; then
        ready=1
        break
    fi
    sleep 0.1
done
[ "${ready}" -eq 1 ] || die "server did not become ready"

request() {
    local expected_status="$1"
    local label="$2"
    shift 2
    local actual_status
    actual_status="$(curl --silent --show-error --output "${RESPONSE_FILE}" \
        --write-out '%{http_code}' "$@")" || die "${label}: curl transport failed"
    if [ "${actual_status}" != "${expected_status}" ]; then
        echo "Response body:" >&2
        sed -n '1,80p' "${RESPONSE_FILE}" >&2
        die "${label}: expected HTTP ${expected_status}, got ${actual_status}"
    fi
    echo "PASS: ${label} (HTTP ${actual_status})"
}

expect_body() {
    local needle="$1"
    local label="$2"
    if ! grep -Fq -- "${needle}" "${RESPONSE_FILE}"; then
        echo "Response body:" >&2
        sed -n '1,80p' "${RESPONSE_FILE}" >&2
        die "${label}: response did not contain ${needle}"
    fi
}

BASE_URL="http://127.0.0.1:${PORT}"
FULL_AUTH=(--header "Authorization: Bearer ${FULL_SECRET}")
MCP_AUTH=(--header "Authorization: Bearer ${MCP_SECRET}")

request 200 "public embedded WUI index" "${BASE_URL}/ui/"
expect_body '/ui/assets/app-v1.css' "WUI stylesheet reference"
expect_body '/ui/assets/app-v1.js' "WUI JavaScript reference"
WUI_HEADERS="${TEMP_DIR}/wui-headers.txt"
request 200 "versioned WUI JavaScript" --dump-header "${WUI_HEADERS}" \
    "${BASE_URL}/ui/assets/app-v1.js"
expect_body 'localStorage' "persistent browser token storage"
expect_body 'Invalid authentication' "invalid browser authentication state"
expect_body 'Last-Event-ID' "authenticated SSE replay"
grep -Fq 'Cache-Control: public, max-age=31536000, immutable' "${WUI_HEADERS}" || \
    die "versioned WUI asset did not receive immutable caching"
grep -Fq "Content-Security-Policy: default-src 'none'; script-src 'self'" "${WUI_HEADERS}" || \
    die "WUI response did not receive the strict same-origin CSP"
grep -Fq 'Referrer-Policy: no-referrer' "${WUI_HEADERS}" || \
    die "WUI response did not disable referrer disclosure"
request 404 "WUI directory serving rejection" "${BASE_URL}/ui/assets/"

request 200 "authenticated health" "${FULL_AUTH[@]}" "${BASE_URL}/ainiux/v1/health"
if [ "$(cat "${RESPONSE_FILE}")" != '{"status":"ok"}' ]; then
    die "health response exposed more than the minimal status object"
fi

request 401 "missing authentication" "${BASE_URL}/ainiux/v1/health"
expect_body '"code":"authentication_failed"' "missing authentication"

request 401 "wrong bearer token" --header 'Authorization: Bearer wrong-token' \
    "${BASE_URL}/ainiux/v1/health"

request 200 "authenticated status" "${FULL_AUTH[@]}" "${BASE_URL}/ainiux/v1/status"
expect_body '"api_version":"ainiux/v1"' "status API version"
expect_body '"address":"127.0.0.1"' "status bind address"
expect_body '"auth_scope":"full_control"' "status authentication scope"

request 200 "authenticated capabilities" "${FULL_AUTH[@]}" \
    "${BASE_URL}/ainiux/v1/capabilities"
expect_body '"operations":["health","status","capabilities","chat","run","plan","image","editor_assist","sessions","review","dired","workspace_mutations","files","chat_threads"]' "capability operations"
expect_body '"mcp":true' "MCP adapter availability"
expect_body '"web_ui":true' "embedded WUI availability"

request 401 "MCP token cannot access control API" "${MCP_AUTH[@]}" \
    "${BASE_URL}/ainiux/v1/status"
request 401 "full-control token is not an MCP token" "${FULL_AUTH[@]}" "${BASE_URL}/mcp"
request 405 "MCP endpoint requires POST" "${MCP_AUTH[@]}" "${BASE_URL}/mcp"

MCP_META='"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{"name":"curl-smoke","version":"1"},"io.modelcontextprotocol/clientCapabilities":{}}'
MCP_HEADERS=(--header 'Content-Type: application/json' --header 'Accept: application/json, text/event-stream' \
    --header 'MCP-Protocol-Version: 2026-07-28')
MCP_DISCOVER="{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"server/discover\",\"params\":{${MCP_META}}}"
request 200 "MCP server discovery" "${MCP_AUTH[@]}" "${MCP_HEADERS[@]}" \
    --header 'Mcp-Method: server/discover' --data "${MCP_DISCOVER}" "${BASE_URL}/mcp"
expect_body '"supportedVersions":["2026-07-28"]' "MCP protocol discovery"
MCP_LIST="{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{${MCP_META}}}"
request 200 "MCP tools list" "${MCP_AUTH[@]}" "${MCP_HEADERS[@]}" \
    --header 'Mcp-Method: tools/list' --data "${MCP_LIST}" "${BASE_URL}/mcp"
expect_body 'ainiux_chat' "MCP chat tool"

request 421 "non-loopback Host rejection" "${FULL_AUTH[@]}" --header 'Host: example.com' \
    "${BASE_URL}/ainiux/v1/health"
request 403 "cross-origin rejection" "${FULL_AUTH[@]}" \
    --header 'Origin: https://example.com' "${BASE_URL}/ainiux/v1/health"
request 405 "unsupported method rejection" "${FULL_AUTH[@]}" --request POST \
    "${BASE_URL}/ainiux/v1/health"
request 400 "unexpected body rejection" "${FULL_AUTH[@]}" \
    --request GET --header 'Content-Type: application/json' --data '{}' \
    "${BASE_URL}/ainiux/v1/health"

request 201 "chat thread creation" "${FULL_AUTH[@]}" \
    --header 'Content-Type: application/json' \
    --data '{"revision":0,"name":"Smoke thread","provider":"none","model":"offline"}' \
    "${BASE_URL}/ainiux/v1/chat/threads"
expect_body '"revision":1' "initial chat revision"
THREAD_ID="$(sed -n 's/.*"id":\([0-9][0-9]*\).*/\1/p' "${RESPONSE_FILE}")"
[ -n "${THREAD_ID}" ] || die "chat thread creation did not return a numeric ID"
request 200 "chat thread listing" "${FULL_AUTH[@]}" \
    "${BASE_URL}/ainiux/v1/chat/threads"
expect_body '"name":"Smoke thread"' "chat thread summary"
request 200 "revision-safe message append" "${FULL_AUTH[@]}" \
    --header 'Content-Type: application/json' \
    --data '{"revision":1,"messages":[{"role":"user","content":"hello from curl"}]}' \
    "${BASE_URL}/ainiux/v1/chat/threads/${THREAD_ID}/messages"
expect_body '"revision":2' "advanced chat revision"
request 409 "stale chat revision conflict" "${FULL_AUTH[@]}" \
    --header 'Content-Type: application/json' \
    --data '{"revision":1,"messages":[{"role":"user","content":"stale"}]}' \
    "${BASE_URL}/ainiux/v1/chat/threads/${THREAD_ID}/messages"
expect_body '"code":"revision_conflict"' "chat revision conflict code"
expect_body '"current_revision":2' "chat conflict reload metadata"
request 200 "chat thread load" "${FULL_AUTH[@]}" \
    "${BASE_URL}/ainiux/v1/chat/threads/${THREAD_ID}"
expect_body '"content":"hello from curl"' "persisted chat message"

JOB_BODY='{"provider":"none","messages":[{"role":"user","content":"offline smoke"}]}'
request 202 "chat job submission" "${FULL_AUTH[@]}" \
    --header 'Content-Type: application/json' --header 'Idempotency-Key: smoke-chat' \
    --data "${JOB_BODY}" "${BASE_URL}/ainiux/v1/jobs/chat"
expect_body '"existing":false' "new job marker"
JOB_ID="$(sed -n 's/.*"id":"\([A-Za-z0-9_-]*\)".*/\1/p' "${RESPONSE_FILE}")"
[ -n "${JOB_ID}" ] || die "job submission did not return a job ID"

request 200 "idempotent chat job replay" "${FULL_AUTH[@]}" \
    --header 'Content-Type: application/json' --header 'Idempotency-Key: smoke-chat' \
    --data "${JOB_BODY}" "${BASE_URL}/ainiux/v1/jobs/chat"
expect_body '"existing":true' "existing job marker"
request 409 "idempotency payload conflict" "${FULL_AUTH[@]}" \
    --header 'Content-Type: application/json' --header 'Idempotency-Key: smoke-chat' \
    --data '{"provider":"none","messages":[{"role":"user","content":"changed"}]}' \
    "${BASE_URL}/ainiux/v1/jobs/chat"
expect_body '"code":"idempotency_conflict"' "idempotency conflict code"

request 200 "job status snapshot" "${FULL_AUTH[@]}" \
    "${BASE_URL}/ainiux/v1/jobs/${JOB_ID}"
expect_body "\"id\":\"${JOB_ID}\"" "job status ID"
request 200 "job SSE replay" "${FULL_AUTH[@]}" \
    "${BASE_URL}/ainiux/v1/jobs/${JOB_ID}/events"
expect_body 'event: queued' "SSE queued event"
expect_body 'data: {"id":1' "SSE stable envelope"
request 200 "idempotent terminal cancellation" "${FULL_AUTH[@]}" --request POST \
    "${BASE_URL}/ainiux/v1/jobs/${JOB_ID}/cancel"

echo "==> All control-server smoke tests passed"
