/* src/clickhouse/n00b_clickhouse_client.c — opaque-handle construction
 * and URL parsing for libn00b_clickhouse.
 *
 * The struct holds the parsed origin plus the configured default
 * database. Callers reach the fields through the small accessor
 * surface declared in <clickhouse/n00b_clickhouse.h>; nothing here is
 * thread-mutated after construction.
 */

#include "n00b.h"
#include "clickhouse/n00b_clickhouse.h"
#include "internal/clickhouse/client.h"

#include "core/buffer.h"
#include "text/strings/format.h"

/* ClickHouse SQL identifiers are restricted to `[A-Za-z_][A-Za-z0-9_]*`
 * — strictly ASCII, locale-independent. Inline ASCII range checks
 * avoid pulling in <ctype.h> (locale-sensitive and not on the
 * tolerated header-only set) and avoid the locale-aware unicode
 * classifiers (overkill for a strict ASCII spec). */
static inline bool
ascii_is_alpha_or_underscore(unsigned char c)
{
    return (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z')
        || c == '_';
}

static inline bool
ascii_is_alnum_or_underscore(unsigned char c)
{
    return ascii_is_alpha_or_underscore(c)
        || (c >= '0' && c <= '9');
}

bool
n00b_clickhouse_identifier_ok(n00b_string_t *name)
{
    if (!name || name->u8_bytes == 0) {
        return false;
    }
    const unsigned char *s   = (const unsigned char *)name->data;
    size_t               len = name->u8_bytes;
    if (!ascii_is_alpha_or_underscore(s[0])) {
        return false;
    }
    for (size_t i = 1; i < len; i++) {
        if (!ascii_is_alnum_or_underscore(s[i])) {
            return false;
        }
    }
    return true;
}

static int64_t
parse_decimal_u64(const char *p, size_t len)
{
    if (len == 0 || len > 5) {
        return -1;
    }
    int64_t value = 0;
    for (size_t i = 0; i < len; i++) {
        if (p[i] < '0' || p[i] > '9') {
            return -1;
        }
        value = value * 10 + (p[i] - '0');
        if (value > 65535) {
            return -1;
        }
    }
    return value;
}

static n00b_clickhouse_status_t
parse_url(n00b_string_t                    *url,
          struct n00b_clickhouse_client_handle *out)
{
    if (!url || url->u8_bytes == 0) {
        return N00B_CLICKHOUSE_ERR_INVALID_URL;
    }
    const char *p     = url->data;
    size_t      n     = url->u8_bytes;
    bool        https = false;

    if (n >= 7 && memcmp(p, "http://", 7) == 0) {
        p += 7;
        n -= 7;
    }
    else if (n >= 8 && memcmp(p, "https://", 8) == 0) {
        https = true;
        p += 8;
        n -= 8;
    }
    else {
        return N00B_CLICKHOUSE_ERR_INVALID_URL;
    }

    size_t authority_end = 0;
    while (authority_end < n && p[authority_end] != '/'
           && p[authority_end] != '?') {
        authority_end++;
    }
    if (authority_end == 0) {
        return N00B_CLICKHOUSE_ERR_INVALID_URL;
    }

    size_t colon = authority_end;
    for (size_t i = authority_end; i > 0; i--) {
        if (p[i - 1] == ':') {
            colon = i - 1;
            break;
        }
    }

    n00b_string_t *host = nullptr;
    int32_t        port = https ? 8443 : 8123;
    if (colon < authority_end && colon > 0) {
        host = n00b_string_from_raw(p, (int64_t)colon);
        int64_t parsed = parse_decimal_u64(p + colon + 1,
                                           authority_end - colon - 1);
        if (parsed <= 0) {
            return N00B_CLICKHOUSE_ERR_INVALID_URL;
        }
        port = (int32_t)parsed;
    }
    else {
        host = n00b_string_from_raw(p, (int64_t)authority_end);
    }

    out->url   = url;
    out->host  = host;
    out->port  = port;
    out->https = https;
    return N00B_CLICKHOUSE_OK;
}

n00b_clickhouse_client_t *
n00b_clickhouse_client(n00b_string_t *url, n00b_string_t *database) _kargs {
    n00b_allocator_t *allocator = nullptr;
}
{
    if (!database || !n00b_clickhouse_identifier_ok(database)) {
        return nullptr;
    }
    n00b_clickhouse_client_t *c = n00b_alloc(n00b_clickhouse_client_t,
                                             N00B_ALLOC_OPTS(allocator));
    if (!c) {
        return nullptr;
    }
    if (parse_url(url, c) != N00B_CLICKHOUSE_OK) {
        return nullptr;
    }
    c->database = database;
    return c;
}

n00b_string_t *
n00b_clickhouse_qualify_table(n00b_clickhouse_client_t *client,
                              n00b_string_t            *table)
{
    if (!client || !table || !n00b_clickhouse_identifier_ok(table)) {
        return nullptr;
    }
    if (!n00b_clickhouse_identifier_ok(client->database)) {
        return nullptr;
    }
    return n00b_cformat("[|#|].[|#|]", client->database, table);
}

n00b_string_t *
n00b_clickhouse_get_host(const n00b_clickhouse_client_t *client)
{
    return client ? client->host : nullptr;
}

int32_t
n00b_clickhouse_get_port(const n00b_clickhouse_client_t *client)
{
    return client ? client->port : 0;
}

n00b_string_t *
n00b_clickhouse_get_database(const n00b_clickhouse_client_t *client)
{
    return client ? client->database : nullptr;
}

bool
n00b_clickhouse_is_https(const n00b_clickhouse_client_t *client)
{
    return client && client->https;
}
