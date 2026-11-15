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
