#!/usr/bin/env bash
set -euo pipefail

phase() {
    printf '\n========== %s ==========\n' "$1"
}

phase "environment"
echo "target platform: ${TARGETPLATFORM:-unknown}"
echo "target arch    : ${TARGETARCH:-unknown}"
echo "timestamp (UTC): $(date -u +"%Y-%m-%dT%H:%M:%SZ")"

phase "toolchain"
clang --version
python3 --version
meson --version
ninja --version
x86_64-w64-mingw32-gcc-ar --version | head -n 1
x86_64-w64-mingw32-strip --version | head -n 1

export CC="${CC:-clang}"

phase "windows cross configure and build"
N00B_MESON_CROSS_FILE=meson/cross/windows-x86_64.ini \
N00B_NCC_COMPILER=clang \
N00B_TOOLCHAIN_TARGET=x86_64-w64-windows-gnu \
N00B_CLEAN=1 ./build.sh build_cross_windows_x86_64

phase "artifact type check"
file build_cross_windows_x86_64/test_list.exe

phase "optional smoke"
if command -v wine64 >/dev/null 2>&1; then
    if meson test -C build_cross_windows_x86_64 --print-errorlogs --wrapper wine64 list tuple; then
        echo "wine smoke tests passed"
    else
        echo "wine smoke tests failed (non-blocking)"
    fi
else
    echo "wine64 unavailable, skipping smoke tests"
fi

phase "success"
echo "windows cross configure+compile verification passed"
