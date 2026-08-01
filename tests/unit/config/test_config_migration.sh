#!/bin/sh

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf -- "${test_dir}"' EXIT HUP INT TERM

config_file="${test_dir}/config.conf"
models_file="${test_dir}/models.conf"

printf '%s\n' \
    '[generation]' \
    '# retained comment' \
    'stream = true' \
    '[network]' \
    'insecure_tls=false' \
    '[agent]' \
    'auto_compact = yes' \
    'show_command_output = disabled' \
    '[tui]' \
    'colors = 0' \
    'theme = off' \
    '[reasoning]' \
    'effort = disabled' >"${config_file}"

printf '%s\n' \
    '[model]' \
    'id = "example"' \
    'enabled = true' \
    'reasoning = ["none", "enabled", "disabled"]' \
    '[preset]' \
    'name = "coding"' \
    'enabled = false' >"${models_file}"

"${repo_root}/scripts/migrate-config-booleans.sh" "${config_file}" "${models_file}" >/dev/null

grep -Fq '# retained comment' "${config_file}"
grep -Fq 'stream = on' "${config_file}"
grep -Fq 'insecure_tls=off' "${config_file}"
grep -Fq 'auto_compact = on' "${config_file}"
grep -Fq 'show_command_output = off' "${config_file}"
grep -Fq 'colors = off' "${config_file}"
grep -Fq 'theme = off' "${config_file}"
grep -Fq 'effort = disabled' "${config_file}"
grep -Fq 'enabled = on' "${models_file}"
grep -Fq 'reasoning = ["none", "enabled", "disabled"]' "${models_file}"
grep -Fq 'enabled = off' "${models_file}"

# A second run must not change an already-migrated file.
cp "${config_file}" "${config_file}.before"
cp "${models_file}" "${models_file}.before"
"${repo_root}/scripts/migrate-config-booleans.sh" "${config_file}" "${models_file}" >/dev/null
cmp -s "${config_file}.before" "${config_file}"
cmp -s "${models_file}.before" "${models_file}"
