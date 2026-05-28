/**
 * @file clickhouse/n00b_clickhouse.h
 * @brief libn00b_clickhouse — pure-C ClickHouse HTTP client substrate.
 *
 * Per D-062 (amended 2026-05-27), n00b owns the ClickHouse substrate
 * just like the AWS substrate, so multiple downstream services don't
 * each re-implement parse-URL / POST-query / map-error plumbing.
 * Unlike libn00b_aws, libn00b_clickhouse has no heavy C++ SDK
 * dependency — it speaks the documented ClickHouse HTTP protocol on
 * top of `n00b_http_request_sync` — so it is bundled into libn00b
 * directly (no separate library, no opt-in option).
 *
 * Surface (Phase 3c-parallel):
 *
 *   - Client lifecycle: `n00b_clickhouse_client` / accessors.
 *   - Identifier validation + table qualification (the bits SKP and
 *     future services duplicate today).
 *   - One-shot statement execution: `n00b_clickhouse_exec` (DDL,
 *     INSERT) and `n00b_clickhouse_query` (SELECT, returns body).
 *
 * Higher-level builders (typed row inserts, query result decode) stay
 * in the calling service; libn00b_clickhouse keeps the wire layer
 * narrow so different schemas don't have to fight for a shared API.
 */
#pragma once

#include "n00b.h"
#include "core/alloc.h"
#include "core/string.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct n00b_clickhouse_client_handle n00b_clickhouse_client_t;

typedef enum {
    N00B_CLICKHOUSE_OK              = 0,
    N00B_CLICKHOUSE_ERR_INVALID_URL = -1,
    N00B_CLICKHOUSE_ERR_HTTP        = -2,
    N00B_CLICKHOUSE_ERR_STATUS      = -3,
    N00B_CLICKHOUSE_ERR_INTERNAL    = -4,
    N00B_CLICKHOUSE_ERR_INVALID_ARG = -5,
} n00b_clickhouse_status_t;

/** @brief Static debug string for an `n00b_clickhouse_status_t` code. */
extern const char *n00b_clickhouse_status_str(n00b_clickhouse_status_t status);

/**
 * @brief Construct a ClickHouse client.
 *
 * @param url       Base URL — `http[s]://host[:port]`. Path / query /
 *                  fragment are ignored. Port defaults to 8123 for
 *                  http and 8443 for https.
 * @param database  Default database identifier. Must match
 *                  `[A-Za-z_][A-Za-z0-9_]*`; an invalid identifier
 *                  produces a `nullptr` return.
 * @kw allocator    Override n00b's default allocator. `nullptr` keeps
 *                  the default arena.
 * @return          New client, or `nullptr` on URL/identifier
 *                  validation failure. The client lives in the n00b
 *                  GC heap.
 */
extern n00b_clickhouse_client_t *
n00b_clickhouse_client(n00b_string_t *url, n00b_string_t *database) _kargs {
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Execute a one-shot statement (DDL, INSERT, etc.).
 *
 * @return `N00B_CLICKHOUSE_OK` on success, a negative status on
 *         failure. On `ERR_STATUS` the response body is available via
 *         the `out_body` argument if non-`nullptr`.
 */
extern n00b_clickhouse_status_t
n00b_clickhouse_exec(n00b_clickhouse_client_t *client,
                     n00b_string_t            *sql);

/**
 * @brief Run a SELECT and return the response body.
 *
 * The body is returned verbatim — callers are responsible for
 * decoding (FORMAT TSV, JSON, etc.). For server errors the body is
 * still populated so callers can surface the diagnostic.
 */
extern n00b_clickhouse_status_t
n00b_clickhouse_query(n00b_clickhouse_client_t *client,
                      n00b_string_t            *sql,
                      n00b_string_t           **out_body);

/**
 * @brief Validate a ClickHouse SQL identifier (database, table).
 *
 * Returns true iff the name matches `[A-Za-z_][A-Za-z0-9_]*`. The
 * check is conservative — ClickHouse itself allows backticked
 * identifiers with more chars, but the strict shape is the right
 * default for SKP-style integrations and rules out accidental SQL
 * injection through dynamically constructed table refs.
 */
extern bool n00b_clickhouse_identifier_ok(n00b_string_t *name);

/**
 * @brief Build a `database.table` reference, validating both halves.
 *
 * Returns NULL if either the client's database or @p table is not a
 * legal identifier.
 */
extern n00b_string_t *
n00b_clickhouse_qualify_table(n00b_clickhouse_client_t *client,
                              n00b_string_t            *table);

/** @brief Accessor: the host portion of the configured URL. */
extern n00b_string_t *
n00b_clickhouse_get_host(const n00b_clickhouse_client_t *client);

/** @brief Accessor: the port portion of the configured URL. */
extern int32_t
n00b_clickhouse_get_port(const n00b_clickhouse_client_t *client);

/** @brief Accessor: the configured default database identifier. */
extern n00b_string_t *
n00b_clickhouse_get_database(const n00b_clickhouse_client_t *client);

/** @brief Accessor: was the configured URL https? */
extern bool
n00b_clickhouse_is_https(const n00b_clickhouse_client_t *client);

/**
 * @brief Module init / shutdown hooks.
 *
 * Reserved for future client-pool / dictionary-cache setup. Today
 * `n00b_clickhouse_module_init` is idempotent and side-effect-free,
 * but consumers must still call it once at startup so future
 * additions land transparently.
 */
extern void n00b_clickhouse_module_init(void);
extern void n00b_clickhouse_module_shutdown(void);

#ifdef __cplusplus
}
#endif
