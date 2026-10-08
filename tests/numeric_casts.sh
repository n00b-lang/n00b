#!/bin/sh
set -eu

compiler=$1
valid=$2
invalid=$3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$compiler" build --output "$tmp/valid" "$valid" \
    >"$tmp/compile.out" 2>"$tmp/compile.err"
"$tmp/valid" >"$tmp/stdout" 2>"$tmp/stderr"
test ! -s "$tmp/stdout"
test "$(cat "$tmp/stderr")" = ok

"$compiler" build --output "$tmp/invalid" "$invalid" \
    >"$tmp/compile.out" 2>"$tmp/compile.err"
status=0
(ulimit -c 0; "$tmp/invalid") >"$tmp/stdout" 2>"$tmp/stderr" || status=$?
test "$status" -eq 133
test ! -s "$tmp/stdout"
grep -q '^before' "$tmp/stderr"
if grep -q 'after' "$tmp/stderr"; then
    echo 'out-of-range float-to-integer cast returned' >&2
    exit 1
fi
