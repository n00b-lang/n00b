#!/bin/sh
set -eu

compiler=$1
program=$2
preferences_program=$3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if ! "$compiler" lower "$program" >"$tmp/out" 2>"$tmp/err"; then
    cat "$tmp/err" >&2
    exit 1
fi

count=$(grep -c 'warning N00B-P0011:' "$tmp/err")
if [ "$count" -ne 9 ]; then
    cat "$tmp/err" >&2
    echo "expected 9 mixed-syntax warnings, got $count" >&2
    exit 1
fi

for concept in \
    'function declarations' 'logical or' 'logical and' \
    'logical negation' 'the nil value' 'else-if clauses' 'blocks' \
    'record declarations' 'function return types'
do
    grep -q "inconsistent spelling for $concept" "$tmp/err" || {
        cat "$tmp/err" >&2
        echo "missing mixed-syntax warning for $concept" >&2
        exit 1
    }
done

if ! "$compiler" lower "$preferences_program" >"$tmp/out" 2>"$tmp/err"; then
    cat "$tmp/err" >&2
    exit 1
fi
count=$(grep -c 'warning N00B-P0012:' "$tmp/err")
if [ "$count" -ne 9 ]; then
    cat "$tmp/err" >&2
    echo "expected 9 preference warnings, got $count" >&2
    exit 1
fi

set -- \
    --syntax-preference function=fn \
    --syntax-preference 'or=||' \
    --syntax-preference 'and=&&' \
    --syntax-preference not=not \
    --syntax-preference nil=None \
    --syntax-preference else-if=elseif \
    --syntax-preference 'block=:' \
    --syntax-preference record=struct \
    --syntax-preference 'return-type=return:'
if ! "$compiler" "$@" lower "$preferences_program" \
    >"$tmp/out" 2>"$tmp/err"; then
    cat "$tmp/err" >&2
    exit 1
fi
if grep -q 'warning N00B-P0012:' "$tmp/err"; then
    cat "$tmp/err" >&2
    echo "preference overrides did not suppress preference warnings" >&2
    exit 1
fi

if ! "$compiler" --no-syntax-warnings lower "$program" \
    >"$tmp/out" 2>"$tmp/err"; then
    cat "$tmp/err" >&2
    exit 1
fi
if grep -q 'warning N00B-P001[12]:' "$tmp/err"; then
    cat "$tmp/err" >&2
    echo "--no-syntax-warnings did not suppress syntax warnings" >&2
    exit 1
fi

if "$compiler" --syntax-preference function=bogus lower \
    "$preferences_program" >"$tmp/out" 2>"$tmp/err"; then
    echo "invalid syntax preference unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'error N00B-P0033: `--syntax-preference function=bogus`' "$tmp/err" || {
    cat "$tmp/err" >&2
    echo "missing invalid syntax preference diagnostic" >&2
    exit 1
}
