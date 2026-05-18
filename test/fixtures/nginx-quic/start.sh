#!/bin/bash
# start.sh — boot the nginx-quic H3 fixture for one test run.
#
# Mirrors test/fixtures/caddy/start.sh.  Differences vs caddy:
#  - nginx doesn't auto-issue TLS, so we generate a self-signed
#    cert here with openssl and mount it into the container.
#  - nginx serves a fixed body (no internal echo module in the
#    stock image), so the H3 server smoke test asserts on a known
#    response body rather than a true round-tripped echo.  See
#    nginx.conf for the rationale.
#
# Usage:
#   eval "$(bash test/fixtures/nginx-quic/start.sh)"
#   build_debug/test_quic_h3_nginx_smoke
#   bash test/fixtures/nginx-quic/stop.sh
#
# Sets:
#   NGINX_QUIC_CONTAINER    Container name (unique per port)
#   NGINX_QUIC_PORT         Host port (TCP+UDP) forwarded to nginx
#   NGINX_QUIC_BASE_URL     e.g., https://localhost:54321
#   NGINX_QUIC_CERT_DIR     Tmp dir holding the generated cert+key
#                           (also mounted read-only into the
#                           container; stop.sh removes it).
#   NGINX_QUIC_CERT_FP      SHA-256 fingerprint of the leaf cert.

set -euo pipefail

if ! command -v docker >/dev/null; then
    echo "ERR: docker not found; install Docker Desktop or skip this fixture." >&2
    exit 1
fi
if ! command -v envsubst >/dev/null; then
    echo "ERR: envsubst not found (gettext package)." >&2
    exit 1
fi

# Pick a free ephemeral port; explicit override via NGINX_QUIC_PORT.
if [ -z "${NGINX_QUIC_PORT:-}" ]; then
    NGINX_QUIC_PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
fi

NGINX_QUIC_CONTAINER="${NGINX_QUIC_CONTAINER:-n00b-test-nginx-quic-${NGINX_QUIC_PORT}}"
NGINX_QUIC_BASE_URL="https://localhost:${NGINX_QUIC_PORT}"
NGINX_QUIC_CERT_DIR="${NGINX_QUIC_CERT_DIR:-$(mktemp -d -t n00b-nginx-quic-XXXXXX)}"

export NGINX_QUIC_PORT NGINX_QUIC_CONTAINER NGINX_QUIC_CERT_DIR

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# --- Generate a self-signed leaf cert ----------------------------------
# RFC 5280 SAN with DNS:localhost so a client pinning the SPKI / FP
# can verify the hostname.  EC P-256 because TLS 1.3 + QUIC are
# happy with it and it's faster than RSA at this size.
openssl req -x509 -nodes \
    -newkey ec:<(openssl ecparam -name prime256v1) \
    -days 1 \
    -subj '/CN=localhost' \
    -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' \
    -keyout "$NGINX_QUIC_CERT_DIR/server.key" \
    -out    "$NGINX_QUIC_CERT_DIR/server.crt" \
    >/dev/null 2>&1

chmod 600 "$NGINX_QUIC_CERT_DIR/server.key"
chmod 644 "$NGINX_QUIC_CERT_DIR/server.crt"

# Capture the cert SHA-256 fingerprint up front (we generated it,
# so no race here — unlike caddy where issuance is lazy).
LEAF_FP=$(openssl x509 -in "$NGINX_QUIC_CERT_DIR/server.crt" \
              -outform DER 2>/dev/null \
          | openssl dgst -sha256 -binary 2>/dev/null \
          | xxd -p -c 64)
if [ -z "$LEAF_FP" ]; then
    echo "ERR: Failed to compute nginx cert fingerprint" >&2
    rm -rf "$NGINX_QUIC_CERT_DIR"
    exit 1
fi

# --- Render nginx.conf with the chosen port ---------------------------
# We do the substitution on the host (not via nginx envsubst) so the
# baked conf is mounted read-only into the container — the official
# image's envsubst only runs on /etc/nginx/templates/*.template, not
# on /etc/nginx/nginx.conf.
NGINX_QUIC_PORT="$NGINX_QUIC_PORT" envsubst '$NGINX_QUIC_PORT' \
    < "$SCRIPT_DIR/nginx.conf" \
    > "$NGINX_QUIC_CERT_DIR/nginx.conf"

# --- Bring the compose stub up ----------------------------------------
# docker-compose.yml mounts ${NGINX_QUIC_CERT_DIR}/nginx.conf and the
# cert dir; both are populated above before this point.
docker compose -f "$SCRIPT_DIR/docker-compose.yml" \
    -p "$NGINX_QUIC_CONTAINER" \
    up -d --quiet-pull >/dev/null 2>&1

cleanup_on_fail() {
    docker compose -f "$SCRIPT_DIR/docker-compose.yml" \
        -p "$NGINX_QUIC_CONTAINER" down --timeout 5 \
        >/dev/null 2>&1 || true
    docker rm -f "$NGINX_QUIC_CONTAINER" >/dev/null 2>&1 || true
    rm -rf "$NGINX_QUIC_CERT_DIR" 2>/dev/null || true
}
trap cleanup_on_fail ERR

# Wait up to ~30s for nginx to start serving HTTPS.  We probe TLS
# (not HTTP) so we don't depend on local curl having QUIC support.
healthy=0
DEADLINE=$(($(date +%s) + 30))
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    PROBE=$(openssl s_client -connect "127.0.0.1:${NGINX_QUIC_PORT}" \
                -servername "localhost" -showcerts \
                < /dev/null 2>/dev/null || true)
    if echo "$PROBE" | grep -q 'BEGIN CERTIFICATE'; then
        healthy=1
        break
    fi
    sleep 0.3
done

if [ "$healthy" -ne 1 ]; then
    echo "ERR: nginx-quic didn't become healthy within 30s" >&2
    docker logs "$NGINX_QUIC_CONTAINER" >&2 || true
    cleanup_on_fail
    exit 1
fi

# Past the danger zone — disarm the trap.
trap - ERR

cat <<EOF
export NGINX_QUIC_CONTAINER='${NGINX_QUIC_CONTAINER}'
export NGINX_QUIC_PORT='${NGINX_QUIC_PORT}'
export NGINX_QUIC_BASE_URL='${NGINX_QUIC_BASE_URL}'
export NGINX_QUIC_CERT_DIR='${NGINX_QUIC_CERT_DIR}'
export NGINX_QUIC_CERT_FP='${LEAF_FP}'
EOF
