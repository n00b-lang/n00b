# Phase 5 § 5.4 — n00b runtime container for the K8s playbook fixture.
#
# Builds n00b + quic_phase5_demo inside ubuntu:plucky with clang-22
# from apt.llvm.org (mirrors the project's docker/Dockerfile shape so
# the picoquic + picotls vendored sources compile cleanly on Linux —
# debian:trixie-default clang-19 hits picoquic's pthread/spin_lock
# header-collision wall, plucky+clang-22 doesn't).
#
# Two-stage to keep the runtime image small (~50 MB) — the build stage
# pulls the toolchain + dev headers; the runtime stage only carries
# libfreetype + libssl + the binary itself.

# ---------- builder ----------
FROM ubuntu:plucky AS builder

# Match the project's existing docker/Dockerfile toolchain layout.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential python3 python3-pip ninja-build pkg-config \
        git wget ca-certificates gnupg lsb-release \
        libfreetype-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Newer clang from apt.llvm.org — n00b's stricter type checks need
# clang-19+ but vendored picotls/picoquic only build cleanly with
# clang-20+ (clang-19 on trixie hits implicit-decl errors that don't
# surface on macOS).  Try 22 → 20 → 19 as the project's docker/
# already does.
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

# Reuse the project's build harness; N00B_TEST=0 skips test compilation
# (the demo binary itself doesn't need the unit tests).  N00B_JOBS=2
# caps parallelism — each clang+ncc invocation can use ~1 GB resident
# memory; full -j on a memory-tight host (Docker's default 4 GB on
# macOS) trips the OOM killer.  On failure, dump meson-log to surface
# the real error (build.sh redirects it).
RUN N00B_TEST=0 N00B_JOBS=2 bash build.sh \
    || (echo "===== meson-log =====" \
        && cat /src/build_debug/meson-logs/meson-log.txt \
        && echo "===== ninja errors =====" \
        && (ninja -C /src/build_debug -j 1 -v 2>&1 | tail -60) \
        && exit 1)

# Sanity: verify both demo binaries exist.
RUN ls -l /src/build_debug/quic_phase5_demo /src/build_debug/quic_acme_http01_demo

# ---------- runtime ----------
FROM ubuntu:plucky

RUN apt-get update && \
    apt-get install --yes --no-install-recommends \
        ca-certificates \
        libatomic1 \
        libfreetype6 \
        libncursesw6 \
        libssl3t64 \
        && rm -rf /var/lib/apt/lists/*

# Phase 5 § 5.7 — bake Pebble's Mini-CA into the runtime image's
# system trust store so n00b's ACME HTTPS client (which validates
# via OpenSSL's default-path X509_STORE) trusts Pebble's directory
# TLS endpoint without requiring per-Pod SSL_CERT_FILE/DIR
# overrides.  This image is the fixture's test image — *not*
# shipped to production — so polluting its trust store with the
# upstream Pebble Mini-CA is appropriate scoping.
COPY test/integration/phase5_k8s/pebble/pebble.minica.pem \
     /usr/local/share/ca-certificates/pebble-minica.crt
RUN update-ca-certificates

COPY --from=builder /src/build_debug/quic_phase5_demo \
                    /usr/local/bin/quic_phase5_demo
COPY --from=builder /src/build_debug/quic_acme_http01_demo \
                    /usr/local/bin/quic_acme_http01_demo

# The demo binds an ephemeral UDP port for QUIC and an ephemeral TCP
# port for /metrics.  Production deployments override via an env-driven
# manifest (see playbook_k8s.md); the loopback default just runs.
ENTRYPOINT ["/usr/local/bin/quic_phase5_demo"]
CMD ["--loopback"]
