#!/bin/sh
set -eu

compiler=$1
program_dir=$2
contract_dir=$3
register_object=$4
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cd "$program_dir"

builds_and_runs() {
    name=$1
    shift
    if ! "$compiler" build --contract-dir "$contract_dir" "$@" \
        --output "$tmp/$name" "$name.n00b" >"$tmp/$name.log" 2>&1; then
        cat "$tmp/$name.log" >&2
        exit 1
    fi
    "$tmp/$name"
}

builds_and_runs zlib_round_trip
builds_and_runs zlib_round_trip --lto
builds_and_runs zlib_subpackage
builds_and_runs zlib_subpackage --lto
builds_and_runs zlib_const
nm "$tmp/zlib_const" | grep -q 'zlib\.__ffi___init_zlib\.uncompress$'
if nm "$tmp/zlib_const" | grep -q 'zlib\.__ffi___init_zlib\.compress$'; then
    echo "zlib_const shipped the build-time compress call" >&2
    exit 1
fi
builds_and_runs zlib_litmod
if nm "$tmp/zlib_litmod" | grep -qE 'n00b_ffi_[0-9]+_| U _?(crc32|compress2|inflate|deflate)$'; then
    echo "zlib_litmod linked zlib without using it at run time" >&2
    exit 1
fi
builds_and_runs zlib_two_modules
if grep -q 'duplicate libraries' "$tmp/zlib_two_modules.log"; then
    cat "$tmp/zlib_two_modules.log" >&2
    exit 1
fi

"$compiler" build --contract-dir "$contract_dir" \
    --show-ffi zlib.__init.zlib zlib_round_trip.n00b \
    >"$tmp/show-ffi" 2>"$tmp/show-ffi.log" || {
    cat "$tmp/show-ffi.log" >&2
    exit 1
}
grep -v 'ZLIB_VER' zlib.show-ffi.expected >"$tmp/show-ffi.expected"
grep -v 'ZLIB_VER' "$tmp/show-ffi" >"$tmp/show-ffi.actual"
diff -u "$tmp/show-ffi.expected" "$tmp/show-ffi.actual"
test ! -s "$tmp/show-ffi.log"
test ! -e a.out

if "$compiler" build --contract-dir "$contract_dir" \
    --output "$tmp/renamed" zlib_renamed_export.n00b \
    >"$tmp/renamed.log" 2>&1; then
    echo "zlib_renamed_export unexpectedly built" >&2
    exit 1
fi
grep -q 'N00BB-M0007' "$tmp/renamed.log"

N00B_PATH="$program_dir" "$compiler" library --contract-dir "$contract_dir" \
    --output "$tmp/zlib.n00bm" zlib.__init >"$tmp/library.log" 2>&1 || {
    cat "$tmp/library.log" >&2
    exit 1
}
shim=n00b_ffi_0
"$register_object" "$tmp/zlib.n00bm" '>=2' \
    --ffi-module zlib.__ffi___init_zlib \
    Error@1 Stream@7 Deflate_Outcome Inflate_Outcome@108 Z_OK@37 Z_BUF_ERROR \
    ZLIB_VERSION deflate deflate_end inflate inflate_end compress@157 \
    compress_bound uncompress@178 adler32 crc32 deflate_init inflate_init \
    __ffi_take_bytes \
    "__deflate=${shim}_n00b_deflate" "__deflateEnd=${shim}_n00b_deflateEnd" \
    "__inflate=${shim}_n00b_inflate" "__inflateEnd=${shim}_n00b_inflateEnd" \
    "__compress2=${shim}_n00b_compress2" \
    "__compressBound=${shim}_n00b_compressBound" \
    "__uncompress=${shim}_n00b_uncompress" "__adler32=${shim}_n00b_adler32" \
    "__crc32=${shim}_n00b_crc32" "__deflateInit=${shim}_n00b_deflateInit" \
    "__inflateInit=${shim}_n00b_inflateInit" \
    "__ffi_bytes_take=${shim}_take_bytes"
