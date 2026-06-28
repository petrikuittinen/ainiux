#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
PORT="${PKCHAT_SQLITE_TEST_PORT:-18181}"
MODEL="mock-model"
SERVER_LOG="$ROOT/build/sqlite_mock_server.log"
HOME_DIR="$ROOT/build/sqlite-integration-home"

rm -rf "$HOME_DIR"
mkdir -p "$HOME_DIR"
export HOME="$HOME_DIR"
export OPENAI_API_KEY="${OPENAI_API_KEY:-integration-test-key}"

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
python3 "$ROOT/tests/integration/tui_sqlite_driver.py" \
    "$ROOT/pkchat" "$BASE" "$MODEL" "$HOME_DIR" all

echo "sqlite persistence integration tests passed"