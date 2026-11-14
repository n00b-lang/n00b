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
diff -u "$expected" "$tmp/actual" >&2
for pattern do
    if ! grep -Eq "$pattern" "$tmp/report"; then
        echo "no line matches: $pattern" >&2
        exit 1
    fi
done
