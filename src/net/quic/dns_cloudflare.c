/*
 * dns_cloudflare.c — Cloudflare DNS provider (API-token-based).
 *
 * Talks to api.cloudflare.com via the existing HTTPS shim.
 * Operations:
 *
 *   set_txt(fqdn, value):
 *     POST /client/v4/zones/{zone_id}/dns_records
 *       { "type": "TXT", "name": fqdn, "content": value, "ttl": 60 }
 *     Auth: Bearer <api_token>
 *
 *   remove_txt(fqdn, value):
 *     GET /client/v4/zones/{zone_id}/dns_records?type=TXT&name=<fqdn>
 *       → list of records (filter by name)
 *     For each match (or just the matching value if value is non-empty):
 *       DELETE /client/v4/zones/{zone_id}/dns_records/{id}
 *
 * Cloudflare's API is documented at
 * https://developers.cloudflare.com/api/operations/dns-records-for-a-zone-create-dns-record
 */

#define N00B_USE_INTERNAL_API
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "core/string.h"
#include "adt/result.h"
#include "adt/dict_untyped.h"
#include "adt/list.h"
#include "parsers/json.h"
#include "net/quic/quic_types.h"
#include "net/quic/dns_provider.h"
#include "net/http/http_client.h"
#include "internal/net/http/http_h1.h"

typedef struct {
    char *api_token;
    char *zone_id;
} cf_state_t;

