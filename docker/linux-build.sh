#!/usr/bin/env bash
# Build and optionally test n00b inside a Linux Docker container.
#
# Usage:
#   bash docker/linux-build.sh                          # build only
#   N00B_TEST=1 bash docker/linux-build.sh              # build + test
#   N00B_CLEAN=1 N00B_TEST=1 bash docker/linux-build.sh # clean rebuild + test
#   N00B_JOBS=4 bash docker/linux-build.sh              # limit parallelism (default: 2)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="n00b-linux"

# Verify ncc subproject exists.
if [[ ! -f "${PROJECT_ROOT}/subprojects/ncc/meson.build" ]]; then
    echo "ERROR: ncc subproject not found at subprojects/ncc/"
    echo "Run 'bash build.sh' first (it will clone ncc automatically),"
    echo "or manually: git clone https://github.com/crashappsec/ncc.git subprojects/ncc"
    exit 1
fi

echo "=== Building Docker image ($IMAGE_NAME) ==="
# Ensure sdk/ dir exists (Dockerfile COPY requires it, even when empty)
mkdir -p "$SCRIPT_DIR/sdk"
docker build -t "$IMAGE_NAME" "$SCRIPT_DIR"

DOCKER_MEM="${N00B_DOCKER_MEM:-8g}"

echo "=== Running build inside container ==="
docker run --rm \
    --memory="${DOCKER_MEM}" \
    --memory-swap="${DOCKER_MEM}" \
    -v "$PROJECT_ROOT:/src:ro" \
    -e N00B_TEST="${N00B_TEST:-0}" \
    -e N00B_CLEAN=1 \
    -e N00B_NATIVE=1 \
    -e N00B_BUILD_TYPE="${N00B_BUILD_TYPE:-debug}" \
    -e N00B_JOBS="${N00B_JOBS:-2}" \
    -e CC=clang \
    "$IMAGE_NAME" \
    bash -c '
        JOBS="${N00B_JOBS:-2}"

        # Step 1: Build ncc from subproject source and install it.
        echo "--- Building ncc (native Linux) ---"
        cp -a /src/subprojects/ncc /build_ncc && cd /build_ncc
        rm -rf build
        CC=clang meson setup --buildtype=release build .
        meson compile -C build -j "$JOBS"
        cp build/ncc /usr/local/bin/ncc
        cd /

        # Step 2: Build n00b using the freshly-built ncc.
        cp -a /src /build && cd /build
        bash build.sh build_linux
    '
