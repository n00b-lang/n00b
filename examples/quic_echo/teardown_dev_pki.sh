#!/usr/bin/env bash
#
# examples/quic_echo/teardown_dev_pki.sh — reverse setup_dev_pki.sh.
#
# What it does (idempotent):
#
#   1. Stops the local step-ca process.
#   2. Removes the CA root from the macOS System Keychain (needs sudo).
#   3. Optionally removes ~/.n00b-dev-pki/ if `--purge` is given.
#
# Default leaves ~/.n00b-dev-pki/ in place so re-running setup is fast.

set -euo pipefail

DEV_PKI="${HOME}/.n00b-dev-pki"
STEP_DIR="${DEV_PKI}/step"
ROOT_CRT="${STEP_DIR}/certs/root_ca.crt"
PID_FILE="${DEV_PKI}/step-ca.pid"
CA_NAME="n00b dev"

log()  { printf "\033[36m[teardown_dev_pki]\033[0m %s\n" "$*"; }
warn() { printf "\033[33m[teardown_dev_pki]\033[0m %s\n" "$*" >&2; }

# ---------------------------------------------------------------------------
# 1. Stop step-ca if running
# ---------------------------------------------------------------------------
if [[ -f "${PID_FILE}" ]]; then
    pid="$(cat "${PID_FILE}")"
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
        log "stopping step-ca (pid ${pid})"
        kill -TERM "${pid}" 2>/dev/null || true
        # Wait briefly for graceful exit.
        for _ in $(seq 1 10); do
            kill -0 "${pid}" 2>/dev/null || break
            sleep 0.2
        done
        if kill -0 "${pid}" 2>/dev/null; then
            warn "step-ca didn't exit on TERM; sending KILL"
            kill -KILL "${pid}" 2>/dev/null || true
        fi
    else
        log "step-ca pid file present but process ${pid} not running"
    fi
    rm -f "${PID_FILE}"
else
    log "step-ca not running"
fi

# ---------------------------------------------------------------------------
# 2. Remove CA root from System Keychain
# ---------------------------------------------------------------------------
if security find-certificate -c "${CA_NAME}" /Library/Keychains/System.keychain >/dev/null 2>&1; then
    log "removing CA root \"${CA_NAME}\" from System Keychain (will prompt for sudo)"
    # `security delete-certificate` removes the cert AND its trust entry.
    sudo security delete-certificate -c "${CA_NAME}" \
         /Library/Keychains/System.keychain
else
    log "no CA root \"${CA_NAME}\" found in System Keychain"
fi

# ---------------------------------------------------------------------------
# 3. Optional purge
# ---------------------------------------------------------------------------
if [[ "${1:-}" == "--purge" ]]; then
    if [[ -d "${DEV_PKI}" ]]; then
        log "purging ${DEV_PKI}"
        rm -rf "${DEV_PKI}"
    fi
else
    log "leaving ${DEV_PKI} in place (re-run setup is fast)"
    log "  pass --purge to remove it entirely"
fi

log "done."
