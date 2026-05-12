#!/bin/bash
# stop.sh — tear down the nginx-quic H3 fixture.
#
# Usage:
#   NGINX_QUIC_CONTAINER=... NGINX_QUIC_CERT_DIR=... \
#       bash test/fixtures/nginx-quic/stop.sh
#
# Idempotent: missing container / dir is fine.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -n "${NGINX_QUIC_CONTAINER:-}" ] && command -v docker >/dev/null; then
    docker compose -f "$SCRIPT_DIR/docker-compose.yml" \
        -p "$NGINX_QUIC_CONTAINER" down --timeout 5 \
        >/dev/null 2>&1 || true
    docker rm -f "$NGINX_QUIC_CONTAINER" >/dev/null 2>&1 || true
fi

# Refuse to wipe anything outside the temp roots — defensive against
# a stale env-var pointing somewhere user-meaningful.
if [ -n "${NGINX_QUIC_CERT_DIR:-}" ] && [ -d "$NGINX_QUIC_CERT_DIR" ]; then
    case "$NGINX_QUIC_CERT_DIR" in
        /tmp/*|/var/folders/*)
            rm -rf "$NGINX_QUIC_CERT_DIR"
            ;;
        *)
            echo "stop.sh: refusing to remove non-temp NGINX_QUIC_CERT_DIR=$NGINX_QUIC_CERT_DIR" >&2
            ;;
    esac
fi
