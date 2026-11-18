#!/bin/sh
set -eu

strip_outputs=yes
if [ "$1" = --with-outputs ]; then
    strip_outputs=
    shift
fi
compiler=$1
program=$2
expected=$3
names=$4
shift 4
# What the OS layer does differs by platform (Linux enters the kernel with
# syscalls; macOS reads errno after libSystem calls), so a platform can have
# its own expectations, NAME.linux.expected or NAME.windows.expected beside
# NAME.expected.
platform=$(uname -s | tr '[:upper:]' '[:lower:]')
case "$platform" in
mingw*|msys*|cygwin*) platform=windows ;;
esac
if [ -f "${expected%.expected}.$platform.expected" ]; then
    expected="${expected%.expected}.$platform.expected"
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if ! "$compiler" check --effects-of "$names" "$program" \
        >"$tmp/report" 2>"$tmp/errors"; then
    cat "$tmp/errors" >&2
    exit 1
fi
grep -v -e '^  trap ' -e '^  why ' ${strip_outputs:+-e '^  output '} \
    "$tmp/report" >"$tmp/actual" || true
# Source positions are not what these expectations check: an edit that
# moves code, in the library above all, doesn't change its effects.
positions() {
    sed -E -e 's/\([0-9]+:[0-9]+\)/(_:_)/g' \
        -e 's/([A-Za-z_][A-Za-z0-9_.]*):[0-9]+:[0-9]+/\1:_:_/g' "$1"
}
positions "$expected" >"$tmp/want"
positions "$tmp/actual" >"$tmp/have"
diff -u "$tmp/want" "$tmp/have" >&2
for pattern do
    if ! grep -Eq "$pattern" "$tmp/report"; then
        echo "no line matches: $pattern" >&2
        exit 1
    fi
done
