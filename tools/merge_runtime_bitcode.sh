#!/usr/bin/env bash
set -euo pipefail

archive="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
output="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
ar="$3"
link="$4"
if [ "$(head -c 7 "$archive")" = '!<thin>' ]; then
    cd "$(dirname "$archive")"
    mapfile -t members < <("$ar" t "$archive")
    "$link" "${members[@]}" -o "$output"
    exit 0
fi
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
cd "$scratch"
"$ar" x "$archive"
"$link" ./*.o -o "$output"
