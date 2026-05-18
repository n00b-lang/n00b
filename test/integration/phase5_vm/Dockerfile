# Phase 5 § 5.9 — bare-VM playbook fixture image.
#
# Boots systemd as PID 1 inside an Ubuntu plucky container; the n00b
# Phase 5 demo (built once for the K8s fixture, reused here) is
# wired in as a one-shot systemd unit.  The smoke runs the same
# `--loopback` binary the K8s fixture runs, but observes it through
# `journalctl -u n00b-phase5` instead of `kubectl logs`, exercising
# the systemd-unit / journal / cap_net_bind path operators ship with
# bare-VM deployments.
#
# Two-stage:
#   1. Build the demo binary (same recipe as the K8s Dockerfile;
#      docker layer cache means this is cached after the K8s image
#      builds the binary the first time).
#   2. Runtime stage adds systemd + the unit + Pebble Mini-CA, and
#      sets `/sbin/init` as the entrypoint.

# ---------- builder ----------
FROM ubuntu:plucky AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential python3 python3-pip ninja-build pkg-config \
        git wget ca-certificates gnupg lsb-release \
        libfreetype-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /etc/apt/keyrings && \
    wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key \
        | gpg --dearmor -o /etc/apt/keyrings/llvm.gpg && \
    echo "deb [signed-by=/etc/apt/keyrings/llvm.gpg] http://apt.llvm.org/plucky/ llvm-toolchain-plucky main" \
        > /etc/apt/sources.list.d/llvm.list && \
    apt-get update && \
    if apt-get install -y --no-install-recommends clang-22 lld-22 2>/dev/null; then \
        ln -sf /usr/bin/clang-22 /usr/bin/clang && \
        ln -sf /usr/bin/clang++-22 /usr/bin/clang++; \
    elif apt-get install -y --no-install-recommends clang-20 lld-20 2>/dev/null; then \
        ln -sf /usr/bin/clang-20 /usr/bin/clang && \
        ln -sf /usr/bin/clang++-20 /usr/bin/clang++; \
    fi && \
    rm -rf /var/lib/apt/lists/*

RUN pip3 install --break-system-packages 'meson>=1.6.0'

WORKDIR /src
COPY . /src/

RUN N00B_TEST=0 N00B_JOBS=2 bash build.sh \
    || (echo "===== meson-log =====" \
        && cat /src/build_debug/meson-logs/meson-log.txt \
        && exit 1)

# ---------- runtime ----------
FROM ubuntu:plucky

# systemd PID-1 + the runtime libraries the demo binary links
# against.  `dbus` is required so systemctl works inside the
# container.
RUN apt-get update && \
    apt-get install --yes --no-install-recommends \
        systemd systemd-sysv dbus \
        ca-certificates \
        libatomic1 \
        libfreetype6 \
        libncursesw6 \
        libssl3t64 \
        && rm -rf /var/lib/apt/lists/* \
        && find /etc/systemd/system /lib/systemd/system \
                -path '*.wants/*' \
                -not -name '*systemd-tmpfiles-setup*' \
                -not -name '*journald*' \
                -not -name '*systemd-user-sessions*' \
                -not -name '*remote-fs*' \
                -delete

# Bake Pebble's Mini-CA into the system trust store (same as the
# K8s image — the bare-VM playbook also uses Pebble as its in-test
# ACME server in 5.10's variant-A path).
COPY test/integration/phase5_k8s/pebble/pebble.minica.pem \
     /usr/local/share/ca-certificates/pebble-minica.crt
RUN update-ca-certificates

COPY --from=builder /src/build_debug/quic_phase5_demo \
                    /usr/local/bin/quic_phase5_demo

# n00b service unit: a one-shot that runs the demo's `--loopback`,
# captures stdout/stderr to the journal, and exits 0 on success.
COPY test/integration/phase5_vm/n00b-phase5.service \
     /etc/systemd/system/n00b-phase5.service
RUN systemctl enable n00b-phase5.service

STOPSIGNAL SIGRTMIN+3
ENTRYPOINT ["/sbin/init"]
