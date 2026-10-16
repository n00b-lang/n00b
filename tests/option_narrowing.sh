#!/bin/sh
set -eu

compiler=$1
programs=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

for program in assigned modified referenced through_ref wrong_branch \
    promoted_wrong_branch; do
    if "$compiler" lower "$programs/$program.n" >"$tmp/$program.out" \
        2>"$tmp/$program.err"; then
        echo "$program: unsound option narrowing was accepted" >&2
        exit 1
    fi
    if ! grep -q "an option's value requires it to be proven non-nil" \
        "$tmp/$program.err"; then
        cat "$tmp/$program.err" >&2
        exit 1
    fi
done
