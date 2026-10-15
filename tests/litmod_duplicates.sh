#!/bin/sh
set -eu

compiler=$1
program_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cd "$program_dir"

fails_with() {
    name=$1
    pattern=$2
    if "$compiler" build --output "$tmp/$name" "$name.n00b" \
        >"$tmp/$name.log" 2>&1; then
        echo "$name unexpectedly built" >&2
        exit 1
    fi
    if ! grep -q -- "$pattern" "$tmp/$name.log"; then
        echo "$name: expected '$pattern'" >&2
        cat "$tmp/$name.log" >&2
        exit 1
    fi
}

fails_with cross_module "literal modifier 'stamp' for quotes literals is already declared in module 'provider'"
fails_with same_module "literal modifier 'twice' for quotes literals is already declared in module 'same_module'"
fails_with builtin_dict "literal modifier 'dict' for dict literals is already declared in module 'builtin.containers.dict'"

if ! "$compiler" build --output "$tmp/other_kind" other_kind.n00b \
    >"$tmp/other_kind.log" 2>&1; then
    cat "$tmp/other_kind.log" >&2
    exit 1
fi
"$tmp/other_kind"
