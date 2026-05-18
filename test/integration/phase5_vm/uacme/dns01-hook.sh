#!/usr/bin/env bash
# Phase 5 § 5.10 — uacme DNS-01 hook for the bare-VM fixture.
#
# uacme calls this script with three positional arguments:
#   $1 = "begin" | "done" | "failed"
#   $2 = "dns-01"
#   $3 = identifier (the bare DNS name, e.g., "n00b.test.example")
# When method=dns-01, $4..$5 carry the token + key-authz hash.
#
# Real-world DNS-01 hooks talk to a DNS provider's API
# (Cloudflare, Route53, etc.) to add a TXT record at
# `_acme-challenge.<identifier>` containing the SHA-256 (base64url)
# of the key-authorization.  This fixture talks to
# pebble-challtestsrv's management API instead — same shape, no
# real DNS.
#
# uacme contract: exit 0 on success, non-zero on failure.

set -euo pipefail

METHOD=${1:-}
TYPE=${2:-}
IDENTIFIER=${3:-}
TOKEN=${4:-}
KEY_AUTHZ=${5:-}

CHALLTESTSRV=${CHALLTESTSRV:-http://127.0.0.1:8055}

case "$METHOD" in
    begin)
        if [ "$TYPE" != "dns-01" ]; then
            echo "[hook] only dns-01 supported (got $TYPE)" >&2
            exit 1
        fi
        # uacme provides $5 as the *base64url(SHA-256(key-authz))*
        # — the value Pebble's validator will compare against the
        # TXT record body.  We POST it to challtestsrv's
        # /set-txt, which makes the DNS server return that value
        # for `_acme-challenge.<identifier>` until cleared.
        curl -fsS --data-binary "{\"host\":\"_acme-challenge.${IDENTIFIER}.\",\"value\":\"${KEY_AUTHZ}\"}" \
            "${CHALLTESTSRV}/set-txt"
        echo "[hook] published TXT _acme-challenge.${IDENTIFIER} → ${KEY_AUTHZ}"
        ;;
    done|failed)
        # Clean up the TXT record regardless of outcome.
        curl -fsS --data-binary "{\"host\":\"_acme-challenge.${IDENTIFIER}.\"}" \
            "${CHALLTESTSRV}/clear-txt" || true
        echo "[hook] cleared TXT _acme-challenge.${IDENTIFIER}"
        ;;
    *)
        echo "[hook] unknown method '$METHOD'" >&2
        exit 1
        ;;
esac
