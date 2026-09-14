#!/usr/bin/env bash
# Verify that `ainiux` on PATH runs the binary just installed. An existing
# ~/.local/bin copy is a supported legacy/user-install location, so refresh that
# exact regular file atomically when it would otherwise shadow another prefix.

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

install_user_home="${AINIUX_INSTALL_USER_HOME:-${HOME:-}}"
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
    fi
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
