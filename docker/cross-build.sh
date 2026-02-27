#!/usr/bin/env bash
# Cross-compile n00b inside a Docker container for a non-native target.
#
# Usage:
#   bash docker/cross-build.sh macos-arm64       # default on macOS
#   bash docker/cross-build.sh macos-x86_64
#   bash docker/cross-build.sh windows-x86_64
#
# Environment:
#   N00B_TEST=1       Run tests after build (only works when target matches host)
#   N00B_CLEAN=1      Force clean rebuild
#   N00B_BUILD_TYPE   debug (default) or release
#   N00B_JOBS         Parallelism limit (default: 2)
#
# This script is invoked either:
#   1. From the host — launches Docker and runs itself inside the container
#   2. Inside the container (CROSS_INSIDE_CONTAINER=1) — does the actual build
#
# NOTE: The host-side path must work with bash 3.2 (macOS default).
# Associative arrays (bash 4+) are only used inside the container (Ubuntu).
set -euo pipefail

KNOWN_TARGETS="macos-arm64 macos-x86_64 windows-x86_64"

# ── Parse arguments ──────────────────────────────────────────────────────────

TARGET="${1:-}"
if [[ -z "$TARGET" ]]; then
    echo "Usage: $0 <target>"
    echo ""
    echo "Available targets:"
    echo "  macos-arm64       macOS Apple Silicon (requires osxcross + SDK)"
    echo "  macos-x86_64      macOS Intel (requires osxcross + SDK)"
    echo "  windows-x86_64    Windows x86_64 (mingw-w64)"
    exit 1
fi

# Validate target (bash 3.2 compatible)
_valid=false
for _t in $KNOWN_TARGETS; do
    if [[ "$_t" == "$TARGET" ]]; then _valid=true; break; fi
done
if [[ "$_valid" != "true" ]]; then
    echo "ERROR: Unknown target '$TARGET'"
    echo "Available targets: $KNOWN_TARGETS"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="n00b-linux"
BUILD_TYPE="${N00B_BUILD_TYPE:-debug}"
OUTPUT_DIR="${PROJECT_ROOT}/build_cross_${TARGET}"

# ── Inside container: do the actual build ────────────────────────────────────

if [[ "${CROSS_INSIDE_CONTAINER:-0}" == "1" ]]; then
    # Inside Docker (Ubuntu, bash 5+) — associative arrays are fine here.
    declare -A TARGET_SYSTEM TARGET_CPU_FAMILY TARGET_CPU TARGET_CC TARGET_AR TARGET_STRIP TARGET_EXTRA_BINARIES TARGET_NEEDS_WRAPPER

    TARGET_SYSTEM[macos-arm64]="darwin"
    TARGET_CPU_FAMILY[macos-arm64]="aarch64"
    TARGET_CPU[macos-arm64]="arm64"
    TARGET_CC[macos-arm64]="arm64-apple-darwin-clang"
    TARGET_AR[macos-arm64]="arm64-apple-darwin-ar"
    TARGET_STRIP[macos-arm64]="arm64-apple-darwin-strip"
    TARGET_EXTRA_BINARIES[macos-arm64]=""
    TARGET_NEEDS_WRAPPER[macos-arm64]="true"

    TARGET_SYSTEM[macos-x86_64]="darwin"
    TARGET_CPU_FAMILY[macos-x86_64]="x86_64"
    TARGET_CPU[macos-x86_64]="x86_64"
    TARGET_CC[macos-x86_64]="x86_64-apple-darwin-clang"
    TARGET_AR[macos-x86_64]="x86_64-apple-darwin-ar"
    TARGET_STRIP[macos-x86_64]="x86_64-apple-darwin-strip"
    TARGET_EXTRA_BINARIES[macos-x86_64]=""
    TARGET_NEEDS_WRAPPER[macos-x86_64]="true"

    TARGET_SYSTEM[windows-x86_64]="windows"
    TARGET_CPU_FAMILY[windows-x86_64]="x86_64"
    TARGET_CPU[windows-x86_64]="x86_64"
    TARGET_CC[windows-x86_64]="mingw-clang-wrapper"
    TARGET_AR[windows-x86_64]="x86_64-w64-mingw32-ar"
    TARGET_STRIP[windows-x86_64]="x86_64-w64-mingw32-strip"
    TARGET_EXTRA_BINARIES[windows-x86_64]="windres = '/usr/bin/x86_64-w64-mingw32-windres'"
    TARGET_NEEDS_WRAPPER[windows-x86_64]="true"

    # Create a clang wrapper for Windows cross-compilation so that
    # clang (not GCC) is used with the correct --target flag.
    if [[ "$TARGET" == windows-* ]]; then
        cat > /usr/local/bin/mingw-clang-wrapper <<'WRAPEOF'
