/*
 * tenant_resolver.c — header-based tenant routing for Phase 5.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "tenant_resolver.h"

static const n00b_h3_header_t *
find_header(const n00b_h3_header_t *hdrs, size_t n_hdrs, const char *name)
{
    size_t nl = strlen(name);
    for (size_t i = 0; i < n_hdrs; i++) {
        if (hdrs[i].name_len != nl) continue;
        /* H3 header names are typically lowercased on the wire (HTTP/3
         * § 4.2 — pseudo-headers must be lowercase, regular headers
         * are case-insensitive).  Use byte-case-insensitive compare. */
        if (strncasecmp((const char *)hdrs[i].name, name, nl) == 0) {
            return &hdrs[i];
        }
    }
    return nullptr;
}

n00b_jwt_verifier_t *
phase5_tenant_resolver(const char             *idp_id,
                       const n00b_h3_header_t *hdrs,
                       size_t                  n_hdrs,
                       void                   *ctx)
{
    phase5_tenant_table_t *t = ctx;
    if (!t) return nullptr;

    /* The X-Tenant header is the routing key.  When absent we
     * still try to match `idp_id` directly against the table's
     * tenant entries — useful for legacy callers that don't set
     * `X-Tenant`. */
    const n00b_h3_header_t *xt = find_header(hdrs, n_hdrs, "x-tenant");
    if (xt) {
        for (int i = 0; i < t->n; i++) {
            const char *tid = t->routes[i].tenant_id;
            if (!tid) continue;
            size_t tl = strlen(tid);
            if (tl == xt->value_len
                && memcmp(tid, xt->value, tl) == 0) {
                return t->routes[i].verifier;
            }
        }
    }

    /* Fallback: match by `idp_id` from the applied policy. */
    if (idp_id) {
        for (int i = 0; i < t->n; i++) {
            const char *iid = t->routes[i].idp_id;
            if (iid && strcmp(iid, idp_id) == 0) {
                return t->routes[i].verifier;
            }
        }
    }
    return nullptr;
}
