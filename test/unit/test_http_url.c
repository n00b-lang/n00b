/*
 * test_http_url.c — Phase 6 chunk 1: URL parser unit tests.
 *
 * Coverage:
 *   - HTTPS-only acceptance + scheme-rejection set
 *   - Default port + explicit port + bracketed IPv6 literal
 *   - Path / query / fragment partitioning
 *   - Origin canonicalization (port-omission rules)
 *   - Userinfo rejection
 *   - Malformed authority + port edge cases
 */

#define N00B_USE_INTERNAL_API
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/string.h"
#include "adt/result.h"
#include "internal/net/http/http_url.h"

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

static n00b_http_url_t *
parse_ok(const char *url)
{
    auto r = n00b_http_url_parse(S(url));
    if (n00b_result_is_err(r)) {
        fprintf(stderr,
                "  [FAIL] expected success for <%s>, got err=%d (%s)\n",
                url,
                (int)n00b_result_get_err(r),
                n00b_http_err_str((n00b_http_err_t)n00b_result_get_err(r)));
        abort();
    }
    return n00b_result_get(r);
}

static int32_t
parse_err(const char *url)
{
    auto r = n00b_http_url_parse(S(url));
    if (n00b_result_is_ok(r)) {
        fprintf(stderr,
                "  [FAIL] expected error for <%s>, but parse succeeded\n",
                url);
        abort();
    }
    return (int32_t)n00b_result_get_err(r);
}

static void
test_basic_https(void)
{
    n00b_http_url_t *u = parse_ok("https://example.com/");
    assert(streq(u->scheme, "https"));
    assert(streq(u->host, "example.com"));
    assert(!u->is_ipv6_literal);
    assert(u->port == 443);
    assert(!u->has_explicit_port);
    assert(streq(u->path, "/"));
    assert(streq(u->query, ""));
    assert(streq(u->origin, "https://example.com"));
    printf("  [PASS] basic https://example.com/\n");
}

static void
test_default_path(void)
{
    n00b_http_url_t *u = parse_ok("https://example.com");
    assert(streq(u->path, "/"));
    assert(streq(u->origin, "https://example.com"));
    printf("  [PASS] no-path defaults path to /\n");
}

static void
test_explicit_port(void)
{
    n00b_http_url_t *u = parse_ok("https://example.com:8443/api");
    assert(u->port == 8443);
    assert(u->has_explicit_port);
    assert(streq(u->path, "/api"));
    assert(streq(u->origin, "https://example.com:8443"));
    printf("  [PASS] explicit port preserved + included in origin\n");
}

static void
test_explicit_443_omitted_from_origin(void)
{
    n00b_http_url_t *u = parse_ok("https://example.com:443/x");
    assert(u->port == 443);
    assert(u->has_explicit_port);
    assert(streq(u->origin, "https://example.com"));
    printf("  [PASS] explicit :443 omitted from origin canonical form\n");
}

static void
test_query(void)
{
    n00b_http_url_t *u = parse_ok("https://example.com/path?a=1&b=2");
    assert(streq(u->path, "/path"));
    assert(streq(u->query, "a=1&b=2"));
    printf("  [PASS] path + query split\n");
}

static void
test_query_only(void)
{
    n00b_http_url_t *u = parse_ok("https://example.com?a=1");
    assert(streq(u->path, "/"));
    assert(streq(u->query, "a=1"));
    printf("  [PASS] no path but query present → path defaults to /\n");
}

static void
test_fragment_stripped(void)
{
    n00b_http_url_t *u = parse_ok("https://example.com/x?y=z#frag");
    assert(streq(u->path, "/x"));
    assert(streq(u->query, "y=z"));
    /* fragment must not appear anywhere in the parsed output */
    assert(!streq(u->query, "y=z#frag"));
    printf("  [PASS] fragment stripped (HTTP URIs do not carry it)\n");
}

static void
test_ipv6_literal(void)
{
    n00b_http_url_t *u = parse_ok("https://[::1]:8443/");
    assert(u->is_ipv6_literal);
    assert(streq(u->host, "::1"));         /* brackets stripped from host */
    assert(u->port == 8443);
    assert(streq(u->origin, "https://[::1]:8443"));
    printf("  [PASS] IPv6 literal: [::1]:8443\n");
}

static void
test_ipv6_default_port(void)
{
    n00b_http_url_t *u = parse_ok("https://[2001:db8::1]/x");
    assert(u->is_ipv6_literal);
    assert(streq(u->host, "2001:db8::1"));
    assert(u->port == 443);
    assert(!u->has_explicit_port);
    assert(streq(u->origin, "https://[2001:db8::1]"));
    assert(streq(u->path, "/x"));
    printf("  [PASS] IPv6 with default port re-brackets host in origin\n");
}

static void
test_ipv6_explicit_443_omitted(void)
{
    /* Same canonicalization rule as IPv4 hosts: explicit :443 is
     * stripped from origin so cache keys collapse. */
    n00b_http_url_t *u = parse_ok("https://[::1]:443/");
    assert(u->is_ipv6_literal);
    assert(u->port == 443);
    assert(u->has_explicit_port);
    assert(streq(u->origin, "https://[::1]"));
    printf("  [PASS] IPv6 :443 omitted from origin\n");
}

