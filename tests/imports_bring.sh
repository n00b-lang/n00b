#!/bin/sh
set -eu

compiler=$1
program_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cp "$program_dir"/*.n "$tmp"/

runs() {
    name=$1
    source=$2
    if ! "$compiler" build --output "$tmp/$name" "$source" \
        >"$tmp/$name.log" 2>&1; then
        cat "$tmp/$name.log" >&2
        exit 1
    fi
    "$tmp/$name" 2>"$tmp/$name.out"
    grep -Fq ok "$tmp/$name.out"
}

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

runs forms "$tmp/forms.n"
if grep -Fq N00BB-W0020 "$tmp/forms.log"; then
    echo "unused public functions of imported modules were reported" >&2
    exit 1
fi
printf '%b' 'bring all from shapes\nrequire triangle() == 3 and circle() == 9\n__fd_write(2, "ok")\n' \
    >"$tmp/all.n"
runs all "$tmp/all.n"

rejects brought_path N00BB-T0060 \
    'bring circle from geo\nrequire geo.circle() == 1\n'
rejects renamed_path N00BB-T0060 \
    'import geo as g\nrequire geo.circle() == 1\n'
rejects missing_name N00BB-T0061 'bring missing from geo\n'
rejects import_bring_all "only written 'bring all from geo'" \
    'import geo bring all\n'
rejects private_name N00BB-T0061 'bring secret from geo\n'
rejects duplicate N00BB-T0062 \
    'bring circle from geo\nbring circle from shapes\n'
rejects all_overlap N00BB-T0062 'bring all from geo\nbring all from shapes\n'
rejects local_clash N00BB-T0062 \
    'bring circle from geo\nfunc circle() -> int {\n    return 5\n}\n'
