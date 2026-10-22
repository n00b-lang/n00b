#!/bin/sh
set -eu

compiler=$1
program=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if ! "$compiler" build --verify-analyses --output "$tmp/program" "$program" \
    >"$tmp/compile.out" 2>"$tmp/compile.err"; then
    cat "$tmp/compile.out" "$tmp/compile.err" >&2
    exit 1
fi
