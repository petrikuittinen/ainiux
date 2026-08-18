#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
PORT="${AINIUX_SMOKE_TEST_PORT:-18079}"
RESPONSES_PORT="${AINIUX_RESPONSES_SMOKE_TEST_PORT:-18078}"
MODEL="mock-model"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/ainiux-mock-smoke.XXXXXX")
SERVER_LOG="$WORK/openai-mock.log"
RESPONSES_SERVER_LOG="$WORK/responses-mock.log"

cleanup() {
    if [ "${SERVER_PID:-0}" -gt 0 ]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" >/dev/null 2>&1 || true
    fi
    if [ "${RESPONSES_SERVER_PID:-0}" -gt 0 ]; then
        kill "$RESPONSES_SERVER_PID" >/dev/null 2>&1 || true
        wait "$RESPONSES_SERVER_PID" >/dev/null 2>&1 || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT HUP INT TERM

python3 "$ROOT/tests/mock_server/openai_mock.py" --api chat --port "$PORT" --model "$MODEL" \
    >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
python3 "$ROOT/tests/mock_server/openai_mock.py" --api responses --port "$RESPONSES_PORT" \
    --model "$MODEL" >"$RESPONSES_SERVER_LOG" 2>&1 &
RESPONSES_SERVER_PID=$!

i=0
while [ "$i" -lt 50 ]; do
    if curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1 &&
       curl -fsS "http://127.0.0.1:$RESPONSES_PORT/v1/models" >/dev/null 2>&1; then
        break
    fi
    i=$((i + 1))
    sleep 0.05
done
if [ "$i" -ge 50 ]; then
    echo "mock smoke server did not become ready" >&2
    exit 1
fi

BASE="http://127.0.0.1:$PORT"
RESPONSES_BASE="http://127.0.0.1:$RESPONSES_PORT"
export HOME="$WORK/home"
export XDG_CONFIG_HOME="$WORK/config"
mkdir -p "$HOME" "$XDG_CONFIG_HOME"

chat_err="$WORK/chat.err"
chat=$("$ROOT/ainiux" "$BASE" --quiet --stream -p "hello" 2>"$chat_err")
test "$chat" = "Hello"
test ! -s "$chat_err"

responses=$("$ROOT/ainiux" "$RESPONSES_BASE" --quiet --api responses --no-stream \
    -m "$MODEL" -p "hello")
test "$responses" = "Hello"

no_input_out="$WORK/no-input.out"
no_input_err="$WORK/no-input.err"
set +e
"$ROOT/ainiux" "$BASE" --quiet --no-stream -m "$MODEL" </dev/null \
    >"$no_input_out" 2>"$no_input_err"
no_input_status=$?
set -e
test "$no_input_status" -eq 2
test ! -s "$no_input_out"
grep '^AINIUX_ERR_BAD_ARGS: prompt is empty;' "$no_input_err" >/dev/null

expect_provider_failure() {
    label=$1
    api_base=$2
    api_kind=$3
    stream_flag=$4
    prompt=$5
    error_code=$6
    detail=$7
    failure_out="$WORK/$label.out"
    failure_err="$WORK/$label.err"
    set +e
    "$ROOT/ainiux" "$api_base" --quiet "$stream_flag" --api "$api_kind" \
        -m "$MODEL" -p "$prompt" >"$failure_out" 2>"$failure_err"
    failure_status=$?
    set -e
    test "$failure_status" -eq 4
    test ! -s "$failure_out"
    grep "^$error_code:" "$failure_err" >/dev/null
    grep -F "$detail" "$failure_err" >/dev/null
}

for api_kind in chat responses; do
    if [ "$api_kind" = chat ]; then
        api_base=$BASE
    else
        api_base=$RESPONSES_BASE
    fi
    expect_provider_failure "$api_kind-empty-json" "$api_base" "$api_kind" \
        --no-stream mock-empty-response AINIUX_ERR_JSON_PARSE "unexpected end of input"
    expect_provider_failure "$api_kind-malformed-json" "$api_base" "$api_kind" \
        --no-stream mock-malformed-json AINIUX_ERR_JSON_PARSE "JSON parse error"
    expect_provider_failure "$api_kind-invalid-utf8" "$api_base" "$api_kind" \
        --no-stream mock-invalid-utf8 AINIUX_ERR_JSON_PARSE "invalid UTF-8"
    expect_provider_failure "$api_kind-empty-stream" "$api_base" "$api_kind" \
        --stream mock-empty-response AINIUX_ERR_PROVIDER_SCHEMA "did not contain"
    expect_provider_failure "$api_kind-malformed-stream" "$api_base" "$api_kind" \
        --stream mock-malformed-json AINIUX_ERR_SSE_PARSE "JSON parse error"
    expect_provider_failure "$api_kind-invalid-utf8-stream" "$api_base" "$api_kind" \
        --stream mock-invalid-utf8 AINIUX_ERR_SSE_PARSE "invalid UTF-8"
done

agent_workspace="$WORK/agent-workspace"
mkdir -p "$agent_workspace"
printf '%s\n' "agent smoke fixture" >"$agent_workspace/agent-smoke.txt"
agent_out="$WORK/agent.out"
agent_err="$WORK/agent.err"
(
    cd "$agent_workspace"
    "$ROOT/ainiux" run "$BASE" -m "$MODEL" --run "AINIUX_AGENT_SMOKE" \
        --no-stream --no-agent-log >"$agent_out" 2>"$agent_err"
)
test "$(cat "$agent_out")" = "agent-smoke-ok"
grep -E 'read_file.* in [0-9]+ ms' "$agent_err" >/dev/null || {
    cat "$agent_err" >&2
    exit 1
}
grep -E '^Agent metrics: tool calls [0-9]+ \([0-9]+ failed\), input [0-9]+ tokens( \(estimated\))?, output [0-9]+ tokens( \(estimated\))?, time [0-9]+\.[0-9]{2} s$' "$agent_err" >/dev/null || {
    cat "$agent_err" >&2
    exit 1
}
test -f "$agent_workspace/.ainiux-pr/agent.sqlite"
if grep -F 'agent-smoke-ok' "$agent_err" >/dev/null; then
    echo "agent final answer leaked to stderr" >&2
    exit 1
fi

# A second one-shot run in the same project must not inject the first run's
# durable transcript into its model request.
agent_second_out="$WORK/agent-second.out"
agent_second_err="$WORK/agent-second.err"
(
    cd "$agent_workspace"
    "$ROOT/ainiux" run "$BASE" -m "$MODEL" --run "AINIUX_AGENT_SMOKE" \
        --no-stream --no-agent-log >"$agent_second_out" 2>"$agent_second_err"
)
test "$(cat "$agent_second_out")" = "agent-smoke-ok"
if grep -F 'Injected prior agent transcript' "$agent_second_err" >/dev/null; then
    cat "$agent_second_err" >&2
    exit 1
fi

echo "mock smoke tests passed"
