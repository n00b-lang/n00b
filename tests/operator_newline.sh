#!/bin/sh
set -eu

compiler=$1
valid=$2
invalid=$3

"$compiler" parse "$valid" >/dev/null
if "$compiler" parse "$invalid" >/dev/null 2>&1; then
    echo 'two newlines after a binary operator unexpectedly parsed' >&2
    exit 1
fi
