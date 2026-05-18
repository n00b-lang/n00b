# Phase 5 § 5.10 — bare-VM uacme + DNS-01 fixture image.
#
# Boots systemd as PID 1 with three additional services on top
# of the base 5.9 fixture:
#   - pebble-challtestsrv  (fake DNS + management API on :8055)
#   - pebble               (ACME server, validates via challtestsrv DNS)
#   - n00b-cert-renew      (one-shot uacme run that drives the
#                           DNS-01 flow against Pebble)
#
# uacme writes the resulting cert + key to
# /var/lib/n00b/tls/{cert,key}.pem; the smoke just asserts the
# files exist and parse.
#
# uacme is taken straight from Ubuntu plucky's `uacme` package
# (1.7.6+).  Pebble + pebble-challtestsrv binaries are copied in
# from upstream's `ghcr.io/letsencrypt/pebble*:latest` images at
# build time — both are statically-linked Go binaries so they
# run as-is.

# ---------- pebble + challtestsrv binaries ----------
FROM ghcr.io/letsencrypt/pebble:latest AS pebble-bin
FROM ghcr.io/letsencrypt/pebble-challtestsrv:latest AS challtestsrv-bin

# ---------- runtime ----------
FROM ubuntu:plucky

RUN apt-get update && \
    apt-get install --yes --no-install-recommends \
        systemd systemd-sysv dbus \
        ca-certificates \
        curl jq \
        uacme \
        libgnutls30 libcurl4t64 \
        && rm -rf /var/lib/apt/lists/* \
        && find /etc/systemd/system /lib/systemd/system \
                -path '*.wants/*' \
                -not -name '*systemd-tmpfiles-setup*' \
                -not -name '*journald*' \
                -not -name '*systemd-user-sessions*' \
                -not -name '*remote-fs*' \
                -delete

# Pebble + challtestsrv binaries (each image's entrypoint is /app).
COPY --from=pebble-bin       /app /usr/local/bin/pebble
COPY --from=challtestsrv-bin /app /usr/local/bin/pebble-challtestsrv

# Pebble's TLS cert + Mini-CA (vendored under phase5_k8s/pebble/
# — the same artefacts the K8s fixture uses).
COPY test/integration/phase5_k8s/pebble/pebble.minica.pem  /etc/pebble/minica.pem
COPY test/integration/phase5_k8s/pebble/cert.pem           /etc/pebble/cert.pem
COPY test/integration/phase5_k8s/pebble/key.pem            /etc/pebble/key.pem

# Bake Pebble's Mini-CA into the host trust store so uacme's
# libcurl trusts the directory's TLS endpoint.
RUN install -m 644 /etc/pebble/minica.pem \
    /usr/local/share/ca-certificates/pebble-minica.crt \
    && update-ca-certificates

# Pebble config: HTTP-01 + DNS-01 ports rewritten to Pebble's
# *internal* defaults (5002 / 5001), with -dnsserver pointed at
# pebble-challtestsrv's local listener so DNS-01 actually
# resolves the TXT records the hook publishes.
RUN mkdir -p /etc/pebble /var/lib/n00b/tls /var/lib/n00b/uacme \
    && chmod 700 /var/lib/n00b/tls /var/lib/n00b/uacme

COPY test/integration/phase5_vm/uacme/pebble-config.json     /etc/pebble/config.json
COPY test/integration/phase5_vm/uacme/pebble.service          /etc/systemd/system/pebble.service
COPY test/integration/phase5_vm/uacme/pebble-challtestsrv.service \
                                                              /etc/systemd/system/pebble-challtestsrv.service
COPY test/integration/phase5_vm/uacme/n00b-cert-renew.service /etc/systemd/system/n00b-cert-renew.service
COPY test/integration/phase5_vm/uacme/dns01-hook.sh           /usr/local/bin/dns01-hook.sh

RUN chmod +x /usr/local/bin/dns01-hook.sh \
    && systemctl enable pebble-challtestsrv pebble n00b-cert-renew

STOPSIGNAL SIGRTMIN+3
ENTRYPOINT ["/sbin/init"]
