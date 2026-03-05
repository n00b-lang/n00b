/**
 * @file nfs_xdr.h
 * @brief Minimal XDR (RFC 4506) encoder/decoder for NFSv3.
 *
 * Only implements the types needed by the NFSv3 subset:
 * uint32, uint64, opaque (fixed + variable), string, bool.
 *
 * All functions operate on a cursor into a flat buffer.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// ============================================================================
// XDR cursor
// ============================================================================

typedef struct {
    uint8_t *buf;
    uint32_t pos;
    uint32_t len;   /**< Total buffer size. */
} n00b_xdr_t;

static inline void
n00b_xdr_init(n00b_xdr_t *x, uint8_t *buf, uint32_t len)
{
    x->buf = buf;
    x->pos = 0;
    x->len = len;
}

static inline bool
n00b_xdr_ok(n00b_xdr_t *x, uint32_t need)
{
    return (x->pos + need) <= x->len;
}

// ============================================================================
// Encode (write)
// ============================================================================

static inline bool
n00b_xdr_put_u32(n00b_xdr_t *x, uint32_t v)
{
    if (!n00b_xdr_ok(x, 4)) return false;
    x->buf[x->pos++] = (uint8_t)(v >> 24);
    x->buf[x->pos++] = (uint8_t)(v >> 16);
    x->buf[x->pos++] = (uint8_t)(v >> 8);
    x->buf[x->pos++] = (uint8_t)(v);
    return true;
}

static inline bool
n00b_xdr_put_u64(n00b_xdr_t *x, uint64_t v)
{
    return n00b_xdr_put_u32(x, (uint32_t)(v >> 32))
        && n00b_xdr_put_u32(x, (uint32_t)(v & 0xFFFFFFFF));
}

static inline bool
n00b_xdr_put_bool(n00b_xdr_t *x, bool v)
{
    return n00b_xdr_put_u32(x, v ? 1 : 0);
}

/** @brief Encode variable-length opaque (length + data + padding). */
static inline bool
n00b_xdr_put_opaque(n00b_xdr_t *x, const void *data, uint32_t len)
{
    if (!n00b_xdr_put_u32(x, len)) return false;
    uint32_t padded = (len + 3) & ~3u;
    if (!n00b_xdr_ok(x, padded)) return false;
    memcpy(x->buf + x->pos, data, len);
    // Zero padding bytes.
    for (uint32_t i = len; i < padded; i++) {
        x->buf[x->pos + i] = 0;
    }
    x->pos += padded;
    return true;
}

/** @brief Encode fixed-length opaque (data + padding, no length prefix). */
static inline bool
n00b_xdr_put_fixed(n00b_xdr_t *x, const void *data, uint32_t len)
{
    uint32_t padded = (len + 3) & ~3u;
    if (!n00b_xdr_ok(x, padded)) return false;
    memcpy(x->buf + x->pos, data, len);
    for (uint32_t i = len; i < padded; i++) {
        x->buf[x->pos + i] = 0;
    }
    x->pos += padded;
    return true;
}

static inline bool
n00b_xdr_put_string(n00b_xdr_t *x, const char *s, uint32_t len)
{
    return n00b_xdr_put_opaque(x, s, len);
}

// ============================================================================
// Decode (read)
// ============================================================================

static inline bool
n00b_xdr_get_u32(n00b_xdr_t *x, uint32_t *out)
{
    if (!n00b_xdr_ok(x, 4)) return false;
    *out = ((uint32_t)x->buf[x->pos] << 24)
         | ((uint32_t)x->buf[x->pos + 1] << 16)
         | ((uint32_t)x->buf[x->pos + 2] << 8)
         | ((uint32_t)x->buf[x->pos + 3]);
    x->pos += 4;
    return true;
}

static inline bool
n00b_xdr_get_u64(n00b_xdr_t *x, uint64_t *out)
{
    uint32_t hi, lo;
    if (!n00b_xdr_get_u32(x, &hi)) return false;
    if (!n00b_xdr_get_u32(x, &lo)) return false;
    *out = ((uint64_t)hi << 32) | lo;
    return true;
}

static inline bool
n00b_xdr_get_bool(n00b_xdr_t *x, bool *out)
{
    uint32_t v;
    if (!n00b_xdr_get_u32(x, &v)) return false;
    *out = (v != 0);
    return true;
}

/** @brief Decode variable-length opaque.  Returns pointer into buffer (no copy). */
static inline bool
n00b_xdr_get_opaque(n00b_xdr_t *x, const uint8_t **out, uint32_t *out_len)
{
    uint32_t len;
    if (!n00b_xdr_get_u32(x, &len)) return false;
    // Guard against overflow: len + 3 could wrap for large len values.
    if (len > x->len || len > x->len - x->pos) return false;
    uint32_t padded = (len + 3) & ~3u;
    if (!n00b_xdr_ok(x, padded)) return false;
    *out     = x->buf + x->pos;
    *out_len = len;
    x->pos  += padded;
    return true;
}

static inline bool
n00b_xdr_get_fixed(n00b_xdr_t *x, void *out, uint32_t len)
{
    uint32_t padded = (len + 3) & ~3u;
    if (!n00b_xdr_ok(x, padded)) return false;
    memcpy(out, x->buf + x->pos, len);
    x->pos += padded;
    return true;
}
