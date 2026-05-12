/*
 * test_http_cookies.c — Phase 6 chunk 9 unit tests.
 *
 * Coverage:
 *   - Set-Cookie parsing: minimal, all attributes, defaults
 *   - Domain rejection (cross-origin Set-Cookie)
 *   - Max-Age vs Expires precedence
 *   - Max-Age=0 deletes
 *   - Jar set + jar header_for: domain match, path match
 *   - Suffix domain match (subdomain)
 *   - Expiry-driven removal at lookup time
 *   - SameSite parsing (storage only — enforcement deferred)
 */

#define N00B_USE_INTERNAL_API
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/string.h"
#include "internal/net/http/http_url.h"
#include "internal/net/http/http_cookies.h"

static n00b_string_t *
S(const char *cstr)
{
    return n00b_string_from_cstr(cstr);
}

static n00b_http_url_t *
URL(const char *url)
{
    auto r = n00b_http_url_parse(S(url));
    assert(n00b_result_is_ok(r));
    return n00b_result_get(r);
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
test_parse_minimal(void)
{
    n00b_http_url_t *u = URL("https://example.com/path");
    n00b_http_cookie_t *c =
        n00b_http_cookie_parse("sid=abc123", u);
    assert(c);
    assert(streq(c->name, "sid"));
    assert(streq(c->value, "abc123"));
    /* Domain defaults to host; host_only true. */
    assert(streq(c->domain, "example.com"));
    assert(c->host_only);
    /* Default path is dirname of request URL — /path → /. */
    assert(streq(c->path, "/"));
    assert(c->expires_ms == 0);     /* session */
    assert(!c->secure);
    assert(!c->http_only);
    assert(c->samesite == N00B_COOKIE_SAMESITE_UNSET);
    printf("  [PASS] minimal cookie parses with defaults\n");
}

static void
test_parse_all_attrs(void)
{
    n00b_http_url_t *u = URL("https://api.example.com/v1/foo");
    n00b_http_cookie_t *c = n00b_http_cookie_parse(
        "session=xyz; Domain=example.com; Path=/v1; "
        "Max-Age=3600; Secure; HttpOnly; SameSite=Lax", u);
    assert(c);
    assert(streq(c->name, "session"));
    assert(streq(c->value, "xyz"));
    assert(streq(c->domain, "example.com"));
    assert(!c->host_only);     /* Domain attribute set */
    assert(streq(c->path, "/v1"));
    assert(c->expires_ms > 0); /* Max-Age=3600 → future */
    assert(c->secure);
    assert(c->http_only);
    assert(c->samesite == N00B_COOKIE_SAMESITE_LAX);
    printf("  [PASS] full attribute set parses\n");
}

static void
test_parse_cross_origin_domain_rejected(void)
{
    n00b_http_url_t *u = URL("https://attacker.com/");
    n00b_http_cookie_t *c =
        n00b_http_cookie_parse("evil=1; Domain=victim.com", u);
    assert(c == nullptr);     /* hard rejection */
    printf("  [PASS] cross-origin Domain= rejected\n");
}

static void
test_parse_max_age_zero_deletes(void)
{
    n00b_http_url_t *u = URL("https://example.com/");
    n00b_http_cookie_t *c =
        n00b_http_cookie_parse("a=1; Max-Age=0", u);
    assert(c);
    assert(c->expires_ms == -1);   /* delete sentinel */
    printf("  [PASS] Max-Age=0 sets delete sentinel\n");
}

static void
test_parse_samesite_strict_none(void)
{
    n00b_http_url_t *u = URL("https://example.com/");
    n00b_http_cookie_t *c =
        n00b_http_cookie_parse("a=1; SameSite=Strict", u);
    assert(c);
    assert(c->samesite == N00B_COOKIE_SAMESITE_STRICT);

    c = n00b_http_cookie_parse("a=1; SameSite=None; Secure", u);
    assert(c);
    assert(c->samesite == N00B_COOKIE_SAMESITE_NONE);
    printf("  [PASS] SameSite Strict / None parsed\n");
}

static void
test_parse_dot_domain_normalized(void)
{
    n00b_http_url_t *u = URL("https://api.example.com/");
    /* Leading dot in Domain= is allowed but ignored per RFC 6265 § 5.2.3. */
    n00b_http_cookie_t *c =
        n00b_http_cookie_parse("a=1; Domain=.example.com", u);
    assert(c);
    assert(streq(c->domain, "example.com"));
    printf("  [PASS] Domain=.example.com normalized to example.com\n");
}

/* ---- Jar ---- */

static void
test_jar_set_and_lookup(void)
{
    n00b_http_cookie_jar_t *j = n00b_http_cookie_jar_new();
    n00b_http_url_t *u = URL("https://example.com/");
    n00b_http_cookie_jar_set_from_response(j, u, "sid=abc; Path=/");
    assert(n00b_http_cookie_jar_size(j) == 1);

    n00b_string_t *h = n00b_http_cookie_jar_header_for(j, u);
    assert(h);
    assert(streq(h, "sid=abc"));

    /* Different host with no Domain attribute → host_only, won't apply. */
    n00b_http_url_t *u2 = URL("https://other.com/");
    n00b_string_t *h2 = n00b_http_cookie_jar_header_for(j, u2);
    assert(h2 == nullptr);

    n00b_http_cookie_jar_close(j);
    printf("  [PASS] jar set + header_for; host_only scoping\n");
}

static void
test_jar_subdomain_match(void)
{
    n00b_http_cookie_jar_t *j = n00b_http_cookie_jar_new();
    n00b_http_url_t *u = URL("https://example.com/");
    n00b_http_cookie_jar_set_from_response(
        j, u, "tracker=42; Domain=example.com; Path=/");

    /* Subdomain request matches Domain=example.com. */
    n00b_http_url_t *u2 = URL("https://api.example.com/");
    n00b_string_t *h = n00b_http_cookie_jar_header_for(j, u2);
    assert(h);
    assert(streq(h, "tracker=42"));
    n00b_http_cookie_jar_close(j);
    printf("  [PASS] subdomain match honors Domain=example.com\n");
}

static void
test_jar_path_match(void)
{
    n00b_http_cookie_jar_t *j = n00b_http_cookie_jar_new();
    n00b_http_url_t *u = URL("https://example.com/v1/foo");
    n00b_http_cookie_jar_set_from_response(
        j, u, "scope=v1; Path=/v1");

    /* Same path → match. */
    n00b_string_t *h = n00b_http_cookie_jar_header_for(
        j, URL("https://example.com/v1/foo"));
    assert(h && streq(h, "scope=v1"));

    /* Subpath → match. */
    h = n00b_http_cookie_jar_header_for(
        j, URL("https://example.com/v1/foo/bar"));
    assert(h && streq(h, "scope=v1"));

    /* Sibling path → no match. */
    h = n00b_http_cookie_jar_header_for(
        j, URL("https://example.com/v2/foo"));
    assert(h == nullptr);

    n00b_http_cookie_jar_close(j);
    printf("  [PASS] path-prefix scoping enforced\n");
}

static void
test_jar_replace_same_key(void)
{
    n00b_http_cookie_jar_t *j = n00b_http_cookie_jar_new();
    n00b_http_url_t *u = URL("https://example.com/");
    n00b_http_cookie_jar_set_from_response(j, u, "x=1; Path=/");
    n00b_http_cookie_jar_set_from_response(j, u, "x=2; Path=/");
    assert(n00b_http_cookie_jar_size(j) == 1);
    n00b_string_t *h = n00b_http_cookie_jar_header_for(j, u);
    assert(streq(h, "x=2"));
    n00b_http_cookie_jar_close(j);
    printf("  [PASS] re-set with same (name, domain, path) replaces\n");
}

static void
test_jar_max_age_zero_removes(void)
{
    n00b_http_cookie_jar_t *j = n00b_http_cookie_jar_new();
    n00b_http_url_t *u = URL("https://example.com/");
    n00b_http_cookie_jar_set_from_response(j, u, "x=1; Path=/");
    assert(n00b_http_cookie_jar_size(j) == 1);
    n00b_http_cookie_jar_set_from_response(j, u, "x=anything; Path=/; Max-Age=0");
    assert(n00b_http_cookie_jar_size(j) == 0);
    n00b_http_cookie_jar_close(j);
    printf("  [PASS] Max-Age=0 deletes existing cookie\n");
}

static void
test_jar_expiry(void)
{
    n00b_http_cookie_jar_t *j = n00b_http_cookie_jar_new();
    n00b_http_cookie_jar_set_now_for_test(j, 1000);
    n00b_http_url_t *u = URL("https://example.com/");
    n00b_http_cookie_jar_set_from_response(j, u, "x=1; Max-Age=10; Path=/");

    /* 5s in: still live. */
    n00b_http_cookie_jar_set_now_for_test(j, 1000 + 5000);
    assert(n00b_http_cookie_jar_size(j) == 1);
    assert(n00b_http_cookie_jar_header_for(j, u));

    /* 11s in: expired — header_for skips. */
    n00b_http_cookie_jar_set_now_for_test(j, 1000 + 11000);
    assert(n00b_http_cookie_jar_size(j) == 0);
    assert(n00b_http_cookie_jar_header_for(j, u) == nullptr);

    n00b_http_cookie_jar_close(j);
    printf("  [PASS] Max-Age expiry filters at lookup\n");
}

static void
test_jar_close_idempotent(void)
{
    n00b_http_cookie_jar_t *j = n00b_http_cookie_jar_new();
    n00b_http_url_t *u = URL("https://example.com/");
    n00b_http_cookie_jar_set_from_response(j, u, "a=1");
    n00b_http_cookie_jar_close(j);
    n00b_http_cookie_jar_close(j);
    n00b_http_cookie_jar_close(nullptr);
    printf("  [PASS] jar close idempotent + null-safe\n");
}

static void
test_parse_public_suffix_rejected(void)
{
    n00b_http_url_t *u = URL("https://www.example.co.uk/");
    /* "co.uk" is a public suffix — Domain= attribute matching it
     * would create a super-cookie. */
    n00b_http_cookie_t *c =
        n00b_http_cookie_parse("a=1; Domain=co.uk", u);
    assert(c == nullptr);
    /* Single-label TLDs likewise rejected. */
    n00b_http_url_t *u2 = URL("https://example.com/");
    c = n00b_http_cookie_parse("a=1; Domain=com", u2);
    assert(c == nullptr);
    /* Sanity: a normal Domain still works. */
    c = n00b_http_cookie_parse("a=1; Domain=example.com", u2);
    assert(c != nullptr);
    printf("  [PASS] public-suffix Domain= rejected (super-cookie defense)\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_http_cookies:\n");
    test_parse_minimal();
    test_parse_all_attrs();
    test_parse_cross_origin_domain_rejected();
    test_parse_max_age_zero_deletes();
    test_parse_samesite_strict_none();
    test_parse_dot_domain_normalized();
    test_parse_public_suffix_rejected();
    test_jar_set_and_lookup();
    test_jar_subdomain_match();
    test_jar_path_match();
    test_jar_replace_same_key();
    test_jar_max_age_zero_removes();
    test_jar_expiry();
    test_jar_close_idempotent();
    printf("All test_http_cookies tests passed.\n");

    n00b_shutdown();
    return 0;
}
