#!/usr/bin/env bash
# Phase 5 § 5.4 — K8s playbook fixture: bring up a kind cluster, deploy
# the n00b phase5 demo, assert the pod runs cleanly, tear down.
#
# Idempotent: safe to re-run; deletes any prior n00b-phase5 cluster
# before booting a fresh one.
#
# Run via:
#   N00B_TEST_KIND=1 bash run.sh
#
# Skips (exit 77 — meson skip) when N00B_TEST_KIND isn't set or when
# kind/kubectl/docker aren't available.

set -e
set -o pipefail

CLUSTER_NAME=${CLUSTER_NAME:-n00b-phase5}
IMAGE_NAME=${IMAGE_NAME:-n00b-phase5-demo}
IMAGE_TAG=${IMAGE_TAG:-latest}
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$HERE/../../.." && pwd)"

require() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "  [SKIP] '$1' not on PATH" >&2
        exit 77
    fi
}

if [ "${N00B_TEST_KIND:-0}" != "1" ]; then
    echo "  [SKIP] N00B_TEST_KIND!=1; set N00B_TEST_KIND=1 to run the fixture"
    exit 77
fi

require kind
require kubectl
require docker
require helm

echo "[fixture] using cluster=$CLUSTER_NAME image=$IMAGE_NAME:$IMAGE_TAG"

# 1. Build the n00b runtime image (slow first time — the build runs
#    the project's bash build.sh inside debian:trixie-slim).  Cached
#    after the first successful build, so re-runs are fast.
echo "[fixture] building Docker image (first run takes 5-10 min)..."
docker build \
    -t "$IMAGE_NAME:$IMAGE_TAG" \
    -f "$HERE/Dockerfile" \
    "$PROJECT_ROOT"

# 2. Ensure no stale cluster from a prior run.
if kind get clusters | grep -qx "$CLUSTER_NAME"; then
    echo "[fixture] tearing down stale cluster '$CLUSTER_NAME'..."
    kind delete cluster --name "$CLUSTER_NAME"
fi

# 3. Boot fresh cluster.
echo "[fixture] booting kind cluster..."
kind create cluster \
    --name "$CLUSTER_NAME" \
    --config "$HERE/kind-config.yaml" \
    --wait 60s

# 4. Load the demo image into kind's nodes (avoids needing a registry).
echo "[fixture] loading image into kind..."
kind load docker-image "$IMAGE_NAME:$IMAGE_TAG" --name "$CLUSTER_NAME"

# 5a. Phase 5 § 5.5 — install Keycloak with the 2-tenant realm
#     import.  ConfigMap carries both realm JSONs.
echo "[fixture] installing Keycloak (Bitnami chart)..."
helm repo add --force-update bitnami https://charts.bitnami.com/bitnami >/dev/null
helm repo update bitnami >/dev/null
kubectl create configmap phase5-keycloak-realms \
    --from-file="$HERE/helm/keycloak-realm-tenant-alpha.json" \
    --from-file="$HERE/helm/keycloak-realm-tenant-beta.json" \
    --dry-run=client -o yaml | kubectl apply -f -
helm install keycloak bitnami/keycloak \
    --version 24.9.0 \
    -f "$HERE/helm/keycloak-values.yaml" \
    --wait \
    --timeout 10m
echo "[fixture] waiting for Keycloak realm import..."
# `--wait` returns when the readiness probe passes; the realm import
# happens at startup so the realms should be queryable now.
sleep 5

# Sanity-check both realms imported, plus the *binding behaviour*
# they're meant to enforce.  Tests run from one-shot curl pods against
# the in-cluster Keycloak Service rather than `kubectl exec`'ing into
# the keycloak pod itself — the bitnamilegacy image's exact tool
# surface (curl vs wget vs nothing) is undocumented and changes across
# r-tags, but the in-cluster DNS + Service contract is stable.
realms_ok=true

# (a) realm well-knowns
for realm in tenant-alpha tenant-beta; do
    if kubectl run "realm-check-$realm" --rm -i --quiet --restart=Never \
        --image=curlimages/curl:8.10.1 \
        --command -- curl -fsS -m 10 \
        "http://keycloak.default.svc.cluster.local/realms/$realm/.well-known/openid-configuration" \
        | grep -q "issuer"; then
        echo "[fixture] OK: realm '$realm' imported"
    else
        echo "[fixture] FAIL: realm '$realm' missing"
        realms_ok=false
    fi
done

