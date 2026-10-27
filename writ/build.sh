#!/usr/bin/env bash
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

case "${1:-}" in
    ""|compile) exec "$ROOT/build.sh" compile ;;
    test) exec "$ROOT/build.sh" test-writ ;;
    linux) shift; exec "$ROOT/build.sh" linux test-writ ;;
    clean) exec "$ROOT/build.sh" clean ;;
    *)
        echo "Usage: writ/build.sh [compile|test|linux|clean]" >&2
        echo "writ builds with the rest of the tree; see ../build.sh." >&2
        exit 1 ;;
esac
