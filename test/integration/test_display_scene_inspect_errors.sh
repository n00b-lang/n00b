#!/bin/sh

set -eu

export LC_ALL=C

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <display_scene_inspect>" >&2
    exit 1
fi

inspect_tool=$1

if [ ! -e /dev/full ]; then
    echo "[SKIP] /dev/full not available" >&2
    exit 77
fi

set +e
output=$("$inspect_tool" --out /dev/full 2>&1)
status=$?
set -e

if [ "$status" -eq 0 ]; then
    echo "expected $inspect_tool --out /dev/full to fail" >&2
    printf '%s\n' "$output" >&2
    exit 1
fi

case "$output" in
    *"/dev/full"*) ;;
    *)
        echo "expected diagnostic to mention /dev/full" >&2
        printf '%s\n' "$output" >&2
        exit 1
        ;;
esac

case "$output" in
    *"No space left on device"*|*"ENOSPC"*) ;;
    *)
        echo "expected diagnostic to mention the write error" >&2
        printf '%s\n' "$output" >&2
        exit 1
        ;;
esac
