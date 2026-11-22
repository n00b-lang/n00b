#!/bin/sh
set -u

build=$1
tests=$2
expected=$3
compiler="$build/n00b"
llvm=$(sed -n 's/^LLVM_BINDIR := //p' "$build/config.mk")
programs="$tests/programs"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
export N00B_DIAGNOSTIC_CODES=1 N00B_DIAGNOSTICS=terse
unset N00B_NO_OBJECT_CACHE N00B_PATH
: >"$tmp/failed"
: >"$tmp/cells"

hello="$tests/perf/hello.n"
stack="$programs/cache_barrier/stack_caller.n"
heap="$programs/cache_barrier/heap_caller.n"
racy="$tmp/record/racy.n"
slots=${N00B_MATRIX_JOBS:-2}

acquire() {
    while :; do
        slot=0
        while [ $slot -lt $slots ]; do
            mkdir "$tmp/slot.$slot" 2>/dev/null && return
            slot=$((slot + 1))
        done
        sleep 1
    done
}

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
    label=$1 cache=$2 command=$3 target=$4 extra=${5:-}
    acquire
    out="$tmp/$label"
    mkdir -p "$out"
    if [ "$cache" = off ]; then
        set -- env N00B_NO_OBJECT_CACHE=1 N00B_CACHE="$tmp/off.$label"
    else
        set -- env N00B_CACHE="$tmp/cache.$cache" \
            N00B_CACHE_LOG="$tmp/$label.log"
    fi
    case $command in
        syntax)
            (cd "$tmp/syntax" && "$@" "$compiler" check $extra main.n \
                >/dev/null 2>"$out/diagnostics")
            echo "status $?" >>"$out/diagnostics"
            ;;
        check)
            "$@" "$compiler" check --dump-summaries "$target" \
                >"$out/dump" 2>"$out/diagnostics"
            echo "status $?" >>"$out/diagnostics"
            LC_ALL=C sort "$out/dump" >"$out/summaries"
            rm -f "$out/dump"
            ;;
        build)
            "$@" "$compiler" build "$target" -o "$out/program" \
                >"$out/stdout" 2>"$out/diagnostics"
            echo "status $?" >>"$out/diagnostics"
            [ -f "$out/program" ] && image "$out/program" >"$out/image"
            rm -rf "$out/program" "$out/program.dSYM"
            ;;
        library)
            mkdir -p "$out/abi"
            "$@" N00B_PATH="$programs/link_records" "$compiler" library \
                --dev-module-abi "$out/abi" --output "$out/abi/linked.n00bm" \
                "$target" >"$out/stdout" 2>"$out/diagnostics"
            echo "status $?" >>"$out/diagnostics"
            ;;
    esac
    rmdir "$tmp/slot.$slot"
}

same() {
    cell=$1 left="$tmp/$2" right="$tmp/$3"
    echo "$cell" >>"$tmp/cells"
    if diff -rq "$left" "$right" >"$tmp/diff.$cell" 2>&1; then
        echo "pass $cell"
    else
        echo "FAIL $cell"
        head -20 "$tmp/diff.$cell" | sed 's/^/    /'
        echo "$cell" >>"$tmp/failed"
    fi
}

logged() {
    cell=$1 log="$tmp/$2.log" found=$3 missing=$4
    echo "$cell" >>"$tmp/cells"
    if grep -q "$found" "$log" && ! grep -q "$missing" "$log"; then
        echo "pass $cell"
    else
        echo "FAIL $cell"
        echo "$cell" >>"$tmp/failed"
    fi
}

checks() {
    run check.hello.cold h check "$hello"
    cp -R "$tmp/cache.h" "$tmp/cache.s"
    cp -R "$tmp/cache.h" "$tmp/cache.so"
    run check.hello.warm h check "$hello"
    mkdir -p "$tmp/syntax"
    cp "$programs/syntax_preferences.n" "$tmp/syntax/prefs.n"
    printf 'import prefs\n' >"$tmp/syntax/main.n"
    run syntax.fill s syntax ""
    run syntax.warm s syntax "" --no-syntax-warnings &
    run syntax.cold so syntax "" --no-syntax-warnings
    wait
}

builds() {
    mkdir -p "$tmp/record"
    cp "$programs/cache_record/racy.n" "$programs/cache_record/state.n" \
        "$tmp/record/"
    export N00B_PATH="$tmp/record"
    run build.racy.cold r build "$racy" &
    run build.heap.cold off build "$heap"
    wait
    cp -R "$tmp/cache.r" "$tmp/cache.b"
    run build.racy.warm r build "$racy" &
    run build.stack.fill b build "$stack"
    run build.heap.cross b build "$heap"
    wait
    echo >>"$racy"
    run build.racy.edited r build "$racy" &
    run build.racy.edited-cold off build "$racy"
    wait
    run library.cold off library record_root &
    run library.fill l library record_root
    wait
}

checks &
builds &
wait
same check.cold-warm check.hello.cold check.hello.warm
same check.syntax-setting syntax.cold syntax.warm
same build.cold-warm build.racy.cold build.racy.warm
logged build.warm-reuses-record build.racy.warm "reuse record" "store scc"
same build.heap_caller.cross-stack-caller build.heap.cold build.heap.cross
same build.edited-root build.racy.edited-cold build.racy.edited
logged build.edited-rebuilds-record build.racy.edited "reuse scc builtin" \
    "reuse record"
same library.fill library.cold library.fill

LC_ALL=C sort "$tmp/cells" >"$tmp/cells.sorted"
LC_ALL=C sort "$expected" | LC_ALL=C join - "$tmp/cells.sorted" \
    >"$tmp/expected"
LC_ALL=C sort "$tmp/failed" >"$tmp/failed.sorted"
if ! diff -u "$tmp/expected" "$tmp/failed.sorted"; then
    echo "the failing cells differ from $expected"
    exit 1
fi
