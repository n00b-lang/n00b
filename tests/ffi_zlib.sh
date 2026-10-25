#!/bin/sh
set -eu

compiler=$1
program_dir=$2
contract_dir=$3
register_object=$4
part=$5
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cd "$program_dir"

builds_and_runs() {
    name=$1
    shift
    if ! "$compiler" build --contract-dir "$contract_dir" "$@" \
        --output "$tmp/$name" "$name.n" >"$tmp/$name.log" 2>&1; then
        cat "$tmp/$name.log" >&2
        exit 1
    fi
    "$tmp/$name"
}

case $part in
const)
    builds_and_runs zlib_const
    nm "$tmp/zlib_const" | grep -q 'zlib\.__ffi___init___ffi_literal_1\.uncompress$'
    if nm "$tmp/zlib_const" | grep -q 'zlib\.__ffi___init___ffi_literal_1\.compress$'; then
        echo "zlib_const shipped the build-time compress call" >&2
        exit 1
    fi
    ;;
litmod)
    builds_and_runs zlib_litmod
    if nm "$tmp/zlib_litmod" | grep -qE 'n00b_ffi_zlib_| U _?(crc32|compress2|inflate|deflate)$'; then
        echo "zlib_litmod linked zlib without using it at run time" >&2
        exit 1
    fi
    ;;
modules)
    builds_and_runs zlib_two_modules
    if grep -q 'duplicate libraries' "$tmp/zlib_two_modules.log"; then
        cat "$tmp/zlib_two_modules.log" >&2
        exit 1
    fi

    "$compiler" build --contract-dir "$contract_dir" \
        --show-ffi zlib.__init:1 zlib_const.n \
        >"$tmp/show-ffi" 2>"$tmp/show-ffi.log" || {
        cat "$tmp/show-ffi.log" >&2
        exit 1
    }
    expected=zlib.show-ffi.expected
    case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) expected=zlib.show-ffi.llp64.expected ;;
    esac
    grep -v 'ZLIB_VER' "$expected" >"$tmp/show-ffi.expected"
    grep -v 'ZLIB_VER' "$tmp/show-ffi" >"$tmp/show-ffi.actual"
    diff -u "$tmp/show-ffi.expected" "$tmp/show-ffi.actual"
    test ! -s "$tmp/show-ffi.log"
    test ! -e zlib_const

    # A manifest re-exports a literal's names like any other member, and
    # two literals' failures share `Ffi_Error`, so one handler takes both.
    if ! "$compiler" check --contract-dir "$contract_dir" zlib_renamed_export.n \
        zlib_mixed_try.n >"$tmp/renamed.log" 2>&1; then
        cat "$tmp/renamed.log" >&2
        exit 1
    fi
    if grep -q 'N00B-I' "$tmp/renamed.log"; then
        cat "$tmp/renamed.log" >&2
        exit 1
    fi

    N00B_PATH="$program_dir" "$compiler" library --contract-dir "$contract_dir" \
        --output "$tmp/zlib.n00bm" zlib.__init >"$tmp/library.log" 2>&1 || {
        cat "$tmp/library.log" >&2
        exit 1
    }
    shim=n00b_ffi_zlib___ffi___init___ffi_literal_1
    "$register_object" "$tmp/zlib.n00bm" '>=2' \
        --ffi-module zlib.__ffi___init___ffi_literal_1 \
        Stream@2 Deflate_Outcome Inflate_Outcome@103 Z_OK@32 Z_BUF_ERROR \
        ZLIB_VERSION deflate deflate_end inflate inflate_end compress@152 \
        compress_bound uncompress@173 adler32 crc32 deflate_init inflate_init \
        __ffi_take_bytes \
        "__deflate=${shim}_n00b_deflate" "__deflateEnd=${shim}_n00b_deflateEnd" \
        "__inflate=${shim}_n00b_inflate" "__inflateEnd=${shim}_n00b_inflateEnd" \
        "__compress2=${shim}_n00b_compress2" \
        "__compressBound=${shim}_n00b_compressBound" \
        "__uncompress=${shim}_n00b_uncompress" "__adler32=${shim}_n00b_adler32" \
        "__crc32=${shim}_n00b_crc32" "__deflateInit=${shim}_n00b_deflateInit" \
        "__inflateInit=${shim}_n00b_inflateInit" \
        "__ffi_bytes_take=${shim}_take_bytes"
    ;;
*)
    echo "unknown part $part" >&2
    exit 2
    ;;
esac
