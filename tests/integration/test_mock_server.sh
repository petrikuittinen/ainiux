#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
PORT="${PKCHAT_TEST_PORT:-18080}"
MODEL="mock-model"
SERVER_LOG="$ROOT/build/mock_server.log"

python3 "$ROOT/tests/mock_server/openai_mock.py" --port "$PORT" --model "$MODEL" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
trap 'kill "$SERVER_PID" >/dev/null 2>&1 || true; wait "$SERVER_PID" >/dev/null 2>&1 || true' EXIT INT TERM

i=0
while [ "$i" -lt 50 ]; do
    if curl -sS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done

BASE="http://127.0.0.1:$PORT"

models=$("$ROOT/pkchat" --list-models "$BASE" --quiet)
test "$models" = "$MODEL"

reply=$("$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "hello")
test "$reply" = "Hello"

json=$("$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "hello" --format json)
printf '%s' "$json" | grep '"content":"Hello"' >/dev/null

stream=$("$ROOT/pkchat" "$BASE" --quiet --stream -m "$MODEL" -p "hello")
test "$stream" = "Hello"

ndjson=$("$ROOT/pkchat" "$BASE" --quiet --stream -m "$MODEL" -p "hello" --format ndjson)
printf '%s\n' "$ndjson" | grep '"event":"delta"' >/dev/null
printf '%s\n' "$ndjson" | grep '"event":"done"' >/dev/null

echo "integration tests passed"
