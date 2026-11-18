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
if "$compiler" check "$tmp/maybe.n" >"$tmp/check.out" 2>"$tmp/check.err"; then
    echo 'a possibly-nil option was narrowed' >&2
    exit 1
fi
test "$(grep -c 'may be `nil`' "$tmp/check.err")" -eq 2
