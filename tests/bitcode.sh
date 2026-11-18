#!/bin/sh
set -eu

# The checks below grep for diagnostic codes, so force them on regardless of
# the caller's environment.
export N00B_DIAGNOSTIC_CODES=1

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

# A literal handler that fails at build time is a source diagnostic at the
# literal, not a panic of the comptime run. Each further argument is a
# `file:line:col: modifier: message` failure that must be reported.
literal_failures() {
    name=$1
    shift
    if "$compiler" build --output "$tmp/$name" "$program_dir/$name.n" \
        >"$tmp/$name.out" 2>&1; then
        echo "$name unexpectedly built" >&2
        exit 1
    fi
    expected=0
    for failure in "$@"; do
        location=${failure%%: *}
        rest=${failure#*: }
        modifier=${rest%%: *}
        message=${rest#*: }
        grep -qF "$location: error N00B-B0116: literal modifier \`'$modifier\` failed: $message" \
            "$tmp/$name.out"
        expected=$((expected + 1))
    done
    test "$(grep -c 'error N00B-B0116' "$tmp/$name.out")" = "$expected"
    if grep -q '^panic:\|comptime run of' "$tmp/$name.out"; then
        echo "$name reported a literal failure as a failed comptime run" >&2
        exit 1
    fi
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
    literal_failures bitcode_litmod_comptime_fail \
        'bitcode_litmod_comptime_fail.n:7:5: noop: prelude failed'
    literal_failures bitcode_litmod_fence_nil_fail \
        'bitcode_litmod_fence_nil_fail.n:6:1: noop: fence failed'
    literal_failures bitcode_litmod_string_fail \
        'bitcode_litmod_string_fail.n:7:12: reject: bad literal'
    too_large='rich string substitution index is too large'
    literal_failures bitcode_litmod_many_fail \
        "bitcode_litmod_many_fail.n:5:25: r: $too_large" \
        'bitcode_litmod_many_fail.n:7:24: reject: bad literal' \
        "bitcode_litmod_many_fail.n:10:12: r: $too_large"
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
