#!/bin/sh
set -eu

compiler=$1
program=$2
link_object=${3:-}
native_root=${4:-}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

set -- "$compiler" build --output "$tmp/os" "$program"
if test -n "$link_object"; then
    set -- "$@" --link-object "$link_object"
fi
if test -n "$native_root"; then
    set -- "$@" --native-root "$native_root"
fi
if ! "$@" \
    >"$tmp/compile.out" 2>"$tmp/compile.err"; then
    cat "$tmp/compile.out" "$tmp/compile.err" >&2
    exit 1
fi
if ! "$tmp/os" >"$tmp/stdout" 2>"$tmp/stderr"; then
    cat "$tmp/stdout" "$tmp/stderr" >&2
    exit 1
fi
if test -s "$tmp/stdout" || test "$(cat "$tmp/stderr")" != ok; then
    cat "$tmp/stdout" "$tmp/stderr" >&2
    exit 1
fi
