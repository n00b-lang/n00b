#!/bin/sh
set -eu

compiler=$1
program_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cp "$program_dir"/*.n "$tmp"/
cd "$tmp"

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

if ! "$compiler" build --output "$tmp/main" "$tmp/main.n" \
    >"$tmp/main.log" 2>&1; then
    cat "$tmp/main.log" >&2
    exit 1
fi
"$tmp/main" 2>"$tmp/main.out"
grep -Fq ok "$tmp/main.out"
if grep -Eq '`(circle|triangle|rectangle|double)` is never called' \
    "$tmp/main.log"; then
    echo "unused exported functions of imported modules were reported" >&2
    exit 1
fi

rejects brought_path N00B-N0016 \
    'bring circle from geo\nrequire geo.circle() == 1\n'
rejects renamed_path N00B-N0016 \
    'import geo as g\nrequire geo.circle() == 1\n'
rejects missing_name N00B-N0015 'bring missing from geo\n'
rejects import_bring_all 'is written `bring all from geo`' \
    'import geo bring all\n'
rejects private_name N00B-N0015 'bring secret from geo\n'
rejects duplicate N00B-N0014 \
    'bring circle from geo\nbring circle from shapes\n'
rejects all_overlap N00B-N0014 'bring all from geo\nbring all from shapes\n'
rejects local_clash N00B-N0014 \
    'bring circle from geo\nfunc circle() -> int {\n    return 5\n}\n'
