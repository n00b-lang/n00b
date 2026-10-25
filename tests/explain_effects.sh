#!/bin/sh
set -eu

mode=check
if [ "$1" = --library ]; then
    mode=library
    shift
fi
compiler=$1
program=$2
expected=$3
names=$4
# A platform can have its own expectations, as in effects_expected.sh.
platform=$(uname -s | tr '[:upper:]' '[:lower:]')
case "$platform" in
mingw*|msys*|cygwin*) platform=windows ;;
esac
if [ -f "${expected%.expected}.$platform.expected" ]; then
    expected="${expected%.expected}.$platform.expected"
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if [ "$mode" = library ]; then
    set -- library --output "$tmp/library.n00bm"
else
    set -- check
fi
if ! "$compiler" "$@" --effects-of "$names" --effects-explain "$program" \
        >"$tmp/actual" 2>"$tmp/errors"; then
    cat "$tmp/errors" >&2
    exit 1
fi
diff -u "$expected" "$tmp/actual" >&2
