#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: run_enospc_test.sh CXX POSIX_IO_MOCK TEST_BINARY" >&2
    exit 2
fi

cxx=$1
mock=$2
binary=$3

if [ ! -r "$mock" ] || [ ! -x "$binary" ]; then
    echo "ENOSPC test launcher: mock or test binary is missing" >&2
    exit 2
fi

preload=$mock
sanitized=false
if command -v readelf >/dev/null 2>&1; then
    if readelf -d "$binary" 2>/dev/null | grep -Eq 'libasan|libclang_rt[^]]*asan'; then
        sanitized=true
    fi
elif grep -aEq 'libasan|libclang_rt.*asan|__asan_init' "$binary" 2>/dev/null; then
    sanitized=true
fi

if [ "$sanitized" = true ]; then
    asan=$($cxx -print-file-name=libasan.so 2>/dev/null || true)
    if [ "$asan" = "libasan.so" ] || [ ! -r "$asan" ]; then
        runtime_dir=$($cxx --print-runtime-dir 2>/dev/null || true)
        asan=
        if [ -n "$runtime_dir" ] && [ -d "$runtime_dir" ]; then
            for candidate in "$runtime_dir"/libclang_rt.asan-*.so; do
                if [ -r "$candidate" ]; then
                    asan=$candidate
                    break
                fi
            done
        fi
    fi
    if [ -z "${asan:-}" ] || [ ! -r "$asan" ]; then
        echo "ENOSPC test launcher: sanitized binary detected, but $cxx could not resolve a readable ASan runtime" >&2
        echo "This sanitizer/toolchain combination is unsupported for the LD_PRELOAD fault test." >&2
        exit 2
    fi
    preload=$asan:$mock
fi

PKCHAT_MOCK_ENOSPC=1 LD_PRELOAD=$preload exec "$binary" --enospc
