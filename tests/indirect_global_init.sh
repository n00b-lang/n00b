#!/bin/sh
set -eu

compiler=$1
program_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

build() {
    if ! "$compiler" build --output "$tmp/$1" \
        "$program_dir/bitcode_indirect_global_$1.n" \
        >"$tmp/compile.out" 2>"$tmp/compile.err"; then
        cat "$tmp/compile.out" "$tmp/compile.err" >&2
        exit 1
    fi
}

expect_uninitialized_read() {
    build "$1"
    status=0
    "$tmp/$1" >"$tmp/stdout" 2>"$tmp/stderr" || status=$?
    if test "$status" -ne 101 \
        || ! grep -q "global \`value\` is read before it is initialized" \
            "$tmp/stderr"; then
        cat "$tmp/stdout" "$tmp/stderr" >&2
        echo "$1: expected a read-before-initialization failure" >&2
        exit 1
    fi
}

expect_uninitialized_read ref_uninitialized
