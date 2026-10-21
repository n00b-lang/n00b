#!/bin/sh
set -eu

compiler=$1
program=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$compiler" build --closed-world --output "$tmp/closed" "$program" \
    >"$tmp/closed.log" 2>&1
for expected in "field 'spare' of 'Point' is never used" \
    "bitcode_unused_fields.n:5:5" \
    "field 'note' of 'Point' is written but never read" \
    "bitcode_unused_fields.n:4:5" \
    "field 'unused' of 'Wire' is never used"; do
    if ! grep -Fq "$expected" "$tmp/closed.log"; then
        echo "missing: $expected" >&2
        cat "$tmp/closed.log" >&2
        exit 1
    fi
done
if [ "$(grep -c "N00BB-W0015" "$tmp/closed.log")" != 3 ]; then
    echo "unexpected unused-field warnings" >&2
    cat "$tmp/closed.log" >&2
    exit 1
fi
"$tmp/closed"
"$compiler" build --output "$tmp/open" "$program" >"$tmp/open.log" 2>&1
if grep -Fq "N00BB-W0015" "$tmp/open.log"; then
    echo "an open build reported unused fields" >&2
    exit 1
fi
