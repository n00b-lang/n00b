#!/usr/bin/env bash
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

case "${1:-}" in
    ""|compile) exec "$ROOT/build.sh" compile ;;
    test) exec "$ROOT/build.sh" test-lantern ;;
    bench) exec "$ROOT/build.sh" bench ;;
    tu) shift; exec "$ROOT/build.sh" tu "$@" ;;
    clean) exec "$ROOT/build.sh" clean ;;
    *)
        echo "Usage: lantern/build.sh [compile|test|bench|tu <file.c>|clean]" >&2
        echo "lantern builds with the rest of the tree; see ../build.sh." >&2
        exit 1 ;;
esac
