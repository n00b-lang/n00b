#!/bin/sh
set -eu

compiler=$1
program_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

N00B_PATH="$program_dir" "$compiler" build \
    --output "$tmp/default_runtime" "$program_dir/default_runtime.n" \
    >"$tmp/build.out" 2>&1 || {
    cat "$tmp/build.out"
    exit 1
}
"$tmp/default_runtime"

for case in private_type_consumer:N0004 private_field_consumer:N0006 \
    private_type_peer:N0004 private_field_peer:N0006 \
    default_manifest_variable_consumer:N0004 \
    variable_private_consumer:N0004 redundant_private_field:N0510; do
    source=${case%%:*}
    code=${case#*:}
    if N00B_PATH="$program_dir" "$compiler" check \
        "$program_dir/$source.n" >"$tmp/$source.out" 2>&1; then
        echo "$source unexpectedly checked" >&2
        exit 1
    fi
    grep -q "error N00B-$code:" "$tmp/$source.out" || {
        cat "$tmp/$source.out"
        exit 1
    }
done
