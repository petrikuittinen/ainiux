#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/ainiux"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/ainiux-index-integration.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

mkdir -p "$WORK/src" "$WORK/build"
printf '%s\n' 'def greet(name):' '    """Return a greeting."""' '    return "Hello " + name' > "$WORK/tool.py"
printf '%s\n' 'struct Point { int x; };' 'int add(int a, int b);' > "$WORK/src/math.h"
printf '%s\n' 'def ignored():' '    pass' > "$WORK/ignored.py"
printf '%s\n' 'ignored.py' > "$WORK/.gitignore"
printf '%s\n' 'int generated(void);' > "$WORK/build/generated.c"
printf 'int valid;\000binary' > "$WORK/binary.c"

stdout="$WORK/index.stdout"
stderr="$WORK/index.stderr"
(cd "$WORK" && "$BIN" --index-code >"$stdout" 2>"$stderr")
test ! -s "$stdout"
grep -q 'Code index updated' "$stderr"
grep -q 'binary.c' "$stderr"
test -f "$WORK/.ainiux/index.sqlite"

(cd "$WORK" && "$BIN" --print-index >"$WORK/report.md" 2>"$WORK/print.stderr")
grep -q '# ainiux Code Index' "$WORK/report.md"
grep -q 'greet' "$WORK/report.md"
grep -q 'Point' "$WORK/report.md"
if grep -q 'ignored.py' "$WORK/report.md"; then
    echo 'ignored source appeared in code index report' >&2
    exit 1
fi

(cd "$WORK" && "$BIN" --print-index --output saved.md >"$WORK/output.stdout")
test ! -s "$WORK/output.stdout"
test -s "$WORK/saved.md"

printf '%s\n' 'def later():' '    pass' > "$WORK/later.py"
(cd "$WORK" && "$BIN" --print-index >"$WORK/stale.md" 2>"$WORK/stale.stderr")
grep -q 'snapshot is stale' "$WORK/stale.stderr"
grep -q 'New: `later.py`' "$WORK/stale.md"
if grep -q 'function.*later' "$WORK/stale.md"; then
    echo 'stale report unexpectedly indexed a new symbol' >&2
    exit 1
fi

if (cd "$WORK" && "$BIN" --index-code --provider none >/dev/null 2>"$WORK/bad.stderr"); then
    echo 'code index accepted an unrelated provider option' >&2
    exit 1
fi
grep -q 'cannot be combined' "$WORK/bad.stderr"

echo 'code index integration tests passed'
