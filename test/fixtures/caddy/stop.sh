#!/bin/bash
# stop.sh — tear down the caddy H3 fixture.
#
# Usage:
#   CADDY_CONTAINER=... bash test/fixtures/caddy/stop.sh
#
# Idempotent: missing container is fine.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -n "${CADDY_CONTAINER:-}" ] && command -v docker >/dev/null; then
    # `down` removes the container, network, and any anonymous volumes
    # attached via the compose project.
    docker compose -f "$SCRIPT_DIR/docker-compose.yml" \
        -p "$CADDY_CONTAINER" down --timeout 5 \
        >/dev/null 2>&1 || true
    # Belt-and-suspenders: if the container outlived compose down for
    # any reason, force-remove it by name.
    docker rm -f "$CADDY_CONTAINER" >/dev/null 2>&1 || true
fi
