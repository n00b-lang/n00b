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
"$compiler" effects may_effect Audited and not has_effect Audited --why \
    server.n >"$tmp/out" 2>&1 || fail "n00b effects found no partial auditor"
cat >"$tmp/want" <<'EOF'
server.n:17:1  server.maybe
    server.n:19:9  `maybe` calls `server.stamp`, which may have effect `audit_log.Audited`
1 function matches
EOF
diff -u "$tmp/want" "$tmp/out" >&2 || fail "n00b effects listed the wrong functions"
status=0
"$compiler" effects forbids_effect Audited --in server.handle \
    --in server.stamp server.n >"$tmp/out" 2>&1 || status=$?
test "$status" = 1 || fail "an empty n00b effects result exited $status, not 1"
grep -q 'no function in `server.handle`, `server.stamp` matches the query' \
    "$tmp/out" \
    || fail "an empty n00b effects result didn't say so"
status=0
"$compiler" effects may_effect Nope server.n >"$tmp/out" 2>&1 || status=$?
test "$status" = 2 || fail "a bad n00b effects query exited $status, not 2"
grep -q 'N00B-C0036.*`Nope` is not an effect' "$tmp/out" \
    || fail "a bad n00b effects query didn't report N00B-C0036"
sed '/handled_by/d' audit_log.n >audit_log.next
mv audit_log.next audit_log.n
if check server.n; then
    fail "removing handled by didn't reach the cached server module"
fi
grep -q 'N00B-S0073.*`batched` may have effect `audit_log.Audited`' "$tmp/out" \
    || fail "removing handled by didn't make batched's check false"
