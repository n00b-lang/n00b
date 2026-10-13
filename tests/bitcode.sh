#!/bin/sh
set -eu

compiler=$1
program_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if "$compiler" build --native --generic --output "$tmp/conflict" \
    "$program_dir/bitcode_runtime_ok.n00b" >"$tmp/conflict.out" 2>&1; then
    echo 'conflicting CPU options unexpectedly accepted' >&2
    exit 1
fi
grep -q -- '--native and --generic are mutually exclusive' "$tmp/conflict.out"
"$compiler" build --generic --output "$tmp/ok" \
    "$program_dir/bitcode_runtime_ok.n00b"
"$tmp/ok"
"$compiler" build --native --output "$tmp/bootstrap_io" \
    "$program_dir/bitcode_bootstrap_io.n00b" \
    >"$tmp/bootstrap_io_compile"
test "$(cat "$tmp/bootstrap_io_compile")" = "compile"
"$tmp/bootstrap_io" >"$tmp/bootstrap_io_runtime"
test "$(cat "$tmp/bootstrap_io_runtime")" = \
    "runtime0123456789abcdef0123456789abcdef"
N00B_PATH="$program_dir:$program_dir/.." \
    "$compiler" build --output "$tmp/litmod_suite" \
    "$program_dir/bitcode_litmod_suite.n00b"
"$tmp/litmod_suite" 2>"$tmp/litmod_declaration.out"
test "$(cat "$tmp/litmod_declaration.out")" = \
    'hello|world|123|2a|1.5|last'
"$compiler" build --output "$tmp/litmod_runtime_trap" \
    "$program_dir/bitcode_litmod_runtime_trap.n00b"
if (ulimit -c 0; "$tmp/litmod_runtime_trap") >/dev/null 2>&1; then
    echo 'uncaught runtime literal failure unexpectedly returned' >&2
    exit 1
fi
if "$compiler" build --output "$tmp/litmod_comptime_fail" \
    "$program_dir/bitcode_litmod_comptime_fail.n00b" \
    >"$tmp/litmod_comptime_fail.out" 2>&1; then
    echo 'failing comptime literal prelude unexpectedly built' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-B0014\]: prelude failed' \
    "$tmp/litmod_comptime_fail.out"
test ! -e "$tmp/litmod_comptime_fail"
if "$compiler" build --output "$tmp/litmod_fence_nil_fail" \
    "$program_dir/bitcode_litmod_fence_nil_fail.n00b" \
    >"$tmp/litmod_fence_nil_fail.out" 2>&1; then
    echo 'failing effect-only literal handler unexpectedly built' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-B0014\]: fence failed' \
    "$tmp/litmod_fence_nil_fail.out"
grep -q 'bitcode_litmod_fence_nil_fail.n00b:7:5' \
    "$tmp/litmod_fence_nil_fail.out"
test ! -e "$tmp/litmod_fence_nil_fail"
if "$compiler" build --output "$tmp/litmod_string_fail" \
    "$program_dir/bitcode_litmod_string_fail.n00b" \
    >"$tmp/litmod_string_fail.out" 2>&1; then
    echo 'failing static literal handler unexpectedly built' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-B0014\]: bad literal' \
    "$tmp/litmod_string_fail.out"
grep -q 'bitcode_litmod_string_fail.n00b:6:12' \
    "$tmp/litmod_string_fail.out"
test ! -e "$tmp/litmod_string_fail"
if "$compiler" build --output "$tmp/comptime_failed" \
    "$program_dir/bitcode_comptime_fail.n00b" \
    >"$tmp/comptime.out" 2>&1; then
    echo 'failing comptime initializer unexpectedly built' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-B0014\]: compile-time execution' \
    "$tmp/comptime.out"
test ! -e "$tmp/comptime_failed"
"$compiler" build --output "$tmp/trap" "$program_dir/bitcode_runtime_trap.n00b"
if (ulimit -c 0; "$tmp/trap") >/dev/null 2>&1; then
    echo 'failing require unexpectedly returned' >&2
    exit 1
fi

N00B_PATH="$program_dir" "$compiler" build --output "$tmp/importer" \
    "$program_dir/bitcode_importer.n00b"
if (ulimit -c 0; "$tmp/importer") >/dev/null 2>&1; then
    echo 'imported initializer was not executed' >&2
    exit 1
fi
