#!/bin/sh
set -eu

compiler=$1
program_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$compiler" build --output "$tmp/ok" "$program_dir/bitcode_runtime_ok.n00b"
"$tmp/ok"
"$compiler" build --output "$tmp/comptime_ok" \
    "$program_dir/bitcode_comptime_ok.n00b"
"$tmp/comptime_ok"
"$compiler" build --output "$tmp/comptime_scalar_call" \
    "$program_dir/bitcode_comptime_scalar_call.n00b"
"$tmp/comptime_scalar_call"
"$compiler" build --output "$tmp/failable_success" \
    "$program_dir/bitcode_failable_success.n00b"
"$tmp/failable_success"
"$compiler" build --output "$tmp/litmod_static" \
    "$program_dir/../litmod_array_site.n00b"
"$tmp/litmod_static"
"$compiler" build --output "$tmp/litmod_runtime" \
    "$program_dir/bitcode_litmod_runtime.n00b"
"$tmp/litmod_runtime"
"$compiler" build --output "$tmp/litmod_set_shape" \
    "$program_dir/bitcode_litmod_set_shape.n00b"
"$tmp/litmod_set_shape"
"$compiler" build --output "$tmp/litmod_generic_set" \
    "$program_dir/bitcode_litmod_generic_set.n00b"
"$tmp/litmod_generic_set"
"$compiler" build --output "$tmp/litmod_dict_shape" \
    "$program_dir/bitcode_litmod_dict_shape.n00b"
"$tmp/litmod_dict_shape"
"$compiler" build --output "$tmp/litmod_string" \
    "$program_dir/bitcode_litmod_string.n00b"
"$tmp/litmod_string"
"$compiler" build --output "$tmp/litmod_fence" \
    "$program_dir/bitcode_litmod_fence.n00b"
"$tmp/litmod_fence"
"$compiler" build --output "$tmp/litmod_fence_nil" \
    "$program_dir/bitcode_litmod_fence_nil.n00b"
"$tmp/litmod_fence_nil"
"$compiler" build --output "$tmp/litmod_comptime_value" \
    "$program_dir/bitcode_litmod_comptime_value.n00b"
"$tmp/litmod_comptime_value"
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
grep -q 'bitcode_litmod_fence_nil_fail.n00b:9:5' \
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
grep -q 'bitcode_litmod_string_fail.n00b:8:12' \
    "$tmp/litmod_string_fail.out"
test ! -e "$tmp/litmod_string_fail"
"$compiler" build --output "$tmp/value_handles" \
    "$program_dir/bitcode_value_handles.n00b"
"$tmp/value_handles"
if "$compiler" build --output "$tmp/comptime_failed" \
    "$program_dir/bitcode_comptime_fail.n00b" \
    >"$tmp/comptime.out" 2>&1; then
    echo 'failing comptime initializer unexpectedly built' >&2
    exit 1
fi
grep -q 'compile-time execution failed' "$tmp/comptime.out"
test ! -e "$tmp/comptime_failed"
if "$compiler" build --output "$tmp/comptime_state" \
    "$program_dir/bitcode_comptime_state.n00b" \
    >"$tmp/comptime_state.out" 2>&1; then
    echo 'unmaterialized compile-time state unexpectedly built' >&2
    exit 1
fi
grep -q 'cannot materialize compile-time results' "$tmp/comptime_state.out"
test ! -e "$tmp/comptime_state"
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
