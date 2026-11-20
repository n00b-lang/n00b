#!/bin/sh
# Regression checks for the medium punch-list fixes.
set -eu

compiler=$1
src=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$compiler" build --no-syntax-warnings \
    --output "$tmp/medium" "$src/tests/programs/punch_list_medium.n" \
    >"$tmp/compile.out" 2>"$tmp/compile.err" || {
    cat "$tmp/compile.err" >&2
    exit 1
}
"$tmp/medium" >"$tmp/stdout" 2>"$tmp/stderr"
test ! -s "$tmp/stdout"
test "$(cat "$tmp/stderr")" = ok

# A variable that may still be `nil` on some path stays an option.
cat >"$tmp/maybe.n" <<'EOF'
func maybe(c: bool) -> int {
    var x: int? = nil
    if c { x = 1 }
    return x + 1
}
func reset() -> int {
    var x: int? = 3
    x = nil
    return x + 1
}
EOF
# (Checked below, in the same rejected file as the `Point` key: every check
# is a full compile, and the two programs' errors don't touch.)

# A stateful callback keeps the address of its state where the state was
# declared: a module variable, or a local of the function that uses the
# callback, which stays on the stack since the callback never leaves it.
# (Callbacks are kept out of punch_list_medium.n: a module that makes one
# isn't run at comptime, which that file's comptime checks rely on.)
cat >"$tmp/callback_state.n" <<'EOF'
record Tally { count: int }
callback Tally_Counter {
    call(value: int) -> int
    handler(modify state: Tally, value: int) -> int
    setup(handler: Ref[(Tally, int) -> int],
          state: Ref[Tally]) -> nil
}
func tally_add(modify state: Tally, value: int) -> int {
    state.count += value
    return state.count
}
func setup(handler: Ref[(Tally, int) -> int],
           state: Ref[Tally]) -> nil {}
var tally: Tally = (count = 0)
let counter = callback[Tally_Counter](ref tally_add, ref tally)
require counter.call(2) == 2
require counter.call(3) == 5
require tally.count == 5
func local_tally() -> int {
    var local: Tally = (count = 10)
    let local_counter = callback[Tally_Counter](ref tally_add, ref local)
    require local_counter.call(2) == 12
    require local_counter.call(3) == 15
    return local.count
}
require local_tally() == 15
__fd_write(2, "ok")
EOF

# The state a callback retains is read through it later, so it must be
# set in full when the callback is made. (The analysis reports it, so
# `check` stops where `build` would. Checked below, in the same rejected
# file as the `Point` key.)
sed -e 's/^    var local: Tally = (count = 10)$/    var local: Tally/' \
    -e '/^__fd_write(2, "ok")$/d' \
    "$tmp/callback_state.n" >"$tmp/unset_state.n"

# A callback's `call` takes its arguments by the names its role declares,
# in any order, as well as by position.
cat >"$tmp/callback_names.n" <<'EOF'
callback Joiner {
    call(label: string, count: int) -> string
    handler(label: string, count: int) -> string
}
func join(label: string, count: int) -> string {
    var out = ""
    var i = 0
    while i < count {
        out = out + label
        i += 1
    }
    return out
}
func joined() -> string {
    let local_joiner = callback[Joiner](ref join)
    return local_joiner.call(count = 3, label = "ab")
}
let joiner = callback[Joiner](ref join)
require joiner.call(count = 2, label = "x") == "xx"
require joiner.call("y", 2) == "yy"
require joined() == "ababab"
__fd_write(2, "ok")
EOF
# (Built and run with own_equality.n below.)

