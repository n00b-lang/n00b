#!/bin/bash
# stop.sh — tear down the step-ca fixture started by start.sh.
#
# Usage:
#   STEPCA_PID=... STEPPATH=... bash test/fixtures/stepca/stop.sh
#
# Idempotent: missing pid / dir is fine.

set -u

if [ -n "${STEPCA_PID:-}" ]; then
    kill "$STEPCA_PID" 2>/dev/null || true
    # Poll up to ~5 seconds for the process to release its files
    # (badger DB) before we rm -rf the temp dir.
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 \
              21 22 23 24 25; do
        if ! kill -0 "$STEPCA_PID" 2>/dev/null; then
            break
        fi
        sleep 0.2
    done
    # Force-kill if still alive.
    kill -9 "$STEPCA_PID" 2>/dev/null || true
    wait "$STEPCA_PID" 2>/dev/null || true
fi

if [ -n "${STEPPATH:-}" ] && [ -d "$STEPPATH" ]; then
    case "$STEPPATH" in
        /tmp/*|/var/folders/*)
            rm -rf "$STEPPATH"
            ;;
        *)
            echo "stop.sh: refusing to remove non-temp STEPPATH=$STEPPATH" >&2
            ;;
    esac
fi
