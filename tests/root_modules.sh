#!/bin/sh
set -eu

compiler=$1
root=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

mkdir "$tmp/elsewhere"
cd "$tmp/elsewhere"
for operand in "$root/main.n00b" main; do
    if ! N00B_PATH="$root" "$compiler" build --output "$tmp/main" "$operand" \
        >"$tmp/build.log" 2>&1; then
        cat "$tmp/build.log" >&2
        exit 1
    fi
    "$tmp/main"
done
