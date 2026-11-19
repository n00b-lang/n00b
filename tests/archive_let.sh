#!/bin/sh
# Module variables a library's initializer sets, read by an importer checked
# only against the library's archived interface. A variable nothing may have
# set is still an error there.
set -eu

compiler=$1
programs=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

fail() {
    echo "$1" >&2
    cat "$tmp/out" >&2
    exit 1
}

N00B_PATH="$programs" "$compiler" library \
    --emit-interface "$tmp/letlib.n00bi" letlib.facade >"$tmp/out" 2>&1 \
    || fail "emitting letlib's interface failed"
# No N00B_PATH: the library is only its archive.
"$compiler" check --link-interface "$tmp/letlib.n00bi" \
    "$programs/letlib_user.n" >"$tmp/out" 2>&1 \
    || fail "an importer of letlib's archive didn't check"
if "$compiler" check --link-interface "$tmp/letlib.n00bi" \
        "$programs/letlib_unset.n" >"$tmp/out" 2>&1; then
    fail "reading a variable nothing may have set checked against the archive"
fi
grep -q 'letlib_unset.n:6:12: error.*may read global `unset`' "$tmp/out" \
    || fail "the archive check didn't report the unset read"
