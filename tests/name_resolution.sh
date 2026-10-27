#!/bin/sh
set -eu

compiler=$1
program_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
export N00B_PATH="$program_dir"

if ! "$compiler" build --output "$tmp/qualified" \
    "$program_dir/qualified.n" >"$tmp/qualified.log" 2>&1; then
    cat "$tmp/qualified.log" >&2
    exit 1
fi
"$tmp/qualified" 2>"$tmp/qualified.out"
grep -Fq ok "$tmp/qualified.out"
grep -Fq 'N00B-N0018: `byte_len` overlaps' "$tmp/qualified.log"

rejects() {
    name=$1
    pattern=$2
    printf '%b' "$3" >"$tmp/$name.n"
    if "$compiler" check "$tmp/$name.n" >"$tmp/$name.log" 2>&1; then
        echo "$name unexpectedly checked" >&2
        exit 1
    fi
    if ! grep -Fq -- "$pattern" "$tmp/$name.log"; then
        echo "$name: expected '$pattern'" >&2
        cat "$tmp/$name.log" >&2
        exit 1
    fi
}

rejects call_conflict N00B-T0013 \
    'import pk\nrequire pk.size("abc") == 3\n'
rejects value_conflict N00B-N0013 \
    'import pk\nrequire pk.limit == 1\n'
rejects clobbered_function N00B-N0012 \
    'let len = 4\nrequire len("abc") == 3\n'
rejects shadowed_type N00B-N0011 \
    'record List {\n    x: int\n}\n'
rejects ambiguous_dispatch "call one directly by its qualified path" \
    'import pk\nlet it: pk.Item = (w = 3)\nrequire it.area() == 1\n'
