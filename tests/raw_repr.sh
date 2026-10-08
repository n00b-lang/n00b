#!/bin/sh
set -eu

compiler=$1
program=$2
out_of_range=$3
defaults=$4
array_overrides=$5
reference=$6
element_override=$7
nested_override=$8
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$compiler" build --output "$tmp/raw-repr" "$program" 2>"$tmp/compile.err"
"$tmp/raw-repr" >"$tmp/stdout" 2>"$tmp/stderr"
test ! -s "$tmp/stdout"
test "$(cat "$tmp/stderr")" = "$(printf 'bool\nstring\n')"
"$compiler" build --output "$tmp/defaults" "$defaults" \
    2>"$tmp/defaults.compile.err"
"$tmp/defaults" >"$tmp/defaults.stdout" 2>"$tmp/defaults.stderr"
test ! -s "$tmp/defaults.stdout"
test "$(cat "$tmp/defaults.stderr")" = \
    "$(printf 'True\nFalse\n42\n-17\n3.5\n-0.0\n1.0e6\n1.0e-5\n[1, 2]\n[1.25, 2.5]\n[a, b]\n[[1, 2], [3]]\n')"
"$compiler" build --output "$tmp/array-overrides" "$array_overrides" \
    2>"$tmp/array-overrides.compile.err"
"$tmp/array-overrides" >"$tmp/array-overrides.stdout" \
    2>"$tmp/array-overrides.stderr"
test ! -s "$tmp/array-overrides.stdout"
test "$(cat "$tmp/array-overrides.stderr")" = \
    "$(printf 'int array override\narray override\n')"
"$compiler" build --output "$tmp/element-override" "$element_override" \
    2>"$tmp/element-override.compile.err"
"$tmp/element-override" >"$tmp/element-override.stdout" \
    2>"$tmp/element-override.stderr"
test ! -s "$tmp/element-override.stdout"
test "$(cat "$tmp/element-override.stderr")" = '[item, item]'
"$compiler" build --output "$tmp/nested-override" "$nested_override" \
    2>"$tmp/nested-override.compile.err"
"$tmp/nested-override" >"$tmp/nested-override.stdout" \
    2>"$tmp/nested-override.stderr"
test ! -s "$tmp/nested-override.stdout"
test "$(cat "$tmp/nested-override.stderr")" = '[inner, inner]'
"$compiler" build --output "$tmp/reference" "$reference" \
    2>"$tmp/reference.compile.err"
"$tmp/reference" >"$tmp/reference.stdout" \
    2>"$tmp/reference.stderr"
test ! -s "$tmp/reference.stdout"
grep -Eq '^[[:xdigit:]]{16}$' "$tmp/reference.stderr"
test "$(wc -l <"$tmp/reference.stderr")" -eq 1
! grep -Eq '^0{16}$' "$tmp/reference.stderr"
if "$compiler" build --output "$tmp/out-of-range" "$out_of_range" \
    >"$tmp/negative.stdout" 2>"$tmp/negative.stderr"; then
    exit 1
fi
grep -q 'no matching method implementation (expected i32)' "$tmp/negative.stderr"
