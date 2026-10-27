#!/bin/sh
set -eu

compiler=$1
program_dir=$2
part=$3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cd "$program_dir"

builds_and_runs() {
    name=$1
    shift
    if ! "$compiler" build "$@" --output "$tmp/$name" "$name.n00b" \
        >"$tmp/$name.log" 2>&1; then
        cat "$tmp/$name.log" >&2
        exit 1
    fi
    "$tmp/$name"
}

reported() {
    if ! grep -F -- "$2" "$tmp/rejected.log" | grep -Fq -- "$1"; then
        echo "expected '$1' with '$2'" >&2
        cat "$tmp/rejected.log" >&2
        exit 1
    fi
}

case $part in
basics)
    builds_and_runs ffi_basics
    for marker in marker-literal-5be1 marker-contract-5be1; do
        if grep -q -a "$marker" "$tmp/ffi_basics"; then
            echo "ffi_basics contains $marker" >&2
            exit 1
        fi
    done

    if "$compiler" check unprovided_callback.n00b provided_missing.n00b \
        provide_mismatch.n00b export_same_as.n00b missing.n00b \
        missing_library.n00b extern_hidden.n00b malformed.n00b \
        binding_error.n00b local.n00b not_const.n00b expression.n00b \
        shadow.n00b shadow_alias.n00b >"$tmp/rejected.log" 2>&1; then
        echo "the rejected ffi programs checked" >&2
        exit 1
    fi
    if grep -q 'LANTERN-L00' "$tmp/rejected.log"; then
        cat "$tmp/rejected.log" >&2
        exit 1
    fi
    reported unprovided_callback.n00b N00B-F1509
    reported 'N00B-F0006' 'an '"'"'ffi binding provides `provided_missing.missing`'
    reported provide_mismatch.n00b:11:5 error
    reported export_same_as.n00b:14:5 error
    reported missing.n00b 'is not on the contract search path'
    reported missing_library.n00b:2:1 'N00B-F0001'
    reported missing_library.n00b '`n00bb_absent_library`'
    reported extern_hidden.n00b '`__abs_raw`'
    reported malformed.n00b:5:30 error
    reported binding_error.n00b:5:5 'N00B-F1501'
    for name in local not_const; do
        reported "$name.n00b" 'N00B-P0030: a module literal must initialize exactly one `const`'
    done
    reported expression.n00b 'N00B-P0030: a module literal can only initialize a module-level `const`'
    for name in shadow shadow_alias; do
        reported "$name.n00b:" "literal modifier \`'ffi\` for fence literals is already declared in module \`builtin.ffi\`"
    done

    if "$compiler" check --contract-dir broken broken_contract.n00b \
        >"$tmp/broken_contract.log" 2>&1; then
        echo "broken_contract unexpectedly checked" >&2
        exit 1
    fi
    grep -q 'broken/broken.writ:3:1' "$tmp/broken_contract.log"
    test "$(grep -c 'error N00B-F1001:' "$tmp/broken_contract.log")" = 1
    ;;
package)
    builds_and_runs package_contracts
    ;;
linked)
    set --
    for source in text/text.c kept/kept.c keywords/keywords.c \
        cursor/cursor.c callbacks/hooks.c sorter/sorter.c guard/guard.c \
        kv/kv.c dispatch/dispatch.c; do
        object="$tmp/$(basename "$source" .c).o"
        "${CC:-cc}" -c "$source" -o "$object"
        set -- "$@" --link-object "$object"
    done
    "$compiler" build "$@" \
        --native-root ffi_linked.dispatch_doubler=n00b_callback_table_dispatch_doubler \
        --output "$tmp/ffi_linked" ffi_linked.n00b >"$tmp/ffi_linked.log" 2>&1 || {
        cat "$tmp/ffi_linked.log" >&2
        exit 1
    }
    "$tmp/ffi_linked" 2>"$tmp/ffi_linked.err" || {
        cat "$tmp/ffi_linked.err" >&2
        exit 1
    }
    grep -q 'panic: negative input' "$tmp/ffi_linked.err"
    test "$(tail -c 2 "$tmp/ffi_linked.err")" = ok
    ;;
serve)
    "$compiler" lower --output "$tmp/kv_serve.ir" kv_serve.n00b >"$tmp/kv_serve.log" 2>&1 \
        || { cat "$tmp/kv_serve.log" >&2; exit 1; }
    "${CC:-cc}" -I"$tmp" -c kv/kv_client.c -o "$tmp/kv_client.o"
    "${CC:-cc}" -I"$tmp" -c kv/kv_thread.c -o "$tmp/kv_thread.o"
    (builds_and_runs kv_serve --link-object "$tmp/kv_client.o" \
        --link-object "$tmp/kv_thread.o") 2>"$tmp/kv_serve.err" || {
        cat "$tmp/kv_serve.err" >&2
        exit 1
    }
    ;;
trip)
    "$compiler" lower --output "$tmp/trip_serve.ir" trip_serve.n00b >"$tmp/trip_serve.log" 2>&1 \
        || { cat "$tmp/trip_serve.log" >&2; exit 1; }
    "${CC:-cc}" -I"$tmp" -c trip/trip_client.c -o "$tmp/trip_client.o"
    "$compiler" build --link-object "$tmp/trip_client.o" --output "$tmp/trip_serve" trip_serve.n00b \
        >"$tmp/trip_serve.log" 2>&1 || { cat "$tmp/trip_serve.log" >&2; exit 1; }
    if "$tmp/trip_serve" 2>"$tmp/trip_serve.err"; then
        echo "trip_serve survived an on_panic abort" >&2
        exit 1
    fi
    grep -q 'panic: tripped' "$tmp/trip_serve.err"
    ;;
edit)
    mkdir "$tmp/source"
    cp contracts/limits.writ "$tmp/source/limits.writ"
    cd "$tmp/source"
    cat >edit.n00b <<'EOF'
const limits = [-
use <limits.writ>
-]'ffi

require limits.LIMIT == 40
EOF
    builds_and_runs edit --contract-dir .
    printf 'library limits\nconst LIMIT: c_int = 41\n' > limits.writ
    "$compiler" build --contract-dir . --output "$tmp/edited" edit.n00b \
        >"$tmp/edited.log" 2>&1 || { cat "$tmp/edited.log" >&2; exit 1; }
    if "$tmp/edited" 2>/dev/null; then
        echo "edited contract did not change the build" >&2
        exit 1
    fi
    ;;
*)
    echo "unknown part $part" >&2
    exit 2
    ;;
esac
