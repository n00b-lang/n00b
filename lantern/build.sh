#!/usr/bin/env bash
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"

usage() {
    cat <<'USAGE'
Usage: ./build.sh [command] [args]

  (no command)        configure if needed, compile, run tests
  configure [opts]    reconfigure from scratch; opts go to meson setup
  compile [opts]      compile only; opts go to meson compile
  test [opts]         compile, then run the test suite
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

do_configure() {
    rm -rf "$BUILD_DIR"
    meson setup "$BUILD_DIR" "$ROOT" "$@"
}

do_compile() {
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

    meson test -C "$BUILD_DIR" "$@"
}

do_bench() {
    do_compile || refuse_stale benchmarks

    meson test -C "$BUILD_DIR" --benchmark "$@"
}

case "${1:-}" in
    "")
        do_test
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
