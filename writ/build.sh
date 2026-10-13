#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LANTERN_ROOT="${LANTERN_ROOT:-$HOME/lantern-writ}"
PLATFORM_TAG="${PLATFORM_TAG:-$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)}"
LANTERN_BUILD="$ROOT/deps/lantern-build-$PLATFORM_TAG"
LANTERN_PREFIX="$ROOT/deps/prefix-$PLATFORM_TAG"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-$PLATFORM_TAG}"
LLVM_PREFIX="${LLVM_PREFIX:-$(brew --prefix llvm 2>/dev/null || echo /usr/lib/llvm-22)}"

if [ "$(uname -s)" = Darwin ] && [ -z "${SDKROOT:-}" ]; then
    SDKROOT="$(xcrun --show-sdk-path)"
    export SDKROOT
fi

export CC="${CC:-$LLVM_PREFIX/bin/clang}"

fail() {
    echo "build.sh: $*" >&2
    exit 1
}

verify_pin() {
    local want have
    want="$(tr -d '[:space:]' < "$ROOT/deps/LANTERN_REVISION")"
    [ -d "$LANTERN_ROOT" ] || fail "no lantern workspace at $LANTERN_ROOT"
    if command -v jj >/dev/null 2>&1 && [ -d "$LANTERN_ROOT/.jj" ]; then
        have="$(jj -R "$LANTERN_ROOT" log --no-graph -r '@-' -T 'commit_id')"
        [ "$have" = "$want" ] || fail "lantern workspace is on $have, pinned revision is $want"
        [ -z "$(jj -R "$LANTERN_ROOT" diff --summary)" ] || fail "lantern workspace has local changes"
    else
        [ -f "$LANTERN_ROOT/.writ-pin" ] && [ "$(cat "$LANTERN_ROOT/.writ-pin")" = "$want" ] \
            || fail "lantern copy at $LANTERN_ROOT is not marked with the pinned revision"
    fi
}

build_lantern() {
    local stamp="$LANTERN_PREFIX/.revision"
    local want
    want="$(tr -d '[:space:]' < "$ROOT/deps/LANTERN_REVISION")"
    if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ]; then
        return 0
    fi
    rm -rf "$LANTERN_BUILD" "$LANTERN_PREFIX"
    (cd "$LANTERN_ROOT" && BUILD_DIR="$LANTERN_BUILD" ./build.sh configure \
        --prefix="$LANTERN_PREFIX" --libdir=lib --buildtype=debugoptimized >/dev/null)
    (cd "$LANTERN_ROOT" && BUILD_DIR="$LANTERN_BUILD" ./build.sh compile >/dev/null)
    meson install -C "$LANTERN_BUILD" --quiet >/dev/null
    echo "$want" > "$stamp"
}

configure_writ() {
    local sanitize="$1"
    local opts=(-Dllvm_prefix="$LLVM_PREFIX" -Dlantern_prefix="$LANTERN_PREFIX" -Dlantern_root="$LANTERN_ROOT")
    if [ "$sanitize" = yes ]; then
        opts+=(-Db_sanitize=address,undefined -Db_lundef=false)
    fi
    if [ ! -f "$BUILD_DIR/build.ninja" ]; then
        PKG_CONFIG_PATH="$LANTERN_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}" \
            meson setup "$BUILD_DIR" "$ROOT" "${opts[@]}" >/dev/null
    fi
}

run_linux() {
    local engine
    engine="$(command -v podman || command -v docker)" || fail "no container engine"
    "$engine" build -q -t writ-linux -f "$ROOT/tools/linux.Containerfile" "$ROOT/tools" >/dev/null
    "$engine" run --rm \
        -v "$ROOT:/work/writ" \
        -v "$LANTERN_ROOT:/work/lantern:ro" \
        -e LANTERN_ROOT=/work/lantern \
        -w /work/writ writ-linux ./build.sh "$@"
}

main() {
    local cmd="${1:-compile}"
    local sanitize=no
    local args=()
    shift || true
    for a in "$@"; do
        case "$a" in
            --sanitize) sanitize=yes ;;
            --linux) run_linux "$cmd" "${args[@]+"${args[@]}"}"; return ;;
            *) args+=("$a") ;;
        esac
    done
    if [ "$sanitize" = yes ]; then
        BUILD_DIR="$BUILD_DIR-sanitize"
    fi
    verify_pin
    build_lantern
    configure_writ "$sanitize"
    case "$cmd" in
        compile) meson compile -C "$BUILD_DIR" ;;
        test) meson compile -C "$BUILD_DIR" && meson test -C "$BUILD_DIR" --print-errorlogs "${args[@]+"${args[@]}"}" ;;
        clean) rm -rf "$BUILD_DIR" ;;
        *) fail "unknown command $cmd" ;;
    esac
}

main "$@"
