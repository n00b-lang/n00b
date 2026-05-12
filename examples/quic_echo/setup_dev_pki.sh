#!/usr/bin/env bash
#
# examples/quic_echo/setup_dev_pki.sh — bootstrap a local dev PKI for the
# n00b QUIC echo demo.
#
# What it does (idempotent on every step):
#
#   1. Creates ~/.n00b-dev-pki/ if missing — a self-contained step-ca
#      data dir, isolated from any system-wide step-ca install.
#   2. Initializes step-ca for the first run (single-CA, ACME provisioner).
#   3. Trusts the CA root cert in the macOS System Keychain via
#      `sudo security add-trusted-cert`.  This is the only sudo step.
#   4. Starts step-ca in the background on 127.0.0.1:8443 (or whatever
#      port is free), writing PID + log to ~/.n00b-dev-pki/.
#   5. Issues a server cert + key via ACME against the local step-ca,
#      writing them to ~/.n00b-dev-pki/server.{crt,key}.pem.
#
# Designed to run safely on a developer's own machine.  The CA is
# scoped to that user; the only system-wide effect is the trust-anchor
# entry in System Keychain, which `teardown_dev_pki.sh` removes.
#
# Re-run any time — every step checks for prior state and skips if
# already done.

set -euo pipefail

DEV_PKI="${HOME}/.n00b-dev-pki"
STEP_DIR="${DEV_PKI}/step"
SERVER_HOST="${SERVER_HOST:-localhost}"
CA_NAME="n00b dev"
CA_DNS="localhost"
CA_PORT="${CA_PORT:-8443}"
CA_URL="https://${CA_DNS}:${CA_PORT}"
CA_FINGERPRINT_FILE="${DEV_PKI}/ca-fingerprint"
SERVER_CRT="${DEV_PKI}/server.crt.pem"
SERVER_KEY="${DEV_PKI}/server.key.pem"
PASSWORD_FILE="${DEV_PKI}/.password"
PROVISIONER_PASSWORD_FILE="${DEV_PKI}/.provisioner-password"
ACME_PROVISIONER="acme"   # default name when `step ca init --acme` runs
PID_FILE="${DEV_PKI}/step-ca.pid"
LOG_FILE="${DEV_PKI}/step-ca.log"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

log()  { printf "\033[36m[setup_dev_pki]\033[0m %s\n" "$*"; }
warn() { printf "\033[33m[setup_dev_pki]\033[0m %s\n" "$*" >&2; }
die()  { printf "\033[31m[setup_dev_pki]\033[0m %s\n" "$*" >&2; exit 1; }

