#!/bin/sh
set -eu

compiler=$1
program=$2
shift 2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

# A step that dies without a word (a crash, or an abort on running out of
# memory) is told apart by its exit status, so each failure names it.
status=0
"$compiler" build --output "$tmp/program" "$program" \
    >"$tmp/compile.out" 2>"$tmp/compile.err" || status=$?
if [ "$status" -ne 0 ]; then
    echo "the compile exited with status $status" >&2
    cat "$tmp/compile.out" "$tmp/compile.err" >&2
    exit 1
fi
"$tmp/program" "$@" >"$tmp/stdout" 2>"$tmp/stderr" || status=$?
if [ "$status" -ne 0 ]; then
    echo "the program exited with status $status" >&2
    cat "$tmp/stdout" "$tmp/stderr" >&2
    exit 1
fi
if test -s "$tmp/stdout" || test "$(cat "$tmp/stderr")" != ok; then
    cat "$tmp/stdout" "$tmp/stderr" >&2
    exit 1
fi