# A type with its own `==` (a user `__equal`, or a `__cmp` that `==`
# follows) and no user `hash` gets no derived hash: not a record, not an
# object (whose identity hash would disagree with its `==`), and not a
# record holding a field of such a type. Declaring such a record is fine
# (it once failed to build); hashing it, directly or as a dict key, is
# diagnosed at the use.
cat >"$tmp/own_equality.n" <<'EOF'
record Eq_Field {
    v: int
}
func __equal(view a: Eq_Field, view b: Eq_Field) -> bool {
    return a.v == b.v
}
record Eq_Holder {
    inner: Eq_Field
    n: int
}
record Cmp_Field {
    v: int
}
func __cmp(view a: Cmp_Field, view b: Cmp_Field) -> Comparison {
    if a.v < b.v { return Comparison.less }
    if a.v > b.v { return Comparison.greater }
    return Comparison.equal
}
record Cmp_Holder {
    inner: Cmp_Field
    n: int
}
object Point {
    x: int
}
func __init(self: Point, x: int) {
    self.x = x
}
func __equal(view self: Point, view other: Point) -> bool {
    return self.x == other.x
}
let eq: Eq_Holder = (inner = (v = 1), n = 2)
let cmp: Cmp_Holder = (inner = (v = 1), n = 2)
let point = new[Point](1)
require eq.inner.v + cmp.inner.v + eq.n + cmp.n + point.x == 7
EOF
# The two callback programs and own_equality.n are built and run as one:
# their names don't clash, the callbacks keep the module from running at
# comptime either way, and each part's requires run as they did on their
# own. The result writes `ok` once, after all of them.
{
    sed '/^__fd_write(2, "ok")$/d' "$tmp/callback_state.n"
    cat "$tmp/own_equality.n" "$tmp/callback_names.n"
} >"$tmp/callbacks.n"
"$compiler" build --no-syntax-warnings \
    --output "$tmp/callbacks" "$tmp/callbacks.n" \
    >"$tmp/compile.out" 2>"$tmp/compile.err" || {
    echo 'the callbacks or types with their own equality failed to build' >&2
    cat "$tmp/compile.err" >&2
    exit 1
}
"$tmp/callbacks" >"$tmp/stdout" 2>"$tmp/stderr"
test "$(cat "$tmp/stderr")" = ok
# Every use in one check: each is diagnosed, and nothing crashes.
cp "$tmp/own_equality.n" "$tmp/own_use.n"
for case in Eq_Holder:eq Cmp_Holder:cmp Point:point; do
    type=${case%%:*}
    value=${case#*:}
    printf 'let %s_hash = %s.hash()\n' "$value" "$value" >>"$tmp/own_use.n"
    printf 'var %s_keyed: Dict[%s, int] = new[Dict[%s, int]]()\n' \
        "$value" "$type" "$type" >>"$tmp/own_use.n"
done
if "$compiler" check "$tmp/own_use.n" >"$tmp/own.out" 2>"$tmp/own.err"; then
    echo 'hashing types with their own equality passed' >&2
    exit 1
fi
if grep -q 'internal compiler error' "$tmp/own.err"; then
    cat "$tmp/own.err" >&2
    exit 1
fi
# Six errors, of which the file shows its first five (`--stop-on-error`);
# the last, `Point`'s `Hash`, is checked on its own below.
grep -Fq 'errors not shown: 1 ' "$tmp/own.err" || {
    echo 'hashing: the sixth error is not counted as left out' >&2
    cat "$tmp/own.err" >&2
    exit 1
}
cp "$tmp/own_equality.n" "$tmp/own_point.n"
printf 'var point_keyed: Dict[Point, int] = new[Dict[Point, int]]()\n' \
    >>"$tmp/own_point.n"
cat "$tmp/maybe.n" "$tmp/unset_state.n" >>"$tmp/own_point.n"
if "$compiler" check --no-syntax-warnings "$tmp/own_point.n" \
        >"$tmp/point.out" 2>"$tmp/point.err"
then
    echo 'keying by Point passed' >&2
    exit 1
fi
grep -q '`local.count` is read on a path where it has no value' \
    "$tmp/point.err" || {
    echo 'callback state with no value was retained' >&2
    cat "$tmp/point.err" >&2
    exit 1
}
# A variable that may still be `nil` on some path stays an option: both
# of maybe.n's functions are reported (and only they say so).
test "$(grep -c 'may be `nil`' "$tmp/point.err")" -eq 2 || {
    echo 'a possibly-nil option was narrowed' >&2
    cat "$tmp/point.err" >&2
    exit 1
}
cat "$tmp/point.err" >>"$tmp/own.err"
for type in Eq_Holder Cmp_Holder Point; do
    grep -Fq "no trait that \`$type\` implements provides \`hash\`" \
        "$tmp/own.err" \
        && grep -Fq "\`$type\` does not implement \`Hash\`" "$tmp/own.err" \
        || {
        echo "hashing $type: not diagnosed at the use" >&2
        cat "$tmp/own.err" >&2
        exit 1
    }
done
