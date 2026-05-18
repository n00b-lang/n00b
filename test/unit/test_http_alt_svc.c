/*
 * test_http_alt_svc.c — Phase 6 chunk 6 unit tests.
 *
 * Coverage:
 *   - Parser: simple, multi-alt, with params, IPv6 alt-authority,
 *     same-host (omitted host), `clear` directive, malformed inputs
 *   - Cache: set + lookup, multi-entry rotation, clear directive,
 *     idle reaping, lookup misses for unknown / expired origins,
 *     test-clock injection
 */

#define N00B_USE_INTERNAL_API
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/string.h"
#include "internal/net/http/http_alt_svc.h"

static n00b_string_t *
S(const char *cstr)
{
    return n00b_string_from_cstr(cstr);
}

static bool
streq(n00b_string_t *s, const char *cstr)
{
    if (!s) return false;
    size_t cl = strlen(cstr);
    return s->u8_bytes == cl && memcmp(s->data, cstr, cl) == 0;
}

/* ---- Parser ---- */

static void
test_parse_simple(void)
{
    size_t n;
    bool   clear;
    n00b_http_alt_svc_entry_t *es =
        n00b_http_alt_svc_parse("h3=\":443\"; ma=86400", &n, &clear);
    assert(!clear);
    assert(n == 1);
    assert(streq(es[0].protocol_id, "h3"));
    assert(streq(es[0].host, ""));
    assert(es[0].port == 443);
    assert(es[0].ma_seconds == 86400);
    assert(!es[0].persist);
    printf("  [PASS] simple Alt-Svc parses\n");
}

static void
test_parse_default_ma(void)
{
    size_t n;
    bool   clear;
    n00b_http_alt_svc_entry_t *es =
        n00b_http_alt_svc_parse("h3=\":443\"", &n, &clear);
    assert(n == 1);
    /* RFC 7838 § 3.1 default = 24h */
    assert(es[0].ma_seconds == 24 * 3600);
    printf("  [PASS] default ma is 24h when omitted\n");
}

static void
test_parse_multi(void)
{
    size_t n;
    bool   clear;
    n00b_http_alt_svc_entry_t *es =
        n00b_http_alt_svc_parse(
            "h3=\":443\"; ma=86400, h3-29=\"alt.example.com:443\"; "
            "ma=3600, h2=\":443\"",
            &n, &clear);
    assert(!clear);
    assert(n == 3);
    assert(streq(es[0].protocol_id, "h3"));
    assert(es[0].port == 443);
    assert(es[0].ma_seconds == 86400);

    assert(streq(es[1].protocol_id, "h3-29"));
    assert(streq(es[1].host, "alt.example.com"));
    assert(es[1].port == 443);
    assert(es[1].ma_seconds == 3600);

    assert(streq(es[2].protocol_id, "h2"));
    assert(streq(es[2].host, ""));
    assert(es[2].port == 443);
    /* h2 has no `ma` → default. */
    assert(es[2].ma_seconds == 24 * 3600);
    printf("  [PASS] 3 alternatives parse with mixed params\n");
}

static void
test_parse_persist(void)
{
    size_t n;
    bool   clear;
    n00b_http_alt_svc_entry_t *es =
        n00b_http_alt_svc_parse("h3=\":443\"; persist=1; ma=600",
                                 &n, &clear);
    assert(n == 1);
    assert(es[0].persist == true);
    assert(es[0].ma_seconds == 600);
    printf("  [PASS] persist=1 + ma parse together\n");
}

static void
test_parse_ipv6_alt_authority(void)
{
    size_t n;
    bool   clear;
    n00b_http_alt_svc_entry_t *es =
        n00b_http_alt_svc_parse("h3=\"[2001:db8::1]:443\"; ma=300",
                                 &n, &clear);
    assert(n == 1);
    assert(streq(es[0].host, "2001:db8::1"));
    assert(es[0].port == 443);
    printf("  [PASS] IPv6 alt-authority extracted (brackets stripped)\n");
}

static void
test_parse_clear(void)
{
    size_t n   = 99;
    bool   clr = false;
    n00b_http_alt_svc_entry_t *es =
        n00b_http_alt_svc_parse("clear", &n, &clr);
    assert(es == nullptr);
    assert(n == 0);
    assert(clr == true);

    /* Case-insensitive. */
    clr = false;
    es = n00b_http_alt_svc_parse("CLEAR", &n, &clr);
    assert(clr == true);
    printf("  [PASS] clear directive recognized (case-insensitive)\n");
}

