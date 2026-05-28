/** @file test/unit/test_clickhouse_module.c — libn00b_clickhouse skeleton.
 *
 *  Phase 3c-parallel smoke test. Covers the bits that do not need a
 *  live ClickHouse endpoint:
 *
 *    - module init idempotency
 *    - identifier validation
 *    - URL parsing (http + https, default + explicit port, bad input)
 *    - table qualification (database.table) honours identifier rules
 *
 *  Live HTTP exec/query tests run against the docker-backed
 *  ClickHouse used by SKP's `test-clickhouse` script — they belong on
 *  the consumer side until libn00b_clickhouse gains its own
 *  optional-integration suite. The module-skeleton test here keeps
 *  build + link health visible from the n00b repo alone.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/string.h"

#include "clickhouse/n00b_clickhouse.h"

static void
test_module_init(void)
{
    n00b_clickhouse_module_init();
    n00b_clickhouse_module_init();
    n00b_clickhouse_module_shutdown();
    n00b_clickhouse_module_init();
    printf("  [PASS] module_init\n");
}

static void
test_identifier_ok(void)
{
    assert(n00b_clickhouse_identifier_ok(n00b_string_from_cstr("skp")));
    assert(n00b_clickhouse_identifier_ok(n00b_string_from_cstr("_evidence")));
    assert(n00b_clickhouse_identifier_ok(
        n00b_string_from_cstr("evidence_row_2026")));
    assert(!n00b_clickhouse_identifier_ok(n00b_string_from_cstr("")));
    assert(!n00b_clickhouse_identifier_ok(n00b_string_from_cstr("1bad")));
    assert(!n00b_clickhouse_identifier_ok(
        n00b_string_from_cstr("evidence; DROP TABLE foo")));
    assert(!n00b_clickhouse_identifier_ok(n00b_string_from_cstr("a.b")));
    printf("  [PASS] identifier_ok\n");
}

static void
test_client_url_defaults(void)
{
    n00b_clickhouse_client_t *c = n00b_clickhouse_client(
        n00b_string_from_cstr("http://localhost"),
        n00b_string_from_cstr("skp"));
    assert(c != nullptr);
    assert(n00b_clickhouse_get_port(c) == 8123);
    assert(!n00b_clickhouse_is_https(c));
    n00b_string_t *host = n00b_clickhouse_get_host(c);
    assert(host && strcmp(host->data, "localhost") == 0);

    c = n00b_clickhouse_client(
        n00b_string_from_cstr("https://clickhouse.internal"),
        n00b_string_from_cstr("skp"));
    assert(c != nullptr);
    assert(n00b_clickhouse_get_port(c) == 8443);
    assert(n00b_clickhouse_is_https(c));
    printf("  [PASS] client_url_defaults\n");
}

static void
test_client_url_explicit_port(void)
{
    n00b_clickhouse_client_t *c = n00b_clickhouse_client(
        n00b_string_from_cstr("http://127.0.0.1:9000"),
        n00b_string_from_cstr("skp"));
    assert(c != nullptr);
    assert(n00b_clickhouse_get_port(c) == 9000);
    assert(!n00b_clickhouse_is_https(c));
    printf("  [PASS] client_url_explicit_port\n");
}

static void
test_client_invalid_inputs(void)
{
    assert(n00b_clickhouse_client(
               n00b_string_from_cstr(""),
               n00b_string_from_cstr("skp"))
           == nullptr);
    assert(n00b_clickhouse_client(
               n00b_string_from_cstr("ftp://example.com"),
               n00b_string_from_cstr("skp"))
           == nullptr);
    assert(n00b_clickhouse_client(
               n00b_string_from_cstr("http://host:0"),
               n00b_string_from_cstr("skp"))
           == nullptr);
    assert(n00b_clickhouse_client(
               n00b_string_from_cstr("http://host:99999"),
               n00b_string_from_cstr("skp"))
           == nullptr);
    assert(n00b_clickhouse_client(
               n00b_string_from_cstr("http://host"),
               n00b_string_from_cstr("1bad"))
           == nullptr);
    printf("  [PASS] client_invalid_inputs\n");
}

static void
test_qualify_table(void)
{
    n00b_clickhouse_client_t *c = n00b_clickhouse_client(
        n00b_string_from_cstr("http://host"),
        n00b_string_from_cstr("skp"));
    n00b_string_t *qualified = n00b_clickhouse_qualify_table(
        c, n00b_string_from_cstr("evidence"));
    assert(qualified != nullptr);
    assert(strcmp(qualified->data, "skp.evidence") == 0);

    assert(n00b_clickhouse_qualify_table(
               c, n00b_string_from_cstr("1bad"))
           == nullptr);
    printf("  [PASS] qualify_table\n");
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);

    printf("== libn00b_clickhouse skeleton ==\n");
    test_module_init();
    test_identifier_ok();
    test_client_url_defaults();
    test_client_url_explicit_port();
    test_client_invalid_inputs();
    test_qualify_table();

    n00b_clickhouse_module_shutdown();
    printf("All libn00b_clickhouse skeleton tests passed.\n");
    return 0;
}
