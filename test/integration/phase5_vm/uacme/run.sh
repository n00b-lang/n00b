#!/usr/bin/env bash
# Phase 5 § 5.10 — bare-VM uacme + DNS-01 fixture: boot a
# privileged container with systemd PID-1 + Pebble +
# pebble-challtestsrv + a uacme cert-renewal one-shot, watch the
# unit run end-to-end, assert the resulting cert is parseable +
# signed by Pebble's runtime CA.

set -e
set -o pipefail

CONTAINER_NAME=${CONTAINER_NAME:-n00b-phase5-vm-uacme}
IMAGE_NAME=${IMAGE_NAME:-n00b-phase5-vm-uacme}
IMAGE_TAG=${IMAGE_TAG:-latest}
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$HERE/../../../.." && pwd)"

require() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "  [SKIP] '$1' not on PATH" >&2
        exit 77
    fi
}

if [ "${N00B_TEST_VM:-0}" != "1" ]; then
    echo "  [SKIP] N00B_TEST_VM!=1; set N00B_TEST_VM=1 to run the fixture"
    exit 77
fi

require docker

echo "[fixture] using container=$CONTAINER_NAME image=$IMAGE_NAME:$IMAGE_TAG"

echo "[fixture] building Docker image (first run takes 1-3 min)..."
docker build \
    -t "$IMAGE_NAME:$IMAGE_TAG" \
    -f "$HERE/Dockerfile" \
    "$PROJECT_ROOT"

if docker ps -a --format '{{.Names}}' | grep -qx "$CONTAINER_NAME"; then
    echo "[fixture] stopping stale container..."
    docker rm -f "$CONTAINER_NAME" >/dev/null
fi

echo "[fixture] booting container with systemd PID-1..."
docker run -d --name "$CONTAINER_NAME" \
    --privileged \
    --cgroupns=host \
    --tmpfs /run \
    --tmpfs /run/lock \
    -v /sys/fs/cgroup:/sys/fs/cgroup:rw \
    "$IMAGE_NAME:$IMAGE_TAG" >/dev/null

echo "[fixture] waiting for systemd multi-user.target..."
ready=0
state=
for i in $(seq 1 180); do
    # systemctl is-system-running returns exit 0 only for
    # `running`; `degraded` exits 1.  We don't care about exit
    # status — only the printed state.  `2>/dev/null || true`
    # discards stderr on failures (e.g., dbus not yet available)
    # and absorbs the non-zero exit so the trailing fallback
    # `echo starting` doesn't pollute $state.
    state=$(docker exec "$CONTAINER_NAME" \
        systemctl is-system-running 2>/dev/null || true)
    case "$state" in
        running|degraded) ready=1; break ;;
    esac
    sleep 1
done
if [ "$ready" -ne 1 ]; then
    echo "[fixture] FAIL: systemd never reached multi-user.target (state=$state)" >&2
    docker exec "$CONTAINER_NAME" systemctl --failed --no-pager || true
    docker exec "$CONTAINER_NAME" journalctl --no-pager -n 80 || true
    docker rm -f "$CONTAINER_NAME" >/dev/null || true
    exit 1
fi

echo "[fixture] waiting for pebble + pebble-challtestsrv to be active..."
for i in $(seq 1 30); do
    p_active=$(docker exec "$CONTAINER_NAME" systemctl is-active pebble 2>/dev/null || true)
    c_active=$(docker exec "$CONTAINER_NAME" systemctl is-active pebble-challtestsrv 2>/dev/null || true)
    if [ "$p_active" = "active" ] && [ "$c_active" = "active" ]; then break; fi
    sleep 2
done

echo "[fixture] waiting up to 3m for n00b-cert-renew.service to complete..."
done=0
for i in $(seq 1 180); do
    active=$(docker exec "$CONTAINER_NAME" systemctl is-active n00b-cert-renew 2>/dev/null || true)
    case "$active" in
        inactive|failed) done=1; break ;;
    esac
    sleep 1
done
if [ "$done" -ne 1 ]; then
    echo "[fixture] FAIL: n00b-cert-renew didn't complete in time" >&2
    docker exec "$CONTAINER_NAME" systemctl status n00b-cert-renew --no-pager -l || true
    docker exec "$CONTAINER_NAME" journalctl -u n00b-cert-renew --no-pager -l || true
    docker exec "$CONTAINER_NAME" journalctl -u pebble --no-pager -l --tail 40 || true
    docker rm -f "$CONTAINER_NAME" >/dev/null || true
    exit 1
fi

result=$(docker exec "$CONTAINER_NAME" \
    systemctl show n00b-cert-renew --property=Result --value 2>/dev/null || echo unknown)
if [ "$result" != "success" ]; then
    echo "[fixture] FAIL: n00b-cert-renew Result=$result" >&2
    docker exec "$CONTAINER_NAME" journalctl -u n00b-cert-renew --no-pager -l || true
    docker exec "$CONTAINER_NAME" journalctl -u pebble --no-pager -l --tail 40 || true
    docker rm -f "$CONTAINER_NAME" >/dev/null || true
    exit 1
fi
echo "[fixture] OK: n00b-cert-renew.service Result=success"

echo "[fixture] verifying cert + key on disk..."
docker exec "$CONTAINER_NAME" test -f /var/lib/n00b/tls/cert.pem || {
    echo "[fixture] FAIL: cert.pem missing" >&2
    docker rm -f "$CONTAINER_NAME" >/dev/null || true
    exit 1
}
docker exec "$CONTAINER_NAME" test -f /var/lib/n00b/tls/key.pem || {
    echo "[fixture] FAIL: key.pem missing" >&2
    docker rm -f "$CONTAINER_NAME" >/dev/null || true
    exit 1
}

echo "----- cert summary -----"
docker exec "$CONTAINER_NAME" openssl x509 \
    -in /var/lib/n00b/tls/cert.pem -noout -subject -issuer -dates 2>&1
echo "----- end cert summary -----"

issuer=$(docker exec "$CONTAINER_NAME" openssl x509 \
    -in /var/lib/n00b/tls/cert.pem -noout -issuer 2>&1)
case "$issuer" in
    *Pebble*|*minica*)
        echo "[fixture] OK: cert issued by Pebble Intermediate CA" ;;
    *)
        echo "[fixture] FAIL: unexpected issuer: $issuer" >&2
        docker rm -f "$CONTAINER_NAME" >/dev/null || true
        exit 1 ;;
esac

echo "[fixture] stopping container..."
docker rm -f "$CONTAINER_NAME" >/dev/null

echo "[fixture] PASS"
