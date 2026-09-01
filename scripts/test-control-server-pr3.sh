#!/usr/bin/env bash
# Repeatable v1.3 PR3 verification: build/tests plus a live mock-provider job.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
BINARY="${REPO_ROOT}/ainiux"
BUILD=1
RUN_FAULTS=1
MOCK_PID=""
SERVER_PID=""
TEMP_DIR=""

usage() {
    cat <<'EOF'
Run repeatable v1.3 PR3 verification.

Usage: scripts/test-control-server-pr3.sh [options]

Options:
  --binary PATH   Ainiux executable (default: ./ainiux)
  --no-build      Skip make before testing
  --no-faults     Skip make test-unit-faults
  -h, --help      Show this help

The script runs make test, the control-server curl smoke test, and a live chat
job against tests/mock_server/openai_mock.py. It uses loopback only.
EOF
}

die() {
    echo "FAIL: $*" >&2
    if [ -n "${TEMP_DIR}" ] && [ -s "${TEMP_DIR}/server.log" ]; then
        sed -n '1,160p' "${TEMP_DIR}/server.log" >&2
    fi
    exit 1
}

free_port() {
    python3 - <<'PY'
import socket
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.bind(("127.0.0.1", 0))
print(sock.getsockname()[1])
sock.close()
PY
}

cleanup() {
    local exit_code=$?
    set +e
    trap - EXIT INT TERM
    if [ -n "${SERVER_PID}" ] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill -INT "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    if [ -n "${MOCK_PID}" ] && kill -0 "${MOCK_PID}" 2>/dev/null; then
        kill "${MOCK_PID}" 2>/dev/null || true
        wait "${MOCK_PID}" 2>/dev/null || true
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
        --binary) BINARY="${2:?--binary requires a path}"; shift 2 ;;
        --no-build) BUILD=0; shift ;;
        --no-faults) RUN_FAULTS=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

command -v curl >/dev/null 2>&1 || die "curl is required"
command -v python3 >/dev/null 2>&1 || die "python3 is required"
if [ "${BUILD}" -eq 1 ]; then
    echo "==> Building ainiux"
    make -C "${REPO_ROOT}" ainiux
fi
[ -x "${BINARY}" ] || die "Ainiux executable not found: ${BINARY}"

echo "==> Running unit and integration smoke tests"
make -C "${REPO_ROOT}" test
if [ "${RUN_FAULTS}" -eq 1 ]; then make -C "${REPO_ROOT}" test-unit-faults; fi

echo "==> Running authenticated control-server smoke test"
"${SCRIPT_DIR}/test-control-server.sh" --binary "${BINARY}"

TEMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ainiux-pr3-live.XXXXXX")"
MOCK_PORT="$(free_port)"
SERVER_PORT="$(free_port)"
SECRET="ainiux-pr3-${$}-secret"
MODEL="mock-model"
BASE_URL="http://127.0.0.1:${SERVER_PORT}"
AUTH=(--header "Authorization: Bearer ${SECRET}")
RESPONSE="${TEMP_DIR}/response.json"

echo "==> Starting mock provider on 127.0.0.1:${MOCK_PORT}"
python3 "${REPO_ROOT}/tests/mock_server/openai_mock.py" \
    --api chat --port "${MOCK_PORT}" --model "${MODEL}" --stream-delay 0.1 \
    >"${TEMP_DIR}/mock.log" 2>&1 &
MOCK_PID=$!
for _ in $(seq 1 100); do
    if curl --silent --show-error --fail "http://127.0.0.1:${MOCK_PORT}/v1/models" >/dev/null 2>&1; then break; fi
    sleep 0.05
done
kill -0 "${MOCK_PID}" 2>/dev/null || die "mock provider exited during startup"

echo "==> Starting control server on 127.0.0.1:${SERVER_PORT}"
AINIUX_SERVER_SECRET="${SECRET}" OPENAI_API_KEY="ainiux-pr3-mock-key" \
    "${BINARY}" server --no-config --quiet --workspace "${REPO_ROOT}" \
    --port "${SERVER_PORT}" --base-url "http://127.0.0.1:${MOCK_PORT}" \
    --api chat -m "${MODEL}" >"${TEMP_DIR}/server.stdout" 2>"${TEMP_DIR}/server.log" &
SERVER_PID=$!
ready=0
for _ in $(seq 1 100); do
    if curl --silent --show-error --output /dev/null --write-out '%{http_code}' \
        "${AUTH[@]}" "${BASE_URL}/ainiux/v1/health" 2>/dev/null | grep -qx '200'; then
        ready=1; break
    fi
    kill -0 "${SERVER_PID}" 2>/dev/null || break
    sleep 0.05
