#!/bin/sh
set -eu

compiler=$1
sources=$2
expected=$3
names=$4
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
N00B_CACHE="$tmp/cache"
export N00B_CACHE
cp "$sources"/*.n "$tmp/"
cd "$tmp"

fail() {
    echo "$1" >&2
    cat "$tmp/out" >&2
    exit 1
}

check() {
    N00B_DIAGNOSTICS=terse "$compiler" check "$@" >"$tmp/out" 2>&1
}

check server.n || fail "the first check failed"
check --effects-of "$names" --effects-explain server.n \
    || fail "explaining effects failed"
diff -u "$expected" "$tmp/out" >&2
check server.n || fail "the cached check failed"
sed '/handled by/d' audit_log.n >audit_log.next
mv audit_log.next audit_log.n
if check server.n; then
    fail "removing handled by didn't reach the cached server module"
fi
grep -q 'N00B-S0073.*`batched` may do `audit_log.Audited`' "$tmp/out" \
    || fail "removing handled by didn't make batched's check false"
