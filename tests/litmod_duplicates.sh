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

builds() {
    name=$1
    if ! "$compiler" build --output "$tmp/$name" "$name.n00b" \
        >"$tmp/$name.log" 2>&1; then
        cat "$tmp/$name.log" >&2
        exit 1
    fi
}

fails_with same_module "literal modifier 'twice' for quotes literals is already declared in this module"
fails_with builtin_dict "literal modifier 'dict' for dict literals is already declared in module 'builtin.containers.dict'"

fails_with cross_module "literal modifier 'stamp' for quotes literals is already declared in module 'provider'"
builds other_kind
"$tmp/other_kind"
builds public_alias
"$tmp/public_alias" 2>&1 | grep -q ok
fails_with unexported_alias "N00BB-T0067"
builds alias_only
"$tmp/alias_only" 2>&1 | grep -q ok
fails_with alias_only_target "N00BB-T0067"
