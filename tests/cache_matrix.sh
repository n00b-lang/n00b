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

stack="$programs/cache_barrier/stack_caller.n"
heap="$programs/cache_barrier/heap_caller.n"
racy="$tmp/record/racy.n"
slots=${N00B_MATRIX_JOBS:-4}

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
    label=$1 cache=$2 target=$3 extra=${4:-}
    acquire
    out="$tmp/$label"
    mkdir -p "$out"
    if [ "$cache" = off ]; then
        set -- env N00B_NO_OBJECT_CACHE=1 N00B_CACHE="$tmp/off.$label"
    else
        set -- env N00B_CACHE="$tmp/cache.$cache"
    fi
    "$@" "$compiler" build $extra "$target" -o "$out/program" \
        >"$out/stdout" 2>"$out/diagnostics"
    echo "status $?" >>"$out/diagnostics"
    [ -f "$out/program" ] && image "$out/program" >"$out/image"
    rm -rf "$out/program" "$out/program.dSYM"
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

mkdir -p "$tmp/record" "$tmp/syntax"
cp "$programs/cache_record/racy.n" "$programs/cache_record/state.n" \
    "$tmp/record/"
cp "$programs/syntax_preferences.n" "$tmp/syntax/prefs.n"
printf 'import prefs\n\nfunc main() {\n}\n' >"$tmp/syntax/main.n"
syntax="$tmp/syntax/main.n"
export N00B_PATH="$tmp/record:$tmp/syntax"
run build.racy.cold r "$racy" &
run build.heap.cold off "$heap"
wait
for copy in b s so; do cp -R "$tmp/cache.r" "$tmp/cache.$copy"; done
run build.racy.warm r "$racy" &
(run build.stack.fill b "$stack"; run build.heap.cross b "$heap") &
(run syntax.fill s "$syntax"; run syntax.warm s "$syntax" --no-syntax-warnings) &
run syntax.cold so "$syntax" --no-syntax-warnings
wait
echo >>"$racy"
run build.racy.edited r "$racy" &
run build.racy.edited-cold off "$racy"
wait
same build.cold-warm build.racy.cold build.racy.warm
same build.heap_caller.cross-stack-caller build.heap.cold build.heap.cross
same build.edited-root build.racy.edited-cold build.racy.edited
same build.syntax-setting syntax.cold syntax.warm

LC_ALL=C sort "$tmp/cells" >"$tmp/cells.sorted"
LC_ALL=C sort "$expected" | LC_ALL=C join - "$tmp/cells.sorted" \
    >"$tmp/expected"
LC_ALL=C sort "$tmp/failed" >"$tmp/failed.sorted"
if ! diff -u "$tmp/expected" "$tmp/failed.sorted"; then
    echo "the failing cells differ from $expected"
    exit 1
fi
