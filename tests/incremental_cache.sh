#!/bin/sh
set -eu

compiler=$1
sources=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

fail() {
    echo "$1" >&2
    if test -f "$tmp/log"; then cat "$tmp/log" >&2; fi
    exit 1
}

prepare() {
    rm -rf "$tmp/$1"
    mkdir -p "$tmp/$1"
    cp "$sources"/*.n "$tmp/$1/"
}

build() {
    rm -f "$tmp/log"
    if ! (cd "$tmp/$1" && N00BB_CACHE_LOG="$tmp/log" "$compiler" build \
            --output program "$2" >"$tmp/build.out" 2>&1); then
        cat "$tmp/build.out" >&2
        fail "build of $2 failed"
    fi
    touch "$tmp/log"
    if ! "$tmp/$1/program" >"$tmp/run.out" 2>&1; then
        cat "$tmp/run.out" >&2
        fail "program $2 failed"
    fi
}

stored() {
    grep '^runtime store scc ' "$tmp/log" | sed 's/^runtime store scc //' \
        | sort | tr '\n' ' ' | sed 's/ $//'
}

expect_stored() {
    actual=$(stored)
    test "$actual" = "$2" || fail "$1: stored '$actual', expected '$2'"
}

prepare work
build work main.n
grep -q '^runtime store scc ping,pong$' "$tmp/log" \
    || fail "cycle: ping and pong were not stored as one SCC"

build work main.n
expect_stored "unchanged rebuild" ""
if grep -q '^runtime store instance ' "$tmp/log"; then
    fail "unchanged rebuild stored an instantiation"
fi

sed 's/return 41/return 40 + 1/' "$tmp/work/leaf.n" >"$tmp/leaf.n"
mv "$tmp/leaf.n" "$tmp/work/leaf.n"
build work main.n
expect_stored "leaf body edit" "leaf"

printf '\npublic func leaf_extra() -> int {\n    return 1\n}\n' \
    >>"$tmp/work/leaf.n"
build work main.n
expect_stored "leaf interface edit" "helper leaf"

sed 's/value \* 2/value + value/' "$tmp/work/shapes.n" >"$tmp/shapes.n"
mv "$tmp/shapes.n" "$tmp/work/shapes.n"
build work main.n
expect_stored "inline body edit" "shapes user"

sed 's/return 0$/return n - n/' "$tmp/work/pong.n" >"$tmp/pong.n"
mv "$tmp/pong.n" "$tmp/work/pong.n"
build work main.n
expect_stored "cycle edit" "ping,pong"

build work main_new.n
count=$(grep -c '^runtime store instance boxes\.pick$' "$tmp/log" || true)
test "$count" = 1 \
    || fail "new instantiation: stored $count boxes.pick instantiations"
grep -q '^runtime reuse instance boxes\.pick$' "$tmp/log" \
    || fail "new instantiation: the cached boxes.pick instantiation was not reused"

prepare first
prepare second
build first main.n
build second main_new.n
shared=0
for key in "$tmp/first/.n00b-cache/obj/modules"/builtin.*.runtime.key; do
    name=$(basename "$key")
    other="$tmp/second/.n00b-cache/obj/modules/$name"
    test -f "$other" || continue
    cmp -s "$key" "$other" || fail "identical object: $name keys differ"
    object=$(cat "$key").n00bo
    cmp -s "$tmp/first/.n00b-cache/obj/$object" \
        "$tmp/second/.n00b-cache/obj/$object" \
        || fail "identical object: $name objects differ"
    shared=$((shared + 1))
done
test "$shared" -gt 0 || fail "identical object: no builtin SCC was shared"
