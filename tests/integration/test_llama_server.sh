#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BASE="${PKCHAT_LLAMA_SERVER_URL:-http://127.0.0.1:30000}"
MODEL="${PKCHAT_LLAMA_SERVER_MODEL:-}"

if ! curl -fsS "$BASE/v1/models" >/dev/null 2>&1; then
    echo "skipping llama-server integration test: $BASE/v1/models is unavailable" >&2
    exit 0
fi

if [ -z "$MODEL" ]; then
    MODEL=$(curl -fsS "$BASE/v1/models" | python3 -c '
import json, sys
payload = json.load(sys.stdin)
for key in ("data", "models"):
    items = payload.get(key) or []
    for item in items:
        for field in ("id", "name", "model"):
            value = item.get(field)
            if isinstance(value, str) and value:
                print(value)
                raise SystemExit
raise SystemExit("no model id found in llama-server /v1/models response")
')
fi

models_markdown=$("$ROOT/pkchat" "$BASE" --list-models)
printf '%s\n' "$models_markdown" | grep '131,072 tokens\|131072 tokens' >/dev/null

auto_model_out=$("$ROOT/pkchat" "$BASE" --quiet --no-stream -p "ping")
test -n "$auto_model_out"

verbose_err="$ROOT/build/llama-server-verbose.err"
verbose_out=$("$ROOT/pkchat" "$BASE" -v --stream -m "$MODEL" -p "hello" 2>"$verbose_err")
test -n "$verbose_out"
grep 'context: ' "$verbose_err" | grep '%)' >/dev/null

wrong_verbose_err="$ROOT/build/llama-server-wrong-model.err"
wrong_verbose_out=$("$ROOT/pkchat" "$BASE" -v --stream -m "definitely-not-a-model" -p "hello" 2>"$wrong_verbose_err")
test -n "$wrong_verbose_out"
grep 'context: ' "$wrong_verbose_err" | grep '%)' >/dev/null

echo "llama-server integration tests passed (model: $MODEL)"