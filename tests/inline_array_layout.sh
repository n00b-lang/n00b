#!/bin/sh
set -eu

compiler=$1
source=$2
output=$(mktemp)
trap 'rm -f "$output"' EXIT HUP INT TERM

"$compiler" lower --dump-layout "$source" >"$output"
grep -q '^layout inline_array_record_layout.Block .* size=24 align=8$' "$output"
grep -q '^  member marks bit-offset=0 bit-width=32$' "$output"
grep -q '^  member bits bit-offset=64 bit-width=128$' "$output"
grep -q '^layout inline_array_record_layout.Plain .* align=8$' "$output"
grep -q '^  member marks bit-offset=128 bit-width=32$' "$output"
grep -q '^  member bits bit-offset=0 bit-width=128$' "$output"
