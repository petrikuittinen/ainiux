#!/bin/sh
# Migrate preserved Ainiux configuration files to canonical on/off booleans.
# Only known boolean schema fields are changed; unrelated values and reasoning
# vocabulary remain untouched. The runtime parser intentionally stays strict.

set -eu

if [ "$#" -eq 0 ]; then
    echo "Usage: $0 CONFIG_FILE..." >&2
    exit 2
fi

temporary_file=""
cleanup() {
    if [ -n "${temporary_file}" ] && [ -e "${temporary_file}" ]; then
        rm -f -- "${temporary_file}"
    fi
}
trap cleanup EXIT HUP INT TERM

for config_path do
    if [ ! -e "${config_path}" ]; then
        continue
    fi
    if [ ! -f "${config_path}" ]; then
        echo "Cannot migrate non-regular config file: ${config_path}" >&2
        exit 1
    fi

    config_dir=$(dirname -- "${config_path}")
    temporary_file=$(mktemp "${config_dir}/.ainiux-config-migrate.XXXXXX")

    awk '
        function trim(value) {
            sub(/^[ \t]+/, "", value)
            sub(/[ \t]+$/, "", value)
            return value
        }
        function is_boolean_field(section, key, qualified) {
            qualified = section "." key
            return qualified == "generation.stream" ||
                   qualified == "network.insecure_tls" ||
                   qualified == "agent.security_review_log_enabled" ||
                   qualified == "agent.history_backup_enabled" ||
                   qualified == "agent.auto_compact" ||
                   qualified == "agent.show_command_output" ||
                   qualified == "input.auto-convert-html-to-md" ||
                   qualified == "editor.auto-save-mode" ||
                   qualified == "url_fetch.allow_private_addresses" ||
                   qualified == "tui.colors" ||
                   qualified == "tui.highlight" ||
                   qualified == "tui.thinking_traces" ||
                   ((section == "model" || section == "preset") && key == "enabled")
        }
        function canonical_boolean(value) {
            if (value == "true" || value == "yes" || value == "1" || value == "enabled") {
                return "on"
            }
            if (value == "false" || value == "no" || value == "0" || value == "disabled") {
                return "off"
            }
            return ""
        }
        {
            line = $0
            header = trim(line)
            if (header ~ /^\[\[[^]]+\]\]([ \t]*#.*)?$/) {
                sub(/^\[\[/, "", header)
                sub(/\]\].*$/, "", header)
                section = trim(header)
                print line
                next
            }
            if (header ~ /^\[[^]]+\]([ \t]*#.*)?$/) {
                sub(/^\[/, "", header)
                sub(/\].*$/, "", header)
                section = trim(header)
                print line
                next
            }

            equals = index(line, "=")
            if (equals == 0) {
                print line
                next
            }
            key = trim(substr(line, 1, equals - 1))
            if (!is_boolean_field(section, key)) {
                print line
                next
            }

            after_equals = substr(line, equals + 1)
            leading = after_equals
            sub(/[^ \t].*$/, "", leading)
            body = substr(after_equals, length(leading) + 1)
            if (!match(body, /^[^ \t#]+/)) {
                print line
                next
            }
            token = substr(body, RSTART, RLENGTH)
            replacement = canonical_boolean(token)
            if (replacement == "") {
                print line
                next
            }
            suffix = substr(body, RLENGTH + 1)
            print substr(line, 1, equals) leading replacement suffix
        }
    ' "${config_path}" >"${temporary_file}"

    if cmp -s -- "${config_path}" "${temporary_file}"; then
        rm -f -- "${temporary_file}"
        temporary_file=""
        continue
    fi

    config_mode=$(stat -c '%a' "${config_path}" 2>/dev/null || stat -f '%Lp' "${config_path}")
    chmod "${config_mode}" "${temporary_file}"
    if [ "$(id -u)" -eq 0 ]; then
        config_owner=$(stat -c '%u:%g' "${config_path}" 2>/dev/null || stat -f '%u:%g' "${config_path}")
        chown "${config_owner}" "${temporary_file}"
    fi
    mv -f -- "${temporary_file}" "${config_path}"
    temporary_file=""
    echo "Migrated legacy boolean values to on/off: ${config_path}"
done
