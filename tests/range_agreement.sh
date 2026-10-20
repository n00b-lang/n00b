#!/bin/sh
set -eu

compiler=$1
program=$2
module=$3
minimum=$4
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$compiler" lower --dump-ranges "$program" >"$tmp/output" 2>&1
grep "^range $module " "$tmp/output" >"$tmp/pairs" || true
count=$(wc -l <"$tmp/pairs")
if test "$count" -lt "$minimum"; then
    echo "only $count shared range pairs" >&2
    exit 1
fi
if awk '$5 != $8 || $6 != $9 { bad = 1; print } END { exit !bad }' \
        "$tmp/pairs" >&2; then
    exit 1
fi
