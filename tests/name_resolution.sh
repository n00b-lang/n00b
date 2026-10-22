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

rejects() {
    name=$1
    pattern=$2
    printf '%b' "$3" >"$tmp/$name.n"
    if "$compiler" build --output "$tmp/$name" "$tmp/$name.n" \
        >"$tmp/$name.log" 2>&1; then
        echo "$name unexpectedly built" >&2
        exit 1
    fi
    if ! grep -Fq -- "$pattern" "$tmp/$name.log"; then
        echo "$name: expected '$pattern'" >&2
        cat "$tmp/$name.log" >&2
        exit 1
    fi
}

rejects call_conflict N00BB-T0056 \
    'import pk\nrequire pk.size("abc") == 3\n'
rejects value_conflict N00BB-T0057 \
    'import pk\nrequire pk.limit == 1\n'
rejects clobbered_function N00BB-T0058 \
    'let len = 4\nrequire len("abc") == 3\n'
rejects shadowed_type N00BB-T0059 \
    'record List {\n    x: int\n}\n'
rejects ambiguous_dispatch "call one statically" \
    'import pk\nlet it: pk.Item = (w = 3)\nrequire it.area() == 1\n'

printf '%b' "func len(x: 'T) -> uint {\n    return 9\n}\nrequire len(3) == 9\n" \
    >"$tmp/overlap.n"
if ! "$compiler" build --output "$tmp/overlap" "$tmp/overlap.n" \
    >"$tmp/overlap.log" 2>&1; then
    cat "$tmp/overlap.log" >&2
    exit 1
fi
grep -Fq N00BB-W0019 "$tmp/overlap.log"
