#!/bin/sh
set -eu

compiler=$1
program=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if "$compiler" lower "$program" >"$tmp/out" 2>"$tmp/err"; then
    echo 'option negation through an index read was accepted' >&2
    exit 1
fi
if ! grep -q "trait constraint '__not' is not satisfied for int?" "$tmp/err" \
    || ! grep -q 'option_not_through_index.n:9:8' "$tmp/err"; then
    cat "$tmp/err" >&2
    exit 1
fi
