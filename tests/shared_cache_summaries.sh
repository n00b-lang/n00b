#!/bin/sh
set -eu

compiler=$1
programs=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

blocks() {
    awk '/^summary @/ { name = $2; print name "\t" $0; next }
         /^  / && name != "" { print name "\t" $0; next }
         { name = "" }' "$1" | LC_ALL=C sort
}

dump() {
    N00B_CACHE=$1 N00B_PATH="$programs/bitcode" "$compiler" check \
        --dump-summaries "$2" >"$3.out" 2>"$3.err" || {
        echo "check failed: $2 with cache $1" >&2
        cat "$3.err" >&2
        exit 1
    }
}

# TARGET's summaries from an empty cache, $tmp/LABEL.cold, which the check
# leaves filled with what TARGET needs: byte for byte the cache a plain
# `check` of TARGET leaves, so it is the shared cache for another program.
cold() {
    dump "$tmp/$2.cold" "$1" "$tmp/$2.cold"
}

# TARGET's summaries from SHARED, a cache other programs filled, match
# its cold ones.
compare() {
    target=$1
    label=$2
    shared=$3
    dump "$shared" "$target" "$tmp/$label.warm"
    if ! cmp -s "$tmp/$label.cold.err" "$tmp/$label.warm.err"; then
        echo "$label: diagnostics differ between cold and shared-cache runs" >&2
        diff "$tmp/$label.cold.err" "$tmp/$label.warm.err" >&2 || true
        exit 1
    fi
    blocks "$tmp/$label.cold.out" >"$tmp/$label.cold.blocks"
    blocks "$tmp/$label.warm.out" >"$tmp/$label.warm.blocks"
    cut -f1 "$tmp/$label.cold.blocks" | LC_ALL=C sort -u >"$tmp/$label.cold.names"
    cut -f1 "$tmp/$label.warm.blocks" | LC_ALL=C sort -u >"$tmp/$label.warm.names"
    LC_ALL=C comm -12 "$tmp/$label.cold.names" "$tmp/$label.warm.names" \
        >"$tmp/$label.common"
    common=$(wc -l <"$tmp/$label.common" | tr -d ' ')
    if [ "$common" -lt 50 ]; then
        echo "$label: only $common summaries in common; the shared cache was not used" >&2
        exit 1
    fi
    awk -F '\t' 'NR == FNR { keep[$1] = 1; next } keep[$1]' \
        "$tmp/$label.common" "$tmp/$label.cold.blocks" >"$tmp/$label.cold.kept"
    awk -F '\t' 'NR == FNR { keep[$1] = 1; next } keep[$1]' \
        "$tmp/$label.common" "$tmp/$label.warm.blocks" >"$tmp/$label.warm.kept"
    if ! cmp -s "$tmp/$label.cold.kept" "$tmp/$label.warm.kept"; then
        echo "$label: summaries differ between cold and shared-cache runs" >&2
        diff "$tmp/$label.cold.kept" "$tmp/$label.warm.kept" >&2 || true
        exit 1
    fi
}

# TARGET, which the check rejects, reports the same diagnostics in two
# checks, the first with the environment setting FIRST (`NAME=value`) and
# the second with SECOND: from a cold cache and from one other programs
# filled, or from source and then from the cache. EXPECTED is how many
# errors.
compare_rejected() {
    target=$1
    label=$2
    expected=$3
    first=$4
    second=$5
    for run in first second; do
        if [ "$run" = first ]; then setting=$first; else setting=$second; fi
        if env "$setting" N00B_DIAGNOSTICS=terse "$compiler" check \
                --no-syntax-warnings "$target" \
                >/dev/null 2>"$tmp/$label.$run.err"; then
            echo "$label: the $run check passed, but should be rejected" >&2
            exit 1
        fi
    done
    if ! cmp -s "$tmp/$label.first.err" "$tmp/$label.second.err"; then
        echo "$label: diagnostics differ between the $first and" \
            "$second checks" >&2
        diff "$tmp/$label.first.err" "$tmp/$label.second.err" >&2 || true
        exit 1
    fi
    errors=$(grep -c ': error' "$tmp/$label.first.err" || true)
    if [ "$errors" -ne "$expected" ]; then
        echo "$label: expected $expected errors, got $errors" >&2
        cat "$tmp/$label.first.err" >&2
        exit 1
    fi
}

# TARGET checked with every module cached prints the same summaries, byte
# for byte, whether another program (FILLER) filled the cache with the
# library or TARGET did: what the cache keeps of a library module may not
# depend on the program that compiled it.
compare_warm() {
    filler=$1
    target=$2
    label=$3
    N00B_CACHE="$tmp/$label.shared" "$compiler" check "$filler" \
        >/dev/null 2>"$tmp/$label.filler.err" || {
        echo "$label: check of $filler failed" >&2
        cat "$tmp/$label.filler.err" >&2
        exit 1
    }
    for cache in shared own; do
        for run in first second; do
            N00B_CACHE="$tmp/$label.$cache" "$compiler" check \
                --dump-summaries "$target" \
                >"$tmp/$label.$cache.$run" 2>&1 || {
                echo "$label: check of $target failed ($cache, $run)" >&2
                cat "$tmp/$label.$cache.$run" >&2
                exit 1
            }
        done
    done
    if ! cmp -s "$tmp/$label.own.second" "$tmp/$label.shared.second"; then
        echo "$label: summaries differ between a cache $filler filled and" \
            "the program's own" >&2
        diff "$tmp/$label.own.second" "$tmp/$label.shared.second" \
            | head -40 >&2
        exit 1
    fi
}

# What is cached here is the program's own dependency, so its modules are
# copied to a directory no cache has seen and the caller's cache is used:
# the first check reads them from source, the second from the cache. (The
# library being cold or warm is what state_unset's check below covers.)
cp -R "$programs/cache_errors" "$tmp/cache_errors"
compare_rejected "$tmp/cache_errors/app.n" cache_errors 2 \
    "N00B_PATH=$tmp/cache_errors" "N00B_PATH=$tmp/cache_errors"
cold "$programs/merged/calls.n" calls
cold "$programs/features/fast_suite.n" fast
# calls.n from the cache fast_suite.n filled, then fast_suite.n from the
# one calls.n filled.
compare "$programs/merged/calls.n" calls "$tmp/fast.cold"
compare "$programs/features/fast_suite.n" fast "$tmp/calls.cold"
# That cache now holds what both programs need.
compare_rejected "$programs/callbacks/state_unset.n" state_unset 1 \
    "N00B_CACHE=$tmp/state_unset.cold" "N00B_CACHE=$tmp/calls.cold"
# A program that starts threads, after one that starts none filled the
# cache with the library.
compare_warm "$programs/shared_cache/lex_import.n" \
    "$programs/buffer/buffer_thread_slice.n" threads
