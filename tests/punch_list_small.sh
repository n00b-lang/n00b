#!/bin/sh
# Regression checks for the small punch-list fixes that need a built
# program or a command other than `n00b check`.
set -eu

compiler=$1
src=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

# The behaviors in punch_list_small.n; `-D PUNCH_FLAG` has no `=`.
"$compiler" build --no-syntax-warnings -D PUNCH_FLAG \
    --output "$tmp/small" "$src/tests/programs/punch_list_small.n" \
    >"$tmp/compile.out" 2>"$tmp/compile.err"
"$tmp/small" >"$tmp/stdout" 2>"$tmp/stderr"
test ! -s "$tmp/stdout"
test "$(cat "$tmp/stderr")" = ok

# An option local assigned from a call whose type the rewrite settles keeps
# its option type (this used to be an internal error in the LLVM backend).
# It is its own program: the miscompile depended on the module's shape.
"$compiler" build --output "$tmp/settled" \
    "$src/tests/programs/option_local_settled_call.n" \
    >"$tmp/compile.out" 2>"$tmp/compile.err"
"$tmp/settled" >"$tmp/stdout" 2>"$tmp/stderr"
test ! -s "$tmp/stdout"
test "$(cat "$tmp/stderr")" = ok

# Reaching `unreachable()` stops the program with its own message.
cat >"$tmp/unreachable.n" <<'EOF'
func f(x: int) -> int {
    if x > 0 { return 1 }
    unreachable()
}
let n = f(-1)
EOF
"$compiler" build --output "$tmp/unreachable" "$tmp/unreachable.n" \
    >"$tmp/compile.out" 2>"$tmp/compile.err"
status=0
(ulimit -c 0; "$tmp/unreachable") >"$tmp/stdout" 2>"$tmp/stderr" || status=$?
test "$status" -eq 101
grep -q '^panic: reached code marked `unreachable()`$' "$tmp/stderr"

# `fmt --normalize` writes inferred types that parse and check again,
# tuples, records and builtin containers included.
cat >"$tmp/normalize.n" <<'EOF'
let t = (1, 2)
let r = (a = 1, b = 2)
var l: List[int] = []
var d: Dict[int, int] = {}
var a: Array[int][3][]
let s = {1, 2}
EOF
"$compiler" fmt --normalize "$tmp/normalize.n"
grep -q 'Tuple\[u8, u8\]' "$tmp/normalize.n"
grep -q 'Tuple\[a: u8, b: u8\]' "$tmp/normalize.n"
grep -q 'Dict\[u8, nil\]' "$tmp/normalize.n"
if grep -q 'builtin\.' "$tmp/normalize.n"; then
    echo 'normalized types name builtin modules' >&2
    exit 1
fi
"$compiler" check "$tmp/normalize.n" >"$tmp/check.out" 2>"$tmp/check.err" || {
    cat "$tmp/check.err" >&2
    exit 1
}

# Literal sizing: a product of literals fits its value; a module constant
# sizes what it meets; set literals that meet get one element type; a
# literal past 2^127 is `u128`.
cat >"$tmp/sizing.n" <<'EOF'
let product = 2_000_000_000 * 3
let base = 2_000_000_000
let scaled = base * 3
let huge = 170141183460469231731687303715884105728
func meet() {
    let small = {1, 2}
    let large = {300, 4}
    let both = small + large
}
EOF
"$compiler" fmt --normalize "$tmp/sizing.n"
# The formatter aligns the declarations, so allow spaces after the colon.
grep -q 'let product: *uint ' "$tmp/sizing.n"
grep -q 'let base: *uint ' "$tmp/sizing.n"
grep -q 'let scaled: *uint ' "$tmp/sizing.n"
grep -q 'let huge: *u128 ' "$tmp/sizing.n"
grep -q 'let small: *Dict\[u16, nil\]' "$tmp/sizing.n"
grep -q 'let large: *Dict\[u16, nil\]' "$tmp/sizing.n"

