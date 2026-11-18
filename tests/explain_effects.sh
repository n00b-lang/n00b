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
# Source positions are not what these expectations check: an edit that
# moves code, in the library above all, doesn't change its effects.
positions() {
    sed -E -e 's/\([0-9]+:[0-9]+\)/(_:_)/g' \
        -e 's/([A-Za-z_][A-Za-z0-9_.]*):[0-9]+:[0-9]+/\1:_:_/g' "$1"
}
positions "$expected" >"$tmp/want"
positions "$tmp/actual" >"$tmp/have"
diff -u "$tmp/want" "$tmp/have" >&2
