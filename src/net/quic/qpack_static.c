/*
 * qpack_static.c — RFC 9204 Appendix A static table.
 *
 * Indices 0..98.  Order is fixed by the RFC; do not reorder.  Each
 * entry's name is lowercase ASCII; values are exactly as listed.
 *
 * The lengths are computed at compile time via sizeof(literal) - 1,
 * so a typo in a value shows up as a length mismatch in tests.
 */
#include "internal/net/quic/qpack_internal.h"

#define E(N, V) { (N), sizeof((N)) - 1, (V), sizeof((V)) - 1 }

const n00b_qpack_static_entry_t
n00b_qpack_static_table[N00B_QPACK_STATIC_TABLE_SIZE] = {
    /*  0 */ E(":authority",                   ""),
    /*  1 */ E(":path",                        "/"),
    /*  2 */ E("age",                          "0"),
    /*  3 */ E("content-disposition",          ""),
    /*  4 */ E("content-length",               "0"),
    /*  5 */ E("cookie",                       ""),
    /*  6 */ E("date",                         ""),
    /*  7 */ E("etag",                         ""),
    /*  8 */ E("if-modified-since",            ""),
    /*  9 */ E("if-none-match",                ""),
    /* 10 */ E("last-modified",                ""),
    /* 11 */ E("link",                         ""),
    /* 12 */ E("location",                     ""),
    /* 13 */ E("referer",                      ""),
    /* 14 */ E("set-cookie",                   ""),
    /* 15 */ E(":method",                      "CONNECT"),
    /* 16 */ E(":method",                      "DELETE"),
    /* 17 */ E(":method",                      "GET"),
    /* 18 */ E(":method",                      "HEAD"),
    /* 19 */ E(":method",                      "OPTIONS"),
    /* 20 */ E(":method",                      "POST"),
    /* 21 */ E(":method",                      "PUT"),
    /* 22 */ E(":scheme",                      "http"),
    /* 23 */ E(":scheme",                      "https"),
    /* 24 */ E(":status",                      "103"),
    /* 25 */ E(":status",                      "200"),
    /* 26 */ E(":status",                      "304"),
    /* 27 */ E(":status",                      "404"),
    /* 28 */ E(":status",                      "503"),
    /* 29 */ E("accept",                       "*/*"),
    /* 30 */ E("accept",                       "application/dns-message"),
    /* 31 */ E("accept-encoding",              "gzip, deflate, br"),
    /* 32 */ E("accept-ranges",                "bytes"),
    /* 33 */ E("access-control-allow-headers", "cache-control"),
    /* 34 */ E("access-control-allow-headers", "content-type"),
    /* 35 */ E("access-control-allow-origin",  "*"),
    /* 36 */ E("cache-control",                "max-age=0"),
    /* 37 */ E("cache-control",                "max-age=2592000"),
    /* 38 */ E("cache-control",                "max-age=604800"),
    /* 39 */ E("cache-control",                "no-cache"),
    /* 40 */ E("cache-control",                "no-store"),
    /* 41 */ E("cache-control",                "public, max-age=31536000"),
    /* 42 */ E("content-encoding",             "br"),
    /* 43 */ E("content-encoding",             "gzip"),
    /* 44 */ E("content-type",                 "application/dns-message"),
    /* 45 */ E("content-type",                 "application/javascript"),
    /* 46 */ E("content-type",                 "application/json"),
    /* 47 */ E("content-type",                 "application/x-www-form-urlencoded"),
    /* 48 */ E("content-type",                 "image/gif"),
    /* 49 */ E("content-type",                 "image/jpeg"),
    /* 50 */ E("content-type",                 "image/png"),
    /* 51 */ E("content-type",                 "text/css"),
    /* 52 */ E("content-type",                 "text/html; charset=utf-8"),
    /* 53 */ E("content-type",                 "text/plain"),
    /* 54 */ E("content-type",                 "text/plain;charset=utf-8"),
    /* 55 */ E("range",                        "bytes=0-"),
    /* 56 */ E("strict-transport-security",    "max-age=31536000"),
    /* 57 */ E("strict-transport-security",    "max-age=31536000; includesubdomains"),
    /* 58 */ E("strict-transport-security",    "max-age=31536000; includesubdomains; preload"),
    /* 59 */ E("vary",                         "accept-encoding"),
    /* 60 */ E("vary",                         "origin"),
    /* 61 */ E("x-content-type-options",       "nosniff"),
    /* 62 */ E("x-xss-protection",             "1; mode=block"),
    /* 63 */ E(":status",                      "100"),
    /* 64 */ E(":status",                      "204"),
    /* 65 */ E(":status",                      "206"),
    /* 66 */ E(":status",                      "302"),
    /* 67 */ E(":status",                      "400"),
    /* 68 */ E(":status",                      "403"),
    /* 69 */ E(":status",                      "421"),
    /* 70 */ E(":status",                      "425"),
    /* 71 */ E(":status",                      "500"),
    /* 72 */ E("accept-language",              ""),
    /* 73 */ E("access-control-allow-credentials", "FALSE"),
    /* 74 */ E("access-control-allow-credentials", "TRUE"),
    /* 75 */ E("access-control-allow-headers", "*"),
    /* 76 */ E("access-control-allow-methods", "get"),
    /* 77 */ E("access-control-allow-methods", "get, post, options"),
    /* 78 */ E("access-control-allow-methods", "options"),
    /* 79 */ E("access-control-expose-headers", "content-length"),
    /* 80 */ E("access-control-request-headers", "content-type"),
    /* 81 */ E("access-control-request-method", "get"),
    /* 82 */ E("access-control-request-method", "post"),
    /* 83 */ E("alt-svc",                      "clear"),
    /* 84 */ E("authorization",                ""),
    /* 85 */ E("content-security-policy",
             "script-src 'none'; object-src 'none'; base-uri 'none'"),
    /* 86 */ E("early-data",                   "1"),
    /* 87 */ E("expect-ct",                    ""),
    /* 88 */ E("forwarded",                    ""),
    /* 89 */ E("if-range",                     ""),
    /* 90 */ E("origin",                       ""),
    /* 91 */ E("purpose",                      "prefetch"),
    /* 92 */ E("server",                       ""),
    /* 93 */ E("timing-allow-origin",          "*"),
    /* 94 */ E("upgrade-insecure-requests",    "1"),
    /* 95 */ E("user-agent",                   ""),
    /* 96 */ E("x-forwarded-for",              ""),
    /* 97 */ E("x-frame-options",              "deny"),
    /* 98 */ E("x-frame-options",              "sameorigin"),
};

#undef E

bool
n00b_qpack_static_lookup(size_t idx, n00b_qpack_field_t *out)
{
    if (idx >= N00B_QPACK_STATIC_TABLE_SIZE || !out) {
        return false;
    }
    const n00b_qpack_static_entry_t *e = &n00b_qpack_static_table[idx];
    out->name      = (const uint8_t *)e->name;
    out->name_len  = e->name_len;
    out->value     = (const uint8_t *)e->value;
    out->value_len = e->value_len;
    return true;
}
