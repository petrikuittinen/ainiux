#!/usr/bin/env bash
# Remove a system (or custom PREFIX) Ainiux installation created by
# `make install` / scripts/install.sh.
#
# By default keeps administrator templates under SYSCONFDIR/xdg/ainiux.
# Use --purge to remove those as well.
#
# Usage:
#   sudo ./scripts/uninstall.sh
#   ./scripts/uninstall.sh --prefix "$HOME/.local"
#   sudo ./scripts/uninstall.sh --purge

set -euo pipefail

PREFIX="${PREFIX:-/usr/local}"
SYSCONFDIR="${SYSCONFDIR:-/etc}"
DESTDIR="${DESTDIR:-}"
PURGE=0

usage() {
    cat <<'EOF'
Uninstall Ainiux files installed by make install / scripts/install.sh.

Usage: uninstall.sh [options]

Options:
  --prefix DIR       Installation prefix (default: /usr/local or $PREFIX)
  --sysconfdir DIR   System config root (default: /etc or $SYSCONFDIR)
  --destdir DIR      Staging root for packaging (default: empty or $DESTDIR)
  --purge            Also remove SYSCONFDIR/xdg/ainiux templates
  -h, --help         Show this help

Examples:
  sudo ./scripts/uninstall.sh
  ./scripts/uninstall.sh --prefix "$HOME/.local"
  sudo ./scripts/uninstall.sh --purge
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix)
            PREFIX="${2:?--prefix requires a directory}"
            shift 2
            ;;
        --sysconfdir)
            SYSCONFDIR="${2:?--sysconfdir requires a directory}"
            shift 2
            ;;
        --destdir)
            DESTDIR="${2:?--destdir requires a directory}"
            shift 2
            ;;
        --purge)
            PURGE=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

root_path() {
    # Join DESTDIR + absolute path without doubling slashes.
    local abs="$1"
    if [ -z "${DESTDIR}" ]; then
        printf '%s\n' "${abs}"
    else
        printf '%s%s\n' "${DESTDIR%/}" "${abs}"
    fi
}

BIN_PATH="$(root_path "${PREFIX}/bin/ainiux")"
SHARE_DIR="$(root_path "${PREFIX}/share/ainiux")"
XDG_DIR="$(root_path "${SYSCONFDIR}/xdg/ainiux")"

# Elevate only when an existing install path (or its first existing ancestor)
# is not writable by the current user.
need_root=0
for path in "${BIN_PATH}" "${SHARE_DIR}" "${XDG_DIR}"; do
    probe="${path}"
    while [ ! -e "${probe}" ]; do
        parent="$(dirname -- "${probe}")"
        if [ "${parent}" = "${probe}" ]; then
            break
        fi
        probe="${parent}"
    done
    if [ -e "${probe}" ] && [ ! -w "${probe}" ] 2>/dev/null; then
        need_root=1
        break
    fi
done

run_install_cmd() {
    if [ "${need_root}" -eq 0 ] || [ "$(id -u)" -eq 0 ]; then
        "$@"
    elif command -v sudo >/dev/null 2>&1; then
        sudo env DESTDIR="${DESTDIR}" PREFIX="${PREFIX}" SYSCONFDIR="${SYSCONFDIR}" "$@"
    else
        echo "Root privileges are required to uninstall under ${PREFIX} (sudo or root)." >&2
        exit 1
    fi
}

remove_path() {
    local path="$1"
    if [ -e "${path}" ] || [ -L "${path}" ]; then
        echo "Removing ${path}"
        run_install_cmd rm -rf -- "${path}"
    else
        echo "Not present: ${path}"
    fi
}

echo "Uninstalling Ainiux"
echo "  PREFIX=${PREFIX}"
echo "  SYSCONFDIR=${SYSCONFDIR}"
if [ -n "${DESTDIR}" ]; then
    echo "  DESTDIR=${DESTDIR}"
fi

remove_path "${BIN_PATH}"
remove_path "${SHARE_DIR}"

if [ "${PURGE}" -eq 1 ]; then
    remove_path "${XDG_DIR}"
else
    if [ -e "${XDG_DIR}" ]; then
        echo "Keeping system config directory: ${XDG_DIR}"
        echo "  (pass --purge to remove it)"
    fi
fi

echo "Uninstall finished."
