#!/bin/bash
# start.sh — boot the caddy H3 fixture for one test run.
#
# Mirrors the shape of test/fixtures/stepca/start.sh: pick an
# ephemeral port, bring the container up, wait for readiness, capture
# the leaf cert SHA-256 fingerprint, print eval-able env-vars.
#
# Usage:
#   eval "$(bash test/fixtures/caddy/start.sh)"
#   build_debug/test_quic_h3_caddy_smoke
#   bash test/fixtures/caddy/stop.sh
#
# Sets:
#   CADDY_CONTAINER   Container name (unique per port)
#   CADDY_PORT        Host port forwarded to caddy
#   CADDY_BASE_URL    e.g., https://localhost:54321
#   CADDY_CERT_FP     SHA-256 fingerprint of the live leaf cert (hex,
#                     lower-case, no colons) — pin trust against this
#                     instead of dragging caddy's internal CA into
#                     the OS trust store.
#
# The fixture is gated on `docker --version` succeeding.  Locally,
# running it without docker is a hard failure (so a missing prereq
# isn't silently skipped); CI gates the whole interop suite via
# N00B_TEST_DOCKER=1 in meson.build.

set -euo pipefail

if ! command -v docker >/dev/null; then
    echo "ERR: docker not found; install Docker Desktop or skip this fixture." >&2
    exit 1
fi

# Pick a free ephemeral port; explicit override via CADDY_PORT.
if [ -z "${CADDY_PORT:-}" ]; then
    CADDY_PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
fi

# Container names are port-suffixed so concurrent test runs don't
# collide (each picks its own ephemeral port).
CADDY_CONTAINER="${CADDY_CONTAINER:-n00b-test-caddy-${CADDY_PORT}}"
CADDY_BASE_URL="https://localhost:${CADDY_PORT}"

export CADDY_PORT CADDY_CONTAINER

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Bring the compose stub up.  --wait would be ideal but it depends on
# a HEALTHCHECK in the image (caddy:2 doesn't ship one); we poll
# manually below.
docker compose -f "$SCRIPT_DIR/docker-compose.yml" \
    -p "$CADDY_CONTAINER" \
    up -d --quiet-pull >/dev/null 2>&1

# If anything below this point fails, tear the container down so the
# port doesn't leak across runs.
cleanup_on_fail() {
    docker compose -f "$SCRIPT_DIR/docker-compose.yml" \
        -p "$CADDY_CONTAINER" down --timeout 5 \
        >/dev/null 2>&1 || true
}
trap cleanup_on_fail ERR

# Wait up to ~30s for caddy to start serving HTTPS with an issued
# leaf cert.  We probe the TLS handshake (not HTTP) so we don't
# depend on local curl having QUIC support.  Caddy's internal CA
# issues the leaf lazily on first SNI lookup, so a fresh container
# may briefly answer ClientHello with `internal_error` before the
# cert is ready.  We look for an actual `BEGIN CERTIFICATE` line in
# the s_client output to distinguish a real handshake from
# pre-issuance failure.
healthy=0
DEADLINE=$(($(date +%s) + 30))
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    PROBE=$(openssl s_client -connect "127.0.0.1:${CADDY_PORT}" \
                -servername "localhost" -showcerts \
                < /dev/null 2>/dev/null || true)
    if echo "$PROBE" | grep -q 'BEGIN CERTIFICATE'; then
        healthy=1
        break
    fi
    sleep 0.3
done

if [ "$healthy" -ne 1 ]; then
    echo "ERR: caddy didn't become healthy within 30s" >&2
    docker logs "$CADDY_CONTAINER" >&2 || true
    cleanup_on_fail
    exit 1
fi

# Capture the SHA-256 fingerprint of the live HTTPS leaf cert.  Used
# by the test to pin trust without dragging caddy's internal CA into
# the OS trust store.  Caddy's internal CA regenerates this leaf on
# every start, so we compute it freshly here.
#
# `openssl s_client` returns non-zero on clean disconnect (after our
# stdin EOF) which trips pipefail; we explicitly tolerate that here.
set +e
set +o pipefail
LEAF_FP=$(openssl s_client -connect "127.0.0.1:${CADDY_PORT}" \
              -servername "localhost" -showcerts \
              < /dev/null 2>/dev/null \
          | sed -n '/BEGIN CERTIFICATE/,/END CERTIFICATE/p' \
          | sed -n '1,/END CERTIFICATE/p' \
          | openssl x509 -outform DER 2>/dev/null \
          | openssl dgst -sha256 -binary 2>/dev/null \
          | xxd -p -c 64)
set -e
set -o pipefail
if [ -z "$LEAF_FP" ]; then
    echo "ERR: Failed to capture caddy leaf fingerprint" >&2
    cleanup_on_fail
    exit 1
fi

# Past the danger zone — disarm the trap.
trap - ERR

cat <<EOF
export CADDY_CONTAINER='${CADDY_CONTAINER}'
export CADDY_PORT='${CADDY_PORT}'
export CADDY_BASE_URL='${CADDY_BASE_URL}'
export CADDY_CERT_FP='${LEAF_FP}'
EOF
