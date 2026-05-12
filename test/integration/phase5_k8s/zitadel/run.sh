#!/usr/bin/env bash
# Phase 5 § 5.11 — ZITADEL alternate-IdP fixture.
#
# Stands up ZITADEL via the upstream Helm chart with a bundled
# Postgres sub-chart, waits for the ZITADEL pod to be ready, and
# verifies the OIDC discovery endpoint returns a sane document.
#
# This fixture is *separate* from the main K8s smoke (which uses
# Keycloak as the IdP).  The smoke proves that ZITADEL can drop
# into the same playbook slot Keycloak occupies — operators pick
# whichever IdP fits their estate; n00b's verifier-resolver is
# decoupled from the IdP brand.
#
# Run via:
#   N00B_TEST_KIND_ZITADEL=1 bash run.sh
#
# Skips (exit 77) when N00B_TEST_KIND_ZITADEL!=1 or when
# kind/kubectl/docker/helm aren't available.

set -e
set -o pipefail

CLUSTER_NAME=${CLUSTER_NAME:-n00b-phase5-zitadel}
HERE="$(cd "$(dirname "$0")" && pwd)"

require() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "  [SKIP] '$1' not on PATH" >&2
        exit 77
    fi
}

if [ "${N00B_TEST_KIND_ZITADEL:-0}" != "1" ]; then
    echo "  [SKIP] N00B_TEST_KIND_ZITADEL!=1; set N00B_TEST_KIND_ZITADEL=1 to run"
    exit 77
fi

require kind
require kubectl
require docker
require helm

echo "[fixture] using cluster=$CLUSTER_NAME"

if kind get clusters | grep -qx "$CLUSTER_NAME"; then
    echo "[fixture] tearing down stale cluster '$CLUSTER_NAME'..."
    kind delete cluster --name "$CLUSTER_NAME"
fi

echo "[fixture] booting kind cluster..."
kind create cluster --name "$CLUSTER_NAME" --wait 60s

echo "[fixture] installing Postgres (separate Bitnami release)..."
helm repo add --force-update zitadel https://charts.zitadel.com >/dev/null
helm repo add --force-update bitnami https://charts.bitnami.com/bitnami >/dev/null
helm repo update >/dev/null

kubectl create namespace zitadel --dry-run=client -o yaml | kubectl apply -f -

helm upgrade --install zitadel-pg bitnami/postgresql \
    --version 16.5.0 \
    --namespace zitadel \
    --set image.repository=bitnamilegacy/postgresql \
    --set image.tag=17.4.0-debian-12-r17 \
    --set global.security.allowInsecureImages=true \
    --set auth.postgresPassword=phase5-zitadel-pg-pw \
    --set auth.username=zitadel \
    --set auth.password=phase5-zitadel-pg-pw \
    --set auth.database=zitadel \
    --set primary.persistence.enabled=false \
    --set primary.resources.requests.cpu=100m \
    --set primary.resources.requests.memory=256Mi \
    --wait --timeout 5m

echo "[fixture] creating ZITADEL Postgres-DSN secret..."
kubectl -n zitadel create secret generic zitadel-pg-dsn \
    --from-literal=dsn="host=zitadel-pg-postgresql port=5432 user=zitadel password=phase5-zitadel-pg-pw dbname=zitadel sslmode=disable" \
    --dry-run=client -o yaml | kubectl apply -f -

echo "[fixture] installing ZITADEL (Helm chart)..."
helm upgrade --install zitadel zitadel/zitadel \
    --version 9.34.1 \
    --namespace zitadel \
    -f "$HERE/zitadel-values.yaml" \
    --wait --timeout 15m

echo "[fixture] waiting for ZITADEL pod to be Ready..."
kubectl -n zitadel rollout status deployment zitadel --timeout=300s

echo "[fixture] verifying OIDC discovery..."
out=$(kubectl run zitadel-discovery --rm -i --quiet --restart=Never \
    --image=curlimages/curl:8.10.1 \
    --command -- curl -fsS -m 15 \
        "http://zitadel.zitadel.svc.cluster.local:8080/.well-known/openid-configuration" \
        2>&1 || true)

echo "----- OIDC discovery (truncated) -----"
echo "$out" | head -c 600
echo
echo "----- end -----"

if echo "$out" | grep -q '"issuer"'; then
    echo "[fixture] OK: ZITADEL OIDC discovery is queryable"
else
    echo "[fixture] FAIL: ZITADEL OIDC discovery missing 'issuer'" >&2
    echo "----- pods -----"
    kubectl -n zitadel get pods -o wide || true
    echo "----- zitadel deployment status -----"
    kubectl -n zitadel describe deployment zitadel | tail -20 || true
    echo "----- zitadel pod log tail -----"
    kubectl -n zitadel logs deployment/zitadel --tail=80 || true
    echo "----- postgres pod log tail -----"
    kubectl -n zitadel logs zitadel-pg-postgresql-0 --tail=40 || true
    echo "----- helm release state -----"
    helm status zitadel --namespace zitadel || true
    echo "----- end diagnostics -----"
    helm uninstall zitadel --namespace zitadel --wait || true
    kind delete cluster --name "$CLUSTER_NAME" || true
    exit 1
fi

# Multi-tenant org configuration goes through ZITADEL's gRPC /
# REST API after install (see docs/quic/playbook_k8s.md §
# ZITADEL).  We don't run the full org-create dance in this
# fixture — operators do it via terraform-provider-zitadel /
# the management API.  The smoke is "ZITADEL drops into the
# IdP slot cleanly".

echo "[fixture] tearing down..."
helm uninstall zitadel --namespace zitadel --wait 2>/dev/null || true
kind delete cluster --name "$CLUSTER_NAME"

echo "[fixture] PASS"
