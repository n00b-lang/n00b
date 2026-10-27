#!/bin/sh
set -eu

compiler=$1
program=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if ! "$compiler" build --panic-trap --output "$tmp/trapping" "$program" \
    >"$tmp/trapping.out" 2>&1; then
    cat "$tmp/trapping.out" >&2
    exit 1
fi
status=0
(ulimit -c 0; exec "$tmp/trapping" >"$tmp/stdout" 2>"$tmp/stderr") \
    2>/dev/null || status=$?
trapped=false
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*) test "$status" -eq 3 && trapped=true ;;
*) { test "$status" -eq 133 || test "$status" -eq 132; } && trapped=true ;;
esac
if ! { $trapped \
    && test ! -s "$tmp/stdout" \
    && test "$(sed -n 1,2p "$tmp/stderr")" = \
        "$(printf 'opened\npanic: negative balance')"; }; then
    echo "trapping program exited with status $status" >&2
    cat "$tmp/stdout" "$tmp/stderr" >&2
    exit 1
fi
