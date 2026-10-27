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

"$compiler" lower --dump-ir "$program" >"$tmp/ir" 2>"$tmp/diagnostics" || {
    cat "$tmp/diagnostics" >&2
    exit 1
}
if awk '/^  func @check_type_only_probe/ { inside = 1 }
        inside && /^  }/ { inside = 0 }
        inside && / = alloc / { found = 1 }
        END { exit !found }' "$tmp/ir"; then
    echo 'type-only probe was moved to heap storage' >&2
    exit 1
fi
awk '
    /^  func @copy_(word|count|status|slice)\(/ {
        functions++
        if ($0 !~ /ir.storage_duration\(2\)/ || $0 !~ /ir.alias_analysis_complete\(true\)/) exit 1
        inside = 1
    }
    inside && / = alloc / { exit 1 }
    inside && /^  }/ { inside = 0 }
    END { if (functions != 4) exit 1 }
' "$tmp/ir" || {
    echo 'nominal scalar copies were not stack-only and fully analyzed' >&2
    exit 1
}
awk '
    /^  func @prepend\(/ { inside = 1; functions++ }
    inside && / = alloc / {
        allocations++
        if ($0 !~ /alloc .*Chain_Node/) exit 1
    }
    inside && /^  }/ { inside = 0 }
    END { if (functions != 1 || allocations != 1) exit 1 }
' "$tmp/ir" || {
    echo 'prepend did not place exactly its escaping node on the heap' >&2
    exit 1
}

placed=$(awk '/^  func @/ { name = $2 }
    / = alloc / { print name, $4, $5 }' "$tmp/ir")
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
