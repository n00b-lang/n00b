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

export CC="${CC:-clang}"

phase "clean build"
N00B_CLEAN=1 ./build.sh

phase "unit suite"
meson test -C build_debug --print-errorlogs --suite unit

phase "full regression"
N00B_TEST=1 ./build.sh

phase "success"
echo "all required build and test phases passed"
