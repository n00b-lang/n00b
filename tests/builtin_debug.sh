#!/bin/sh
set -eu

compiler=$1
program=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$compiler" build --output "$tmp/debug" "$program" 2>"$tmp/compile.err"
test "$(head -n 1 "$tmp/compile.err")" = "compile-debug"
"$tmp/debug" 2>"$tmp/runtime.err"
test "$(cat "$tmp/runtime.err")" = "runtime-debug"
