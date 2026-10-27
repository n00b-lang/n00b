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
    if "$compiler" check "$name.n00b" >"$tmp/$name.log" 2>&1; then
        echo "$name unexpectedly checked" >&2
        exit 1
    fi
    if ! grep -q -- "$pattern" "$tmp/$name.log"; then
        echo "$name: expected '$pattern'" >&2
        cat "$tmp/$name.log" >&2
        exit 1
    fi
}

fails_with same_module "literal modifier \`'twice\` for quotes literals is already declared in this module"
fails_with builtin_dict "literal modifier \`'dict\` for dict literals is already declared in module \`builtin.containers.dict\`"
fails_with cross_module "literal modifier \`'stamp\` for quotes literals is already declared in module \`provider\`"
fails_with unexported_alias "N00B-N0513"
fails_with alias_only_target "N00B-N0513"

if ! "$compiler" build --output "$tmp/accepted" accepted.n00b \
    >"$tmp/accepted.log" 2>&1; then
    cat "$tmp/accepted.log" >&2
    exit 1
fi
"$tmp/accepted" 2>&1 | grep -q ok
