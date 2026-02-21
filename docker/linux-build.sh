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

echo "=== Building Docker image ($IMAGE_NAME) ==="
# Ensure sdk/ dir exists (Dockerfile COPY requires it, even when empty)
mkdir -p "$SCRIPT_DIR/sdk"
docker build -t "$IMAGE_NAME" "$SCRIPT_DIR"

# The bootstrap installs ncc-bootstrap to $N00B_ROOT/bin/, which would
# clobber the host's macOS binary when bind-mounted.  Copy the source
# tree into the container so host artifacts are untouched.

echo "=== Running build inside container ==="
docker run --rm \
    -v "$PROJECT_ROOT:/src:ro" \
    -e N00B_TEST="${N00B_TEST:-0}" \
    -e N00B_CLEAN=1 \
    -e N00B_NATIVE=1 \
    -e N00B_BUILD_TYPE="${N00B_BUILD_TYPE:-debug}" \
    -e N00B_JOBS="${N00B_JOBS:-2}" \
    -e CC=clang \
    "$IMAGE_NAME" \
    bash -c '
        cp -a /src /build && cd /build
        bash build.sh build_linux
    '
