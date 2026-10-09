#!/bin/sh
set -eu

compiler=$1
program_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if ! "$compiler" build --output "$tmp/initialized" \
    "$program_dir/bitcode_indirect_global_initialized.n" \
    >"$tmp/compile.out" 2>"$tmp/compile.err"; then
    cat "$tmp/compile.out" "$tmp/compile.err" >&2
    exit 1
fi
"$tmp/initialized" >"$tmp/stdout" 2>"$tmp/stderr"
test ! -s "$tmp/stdout"
test "$(cat "$tmp/stderr")" = ok

if ! "$compiler" build --output "$tmp/uninitialized" \
    "$program_dir/bitcode_indirect_global_uninitialized.n" \
    >"$tmp/compile.out" 2>"$tmp/compile.err"; then
    cat "$tmp/compile.out" "$tmp/compile.err" >&2
    exit 1
fi
if (ulimit -c 0; "$tmp/uninitialized") >"$tmp/stdout" 2>"$tmp/stderr"; then
    echo 'indirect read of uninitialized global unexpectedly succeeded' >&2
    exit 1
else
    status=$?
    if test "$status" -lt 128; then
        cat "$tmp/stdout" "$tmp/stderr" >&2
        echo 'indirect read failed without a runtime trap' >&2
        exit 1
    fi
fi

if ! "$compiler" build --output "$tmp/ref_uninitialized" \
    "$program_dir/bitcode_indirect_global_ref_uninitialized.n" \
    >"$tmp/compile.out" 2>"$tmp/compile.err"; then
    cat "$tmp/compile.out" "$tmp/compile.err" >&2
    exit 1
fi
if (ulimit -c 0; "$tmp/ref_uninitialized") >"$tmp/stdout" 2>"$tmp/stderr"; then
    echo 'indirect ref read of uninitialized global unexpectedly succeeded' >&2
    exit 1
else
    status=$?
    if test "$status" -lt 128; then
        cat "$tmp/stdout" "$tmp/stderr" >&2
        echo 'indirect ref read failed without a runtime trap' >&2
        exit 1
    fi
fi
