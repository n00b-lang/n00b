#!/bin/sh
set -eu

compiler=$1
program=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

for mode in boxed; do
    flags=
    if test "$mode" = boxed; then flags=--no-specialize; fi
    if ! "$compiler" build $flags --cache "$tmp/cache" \
        --output "$tmp/$mode" "$program" \
        >"$tmp/$mode.compile.out" 2>"$tmp/$mode.compile.err"; then
        cat "$tmp/$mode.compile.out" "$tmp/$mode.compile.err" >&2
        exit 1
    fi
    if ! "$tmp/$mode" >"$tmp/$mode.stdout" 2>"$tmp/$mode.stderr"; then
        echo "$mode run failed" >&2
        cat "$tmp/$mode.stdout" "$tmp/$mode.stderr" >&2
        exit 1
    fi
done
