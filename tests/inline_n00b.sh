#!/bin/sh
set -eu

compiler=$1
programs=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

N00B_PATH="$programs:$programs/../../../lib" \
    "$compiler" --release --output "$tmp/inline" \
    "$programs/inline_main.n" >"$tmp/build.out" 2>"$tmp/build.err"
"$tmp/inline" >"$tmp/stdout" 2>"$tmp/stderr"
test ! -s "$tmp/stdout"
test "$(cat "$tmp/stderr")" = ok

N00B_PATH="$programs:$programs/../../../lib" \
    "$compiler" library --emit-interface "$tmp/inline.n00bi" \
    inline_helper >"$tmp/interface.out" 2>"$tmp/interface.err"
N00B_PATH="$programs:$programs/../../../lib" \
    "$compiler" lower --link-interface "$tmp/inline.n00bi" \
    "$programs/inline_main.n" >"$tmp/lower.out" 2>"$tmp/lower.err"

if N00B_PATH="$programs:$programs/../../../lib" \
    "$compiler" --release --output "$tmp/recursive" \
    "$programs/inline_recursive.n" >"$tmp/recursive.out" \
    2>"$tmp/recursive.err"; then
    exit 1
fi
grep -q 'inline function.*participates in direct or indirect recursion' \
    "$tmp/recursive.err"
