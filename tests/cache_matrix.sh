#!/bin/sh
set -u

build=$1
tests=$2
expected=$3
scope=${4:-full}
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
records="$programs/merged/records.n"
text="$programs/text/text_suite.n"
threads="$programs/buffer/buffer_thread_slice.n"

image() {
    "$llvm/llvm-objdump" -s "$1" | awk '
        /^Contents of section / {
            keep = $0 !~ /\.note\.gnu\.build-id|\.debug_|__DWARF|\.gnu_debuglink/
        }
        /file format/ { next }
        keep'
    "$llvm/llvm-nm" -P "$1" | LC_ALL=C sort
}

run() {
    label=$1 cache=$2 command=$3 target=$4 extra=${5:-}
    out="$tmp/$label"
    mkdir -p "$out"
    if [ "$cache" = off ]; then
        set -- env N00B_NO_OBJECT_CACHE=1 N00B_CACHE="$tmp/off.$label"
    else
        set -- env N00B_CACHE="$tmp/cache.$cache"
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
            awk '/^summary @/ { name = $2; print name "\t" $0; next }
                 /^  / && name != "" { print name "\t" $0; next }
                 { name = ""; print }' "$out/dump" | LC_ALL=C sort \
                >"$out/summaries"
            rm -f "$out/dump"
            ;;
        build|build-summaries)
            flags=
            [ "$command" = build-summaries ] && flags=--function-summaries
            "$@" "$compiler" build $flags "$target" -o "$out/program" \
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

cold() {
    run check.hello.off off check "$hello"
    run check.hello.off2 off check "$hello"
    run check.records.off off check "$records"
    run check.text.off off check "$text"
    run check.threads.off off check "$threads"
    run build.hello.off off build "$hello"
    run build.hello.off2 off build "$hello"
    run build.records.off off build "$records"
    run build.summaries.off off build-summaries "$hello"
    run library.off off library record_root
}

filled_by_hello() {
    run check.hello.fill h check "$hello"
    run check.hello.warm h check "$hello"
    run check.records.cross h check "$records"
    [ "$scope" = pr ] && return
    run check.text.cross h check "$text"
    run check.text.warm h check "$text"
    run check.threads.cross-hello h check "$threads"
    run build.hello.fill bh build "$hello"
    run build.hello.warm bh build "$hello"
    run build.records.cross bh build "$records"
}

filled_by_records() {
    run check.records.fill r check "$records"
    run check.records.warm r check "$records"
    run check.hello.cross r check "$hello"
    [ "$scope" = pr ] && return
    run check.threads.cross-records r check "$threads"
    run build.records.fill br build "$records"
    run build.records.warm br build "$records"
    run build.hello.cross br build "$hello"
    run build.summaries.cross br build-summaries "$hello"
    run library.cross br library record_root
}

library_cache() {
    run library.fill lr library record_root
    run library.warm lr library record_root
}

built_hello() {
    run build.hello.fill bh build "$hello"
    run build.hello.warm bh build "$hello"
}

syntax_settings() {
    mkdir -p "$tmp/syntax"
    cp "$programs/syntax_aliases.n" "$tmp/syntax/aliases.n"
    cp "$programs/syntax_preferences.n" "$tmp/syntax/prefs.n"
    printf 'import aliases\nimport prefs\n' >"$tmp/syntax/main.n"
    run syntax.fill s syntax ""
    for setting in default no-warnings function-fn block-colon; do
        case $setting in
            default) flags= ;;
            no-warnings) flags=--no-syntax-warnings ;;
            function-fn) flags="--syntax-preference function=fn" ;;
            block-colon) flags="--syntax-preference block=:" ;;
        esac
        run "syntax.off.$setting" off syntax "" "$flags"
        run "syntax.warm.$setting" s syntax "" "$flags"
    done
}

if [ "$scope" = pr ]; then
    filled_by_hello
    filled_by_records
    built_hello
    same check.hello.warm check.hello.fill check.hello.warm
    same check.hello.cross-records check.hello.fill check.hello.cross
    same check.records.warm check.records.fill check.records.warm
    same check.records.cross-hello check.records.fill check.records.cross
    same build.hello.warm build.hello.fill build.hello.warm
else
    cold &
    filled_by_hello &
    filled_by_records &
    library_cache &
    syntax_settings &
    wait
    for setting in default no-warnings function-fn block-colon; do
        same "check.syntax-warnings.$setting" "syntax.off.$setting" \
            "syntax.warm.$setting"
    done
    same check.hello.cold-again check.hello.off check.hello.off2
    same check.hello.fill check.hello.off check.hello.fill
    same check.hello.warm check.hello.off check.hello.warm
    same check.hello.cross-records check.hello.off check.hello.cross
    same check.records.fill check.records.off check.records.fill
    same check.records.warm check.records.off check.records.warm
    same check.records.cross-hello check.records.off check.records.cross
    same check.text_suite.cross-hello check.text.off check.text.cross
    same check.text_suite.warm check.text.off check.text.warm
    same check.threads.cross-hello check.threads.off \
        check.threads.cross-hello
    same check.threads.cross-records check.threads.off \
        check.threads.cross-records
    same build.hello.cold-again build.hello.off build.hello.off2
    same build.hello.fill build.hello.off build.hello.fill
    same build.hello.warm build.hello.off build.hello.warm
    same build.hello.cross-records build.hello.off build.hello.cross
    same build.records.fill build.records.off build.records.fill
    same build.records.warm build.records.off build.records.warm
    same build.records.cross-hello build.records.off build.records.cross
    same build.hello-function-summaries.cross-records build.summaries.off \
        build.summaries.cross
    same library.record_root.fill library.off library.fill
    same library.record_root.warm library.off library.warm
    same library.record_root.cross-records library.off library.cross
fi

LC_ALL=C sort "$tmp/cells" >"$tmp/cells.sorted"
LC_ALL=C sort "$expected" | LC_ALL=C join - "$tmp/cells.sorted" \
    >"$tmp/expected"
LC_ALL=C sort "$tmp/failed" >"$tmp/failed.sorted"
if ! diff -u "$tmp/expected" "$tmp/failed.sorted"; then
    echo "the failing cells differ from $expected"
    exit 1
fi