# A result that doesn't fit a smaller declared target fails its checked
# narrowing rather than wrapping.
cat >"$tmp/narrow.n" <<'EOF'
func narrowed(a: u16, b: u8) -> u8 {
    let r: u8 = a + b
    return r
}
let n = narrowed(300, 20)
EOF
"$compiler" build --output "$tmp/narrow" "$tmp/narrow.n" \
    >"$tmp/compile.out" 2>"$tmp/compile.err"
status=0
(ulimit -c 0; "$tmp/narrow") >"$tmp/stdout" 2>"$tmp/stderr" || status=$?
test "$status" -eq 101
grep -q 'overflows `u8`' "$tmp/stderr"

# A loop counter bounded by a declared `uint` stays 64-bit even when a
# product of it passes 64 bits: without explicit evidence the range
# analysis caps at 64.
cat >"$tmp/counter.n" <<'EOF'
func g(n: uint) -> uint {
    var i = 4
    while i < n {
        i += 1
    }
    return 32 * (i - 4)
}
EOF
"$compiler" fmt --normalize "$tmp/counter.n"
grep -q 'var i: *uint ' "$tmp/counter.n"

# Type arguments on an imported type are counted the same whether the
# module comes from source or from its cached interface: both checks
# report `Plain[int]`, `Holder[int, int]`, `Level[int]`, and the callbacks
# `Pick[int, int]` and `Count[int]`.
for pass in fresh cached; do
    N00B_CACHE="$tmp/arity-cache" N00B_DIAGNOSTICS=terse \
        N00B_PATH="$src/tests/diagnostics/modules/type_argument_arity_imported" \
        "$compiler" check "$src/tests/diagnostics/tu/type_argument_arity_imported.n" \
        >"$tmp/arity.out" 2>"$tmp/arity.err" && {
        echo "type argument check passed on the $pass pass" >&2
        exit 1
    }
    grep -q '`Plain` takes no type arguments, but 1 is given' "$tmp/arity.err"
    grep -q '`Holder` takes 1 type argument, but 2 are given' "$tmp/arity.err"
    grep -q '`Level` takes no type arguments, but 1 is given' "$tmp/arity.err"
    grep -q '`Pick` takes 1 type argument, but 2 are given' "$tmp/arity.err"
    grep -q '`Count` takes no type arguments, but 1 is given' "$tmp/arity.err"
done

# A record's, variant's or alias's `where` clauses constrain a use of the
# type from another module the same way whether that module comes from
# source or from its cached interface.
for pass in fresh cached; do
    N00B_CACHE="$tmp/where-cache" N00B_DIAGNOSTICS=terse \
        N00B_PATH="$src/tests/diagnostics/modules/where_imported" \
        "$compiler" check "$src/tests/diagnostics/tu/where_imported.n" \
        >"$tmp/where.out" 2>"$tmp/where.err" && {
        echo "where check passed on the $pass pass" >&2
        exit 1
    }
    grep -q '`string` does not implement `Integer`, which `Box` requires' \
        "$tmp/where.err"
    grep -q '`Maybe` cannot take `string` as a type argument' \
        "$tmp/where.err"
    grep -q '`string` does not implement `Integer`, which `Bag` requires' \
        "$tmp/where.err"
    grep -q '`string` does not implement `Integer`, which `counted` requires' \
        "$tmp/where.err"
    grep -q '`string` does not implement `Integer`, which `bare` requires' \
        "$tmp/where.err"
    # A trait named as a type is a parameter of its own, in field order.
    # `shaped`, which gives them fitting arguments, is not reported.
    for kind in Pair Num Ints; do
        test "$(grep -c "\`string\` does not implement \`Integer\`, which \`$kind\` requires" \
            "$tmp/where.err")" -eq 1
    done
    grep -q '`Pair` takes 2 type arguments, but 1 is given' "$tmp/where.err"
    if grep -q 'internal compiler error' "$tmp/where.err"; then
        echo "where check hit an internal error on the $pass pass" >&2
        exit 1
    fi
done
