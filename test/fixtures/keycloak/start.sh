#!/bin/bash
# start.sh — Spin up a fresh Keycloak instance in Docker for one test run.
#
# Output: prints `eval`-able shell variables to stdout.  Usage:
#   eval "$(bash test/fixtures/keycloak/start.sh)"
#   build_debug/test_quic_keycloak_interop
#   bash test/fixtures/keycloak/stop.sh
#
# Sets:
#   KEYCLOAK_CONTAINER   Docker container ID/name
#   KEYCLOAK_PORT        Host port forwarded to Keycloak's 8080
#   KEYCLOAK_BASE_URL    e.g., http://localhost:8081
#   KEYCLOAK_REALM       Realm name (test fixture creates "n00b-test")
#   KEYCLOAK_CLIENT_ID   "n00b-cli"
#   KEYCLOAK_CLIENT_PW   Client secret for password-grant flows
#   KEYCLOAK_USER        Test user "alice"
#   KEYCLOAK_USER_PW     Test user password
#   KEYCLOAK_ISSUER      Issuer URL (the IdP base + /realms/<name>)
#
# Phase 3.10 manual interop fixture.  Assumes Docker is on the path.
# Phase 3.10 auto-runs only the *synthetic* IdP test in CI; Keycloak
# is opt-in.  Real-IdP interop runs once-per-release per the project
# convention from `quic_2.md` (LE-staging precedent).

set -euo pipefail

if ! command -v docker >/dev/null; then
    echo "ERR: docker not found; install Docker Desktop or skip this fixture." >&2
    exit 1
fi

KEYCLOAK_PORT="${KEYCLOAK_PORT:-$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')}"
KEYCLOAK_BASE_URL="http://localhost:${KEYCLOAK_PORT}"
KEYCLOAK_REALM="n00b-test"
KEYCLOAK_CLIENT_ID="n00b-cli"
KEYCLOAK_CLIENT_PW="cli-secret-test"
KEYCLOAK_USER="alice"
KEYCLOAK_USER_PW="alice-pw-test"
KEYCLOAK_ISSUER="${KEYCLOAK_BASE_URL}/realms/${KEYCLOAK_REALM}"

KEYCLOAK_CONTAINER=$(docker run -d --rm \
    -p "${KEYCLOAK_PORT}:8080" \
    -e KEYCLOAK_ADMIN=admin \
    -e KEYCLOAK_ADMIN_PASSWORD=admin \
    --name "n00b-test-keycloak-${KEYCLOAK_PORT}" \
    quay.io/keycloak/keycloak:25.0 \
    start-dev)

# Wait for Keycloak admin endpoint to come up (typically ~25s on a
# warm Docker, longer on a cold pull).
DEADLINE=$(($(date +%s) + 120))
until curl -fsSL "${KEYCLOAK_BASE_URL}/realms/master/.well-known/openid-configuration" >/dev/null 2>&1; do
    if [ "$(date +%s)" -ge "$DEADLINE" ]; then
        echo "ERR: Keycloak failed to come up within 120s" >&2
        docker logs "$KEYCLOAK_CONTAINER" >&2 || true
        docker rm -f "$KEYCLOAK_CONTAINER" >/dev/null 2>&1 || true
        exit 1
    fi
    sleep 2
done

# Bootstrap an admin token, create the realm + user + client via REST.
ADMIN_TOKEN=$(curl -fsSL -X POST \
    -d 'username=admin' -d 'password=admin' \
    -d 'grant_type=password' -d 'client_id=admin-cli' \
    "${KEYCLOAK_BASE_URL}/realms/master/protocol/openid-connect/token" \
    | python3 -c 'import sys, json; print(json.load(sys.stdin)["access_token"])')

# Create the realm.
curl -fsSL -X POST -H "Authorization: Bearer ${ADMIN_TOKEN}" \
    -H "Content-Type: application/json" \
    -d "{\"realm\":\"${KEYCLOAK_REALM}\",\"enabled\":true}" \
    "${KEYCLOAK_BASE_URL}/admin/realms" >/dev/null

# Create a confidential client that supports the password grant.
curl -fsSL -X POST -H "Authorization: Bearer ${ADMIN_TOKEN}" \
    -H "Content-Type: application/json" \
    -d "{\"clientId\":\"${KEYCLOAK_CLIENT_ID}\",\"enabled\":true,\
\"directAccessGrantsEnabled\":true,\"secret\":\"${KEYCLOAK_CLIENT_PW}\",\
\"publicClient\":false,\"protocol\":\"openid-connect\"}" \
    "${KEYCLOAK_BASE_URL}/admin/realms/${KEYCLOAK_REALM}/clients" >/dev/null

# Create the test user with a credential.
curl -fsSL -X POST -H "Authorization: Bearer ${ADMIN_TOKEN}" \
    -H "Content-Type: application/json" \
    -d "{\"username\":\"${KEYCLOAK_USER}\",\"enabled\":true,\
\"credentials\":[{\"type\":\"password\",\"value\":\"${KEYCLOAK_USER_PW}\",\
\"temporary\":false}]}" \
    "${KEYCLOAK_BASE_URL}/admin/realms/${KEYCLOAK_REALM}/users" >/dev/null

cat <<EOF
export KEYCLOAK_CONTAINER='${KEYCLOAK_CONTAINER}'
export KEYCLOAK_PORT='${KEYCLOAK_PORT}'
export KEYCLOAK_BASE_URL='${KEYCLOAK_BASE_URL}'
export KEYCLOAK_REALM='${KEYCLOAK_REALM}'
export KEYCLOAK_CLIENT_ID='${KEYCLOAK_CLIENT_ID}'
export KEYCLOAK_CLIENT_PW='${KEYCLOAK_CLIENT_PW}'
export KEYCLOAK_USER='${KEYCLOAK_USER}'
export KEYCLOAK_USER_PW='${KEYCLOAK_USER_PW}'
export KEYCLOAK_ISSUER='${KEYCLOAK_ISSUER}'
EOF
