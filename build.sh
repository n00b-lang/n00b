#!/usr/bin/env bash
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

usage() {
    cat <<'USAGE'
Usage: ./build.sh [command] [args]

  (no command)        configure if needed, compile
  configure [VAR=x]   reconfigure; BUILD_TYPE=debug|debugoptimized|release|
                      sanitize, LLVM_CONFIG=path, N00B_RUST=1
  compile             compile only
  test [names]        compile, then run the dev tier (run after every change;
                      budget in tests/budget), or the named tests
  test-pr             compile, then run the pr tier (run before raising a PR)
  test-all            compile, then run both tiers
  test-lantern        compile, then run lantern's tests
  test-writ           compile, then run writ's tests
  bench               compile, then run lantern's benchmarks and gates
  perf [opts] [names] compile, then run the performance suite in tests/perf
                      (--baseline REV|COMPILER|RESULTS.json, --runs N,
                      --threshold PCT); fails on regressions past threshold.
                      --languages runs tests/perf/xlang with n00b and C at
                      -O0 and -Ofast, plus Go and Python (--skip c|go|python)
  tu <file.c>         recompile exactly one source file
  install [opts]      compile, then install a toolchain and make it active
                      (--prefix DIR, --link, --add-to-path, --no-modify-path)
  uninstall [opts]    remove every installed toolchain (--prefix DIR)
  linux [--arch A] <command>
                      run any of these commands in a Linux container;
                      A is x86_64 (default) or arm64, or set LINUX_ARCH
  clean               remove the build directory

This wraps make; ./build.sh and make produce the same build. BUILD_DIR
overrides the build directory (default: build/), JOBS the parallelism.
USAGE
}

fail() {
    printf 'build.sh: error: %s\n' "$1" >&2
    exit 1
}

usage_error() {
    printf 'build.sh: error: %s\n' "$1" >&2
    printf "Run './build.sh help' for the commands.\n" >&2
    exit 2
}

run_make() {
    make -C "$ROOT" --no-print-directory -j"$JOBS" B="$BUILD_DIR" "$@"
}

check_test_names() {
    local known name
    known="$(awk -F'\t' '!/^#/ && NF { print $1 }' \
        "$ROOT/tests/n00b.tests" "$ROOT/lantern/tests/lantern.tests" \
        "$ROOT/writ/tests/writ.tests")"
    for name in "$@"; do
        printf '%s\n' "$known" | grep -qxF -- "$name" ||
            usage_error "no test named '$name'; test names are the first column of tests/n00b.tests, lantern/tests/lantern.tests and writ/tests/writ.tests"
    done
}

config_matches() {
    local config="$BUILD_DIR/config.mk"
    [ -f "$config" ] || return 0
    grep -qx "N00B_RUST := ${N00B_RUST:-0}" "$config"
}

do_configure() {
    local assignment
    for assignment in "$@"; do
        case "$assignment" in
            *=*) export "${assignment?}" ;;
            *) usage_error "configure takes VAR=value settings, not '$assignment'" ;;
        esac
    done
    rm -f "$BUILD_DIR/config.mk"
    run_make "$BUILD_DIR/config.mk"
}

do_tu() {
    local file="${1#"$ROOT"/}"
    local object
    case "$file" in
        lantern/*) object="$BUILD_DIR/lantern/obj/${file%.c}.o" ;;
        writ/*) object="$BUILD_DIR/writ/obj/${file%.c}.o" ;;
        *) object="$BUILD_DIR/obj/${file%.c}.o" ;;
    esac
    case "$file" in
        *.c) ;;
        *) usage_error "tu takes a C source file, not '$1'" ;;
    esac
    [ -f "$ROOT/$file" ] || fail "no source file '$file' in $ROOT"
    rm -f "$object"
    run_make "$object"
}

do_linux() {
    local arch="${LINUX_ARCH:-x86_64}"
    if [ "${1:-}" = "--arch" ]; then
        [ $# -ge 2 ] || usage_error "--arch needs x86_64 or arm64"
        arch="$2"
        shift 2
    fi
    local platform
    case "$arch" in
        x86_64|amd64) arch=x86_64 platform=amd64 ;;
        arm64|aarch64) arch=arm64 platform=arm64 ;;
        *) usage_error "unknown Linux architecture '$arch'; use x86_64 or arm64" ;;
    esac
    [ $# -ge 1 ] || usage_error "linux needs a command to run, such as 'test'"
    local engine
    engine="$(command -v podman || command -v docker)" ||
        fail "linux needs podman or docker on PATH"
    local native emulated=0
    native="$("$engine" info --format '{{.Host.Arch}}' 2>/dev/null)"
    [ "$native" = "$platform" ] || emulated=1
    local scale="${N00B_TEST_BUDGET_SCALE:-$((emulated ? 24 : 2))}"
    local cores host_cores
    cores="$("$engine" info --format '{{.Host.CPUs}}' 2>/dev/null)"
    host_cores="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
    [ "${cores:-0}" -gt 0 ] 2>/dev/null || cores="$host_cores"
    local wall_scale="${N00B_WALL_BUDGET_SCALE:-$((scale * ((host_cores + cores - 1) / cores)))}"
    local rust=()
    if [ "${N00B_RUST:-0}" = 1 ]; then
        rust=(-v "${RESHARP_ROOT:-$HOME/resharp-writ}:/work/resharp-writ:ro"
              -e RESHARP_ROOT=/work/resharp-writ)
    fi
    "$engine" build -q --platform "linux/$platform" -t "n00bb-linux-$arch" \
        -f "$ROOT/tools/linux.Dockerfile" "$ROOT/tools" >/dev/null || exit 1
    "$engine" run --rm --platform "linux/$platform" \
        -v "$ROOT:/work/n00bb" \
        -e BUILD_DIR="/work/n00bb/build-linux-$arch" \
        -e N00B_EMULATED="$emulated" \
        -e N00B_RUST="${N00B_RUST:-0}" \
        -e N00B_TEST_BUDGET_SCALE="$scale" \
        -e N00B_WALL_BUDGET_SCALE="$wall_scale" \
        "${rust[@]+"${rust[@]}"}" \
        -w /work/n00bb "n00bb-linux-$arch" ./build.sh "$@"
}

config_matches || rm -f "$BUILD_DIR/config.mk"

case "${1:-}" in
    ""|compile) run_make all ;;
    configure) shift; do_configure "$@" ;;
    test)
        shift
        if [ "$#" -gt 0 ]; then
            check_test_names "$@"
            run_make test T="$*"
        else run_make test; fi ;;
    test-pr|test-all|test-lantern|test-writ|bench) run_make "$1" ;;
    perf)
        shift
        run_make all >/dev/null && python3 "$ROOT/tools/perf.py" --source "$ROOT" \
            --build "$BUILD_DIR" "$@" ;;
    tu)
        shift
        [ $# -eq 1 ] || usage_error "tu takes exactly one C source file"
        do_tu "$1" ;;
    install) shift; run_make install INSTALL_FLAGS="$*" ;;
    uninstall) shift; "$ROOT/tools/install.sh" --uninstall "$@" ;;
    linux) shift; do_linux "$@" ;;
    clean) rm -rf "$BUILD_DIR" ;;
    -h|--help|help) usage ;;
    *) usage_error "unknown command '$1'" ;;
esac
