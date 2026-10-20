#!/bin/sh
set -eu

compiler=$1
program=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$compiler" lower "$program" >/dev/null 2>"$tmp/output"
if ! grep -Fq "global 'bitcode_thread_races.shared' is written by thread entry 'worker'" "$tmp/output" \
    || ! grep -Fq "bitcode_thread_races.n:8:5" "$tmp/output" \
    || [ "$(grep -c "N00BB-W0016" "$tmp/output")" != 1 ]; then
    cat "$tmp/output" >&2
    exit 1
fi
