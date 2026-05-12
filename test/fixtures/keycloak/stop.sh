#!/bin/bash
# stop.sh — tear down the Keycloak fixture.
set -euo pipefail

if [ -n "${KEYCLOAK_CONTAINER:-}" ]; then
    docker rm -f "$KEYCLOAK_CONTAINER" >/dev/null 2>&1 || true
fi