done
[ "${ready}" -eq 1 ] || die "control server did not become ready"

request_status() {
    local expected="$1" label="$2" actual
    shift 2
    actual="$(curl --silent --show-error --output "${RESPONSE}" --write-out '%{http_code}' "$@")" \
        || die "${label}: curl transport failed"
    [ "${actual}" = "${expected}" ] || die "${label}: expected HTTP ${expected}, got ${actual}"
    echo "PASS: ${label} (HTTP ${actual})"
}

json_field() {
    python3 - "$1" "$2" <<'PY'
import json
import sys
value = json.load(open(sys.argv[1], encoding="utf-8"))
for part in sys.argv[2].split("."):
    value = value[part]
print(value)
PY
}

CHAT_JSON='{"messages":[{"role":"user","content":"hello"}]}'
request_status 202 "live chat submission" "${AUTH[@]}" \
    --header 'Content-Type: application/json' --header 'Idempotency-Key: live-chat-1' \
    --data-binary "${CHAT_JSON}" "${BASE_URL}/ainiux/v1/jobs/chat"
JOB_ID="$(json_field "${RESPONSE}" 'job.id')"
[ -n "${JOB_ID}" ] || die "live chat submission returned no job ID"

STATE=""
for _ in $(seq 1 200); do
    curl --silent --show-error --output "${RESPONSE}" \
        "${AUTH[@]}" "${BASE_URL}/ainiux/v1/jobs/${JOB_ID}" || die "status poll failed"
    STATE="$(json_field "${RESPONSE}" 'state')"
    case "${STATE}" in succeeded|failed|cancelled) break ;; esac
    sleep 0.05
done
[ "${STATE}" = "succeeded" ] || die "live chat job ended in state ${STATE}"
[ "$(json_field "${RESPONSE}" 'result.content')" = "Hello" ] || die "unexpected live chat result"
echo "PASS: live chat job reached succeeded with mock response"

request_status 200 "same idempotency key replay" "${AUTH[@]}" \
    --header 'Content-Type: application/json' --header 'Idempotency-Key: live-chat-1' \
    --data-binary "${CHAT_JSON}" "${BASE_URL}/ainiux/v1/jobs/chat"
grep -Fq '"existing":true' "${RESPONSE}" || die "idempotency replay was not marked existing"

request_status 409 "changed idempotency payload conflict" "${AUTH[@]}" \
    --header 'Content-Type: application/json' --header 'Idempotency-Key: live-chat-1' \
    --data-binary '{"messages":[{"role":"user","content":"changed"}]}' \
    "${BASE_URL}/ainiux/v1/jobs/chat"
grep -Fq '"code":"idempotency_conflict"' "${RESPONSE}" || die "missing idempotency conflict code"

SSE="${TEMP_DIR}/events.sse"
curl --silent --show-error --max-time 10 "${AUTH[@]}" \
    "${BASE_URL}/ainiux/v1/jobs/${JOB_ID}/events" >"${SSE}" \
    || die "SSE replay request failed"
grep -Fq 'id: 1' "${SSE}" || die "SSE replay missed first event ID"
grep -Fq 'event: queued' "${SSE}" || die "SSE replay missed queued event"
grep -Fq 'event: succeeded' "${SSE}" || die "SSE replay missed succeeded event"
echo "PASS: live SSE replay includes ordered terminal events"

SSE_REPLAY="${TEMP_DIR}/events-replay.sse"
curl --silent --show-error --max-time 10 "${AUTH[@]}" \
    --header 'Last-Event-ID: 1' \
    "${BASE_URL}/ainiux/v1/jobs/${JOB_ID}/events" >"${SSE_REPLAY}" \
    || die "SSE Last-Event-ID replay request failed"
if grep -Fq 'id: 1' "${SSE_REPLAY}"; then die "SSE replay repeated event 1"; fi
grep -Fq 'event: succeeded' "${SSE_REPLAY}" || die "SSE reconnect missed terminal event"
echo "PASS: Last-Event-ID resumes after the requested cursor"

request_status 202 "cancellable chat submission" "${AUTH[@]}" \
    --header 'Content-Type: application/json' --header 'Idempotency-Key: live-cancel-1' \
    --data-binary '{"messages":[{"role":"user","content":"cancel me"}]}' \
    "${BASE_URL}/ainiux/v1/jobs/chat"
CANCEL_ID="$(json_field "${RESPONSE}" 'job.id')"
request_status 200 "job cancellation request" "${AUTH[@]}" --request POST \
    "${BASE_URL}/ainiux/v1/jobs/${CANCEL_ID}/cancel"
echo "PASS: cancellation endpoint returned a job snapshot"

echo "==> PR3 live control-server verification passed"
