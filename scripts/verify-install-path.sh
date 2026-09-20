#!/usr/bin/env bash
# Verify that `ainiux` on PATH runs the binary just installed. An existing
# ~/.local/bin copy is a supported legacy/user-install location, so refresh that
# exact regular file atomically when it would otherwise shadow another prefix.
# Its adjacent immutable share tree must move with it: otherwise the refreshed
# executable can load stale model/config metadata from ~/.local/share/ainiux.

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "Usage: verify-install-path.sh INSTALLED_BINARY PREFIX" >&2
    exit 2
fi

installed_binary="$1"
installed_prefix="$2"

if [ ! -f "${installed_binary}" ] || [ ! -x "${installed_binary}" ]; then
    echo "Install verification requires an executable regular file: ${installed_binary}" >&2
    exit 1
fi

installed_bin_directory="${installed_prefix%/}/bin"
case ":${PATH}:" in
    *":${installed_bin_directory}:"*) ;;
    *)
        echo "Note: ${installed_bin_directory} is not on PATH; executable shadow verification was skipped."
        exit 0
        ;;
esac

same_binary() {
    local first="$1"
    local second="$2"
    [ "${first}" -ef "${second}" ] || cmp -s "${first}" "${second}"
}

sync_user_local_share() {
    local source_root="${installed_prefix%/}/share/ainiux"
    local target_root="${install_user_home}/.local/share/ainiux"
    local synchronized=0

    if [ ! -d "${source_root}" ] || [ -L "${source_root}" ]; then
        echo "Install verification requires the installed share directory: ${source_root}" >&2
        return 1
    fi
    if [ -e "${target_root}" ] || [ -L "${target_root}" ]; then
        if [ -L "${target_root}" ] || [ ! -d "${target_root}" ]; then
            echo "Install verification refused to replace an unsafe user-local share path: ${target_root}" >&2
            return 1
        fi
    else
        mkdir -p -- "${target_root}"
    fi

    while IFS= read -r -d '' source_file; do
        local relative="${source_file#"${source_root}/"}"
        local relative_directory
        relative_directory="$(dirname -- "${relative}")"
        local target_directory="${target_root}"
        if [ "${relative_directory}" != "." ]; then
            local remainder="${relative_directory}"
            while [ -n "${remainder}" ]; do
                local component="${remainder%%/*}"
                if [ "${component}" = "${remainder}" ]; then
                    remainder=""
                else
                    remainder="${remainder#*/}"
                fi
                target_directory="${target_directory}/${component}"
                if [ -e "${target_directory}" ] || [ -L "${target_directory}" ]; then
                    if [ -L "${target_directory}" ] || [ ! -d "${target_directory}" ]; then
                        echo "Install verification refused to traverse an unsafe user-local share directory: ${target_directory}" >&2
                        return 1
                    fi
                else
                    mkdir -- "${target_directory}"
                fi
            done
        fi

        local target_file="${target_root}/${relative}"
        if [ -L "${target_file}" ]; then
            echo "Install verification refused to replace a user-local share symlink: ${target_file}" >&2
            return 1
        fi
        if [ -e "${target_file}" ] && [ ! -f "${target_file}" ]; then
            echo "Install verification refused to replace a non-regular user-local share path: ${target_file}" >&2
            return 1
        fi
        if [ -f "${target_file}" ] && cmp -s "${source_file}" "${target_file}"; then
            continue
        fi
        if [ ! -w "${target_directory}" ] ||
           { [ -f "${target_file}" ] && [ ! -w "${target_file}" ]; }; then
            echo "A stale user-local share file cannot be refreshed: ${target_file}" >&2
            return 1
        fi
        local temporary_file
        temporary_file="$(mktemp "${target_file}.tmp.XXXXXX")"
        if ! install -m 0644 "${source_file}" "${temporary_file}"; then
            rm -f -- "${temporary_file}"
            return 1
        fi
        if ! mv -f -- "${temporary_file}" "${target_file}"; then
            rm -f -- "${temporary_file}"
            return 1
        fi
        synchronized=$((synchronized + 1))
    done < <(find "${source_root}" -type f -print0)

    echo "==> Synchronized user-local share assets (${synchronized} updated): ${target_root}"
}

install_user_home="${AINIUX_INSTALL_USER_HOME:-${HOME:-}}"
sync_shadow_share=0
if [ -n "${install_user_home}" ]; then
    user_local_binary="${install_user_home}/.local/bin/ainiux"
    if [ -x "${user_local_binary}" ] || [ -L "${user_local_binary}" ]; then
        if [ -L "${user_local_binary}" ]; then
            echo "Install verification refused to replace the shadowing symlink: ${user_local_binary}" >&2
            echo "Remove or retarget it explicitly, then rerun the installer." >&2
            exit 1
        elif [ "${user_local_binary}" -ef "${installed_binary}" ]; then
            :
        elif [ ! -f "${user_local_binary}" ]; then
            echo "Install verification refused to replace a non-regular shadow: ${user_local_binary}" >&2
            exit 1
        elif ! same_binary "${user_local_binary}" "${installed_binary}"; then
            user_local_directory="$(dirname -- "${user_local_binary}")"
            if [ ! -w "${user_local_binary}" ] || [ ! -w "${user_local_directory}" ]; then
                echo "A stale user-local Ainiux shadows the new install but cannot be refreshed: ${user_local_binary}" >&2
                echo "Remove it or make it writable, then rerun the installer." >&2
                exit 1
            fi
            temporary_binary="$(mktemp "${user_local_binary}.tmp.XXXXXX")"
            cleanup_temporary_binary() {
                rm -f -- "${temporary_binary}"
            }
            trap cleanup_temporary_binary EXIT HUP INT TERM
            install -m 0755 "${installed_binary}" "${temporary_binary}"
            mv -f -- "${temporary_binary}" "${user_local_binary}"
            trap - EXIT HUP INT TERM
            echo "==> Refreshed shadowing user-local executable: ${user_local_binary}"
        fi
        if [ ! "${user_local_binary}" -ef "${installed_binary}" ]; then
            sync_shadow_share=1
        fi
    fi
fi

if [ "${sync_shadow_share}" -eq 1 ]; then
    sync_user_local_share
fi

resolved_binary="$(type -P ainiux 2>/dev/null || true)"
if [ -z "${resolved_binary}" ]; then
    echo "Note: no ainiux executable is currently reachable through PATH."
    exit 0
fi

if same_binary "${resolved_binary}" "${installed_binary}"; then
    if [ "${resolved_binary}" -ef "${installed_binary}" ]; then
        echo "==> Verified PATH executable: ${resolved_binary}"
    else
        echo "==> Verified synchronized PATH duplicate: ${resolved_binary}"
    fi
    exit 0
fi

echo "Install verification failed: PATH still resolves ainiux to a different executable." >&2
echo "  Installed: ${installed_binary}" >&2
echo "  PATH uses: ${resolved_binary}" >&2
echo "Move or remove the shadowing executable, or put ${installed_prefix}/bin first on PATH." >&2
echo "Then run 'hash -r' in shells that cache command paths and rerun the installer." >&2
exit 1
