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

LANG_WORK="$WORK/languages"
mkdir -p "$LANG_WORK"
printf '%s\n' '# Indexed Markdown' > "$LANG_WORK/README.md"
printf '%s\n' 'class IndexedCpp {};' > "$LANG_WORK/model.cpp"
printf '%s\n' 'class IndexedCSharp {}' > "$LANG_WORK/model.cs"
printf '%s\n' 'class IndexedJava {}' > "$LANG_WORK/Model.java"
printf '%s\n' '<main id="indexed-xhtml"><script>function notIndexedHere() {}</script></main>' > "$LANG_WORK/page.xhtml"
printf '%s\n' '<schema><element name="IndexedXml"/></schema>' > "$LANG_WORK/schema.xml"
printf '%s\n' '{"indexedJson": true}' > "$LANG_WORK/data.json"
printf '%s\n' 'indexed_bash() { :; }' > "$LANG_WORK/script.sh"
printf '%s\n' '<?php' 'function indexed_php() {}' > "$LANG_WORK/plugin.php"
printf '%s\n' 'sub indexed_perl {}' > "$LANG_WORK/module.pm"
printf '%s\n' 'def indexed_ruby' 'end' > "$LANG_WORK/worker.rb"
printf '%s\n' 'pub fn indexed_rust() {}' > "$LANG_WORK/lib.rs"
printf '%s\n' 'package demo' 'func indexedGo() {}' > "$LANG_WORK/main.go"
printf '%s\n' 'function Indexed-PowerShell {}' > "$LANG_WORK/script.ps1"
printf '%s\n' '.type indexed_assembly, @function' 'indexed_assembly:' > "$LANG_WORK/start.s"
printf '%s\n' 'CREATE TABLE indexed_sql (id INTEGER);' > "$LANG_WORK/schema.sql"
printf '%s\n' '[indexedToml]' 'enabled = true' > "$LANG_WORK/config.toml"
printf '%s\n' 'indexedYaml:' '  enabled: true' > "$LANG_WORK/config.yaml"
printf '%s\n' '[indexedIni]' 'enabled = true' > "$LANG_WORK/config.ini"
(cd "$LANG_WORK" && "$BIN" --index-code --print-index >"$LANG_WORK/report.md" 2>"$LANG_WORK/index.stderr")
for language in Markdown 'C++' 'C#' Java HTML-only XML JSON Bash PHP Perl Ruby Rust Go PowerShell Assembly SQL TOML YAML INI; do
    grep -Fq "| $language |" "$LANG_WORK/report.md"
done
for symbol in 'Indexed Markdown' IndexedCpp IndexedCSharp IndexedJava indexed-xhtml IndexedXml \
    indexedJson indexed_bash indexed_php indexed_perl indexed_ruby indexed_rust indexedGo \
    Indexed-PowerShell indexed_assembly indexed_sql indexedToml indexedYaml indexedIni; do
    grep -Fq "$symbol" "$LANG_WORK/report.md"
done
if grep -Fq 'notIndexedHere' "$LANG_WORK/report.md"; then
    echo 'HTML-only indexing unexpectedly scanned embedded JavaScript' >&2
    exit 1
fi

echo 'code index integration tests passed'
