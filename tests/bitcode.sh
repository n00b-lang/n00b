#!/bin/sh
set -eu

compiler=$1
program_dir=$2
part=$3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

build_time_failure() {
    name=$1
    panic=$2
    location=$3
    if "$compiler" build --output "$tmp/$name" "$program_dir/$name.n" \
        >"$tmp/$name.out" 2>&1; then
        echo "$name unexpectedly built" >&2
        exit 1
    fi
    grep -q "^panic: $panic" "$tmp/$name.out"
    grep -q "$location" "$tmp/$name.out"
    test ! -e "$tmp/$name"
}

case $part in
runtime)
    if "$compiler" build --native --generic --output "$tmp/conflict" \
        "$program_dir/bitcode_generic.n" >"$tmp/conflict.out" 2>&1; then
        echo 'conflicting CPU options unexpectedly accepted' >&2
        exit 1
    fi
    grep -q -- 'error N00B-B0015: `--native` and `--generic` cannot be used together' \
        "$tmp/conflict.out"
    N00B_PATH="$program_dir" "$compiler" build --generic --verify-analyses \
        --output "$tmp/generic" "$program_dir/bitcode_generic.n"
    "$tmp/generic"
    "$compiler" build --output "$tmp/narrowed_global_field" \
        "$program_dir/bitcode_narrowed_global_field.n"
    "$tmp/narrowed_global_field"
    "$compiler" build --output "$tmp/generic_finalizer_probe" \
        "$program_dir/bitcode_generic_finalizer_probe.n"
    "$tmp/generic_finalizer_probe"
    ;;
litmod)
    N00B_PATH="$program_dir:$program_dir/.." \
        "$compiler" build --output "$tmp/litmod_suite" \
        "$program_dir/bitcode_litmod_suite.n"
    "$tmp/litmod_suite" 2>"$tmp/litmod_declaration.out"
    test "$(cat "$tmp/litmod_declaration.out")" = \
        'hello|world|123|2a|1.5|last'
    ;;
build-time)
    N00B_PATH="$program_dir" "$compiler" build --output "$tmp/importer" \
        "$program_dir/bitcode_importer.n"
    if (ulimit -c 0; "$tmp/importer") >/dev/null 2>&1; then
        echo 'imported initializer was not executed' >&2
        exit 1
    fi
    "$compiler" build --native --output "$tmp/bootstrap_io" \
        "$program_dir/bitcode_bootstrap_io.n" \
        >"$tmp/bootstrap_io_compile"
    test "$(cat "$tmp/bootstrap_io_compile")" = "compile"
    "$tmp/bootstrap_io" >"$tmp/bootstrap_io_runtime"
    test "$(cat "$tmp/bootstrap_io_runtime")" = \
        "runtime0123456789abcdef0123456789abcdef"
    build_time_failure bitcode_litmod_comptime_fail \
        'uncaught failure "prelude failed"' \
        'bitcode_litmod_comptime_fail.n:7:5'
    grep -q 'the comptime run of `bitcode_litmod_comptime_fail` exited' \
        "$tmp/bitcode_litmod_comptime_fail.out"
    build_time_failure bitcode_litmod_fence_nil_fail \
        'uncaught failure "fence failed"' \
        'bitcode_litmod_fence_nil_fail.n:6:1'
    build_time_failure bitcode_litmod_string_fail \
        'uncaught failure "bad literal"' \
        'bitcode_litmod_string_fail.n:7:12'
    build_time_failure bitcode_comptime_fail \
        'requirement `2 + 3 == 6` failed$' \
        '^  --> .*bitcode_comptime_fail.n:2:13$'
    grep -q 'the comptime run of `bitcode_comptime_fail` exited' \
        "$tmp/bitcode_comptime_fail.out"
    "$compiler" build --profile-compile --output "$tmp/build_time_skipped" \
        "$program_dir/bitcode_build_time_skipped.n" \
        2>"$tmp/build_time_skipped.err"
    if grep -q '^PROFILE build-time ' "$tmp/build_time_skipped.err"; then
        echo 'the build-time stage ran without static literals' >&2
        exit 1
    fi
    test "$("$tmp/build_time_skipped")" = 3
    ;;
*)
    echo "unknown part $part" >&2
    exit 2
    ;;
esac
