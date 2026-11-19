#!/bin/sh
# Calls through function references with keyword arguments, run, and
# typed against an imported module's archived interface.
set -eu

compiler=$1
programs=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if ! N00B_PATH="$programs" "$compiler" build --output "$tmp/keywords" \
    "$programs/reference_keywords.n" >"$tmp/build.out" 2>"$tmp/build.err"
then
    cat "$tmp/build.out" "$tmp/build.err" >&2
    exit 1
fi
if ! "$tmp/keywords" >"$tmp/stdout" 2>"$tmp/stderr"; then
    cat "$tmp/stdout" "$tmp/stderr" >&2
    exit 1
fi
if test -s "$tmp/stdout" || test "$(cat "$tmp/stderr")" != ok; then
    cat "$tmp/stdout" "$tmp/stderr" >&2
    exit 1
fi

# The keyword parameters' names, and the reference adapters, come from the
# interface when the imported modules are only an archived interface.
if ! N00B_PATH="$programs" "$compiler" library \
    --emit-interface "$tmp/keyword_lib.n00bi" keyword_lib keyword_refs \
    >"$tmp/interface.out" 2>"$tmp/interface.err"; then
    cat "$tmp/interface.out" "$tmp/interface.err" >&2
    exit 1
fi
if ! "$compiler" lower --link-interface "$tmp/keyword_lib.n00bi" \
    "$programs/reference_keywords.n" >"$tmp/lower.out" 2>"$tmp/lower.err"
then
    cat "$tmp/lower.err" >&2
    exit 1
fi

# `ref` of a function whose parameters are all `in`, with no keyword
# parameters to reorder, is the function itself; of any other, it is the
# function's reference adapter, the one the interface declares for an
# imported function. A call through a reference known to hold an adapter
# calls the function it adapts, and `native_entry` is the function itself.
for expected in \
    'global.store @reference_keywords.plain_reference, @plain_add ' \
    'global.store @reference_keywords.adapted_reference, @__n00bb_reference_adapter_' \
    'global.store @reference_keywords.imported_check, @__n00bb_reference_adapter_' \
    '= call @tally_add, ' \
    '= bitcast @tally_add '
do
    if ! grep -qF "$expected" "$tmp/lower.out"; then
        echo "the lowered program has no \`$expected\`" >&2
        exit 1
    fi
done
