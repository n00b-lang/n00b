#!/bin/sh
# Function references a library makes, called by an importer built only
# against the library's archived interface: linked and run.
set -eu

compiler=$1
programs=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if ! N00B_PATH="$programs" "$compiler" library \
    --emit-interface "$tmp/modlib.n00bi" modlib.facade \
    >"$tmp/interface.out" 2>"$tmp/interface.err"; then
    cat "$tmp/interface.out" "$tmp/interface.err" >&2
    exit 1
fi
# No N00B_PATH: the library is only its archive.
if ! "$compiler" build --link-interface "$tmp/modlib.n00bi" \
    --output "$tmp/user" "$programs/modlib_user.n" \
    >"$tmp/build.out" 2>"$tmp/build.err"; then
    cat "$tmp/build.out" "$tmp/build.err" >&2
    exit 1
fi
if ! "$tmp/user" >"$tmp/stdout" 2>"$tmp/stderr"; then
    cat "$tmp/stdout" "$tmp/stderr" >&2
    exit 1
fi
if test -s "$tmp/stdout" || test "$(cat "$tmp/stderr")" != ok; then
    cat "$tmp/stdout" "$tmp/stderr" >&2
    exit 1
fi
