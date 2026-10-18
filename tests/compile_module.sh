#!/bin/sh
set -eu

compiler=$1
module_dir=$2
module=$3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if ! N00B_PATH="$module_dir" "$compiler" compile --output "$tmp/module.n00bm" \
    "$module" >"$tmp/compile.out" 2>&1; then
    cat "$tmp/compile.out" >&2
    exit 1
fi
test -s "$tmp/module.n00bm"
