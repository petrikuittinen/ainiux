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
auto_model=$("$ROOT/pkchat" "$BASE" --quiet --no-stream -p "model?")

test "$auto_model" = "$MODEL"
json=$("$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "hello" --format json)
printf '%s' "$json" | grep '"content":"Hello"' >/dev/null

stream=$("$ROOT/pkchat" "$BASE" --quiet --stream -m "$MODEL" -p "hello")
test "$stream" = "Hello"

responses_reply=$("$ROOT/pkchat" "$BASE" --quiet --api responses --no-stream -m "$MODEL" -p "hello")
test "$responses_reply" = "Hello"
responses_stream=$("$ROOT/pkchat" "$BASE" --quiet --api responses --stream -m "$MODEL" -p "hello")
test "$responses_stream" = "Hello"
responses_json=$("$ROOT/pkchat" "$BASE" --quiet --api responses --no-stream -m "$MODEL" -p "hello" --format json)
printf '%s' "$responses_json" | grep '"content":"Hello"' >/dev/null

reasoning_expected=$(printf '<think>internal trace</think>\n\nVisible answer')
reasoning_reply=$("$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "reasoning")
test "$reasoning_reply" = "$reasoning_expected"
reasoning_stream=$("$ROOT/pkchat" "$BASE" --quiet --stream -m "$MODEL" -p "reasoning")
test "$reasoning_stream" = "$reasoning_expected"
responses_reasoning=$("$ROOT/pkchat" "$BASE" --quiet --api responses --no-stream -m "$MODEL" -p "reasoning")
test "$responses_reasoning" = "$reasoning_expected"
responses_reasoning_stream=$("$ROOT/pkchat" "$BASE" --quiet --api responses --stream -m "$MODEL" -p "reasoning")
test "$responses_reasoning_stream" = "$reasoning_expected"
reasoning_chat_file="$ROOT/build/reasoning-chat.json"
"$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "reasoning" --save-chat "$reasoning_chat_file" >/dev/null
grep '<think>internal trace</think>' "$reasoning_chat_file" >/dev/null
previous_assistant=$("$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" --load-chat "$reasoning_chat_file" -p "previous-assistant")
test "$previous_assistant" = "Visible answer"

ndjson=$("$ROOT/pkchat" "$BASE" --quiet --stream -m "$MODEL" -p "hello" --format ndjson)
printf '%s\n' "$ndjson" | grep '"event":"delta"' >/dev/null
printf '%s\n' "$ndjson" | grep '"event":"done"' >/dev/null

verbose_err="$ROOT/build/verbose.err"
verbose_out=$("$ROOT/pkchat" "$BASE" -v --stream -m "$MODEL" -p "hello" 2>"$verbose_err")
test "$verbose_out" = "Hello"
grep 'TTFT: [0-9][0-9]* ms, Token/s: [0-9][0-9]*[.][0-9]' "$verbose_err" >/dev/null


CHAT_FILE="$ROOT/build/chat.json"
reply=$("$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "hello" --save-chat "$CHAT_FILE")
test "$reply" = "Hello"
grep '"schema_version"' "$CHAT_FILE" >/dev/null
grep '"role": "assistant"' "$CHAT_FILE" >/dev/null

loaded_reply=$("$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" --load-chat "$CHAT_FILE" -p "count-messages")
test "$loaded_reply" = "messages:3"

REPL_FILE="$ROOT/build/repl-chat.json"
repl_out=$(printf 'repl-one
/quit
' | "$ROOT/pkchat" "$BASE" --quiet --repl --no-stream -m "$MODEL" --save-chat "$REPL_FILE")
test "$repl_out" = "repl-one-reply"
grep 'repl-one' "$REPL_FILE" >/dev/null
grep 'repl-one-reply' "$REPL_FILE" >/dev/null


lmstudio_shortcut_out=$(printf 'repl-one
/quit
' | "$ROOT/pkchat" lmstudio --base-url "$BASE" --quiet --repl --no-stream)
test "$lmstudio_shortcut_out" = "repl-one-reply"
EMPTY_PORT=$((PORT + 1))
EMPTY_SERVER_LOG="$ROOT/build/mock_server_empty_models.log"
python3 "$ROOT/tests/mock_server/openai_mock.py" --port "$EMPTY_PORT" --model "$MODEL" --empty-models >"$EMPTY_SERVER_LOG" 2>&1 &
EMPTY_SERVER_PID=$!
trap 'kill "$SERVER_PID" "$EMPTY_SERVER_PID" >/dev/null 2>&1 || true; wait "$SERVER_PID" "$EMPTY_SERVER_PID" >/dev/null 2>&1 || true' EXIT INT TERM
i=0
while [ "$i" -lt 50 ]; do
    if curl -sS "http://127.0.0.1:$EMPTY_PORT/v1/models" >/dev/null 2>&1; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done
EMPTY_BASE="http://127.0.0.1:$EMPTY_PORT"
unknown_err="$ROOT/build/unknown-model.err"
unknown_model=$("$ROOT/pkchat" "$EMPTY_BASE" --no-stream -p "model?" 2>"$unknown_err")
test "$unknown_model" = "<missing>"
grep 'Model: unknown' "$unknown_err" >/dev/null

echo "integration tests passed"
