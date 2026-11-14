#!/bin/sh
set -eu

compiler=$1
sources=$2
part=$3
lto_flag=${4:-}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
N00B_CACHE="$tmp/cache"
export N00B_CACHE
build_flags=

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
    if ! (cd "$tmp/$1" && N00B_CACHE_LOG="$tmp/log" "$compiler" build \
            $build_flags --output program "$2" >"$tmp/build.out" 2>&1); then
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
    grep '^program store scc ' "$tmp/log" | sed 's/^program store scc //' \
        | sort | tr '\n' ' ' | sed 's/ $//'
}

expect_stored() {
    actual=$(stored)
    test "$actual" = "$2" || fail "$1: stored '$actual', expected '$2'"
}

edits() {
    grep -q '^program store scc ping,pong$' "$tmp/log" \
        || fail "cycle: ping and pong were not stored as one SCC"

    build work main.n
    expect_stored "unchanged rebuild" ""
    if grep -q '^program store instance ' "$tmp/log"; then
        fail "unchanged rebuild stored an instantiation"
    fi
    grep -q '^program runtime object ' "$tmp/log" \
        || fail "unchanged rebuild: no cached runtime-only object was used"

    build work main_literal.n
    build work main_literal.n
    expect_stored "unchanged rebuild with module literals" ""

    sed 's/return 41/return 40 + 1/' "$tmp/work/leaf.n" >"$tmp/leaf.n"
    mv "$tmp/leaf.n" "$tmp/work/leaf.n"
    build work main.n
    expect_stored "leaf body edit" "leaf"

    printf 'func leaf_value() -> int {\n    let values: Array[int] = [40, 1]\n    return values[0] + values[1]\n}\n' \
        >"$tmp/work/leaf.n"
    build work main.n
    grep -q '^program restart cached modules depend on summaries that changed$' \
        "$tmp/log" || fail "summary edit: the optimistic build did not restart"
    expect_stored "summary edit" "helper leaf"

    mkdir -p "$tmp/packages/alpha" "$tmp/packages/beta"
    printf 'import alpha\n\nrequire alpha.read() == 7\n' \
        >"$tmp/packages/main.n"
    for package in alpha beta; do
        printf 'import impl\n\npackage %s {\n    export impl\n}\n' \
            "$package" >"$tmp/packages/$package/facade.n"
    done
    printf 'import beta\n\nfunc read() -> int {\n    return beta.seven()\n}\n' \
        >"$tmp/packages/alpha/impl.n"
    printf 'func seven() -> int {\n    return 7\n}\n' \
        >"$tmp/packages/beta/impl.n"
    build packages main.n
    printf 'import beta\n\nfunc read() -> int {\n    return beta.seven() + 0\n}\n' \
        >"$tmp/packages/alpha/impl.n"
    N00B_NO_PACKAGE_CACHE=1
    export N00B_NO_PACKAGE_CACHE
    build packages main.n
    unset N00B_NO_PACKAGE_CACHE

    build work main_relay.n
    printf '\n' | cat - "$tmp/work/trapper.n" >"$tmp/trapper.n"
    mv "$tmp/trapper.n" "$tmp/work/trapper.n"
    build work main_relay.n
    expect_stored "line shift" "trapper"

    mkdir -p "$tmp/unused"
    printf 'func helper() -> int {\n    return 1\n}\n\nfunc other() -> int {\n    return 2\n}\n' \
        >"$tmp/unused/parts.n"
    printf 'import parts\n\nrequire parts.helper() + parts.other() == 3\n' \
        >"$tmp/unused/main.n"
    build unused main.n
    printf 'import parts\n\nrequire parts.other() == 2\n' >"$tmp/unused/main.n"
    build unused main.n
    grep -q '^program reuse scc parts$' "$tmp/log" \
        || fail "unused across modules: parts was not reused"
    grep -q 'function `helper` is never called' "$tmp/build.out" \
        || fail "unused across modules: the cached parts.helper was not reported"
    printf 'import parts\n\nrequire parts.helper() + parts.other() == 3\n' \
        >"$tmp/unused/main.n"
    build unused main.n
    if grep -q 'is never called' "$tmp/build.out"; then
        fail "unused across modules: a stale warning was replayed"
    fi
}

