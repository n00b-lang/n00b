#!/usr/bin/env bash
# Run the oracle chalk's extract against an artifact, after pinning
# its TIMESTAMP_WHEN_CHALKED to a caller-supplied value.
#
# Usage:  run_oracle_extract.sh <timestamp_ms> <artifact_path>
#
# Requires:
#   CHALK_ORACLE_BINARY  path to a built oracle chalk
#
# Writes the timestamp to ~/.chalk-oracle-ts (the file the oracle
# patch reads from in system.nim) and prints chalk extract's JSON
# report to stdout.
#
# Exits 77 (TAP "skip") if CHALK_ORACLE_BINARY isn't set, so test
# runners can gate roundtrip tests on the oracle's presence.

set -euo pipefail

if [[ -z "${CHALK_ORACLE_BINARY:-}" ]]; then
    echo "CHALK_ORACLE_BINARY not set; skipping oracle round-trip" >&2
    exit 77
fi
if [[ "$#" -ne 2 ]]; then
    echo "usage: $0 <timestamp_ms> <artifact_path>" >&2
    exit 2
fi

TS=$1
ART=$2

printf '%s' "$TS" > "$HOME/.chalk-oracle-ts"
trap 'rm -f "$HOME/.chalk-oracle-ts"' EXIT

# Chalk routes its mark report to a configured sink (default: a log
# file under ~/.local/chalk). Validation errors are on stderr at info+
# log level. We capture both streams; tests look for libchalk-written
# CHALK_ID / METADATA_ID values in the combined output, which appear
# both in chalk's mark trace logs and in any validation error.
"$CHALK_ORACLE_BINARY" --log-level=info extract "$ART" 2>&1
