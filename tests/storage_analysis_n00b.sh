#!/bin/sh
set -eu

compiler=$1
scalar_program=$2
gc_program=$3

scalar_ir=$("$compiler" lower --dump-ir "$scalar_program")
printf '%s\n' "$scalar_ir" | awk '
    /^  func @copy_(word|count|status|slice)\(/ {
        functions++
        if ($0 !~ /ir.storage_duration\(2\)/ || $0 !~ /ir.alias_analysis_complete\(true\)/) exit 1
        inside = 1
    }
    inside && / = alloc / { exit 1 }
    inside && /^  }/ { inside = 0 }
    END { if (functions != 4) exit 1 }
'

gc_ir=$("$compiler" lower --dump-ir "$gc_program")
printf '%s\n' "$gc_ir" | awk '
    /^  func @prepend\(/ { inside = 1; functions++ }
    inside && / = alloc / {
        allocations++
        if ($0 !~ /alloc .*Chain_Node/) exit 1
    }
    inside && /^  }/ { inside = 0 }
    END { if (functions != 1 || allocations != 1) exit 1 }
'
