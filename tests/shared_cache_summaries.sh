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

compare() {
    target=$1
    filler=$2
    label=$3
    dump "$tmp/$label.cold" "$target" "$tmp/$label.cold"
    N00B_CACHE="$tmp/$label.shared" N00B_PATH="$programs/bitcode" \
        "$compiler" check "$filler" >/dev/null 2>&1 || {
        echo "filling the cache with $filler failed" >&2
        exit 1
    }
    dump "$tmp/$label.shared" "$target" "$tmp/$label.warm"
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

compare "$programs/merged/calls.n" "$programs/features/fast_suite.n" calls
compare "$programs/features/fast_suite.n" "$programs/merged/calls.n" fast
