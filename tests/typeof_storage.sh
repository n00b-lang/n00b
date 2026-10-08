#!/bin/sh
set -eu

compiler=$1
program=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$compiler" lower --dump-symbols "$program" >"$tmp/symbols" 2>"$tmp/diagnostics"
if ! grep -q 'probe: variable.*storage-duration=automatic' "$tmp/symbols"; then
    cat "$tmp/symbols" "$tmp/diagnostics" >&2
    exit 1
fi
if grep -q 'probe: variable.*may-escape' "$tmp/symbols"; then
    cat "$tmp/symbols" >&2
    exit 1
fi
