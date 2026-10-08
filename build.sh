#!/usr/bin/env bash
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
LANTERN_BUILD_DIR="${LANTERN_BUILD_DIR:-$ROOT/../lantern/build}"
MESON_PKG_CONFIG_PATH=""

if [ -z "${LLVM_CONFIG:-}" ] && [ "$(uname -s)" = Darwin ] \
    && command -v brew >/dev/null 2>&1; then
    LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null)" || LLVM_PREFIX=""
    if [ -x "$LLVM_PREFIX/bin/llvm-config" ] \
        && "$LLVM_PREFIX/bin/llvm-config" --libfiles core bitwriter bitreader \
            >/dev/null 2>&1; then
        export LLVM_CONFIG="$LLVM_PREFIX/bin/llvm-config"
    fi
fi

if [ "$(uname -s)" = Darwin ] && [ -z "${SDKROOT:-}" ]; then
    if command -v xcrun >/dev/null 2>&1; then
        SDKROOT="$(xcrun --show-sdk-path)"
        export SDKROOT
    fi
fi

usage() {
    cat <<'USAGE'
Usage: ./build.sh [command] [args]

  (no command)        configure if needed, compile only
  configure [opts]    reconfigure from scratch; opts go to meson setup
  compile [opts]      compile only; opts go to meson compile
  test [names/opts]   compile, then run short tests (or selected short tests)
  test-full [opts]    compile, then run the full regression suite
  bench [opts]        compile, then run the benchmarks and gates
  tu <file.c>         recompile exactly one lantern source file
  clean               remove the build directory

This is the only supported path. It exists so nothing has to reach for
meson or ninja by hand; reaching past it is how flags drift between what a
check runs and what the build runs.

BUILD_DIR overrides the build directory (default: build/).
USAGE
}

ensure_configured() {
    [ -f "$BUILD_DIR/build.ninja" ] && return 0

    meson setup "$BUILD_DIR" "$ROOT"
}

prepare_lantern() {
    BUILD_DIR="$LANTERN_BUILD_DIR" \
        "$ROOT/../lantern/build.sh" compile || return 1
    local lantern_pkg_path="$LANTERN_BUILD_DIR/meson-uninstalled"
    local -a pkg_paths
    local -a unique_paths=("$lantern_pkg_path")
    local pkg_path
    local existing
    local seen
    IFS=: read -r -a pkg_paths <<< "${PKG_CONFIG_PATH:-}"
    for pkg_path in "${pkg_paths[@]}"; do
        [ -n "$pkg_path" ] || continue
        seen=false
        for existing in "${unique_paths[@]}"; do
            if [ "$pkg_path" = "$existing" ]; then
                seen=true
                break
            fi
        done
        [ "$seen" = true ] || unique_paths+=("$pkg_path")
    done
    PKG_CONFIG_PATH="${unique_paths[0]}"
    MESON_PKG_CONFIG_PATH="${unique_paths[0]}"
    for pkg_path in "${unique_paths[@]:1}"; do
        PKG_CONFIG_PATH+=":$pkg_path"
        MESON_PKG_CONFIG_PATH+=",$pkg_path"
    done
    export PKG_CONFIG_PATH
    if [ -f "$BUILD_DIR/build.ninja" ]; then
        meson configure "$BUILD_DIR" \
            -Dpkg_config_path="$MESON_PKG_CONFIG_PATH" || return 1
    fi
}

do_configure() {
    prepare_lantern || return 1
    rm -rf "$BUILD_DIR"
    meson setup "$BUILD_DIR" "$ROOT" \
        -Dpkg_config_path="$MESON_PKG_CONFIG_PATH" "$@"
}

do_compile() {
    prepare_lantern || return 1
    ensure_configured || return 1
    meson compile -C "$BUILD_DIR" "$@"
}

# Recompile one source file with the build's own flags, so a syntax or
# transform error is reported without waiting on the whole tree.
do_tu() {
    local src="$1"

    ensure_configured || return 1

    if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
        echo "no compile_commands.json in $BUILD_DIR" >&2
        exit 1
    fi

    python3 - "$BUILD_DIR/compile_commands.json" "$src" <<'PY'
import json, os, shlex, subprocess, sys

ccjson, want = sys.argv[1], os.path.abspath(sys.argv[2])
entries = json.load(open(ccjson))

for e in entries:
    f = os.path.abspath(os.path.join(e.get("directory", "."), e["file"]))
    if f != want:
        continue
    os.chdir(e.get("directory", "."))
    cmd = e.get("command")
    argv = shlex.split(cmd) if cmd else e["arguments"]
    sys.exit(subprocess.call(argv))

print("not a source file in this build: %s" % want, file=sys.stderr)
sys.exit(1)
PY
}

refuse_stale() {
    echo "lantern: compile failed; refusing to run $1 against a stale" >&2
    echo "         binary. Fix the build and re-run." >&2
    exit 1
}

do_test() {
    do_compile || refuse_stale tests

    meson test -C "$BUILD_DIR" --no-suite regression "$@"
}

do_test_full() {
    do_compile || refuse_stale tests

    meson test -C "$BUILD_DIR" "$@"
}

do_bench() {
    do_compile || refuse_stale benchmarks

    meson test -C "$BUILD_DIR" --benchmark "$@"
}

case "${1:-}" in
    "")
        do_compile
        ;;
    configure)
        shift
        do_configure "$@"
        ;;
    compile)
        shift
        do_compile "$@"
        ;;
    test)
        shift
        do_test "$@"
        ;;
    test-full)
        shift
        do_test_full "$@"
        ;;
    bench)
        shift
        do_bench "$@"
        ;;
    tu)
        shift
        [ $# -ge 1 ] || { usage; exit 1; }
        do_tu "$@"
        ;;
    clean)
        rm -rf "$BUILD_DIR"
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage
        exit 1
        ;;
esac
