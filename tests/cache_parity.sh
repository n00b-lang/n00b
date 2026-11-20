#!/bin/sh
# usage: cache_parity.sh compiler sources
#
# A program checks the same way whether its imports are compiled from source
# or read from the object cache. `sources` holds modules under lib/, a
# prime.n that checks cleanly with them, and cases (every other *.n). Each
# case is checked three ways:
#   cold   its modules compiled from source;
#   warm   after checking prime.n stored the modules in the cache;
#   off    with the object cache off;
# and each must print what its .expected holds: terse diagnostics, then the
# exit status.
set -u
export N00B_DIAGNOSTIC_CODES=1 N00B_DIAGNOSTICS=terse

compiler=$1
sources=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
# Each mode checks a fresh copy of the sources, so no cache has seen the
# cases' modules. The standard library comes from the caller's cache.
if [ -z "${N00B_CACHE:-}" ]; then
    N00B_CACHE="$tmp/cache"
    export N00B_CACHE
fi
failed=0

prepare() {
    rm -rf "${tmp:?}/$1"
    mkdir -p "$tmp/$1"
    cp -R "$sources"/. "$tmp/$1/"
}

# run directory case [env...]: prints the diagnostics and the status.
run() {
    directory=$1 case=$2
    shift 2
    (cd "$tmp/$directory" && env N00B_PATH="$tmp/$directory/lib" "$@" \
        "$compiler" check "$case" 2>&1 >/dev/null
     echo "status $?")
}

compare() {
    mode=$1 case=$2 actual=$3
    if ! printf '%s\n' "$actual" \
            | diff -u "$sources/${case%.n}.expected" - >"$tmp/diff"; then
        echo "FAIL $case ($mode)"
        cat "$tmp/diff"
        failed=1
    fi
}

for path in "$sources"/*.n; do
    case=${path##*/}
    [ "$case" = prime.n ] && continue
    prepare cold
    compare cold "$case" "$(run cold "$case")"
    prepare warm
    prime=$(run warm prime.n)
    if [ "$prime" != "status 0" ]; then
        echo "FAIL prime.n: $prime"
        failed=1
        continue
    fi
    compare warm "$case" "$(run warm "$case")"
    prepare off
    compare off "$case" "$(run off "$case" N00B_NO_OBJECT_CACHE=1)"
done
exit $failed
