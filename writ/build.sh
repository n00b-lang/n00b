#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LANTERN_ROOT="${LANTERN_ROOT:-$HOME/lantern-writ}"
RESHARP_ROOT="${RESHARP_ROOT:-$HOME/resharp-writ}"
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

verify_workspace() {
    local name="$1" root="$2" verified="$3" want have
    want="$(tr -d '[:space:]' < "$ROOT/deps/$(echo "$name" | tr '[:lower:]' '[:upper:]')_REVISION")"
    [ -d "$root" ] || fail "no $name workspace at $root"
    if [ "$verified" = "$want" ]; then
        return 0
    elif command -v jj >/dev/null 2>&1 && [ -d "$root/.jj" ]; then
        have="$(jj -R "$root" log --no-graph -r '@-' -T 'commit_id')"
        [ "$have" = "$want" ] || fail "$name workspace is on $have, pinned revision is $want"
        [ -z "$(jj -R "$root" diff --summary)" ] || fail "$name workspace has local changes"
    else
        [ -f "$root/.writ-pin" ] && [ "$(cat "$root/.writ-pin")" = "$want" ] \
            || fail "$name copy at $root is not marked with the pinned revision"
    fi
}

verify_pin() {
    verify_workspace lantern "$LANTERN_ROOT" "${LANTERN_VERIFIED:-}"
    verify_workspace resharp "$RESHARP_ROOT" "${RESHARP_VERIFIED:-}"
}

build_lantern() {
    local stamp="$LANTERN_PREFIX/.revision"
    local want
    want="$(tr -d '[:space:]' < "$ROOT/deps/LANTERN_REVISION")"
    if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ]; then
        return 0
    fi
    rm -rf "$LANTERN_BUILD" "$LANTERN_PREFIX"
    mkdir -p "$ROOT/deps"
    local log="$ROOT/deps/lantern-build-$PLATFORM_TAG.log"
    local flags=""
    if [ "$(uname -s)" = Linux ]; then
        flags="-D_GNU_SOURCE"
    fi
    if ! { (cd "$LANTERN_ROOT" && CFLAGS="$flags" BUILD_DIR="$LANTERN_BUILD" ./build.sh configure \
                --prefix="$LANTERN_PREFIX" --libdir=lib --buildtype=debugoptimized) \
           && (cd "$LANTERN_ROOT" && BUILD_DIR="$LANTERN_BUILD" ./build.sh compile) \
           && meson install -C "$LANTERN_BUILD" --quiet; } >"$log" 2>&1; then
        tail -40 "$log" >&2
        fail "lantern did not build; the full log is $log"
    fi
    echo "$want" > "$stamp"
}

configure_writ() {
    local sanitize="$1"
    local opts=(-Dllvm_prefix="$LLVM_PREFIX" -Dlantern_prefix="$LANTERN_PREFIX" -Dlantern_root="$LANTERN_ROOT"
                -Dresharp_root="$RESHARP_ROOT")
    if [ "$sanitize" = yes ]; then
        opts+=(-Db_sanitize=address,undefined -Db_lundef=false)
    fi
    local reconfigure=()
    if [ -f "$BUILD_DIR/build.ninja" ]; then
        reconfigure=(--reconfigure)
    fi
    PKG_CONFIG_PATH="$LANTERN_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}" \
        meson setup "${reconfigure[@]+"${reconfigure[@]}"}" "$BUILD_DIR" "$ROOT" "${opts[@]}" \
        | grep -E "ERROR|WARNING" >&2 || true
}

run_linux() {
    local engine
    engine="$(command -v podman || command -v docker)" || fail "no container engine"
    verify_pin
    "$engine" build -q -t writ-linux -f "$ROOT/tools/linux.Containerfile" "$ROOT/tools" >/dev/null
    "$engine" run --rm \
        -v "$ROOT:/work/writ" \
        -v "$LANTERN_ROOT:/work/lantern:ro" \
        -v "$RESHARP_ROOT:/work/resharp:ro" \
        -v writ-cargo:/opt/cargo/registry \
        -e LANTERN_ROOT=/work/lantern \
        -e RESHARP_ROOT=/work/resharp \
        -e LANTERN_VERIFIED="$(tr -d '[:space:]' < "$ROOT/deps/LANTERN_REVISION")" \
        -e RESHARP_VERIFIED="$(tr -d '[:space:]' < "$ROOT/deps/RESHARP_REVISION")" \
        -w /work/writ writ-linux ./build.sh "$@"
}

main() {
    local cmd="${1:-compile}"
    local sanitize=no
    local linux=no
    local args=()
    local forwarded=()
    shift || true
    for a in "$@"; do
        case "$a" in
            --sanitize) sanitize=yes; forwarded+=("$a") ;;
            --linux) linux=yes ;;
            *) args+=("$a"); forwarded+=("$a") ;;
        esac
    done
    if [ "$linux" = yes ]; then
        run_linux "$cmd" "${forwarded[@]+"${forwarded[@]}"}"
        return
    fi
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
