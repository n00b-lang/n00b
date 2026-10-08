#!/bin/sh
set -eu

compiler=$1
program_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$compiler" build --output "$tmp/ok" "$program_dir/bitcode_runtime_ok.n00b"
"$tmp/ok"
"$compiler" build --output "$tmp/trap" "$program_dir/bitcode_runtime_trap.n00b"
if (ulimit -c 0; "$tmp/trap") >/dev/null 2>&1; then
    echo 'failing require unexpectedly returned' >&2
    exit 1
fi

N00B_PATH="$program_dir" "$compiler" build --output "$tmp/importer" \
    "$program_dir/bitcode_importer.n00b"
if (ulimit -c 0; "$tmp/importer") >/dev/null 2>&1; then
    echo 'imported initializer was not executed' >&2
    exit 1
fi