step_ca_running() {
    [[ -f "${PID_FILE}" ]] || return 1
    local pid
    pid="$(cat "${PID_FILE}")"
    [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null
}

# ---------------------------------------------------------------------------
# 1. Data dir
# ---------------------------------------------------------------------------
if [[ ! -d "${DEV_PKI}" ]]; then
    log "creating ${DEV_PKI}"
    mkdir -p "${DEV_PKI}"
    chmod 700 "${DEV_PKI}"
fi

# ---------------------------------------------------------------------------
# 2. step-ca init (only on first run)
# ---------------------------------------------------------------------------
if [[ ! -d "${STEP_DIR}" ]]; then
    log "initializing step-ca at ${STEP_DIR}"
    # step ca init expects a password file even for "dev" mode.  We store
    # one inside the data dir; it isn't a security boundary because the
    # private CA material lives next to it anyway.
    {
        # generate a random password if we don't already have one
        if [[ ! -f "${PASSWORD_FILE}" ]]; then
            head -c 24 /dev/urandom | base64 > "${PASSWORD_FILE}"
            chmod 600 "${PASSWORD_FILE}"
        fi
        if [[ ! -f "${PROVISIONER_PASSWORD_FILE}" ]]; then
            head -c 24 /dev/urandom | base64 > "${PROVISIONER_PASSWORD_FILE}"
            chmod 600 "${PROVISIONER_PASSWORD_FILE}"
        fi
    }
    STEPPATH="${STEP_DIR}" step ca init \
        --name="${CA_NAME}"                                         \
        --dns="${CA_DNS}"                                           \
        --address="127.0.0.1:${CA_PORT}"                            \
        --provisioner=admin                                         \
        --acme                                                      \
        --password-file="${PASSWORD_FILE}"                          \
        --provisioner-password-file="${PROVISIONER_PASSWORD_FILE}"  \
        --deployment-type=standalone                                \
        > /dev/null

    # Record CA root fingerprint so teardown can find / remove it.
    STEPPATH="${STEP_DIR}" step certificate fingerprint \
        "${STEP_DIR}/certs/root_ca.crt" > "${CA_FINGERPRINT_FILE}"
    chmod 600 "${CA_FINGERPRINT_FILE}"

    log "step-ca initialized (with default '${ACME_PROVISIONER}' ACME provisioner)"
else
    log "step-ca already initialized at ${STEP_DIR}"
fi

# ---------------------------------------------------------------------------
# 3. Trust CA root in System Keychain (sudo)
# ---------------------------------------------------------------------------
ROOT_CRT="${STEP_DIR}/certs/root_ca.crt"
[[ -f "${ROOT_CRT}" ]] || die "missing CA root cert at ${ROOT_CRT}"

# Compute SHA-256 to check if it's already trusted.
ROOT_FP="$(openssl x509 -in "${ROOT_CRT}" -outform DER 2>/dev/null \
            | shasum -a 256 | awk '{print $1}')"

if security find-certificate -c "${CA_NAME}" /Library/Keychains/System.keychain >/dev/null 2>&1; then
    log "CA root already trusted in System Keychain (cert with CN \"${CA_NAME}\" present)"
else
    log "trusting CA root in System Keychain (will prompt for sudo)"
    sudo security add-trusted-cert -d -r trustRoot \
         -k /Library/Keychains/System.keychain \
         "${ROOT_CRT}"
    log "trusted; SHA-256 = ${ROOT_FP}"
fi

# ---------------------------------------------------------------------------
# 4. Start step-ca in the background
# ---------------------------------------------------------------------------
if step_ca_running; then
    log "step-ca already running (pid $(cat "${PID_FILE}"))"
else
    log "starting step-ca in background → ${LOG_FILE}"
    STEPPATH="${STEP_DIR}" step-ca \
        --password-file="${PASSWORD_FILE}" \
        "${STEP_DIR}/config/ca.json" \
        >> "${LOG_FILE}" 2>&1 &
    echo $! > "${PID_FILE}"
    # Give it a moment to bind.
    sleep 1
    if ! step_ca_running; then
        warn "step-ca failed to start; tail of log:"
        tail -20 "${LOG_FILE}" >&2 || true
        die "see ${LOG_FILE} for details"
    fi
    log "step-ca up (pid $(cat "${PID_FILE}"))"
fi

# ---------------------------------------------------------------------------
# 5. Issue (or refresh) a server cert via the ACME provisioner
# ---------------------------------------------------------------------------
needs_issue=1
if [[ -f "${SERVER_CRT}" && -f "${SERVER_KEY}" ]]; then
    # Re-issue if the cert is within 7 days of expiring.
    if openssl x509 -in "${SERVER_CRT}" -checkend $((7 * 24 * 3600)) \
         > /dev/null 2>&1; then
        log "server cert at ${SERVER_CRT} is current — skipping reissue"
        needs_issue=0
    else
        log "server cert near expiry — reissuing"
    fi
fi

if (( needs_issue )); then
    log "issuing server cert for ${SERVER_HOST} via ACME (${CA_URL})"
    # Bootstrap step's client config to point at our local CA root.
    STEPPATH="${STEP_DIR}" step ca bootstrap --force \
        --ca-url="${CA_URL}" \
        --fingerprint="$(cat "${CA_FINGERPRINT_FILE}")" \
        > /dev/null 2>&1

    # ACME flow.  --provisioner is the ACME provisioner we added above.
    # --webroot doesn't apply for tls-alpn-01 challenge; step uses HTTP
    # if the port's free, else falls back to other challenge types.
    # For loopback dev this works without any external server.
    # ACME provisioners cap cert duration (default 24h).  We
    # re-issue on each setup run if the cert is within 7 days of
    # expiring (see the issuer-skip logic above), so the short-lived
    # default is fine.
    STEPPATH="${STEP_DIR}" step ca certificate "${SERVER_HOST}" \
        "${SERVER_CRT}" "${SERVER_KEY}" \
        --provisioner="${ACME_PROVISIONER}" \
        --acme="${CA_URL}/acme/${ACME_PROVISIONER}/directory" \
        --kty=EC --curve=P-256 \
        --force \
        2>&1 | tail -5

    # `step ca certificate` writes the key in `EC PRIVATE KEY` PEM
    # form (RFC 5915).  picotls's minicrypto loader matches literally
    # on `PRIVATE KEY` (PKCS#8 wrapper), so convert.  Any modern
    # openssl `pkcs8 -topk8 -nocrypt` produces what we need.
    if head -1 "${SERVER_KEY}" | grep -q "EC PRIVATE KEY"; then
        log "converting key to PKCS#8 (picotls minicrypto requirement)"
        OSSL_BIN="$(ls /opt/homebrew/opt/openssl@*/bin/openssl 2>/dev/null | head -1)"
        OSSL_BIN="${OSSL_BIN:-openssl}"
        "${OSSL_BIN}" pkcs8 -in "${SERVER_KEY}" -topk8 -nocrypt \
                     -out "${SERVER_KEY}.pkcs8.tmp"
        mv "${SERVER_KEY}.pkcs8.tmp" "${SERVER_KEY}"
        chmod 600 "${SERVER_KEY}"
    fi
fi

# ---------------------------------------------------------------------------
# Final summary
# ---------------------------------------------------------------------------
log "done."
log "  CA dir:        ${STEP_DIR}"
log "  CA URL:        ${CA_URL}"
log "  step-ca pid:   $(cat "${PID_FILE}")"
log "  Server cert:   ${SERVER_CRT}"
log "  Server key:    ${SERVER_KEY}"
log ""
log "Run the echo demo with:"
log "  ./build_debug/quic_echo server 4433 \\"
log "      --cert-pem=${SERVER_CRT} \\"
log "      --key-pem=${SERVER_KEY}"
log ""
log "Stop / remove with: ./examples/quic_echo/teardown_dev_pki.sh"
