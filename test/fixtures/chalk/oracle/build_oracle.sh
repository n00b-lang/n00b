#!/usr/bin/env bash
# Build the oracle chalk binary from a clean checkout.
#
# Usage:  build_oracle.sh /path/to/chalk-checkout
#
# Steps:
#   1. cd into the chalk checkout
#   2. Apply oracle_patch.diff via git apply
#   3. make (which builds chalk via nimble + Nim)
#
# The resulting binary lives at <chalk-checkout>/chalk and should be
# exported as CHALK_ORACLE_BINARY when running libchalk tests.
#
# This script does NOT run the oracle build in a libchalk dev
# environment by default — it requires Nim 2.0.8+ + nimble.

set -euo pipefail

if [[ "$#" -ne 1 ]]; then
    echo "usage: $0 /path/to/chalk-checkout" >&2
    exit 2
fi

CHALK_DIR=$1
PATCH_DIR=$(cd "$(dirname "$0")" && pwd)

if [[ ! -d "$CHALK_DIR" ]]; then
    echo "$CHALK_DIR: not a directory" >&2
    exit 1
fi

cd "$CHALK_DIR"

if [[ ! -f src/configs/base_keyspecs.c4m ]]; then
    echo "$CHALK_DIR: doesn't look like a chalk checkout" >&2
    exit 1
fi

# Inline patch — sed-based rather than git apply so this works against
# any clean checkout regardless of git history.
sed -i.bak 's/chalk_version :=   "1\.0\.2"/chalk_version :=   "2.0.0"/' \
    src/configs/base_keyspecs.c4m

# Patch system.nim: replace TIMESTAMP_WHEN_CHALKED with the pinned read.
python3 - <<'PYEOF'
import re, pathlib
p = pathlib.Path('src/plugins/system.nim')
src = p.read_text()
old = 'result.setIfNeeded("TIMESTAMP_WHEN_CHALKED", startTime.toUnixInMs())'
pin = '''block:
    var pinned: int64 = startTime.toUnixInMs()
    try:
      let raw = readFile(getHomeDir() / ".chalk-oracle-ts").strip()
      if raw.len > 0: pinned = parseBiggestInt(raw)
    except CatchableError: discard
    result.setIfNeeded("TIMESTAMP_WHEN_CHALKED", pinned)'''
if old not in src:
    raise SystemExit('TIMESTAMP_WHEN_CHALKED setIfNeeded not found in system.nim')
src = src.replace(old, pin)
# Ensure imports for readFile, parseBiggestInt, getHomeDir are present.
if 'import std/strutils' not in src:
    src = 'import std/strutils\n' + src
if 'import std/os' not in src:
    src = 'import std/os\n' + src
p.write_text(src)
PYEOF

echo "Building chalk (this requires Nim + nimble) ..."
make

echo
echo "Built: $CHALK_DIR/chalk"
echo "Export it for libchalk tests:"
echo "  export CHALK_ORACLE_BINARY=$CHALK_DIR/chalk"
