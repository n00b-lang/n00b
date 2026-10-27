#!/bin/sh
set -eu

compiler=$1
program_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cd "$program_dir"
if ! "$compiler" build --lto --output "$tmp/shim_call" shim_call.n00b \
    >"$tmp/shim_call.log" 2>&1; then
    cat "$tmp/shim_call.log" >&2
    exit 1
fi
"$tmp/shim_call"
