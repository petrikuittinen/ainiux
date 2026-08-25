#!/usr/bin/env bash
set -euo pipefail

if [[ "${OS:-}" != "Windows_NT" ]]; then
    echo "package_windows.sh must run in MSYS2 UCRT64" >&2
    exit 2
fi

binary=${1:-ainiux.exe}
build_dir=${2:-build}
version=$(sed -n 's/.*versionNumber\[\] = "\([^"]*\)".*/\1/p' src/version/version.cpp)
if [[ -z "$version" ]]; then
    echo "could not determine Ainiux version" >&2
    exit 2
fi

package="ainiux-${version}-windows-x86_64"
stage="${build_dir}/package-windows/${package}"
archive="${package}.zip"
rm -rf -- "${build_dir}/package-windows"
mkdir -p "$stage/share/ainiux/prompts" "$stage/share/ainiux/benchmarks" \
    "$stage/docs" "$stage/licenses/third-party"
cp "$binary" "$stage/ainiux.exe"
cp config/ainiux.conf "$stage/share/ainiux/config.conf"
cp config/editor-commands.conf config/themes.conf config/benchmarks.conf config/models.conf \
   config/images.conf \
   docs/editor_help.md "$stage/share/ainiux/"
cp resources/prompts/*.md "$stage/share/ainiux/prompts/"
cp benchmarks/builtin.jsonl benchmarks/long-context.jsonl "$stage/share/ainiux/benchmarks/"
cp -R benchmarks/builtin "$stage/share/ainiux/benchmarks/"
cp LICENSE README.md TESTING.md "$stage/"
cp docs/unicode-license.txt "$stage/licenses/"
cp docs/getting-started.md docs/cli.md docs/chat.md docs/agent.md docs/configuration.md \
   docs/benchmarks.md docs/dired-mode.md docs/security.md docs/windows.md "$stage/docs/"

declare -a queue=("$binary")
declare -A seen=()
declare -A licensed_packages=()
while ((${#queue[@]})); do
    current=${queue[0]}
    queue=("${queue[@]:1}")
    while IFS= read -r dependency; do
        [[ -n "$dependency" ]] || continue
        base=$(basename "$dependency")
        lower=${base,,}
        lower_dependency=${dependency,,}
        case "$lower_dependency" in
            *\\windows\\system32\\*|*\\windows\\syswow64\\*|*/windows/system32/*|*/windows/syswow64/*)
                continue
                ;;
        esac
        case "$lower" in
            kernel32.dll|user32.dll|advapi32.dll|shell32.dll|ole32.dll|ws2_32.dll|bcrypt.dll|ntdll.dll|secur32.dll|crypt32.dll|normaliz.dll|iphlpapi.dll|dnsapi.dll|wldap32.dll)
                continue
                ;;
            msys-2.0.dll)
                echo "native package unexpectedly depends on the MSYS2 POSIX runtime" >&2
                exit 1
                ;;
        esac
        [[ -f "$dependency" ]] || continue
        [[ -z "${seen[$lower]:-}" ]] || continue
        seen[$lower]=1
        cp "$dependency" "$stage/$base"
        queue+=("$dependency")
        owner=$(pacman -Qo "$dependency" 2>/dev/null | awk '{print $(NF-1)}' || true)
        if [[ -n "$owner" && -z "${licensed_packages[$owner]:-}" ]]; then
            licensed_packages[$owner]=1
            while IFS= read -r license; do
                [[ -f "$license" ]] || continue
                relative=${license#*/share/licenses/}
                destination="$stage/licenses/third-party/$relative"
                mkdir -p "$(dirname "$destination")"
                cp "$license" "$destination"
            done < <(pacman -Ql "$owner" 2>/dev/null | awk '$2 ~ /\/share\/licenses\// { print $2 }')
        fi
    done < <(ldd "$current" | sed -nE 's/^[[:space:]]*[^=]+=>[[:space:]]*([^[:space:]]+).*/\1/p; s/^[[:space:]]*(\/[^[:space:]]+\.dll).*/\1/p')
done

rm -f -- "$archive" "$archive.sha256"
(cd "${build_dir}/package-windows" && zip -9 -r "../../$archive" "$package")
sha256sum "$archive" >"$archive.sha256"
echo "Created $archive and $archive.sha256"
