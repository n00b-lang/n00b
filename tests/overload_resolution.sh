#!/bin/sh
set -eu

compiler=$1
positive=$2
ambiguous=$3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if ! "$compiler" build --output "$tmp/program" "$positive" \
    >"$tmp/positive.out" 2>"$tmp/positive.err"; then
    cat "$tmp/positive.out" "$tmp/positive.err" >&2
    exit 1
fi

warning_count=$(grep -Fc 'N00B-T0014' "$tmp/positive.err" || true)
if [ "$warning_count" -ne 1 ]; then
    echo "expected N00B-T0014 exactly once, got $warning_count" >&2
    cat "$tmp/positive.out" "$tmp/positive.err" >&2
    exit 1
fi

if ! "$tmp/program" >"$tmp/runtime.out" 2>"$tmp/runtime.err"; then
    cat "$tmp/runtime.out" "$tmp/runtime.err" >&2
    exit 1
fi
if ! grep -Fq 'ok' "$tmp/runtime.err"; then
    cat "$tmp/runtime.out" "$tmp/runtime.err" >&2
    exit 1
fi

if "$compiler" check "$ambiguous" >"$tmp/ambiguous.out" 2>&1; then
    echo 'ambiguous overload unexpectedly compiled' >&2
    exit 1
fi
if ! grep -Fq 'N00B-T0012' "$tmp/ambiguous.out"; then
    cat "$tmp/ambiguous.out" >&2
    exit 1
fi
