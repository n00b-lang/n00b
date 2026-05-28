/**
 * @file internal/clickhouse/client.h
 * @brief Internal layout of `n00b_clickhouse_client_t`.
 *
 * Private to libn00b_clickhouse — consumers go through the accessors
 * in <clickhouse/n00b_clickhouse.h>. Lives in internal/ so other
 * libn00b_clickhouse translation units share the same definition
 * without re-declaring it.
 */
#pragma once

#include "n00b.h"

struct n00b_clickhouse_client_handle {
    n00b_string_t *url;
    n00b_string_t *host;
    n00b_string_t *database;
    int32_t        port;
    bool           https;
};
