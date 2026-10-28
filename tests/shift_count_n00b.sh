#!/bin/sh
set -eu

compiler=$1
programs=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if "$compiler" check "$programs/shift_count_literal_overflow.n" \
    >"$tmp/static.out" 2>"$tmp/static.err"; then
    echo 'out-of-range literal shift count was accepted' >&2
    exit 1
fi
grep -q 'shift count .* is outside 0\.\.' "$tmp/static.err"

"$compiler" lower --dump-ir "$programs/shift_count_proven.n" \
    >"$tmp/proven.ir" 2>"$tmp/proven.err"
grep -q 'ubitint\[6\] = cast wrap' "$tmp/proven.ir"

"$compiler" lower --dump-ir \
    "$programs/bitcode/bitcode_unsafe_arithmetic.n" \
    >"$tmp/proven_uint.ir" 2>"$tmp/proven_uint.err"
grep -q 'ubitint\[6\] = cast wrap' "$tmp/proven_uint.ir"

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
    "$tmp/arithmetic.err" \
    || test "$(sed -n 1p "$tmp/arithmetic.err")" != \
        'panic: `63 + 1` overflows `ubitint[6]`'; then
    cat "$tmp/arithmetic.out" "$tmp/arithmetic.err" >&2
    exit 1
fi
