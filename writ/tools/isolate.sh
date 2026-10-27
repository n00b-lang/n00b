#!/bin/sh
set -eu

compiler="$1"
out="$2"
raw="$3"
lantern="$4"
exports="$5"
work="$out.parts"

rm -rf "$work"
mkdir -p "$work"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        cp "$raw" "$out"
        rm -rf "$work"
        exit 0
        ;;
    Darwin)
        "$compiler" -r -nostdlib -o "$work/writ.o" -Wl,-all_load "$raw" "$lantern" \
            -Wl,-exported_symbols_list,"$exports"
        ;;
    *)
        "$compiler" -r -nostdlib -o "$work/writ.o" -Wl,--whole-archive "$raw" "$lantern" \
            -Wl,--no-whole-archive
        objcopy -w --keep-global-symbol='writ_*' "$work/writ.o"
        ;;
esac

rm -f "$out"
ar rcs "$out" "$work/writ.o"
rm -rf "$work"
