#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
N00B="${1:-$REPO_ROOT/build_debug/n00b}"

python3 "$SCRIPT_DIR/prove_collated_006_large_literals.py" --n00b "$N00B"
python3 "$SCRIPT_DIR/prove_collated_009_kwarg_rescan.py"
python3 "$SCRIPT_DIR/prove_collated_010_codegen_error_exec.py" --n00b "$N00B"
