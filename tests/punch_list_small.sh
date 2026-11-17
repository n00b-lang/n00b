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
grep -q 'Tuple\[a : u8, b : u8\]' "$tmp/normalize.n"
grep -q 'Dict\[u8, nil\]' "$tmp/normalize.n"
if grep -q 'builtin\.' "$tmp/normalize.n"; then
    echo 'normalized types name builtin modules' >&2
    exit 1
fi
"$compiler" check "$tmp/normalize.n" >"$tmp/check.out" 2>"$tmp/check.err" || {
    cat "$tmp/check.err" >&2
    exit 1
}
