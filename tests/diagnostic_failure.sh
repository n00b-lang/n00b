#!/bin/sh
set -eu

compiler=$1
program=$2
expected=$3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if "$compiler" build --output "$tmp/program" "$program" >"$tmp/output" 2>&1; then
    echo 'invalid program unexpectedly compiled' >&2
    exit 1
fi
if ! grep -Fq "$expected" "$tmp/output"; then
    cat "$tmp/output" >&2
    exit 1
fi
