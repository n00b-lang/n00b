#!/bin/sh
set -eu

compiler=$1
positive=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if ! "$compiler" build --output "$tmp/program" "$positive" \
    >"$tmp/positive.out" 2>"$tmp/positive.err"; then
    cat "$tmp/positive.out" "$tmp/positive.err" >&2
    exit 1
fi

if ! "$tmp/program" >"$tmp/runtime.out" 2>"$tmp/runtime.err"; then
    cat "$tmp/runtime.out" "$tmp/runtime.err" >&2
    exit 1
fi
if ! grep -Fq 'ok' "$tmp/runtime.err"; then
    cat "$tmp/runtime.out" "$tmp/runtime.err" >&2
    exit 1
fi
