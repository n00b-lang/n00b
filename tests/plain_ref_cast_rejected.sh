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
grep -q 'cannot cast Ref\[byte\] to Ref\[uint\]: no conversion is defined between them; reinterpreting a reference needs `unsafe_cast`' "$tmp"
