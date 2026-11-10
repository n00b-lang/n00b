#!/bin/sh
set -u

compiler=$1
programs=$2
group=$3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
failed=0

builds() {
    name=$1
    shift
    if ! "$compiler" build --output "$tmp/$name" "$@" \
        >"$tmp/$name.out" 2>"$tmp/$name.err"; then
        echo "$name: build failed" >&2
        cat "$tmp/$name.out" "$tmp/$name.err" >&2
        failed=1
        return 1
    fi
}

prints() {
    name=$1
    expected=$2
    shift 2
    builds "$name" "$@" || return
    "$tmp/$name" >/dev/null 2>"$tmp/$name.run"
    if [ "$(cat "$tmp/$name.run")" != "$expected" ]; then
        echo "$name: expected '$expected', got '$(cat "$tmp/$name.run")'" >&2
        failed=1
    fi
}

rejects() {
    name=$1
    code=$2
    shift 2
    if N00B_DIAGNOSTICS=terse "$compiler" build \
        --output "$tmp/$name" "$@" >"$tmp/$name.out" 2>"$tmp/$name.err"; then
        echo "$name: build succeeded but should report $code" >&2
        failed=1
    elif ! grep -q "$code" "$tmp/$name.err"; then
        echo "$name: expected $code" >&2
        cat "$tmp/$name.err" >&2
        failed=1
    fi
}

warns() {
    name=$1
    code=$2
    shift 2
    prints "$name" "$@" || return
    if ! grep -q "$code" "$tmp/$name.err"; then
        echo "$name: expected warning $code" >&2
        cat "$tmp/$name.err" >&2
        failed=1
    fi
}

prunes() {
    name=$1
    expected=$2
    pruned=$3
    shift 3
    if ! N00BB_PROFILE_COMPILE=1 "$compiler" build --output "$tmp/$name" \
        "$@" >"$tmp/$name.out" 2>"$tmp/$name.err"; then
        echo "$name: build failed" >&2
        cat "$tmp/$name.out" "$tmp/$name.err" >&2
        failed=1
        return
    fi
    if [ "$(grep comptime-pruned "$tmp/$name.err")" != \
        "PROFILE comptime-pruned $pruned" ]; then
        echo "$name: expected only $pruned pruned" >&2
        grep comptime-pruned "$tmp/$name.err" >&2
        failed=1
    fi
    "$tmp/$name" >/dev/null 2>"$tmp/$name.run"
    if [ "$(cat "$tmp/$name.run")" != "$expected" ]; then
        echo "$name: expected '$expected', got '$(cat "$tmp/$name.run")'" >&2
        failed=1
    fi
}

case $group in
imports)
    N00B_PATH="$programs/imports" warns changer N00B-B0114 \
        "imported state kept" "$programs/imports/changer.n"
    N00B_PATH="$programs/imports" prints starter "comptime_start change kept" \
        "$programs/imports/starter.n"
    N00B_PATH="$programs/identity" prints identity \
        "same object across modules" "$programs/identity/user.n" ;;
values)
    prints baked "baked" "$programs/baked.n"
    prints templates "templated
floats" "$programs/templates.n"
    prints const_once "const baked, not recomputed" "$programs/const_once.n"
    prints const_unpack_once "unpacked const baked, not recomputed" \
        "$programs/const_unpack_once.n"
    prints heap_values \
        "hello from comptime, long enough to live on the heap" \
        "$programs/heap_values.n" ;;
graphs)
    prints record_list "second entry with a heap name
mutable at runtime" "$programs/record_list.n"
    N00B_PATH="$programs/imported" prints imported "five" \
        "$programs/imported/app.n" ;;
start)
    prints comptime_start "comptime_start ran" "$programs/comptime_start.n"
    prints start_reads_comptime "ok" "$programs/start_reads_comptime.n"
    prunes runtime_prune "bumped recomputed
phase recomputed
items recomputed
kept" runtime_prune.kept "$programs/runtime_prune.n"
    prunes prune_literal "list kept" prune_literal.names \
        "$programs/prune_literal.n" ;;
entry)
    prints entry_exit "frozen at exit" "$programs/entry_exit.n"
    prints runtime_branch "runtime" "$programs/runtime_branch.n"
    prints loop_back "loop-back ok" "$programs/loop_back.n" ;;
failures)
    rejects entry_failure N00B-B0110 "$programs/entry_failure.n"
    N00B_PATH="$programs/module_exit" rejects module_exit N00B-B0112 \
        "$programs/module_exit/app.n"
    rejects timeout N00B-B0111 --comptime-timeout 1 "$programs/timeout.n" ;;
*)
    echo "unknown group '$group'" >&2
    failed=1 ;;
esac
exit $failed