static void
test_parse_uppercase_protocol(void)
{
    /* RFC 7230 tokens are case-insensitive at the protocol level —
     * we lowercase on parse so cache lookups are byte-exact. */
    size_t n;
    bool   clear;
    n00b_http_alt_svc_entry_t *es =
        n00b_http_alt_svc_parse("H3=\":443\"", &n, &clear);
    assert(n == 1);
    assert(streq(es[0].protocol_id, "h3"));
    printf("  [PASS] protocol-id lowercased on parse\n");
}

static void
test_parse_malformed(void)
{
    size_t n;
    bool   clear;
    /* Missing '=' between protocol-id and authority. */
    n00b_http_alt_svc_entry_t *es =
        n00b_http_alt_svc_parse("h3 \":443\"", &n, &clear);
    assert(n == 0);

    /* Authority without quotes is not RFC-conformant; entry rejected. */
    es = n00b_http_alt_svc_parse("h3=:443", &n, &clear);
    assert(n == 0);

    /* Port 0 explicitly disallowed. */
    es = n00b_http_alt_svc_parse("h3=\":0\"", &n, &clear);
    assert(n == 0);

    /* Mixed valid + malformed: only the valid one survives. */
    es = n00b_http_alt_svc_parse("h3 broken, h3=\":443\"; ma=300",
                                  &n, &clear);
    assert(n == 1);
    assert(es[0].port == 443);
    printf("  [PASS] malformed alts rejected; valid sibling survives\n");
}

/* ---- Cache ---- */

static void
test_cache_set_lookup(void)
{
    n00b_http_alt_svc_cache_t *c = n00b_http_alt_svc_cache_new();
    n00b_http_alt_svc_cache_set_now_for_test(c, 1000);

    n00b_http_alt_svc_entry_t entries[] = {
        {.protocol_id = S("h3"), .host = S(""), .port = 443,
         .ma_seconds = 60, .persist = false},
    };
    n00b_http_alt_svc_cache_set(c, S("https://example.com"), entries, 1);

    n00b_string_t *host = nullptr;
    uint16_t       port = 0;
    bool ok = n00b_http_alt_svc_cache_lookup_h3(
        c, S("https://example.com"), &host, &port);
    assert(ok);
    assert(streq(host, ""));
    assert(port == 443);

    /* Different origin → miss. */
    ok = n00b_http_alt_svc_cache_lookup_h3(
        c, S("https://other.example.com"), &host, &port);
    assert(!ok);

    n00b_http_alt_svc_cache_close(c);
    printf("  [PASS] cache set + h3 lookup; origin scoping\n");
}

static void
test_cache_h3_only(void)
{
    n00b_http_alt_svc_cache_t *c = n00b_http_alt_svc_cache_new();
    n00b_http_alt_svc_cache_set_now_for_test(c, 0);

    n00b_http_alt_svc_entry_t entries[] = {
        {.protocol_id = S("h2"), .host = S(""), .port = 443,
         .ma_seconds = 60},
    };
    n00b_http_alt_svc_cache_set(c, S("o"), entries, 1);

    /* h2-only entry doesn't satisfy h3 lookup. */
    bool ok = n00b_http_alt_svc_cache_lookup_h3(c, S("o"), nullptr, nullptr);
    assert(!ok);
    n00b_http_alt_svc_cache_close(c);
    printf("  [PASS] non-h3 entries don't satisfy h3 lookup\n");
}

static void
test_cache_expiry(void)
{
    n00b_http_alt_svc_cache_t *c = n00b_http_alt_svc_cache_new();
    n00b_http_alt_svc_cache_set_now_for_test(c, 1000);

    n00b_http_alt_svc_entry_t entries[] = {
        {.protocol_id = S("h3"), .host = S(""), .port = 443,
         .ma_seconds = 60},
    };
    n00b_http_alt_svc_cache_set(c, S("o"), entries, 1);

    /* 1 ms before expiry: hit. */
    n00b_http_alt_svc_cache_set_now_for_test(c, 1000 + 60000 - 1);
    assert(n00b_http_alt_svc_cache_lookup_h3(c, S("o"), nullptr, nullptr));

    /* At expiry: miss. */
    n00b_http_alt_svc_cache_set_now_for_test(c, 1000 + 60000);
    assert(!n00b_http_alt_svc_cache_lookup_h3(c, S("o"), nullptr, nullptr));
    /* The expired entry is removed by lookup. */
    assert(n00b_http_alt_svc_cache_size(c) == 0);

    n00b_http_alt_svc_cache_close(c);
    printf("  [PASS] expiry threshold + lookup-driven removal\n");
}

