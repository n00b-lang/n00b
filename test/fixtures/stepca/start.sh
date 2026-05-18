#!/bin/bash
# start.sh — spin up a fresh step-ca instance for a single test run.
#
# Idempotent: blowing away $STEPPATH and re-running gets you a fresh CA.
# Output: prints the directory URL to stdout.
#
# Usage:
#   eval "$(bash test/fixtures/stepca/start.sh)"
# Sets:
#   STEPPATH        Temp dir with the CA tree
#   STEPCA_PID      PID of the running step-ca process
#   STEPCA_PORT     The port step-ca is listening on
#   STEPCA_DIR_URL  The ACME directory URL

set -euo pipefail

# Pick a free ephemeral port; explicit override via STEPCA_PORT.
if [ -z "${STEPCA_PORT:-}" ]; then
    STEPCA_PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
fi
STEPPATH="${STEPPATH:-$(mktemp -d -t n00b-stepca-XXXXXX)}"
PASSFILE="$STEPPATH/.password"

# Use a hard-coded password for the test fixture — it never leaves the
# temp dir and never sees real PKI material.
echo "n00b-test-stepca-fixture-pw" > "$PASSFILE"
chmod 600 "$PASSFILE"

export STEPPATH

# Init a one-off CA with an ACME provisioner.  ACME requires the
# default badger DB (it tracks orders/authorizations); we keep it in
# the temp dir, which is wiped at end-of-test.
step ca init \
    --deployment-type standalone \
    --name "n00b-test-ca" \
    --dns "127.0.0.1" \
    --dns "localhost" \
    --address "127.0.0.1:${STEPCA_PORT}" \
    --provisioner "n00b-test-acme" \
    --acme \
    --password-file "$PASSFILE" \
    --provisioner-password-file "$PASSFILE" \
    > /dev/null 2>&1

# Background-launch step-ca.  Wait until /health responds.
step-ca --password-file "$PASSFILE" "$STEPPATH/config/ca.json" \
    > "$STEPPATH/stepca.log" 2>&1 &
STEPCA_PID=$!

# If anything below this point fails, kill the background process so
# the temp dir's port doesn't leak across runs.
cleanup_on_fail() {
    kill "$STEPCA_PID" 2>/dev/null || true
    cat "$STEPPATH/stepca.log" >&2 2>/dev/null || true
}
trap cleanup_on_fail ERR

# Wait up to 20s for the health endpoint.
HEALTH_URL="https://127.0.0.1:${STEPCA_PORT}/health"
healthy=0
for _ in $(seq 1 100); do
    if curl --cacert "$STEPPATH/certs/root_ca.crt" -sf "$HEALTH_URL" \
        > /dev/null 2>&1
    then
        healthy=1
        break
    fi
    if ! kill -0 "$STEPCA_PID" 2>/dev/null; then
        echo "step-ca died — see $STEPPATH/stepca.log" >&2
        exit 1
    fi
    sleep 0.2
done

if [ "$healthy" -ne 1 ]; then
    echo "step-ca didn't become healthy within 20s" >&2
    kill "$STEPCA_PID" 2>/dev/null || true
    cat "$STEPPATH/stepca.log" >&2
    exit 1
fi

# Capture the SHA-256 fingerprint of the live HTTPS leaf cert.  Used
# by the test to pin trust without dragging the root into the OS
# trust store.  step-ca regenerates this leaf on every start, so we
# compute it freshly here.
#
# `openssl s_client` returns non-zero on clean disconnect (after our
# stdin EOF) which trips pipefail; we explicitly tolerate that here.
set +e
set +o pipefail
LEAF_FP=$(openssl s_client -connect "127.0.0.1:${STEPCA_PORT}" \
              -servername "127.0.0.1" -showcerts \
              < /dev/null 2>/dev/null \
          | sed -n '/BEGIN CERTIFICATE/,/END CERTIFICATE/p' \
          | sed -n '1,/END CERTIFICATE/p' \
          | openssl x509 -outform DER 2>/dev/null \
          | openssl dgst -sha256 -binary 2>/dev/null \
          | xxd -p -c 64)
set -e
set -o pipefail
if [ -z "$LEAF_FP" ]; then
    echo "Failed to capture step-ca leaf fingerprint" >&2
    kill "$STEPCA_PID" 2>/dev/null || true
    exit 1
fi

# Past the danger zone — disarm the trap.
trap - ERR

# The directory URL for a step-ca ACME provisioner is:
#   https://CA-DNS:PORT/acme/<acme-provisioner-name>/directory
#
# `step ca init --provisioner X --acme` actually creates *two*
# provisioners: a JWK one named X and a separate ACME one named
# "acme".  We talk to the latter for the ACME flow.
DIR_URL="https://127.0.0.1:${STEPCA_PORT}/acme/acme/directory"

# Print export-prefixed env-var assignments so callers using
# `eval "$(...)"` get them propagated to subprocesses.
echo "export STEPPATH=$STEPPATH"
echo "export STEPCA_PID=$STEPCA_PID"
echo "export STEPCA_PORT=$STEPCA_PORT"
echo "export STEPCA_DIR_URL=$DIR_URL"
echo "export STEPCA_ROOT_PEM=$STEPPATH/certs/root_ca.crt"
echo "export STEPCA_LEAF_FP=$LEAF_FP"
