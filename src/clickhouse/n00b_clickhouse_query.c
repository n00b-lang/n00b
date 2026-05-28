/* src/clickhouse/n00b_clickhouse_query.c — wire-layer exec + query
 * helpers for libn00b_clickhouse.
 *
 * The transport is `n00b_http_request_sync` with `allow_plain_http`
 * set so the default ClickHouse port (8123) works without TLS. HTTPS
 * URLs route through the same call without the plain-http opt-in.
 * Responses are byte-for-byte exposed to the caller; ClickHouse
 * negotiates the format via FORMAT clauses in the SQL itself, so the
 * substrate stays format-agnostic.
 */

#include "n00b.h"
#include "clickhouse/n00b_clickhouse.h"
#include "internal/clickhouse/client.h"

#include "core/buffer.h"
#include "core/string.h"
#include "net/http/http_client.h"
#include "text/strings/format.h"

static n00b_string_t *
build_endpoint(const n00b_clickhouse_client_t *client)
{
    n00b_string_t *scheme = client->https
                                ? n00b_string_from_cstr("https")
                                : n00b_string_from_cstr("http");
    return n00b_cformat("[|#|]://[|#|]:[|#|]/",
                        scheme,
                        client->host,
                        (int64_t)client->port);
}

static n00b_clickhouse_status_t
post_query(const n00b_clickhouse_client_t *client,
           n00b_string_t                  *sql,
           n00b_string_t                 **out_body)
{
    if (out_body) {
        *out_body = nullptr;
    }
    if (!client || !sql) {
        return N00B_CLICKHOUSE_ERR_INVALID_ARG;
    }

    n00b_string_t *url = build_endpoint(client);
    n00b_buffer_t *body = n00b_buffer_from_bytes(
        sql->data,
        (int64_t)sql->u8_bytes);

    n00b_string_t *method       = n00b_string_from_cstr("POST");
    n00b_string_t *content_type = n00b_string_from_cstr(
        "text/plain; charset=utf-8");

    auto request_result = n00b_http_request_sync(
        url,
        .method           = method,
        .body             = body,
        .content_type     = content_type,
        .allow_plain_http = !client->https);

    if (n00b_result_is_err(request_result)) {
        return N00B_CLICKHOUSE_ERR_HTTP;
    }

    n00b_http_response_t *resp   = n00b_result_get(request_result);
    int                   status = n00b_http_response_status(resp);
    n00b_buffer_t        *raw    = n00b_http_response_body(resp);
    n00b_string_t        *resp_text = raw ? n00b_buffer_to_string(raw)
                                          : n00b_string_empty();

    if (out_body) {
        *out_body = resp_text;
    }

    if (status == 0) {
        return N00B_CLICKHOUSE_ERR_HTTP;
    }
    if (status < 200 || status >= 300) {
        return N00B_CLICKHOUSE_ERR_STATUS;
    }
    return N00B_CLICKHOUSE_OK;
}

n00b_clickhouse_status_t
n00b_clickhouse_exec(n00b_clickhouse_client_t *client,
                     n00b_string_t            *sql)
{
    n00b_string_t *unused = nullptr;
    return post_query(client, sql, &unused);
}

n00b_clickhouse_status_t
n00b_clickhouse_query(n00b_clickhouse_client_t *client,
                      n00b_string_t            *sql,
                      n00b_string_t           **out_body)
{
    return post_query(client, sql, out_body);
}