static void
test_case_insensitive_scheme(void)
{
    n00b_http_url_t *u = parse_ok("HTTPS://Example.COM/");
    assert(streq(u->scheme, "https"));
    assert(streq(u->host, "example.com"));
    printf("  [PASS] scheme + host lowercased\n");
}

static void
test_reject_http(void)
{
    int32_t e = parse_err("http://example.com/");
    assert(e == N00B_HTTP_ERR_UNSUPPORTED_SCHEME);
    printf("  [PASS] http:// rejected\n");
}

static void
test_reject_other_schemes(void)
{
    assert(parse_err("ftp://example.com/")  == N00B_HTTP_ERR_UNSUPPORTED_SCHEME);
    assert(parse_err("file:///etc/passwd")  == N00B_HTTP_ERR_UNSUPPORTED_SCHEME);
    assert(parse_err("ws://example.com/")   == N00B_HTTP_ERR_UNSUPPORTED_SCHEME);
    assert(parse_err("javascript:alert(1)") == N00B_HTTP_ERR_UNSUPPORTED_SCHEME);
    assert(parse_err("//example.com/")      == N00B_HTTP_ERR_UNSUPPORTED_SCHEME);
    printf("  [PASS] non-https schemes rejected\n");
}

static void
test_reject_userinfo(void)
{
    assert(parse_err("https://user@example.com/")
           == N00B_HTTP_ERR_USERINFO_REJECTED);
    assert(parse_err("https://user:pass@example.com/")
           == N00B_HTTP_ERR_USERINFO_REJECTED);
    /* `@` in path is fine. */
    n00b_http_url_t *u = parse_ok("https://example.com/foo@bar");
    assert(streq(u->path, "/foo@bar"));
    printf("  [PASS] userinfo rejected; @ in path passes through\n");
}

static void
test_reject_empty_host(void)
{
    assert(parse_err("https:///path")     == N00B_HTTP_ERR_HOST_EMPTY);
    assert(parse_err("https://:8443/")    == N00B_HTTP_ERR_HOST_EMPTY);
    printf("  [PASS] empty host rejected\n");
}

static void
test_reject_bad_ipv6(void)
{
    assert(parse_err("https://[::1/")     == N00B_HTTP_ERR_HOST_INVALID);
    assert(parse_err("https://[bad@chars]/") == N00B_HTTP_ERR_HOST_INVALID);
    /* Empty brackets — not a valid host. */
    assert(parse_err("https://[]/")       == N00B_HTTP_ERR_HOST_EMPTY);
    printf("  [PASS] malformed IPv6 literals rejected\n");
}

static void
test_reject_bad_port(void)
{
    assert(parse_err("https://example.com:0/")
           == N00B_HTTP_ERR_PORT_INVALID);
    assert(parse_err("https://example.com:99999/")
           == N00B_HTTP_ERR_PORT_INVALID);
    assert(parse_err("https://example.com:abc/")
           == N00B_HTTP_ERR_PORT_INVALID);
    assert(parse_err("https://example.com:/")
           == N00B_HTTP_ERR_PORT_INVALID);
    /* Port at exactly 65535 is valid. */
    n00b_http_url_t *u = parse_ok("https://example.com:65535/");
    assert(u->port == 65535);
    printf("  [PASS] invalid ports rejected; 65535 accepted\n");
}

static void
test_null_input(void)
{
    auto r = n00b_http_url_parse(nullptr);
    assert(n00b_result_is_err(r));
    assert((int32_t)n00b_result_get_err(r) == N00B_HTTP_ERR_NULL_ARG);
    printf("  [PASS] nullptr URL → NULL_ARG\n");
}

static void
test_long_path_query(void)
{
    n00b_http_url_t *u = parse_ok(
        "https://example.com:9443/a/b/c?one=1&two=two&three=th%20ee");
    assert(u->port == 9443);
    assert(streq(u->path, "/a/b/c"));
    assert(streq(u->query, "one=1&two=two&three=th%20ee"));
    assert(streq(u->origin, "https://example.com:9443"));
    printf("  [PASS] longer path + query partitioning\n");
}

/* ------------------------------------------------------------------ */
/* DF-X — IDNA / UTS-46 canonicalization of DNS-label hosts at parse  */
/* time.  Post-lift, `parsed_url->host` is always pure ASCII (ACE     */
/* form for non-ASCII labels, lowercased ASCII for ASCII labels,     */
/* IPv4 dotted-quads + IPv6 literals byte-identical to input).       */
/* ------------------------------------------------------------------ */

static void
test_idn_ascii_passthrough(void)
{
    /* Pure-ASCII input → byte-identical to current behavior.  This
     * is the backward-compat invariant: ASCII domains are a fixed
     * point of UTS-46 ToASCII modulo case folding, which we already
     * applied. */
    n00b_http_url_t *u = parse_ok("https://example.com/");
    assert(streq(u->host, "example.com"));
    printf("  [PASS] IDN: ASCII passthrough\n");
}

