#!/bin/bash
# stop.sh — tear down the zot OCI registry fixture.
#
# Usage:
#   ZOT_CONTAINER=... ZOT_CERT_DIR=... bash test/fixtures/zot/stop.sh
#
# Idempotent: missing container / cert dir is fine.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -n "${ZOT_CONTAINER:-}" ] && command -v docker >/dev/null; then
    # `down` removes the container, network, and any anonymous volumes
    # attached via the compose project.
    docker compose -f "$SCRIPT_DIR/docker-compose.yml" \
        -p "$ZOT_CONTAINER" down --timeout 5 \
        >/dev/null 2>&1 || true
    # Belt-and-suspenders: if the container outlived compose down for
    # any reason, force-remove it by name.
    docker rm -f "$ZOT_CONTAINER" >/dev/null 2>&1 || true
fi

# Refuse to wipe anything outside the temp roots — defensive against
# a stale env-var pointing somewhere user-meaningful.
if [ -n "${ZOT_CERT_DIR:-}" ] && [ -d "$ZOT_CERT_DIR" ]; then
    case "$ZOT_CERT_DIR" in
        /tmp/*|/var/folders/*)
            rm -rf "$ZOT_CERT_DIR"
            ;;
        *)
            echo "stop.sh: refusing to remove non-temp ZOT_CERT_DIR=$ZOT_CERT_DIR" >&2
            ;;
    esac
fi
