#!/bin/sh
set -eu

compiler=$1
program=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$compiler" build --output "$tmp/open" "$program" >"$tmp/open.log" 2>&1
if grep -Fq "N00BB-W0011" "$tmp/open.log"; then
    echo "an open build resolved the callback" >&2
    cat "$tmp/open.log" >&2
    exit 1
fi
"$compiler" build --closed-world --output "$tmp/closed" "$program" \
    >"$tmp/closed.log" 2>&1
if ! grep -Fq "bitcode_closed_world_callback.n:15:1" "$tmp/closed.log"; then
    echo "a closed-world build did not resolve the callback" >&2
    cat "$tmp/closed.log" >&2
    exit 1
fi
"$tmp/closed"