shapes() {
    printf '\nfunc leaf_extra() -> int {\n    return 1\n}\n' \
        >>"$tmp/work/leaf.n"
    build work main.n
    expect_stored "leaf interface edit" "helper leaf"

    sed 's/value \* 2/value + value/' "$tmp/work/shapes.n" >"$tmp/shapes.n"
    mv "$tmp/shapes.n" "$tmp/work/shapes.n"
    sed 's/return 0$/return n - n/' "$tmp/work/pong.n" >"$tmp/pong.n"
    mv "$tmp/pong.n" "$tmp/work/pong.n"
    build work main.n
    expect_stored "inline body and cycle edit" "ping,pong shapes user"

    mkdir -p "$tmp/relay/hooks"
    printf 'import hooks\n\nfunc twice(value: int) -> int {\n    return value * 2\n}\n\nlet step: hooks.Step = callback[hooks.Step](ref twice)\nrequire step.call(3) == 6\n' \
        >"$tmp/relay/main.n"
    printf 'callback Step {\n    call(value: int) -> int\n    handler(value: int) -> int\n}\n\nfunc extra() -> int {\n    return 1\n}\n' \
        >"$tmp/relay/hooks/impl.n"
    printf 'import impl\n\npackage hooks {\n    export impl.Step\n}\n' \
        >"$tmp/relay/hooks/facade.n"
    build relay main.n
    printf 'import impl\n\npackage hooks {\n    export impl.Step\n    export impl.extra\n}\n' \
        >"$tmp/relay/hooks/facade.n"
    build relay main.n
    grep -q '^program reuse scc hooks\.impl$' "$tmp/log" \
        || fail "re-exported callback: the cached impl module was not reused"
}

instances() {
    build work main_new.n
    count=$(grep -c '^program store instance boxes\.pick$' "$tmp/log" || true)
    test "$count" = 1 \
        || fail "new instantiation: stored $count boxes.pick instantiations"
    grep -q '^program reuse instance boxes\.pick$' "$tmp/log" \
        || fail "new instantiation: the cached boxes.pick instantiation was not reused"

    prepare second
    build second main_new.n
    grep -q '^program reuse scc builtin\.' "$tmp/log" \
        || fail "shared stdlib: a second project reused no builtin SCC"
    if grep -q '^program store scc builtin\.' "$tmp/log"; then
        fail "shared stdlib: a second project stored its own builtin SCCs"
    fi

    build work generic_values.n
    build work generic_values.n
    grep -q '^program reuse scc builtin\.containers\.list' "$tmp/log" \
        || fail "generic values: the cached list module was not reused"

    build work generic_fields.n
    build work generic_fields.n
    grep -q '^program reuse scc pairs$' "$tmp/log" \
        || fail "generic fields: the cached pairs module was not reused"

    build work imported_traits.n
    build work imported_traits.n
    grep -q '^program reuse scc scales$' "$tmp/log" \
        || fail "imported traits: the cached scales module was not reused"
    build_flags=--no-specialize
    build work imported_traits.n
    build_flags=

    build work imported_consts.n
    build work imported_consts.n
    grep -q '^program reuse scc limits$' "$tmp/log" \
        || fail "imported consts: the cached limits module was not reused"
}

contexts() {
    build work contexts.n
    build work main.n
    build work contexts.n
    if grep -q 'store scc\|restart' "$tmp/log"; then
        fail "dispatch context: switching back to contexts.n rebuilt cached SCCs"
    fi
    build work main.n
    if grep -q 'store scc\|restart' "$tmp/log"; then
        fail "dispatch context: switching back to main.n rebuilt cached SCCs"
    fi

    build work main_points.n
    sed 's/return self.x == other.x/let probe: Array[int] = [self.x]\
    return probe[0] == other.x/' "$tmp/work/points.n" >"$tmp/points.n"
    mv "$tmp/points.n" "$tmp/work/points.n"
    build work main_points.n
    grep -q '^program store instance compare\.same$' "$tmp/log" \
        || fail "owner summary edit: the compare.same instantiation was reused"
}

lto() {
    test -n "$lto_flag" || fail "the lto part needs the lto flag as its fourth argument"
    build_flags=$lto_flag
    build work main.n
    grep -q '^program store scc leaf$' "$tmp/log" \
        || fail "first lto build: leaf bitcode was not stored"

    build work main.n
    expect_stored "unchanged lto rebuild" ""
    grep -q '^program reuse scc leaf$' "$tmp/log" \
        || fail "unchanged lto rebuild: leaf was not reused"
    grep -q '^program runtime object ' "$tmp/log" \
        || fail "unchanged lto rebuild: no cached runtime-only object was used"
    if grep -q '^program store instance ' "$tmp/log"; then
        fail "unchanged lto rebuild stored an instantiation"
    fi

    sed 's/return 41/return 40 + 1/' "$tmp/work/leaf.n" >"$tmp/leaf.n"
    mv "$tmp/leaf.n" "$tmp/work/leaf.n"
    build work main.n
    expect_stored "lto leaf body edit" "leaf"

    build_flags=
    build work main.n
    expect_stored "native build after lto edit" "leaf"

    build_flags=$lto_flag
    build work main.n
    expect_stored "lto build after native" ""
}

case $part in
edits|shapes|instances|contexts|lto) ;;
*) fail "unknown part $part" ;;
esac
prepare work
build work main.n
"$part"
