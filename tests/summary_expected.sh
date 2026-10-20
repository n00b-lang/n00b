#!/bin/sh
set -eu

compiler=$1
program=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if ! "$compiler" lower --dump-summaries "$program" >"$tmp/output" 2>&1; then
    cat "$tmp/output" >&2
    exit 1
fi
shift 2
for expected do
    header=${expected%%=>*}
    line=${expected#*=>}
    if ! grep -A6 -Fx "$header" "$tmp/output" | sed '1d;/^summary /,$d' \
            | grep -Fxq "$line"; then
        echo "missing under '$header': $line" >&2
        exit 1
    fi
done