#!/bin/sh
exec clang --target=x86_64-w64-mingw32 -fuse-ld=lld "$@"
WRAPEOF
        chmod +x /usr/local/bin/mingw-clang-wrapper
    fi

    # We're inside the Docker container.
    # /src is the read-only mount of the project, /output is the output volume.
    # Copy source to a writable location.
    cp -a /src /build
    cd /build

    CROSS_CC="${TARGET_CC[$TARGET]}"

    # Verify the cross-compiler is available
    if [[ "$TARGET" == macos-* ]]; then
        # osxcross compilers live in /usr/local/osxcross/bin/
        CROSS_CC_PATH="/usr/local/osxcross/bin/${CROSS_CC}"
        CROSS_AR_PATH="/usr/local/osxcross/bin/${TARGET_AR[$TARGET]}"
        CROSS_STRIP_PATH="/usr/local/osxcross/bin/${TARGET_STRIP[$TARGET]}"
    else
        CROSS_CC_PATH="$(command -v "$CROSS_CC" 2>/dev/null || echo "/usr/bin/$CROSS_CC")"
        CROSS_AR_PATH="$(command -v "${TARGET_AR[$TARGET]}" 2>/dev/null || echo "/usr/bin/${TARGET_AR[$TARGET]}")"
        CROSS_STRIP_PATH="$(command -v "${TARGET_STRIP[$TARGET]}" 2>/dev/null || echo "/usr/bin/${TARGET_STRIP[$TARGET]}")"
    fi

    if [[ ! -x "$CROSS_CC_PATH" ]]; then
        echo "ERROR: Cross-compiler not found: ${CROSS_CC_PATH}"
        if [[ "$TARGET" == macos-* ]]; then
            echo "Hint: Rebuild the Docker image with --build-arg MACOS_SDK=..."
            echo "See: bash docker/prepare-sdk.sh --help"
        fi
        exit 1
    fi

    echo "=== Cross-compiling for ${TARGET} ==="
    echo "  Cross-compiler: ${CROSS_CC_PATH}"

    # Step 1: Build ncc natively with Linux clang
    echo "--- Building ncc (native Linux) ---"
    export CC=clang
    cd /build/ncc
    if [[ -d build_ncc ]]; then
        rm -rf build_ncc
    fi
    meson setup --buildtype="${BUILD_TYPE}" \
        -Dcc_path=clang \
        --prefix=/build --bindir=/build/bin \
        build_ncc .
    meson compile -C build_ncc
    meson install -C build_ncc
    cd /build

    # Step 2: Generate cross-file
    #
    # The key trick: meson's [binaries] c = ncc, but ncc delegates to
    # NCC_COMPILER (the actual cross-compiler). This way ncc's AST
    # transformations run on the Linux host, and the code generation
    # targets the cross platform.
    CROSS_FILE="/tmp/cross-${TARGET}.ini"
    cat > "$CROSS_FILE" <<CROSSEOF
[binaries]
c = '/build/bin/ncc'
ar = '${CROSS_AR_PATH}'
strip = '${CROSS_STRIP_PATH}'
${TARGET_EXTRA_BINARIES[$TARGET]}

[built-in options]
c_args = []
c_link_args = []

[properties]
needs_exe_wrapper = ${TARGET_NEEDS_WRAPPER[$TARGET]}

[host_machine]
system = '${TARGET_SYSTEM[$TARGET]}'
cpu_family = '${TARGET_CPU_FAMILY[$TARGET]}'
cpu = '${TARGET_CPU[$TARGET]}'
endian = 'little'
CROSSEOF

    echo "--- Generated cross-file: ---"
    cat "$CROSS_FILE"
    echo "---"

    # Step 3: meson setup + compile
    echo "--- Configuring cross-build ---"
    JOBS="${N00B_JOBS:-2}"
    BUILD_DIR="build_cross_${TARGET}"

    if [[ -d "$BUILD_DIR" ]]; then
        rm -rf "$BUILD_DIR"
    fi

    # NCC_COMPILER tells ncc which actual compiler to invoke
    export NCC_COMPILER="${CROSS_CC_PATH}"
    CC=/build/bin/ncc \
    meson setup \
        --cross-file "$CROSS_FILE" \
        --buildtype="${BUILD_TYPE}" \
        -Dusing_build_script=true \
        "$BUILD_DIR" .

    echo "--- Compiling ---"
    meson compile -C "$BUILD_DIR" -j "$JOBS"

    # Step 4: Copy artifacts to /output
    echo "--- Copying artifacts to /output ---"
    # Copy the whole build directory's outputs
    cp -a "$BUILD_DIR" /output/
    # Also copy ncc for reference
    mkdir -p /output/bin
    cp /build/bin/ncc /output/bin/

    echo "=== Cross-build complete for ${TARGET} ==="
    exit 0
