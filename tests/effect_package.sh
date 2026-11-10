#!/bin/sh
set -eu

compiler=$1
sources=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
shared_cache=${N00B_CACHE-}
N00B_CACHE="$tmp/cache"
export N00B_CACHE
export N00B_DIAGNOSTICS=terse

fail() {
    echo "$1" >&2
    cat "$tmp/out" >&2
    exit 1
}

check_both() {
    N00B_PATH="$sources" "$compiler" check "$sources/$1" >"$tmp/out" 2>&1 \
        || return 1
    "$compiler" check --link-interface "$tmp/alpha.n00bi" "$sources/$1" \
        >"$tmp/out" 2>&1
}

N00B_PATH="$sources" "$compiler" library --emit-interface "$tmp/alpha.n00bi" \
    alpha.facade >"$tmp/out" 2>&1 || fail "emitting alpha's interface failed"
N00B_PATH="$sources" "$compiler" library --emit-interface "$tmp/alpha.n00bi" \
    --output "$tmp/alpha.n00bm" alpha.facade >"$tmp/out" 2>&1 \
    || fail "emitting alpha's interface over the old one failed"
[ -s "$tmp/alpha.n00bm" ] \
    || fail "--output was ignored next to --emit-interface"
check_both uses.n || fail "a package's effect didn't reach its importer"
if N00B_PATH="$sources" "$compiler" check "$sources/leaks.n" >"$tmp/out" 2>&1
then
    fail "a source package's effect check passed when it should fail"
fi
grep -q 'N00B-S0073' "$tmp/out" || fail "leaks.n failed without N00B-S0073"
if "$compiler" check --link-interface "$tmp/alpha.n00bi" "$sources/leaks.n" \
        >"$tmp/out" 2>&1; then
    fail "an interface package's effect check passed when it should fail"
fi
grep -q 'N00B-S0073' "$tmp/out" || fail "leaks.n failed without N00B-S0073"
grep -q 'calls `alpha.impl.note`' "$tmp/out" \
    || fail "the interface note doesn't name alpha.impl.note once"
N00B_PATH="$sources" "$compiler" library --emit-interface "$tmp/sorter.n00bi" \
    sorter.facade >"$tmp/out" 2>&1 || fail "emitting sorter's interface failed"
if [ -n "$shared_cache" ]; then
    set -- env N00B_CACHE="$shared_cache"
else
    set -- env -u N00B_CACHE
fi
"$@" "$compiler" build \
    --link-interface "$tmp/alpha.n00bi" --link-interface "$tmp/sorter.n00bi" \
    --output "$tmp/runs" "$sources/runs.n" >"$tmp/out" 2>&1 \
    || fail "a program didn't build from library interfaces"
status=0
"$tmp/runs" >"$tmp/out" 2>&1 || status=$?
test "$status" = 9 \
    || fail "the program built from library interfaces exited $status, not 9"
N00B_PATH="$sources" "$compiler" check "$sources/handles.n" >"$tmp/out" 2>&1 \
    || fail "handled_by from another package didn't stop the effect"
if grep -q 'handled_by' "$tmp/out"; then
    fail "handled_by from another package warned"
fi
N00B_PATH="$sources" "$compiler" check sorter.facade >"$tmp/out" 2>&1 \
    || fail "sorter failed to check"
for round in fresh cached; do
    if N00B_PATH="$sources" "$compiler" check "$sources/picks.n" \
            >"$tmp/out" 2>&1; then
        fail "a printing callback passed the $round bound in another package"
    fi
    test "$(grep -c 'N00B-S0077' "$tmp/out")" = 1 \
        || fail "picks.n should report one N00B-S0077 ($round)"
    grep -q 'picks.n:20:12: error N00B-S0077' "$tmp/out" \
        || fail "the N00B-S0077 isn't at the printing callback ($round)"
done
