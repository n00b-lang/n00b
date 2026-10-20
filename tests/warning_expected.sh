#!/bin/sh
set -eu

compiler=$1
program=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if ! "$compiler" lower "$program" >/dev/null 2>"$tmp/output"; then
    cat "$tmp/output" >&2
    exit 1
fi
shift 2
for expected do
    if ! grep -Fq "$expected" "$tmp/output"; then
        echo "missing: $expected" >&2
        cat "$tmp/output" >&2
        exit 1
    fi
done
