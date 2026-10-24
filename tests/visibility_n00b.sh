#!/bin/sh
set -eu

compiler=$1
program_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

N00B_PATH="$program_dir" "$compiler" build --release --lto \
    --output "$tmp/default_runtime" "$program_dir/default_runtime.n" \
    >"$tmp/build.out" 2>&1 || {
    cat "$tmp/build.out"
    exit 1
}
"$tmp/default_runtime"

for case in private_type_consumer:T0007 private_field_consumer:L0021 \
    private_type_peer:T0007 private_field_peer:L0021 \
    default_manifest_variable_consumer:T0008 \
    variable_private_consumer:T0009 redundant_private_field:T0068; do
    source=${case%%:*}
    code=${case#*:}
    if N00B_PATH="$program_dir" "$compiler" build \
        --output "$tmp/$source" "$program_dir/$source.n00b" \
        >"$tmp/$source.out" 2>&1; then
        echo "$source unexpectedly built" >&2
        exit 1
    fi
    grep -q "^error\[N00BB-$code\]" "$tmp/$source.out" || {
        cat "$tmp/$source.out"
        exit 1
    }
done
