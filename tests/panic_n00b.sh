#!/bin/sh
set -eu

compiler=$1
program=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if ! "$compiler" build --output "$tmp/program" "$program" \
    >"$tmp/compile.out" 2>&1; then
    cat "$tmp/compile.out" >&2
    exit 1
fi
status=0
(ulimit -c 0; exec "$tmp/program" >"$tmp/stdout" 2>"$tmp/stderr") \
    2>/dev/null || status=$?
test "$status" -eq 133
test ! -s "$tmp/stdout"
test "$(sed -n 1,3p "$tmp/stderr")" = \
    "$(printf 'opened\npanic: negative balance\nbacktrace:')"
grep -q '^  at bitcode_panic.__n00bb_runtime_init (.*bitcode_panic.n:7)$' \
    "$tmp/stderr"
test "$(tail -n 1 "$tmp/stderr")" = '  at main'

checked=$3
if ! "$compiler" build --output "$tmp/checked" "$checked" \
    >"$tmp/checked.out" 2>&1; then
    cat "$tmp/checked.out" >&2
    exit 1
fi
status=0
(ulimit -c 0; exec "$tmp/checked" >"$tmp/stdout" 2>"$tmp/stderr") \
    2>/dev/null || status=$?
test "$status" -eq 133
test "$(sed -n 1p "$tmp/stderr")" = "panic: check failed at $checked:2:5"
test "$(tail -n 1 "$tmp/stderr")" = '  at main'
