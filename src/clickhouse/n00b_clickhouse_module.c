/* src/clickhouse/n00b_clickhouse_module.c — process-wide init hook
 * for libn00b_clickhouse.
 *
 * Today the module has no state to set up — the HTTP client substrate
 * (n00b_http_request_sync) handles its own pooling and TLS material.
 * The init / shutdown functions are reserved so future additions
 * (e.g., a per-process result-set arena, a dictionary cache, or a
 * shared connection pool) land transparently for callers.
 */

#include "n00b.h"
#include "clickhouse/n00b_clickhouse.h"

void
n00b_clickhouse_module_init(void)
{
    /* intentional no-op for Phase 3c-parallel; see file header. */
}

void
n00b_clickhouse_module_shutdown(void)
{
    /* intentional no-op for Phase 3c-parallel; see file header. */
}

const char *
n00b_clickhouse_status_str(n00b_clickhouse_status_t status)
{
    switch (status) {
    case N00B_CLICKHOUSE_OK:              return "OK";
    case N00B_CLICKHOUSE_ERR_INVALID_URL: return "INVALID_URL";
    case N00B_CLICKHOUSE_ERR_HTTP:        return "HTTP";
    case N00B_CLICKHOUSE_ERR_STATUS:      return "STATUS";
    case N00B_CLICKHOUSE_ERR_INTERNAL:    return "INTERNAL";
    case N00B_CLICKHOUSE_ERR_INVALID_ARG: return "INVALID_ARG";
    }
    return "UNKNOWN";
}
