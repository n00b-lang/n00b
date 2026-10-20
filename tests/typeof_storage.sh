#!/bin/sh
set -eu

compiler=$1
program=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$compiler" lower --dump-symbols "$program" >"$tmp/symbols" 2>"$tmp/diagnostics"
if ! grep -q 'probe: variable.*storage-duration=automatic' "$tmp/symbols"; then
    cat "$tmp/symbols" "$tmp/diagnostics" >&2
    exit 1
fi
"$compiler" lower --dump-ir "$program" >"$tmp/ir" 2>"$tmp/diagnostics"
if awk '/^  func @check_type_only_probe/ { inside = 1 }
        inside && /^  }/ { inside = 0 }
        inside && / = alloc / { found = 1 }
        END { exit !found }' "$tmp/ir"; then
    echo 'type-only probe was moved to heap storage' >&2
    exit 1
fi
