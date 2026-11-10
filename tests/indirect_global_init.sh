#!/bin/sh
set -eu

compiler=$1
program_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

expect_uninitialized_read() {
    if "$compiler" check "$program_dir/bitcode_indirect_global_$1.n" \
        >"$tmp/compile.out" 2>"$tmp/compile.err" \
        || ! grep -q "reads global \`value\` before it is guaranteed to be initialized through an indirect call" \
            "$tmp/compile.err"; then
        cat "$tmp/compile.out" "$tmp/compile.err" >&2
        echo "$1: expected a read-before-initialization error" >&2
        exit 1
    fi
}

expect_uninitialized_read ref_uninitialized
