#!/usr/bin/env bash
# Build and install Ainiux system-wide (or to a custom PREFIX).
#
# Typical use after cloning the repository:
#   ./scripts/install-deps.sh -y
#   ./scripts/install.sh
#
# Or in one step (Debian/Ubuntu):
#   ./scripts/install.sh --with-deps -y
#
# System install defaults to PREFIX=/usr/local and uses sudo when needed:
#   sudo make install PREFIX=/usr/local
#
# User-local install (no root):
#   ./scripts/install.sh --user
#
# Runtime libraries: libsqlite3 and libcurl (see install-deps.sh / README).

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

PREFIX="${PREFIX:-/usr/local}"
DESTDIR="${DESTDIR:-}"
WITH_DEPS=0
NO_DEPS=0
OPTIMIZED=0
JOBS=""
YES_FLAG=()
USER_INSTALL=0

usage() {
    cat <<'EOF'
Build and install Ainiux.

Usage: install.sh [options]

Options:
  --with-deps        Run scripts/install-deps.sh first (Debian/Ubuntu apt)
  --no-deps          Do not install packages (default unless --with-deps)
  --optimized        Build with `make optimized` (-O3 -DNDEBUG, stripped)
  --user             Install to ~/.local (PREFIX and no sudo for a writable home)
  --prefix DIR       Installation prefix (default: /usr/local or $PREFIX)
  --destdir DIR      Staging root for packaging (default: empty or $DESTDIR)
  -j N, --jobs N     Parallel make jobs
  -y, --yes          Non-interactive apt when used with --with-deps
  -h, --help         Show this help

Examples:
  ./scripts/install.sh --with-deps -y
  ./scripts/install.sh --optimized
  ./scripts/install.sh --user
  sudo ./scripts/install.sh --prefix /usr

After install, verify:
  ainiux --version
  ainiux --provider none --editor
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --with-deps)
            WITH_DEPS=1
            shift
            ;;
        --no-deps)
            NO_DEPS=1
            WITH_DEPS=0
            shift
            ;;
        --optimized)
            OPTIMIZED=1
            shift
            ;;
        --user)
            USER_INSTALL=1
            shift
            ;;
        --prefix)
            PREFIX="${2:?--prefix requires a directory}"
            shift 2
            ;;
        --destdir)
            DESTDIR="${2:?--destdir requires a directory}"
            shift 2
            ;;
        -j|--jobs)
            JOBS="${2:?--jobs requires a number}"
            shift 2
            ;;
        -y|--yes)
            YES_FLAG=(-y)
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

if [ ! -f "${REPO_ROOT}/Makefile" ] || [ ! -f "${REPO_ROOT}/config/ainiux.conf" ]; then
    echo "Could not find the Ainiux repository root (Makefile / config/ainiux.conf)." >&2
    echo "Run this script from a clone of the ainiux source tree." >&2
    exit 1
fi

if [ "${USER_INSTALL}" -eq 1 ]; then
    PREFIX="${HOME}/.local"
fi

cd -- "${REPO_ROOT}"

if [ "${WITH_DEPS}" -eq 1 ] && [ "${NO_DEPS}" -eq 0 ]; then
    echo "==> Installing dependencies"
    "${SCRIPT_DIR}/install-deps.sh" "${YES_FLAG[@]+"${YES_FLAG[@]}"}"
fi

missing=()
command -v g++ >/dev/null 2>&1 || command -v c++ >/dev/null 2>&1 || missing+=("C++17 compiler (g++)")
command -v make >/dev/null 2>&1 || missing+=("make")
command -v pkg-config >/dev/null 2>&1 || missing+=("pkg-config")
if ! pkg-config --exists libcurl 2>/dev/null && ! command -v curl-config >/dev/null 2>&1; then
    missing+=("libcurl development files (libcurl4-openssl-dev)")
fi
if ! pkg-config --exists sqlite3 2>/dev/null; then
    # Header-only check fallback; link still needs -lsqlite3
    if [ ! -f /usr/include/sqlite3.h ] && [ ! -f /usr/local/include/sqlite3.h ]; then
        missing+=("libsqlite3 development files (libsqlite3-dev)")
    fi
