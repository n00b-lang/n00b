#!/bin/sh
set -eu

compiler=$1
program_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

# Checks that program bitcode_indirect_global_$1 fails because a call reads
# the global `value` before it is set, the call being described as $2.
expect_uninitialized_read() {
    if "$compiler" check "$program_dir/bitcode_indirect_global_$1.n" \
        >"$tmp/compile.out" 2>"$tmp/compile.err" \
        || ! grep -q "reads global \`value\` before it is guaranteed to be initialized through $2" \
            "$tmp/compile.err"; then
        cat "$tmp/compile.out" "$tmp/compile.err" >&2
        echo "$1: expected a read-before-initialization error" >&2
        exit 1
    fi
}

# A `let` bound to a reference is known to hold its function, so a call
# through it calls the function directly.
expect_uninitialized_read ref_uninitialized "the call to \`read_value\`"
# One held in a `var` isn't.
expect_uninitialized_read var_ref_uninitialized "an indirect call"
