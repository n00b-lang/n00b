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
    -v "$PROJECT_ROOT/docker/_linux_logs:/logs" \
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

        # Step 2: Build n00b using the freshly-built ncc.  Force the
        # build to use /usr/local/bin/ncc; the cached
        # subprojects/ncc/build_release/ncc is the host (macOS) binary
        # when the developer ran `bash build.sh` before invoking this
        # cross-build, and ELF/Mach-O do not mix.
        cp -a /src /build && cd /build
        rm -rf /build/subprojects/ncc/build_release
        export NCC_PATH=/usr/local/bin/ncc

        # When running tests, start a session DBus + a headless
        # gnome-keyring so test_quic_secret_libsecret can exchange
        # a credential with a real Secret Service daemon.  Without
        # this wrapper the libsecret test would log "SKIP".
        if [ "${N00B_TEST:-0}" = "1" ] && command -v dbus-run-session >/dev/null; then
            exec dbus-run-session -- bash -c "
                # Unlock the keyring with an empty password (headless
                # CI mode).  gnome-keyring-daemon --unlock reads the
                # password from stdin and prints the bus addresses to
                # stdout for export; we ignore them because the
                # SECRETS service registers itself on the session bus
                # which is already set up by dbus-run-session.
                printf \"\\n\" | gnome-keyring-daemon \
                    --start --foreground \
                    --components=secrets >/dev/null 2>&1 &
                # Give the daemon a moment to register on the bus.
                for _ in 1 2 3 4 5; do
                    if dbus-send --session --print-reply \
                        --dest=org.freedesktop.secrets \
                        /org/freedesktop/secrets \
                        org.freedesktop.DBus.Peer.Ping \
                        >/dev/null 2>&1; then break; fi
                    sleep 0.5
                done
                bash build.sh build_linux
                cp -a build_linux/meson-logs /logs/ 2>/dev/null || true
            "
        else
            bash build.sh build_linux
            cp -a build_linux/meson-logs /logs/ 2>/dev/null || true
        fi
    '