static n00b_allocator_t *
cf_alloc(void)
{
    return (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;
}

static char *
cf_strdup(const char *s)
{
    if (!s) return nullptr;
    size_t l = strlen(s);
    char  *o = n00b_alloc_array_with_opts(char, (int64_t)(l + 1),
                                          &(n00b_alloc_opts_t){
                                              .allocator = cf_alloc(),
                                              .no_scan   = true,
                                          });
    memcpy(o, s, l + 1);
    return o;
}

/* Build a "Authorization: Bearer ..." header bag for the Cloudflare
 * API.  Returned bag is owned by the caller's stack frame. */
static n00b_http_h1_headers_t *
cf_auth_headers(const char *api_token)
{
    n00b_http_h1_headers_t *h = n00b_http_h1_headers_new();
    char buf[1024];
    snprintf(buf, sizeof(buf), "Bearer %s", api_token);
    n00b_http_h1_headers_set(h, "Authorization", buf);
    n00b_http_h1_headers_set(h, "Content-Type", "application/json");
    return h;
}

static int
cf_set_txt(n00b_quic_dns_provider_t *self,
           const char               *fqdn,
           const char               *value)
{
    cf_state_t *st = self->ctx;

    char url[512];
    snprintf(url, sizeof(url),
             "https://api.cloudflare.com/client/v4/zones/%s/dns_records",
             st->zone_id);

    /* JSON body — escapes are minimal because TXT values from
     * RFC 8555 § 8.4 are base64url and fqdns are DNS labels.  No
     * quotes / backslashes need escaping. */
    char body_str[1024];
    int n = snprintf(body_str, sizeof(body_str),
                     "{\"type\":\"TXT\",\"name\":\"%s\","
                     "\"content\":\"%s\",\"ttl\":60}",
                     fqdn, value);
    if (n < 0 || n >= (int)sizeof(body_str)) {
        return N00B_QUIC_ERR_FRAME_TOO_LARGE;
    }
    n00b_buffer_t *body = n00b_buffer_from_bytes(body_str, n);

    auto r = n00b_http_request_sync(
        n00b_string_from_cstr((char *)url),
        .method       = n00b_string_from_cstr("POST"),
        .body         = body,
        .extra        = cf_auth_headers(st->api_token),
        .content_type = n00b_string_from_cstr("application/json"),
        .prefer_h3    = false);
    if (!n00b_result_is_ok(r)) {
        return (int)n00b_result_get_err(r);
    }
    n00b_http_response_t *resp = n00b_result_get(r);
    if (n00b_http_response_status(resp) != 200) {
        return N00B_QUIC_ERR_PROTOCOL;
    }
    return N00B_QUIC_OK;
}

/* Delete every TXT record at @p fqdn whose content matches @p value
 * (or every TXT record at @p fqdn if value is empty). */
static int
cf_remove_txt(n00b_quic_dns_provider_t *self,
              const char               *fqdn,
              const char               *value)
{
    cf_state_t *st = self->ctx;

    char list_url[1024];
    snprintf(list_url, sizeof(list_url),
             "https://api.cloudflare.com/client/v4/zones/%s/"
             "dns_records?type=TXT&name=%s",
             st->zone_id, fqdn);

    auto lr = n00b_http_request_sync(
        n00b_string_from_cstr((char *)list_url),
        .extra     = cf_auth_headers(st->api_token),
        .prefer_h3 = false);
    if (!n00b_result_is_ok(lr)) {
        return (int)n00b_result_get_err(lr);
    }
    n00b_http_response_t *resp = n00b_result_get(lr);
    n00b_buffer_t *resp_body = n00b_http_response_body(resp);
    if (n00b_http_response_status(resp) != 200 || !resp_body) {
        return N00B_QUIC_ERR_PROTOCOL;
    }

    /* Parse the listing.  Cloudflare's response shape is:
     *   { "result": [ {"id": "...", "content": "...", ...}, ... ],
     *     "success": true, ... }
     */
    const char       *err  = nullptr;
    n00b_json_node_t *root = n00b_json_parse(resp_body->data,
                                             (size_t)resp_body->byte_len,
                                             &err);
    if (!root || !n00b_json_is_object(root)) {
        return N00B_QUIC_ERR_PROTOCOL;
    }
    bool found = false;
    n00b_json_node_t *result = nullptr;
    void *rv = n00b_dict_untyped_get(root->object, (void *)"result", &found);
    result = found ? (n00b_json_node_t *)rv : nullptr;
    if (!result || !n00b_json_is_array(result)) {
        return N00B_QUIC_ERR_PROTOCOL;
    }

    size_t n = (size_t)n00b_list_len(result->array);
    for (size_t i = 0; i < n; i++) {
        n00b_json_node_t *rec = n00b_list_get(result->array, i);
        if (!rec || !n00b_json_is_object(rec)) continue;

        bool found_id = false, found_content = false;
        n00b_json_node_t *id      = n00b_dict_untyped_get(rec->object,
                                                          (void *)"id",
                                                          &found_id);
        n00b_json_node_t *content = n00b_dict_untyped_get(rec->object,
                                                          (void *)"content",
                                                          &found_content);
        if (!found_id || !id || !n00b_json_is_string(id)) continue;

        /* If the caller passed a value, only delete records whose
         * content matches; else delete all TXT records at the name. */
        if (value && value[0] != '\0') {
            if (!found_content || !content
                || !n00b_json_is_string(content)) continue;
            if (strcmp(content->string, value) != 0) continue;
        }

        char del_url[1024];
        snprintf(del_url, sizeof(del_url),
                 "https://api.cloudflare.com/client/v4/zones/%s/"
                 "dns_records/%s",
                 st->zone_id, id->string);
        auto dr = n00b_http_request_sync(
            n00b_string_from_cstr((char *)del_url),
            .method    = n00b_string_from_cstr("DELETE"),
            .extra     = cf_auth_headers(st->api_token),
            .prefer_h3 = false);
        if (!n00b_result_is_ok(dr)) {
            return (int)n00b_result_get_err(dr);
        }
        /* Cloudflare returns 200 on successful delete; we don't
         * insist on it (idempotent semantics). */
    }
    return N00B_QUIC_OK;
}

static void
cf_close(n00b_quic_dns_provider_t *self)
{
    if (!self || !self->ctx) return;
    cf_state_t *st = self->ctx;
    /* Best-effort: scrub the api_token bytes before we drop the
     * pointer.  Conduit-pool memory persists until process exit
     * but the GC won't trace into the no_scan buffer. */
    if (st->api_token) {
        memset(st->api_token, 0, strlen(st->api_token));
    }
    self->ctx = nullptr;
}

n00b_result_t(n00b_quic_dns_provider_t *)
n00b_quic_dns_provider_cloudflare(const char *api_token, const char *zone_id)
{
    if (!api_token || !zone_id ||
        api_token[0] == '\0' || zone_id[0] == '\0') {
        return n00b_result_err(n00b_quic_dns_provider_t *,
                               N00B_QUIC_ERR_NULL_ARG);
    }

    cf_state_t *st = n00b_alloc_with_opts(cf_state_t,
        &(n00b_alloc_opts_t){.allocator = cf_alloc()});
    st->api_token = cf_strdup(api_token);
    st->zone_id   = cf_strdup(zone_id);

    n00b_quic_dns_provider_t *p = n00b_alloc_with_opts(
        n00b_quic_dns_provider_t,
        &(n00b_alloc_opts_t){.allocator = cf_alloc()});
    p->name       = "cloudflare";
    p->set_txt    = cf_set_txt;
    p->remove_txt = cf_remove_txt;
    p->close      = cf_close;
    p->ctx        = st;
    return n00b_result_ok(n00b_quic_dns_provider_t *, p);
}