fi

# ── On the host: launch Docker ───────────────────────────────────────────────

# Ensure the Docker image exists
if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
    echo "Docker image '$IMAGE_NAME' not found. Building..."
    # Ensure sdk/ directory exists for COPY
    mkdir -p "${SCRIPT_DIR}/sdk"
    docker build -t "$IMAGE_NAME" "$SCRIPT_DIR"
fi

# For macOS targets, verify osxcross is in the image
if [[ "$TARGET" == macos-* ]]; then
    if ! docker run --rm "$IMAGE_NAME" \
            test -d /usr/local/osxcross/bin 2>/dev/null; then
        echo "ERROR: Docker image does not have osxcross installed."
        echo ""
        echo "To enable macOS cross-compilation:"
        echo "  1. bash docker/prepare-sdk.sh /path/to/MacOSX*.sdk.tar.xz"
        echo "  2. docker build -t n00b-linux --build-arg MACOS_SDK=<filename> docker/"
        exit 1
    fi
fi

# Prepare output directory
if [[ "${N00B_CLEAN:-0}" == "1" ]] && [[ -d "$OUTPUT_DIR" ]]; then
    rm -rf "$OUTPUT_DIR"
fi
mkdir -p "$OUTPUT_DIR"

echo "=== Docker cross-compilation: ${TARGET} ==="

DOCKER_MEM="${N00B_DOCKER_MEM:-6g}"

docker run --rm \
    --memory="${DOCKER_MEM}" \
    --memory-swap="${DOCKER_MEM}" \
    -v "${PROJECT_ROOT}:/src:ro" \
    -v "${OUTPUT_DIR}:/output" \
    -e CROSS_INSIDE_CONTAINER=1 \
    -e N00B_BUILD_TYPE="${BUILD_TYPE}" \
    -e N00B_JOBS="${N00B_JOBS:-2}" \
    "$IMAGE_NAME" \
    bash /src/docker/cross-build.sh "$TARGET"

echo "=== Artifacts in: ${OUTPUT_DIR}/ ==="

# ── Run tests natively if possible ───────────────────────────────────────────

if [[ "${N00B_TEST:-0}" != "0" ]]; then
    HOST_OS="$(uname -s)"
    HOST_ARCH="$(uname -m)"

    CAN_TEST=false
    if [[ "$HOST_OS" == "Darwin" ]] && [[ "$TARGET" == macos-* ]]; then
        # macOS can run both arm64 and x86_64 (via Rosetta) natively
        CAN_TEST=true
    elif [[ "$HOST_OS" == "Linux" ]] && [[ "$TARGET" == linux-* ]]; then
        # Could test if arch matches
        if [[ "$HOST_ARCH" == "x86_64" ]] && [[ "$TARGET" == "linux-x86_64" ]]; then
            CAN_TEST=true
        elif [[ "$HOST_ARCH" == "aarch64" ]] && [[ "$TARGET" == "linux-aarch64" ]]; then
            CAN_TEST=true
        fi
    fi

    if [[ "$CAN_TEST" == "true" ]]; then
        echo "=== Running tests natively ==="
        # The cross-build output mirrors the meson build directory structure.
        # Use meson test pointed at the output directory.
        CROSS_BUILD="${OUTPUT_DIR}/build_cross_${TARGET}"
        if [[ -d "$CROSS_BUILD" ]]; then
            meson test -C "$CROSS_BUILD" --print-errorlogs
            if [[ $? -ne 0 ]]; then
                echo "Tests failed."
                exit 1
            fi
        else
            echo "WARNING: Build directory not found at ${CROSS_BUILD}"
            echo "Test executables may be directly in ${OUTPUT_DIR}/"
        fi
    else
        echo "=== Skipping tests (cannot run ${TARGET} binaries on ${HOST_OS}/${HOST_ARCH}) ==="
    fi
fi
