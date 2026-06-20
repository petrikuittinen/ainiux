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


private_fetch_err="$ROOT/build/fetch-private.err"
if "$ROOT/pkchat" --fetch-url "$BASE/page" --html-format markdown --quiet >"$ROOT/build/fetch-private.out" 2>"$private_fetch_err"; then
    echo "private URL fetch should have failed without --allow-private-url-fetch" >&2
    exit 1
fi
grep 'refusing to fetch private' "$private_fetch_err" >/dev/null

page_md=$("$ROOT/pkchat" --fetch-url "$BASE/page" --allow-private-url-fetch --html-format markdown --quiet)
printf '%s\n' "$page_md" | grep -F '# Mock Page' >/dev/null
printf '%s\n' "$page_md" | grep -F '**bold**' >/dev/null
printf '%s\n' "$page_md" | grep -F '*emphasis*' >/dev/null
printf '%s\n' "$page_md" | grep -F '[docs](https://example.com/docs)' >/dev/null
printf '%s\n' "$page_md" | grep -F 'bad()' >/dev/null && exit 1 || true

page_text=$("$ROOT/pkchat" --fetch-url "$BASE/page" --allow-private-url-fetch --html-format text --quiet)
printf '%s\n' "$page_text" | grep -F 'Mock Page' >/dev/null
printf '%s\n' "$page_text" | grep -F 'Hello bold and emphasis with docs (https://example.com/docs).' >/dev/null

local_html="$ROOT/build/local-input.html"
cat >"$local_html" <<'HTML'
<!doctype html>
<h1>Local Input Title</h1>
<p>Local <strong>bold</strong> and <a href="https://example.com/local">link</a>.</p>
HTML
input_html_plain=$("$ROOT/pkchat" --input "$local_html" --output-format plaintext --quiet)
printf '%s\n' "$input_html_plain" | grep -F 'Local Input Title' >/dev/null
printf '%s\n' "$input_html_plain" | grep -F 'Local bold and link (https://example.com/local).' >/dev/null

local_md="$ROOT/build/local-input.md"
printf '# Local Input Title\n\nLocal **bold** and [link](https://example.com/local).\n' >"$local_md"
input_md_html="$ROOT/build/local-input-output.html"
"$ROOT/pkchat" --input "$local_md" --output-format html --output "$input_md_html" --quiet
grep '<!doctype html>' "$input_md_html" >/dev/null
grep '<meta charset="utf-8">' "$input_md_html" >/dev/null
grep 'name="viewport"' "$input_md_html" >/dev/null
grep '<h1>Local Input Title</h1>' "$input_md_html" >/dev/null

input_json=$("$ROOT/pkchat" --input "$local_md" --output-format json --quiet)
printf '%s\n' "$input_json" | grep '"source":"file ' >/dev/null
printf '%s\n' "$input_json" | grep '"output_format":"md"' >/dev/null
printf '%s\n' "$input_json" | grep 'Local Input Title' >/dev/null

local_txt="$ROOT/build/local-input.txt"
printf 'Plain local input\nSecond line\n' >"$local_txt"
input_jsond=$("$ROOT/pkchat" --input "$local_txt" --output-format jsond --quiet)
printf '%s\n' "$input_jsond" | grep '"event":"content"' >/dev/null
printf '%s\n' "$input_jsond" | grep 'Plain local input' >/dev/null

