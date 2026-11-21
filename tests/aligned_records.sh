#!/bin/sh
set -eu

compiler=$1
directory=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cd "$directory"
"$compiler" lower --dump-layout aligned_records.n >"$tmp/layout" 2>/dev/null
for expected in \
    '^layout aligned_types.Context .* size=32 align=16$' \
    '^layout aligned_types.Page .* size=4096 align=4096$' \
    '^layout aligned_types.Holder .* size=48 align=16$' \
    '^  member state bit-offset=128 bit-width=256$'; do
    if ! grep -q "$expected" "$tmp/layout"; then
        echo "no layout line matches: $expected" >&2
        cat "$tmp/layout" >&2
        exit 1
    fi
done

for build in first second; do
    if ! "$compiler" build --output "$tmp/aligned" \
            aligned_records.n >"$tmp/build" 2>&1; then
        echo "$build build failed" >&2
        cat "$tmp/build" >&2
        exit 1
    fi
    if ! "$tmp/aligned" 2>"$tmp/stderr" || test "$(cat "$tmp/stderr")" != ok
    then
        echo "$build build ran wrong" >&2
        cat "$tmp/stderr" >&2
        exit 1
    fi
done
