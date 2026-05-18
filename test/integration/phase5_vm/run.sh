#!/usr/bin/env bash
# Phase 5 § 5.9 — bare-VM playbook fixture: boot a privileged
# container with systemd PID-1, watch the n00b-phase5 unit run to
# completion, assert the same loopback markers the K8s fixture
# greps for.
#
# Idempotent: deletes any prior n00b-phase5-vm container before
# starting a new one.
#
# Run via:
#   N00B_TEST_VM=1 bash run.sh
#
# Skips (exit 77) when N00B_TEST_VM!=1 or docker isn't available.

set -e
set -o pipefail

CONTAINER_NAME=${CONTAINER_NAME:-n00b-phase5-vm}
IMAGE_NAME=${IMAGE_NAME:-n00b-phase5-vm}
IMAGE_TAG=${IMAGE_TAG:-latest}
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$HERE/../../.." && pwd)"

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

# 1. Build the VM image (cached after first run; the demo binary
#    layer hits the K8s fixture's build cache).
echo "[fixture] building Docker image (first run takes 5-10 min)..."
docker build \
    -t "$IMAGE_NAME:$IMAGE_TAG" \
    -f "$HERE/Dockerfile" \
    "$PROJECT_ROOT"

# 2. Tear down any stale container.
if docker ps -a --format '{{.Names}}' | grep -qx "$CONTAINER_NAME"; then
    echo "[fixture] stopping stale container..."
    docker rm -f "$CONTAINER_NAME" >/dev/null
fi

# 3. Boot privileged container with systemd PID-1.  --privileged +
#    cgroupns=host is the canonical "systemd in Docker" recipe;
#    real bare-VM operators don't need any of this (the kernel
#    already runs systemd).  The tmpfs /run mount is required by
#    systemd; the cgroup mount lets units control resources.
echo "[fixture] booting container with systemd PID-1..."
docker run -d --name "$CONTAINER_NAME" \
    --privileged \
    --cgroupns=host \
    --tmpfs /run \
    --tmpfs /run/lock \
    -v /sys/fs/cgroup:/sys/fs/cgroup:rw \
    "$IMAGE_NAME:$IMAGE_TAG" >/dev/null

# 4. Wait for systemd to reach multi-user.target (n00b-phase5.service
#    starts as part of that).
echo "[fixture] waiting for systemd multi-user.target..."
ready=0
for i in $(seq 1 60); do
    state=$(docker exec "$CONTAINER_NAME" systemctl is-system-running --wait 2>/dev/null \
            || docker exec "$CONTAINER_NAME" systemctl is-system-running 2>/dev/null \
            || echo starting)
    case "$state" in
        running|degraded)
            ready=1; break ;;
    esac
    sleep 1
done
if [ "$ready" -ne 1 ]; then
    echo "[fixture] FAIL: systemd never reached multi-user.target" >&2
    docker exec "$CONTAINER_NAME" systemctl status --no-pager || true
    docker rm -f "$CONTAINER_NAME" >/dev/null || true
    exit 1
fi

# 5. The unit is one-shot (exits 0 on success).  Wait until it has
#    transitioned out of "active(running)" / "activating".
echo "[fixture] waiting for n00b-phase5.service to complete..."
done=0
for i in $(seq 1 120); do
    active=$(docker exec "$CONTAINER_NAME" systemctl is-active n00b-phase5 2>/dev/null || true)
    case "$active" in
        inactive|failed)
            done=1; break ;;
    esac
    sleep 1
done
if [ "$done" -ne 1 ]; then
    echo "[fixture] FAIL: n00b-phase5.service didn't complete in time" >&2
    docker exec "$CONTAINER_NAME" systemctl status n00b-phase5 --no-pager -l || true
    docker exec "$CONTAINER_NAME" journalctl -u n00b-phase5 --no-pager -l || true
    docker rm -f "$CONTAINER_NAME" >/dev/null || true
    exit 1
fi

# 6. Capture journal + assert success markers.  Same set as the
#    K8s smoke — the binary is the same; this proves the
#    systemd / journalctl operator path works.
echo "[fixture] capturing journal..."
LOG=$(docker exec "$CONTAINER_NAME" journalctl -u n00b-phase5 --no-pager -l 2>&1 || true)
echo "----- journal -----"
echo "$LOG"
echo "----- end journal -----"

# Confirm the unit's exit code via systemctl (avoids interpreting
# stdout for success on its own).
exit_state=$(docker exec "$CONTAINER_NAME" \
    systemctl show n00b-phase5 --property=Result --value 2>/dev/null || echo unknown)
if [ "$exit_state" != "success" ]; then
    echo "[fixture] FAIL: n00b-phase5.service Result=$exit_state" >&2
    docker rm -f "$CONTAINER_NAME" >/dev/null || true
    exit 1
fi

assert_contains() {
    if ! echo "$LOG" | grep -q -F "$1"; then
        echo "[fixture] FAIL: journal missing '$1'" >&2
        docker rm -f "$CONTAINER_NAME" >/dev/null || true
        exit 1
    fi
    echo "[fixture] OK: journal has '$1'"
}

assert_contains "Hello reply"
assert_contains "MTls.Echo reply"
assert_contains "metrics surface OK"
assert_contains "LB-CID OK"

# 7. Tear down.
echo "[fixture] stopping container..."
docker rm -f "$CONTAINER_NAME" >/dev/null

echo "[fixture] PASS"
