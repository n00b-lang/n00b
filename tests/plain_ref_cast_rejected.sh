#!/bin/sh
set -eu

compiler=$1
program=$2
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT HUP INT TERM

if "$compiler" lower "$program" >"$tmp" 2>&1; then
    echo 'plain cast unexpectedly reinterpreted a byte reference' >&2
    exit 1
fi
grep -q 'this cast has no defined conversion' "$tmp"
