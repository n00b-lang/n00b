#!/bin/sh
# Assigning literals to module variables, from their own module and from an
# importer: the declaration types the literal (a bounded array's used to be
# an internal compiler error).
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

if "$compiler" check mismatch.n >"$tmp/mismatch.log" 2>&1; then
    echo "mismatch.n checked" >&2
    exit 1
fi
for line in 6 7; do
    grep -q "mismatch.n:$line:.*does not convert to \`u32\`" \
        "$tmp/mismatch.log" || {
        cat "$tmp/mismatch.log" >&2
        exit 1
    }
done
if grep -q 'internal compiler error' "$tmp/mismatch.log"; then
    cat "$tmp/mismatch.log" >&2
    exit 1
fi

"$compiler" build --output "$tmp/short" short.n >"$tmp/short.log" 2>&1 || {
    cat "$tmp/short.log" >&2
    exit 1
}
if "$tmp/short" 2>"$tmp/short.err"; then
    echo "short.n stored a 2-element array into a 4-element variable" >&2
    exit 1
fi
grep -q 'expected 4, found 2' "$tmp/short.err" || {
    cat "$tmp/short.err" >&2
    exit 1
}
