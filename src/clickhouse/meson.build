# src/clickhouse/meson.build — libn00b_clickhouse source aggregation.
#
# Pure-C ClickHouse HTTP substrate. Folded into libn00b directly (no
# separate library, no opt-in option) because it depends only on
# n00b_http_request_sync from the n00b core. Other libn00b consumers
# pay zero extra link cost when they do not use ClickHouse.

n00b_clickhouse_src = files(
    'n00b_clickhouse_module.c',
    'n00b_clickhouse_client.c',
    'n00b_clickhouse_query.c',
)