url_context=$("$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "summarize-url" --fetch-url "$BASE/page" --allow-private-url-fetch)
test "$url_context" = "url-context-ok"
url_system_context=$("$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -s "url-system" -p "summarize-url-system" --fetch-url "$BASE/page" --allow-private-url-fetch)
test "$url_system_context" = "url-system-context-ok"
input_context=$("$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "summarize-input" --input "$local_md")
test "$input_context" = "input-context-ok"

attachment_md="$ROOT/build/attachment-alpha.MD"
attachment_txt="$ROOT/build/attachment-beta.TxT"
printf '# Attachment Alpha\n\nFirst attachment.\n' >"$attachment_md"
printf 'Attachment Beta\nSecond attachment.\n' >"$attachment_txt"
attachment_chat_file="$ROOT/build/attachment-chat.json"
attachment_reply=$("$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "summarize-attachments" \
    --attach "$attachment_md" --attach "$attachment_txt" --save-chat "$attachment_chat_file")
test "$attachment_reply" = "attachments-ok"
grep 'Attachment Alpha' "$attachment_chat_file" >/dev/null
grep 'Attachment Beta' "$attachment_chat_file" >/dev/null

attach_without_prompt_err="$ROOT/build/attach-without-prompt.err"
if "$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" --attach "$attachment_txt" \
    >"$ROOT/build/attach-without-prompt.out" 2>"$attach_without_prompt_err"; then
    echo "attachment without a prompt should fail" >&2
    exit 1
fi
grep -- '--attach requires -p/--prompt' "$attach_without_prompt_err" >/dev/null

insert_file="$ROOT/build/insert-context.txt"
printf 'Inserted Context Marker\n' >"$insert_file"
insert_reply=$(printf '/insert %s\nsummarize-insert\n/quit\n' "$insert_file" | \
    "$ROOT/pkchat" "$BASE" --quiet --repl --no-stream -m "$MODEL")
test "$insert_reply" = "insert-ok"

large_attachment="$ROOT/build/large-attachment.txt"
printf 'this attachment is too large' >"$large_attachment"
large_attachment_err="$ROOT/build/large-attachment.err"
if "$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "hello" --attach "$large_attachment" \
    --max-input-bytes 8 >"$ROOT/build/large-attachment.out" 2>"$large_attachment_err"; then
    echo "oversized text attachment should fail" >&2
    exit 1
fi
grep -- '--max-input-bytes limit of 8 bytes' "$large_attachment_err" >/dev/null

binary_attachment="$ROOT/build/binary-attachment.txt"
printf 'text\000binary' >"$binary_attachment"
binary_attachment_err="$ROOT/build/binary-attachment.err"
if "$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "hello" --attach "$binary_attachment" \
    >"$ROOT/build/binary-attachment.out" 2>"$binary_attachment_err"; then
    echo "binary text attachment should fail" >&2
    exit 1
fi
grep 'input appears to be binary' "$binary_attachment_err" >/dev/null

invalid_utf8_attachment="$ROOT/build/invalid-utf8-attachment.txt"
printf '\377' >"$invalid_utf8_attachment"
invalid_utf8_attachment_err="$ROOT/build/invalid-utf8-attachment.err"
if "$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "hello" --attach "$invalid_utf8_attachment" \
    >"$ROOT/build/invalid-utf8-attachment.out" 2>"$invalid_utf8_attachment_err"; then
    echo "invalid UTF-8 attachment should fail" >&2
    exit 1
fi
grep 'Input expects UTF-8 text' "$invalid_utf8_attachment_err" >/dev/null

missing_attachment_err="$ROOT/build/missing-attachment.err"
if "$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "hello" \
    --attach "$ROOT/build/does-not-exist.txt" >"$ROOT/build/missing-attachment.out" 2>"$missing_attachment_err"; then
    echo "missing text attachment should fail" >&2
    exit 1
fi
grep 'could not open plaintext for reading' "$missing_attachment_err" >/dev/null

for deferred in pdf docx; do
    deferred_path="$ROOT/build/deferred.$deferred"
    printf 'not implemented' >"$deferred_path"
    deferred_err="$ROOT/build/deferred-$deferred.err"
    if "$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "hello" --attach "$deferred_path" \
        >"$ROOT/build/deferred-$deferred.out" 2>"$deferred_err"; then
        echo "$deferred attachment should remain unsupported" >&2
        exit 1
    fi
    grep 'unsupported input file type' "$deferred_err" >/dev/null
done

local_png="$ROOT/build/local-image.PnG"
printf '\211PNG\r\n\032\nmock-image' >"$local_png"
image_chat_file="$ROOT/build/image-chat.json"
image_reply=$("$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "describe-image" --input "$local_png" \
    --image-capability allow --save-chat "$image_chat_file")
test "$image_reply" = "image-input-ok"
grep 'describe-image' "$image_chat_file" >/dev/null
if grep 'data:image/png;base64' "$image_chat_file" >/dev/null; then
    echo "saved chat must not contain base64 image data" >&2
    exit 1
fi

unknown_image_err="$ROOT/build/unknown-image-capability.err"
if "$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "describe-image" --input "$local_png" \
    >"$ROOT/build/unknown-image-capability.out" 2>"$unknown_image_err"; then
    echo "unknown models should require an image capability override" >&2
    exit 1
fi
grep 'not recognized as image-capable' "$unknown_image_err" >/dev/null

local_jpeg="$ROOT/build/local-image.JPEG"
printf '\377\330\377mock-jpeg' >"$local_jpeg"
multiple_image_reply=$("$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "describe-images" \
    --attach "$local_png" --attach "$local_jpeg" --image-capability allow)
test "$multiple_image_reply" = "images:2"

repl_insert_image=$(printf '/insert %s\ndescribe-image\ndescribe-image\n/quit\n' "$local_png" | \
    "$ROOT/pkchat" "$BASE" --quiet --repl --no-stream -m "$MODEL" --image-capability allow)
repl_insert_image_expected=$(printf 'image-input-ok\nmissing-image-input')
test "$repl_insert_image" = "$repl_insert_image_expected"
repl_attach_image=$(printf '/attach %s\ndescribe-image\n/quit\n' "$local_jpeg" | \
    "$ROOT/pkchat" "$BASE" --quiet --repl --no-stream -m "$MODEL" --image-capability allow)
test "$repl_attach_image" = "image-input-ok"
repl_fetch_reply=$(printf '/fetch %s/page\nsummarize-url\n/quit\n' "$BASE" | \
    "$ROOT/pkchat" "$BASE" --quiet --repl --no-stream -m "$MODEL" --allow-private-url-fetch)
test "$repl_fetch_reply" = "url-context-ok"

image_extract_err="$ROOT/build/image-extract.err"
if "$ROOT/pkchat" --input "$local_png" --quiet >"$ROOT/build/image-extract.out" 2>"$image_extract_err"; then
    echo "standalone image extraction should require a prompt" >&2
    exit 1
fi
grep 'combine --input IMAGE with -p' "$image_extract_err" >/dev/null

webm_err="$ROOT/build/webm-input.err"
printf 'RIFFmockWEBM' >"$ROOT/build/not-an-image.webm"
if "$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "describe-image" --input "$ROOT/build/not-an-image.webm" >"$ROOT/build/webm-input.out" 2>"$webm_err"; then
    echo "WebM input should not be classified as an image" >&2
    exit 1
fi
grep 'supported endings' "$webm_err" >/dev/null

legacy_html="$ROOT/build/windows1251-russian.html"
printf '<h1>\317\360\350\342\345\362</h1>' >"$legacy_html"
legacy_err="$ROOT/build/nonutf-html.err"
if "$ROOT/pkchat" --input "$legacy_html" --output-format plaintext --quiet >"$ROOT/build/nonutf-html.out" 2>"$legacy_err"; then
    echo "non-UTF-8 HTML extraction should have failed" >&2
    exit 1
fi
grep 'HTML extraction expects UTF-8 input' "$legacy_err" >/dev/null
grep 'charset conversion is not implemented yet' "$legacy_err" >/dev/null

models=$("$ROOT/pkchat" --list-models "$BASE" --quiet)
test "$models" = "$MODEL"

reply=$("$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "hello")
test "$reply" = "Hello"
auto_model=$("$ROOT/pkchat" "$BASE" --quiet --no-stream -p "model?")

test "$auto_model" = "$MODEL"
json=$("$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "hello" --format json)
printf '%s' "$json" | grep '"content":"Hello"' >/dev/null

html_reply=$("$ROOT/pkchat" "$BASE" --quiet --stream -m "$MODEL" -p "markdown" --output-format html)
printf '%s
' "$html_reply" | grep -F '<h1>Mock Title</h1>' >/dev/null
printf '%s
' "$html_reply" | grep -F '<strong>bold</strong>' >/dev/null
printf '%s
' "$html_reply" | grep -F '<a href="https://example.com/docs">docs</a>' >/dev/null
plain_reply=$("$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "markdown" --output-format plaintext)
printf '%s
' "$plain_reply" | grep -F 'Mock Title' >/dev/null
printf '%s
' "$plain_reply" | grep -F 'Hello bold and docs (https://example.com/docs).' >/dev/null
printf '%s
' "$plain_reply" | grep -F '**bold**' >/dev/null && exit 1 || true
html_file="$ROOT/build/assistant-output.html"
"$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "markdown" --output-format html --output "$html_file"
grep '<!doctype html>' "$html_file" >/dev/null
grep '<meta charset="utf-8">' "$html_file" >/dev/null
grep 'name="viewport"' "$html_file" >/dev/null
grep '<h1>Mock Title</h1>' "$html_file" >/dev/null

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

jsond=$("$ROOT/pkchat" "$BASE" --quiet --stream -m "$MODEL" -p "hello" --output-format jsond)
printf '%s\n' "$jsond" | grep '"event":"delta"' >/dev/null
printf '%s\n' "$jsond" | grep '"event":"done"' >/dev/null

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

COMPACT_CHAT_FILE="$ROOT/build/compact-chat.json"
compacted_reply=$("$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" --load-chat "$CHAT_FILE" \
    -p "count-messages" --context-policy truncate-oldest --max-context-bytes 45 --save-chat "$COMPACT_CHAT_FILE")
test "$compacted_reply" = "messages:1"
grep '"policy":"truncate-oldest"' "$COMPACT_CHAT_FILE" >/dev/null
grep '"messages_compacted":2' "$COMPACT_CHAT_FILE" >/dev/null
grep '"content": "hello"' "$COMPACT_CHAT_FILE" >/dev/null

context_error="$ROOT/build/context-error.err"
if "$ROOT/pkchat" "$BASE" --quiet --no-stream -m "$MODEL" -p "hello" \
    --context-policy error --max-context-bytes 1 >"$ROOT/build/context-error.out" 2>"$context_error"; then
    echo "oversized context with error policy should fail" >&2
    exit 1
fi
grep 'request context is approximately' "$context_error" >/dev/null

REPL_FILE="$ROOT/build/repl-chat.json"
repl_out=$(printf 'repl-one
/quit
' | "$ROOT/pkchat" "$BASE" --quiet --repl --no-stream -m "$MODEL" --save-chat "$REPL_FILE")
test "$repl_out" = "repl-one-reply"
grep 'repl-one' "$REPL_FILE" >/dev/null
grep 'repl-one-reply' "$REPL_FILE" >/dev/null

TUI_FILE="$ROOT/build/tui-insert-chat.json"
python3 "$ROOT/tests/integration/tui_insert_driver.py" \
    "$ROOT/pkchat" "$BASE" "$MODEL" "$insert_file" "$local_png" "$BASE/page" "$TUI_FILE"
grep 'Inserted Context Marker' "$TUI_FILE" >/dev/null
grep 'insert-ok' "$TUI_FILE" >/dev/null
grep 'image-input-ok' "$TUI_FILE" >/dev/null
grep 'Input context from URL' "$TUI_FILE" >/dev/null
grep 'url-context-ok' "$TUI_FILE" >/dev/null
if grep '/quit or /exit' "$TUI_FILE" >/dev/null; then
    echo "TUI help text must not be persisted in chat JSON" >&2
    exit 1
fi


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
