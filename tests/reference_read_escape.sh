#!/bin/sh
set -eu

compiler=$1
source=$2
output=$("$compiler" lower --dump-ir "$source" 2>&1)
placed=$(printf '%s\n' "$output" | awk '/^  func @/ { name = $2 }
    / = alloc / { print name, $4, $5 }')

expect() {
    if ! printf '%s\n' "$placed" | grep -Fqx "$1"; then
        echo "missing heap placement: $1" >&2
        exit 1
    fi
}

expect '@publish_local() alloc int'
expect '@return_local() alloc int'
expect '@expose_slot() alloc int'
expect '@expose_slot() alloc Option[ref[int]]'
for name in publish_local return_local; do
    if printf '%s\n' "$placed" | grep -Fq "@$name() alloc Option"; then
        echo "reference slot escaped with the value read through it" >&2
        exit 1
    fi
done