# (b) tenant-alpha: phase5-greeter is DPoP-bound but accepts a plain
#     password-grant from inside the cluster (DPoP nonce is enforced
#     at resource-server token-introspection, not at /token).  A
#     successful access_token here proves users + client + secret +
#     password mapping all wired up.
if kubectl run "tok-alpha" --rm -i --quiet --restart=Never \
    --image=curlimages/curl:8.10.1 \
    --command -- sh -c '
        curl -sS -X POST \
          -d "grant_type=password" -d "client_id=phase5-greeter" \
          -d "client_secret=phase5-alpha-secret" \
          -d "username=alice" -d "password=alice-pw" \
          "http://keycloak.default.svc.cluster.local/realms/tenant-alpha/protocol/openid-connect/token"
    ' | grep -q "access_token"; then
    echo "[fixture] OK: alice@tenant-alpha password-grant succeeded"
else
    echo "[fixture] FAIL: alice@tenant-alpha password-grant"
    realms_ok=false
fi

# (c) tenant-beta: phase5-mtls is mTLS-HoK-bound.  A plain password
#     grant *must* be rejected with the canonical
#     "Client Certification missing for MTLS HoK Token Binding"
#     error — that's how we know the mTLS binding attribute landed
#     in the realm config.
beta_err=$(kubectl run "tok-beta" --rm -i --quiet --restart=Never \
    --image=curlimages/curl:8.10.1 \
    --command -- sh -c '
        curl -sS -X POST \
          -d "grant_type=password" -d "client_id=phase5-mtls" \
          -d "client_secret=phase5-beta-secret" \
          -d "username=alice" -d "password=alice-pw" \
          "http://keycloak.default.svc.cluster.local/realms/tenant-beta/protocol/openid-connect/token"
    ' 2>&1 || true)
if echo "$beta_err" | grep -q "MTLS HoK Token Binding"; then
    echo "[fixture] OK: tenant-beta rejects non-mTLS grant (mTLS binding active)"
else
    echo "[fixture] FAIL: tenant-beta did not enforce mTLS binding: $beta_err"
    realms_ok=false
fi

if [ "$realms_ok" != "true" ]; then
    helm uninstall keycloak --wait || true
    kind delete cluster --name "$CLUSTER_NAME" || true
    exit 1
fi

# ----------------------------------------------------------------
# 5.6 — cert-manager variant A (cert-manager-fronts)
#
# Stand up ingress-nginx, Pebble (with PEBBLE_VA_NOSLEEP for speed
# but *without* PEBBLE_VA_ALWAYS_VALID — real HTTP-01 validation
# end-to-end), cert-manager, then drive a Certificate through the
# whole ACME order/auth/cert flow and verify the resulting Secret
# parses cleanly.
# ----------------------------------------------------------------

echo "[fixture] § 5.6 — installing ingress-nginx..."
kubectl apply -f \
    https://raw.githubusercontent.com/kubernetes/ingress-nginx/main/deploy/static/provider/kind/deploy.yaml \
    >/dev/null
echo "[fixture] waiting for ingress-nginx controller..."
# kubectl wait can race the deployment's first pod creation
# ("error: no matching resources found").  Use rollout status which
# blocks until the deployment is fully Available.
kubectl --namespace ingress-nginx rollout status deployment/ingress-nginx-controller \
    --timeout=180s

# n00b-phase5.default.svc.cluster.local — the cert-manager
# challenge hostname.  ExternalName CNAME → ingress-nginx's FQDN
# means kube-dns resolves it to ingress-nginx's ClusterIP without
# any CoreDNS / hostAliases surgery, and ingress-nginx then routes
# by Host header to the cert-manager-spawned solver Pod.
echo "[fixture] § 5.6 — applying n00b-phase5 ExternalName Service..."
kubectl apply -f "$HERE/cert-manager/n00b-svc.yaml"

echo "[fixture] § 5.6 — installing Pebble (in-cluster ACME)..."
kubectl create namespace pebble --dry-run=client -o yaml | kubectl apply -f -
kubectl create configmap pebble-config -n pebble \
    --from-file="$HERE/pebble/pebble-config.json" \
    --dry-run=client -o yaml | kubectl apply -f -
kubectl create secret generic pebble-certs -n pebble \
    --from-file=cert.pem="$HERE/pebble/cert.pem" \
    --from-file=key.pem="$HERE/pebble/key.pem" \
    --dry-run=client -o yaml | kubectl apply -f -
kubectl apply -f "$HERE/pebble/pebble.yaml"
echo "[fixture] waiting for Pebble pod..."
kubectl -n pebble wait --for=condition=Ready pod -l app=pebble --timeout=180s

echo "[fixture] § 5.6 — installing cert-manager..."
kubectl apply -f \
    https://github.com/cert-manager/cert-manager/releases/download/v1.20.2/cert-manager.yaml \
    >/dev/null
echo "[fixture] waiting for cert-manager CRDs to register..."
for crd in clusterissuers.cert-manager.io \
           certificates.cert-manager.io \
           orders.acme.cert-manager.io \
           challenges.acme.cert-manager.io; do
    kubectl wait --for=condition=Established "crd/$crd" --timeout=60s
done
echo "[fixture] waiting for cert-manager controller, webhook, cainjector..."
kubectl -n cert-manager wait --for=condition=Ready pod \
    -l app.kubernetes.io/instance=cert-manager --timeout=180s
# Webhook readiness lags the pod-Ready signal by a moment — give the
# admission server a few seconds before applying CRs that go through
# the webhook validation.  Avoids transient "failed calling webhook"
# errors on the next kubectl apply.
sleep 10

echo "[fixture] § 5.6 — applying ClusterIssuer + Certificate..."
kubectl apply -f "$HERE/cert-manager/cluster-issuer.yaml"
kubectl apply -f "$HERE/cert-manager/certificate.yaml"

echo "[fixture] § 5.6 — waiting up to 5m for cert-manager to issue..."
if ! kubectl wait --for=condition=Ready certificate/n00b-phase5-cert --timeout=300s; then
    echo "[fixture] FAIL: cert-manager didn't issue cert in time" >&2
    echo "----- ClusterIssuer status -----"
    kubectl describe clusterissuer phase5-pebble || true
    echo "----- Certificate status -----"
    kubectl describe certificate n00b-phase5-cert || true
    echo "----- Order status -----"
    kubectl get orders -A -o yaml || true
    echo "----- Challenge status -----"
    kubectl get challenges -A -o yaml || true
    echo "----- cert-manager controller log tail -----"
    kubectl -n cert-manager logs -l app.kubernetes.io/component=controller --tail=80 || true
    echo "----- pebble log tail -----"
    kubectl -n pebble logs -l app=pebble --tail=80 || true
    helm uninstall keycloak --wait || true
    kind delete cluster --name "$CLUSTER_NAME" || true
    exit 1
fi
echo "[fixture] OK: cert-manager issued cert via ACME (real HTTP-01)"

echo "[fixture] § 5.6 — running cert-verify Job..."
kubectl apply -f "$HERE/cert-manager/cert-verify-job.yaml"
if ! kubectl wait --for=condition=Complete job/cert-verify --timeout=120s; then
    echo "[fixture] FAIL: cert-verify Job didn't complete" >&2
    kubectl describe job/cert-verify || true
    kubectl logs -l job-name=cert-verify --tail=50 || true
    helm uninstall keycloak --wait || true
    kind delete cluster --name "$CLUSTER_NAME" || true
    exit 1
fi
verify_log=$(kubectl logs -l job-name=cert-verify --tail=200 2>&1 || true)
echo "----- cert-verify log -----"
echo "$verify_log"
echo "----- end cert-verify log -----"
if echo "$verify_log" | grep -q "cert-verify: PASS"; then
    echo "[fixture] OK: cert-verify passed"
else
    echo "[fixture] FAIL: cert-verify did not print PASS" >&2
    helm uninstall keycloak --wait || true
    kind delete cluster --name "$CLUSTER_NAME" || true
    exit 1
fi

# ----------------------------------------------------------------
# 5.7 — cert-manager variant B (n00b-direct-ACME)
#
# Same Pebble + ingress-nginx infra as 5.6, but no cert-manager
# in the cert path: the n00b binary itself runs ACME via
# `n00b_acme_acquire_certificate` + an embedded HTTP-01 responder
# (`quic_acme_http01_demo`).  Service routes Pebble's challenge GET
# directly to the demo Pod's port 80 — no ingress-nginx in the
# data path for this variant (nothing on Ingress side to misroute).
# ----------------------------------------------------------------

echo "[fixture] § 5.7 — switching from cert-manager-fronts to n00b-direct-ACME..."
# The 5.6 ExternalName Service routed challenge traffic to
# ingress-nginx; for variant B we replace it with a ClusterIP
# Service that fronts the demo Pod directly.
kubectl delete -f "$HERE/cert-manager/n00b-svc.yaml" --ignore-not-found
kubectl apply -f "$HERE/acme_b/n00b-acme-svc.yaml"

echo "[fixture] § 5.7 — applying n00b-direct-ACME Job..."
kubectl apply -f "$HERE/acme_b/n00b-acme-job.yaml"

echo "[fixture] § 5.7 — waiting up to 4m for n00b-direct-ACME Job..."
if ! kubectl wait --for=condition=Complete job/n00b-acme-direct --timeout=240s; then
    echo "[fixture] FAIL: n00b-direct-ACME Job didn't complete in time" >&2
    kubectl describe job/n00b-acme-direct || true
    kubectl describe pod -l app=n00b-acme-direct || true
    kubectl logs -l app=n00b-acme-direct --tail=80 || true
    kubectl logs -n pebble -l app=pebble --tail=40 || true
    helm uninstall keycloak --wait || true
    kind delete cluster --name "$CLUSTER_NAME" || true
    exit 1
fi
acme_log=$(kubectl logs -l app=n00b-acme-direct --tail=200 2>&1 || true)
echo "----- n00b-direct-ACME pod log -----"
echo "$acme_log"
echo "----- end n00b-direct-ACME pod log -----"
if echo "$acme_log" | grep -q "ACME OK: cert acquired"; then
    echo "[fixture] OK: n00b-direct-ACME flow completed"
else
    echo "[fixture] FAIL: pod log missing 'ACME OK: cert acquired'" >&2
    helm uninstall keycloak --wait || true
    kind delete cluster --name "$CLUSTER_NAME" || true
    exit 1
fi

# 5b. Apply the demo deployment.
echo "[fixture] applying deployment..."
kubectl apply -f "$HERE/deploy/"

# 6. Wait for the Job to complete (--loopback exits 0 on success).
echo "[fixture] waiting up to 90s for Job to complete..."
if ! kubectl wait --for=condition=Complete job/n00b-phase5 --timeout=90s; then
    echo "[fixture] Job didn't complete in time; capturing diagnostics"
    kubectl describe job/n00b-phase5 || true
    kubectl describe pod -l app=n00b-phase5 || true
fi

# 7. Capture pod log + assert the loopback's success markers.
echo "[fixture] capturing pod logs..."
POD=$(kubectl get pod -l app=n00b-phase5 -o jsonpath='{.items[0].metadata.name}')
LOG=$(kubectl logs "$POD" 2>&1 || true)
echo "----- pod log -----"
echo "$LOG"
echo "----- end pod log -----"

assert_contains() {
    if ! echo "$LOG" | grep -q -F "$1"; then
        echo "[fixture] FAIL: pod log missing '$1'" >&2
        kubectl describe pod "$POD" >&2 || true
        kind delete cluster --name "$CLUSTER_NAME" || true
        exit 1
    fi
    echo "[fixture] OK: pod log has '$1'"
}

assert_contains "Hello reply"
assert_contains "MTls.Echo reply"
assert_contains "metrics surface OK"
# Phase 5 § 5.8 — verify the LB-CID encoding round-tripped: server's
# wire CID decodes (with the shared LB key) to the configured
# server_id.  The loopback inside the Job runs both server + client
# in the same process, so this is a single-replica smoke; in a
# multi-replica deployment the same assertion runs per-replica with
# distinct server_id values, and an LB-CID-aware load balancer
# (Envoy contrib QUIC LB filter / HAProxy lb-cid) routes follow-up
# packets to the right pod by decoding the wire CID.
assert_contains "LB-CID OK"

# 8. Tear down: helm uninstall first (so the chart's hooks finish
#    cleanly), then kind delete.  Image stays cached.
echo "[fixture] cleaning up cert-manager + variant-B artifacts..."
kubectl delete -f "$HERE/acme_b/n00b-acme-job.yaml" --ignore-not-found || true
kubectl delete -f "$HERE/acme_b/n00b-acme-svc.yaml" --ignore-not-found || true
kubectl delete -f "$HERE/cert-manager/cert-verify-job.yaml" --ignore-not-found || true
kubectl delete -f "$HERE/cert-manager/certificate.yaml" --ignore-not-found || true
kubectl delete -f "$HERE/cert-manager/cluster-issuer.yaml" --ignore-not-found || true
kubectl delete -f "$HERE/cert-manager/n00b-svc.yaml" --ignore-not-found || true
echo "[fixture] uninstalling Keycloak..."
helm uninstall keycloak --wait 2>/dev/null || true
echo "[fixture] tearing down cluster..."
kind delete cluster --name "$CLUSTER_NAME"

echo "[fixture] PASS"
