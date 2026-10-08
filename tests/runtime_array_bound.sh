#!/bin/sh
set -eu

output=$(mktemp)
trap 'rm -f "$output"' EXIT HUP INT TERM

"$1" lower "$2" >"$output"
grep -q 'array.new %__n00bb_array_extent_' "$output"
grep -q 'sequence.shape.check .*dim0=runtime:value=' "$output"
! grep -q '^  static func @__n00bb_array_extent_' "$output"
