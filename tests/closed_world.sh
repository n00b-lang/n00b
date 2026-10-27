#!/bin/sh
set -eu

compiler=$1
program=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$compiler" build --closed-world --output "$tmp/closed" "$program" \
    >"$tmp/closed.log" 2>&1
for expected in "field \`spare\` of \`Point\` is never used" \
    "bitcode_closed_world.n:5:5" \
    "field \`note\` of \`Point\` is written but never read" \
    "bitcode_closed_world.n:4:5" \
    "field \`unused\` of \`Wire\` is never used" \
    "bitcode_closed_world.n:62:1: warning N00B-S0062"; do
    if ! grep -Fq "$expected" "$tmp/closed.log"; then
        echo "missing: $expected" >&2
        cat "$tmp/closed.log" >&2
        exit 1
    fi
done
if [ "$(grep -c "N00B-S0061" "$tmp/closed.log")" != 3 ]; then
    echo "unexpected unused-field warnings" >&2
    cat "$tmp/closed.log" >&2
    exit 1
fi
test "$("$tmp/closed" 2>&1)" = ok
"$compiler" check "$program" >"$tmp/open.log" 2>&1
if grep -Eq "N00B-S006[12]" "$tmp/open.log"; then
    echo "an open check reported closed-world findings" >&2
    cat "$tmp/open.log" >&2
    exit 1
fi
