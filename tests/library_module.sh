#!/bin/sh
set -eu

# Builds MODULE as a library and checks the ABI metadata was written.
compiler=$1
module_dir=$2
module=$3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if ! N00B_PATH="$module_dir" "$compiler" library --output "$tmp/module.n00bm" \
    "$module" >"$tmp/library.out" 2>&1; then
    cat "$tmp/library.out" >&2
    exit 1
fi
test -s "$tmp/module.n00bm"
