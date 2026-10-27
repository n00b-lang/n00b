#!/bin/sh
set -eu

compiler=$1
program=$2
message=$3
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
if test "$status" -ne 101 || test -s "$tmp/stdout" \
    || test "$(sed -n 1p "$tmp/stderr")" != "panic: $message"; then
    echo "exit status $status" >&2
    cat "$tmp/stdout" "$tmp/stderr" >&2
    exit 1
fi
