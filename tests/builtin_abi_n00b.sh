#!/bin/sh
set -eu

compiler=$1
program=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$compiler" build --output "$tmp/abi" "$program" \
    >"$tmp/compile.out" 2>"$tmp/compile.err"
"$tmp/abi" >"$tmp/stdout" 2>"$tmp/stderr"
test ! -s "$tmp/stdout"
test ! -s "$tmp/stderr"