static void
test_idn_mixed_case_ascii(void)
{
    /* Sanity check: case-folding still applies on the IDNA path. */
    n00b_http_url_t *u = parse_ok("https://EXAMPLE.com/");
    assert(streq(u->host, "example.com"));
    printf("  [PASS] IDN: mixed-case ASCII lowercased\n");
}

static void
test_idn_unicode_hostname(void)
{
    /* Non-ASCII host → ACE / Punycode form.  `例え` is the test
     * fixture used by deferral-B; its Punycode is `xn--r8jz45g`. */
    n00b_http_url_t *u = parse_ok("https://例え.com/");
    assert(streq(u->host, "xn--r8jz45g.com"));
    printf("  [PASS] IDN: Unicode host -> ACE form\n");
}

static void
test_idn_already_ace(void)
{
    /* Idempotent: an already-ACE host parses to itself.  This
     * confirms UTS-46 ToASCII recognizes `xn--…` labels and
     * re-encodes them without re-Punycoding. */
    n00b_http_url_t *u = parse_ok("https://xn--r8jz45g.com/");
    assert(streq(u->host, "xn--r8jz45g.com"));
    printf("  [PASS] IDN: already-ACE input is idempotent\n");
}

static void
test_idn_ipv4_literal_passthrough(void)
{
    /* IPv4 dotted-quad is a fixed point of UTS-46 (each octet is
     * a valid pure-ASCII DNS label).  Host bytes survive
     * byte-identically — no `xn--` mangling. */
    n00b_http_url_t *u = parse_ok("https://192.168.1.1/");
    assert(streq(u->host, "192.168.1.1"));
    assert(!u->is_ipv6_literal);
    printf("  [PASS] IDN: IPv4 dotted-quad bypasses IDNA\n");
}

static void
test_idn_ipv6_literal_bypasses(void)
{
    /* IPv6 literal payload is hex digits + `:` + `.` + `%`; the
     * parser bypasses IDNA for the bracketed form. */
    n00b_http_url_t *u = parse_ok("https://[::1]/");
    assert(u->is_ipv6_literal);
    assert(streq(u->host, "::1"));
    printf("  [PASS] IDN: IPv6 literal bypasses IDNA\n");
}

static void
test_idn_invalid_utf8_rejected(void)
{
    /* Invalid UTF-8 in the host slice → UTS-46 returns
     * `_PROCESSING_ERROR` (DF-Y contract), which the parser maps
     * to `N00B_HTTP_ERR_HOST_INVALID`.  No silent fallback to
     * raw bytes. */
    const char bad[] = {'h', 't', 't', 'p', 's', ':', '/', '/',
                        (char)0xff, (char)0xfe, '.', 'c', 'o', 'm', '/'};
    n00b_string_t *u  = n00b_string_from_raw(bad, (int64_t)sizeof(bad));
    auto           r  = n00b_http_url_parse(u);
    assert(n00b_result_is_err(r));
    assert((int32_t)n00b_result_get_err(r) == N00B_HTTP_ERR_HOST_INVALID);
    printf("  [PASS] IDN: invalid UTF-8 host -> HOST_INVALID\n");
}

static void
test_err_str_round_trip(void)
{
    /* Every defined enum value yields a non-empty stable string. */
    static const n00b_http_err_t codes[] = {
        N00B_HTTP_OK,
        N00B_HTTP_ERR_NULL_ARG,
        N00B_HTTP_ERR_INVALID_URL,
        N00B_HTTP_ERR_UNSUPPORTED_SCHEME,
        N00B_HTTP_ERR_USERINFO_REJECTED,
        N00B_HTTP_ERR_HOST_EMPTY,
        N00B_HTTP_ERR_HOST_INVALID,
        N00B_HTTP_ERR_PORT_INVALID,
        N00B_HTTP_ERR_BAD_RESPONSE,
    };
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        const char *s = n00b_http_err_str(codes[i]);
        assert(s && s[0] != '\0');
    }
    /* Unknown codes get a stable fallback. */
    const char *u = n00b_http_err_str((n00b_http_err_t)-99);
    assert(u && strcmp(u, "unknown error") == 0);
    printf("  [PASS] n00b_http_err_str covers every enum value\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_http_url:\n");
    test_basic_https();
    test_default_path();
    test_explicit_port();
    test_explicit_443_omitted_from_origin();
    test_query();
    test_query_only();
    test_fragment_stripped();
    test_ipv6_literal();
    test_ipv6_default_port();
    test_ipv6_explicit_443_omitted();
    test_case_insensitive_scheme();
    test_reject_http();
    test_reject_other_schemes();
    test_reject_userinfo();
    test_reject_empty_host();
    test_reject_bad_ipv6();
    test_reject_bad_port();
    test_null_input();
    test_long_path_query();
    test_idn_ascii_passthrough();
    test_idn_mixed_case_ascii();
    test_idn_unicode_hostname();
    test_idn_already_ace();
    test_idn_ipv4_literal_passthrough();
    test_idn_ipv6_literal_bypasses();
    test_idn_invalid_utf8_rejected();
    test_err_str_round_trip();
    printf("All test_http_url tests passed.\n");

    n00b_shutdown();
    return 0;
}
