#!/bin/sh
set -eu

compiler=$1
src=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$compiler" build --no-syntax-warnings -D PUNCH_FLAG \
    --output "$tmp/small" "$src/tests/programs/punch_list_small.n" \
    >"$tmp/compile.out" 2>"$tmp/compile.err"
"$tmp/small" >"$tmp/stdout" 2>"$tmp/stderr"
test ! -s "$tmp/stdout"
test "$(cat "$tmp/stderr")" = ok

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
grep -q 'let product: *uint ' "$tmp/sizing.n"
grep -q 'let base: *uint ' "$tmp/sizing.n"
grep -q 'let scaled: *uint ' "$tmp/sizing.n"
grep -q 'let huge: *u128 ' "$tmp/sizing.n"
grep -q 'let small: *Dict\[u16, nil\]' "$tmp/sizing.n"
grep -q 'let large: *Dict\[u16, nil\]' "$tmp/sizing.n"

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
