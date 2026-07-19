#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/ainiux"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/ainiux-index-integration.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

mkdir -p "$WORK/src" "$WORK/build"
printf '%s\n' 'def greet(name):' '    """Return a greeting."""' '    return "Hello " + name' > "$WORK/tool.py"
printf '%s\n' 'struct Point { int x; };' 'int add(int a, int b);' > "$WORK/src/math.h"
printf '%s\n' 'export function start() { return true; }' 'const render = () => { return "ok"; };' > "$WORK/app.js"
printf '%s\n' 'export interface User {' '    id: number;' '}' > "$WORK/models.ts"
printf '%s\n' 'export function App() {' '    return <main>Hello</main>;' '}' > "$WORK/component.jsx"
printf '%s\n' 'export const View = (props: Props) => {' '    return <section>{props.title}</section>;' '};' > "$WORK/view.tsx"
printf '%s\n' 'export function bootModule() { return true; }' > "$WORK/runtime.mts"
printf '%s\n' 'export const loadLegacy = () => true;' > "$WORK/legacy.cts"
printf '%s\n' ':root { --brand: #369; }' '.card { color: var(--brand); }' > "$WORK/theme.css"
printf '%s\n' '<main id="app">' '<style>' '.embedded { color: green; }' '</style>' '<script>' \
    'function embeddedBoot() {}' '</script>' '</main>' > "$WORK/index.html"
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
grep -q 'start' "$WORK/report.md"
grep -q 'User' "$WORK/report.md"
grep -q 'App' "$WORK/report.md"
grep -q 'View' "$WORK/report.md"
grep -q 'bootModule' "$WORK/report.md"
grep -q 'loadLegacy' "$WORK/report.md"
grep -q '\.card' "$WORK/report.md"
grep -q 'embeddedBoot' "$WORK/report.md"
grep -q '| JavaScript | 2 | 5 |' "$WORK/report.md"
grep -q '| TypeScript | 4 | 8 |' "$WORK/report.md"
grep -q '| CSS | 1 | 2 |' "$WORK/report.md"
grep -q '| HTML | 1 | 8 |' "$WORK/report.md"
grep -q '| \*\*All languages\*\* | \*\*11\*\* | \*\*28\*\* | \*\*10\*\* | \*\*1\*\* |' "$WORK/report.md"
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

(cd "$WORK" && "$BIN" --clear-index >"$WORK/clear.stdout" 2>"$WORK/clear.stderr")
test ! -s "$WORK/clear.stdout"
grep -q 'Code index cleared' "$WORK/clear.stderr"
test ! -e "$WORK/.ainiux/index.sqlite"
if (cd "$WORK" && "$BIN" --print-index >/dev/null 2>"$WORK/missing.stderr"); then
    echo 'print-index succeeded after the index was cleared' >&2
    exit 1
fi
grep -q 'no completed code index exists' "$WORK/missing.stderr"
(cd "$WORK" && "$BIN" --clear-index >"$WORK/clear-again.stdout" 2>"$WORK/clear-again.stderr")
test ! -s "$WORK/clear-again.stdout"
grep -q 'already clear' "$WORK/clear-again.stderr"

echo 'code index integration tests passed'
