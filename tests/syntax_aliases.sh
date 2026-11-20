#!/bin/sh
set -eu
# The checks below match diagnostic codes, whatever the caller's environment.
export N00B_DIAGNOSTIC_CODES=1

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
if [ "$count" -ne 10 ]; then
    cat "$tmp/err" >&2
    echo "expected 10 mixed-syntax warnings, got $count" >&2
    exit 1
fi

for concept in \
    'function declarations' 'logical or' 'logical and' \
    'logical negation' 'the nil value' 'else-if clauses' 'blocks' \
    'record declarations' 'function return types' \
    'keyword parameters in function types'
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
if [ "$count" -ne 10 ]; then
    cat "$tmp/err" >&2
    echo "expected 10 preference warnings, got $count" >&2
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
    --syntax-preference 'return-type=return:' \
    --syntax-preference 'keyword-type=**name:'
# Both programs lowered above; what the runs below test is which syntax
# warnings appear, which `check` reports exactly as `lower` does (and at a
# tenth of the cost, since `lower` compiles the library from source).
if ! "$compiler" "$@" check "$preferences_program" \
    >"$tmp/out" 2>"$tmp/err"; then
    cat "$tmp/err" >&2
    exit 1
fi
if grep -q 'warning N00B-P0012:' "$tmp/err"; then
    cat "$tmp/err" >&2
    echo "preference overrides did not suppress preference warnings" >&2
    exit 1
fi

if ! "$compiler" --no-syntax-warnings check "$program" \
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

# Syntax warnings follow the settings of the compile that reports them,
# whatever the settings when an imported module was cached: a warm check
# reports what a cold one with the same settings does.
mkdir "$tmp/warm" "$tmp/cold"
cp "$preferences_program" "$tmp/warm/helper.n"
printf 'import helper\n' >"$tmp/warm/main.n"
syntax_check() {
    dir=$1
    shift
    (cd "$tmp/$dir" && N00B_DIAGNOSTICS=terse "$compiler" check "$@" main.n) \
        2>&1 | grep 'N00B-P00' || true
}
for settings in "" --no-syntax-warnings "--syntax-preference function=fn" ""; do
    rm -rf "$tmp/cold"
    mkdir "$tmp/cold"
    cp "$tmp/warm/helper.n" "$tmp/warm/main.n" "$tmp/cold/"
    # shellcheck disable=SC2086
    syntax_check warm $settings >"$tmp/warm.out"
    # shellcheck disable=SC2086
    syntax_check cold $settings >"$tmp/cold.out"
    cmp -s "$tmp/warm.out" "$tmp/cold.out" || {
        echo "syntax warnings ($settings) differ between a warm and a cold check" >&2
        diff -u "$tmp/cold.out" "$tmp/warm.out" >&2
        exit 1
    }
done
