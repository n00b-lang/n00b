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
# (Checked below, in one rejected file with unset_state.n: every check is
# a full compile, and the two programs' errors don't touch.)

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
# `check` stops where `build` would. Checked below, with maybe.n.)
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

# The two callback programs are built and run as one: their names don't
# clash, and each part's requires run as they did on their own. The result
# writes `ok` once, after both.
{
    sed '/^__fd_write(2, "ok")$/d' "$tmp/callback_state.n"
    cat "$tmp/callback_names.n"
} >"$tmp/callbacks.n"
"$compiler" build --no-syntax-warnings \
    --output "$tmp/callbacks" "$tmp/callbacks.n" \
    >"$tmp/compile.out" 2>"$tmp/compile.err" || {
    echo 'the callbacks failed to build' >&2
    cat "$tmp/compile.err" >&2
    exit 1
}
"$tmp/callbacks" >"$tmp/stdout" 2>"$tmp/stderr"
test "$(cat "$tmp/stderr")" = ok
cat "$tmp/maybe.n" "$tmp/unset_state.n" >"$tmp/rejected.n"
if "$compiler" check --no-syntax-warnings "$tmp/rejected.n" \
        >"$tmp/rejected.out" 2>"$tmp/rejected.err"
then
    echo 'the unset callback state and the possibly-nil options passed' >&2
    exit 1
fi
grep -q '`local.count` is read on a path where it has no value' \
    "$tmp/rejected.err" || {
    echo 'callback state with no value was retained' >&2
    cat "$tmp/rejected.err" >&2
    exit 1
}
# A variable that may still be `nil` on some path stays an option: both
# of maybe.n's functions are reported (and only they say so).
test "$(grep -c 'may be `nil`' "$tmp/rejected.err")" -eq 2 || {
    echo 'a possibly-nil option was narrowed' >&2
    cat "$tmp/rejected.err" >&2
    exit 1
}
