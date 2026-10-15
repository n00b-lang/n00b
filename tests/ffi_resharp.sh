#!/bin/sh
set -eu

compiler=$1
program_dir=$2
contract_dir=$3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cd "$program_dir"

builds() {
    name=$1
    shift
    if ! "$compiler" build --contract-dir "$contract_dir" "$@" \
        --output "$tmp/$name" "$name.n00b" >"$tmp/$name.log" 2>&1; then
        cat "$tmp/$name.log" >&2
        exit 1
    fi
}

builds resharp_match
"$tmp/resharp_match"

builds resharp_leak
"$tmp/resharp_leak" 2>"$tmp/leak.err"
test "$(grep -c 'Regex collected while open; releasing it' "$tmp/leak.err")" = 100

builds resharp_leak --release
mv "$tmp/resharp_leak" "$tmp/resharp_leak_release"
"$tmp/resharp_leak_release" 2>"$tmp/release.err"
test ! -s "$tmp/release.err"
