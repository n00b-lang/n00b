#!/bin/sh
set -eu

compiler=$1
program_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

runs() {
    "$compiler" build --output "$tmp/$1" "$program_dir/bitcode_raw_repr_$1.n00b" \
        2>"$tmp/$1.compile.err" || {
        cat "$tmp/$1.compile.err" >&2
        exit 1
    }
    "$tmp/$1" >"$tmp/$1.stdout" 2>"$tmp/$1.stderr"
    test ! -s "$tmp/$1.stdout"
}

case $3 in
overrides)
    runs overrides
    test "$(cat "$tmp/overrides.stderr")" = \
        "$(printf 'bool\nstring\n[item, item]\n[inner, inner]\n')"
    runs array_override
    test "$(cat "$tmp/array_override.stderr")" = \
        "$(printf 'int array override\narray override\n')"
    exit 0
    ;;
defaults) ;;
*)
    echo "unknown part $3" >&2
    exit 2
    ;;
esac

runs defaults
head -n 15 "$tmp/defaults.stderr" >"$tmp/defaults.values"
test "$(cat "$tmp/defaults.values")" = \
    "$(printf 'True\nFalse\n42\n-17\n3.5\n-0.0\n1.0e6\n1.0e-5\n[1, 2]\n[1.25, 2.5]\n[a, b]\n[[1, 2], [3]]\n<bitcode_raw_repr_defaults.Sample>\n<bitcode_raw_repr_defaults.Sample>\n2.5\n')"
tail -n +16 "$tmp/defaults.stderr" >"$tmp/reference.stderr"
grep -Eq '^[[:xdigit:]]{16}$' "$tmp/reference.stderr"
test "$(wc -l <"$tmp/reference.stderr")" -eq 1
if grep -Eq '^0{16}$' "$tmp/reference.stderr"; then
    exit 1
fi

if "$compiler" check "$program_dir/bitcode_raw_repr_out_of_range.n00b" \
    >"$tmp/negative.stdout" 2>"$tmp/negative.stderr"; then
    exit 1
fi
grep -q 'integer constant 2147483648 does not fit in `i32`' "$tmp/negative.stderr"
