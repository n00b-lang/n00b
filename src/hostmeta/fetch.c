/** @file src/hostmeta/fetch.c — metadata-endpoint HTTP transport.
 *
 *  Every cloud metadata service (AWS IMDS, Azure IMDS, GCP's metadata
 *  server, the ECS task endpoint) is plain HTTP on a link-local
 *  address that serves no TLS, so this goes through the n00b HTTP
 *  client's `allow_plain_http` path. That path is one-shot — fresh
 *  socket per request, nothing pooled — which is what we want for an
 *  endpoint we hit a few dozen times and never again.
 *
 *  Timeouts are short and retries are few on purpose: off-cloud, every
 *  one of these requests is going to fail, and the collector must not
 *  turn a laptop build into a multi-second stall.
 */

#define N00B_USE_INTERNAL_API

#include "internal/hostmeta/hostmeta_internal.h"

#include "core/buffer.h"
#include "internal/net/http/http_h1.h"
#include "text/strings/format.h"
#include "text/strings/string_ops.h"

n00b_string_t *
n00b_hostmeta_metadata_url(n00b_hostmeta_ctx_t *ctx, const char *path)
{
    n00b_string_t *ip = (ctx && ctx->metadata_ip) ? ctx->metadata_ip
                                                  : r"169.254.169.254";
    return n00b_cformat("http://[|#|][|#|]", ip, n00b_hostmeta_str(path));
}

static n00b_hostmeta_fetch_t
fetch_failed(n00b_string_t *error)
{
    return (n00b_hostmeta_fetch_t){
        .ok     = false,
        .status = 0,
        .body   = nullptr,
        .error  = error,
    };
}

n00b_hostmeta_fetch_t
n00b_hostmeta_fetch(n00b_hostmeta_ctx_t *ctx, n00b_string_t *url) _kargs
{
    const char             *method     = nullptr;
    n00b_http_h1_headers_t *headers    = nullptr;
    bool                    strict     = false;
    int32_t                 timeout_ms = 0;
}
{
    if (ctx == nullptr || url == nullptr) {
        return fetch_failed(r"invalid metadata request");
    }
    if (!ctx->allow_network) {
        return fetch_failed(r"network metadata collection is disabled");
    }

    int32_t attempts = ctx->retries < 0 ? 1 : ctx->retries + 1;
    int32_t timeout  = timeout_ms > 0 ? timeout_ms
                                      : (ctx->timeout_ms > 0 ? ctx->timeout_ms
                                                             : 1000);

    n00b_string_t *last_error = nullptr;
    int            last_status = 0;

    for (int32_t attempt = 0; attempt < attempts; attempt++) {
        auto r = n00b_http_request_sync(url,
                                        .method           = n00b_hostmeta_str(method),
                                        .extra            = headers,
                                        .allow_plain_http = true,
                                        .prefer_h3        = false,
                                        .timeout_ms       = timeout);
        if (n00b_result_is_err(r)) {
            last_error = n00b_cformat("[|#|]: transport error [|#|]",
                                      url,
                                      (int64_t)n00b_result_get_err(r));
            continue;
        }

        n00b_http_response_t *resp = n00b_result_get(r);
        int                   code = n00b_http_response_status(resp);
        last_status                = code;

        if (code == 404 && !strict) {
            // Not an error: metadata trees answer 404 for attributes
            // this particular instance simply does not have.
            return (n00b_hostmeta_fetch_t){
                .ok     = true,
                .status = code,
                .body   = n00b_string_empty(),
                .error  = nullptr,
            };
        }

        if (code < 200 || code > 299) {
            last_error = n00b_cformat("[|#|]: HTTP [|#|]", url, (int64_t)code);
            continue;
        }

        n00b_buffer_t *body = n00b_http_response_body(resp);
        n00b_string_t *text = r"";
        if (body != nullptr && body->byte_len > 0) {
            text = n00b_unicode_str_trim(
                n00b_string_from_raw(body->data, (int64_t)body->byte_len));
        }

        return (n00b_hostmeta_fetch_t){
            .ok     = true,
            .status = code,
            .body   = text,
            .error  = nullptr,
        };
    }

    n00b_hostmeta_fetch_t out = fetch_failed(
        last_error ? last_error : r"metadata endpoint unreachable");
    out.status = last_status;
    return out;
}
