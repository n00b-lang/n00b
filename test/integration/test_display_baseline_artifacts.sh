#!/bin/sh

set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <display_baseline_capture> <source_root>" >&2
    exit 1
fi

capture_tool=$1
source_root=$2
tmpdir="${TMPDIR:-/tmp}/n00b-display-baseline-artifacts.$$"

cleanup() {
    rm -rf "$tmpdir"
}

trap cleanup EXIT HUP INT TERM

rm -rf "$tmpdir"
mkdir -p "$tmpdir"

"$capture_tool" --out-dir "$tmpdir"

diff -u "$source_root/plans/artifacts/display-rewrite/m0/scene_stream.txt" \
    "$tmpdir/scene_stream.txt"
diff -u "$source_root/plans/artifacts/display-rewrite/m0/table_stream.txt" \
    "$tmpdir/table_stream.txt"
diff -u "$source_root/plans/artifacts/display-rewrite/m0/metadata.txt" \
    "$tmpdir/metadata.txt"
