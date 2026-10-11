#!/bin/sh
set -eu

compiler=$1
program_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if "$compiler" lower "$program_dir/bitcode_array_noninteger_index_rejected.n" \
    >"$tmp/invalid.log" 2>&1; then
    echo 'non-integer array index unexpectedly compiled' >&2
    exit 1
fi
grep -Fq "trait constraint '__read' is not satisfied" "$tmp/invalid.log"

"$compiler" build --no-specialize --output "$tmp/high" \
    "$program_dir/bitcode_array_uint_high_trap.n" >"$tmp/high.log" 2>&1
if (ulimit -c 0; "$tmp/high") >/dev/null 2>&1; then
    echo 'oversized unsigned array index did not trap' >&2
    exit 1
fi

"$compiler" build --release --output "$tmp/at-length" \
    "$program_dir/../containers/readiness/array_bounds_runtime_trap.n" \
    >"$tmp/at-length.log" 2>&1
if (ulimit -c 0; "$tmp/at-length") >/dev/null 2>&1; then
    echo 'array index equal to length did not trap' >&2
    exit 1
fi
