#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
PORT="${AINIUX_SMOKE_TEST_PORT:-18079}"
MODEL="mock-model"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/ainiux-mock-smoke.XXXXXX")
SERVER_LOG="$WORK/openai-mock.log"

cleanup() {
    if [ "${SERVER_PID:-0}" -gt 0 ]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" >/dev/null 2>&1 || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT HUP INT TERM

python3 "$ROOT/tests/mock_server/openai_mock.py" --port "$PORT" --model "$MODEL" \
    >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!

i=0
while [ "$i" -lt 50 ]; do
    if curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1; then
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
export HOME="$WORK/home"
export XDG_CONFIG_HOME="$WORK/config"
export XDG_CONFIG_DIRS="$WORK/system-config"
mkdir -p "$HOME" "$XDG_CONFIG_HOME"

chat_err="$WORK/chat.err"
chat=$("$ROOT/ainiux" "$BASE" --quiet --stream -p "hello" 2>"$chat_err")
test "$chat" = "Hello"
test ! -s "$chat_err"

responses=$("$ROOT/ainiux" "$BASE" --quiet --api responses --no-stream \
    -m "$MODEL" -p "hello")
test "$responses" = "Hello"

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
grep -E '^Agent metrics: input [0-9]+ tokens( \(estimated\))?, output [0-9]+ tokens( \(estimated\))?, time [0-9]+\.[0-9]{2} s$' "$agent_err" >/dev/null || {
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
