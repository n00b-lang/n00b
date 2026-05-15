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
TEST_TIMEOUT="${N00B_INTERP_TIMEOUT:-60s}"
JOBS="${N00B_INTERP_JOBS:-${N00B_JOBS:-}}"

if [ ! -x "$N00B" ]; then
    echo "error: n00b binary not found at $N00B"
    exit 1
fi

if [ -z "$JOBS" ]; then
    if command -v nproc >/dev/null 2>&1; then
        JOBS="$(nproc)"
    else
        JOBS=4
    fi

    if [ "$JOBS" -gt 8 ]; then
        JOBS=8
    fi
fi

TMP_BASE="${TMPDIR:-/tmp}"
RUN_TMP="$(mktemp -d "$TMP_BASE/n00b-interp.XXXXXX")"
trap 'rm -rf "$RUN_TMP"' EXIT

run_one() {
    local test_file="$1"
    local name expected_file exit_file expected_exit actual_exit
    local out_file err_file result_file

    name="$(basename "${test_file%.n}")"
    expected_file="$SCRIPT_DIR/$name.expected"
    exit_file="$SCRIPT_DIR/$name.exit"
    out_file="$RUN_TMP/$name.out"
    err_file="$RUN_TMP/$name.err"
    result_file="$RUN_TMP/$name.result"

    expected_exit=0
    if [ -f "$exit_file" ]; then
        expected_exit="$(cat "$exit_file" | tr -d '[:space:]')"
    fi

    actual_exit=0
    if command -v timeout >/dev/null 2>&1; then
        timeout "$TEST_TIMEOUT" "$N00B" run --quiet "$test_file" \
            >"$out_file" 2>"$err_file" || actual_exit=$?
    else
        "$N00B" run --quiet "$test_file" >"$out_file" 2>"$err_file" \
            || actual_exit=$?
    fi

    if [ "$actual_exit" -ne "$expected_exit" ]; then
        {
            echo "FAIL"
            if [ "$actual_exit" -eq 124 ]; then
                echo "  FAIL $name: timed out after $TEST_TIMEOUT"
            else
                echo "  FAIL $name: exit code $actual_exit (expected $expected_exit)"
            fi
        } >"$result_file"
        return 0
    fi

    if [ -f "$expected_file" ]; then
        if ! cmp -s "$expected_file" "$out_file"; then
            {
                echo "FAIL"
                echo "  FAIL $name: stdout mismatch"
                echo "    expected bytes: $(wc -c <"$expected_file" | tr -d '[:space:]')"
                echo "    actual bytes:   $(wc -c <"$out_file" | tr -d '[:space:]')"
                echo "    expected: $(head -3 "$expected_file")"
                echo "    actual:   $(head -3 "$out_file")"
            } >"$result_file"
            return 0
        fi
    fi

    echo "PASS" >"$result_file"
}

export SCRIPT_DIR N00B TEST_TIMEOUT RUN_TMP
export -f run_one

PASS=0
FAIL=0
ERRORS=""

TEST_FILES=()
while IFS= read -r test_file; do
    TEST_FILES+=("$test_file")
done < <(find "$SCRIPT_DIR" -maxdepth 1 -type f -name '*.n' | sort)

if [ "${#TEST_FILES[@]}" -gt 0 ]; then
    printf '%s\0' "${TEST_FILES[@]}" \
        | xargs -0 -n1 -P "$JOBS" bash -c 'run_one "$0"'
fi

for test_file in "${TEST_FILES[@]}"; do
    name="$(basename "${test_file%.n}")"
    result_file="$RUN_TMP/$name.result"

    if [ ! -f "$result_file" ]; then
        FAIL=$((FAIL + 1))
        ERRORS="$ERRORS  FAIL $name: runner did not write a result\n"
        continue
    fi

    if [ "$(sed -n '1p' "$result_file")" = "PASS" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        ERRORS="$ERRORS$(sed -n '2,$p' "$result_file")\n"
    fi
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
