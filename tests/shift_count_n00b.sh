#!/bin/sh
set -eu

compiler=$1
programs=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if "$compiler" lower "$programs/shift_count_literal_overflow.n00b" \
    >"$tmp/static.out" 2>"$tmp/static.err"; then
    echo 'out-of-range literal shift count was accepted' >&2
    exit 1
fi
grep -q 'shift count is outside the receiver width' "$tmp/static.err"

"$compiler" lower --dump-ir "$programs/shift_count_proven.n00b" \
    >"$tmp/proven.ir" 2>"$tmp/proven.err"
grep -q 'ubitint\[6\] = cast wrap' "$tmp/proven.ir"

"$compiler" lower --dump-ir \
    "$programs/bitcode/bitcode_unsafe_arithmetic.n" \
    >"$tmp/proven_uint.ir" 2>"$tmp/proven_uint.err"
grep -q 'ubitint\[6\] = cast wrap' "$tmp/proven_uint.ir"

if ! "$compiler" build --output "$tmp/overflow" \
    "$programs/bitcode/bitcode_shift_count_overflow.n" \
    >"$tmp/compile.out" 2>"$tmp/compile.err"; then
    cat "$tmp/compile.out" "$tmp/compile.err" >&2
    exit 1
fi
if (ulimit -c 0; "$tmp/overflow" >"$tmp/run.out" 2>"$tmp/run.err") \
    2>"$tmp/shell.err"; then
    echo 'out-of-range dynamic shift count did not trap' >&2
    exit 1
fi
if test -s "$tmp/run.out" \
    || test "$(sed -n 1p "$tmp/run.err")" != 'okpanic: runtime check failed'; then
    cat "$tmp/run.out" "$tmp/run.err" >&2
    exit 1
fi

if ! "$compiler" build --output "$tmp/arithmetic" \
    "$programs/bitcode/bitcode_shift_semantic_overflow.n" \
    >"$tmp/arithmetic_compile.out" 2>"$tmp/arithmetic_compile.err"; then
    cat "$tmp/arithmetic_compile.out" "$tmp/arithmetic_compile.err" >&2
    exit 1
fi
if (ulimit -c 0; "$tmp/arithmetic" >"$tmp/arithmetic.out" \
    2>"$tmp/arithmetic.err") 2>"$tmp/arithmetic_shell.err"; then
    echo 'narrow integer arithmetic did not trap' >&2
    exit 1
fi
if test -s "$tmp/arithmetic.out" || grep -q unexpected \
    "$tmp/arithmetic.err"; then
    cat "$tmp/arithmetic.out" "$tmp/arithmetic.err" >&2
    exit 1
fi
