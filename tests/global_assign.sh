#!/bin/sh
# Assigning literals to module variables, from their own module and from an
# importer: the declaration types the literal (a bounded array's used to be
# an internal compiler error). A literal whose length differs from a
# declared size is a compile-time error.
set -eu

compiler=$1
program_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cd "$program_dir"

"$compiler" build --output "$tmp/main" main.n >"$tmp/main.log" 2>&1 || {
    cat "$tmp/main.log" >&2
    exit 1
}
"$tmp/main"

rejected() {
    if "$compiler" check "$1.n" >"$tmp/$1.log" 2>&1; then
        echo "$1.n checked" >&2
        exit 1
    fi
    if grep -q 'internal compiler error' "$tmp/$1.log"; then
        cat "$tmp/$1.log" >&2
        exit 1
    fi
}

reported() {
    grep -q -- "$2" "$tmp/$1.log" || {
        echo "$1.n: expected '$2'" >&2
        cat "$tmp/$1.log" >&2
        exit 1
    }
}

rejected mismatch
for line in 6 7; do
    reported mismatch "mismatch.n:$line:.*does not convert to \`u32\`"
done

# A global's initializer, assignments to a global and a local, a local's
# initializer and an argument.
rejected lengths
for at in 3:28:.*length\ is\ 2 11:13:.*length\ is\ 2 12:32:.*length\ is\ 5 \
    13:13:.*length\ is\ 1 14:19:.*length\ is\ 3; do
    reported lengths "lengths.n:$at, but it is stored where the declared size is 4"
done
test "$(grep -c 'declared size is' "$tmp/lengths.log")" = 5

# A record literal and field assignments, on a local and on another
# module's variable.
rejected fields
for at in 7:50:.*length\ is\ 2 8:20:.*length\ is\ 4 9:27:.*length\ is\ 1; do
    reported fields "fields.n:$at, but it is stored where the declared size is 3"
done
test "$(grep -c 'declared size is' "$tmp/fields.log")" = 3

# A `modify` or `out` parameter declared with a size works on the caller's
# elements in place.
"$compiler" build --output "$tmp/references" references.n \
    >"$tmp/references.log" 2>&1 || {
    cat "$tmp/references.log" >&2
    exit 1
}
"$tmp/references"

"$compiler" build --output "$tmp/inline_tables" inline_tables.n \
    >"$tmp/inline_tables.log" 2>&1 || {
    cat "$tmp/inline_tables.log" >&2
    exit 1
}
"$tmp/inline_tables"

"$compiler" build --output "$tmp/inline_records" inline_records.n \
    >"$tmp/inline_records.log" 2>&1 || {
    cat "$tmp/inline_records.log" >&2
    exit 1
}
"$tmp/inline_records"

"$compiler" build --output "$tmp/rectangular" rectangular.n \
    >"$tmp/rectangular.log" 2>&1 || {
    cat "$tmp/rectangular.log" >&2
    exit 1
}
"$tmp/rectangular"

rejected rectangular_errors
reported rectangular_errors "rectangular_errors.n:10:11:.*\`grid\` is declared with size 3x4, but the parameter it is passed to by reference has size 4x3"

rejected reference_errors
reported reference_errors "reference_errors.n:18:12:.*\`table\` is declared with size 4, so it can only be passed by reference to a parameter declared with that size"
reported reference_errors "reference_errors.n:20:10:.*works on an array of size 4 in place, but \`loose\` isn't declared with that size"
reported reference_errors "reference_errors.n:21:11:.*\`table\` is declared with size 4, but the parameter it is passed to by reference has size 3"

rejected reference_fields
reported reference_fields "reference_fields.n:28:5:.*\`values\` is declared with size 3, so it can only be passed by reference"
reported reference_fields "reference_fields.n:29:5:.*\`values\` is declared with size 3, but the parameter it is passed to by reference has size 4"
reported reference_fields "reference_fields.n:30:43:.*\`fixed\` takes a fixed-size array by reference, so it can only be called directly"
