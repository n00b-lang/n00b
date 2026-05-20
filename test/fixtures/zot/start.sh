#!/bin/bash
# start.sh — boot the zot OCI registry fixture for one test run.
#
# Mirrors test/fixtures/nginx-quic/start.sh (generates a self-signed
# cert + renders the config file via envsubst), adapted for zot's
# config.json schema + the OCI v2 readiness probe.
#
# Usage:
#   eval "$(bash test/fixtures/zot/start.sh)"
#   build_debug/test_attest_oci_push_smoke
#   bash test/fixtures/zot/stop.sh
#
# Sets (and prints as eval-able exports):
#   ZOT_CONTAINER             Container name (unique per port).
#   ZOT_PORT                  Host port (TCP) forwarded to zot.
#   N00B_TEST_DOCKER_ZOT_URL  Registry origin URL, e.g.,
#                             https://localhost:54321  (the test
#                             reads this name to discover the
#                             fixture).
#   N00B_TEST_DOCKER_ZOT_HOST Registry hostname[:port], e.g.,
#                             localhost:54321  (the test reads this
#                             name to construct push image refs).
#   ZOT_CERT_DIR              Tmp dir holding generated server.crt +
#                             server.key + rendered config.json
#                             (also mounted read-only into the
#                             container; stop.sh removes it).
#   N00B_TEST_DOCKER_ZOT_CERT_FP   SHA-256 hex fingerprint of the
#                                  leaf cert (no colons, lowercase);
#                                  consumed by the test to construct
#                                  a pinned-fingerprint trust handle.
#
# Optional inputs:
#   ZOT_PORT          Override the auto-picked ephemeral port.
#   ZOT_CONTAINER     Override the auto-generated container name.
#   ZOT_CERT_DIR      Override the cert/config tmpdir.
#
# Hard prereqs:
#   docker, openssl, envsubst, xxd, python3.  Missing prereqs are a
#   hard failure (not a silent skip) — CI gates the whole smoke suite
#   via N00B_TEST_DOCKER=1 in meson.build, so an unconditional
#   failure here is the right signal at this layer.

set -euo pipefail

if ! command -v docker >/dev/null; then
    echo "ERR: docker not found; install Docker Desktop or skip this fixture." >&2
    exit 1
fi
if ! command -v envsubst >/dev/null; then
    echo "ERR: envsubst not found (gettext package)." >&2
    exit 1
fi
if ! command -v openssl >/dev/null; then
    echo "ERR: openssl not found." >&2
    exit 1
fi
if ! command -v xxd >/dev/null; then
    echo "ERR: xxd not found." >&2
    exit 1
fi

# Pick a free ephemeral port; explicit override via ZOT_PORT.
if [ -z "${ZOT_PORT:-}" ]; then
    ZOT_PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
fi

# Container names are port-suffixed so concurrent test runs don't
# collide (each picks its own ephemeral port).
ZOT_CONTAINER="${ZOT_CONTAINER:-n00b-test-zot-${ZOT_PORT}}"
ZOT_BASE_URL="https://localhost:${ZOT_PORT}"
ZOT_HOST="localhost:${ZOT_PORT}"
ZOT_CERT_DIR="${ZOT_CERT_DIR:-$(mktemp -d -t n00b-zot-XXXXXX)}"

export ZOT_PORT ZOT_CONTAINER ZOT_CERT_DIR

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# --- Generate a self-signed leaf cert ---------------------------------
# RFC 5280 SAN with DNS:localhost so a client that pins the SPKI / FP
# can verify the hostname.  EC P-256 because TLS 1.3 is happy with it
# and it's faster than RSA at this size.  The fixture cert under
# test/fixtures/cert_provisioner/ is bound to a different CN
# (cert-provisioner-test.example), so we don't reuse it here.
openssl req -x509 -nodes \
    -newkey ec:<(openssl ecparam -name prime256v1) \
    -days 1 \
    -subj '/CN=localhost' \
    -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' \
    -keyout "$ZOT_CERT_DIR/server.key" \
    -out    "$ZOT_CERT_DIR/server.crt" \
    >/dev/null 2>&1

chmod 644 "$ZOT_CERT_DIR/server.key"
chmod 644 "$ZOT_CERT_DIR/server.crt"

# Capture the cert SHA-256 fingerprint up front (we generated it, so
# no race here — unlike caddy where issuance is lazy).
LEAF_FP=$(openssl x509 -in "$ZOT_CERT_DIR/server.crt" \
              -outform DER 2>/dev/null \
          | openssl dgst -sha256 -binary 2>/dev/null \
          | xxd -p -c 64)
if [ -z "$LEAF_FP" ]; then
    echo "ERR: Failed to compute zot cert fingerprint" >&2
    rm -rf "$ZOT_CERT_DIR"
    exit 1
fi

# --- Render config.json with the chosen port --------------------------
# zot reads /etc/zot/config.json at startup; we substitute ${ZOT_PORT}
# in-place on the host because zot itself does NOT do env-var
# expansion inside the JSON.
ZOT_PORT="$ZOT_PORT" envsubst '$ZOT_PORT' \
    < "$SCRIPT_DIR/config.json" \
    > "$ZOT_CERT_DIR/config.json"

# --- Bring the compose stub up ----------------------------------------
# docker-compose.yml mounts ${ZOT_CERT_DIR}/config.json and the cert
# dir; both are populated above before this point.
docker compose -f "$SCRIPT_DIR/docker-compose.yml" \
    -p "$ZOT_CONTAINER" \
    up -d --quiet-pull >/dev/null 2>&1

cleanup_on_fail() {
    docker compose -f "$SCRIPT_DIR/docker-compose.yml" \
        -p "$ZOT_CONTAINER" down --timeout 5 \
        >/dev/null 2>&1 || true
    docker rm -f "$ZOT_CONTAINER" >/dev/null 2>&1 || true
    rm -rf "$ZOT_CERT_DIR" 2>/dev/null || true
}
trap cleanup_on_fail ERR

# Wait up to ~30s for zot to start serving the OCI v2 endpoint.
# GET /v2/ on a zot configured with anonymous access returns 200 +
# `{}` body once startup completes; before that the TCP connect or
# TLS handshake may fail.  We use curl --insecure here because we
# pinned the fingerprint above and don't need (and don't have) a
# system trust anchor for this throwaway leaf.
healthy=0
DEADLINE=$(($(date +%s) + 30))
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    STATUS=$(curl -sk -o /dev/null -w '%{http_code}' \
                  --max-time 2 \
                  "https://127.0.0.1:${ZOT_PORT}/v2/" \
                  2>/dev/null || true)
    if [ "$STATUS" = "200" ]; then
        healthy=1
        break
    fi
    sleep 0.3
done

if [ "$healthy" -ne 1 ]; then
    echo "ERR: zot didn't become healthy within 30s" >&2
    docker logs "$ZOT_CONTAINER" >&2 || true
    cleanup_on_fail
    exit 1
fi

# Past the danger zone — disarm the trap.
trap - ERR

cat <<EOF
export ZOT_CONTAINER='${ZOT_CONTAINER}'
export ZOT_PORT='${ZOT_PORT}'
export ZOT_CERT_DIR='${ZOT_CERT_DIR}'
export N00B_TEST_DOCKER_ZOT_URL='${ZOT_BASE_URL}'
export N00B_TEST_DOCKER_ZOT_HOST='${ZOT_HOST}'
export N00B_TEST_DOCKER_ZOT_CERT_FP='${LEAF_FP}'
EOF
