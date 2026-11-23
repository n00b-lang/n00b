#!/bin/sh
set -u

compiler="$1/n00b"
programs="$2/programs"
llvm=$(sed -n 's/^LLVM_BINDIR := //p' "$1/config.mk")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
export N00B_DIAGNOSTIC_CODES=1 N00B_DIAGNOSTICS=terse
unset N00B_NO_OBJECT_CACHE
export N00B_PATH="$programs/cache_record"

image() {
    "$llvm/llvm-objdump" -s "$1" | awk '
        /^Contents of section / {
            keep = $0 !~ /\.note\.gnu\.build-id|\.buildid:|\.debug_|__DWARF|\.gnu_debuglink/
        }
        /file format/ { next }
        keep'
    "$llvm/llvm-nm" -P "$1" | LC_ALL=C sort
}

run() {
    out="$tmp/$1"
    mkdir -p "$out"
    "$compiler" build "$2" -o "$out/program" >"$out/stdout" 2>"$out/diagnostics"
    echo "status $?" >>"$out/diagnostics"
    [ -f "$out/program" ] && image "$out/program" >"$out/image"
    rm -rf "$out/program" "$out/program.dSYM"
}

heap="$programs/cache_barrier/heap_caller.n"
N00B_CACHE="$tmp/cache" run racy "$programs/cache_record/racy.n" &
N00B_NO_OBJECT_CACHE=1 N00B_CACHE="$tmp/off" run cold "$heap"
wait
N00B_CACHE="$tmp/cache" run stack "$programs/cache_barrier/stack_caller.n"
N00B_CACHE="$tmp/cache" run cross "$heap"
[ -s "$tmp/cold/image" ] && diff -rq "$tmp/cold" "$tmp/cross"
