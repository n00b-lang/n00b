#!/bin/sh
set -eu

root=$1
output=$2
version=$(tr -d ' \n' <"$root/VERSION")
revision=none
build=
date=
dirty=0

if command -v jj >/dev/null 2>&1 && [ -d "$root/.jj" ]; then
    state=$(jj -R "$root" log -r @ --no-graph \
        -T 'if(empty, "clean", "dirty")' 2>/dev/null || echo unknown)
    info=$(jj -R "$root" log -r @- --no-graph --limit 1 \
        -T 'change_id.short(8) ++ " " ++ committer.timestamp().utc().format("%y%m%d%H%M") ++ " " ++ committer.timestamp().utc().format("%Y-%m-%d %H:%M")' \
        2>/dev/null || true)
    if [ -n "$info" ]; then
        revision=${info%% *}
        rest=${info#* }
        build=${rest%% *}
        date="${rest#* } UTC"
    fi
    [ "$state" = clean ] || dirty=1
elif command -v git >/dev/null 2>&1 \
    && git -C "$root" rev-parse --git-dir >/dev/null 2>&1; then
    revision=$(git -C "$root" log -1 --format=%h)
    build=$(TZ=UTC git -C "$root" log -1 \
        --date=format-local:%y%m%d%H%M --format=%cd)
    date="$(TZ=UTC git -C "$root" log -1 \
        --date=format-local:'%Y-%m-%d %H:%M' --format=%cd) UTC"
    [ -z "$(git -C "$root" status --porcelain --untracked-files=no)" ] \
        || dirty=1
else
    dirty=1
fi

if [ "$dirty" = 1 ] || [ -z "$build" ]; then
    build=$(date -u +%y%m%d%H%M)
    date="$(date -u '+%Y-%m-%d %H:%M') UTC"
fi

replace() {
    if cmp -s "$1" "$2"; then
        rm -f "$1"
    else
        mv "$1" "$2"
    fi
}

tmp="$output.tmp.$$"
cat >"$tmp" <<EOF
#pragma once
#include <stdint.h>
#define N00BB_VERSION "$version"
extern const uint64_t n00bb_compiler_identity;
extern const char n00bb_build[];
extern const char n00bb_revision[];
extern const char n00bb_build_date[];
extern const int n00bb_dirty;
EOF
replace "$tmp" "$output"

stamp="${output%.h}.c"
cat >"$tmp" <<EOF
const char n00bb_build[] = "$build";
const char n00bb_revision[] = "$revision";
const char n00bb_build_date[] = "$date";
const int n00bb_dirty = $dirty;
EOF
replace "$tmp" "$stamp"
