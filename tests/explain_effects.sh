#!/bin/sh
set -eu

compiler=$1
program=$2
expected=$3
names=$4
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if ! "$compiler" check --effects-of "$names" --effects-explain "$program" \
        >"$tmp/actual" 2>"$tmp/errors"; then
    cat "$tmp/errors" >&2
    exit 1
fi
diff -u "$expected" "$tmp/actual" >&2
