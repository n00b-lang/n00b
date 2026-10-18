#!/bin/sh
set -eu

compiler=$1
program_dir=$2
contract_dir=$3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cd "$program_dir"
if "$compiler" build --contract-dir "$contract_dir" --output "$tmp/program" \
        load_dynamic.n00b >"$tmp/build.log" 2>&1; then
    echo "a binding with load dynamic was accepted" >&2
    exit 1
fi
if ! grep -q 'error\[E0901\]: not supported yet: `load dynamic`' \
        "$tmp/build.log"; then
    cat "$tmp/build.log" >&2
    exit 1
fi
