#!/bin/sh
set -eu

compiler=$1
program_dir=$2
contract_dir=$3
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

"$compiler" build --contract-dir "$contract_dir" \
    --show-ffi zlib.__init.zlib zlib_round_trip.n00b \
    >"$tmp/show-ffi" 2>"$tmp/show-ffi.log" || {
    cat "$tmp/show-ffi.log" >&2
    exit 1
}
diff -u zlib.show-ffi.expected "$tmp/show-ffi"
test ! -s "$tmp/show-ffi.log"
test ! -e a.out

if "$compiler" build --contract-dir "$contract_dir" \
    --output "$tmp/renamed" zlib_renamed_export.n00b \
    >"$tmp/renamed.log" 2>&1; then
    echo "zlib_renamed_export unexpectedly built" >&2
    exit 1
fi
grep -q 'N00BB-M0007' "$tmp/renamed.log"
