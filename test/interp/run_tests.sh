#!/bin/bash
# run_tests.sh — Integration test runner for the n00b interpreter.
#
# For each .n test file in this directory, runs it through the n00b binary
# and checks:
#   1. Exit code matches .exit file (default: 0)
#   2. Stdout matches .expected file (if present)
#
# Usage:
#   bash test/interp/run_tests.sh [path/to/n00b]
#
# Exit code: 0 if all tests pass, 1 if any fail.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
N00B="${1:-./build_debug/n00b}"

if [ ! -x "$N00B" ]; then
    echo "error: n00b binary not found at $N00B"
    exit 1
fi

PASS=0
FAIL=0
ERRORS=""

for test_file in "$SCRIPT_DIR"/*.n; do
    [ -f "$test_file" ] || continue

    name="$(basename "${test_file%.n}")"
    expected_file="$SCRIPT_DIR/$name.expected"
    exit_file="$SCRIPT_DIR/$name.exit"

    # Default expected exit code is 0.
    expected_exit=0
    if [ -f "$exit_file" ]; then
        expected_exit="$(cat "$exit_file" | tr -d '[:space:]')"
    fi

    # Run the test, capture stdout and exit code.
    actual_out=""
    actual_exit=0
    actual_out=$("$N00B" --quiet "$test_file" 2>/dev/null) || actual_exit=$?

    # Check exit code.
    if [ "$actual_exit" -ne "$expected_exit" ]; then
        FAIL=$((FAIL + 1))
        ERRORS="$ERRORS  FAIL $name: exit code $actual_exit (expected $expected_exit)\n"
        continue
    fi

    # Check stdout if .expected file exists.
    if [ -f "$expected_file" ]; then
        expected_out="$(cat "$expected_file")"
        if [ "$actual_out" != "$expected_out" ]; then
            FAIL=$((FAIL + 1))
            ERRORS="$ERRORS  FAIL $name: stdout mismatch\n"
            ERRORS="$ERRORS    expected: $(head -3 "$expected_file")\n"
            ERRORS="$ERRORS    actual:   $(echo "$actual_out" | head -3)\n"
            continue
        fi
    fi

    PASS=$((PASS + 1))
done

TOTAL=$((PASS + FAIL))

if [ "$FAIL" -gt 0 ]; then
    echo "FAILED: $FAIL/$TOTAL tests failed"
    echo ""
    echo -e "$ERRORS"
    exit 1
else
    echo "OK: $PASS/$TOTAL tests passed"
    exit 0
fi
