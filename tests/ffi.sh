#!/bin/sh
set -eu

compiler=$1
program_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cd "$program_dir"

builds_and_runs() {
    name=$1
    shift
    if ! "$compiler" build "$@" --output "$tmp/$name" "$name.n00b" \
        >"$tmp/$name.log" 2>&1; then
        cat "$tmp/$name.log" >&2
        exit 1
    fi
    "$tmp/$name"
}

fails_with() {
    name=$1
    pattern=$2
    shift 2
    if "$compiler" build "$@" --output "$tmp/$name" "$name.n00b" \
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

builds_and_runs constants
builds_and_runs use_path
builds_and_runs use_search
builds_and_runs package_contracts
builds_and_runs raw_extern
builds_and_runs shim_call
builds_and_runs shim_call --lto

for marker in marker-literal-5be1 marker-contract-5be1; do
    for binary in constants use_path use_search; do
        if grep -q -a "$marker" "$tmp/$binary"; then
            echo "$binary contains $marker" >&2
            exit 1
        fi
    done
done

fails_with missing 'is not on the contract search path'
fails_with extern_hidden "has no visible function named '__abs_raw'"
fails_with malformed 'malformed.n00b:5:30'
fails_with binding_error 'binding_error.n00b:5:5'
grep -q 'E0501' "$tmp/binding_error.log"
fails_with broken_contract 'broken/broken.writ:3:1' --contract-dir broken
test "$(grep -c '^error\[E0001\]' "$tmp/broken_contract.log")" = 1
fails_with local 'a module literal initializes exactly one const'
fails_with not_const 'a module literal initializes exactly one const'
fails_with expression 'a module literal only initializes a module-level const'
fails_with shadow "literal modifier 'ffi' for fence literals is already declared in module 'builtin.ffi'"
fails_with shadow_alias "literal modifier 'ffi' for fence literals is already declared in module 'builtin.ffi'"

mkdir "$tmp/source"
cp use_search.n00b "$tmp/source/edit.n00b"
cp contracts/limits.writ "$tmp/source/limits.writ"
cd "$tmp/source"
builds_and_runs edit --contract-dir .
printf 'library limits\nconst LIMIT: c_int = 41\n' > limits.writ
"$compiler" build --contract-dir . --output "$tmp/edited" edit.n00b \
    >"$tmp/edited.log" 2>&1 || { cat "$tmp/edited.log" >&2; exit 1; }
if "$tmp/edited" 2>/dev/null; then
    echo "edited contract did not change the build" >&2
    exit 1
fi
