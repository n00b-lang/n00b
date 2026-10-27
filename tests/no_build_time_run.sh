#!/bin/sh
set -eu

compiler=$1
program=$2
runs=${3:-1}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

while test "$runs" -gt 0; do
    runs=$((runs - 1))
    if ! N00B_PATH="$(dirname "$program")" "$compiler" build --build-time \
        --output "$tmp/build_time" "$program" >"$tmp/compile.out" 2>&1; then
        cat "$tmp/compile.out" >&2
        exit 1
    fi
    if test -s "$tmp/build_time"; then
        echo 'program needs a build-time run' >&2
        exit 1
    fi
done