fi

if [ "${#missing[@]}" -gt 0 ]; then
    echo "Missing build requirements:" >&2
    for item in "${missing[@]}"; do
        echo "  - ${item}" >&2
    done
    echo >&2
    echo "On Debian/Ubuntu, install them with:" >&2
    echo "  ./scripts/install-deps.sh -y" >&2
    echo "or:" >&2
    echo "  ./scripts/install.sh --with-deps -y" >&2
    exit 1
fi

MAKE_ARGS=()
if [ -n "${JOBS}" ]; then
    MAKE_ARGS+=(-j"${JOBS}")
fi

echo "==> Building Ainiux (PREFIX=${PREFIX})"
if [ "${OPTIMIZED}" -eq 1 ]; then
    make "${MAKE_ARGS[@]+"${MAKE_ARGS[@]}"}" optimized
else
    make "${MAKE_ARGS[@]+"${MAKE_ARGS[@]}"}"
fi

if [ ! -x "${REPO_ROOT}/ainiux" ]; then
    echo "Build finished but ./ainiux was not produced." >&2
    exit 1
fi

echo "==> Built $(./ainiux --version 2>/dev/null || true)"

# The runtime parser intentionally accepts only canonical on/off booleans. Migrate
# a preserved per-user override before installing so an upgrade cannot strand the
# newly installed binary behind legacy true/false or yes/no values.
if [ -z "${DESTDIR}" ]; then
    user_config_root="${XDG_CONFIG_HOME:-${HOME}/.config}/ainiux"
    "${SCRIPT_DIR}/migrate-config-booleans.sh" \
        "${user_config_root}/config.conf" \
        "${user_config_root}/models.conf"
fi

# Decide whether install needs elevation: walk up from PREFIX/bin to the first
# existing ancestor and check write access there (a missing PREFIX is fine if
# its parent is writable, e.g. /tmp/foo when installing with --prefix /tmp/foo).
bin_dir="${DESTDIR}${PREFIX}/bin"
need_root=0
probe="${bin_dir}"
while [ ! -e "${probe}" ]; do
    parent="$(dirname -- "${probe}")"
    if [ "${parent}" = "${probe}" ]; then
        break
    fi
    probe="${parent}"
done
if [ ! -w "${probe}" ] 2>/dev/null; then
    need_root=1
fi

run_make_install() {
    local -a cmd=(make install "PREFIX=${PREFIX}")
    if [ -n "${DESTDIR}" ]; then
        cmd+=("DESTDIR=${DESTDIR}")
    fi
    if [ "${need_root}" -eq 0 ] || [ "$(id -u)" -eq 0 ]; then
        "${cmd[@]}"
    elif command -v sudo >/dev/null 2>&1; then
        echo "==> Elevating privileges for install into ${PREFIX} (sudo)"
        sudo "${cmd[@]}"
    else
        echo "Root privileges are required to install into ${PREFIX}." >&2
        echo "Re-run with sudo, or use a writable prefix:" >&2
        echo "  ./scripts/install.sh --user" >&2
        echo "  ./scripts/install.sh --prefix \"\$HOME/.local\"" >&2
        exit 1
    fi
}

echo "==> Installing to PREFIX=${PREFIX}${DESTDIR:+ DESTDIR=${DESTDIR}}"
run_make_install

installed_bin="${DESTDIR}${PREFIX}/bin/ainiux"
if [ -x "${installed_bin}" ]; then
    echo "==> Installed: ${installed_bin}"
    "${installed_bin}" --version || true
else
    echo "Warning: expected binary not found at ${installed_bin}" >&2
fi

case ":${PATH}:" in
    *":${PREFIX}/bin:"*) ;;
    *)
        echo
        echo "Note: ${PREFIX}/bin is not on your PATH."
        echo "Add it, for example:"
        echo "  export PATH=\"${PREFIX}/bin:\$PATH\""
        ;;
esac

echo
echo "Install complete."
echo "  Smoke test:  ainiux -e"
echo "  Uninstall:   ./scripts/uninstall.sh --prefix ${PREFIX}"
