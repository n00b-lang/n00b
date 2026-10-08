#!/bin/sh
set -eu

compiler=$1
program=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if ! "$compiler" build --output "$tmp/os" "$program" \
    >"$tmp/compile.out" 2>"$tmp/compile.err"; then
    cat "$tmp/compile.out" "$tmp/compile.err" >&2
    exit 1
fi
if ! "$tmp/os" >"$tmp/stdout" 2>"$tmp/stderr"; then
    cat "$tmp/stdout" "$tmp/stderr" >&2
    exit 1
fi
if test -s "$tmp/stdout" || test "$(cat "$tmp/stderr")" != ok; then
    cat "$tmp/stdout" "$tmp/stderr" >&2
    exit 1
fi