static void
test_cache_clear(void)
{
    n00b_http_alt_svc_cache_t *c = n00b_http_alt_svc_cache_new();
    n00b_http_alt_svc_cache_set_now_for_test(c, 0);

    n00b_http_alt_svc_entry_t entries[] = {
        {.protocol_id = S("h3"), .host = S(""), .port = 443,
         .ma_seconds = 600},
    };
    n00b_http_alt_svc_cache_set(c, S("a"), entries, 1);
    n00b_http_alt_svc_cache_set(c, S("b"), entries, 1);
    assert(n00b_http_alt_svc_cache_size(c) == 2);

    n00b_http_alt_svc_cache_clear(c, S("a"));
    assert(n00b_http_alt_svc_cache_size(c) == 1);
    assert(!n00b_http_alt_svc_cache_lookup_h3(c, S("a"), nullptr, nullptr));
    assert(n00b_http_alt_svc_cache_lookup_h3(c, S("b"), nullptr, nullptr));

    n00b_http_alt_svc_cache_close(c);
    printf("  [PASS] per-origin clear leaves siblings alone\n");
}

static void
test_cache_reap(void)
{
    n00b_http_alt_svc_cache_t *c = n00b_http_alt_svc_cache_new();
    n00b_http_alt_svc_cache_set_now_for_test(c, 0);

    n00b_http_alt_svc_entry_t a_entry[] = {
        {.protocol_id = S("h3"), .host = S(""), .port = 443,
         .ma_seconds = 60},  /* expires at 60_000 ms */
    };
    n00b_http_alt_svc_entry_t b_entry[] = {
        {.protocol_id = S("h3"), .host = S(""), .port = 443,
         .ma_seconds = 600}, /* expires at 600_000 ms */
    };
    n00b_http_alt_svc_cache_set(c, S("a"), a_entry, 1);
    n00b_http_alt_svc_cache_set(c, S("b"), b_entry, 1);
    assert(n00b_http_alt_svc_cache_size(c) == 2);

    /* Reap at 90s: drops a, keeps b. */
    n00b_http_alt_svc_cache_reap(c, 90 * 1000);
    assert(n00b_http_alt_svc_cache_size(c) == 1);
    assert(n00b_http_alt_svc_cache_lookup_h3(c, S("b"), nullptr, nullptr));

    n00b_http_alt_svc_cache_close(c);
    printf("  [PASS] reap drops only expired buckets\n");
}

static void
test_cache_set_replaces(void)
{
    n00b_http_alt_svc_cache_t *c = n00b_http_alt_svc_cache_new();
    n00b_http_alt_svc_cache_set_now_for_test(c, 0);

    n00b_http_alt_svc_entry_t e1[] = {
        {.protocol_id = S("h3"), .host = S(""), .port = 443,
         .ma_seconds = 60},
    };
    n00b_http_alt_svc_entry_t e2[] = {
        {.protocol_id = S("h3"), .host = S(""), .port = 8443,
         .ma_seconds = 60},
    };
    n00b_http_alt_svc_cache_set(c, S("o"), e1, 1);
    n00b_http_alt_svc_cache_set(c, S("o"), e2, 1);
    assert(n00b_http_alt_svc_cache_size(c) == 1);   /* replaced, not duplicated */

    n00b_string_t *host = nullptr;
    uint16_t       port = 0;
    bool ok = n00b_http_alt_svc_cache_lookup_h3(c, S("o"), &host, &port);
    assert(ok);
    assert(port == 8443);                            /* second entry won */

    n00b_http_alt_svc_cache_close(c);
    printf("  [PASS] set on existing origin replaces, not duplicates\n");
}

static void
test_cache_close_idempotent(void)
{
    n00b_http_alt_svc_cache_t *c = n00b_http_alt_svc_cache_new();
    n00b_http_alt_svc_entry_t entries[] = {
        {.protocol_id = S("h3"), .host = S(""), .port = 443, .ma_seconds = 60},
    };
    n00b_http_alt_svc_cache_set(c, S("o"), entries, 1);
    n00b_http_alt_svc_cache_close(c);
    /* Second close: no-op (no asserts; just must not crash). */
    n00b_http_alt_svc_cache_close(c);
    /* Close on nullptr: no-op. */
    n00b_http_alt_svc_cache_close(nullptr);
    printf("  [PASS] cache close is idempotent + null-safe\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_http_alt_svc:\n");
    test_parse_simple();
    test_parse_default_ma();
    test_parse_multi();
    test_parse_persist();
    test_parse_ipv6_alt_authority();
    test_parse_clear();
    test_parse_uppercase_protocol();
    test_parse_malformed();
    test_cache_set_lookup();
    test_cache_h3_only();
    test_cache_expiry();
    test_cache_clear();
    test_cache_reap();
    test_cache_set_replaces();
    test_cache_close_idempotent();
    printf("All test_http_alt_svc tests passed.\n");

    n00b_shutdown();
    return 0;
}
