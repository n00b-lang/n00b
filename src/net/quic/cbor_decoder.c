/*
 * cbor_decoder.c — RFC 8949 CBOR decoder.
 *
 * Phase 4 § 4.1.  State-machine pull decoder; returns a tagged-union
 * AST (`n00b_cbor_value_t`).  Hardened against malicious input:
 *
 *   - Hard depth cap (`N00B_CBOR_MAX_DEPTH`, default 32).
 *   - Hard total-bytes cap (`N00B_CBOR_MAX_INPUT_BYTES`, default 16
 *     MiB).
 *   - Pre-validates length headers against remaining bytes before
 *     allocating containers (no "1 GiB array of zero bytes" attack).
 *   - Refuses indefinite-length items — RPC bodies don't need them
 *     and they're a known fuzz hot-spot.
 *   - Refuses trailing bytes after the top-level item.
 *
 * Conduit-pool allocation throughout.  AST is GC-friendly: no
 * per-node free is required; the whole tree dies with the pool.
 */

#define N00B_USE_INTERNAL_API
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "core/string.h"
#include "adt/result.h"
#include "net/quic/quic_types.h"
#include "net/quic/cbor.h"
#include "internal/net/quic/cbor_internal.h"

/* ===========================================================================
 * Forward declarations
 * =========================================================================== */

static n00b_err_t decode_value(n00b_cbor_decoder_t *d,
                               n00b_cbor_value_t  **out);

/* ===========================================================================
 * Allocation helpers (conduit pool)
 * =========================================================================== */

static n00b_cbor_value_t *
alloc_value(n00b_cbor_decoder_t *d)
{
    return (n00b_cbor_value_t *)n00b_alloc_with_opts(
        n00b_cbor_value_t,
        &(n00b_alloc_opts_t){
            .allocator = d->alloc,
        });
}

static void *
alloc_bytes(n00b_cbor_decoder_t *d, size_t n)
{
    return n00b_alloc_array_with_opts(uint8_t, (int64_t)n,
                                      &(n00b_alloc_opts_t){
                                          .allocator = d->alloc,
                                          .no_scan   = true,
                                      });
}

/* Allocates an array of cbor-value-pointers, kept scannable so the
 * GC walks the children. */
static n00b_cbor_value_t **
alloc_value_array(n00b_cbor_decoder_t *d, size_t n)
{
    return n00b_alloc_array_with_opts(n00b_cbor_value_t *, (int64_t)n,
                                      &(n00b_alloc_opts_t){
                                          .allocator = d->alloc,
                                      });
}

static n00b_cbor_pair_t *
alloc_pair_array(n00b_cbor_decoder_t *d, size_t n)
{
    return n00b_alloc_array_with_opts(n00b_cbor_pair_t, (int64_t)n,
                                      &(n00b_alloc_opts_t){
                                          .allocator = d->alloc,
                                      });
}

/* ===========================================================================
 * Cursor mechanics
 * =========================================================================== */

static inline bool
remaining(n00b_cbor_decoder_t *d, size_t n)
{
    return d->len - d->pos >= n;
}

static inline uint8_t
read_u8(n00b_cbor_decoder_t *d)
{
    return d->data[d->pos++];
}

/* Read a big-endian uint of `bytes` width, advancing the cursor.
 * Caller must verify availability beforehand. */
static uint64_t
read_be(n00b_cbor_decoder_t *d, int bytes)
{
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++) {
        v = (v << 8) | (uint64_t)d->data[d->pos++];
    }
    return v;
}

/* ===========================================================================
 * Head decode — returns major type + raw "argument" value
 *
 * The argument is the magnitude (for ints), the byte/element count
 * (for strings/arrays/maps), the tag id, or the simple/float
 * encoding for primitives.  Returns one of:
 *
 *   N00B_QUIC_OK              ok
 *   N00B_QUIC_ERR_NEED_MORE_DATA  truncated mid-head
 *   N00B_QUIC_ERR_PROTOCOL    malformed (indefinite, reserved AI)
 * =========================================================================== */

static n00b_err_t
read_head(n00b_cbor_decoder_t *d,
          uint8_t             *mt_out,
          uint8_t             *ai_out,
          uint64_t            *arg_out)
{
    if (!remaining(d, 1)) {
        return N00B_QUIC_ERR_NEED_MORE_DATA;
    }
    uint8_t b  = read_u8(d);
    uint8_t mt = (uint8_t)(b >> 5);
    uint8_t ai = (uint8_t)(b & 0x1f);
    uint64_t arg = 0;

    if (ai < 24) {
        arg = ai;
    } else if (ai == 24) {
        if (!remaining(d, 1)) return N00B_QUIC_ERR_NEED_MORE_DATA;
        arg = read_be(d, 1);
    } else if (ai == 25) {
        if (!remaining(d, 2)) return N00B_QUIC_ERR_NEED_MORE_DATA;
        arg = read_be(d, 2);
    } else if (ai == 26) {
        if (!remaining(d, 4)) return N00B_QUIC_ERR_NEED_MORE_DATA;
        arg = read_be(d, 4);
    } else if (ai == 27) {
        if (!remaining(d, 8)) return N00B_QUIC_ERR_NEED_MORE_DATA;
        arg = read_be(d, 8);
    } else if (ai == 28 || ai == 29 || ai == 30) {
        /* Reserved (RFC 8949 § 3) — refuse. */
        return N00B_QUIC_ERR_PROTOCOL;
    } else /* ai == 31 */ {
        /* Indefinite length / break stop code — refused on purpose. */
        return N00B_QUIC_ERR_PROTOCOL;
    }

    *mt_out  = mt;
    *ai_out  = ai;
    *arg_out = arg;
    return N00B_QUIC_OK;
}

/* ===========================================================================
 * Primitive decoders for the ai>=20 (major-7) family
 * =========================================================================== */

/* Convert IEEE 754 binary16 bits → double (RFC 8949 § Appendix D).
 *
 * The standard formulae; we replicate them here so we don't depend
 * on a half-float library (n00b doesn't ship one yet). */
static double
half_to_double(uint16_t bits)
{
    uint32_t sign = (uint32_t)(bits >> 15);
    uint32_t exp  = (uint32_t)((bits >> 10) & 0x1f);
    uint32_t frac = (uint32_t)(bits & 0x3ff);
    double   mag;

    if (exp == 0) {
        mag = ldexp((double)frac, -24);
    } else if (exp != 31) {
        mag = ldexp((double)(frac + 1024), (int)exp - 25);
    } else {
        /* Inf or NaN.  Use the standard punning. */
        uint32_t f32_bits = (sign << 31)
                          | (uint32_t)(0xffu << 23)
                          | (frac << 13);
        float f;
        memcpy(&f, &f32_bits, sizeof(f));
        return (double)f;
    }
    return sign ? -mag : mag;
}

/* Decode a major-type-7 head (ai already consumed via read_head). */
static n00b_err_t
decode_primitive(n00b_cbor_decoder_t *d,
                 uint8_t              ai,
                 uint64_t             arg,
                 n00b_cbor_value_t   *out)
{
    switch (ai) {
    case N00B_CBOR_AI_FALSE:
        out->kind        = N00B_CBOR_VT_BOOL;
        out->u.boolean   = false;
        return N00B_QUIC_OK;
    case N00B_CBOR_AI_TRUE:
        out->kind        = N00B_CBOR_VT_BOOL;
        out->u.boolean   = true;
        return N00B_QUIC_OK;
    case N00B_CBOR_AI_NULL:
        out->kind        = N00B_CBOR_VT_NULL;
        return N00B_QUIC_OK;
    case N00B_CBOR_AI_UNDEF:
        out->kind        = N00B_CBOR_VT_UNDEFINED;
        return N00B_QUIC_OK;
    case N00B_CBOR_AI_SIMPLE1:
        /* simple value in the next byte; arg already holds it */
        if (arg < 32 || arg > 255) {
            return N00B_QUIC_ERR_PROTOCOL;
        }
        out->kind     = N00B_CBOR_VT_SIMPLE;
        out->u.simple = (uint8_t)arg;
        return N00B_QUIC_OK;
    case N00B_CBOR_AI_FLOAT16: {
        out->kind       = N00B_CBOR_VT_FLOAT16;
        out->u.f16_bits = (uint16_t)arg;
        return N00B_QUIC_OK;
    }
    case N00B_CBOR_AI_FLOAT32: {
        uint32_t bits = (uint32_t)arg;
        float    f;
        memcpy(&f, &bits, sizeof(f));
        out->kind       = N00B_CBOR_VT_FLOAT32;
        out->u.f32      = f;
        return N00B_QUIC_OK;
    }
    case N00B_CBOR_AI_FLOAT64: {
        uint64_t bits = arg;
        double   f;
        memcpy(&f, &bits, sizeof(f));
        out->kind = N00B_CBOR_VT_DOUBLE;
        out->u.f64 = f;
        return N00B_QUIC_OK;
    }
    default:
        if (ai < 20) {
            /* Unassigned simple values (ai 0-19 inside major 7).
             * RFC 8949 § 3.3 reserves these. */
            out->kind     = N00B_CBOR_VT_SIMPLE;
            out->u.simple = (uint8_t)arg;
            return N00B_QUIC_OK;
        }
        return N00B_QUIC_ERR_PROTOCOL;
    }
}

/* ===========================================================================
 * Strings (text + bytes)
 * =========================================================================== */

static n00b_err_t
decode_byte_string(n00b_cbor_decoder_t *d,
                   uint64_t             arg,
                   n00b_cbor_value_t   *out)
{
    if (arg > (uint64_t)(d->len - d->pos)) {
        /* Either truncated or pathological.  We can't tell which
         * without further context; if the document was advertised
         * to be exactly `d->len` bytes, this is malformed. */
        return N00B_QUIC_ERR_NEED_MORE_DATA;
    }
    n00b_buffer_t *buf;
    if (arg == 0) {
        buf = n00b_buffer_empty(.allocator = d->alloc);
    } else {
        uint8_t *mem = alloc_bytes(d, (size_t)arg);
        memcpy(mem, d->data + d->pos, (size_t)arg);
        buf = n00b_buffer_from_bytes((char *)mem, (int64_t)arg,
                                     .allocator = d->alloc);
    }
    d->pos += (size_t)arg;
    out->kind    = N00B_CBOR_VT_BYTES;
    out->u.bytes = buf;
    return N00B_QUIC_OK;
}

static n00b_err_t
decode_text_string(n00b_cbor_decoder_t *d,
                   uint64_t             arg,
                   n00b_cbor_value_t   *out)
{
    if (arg > (uint64_t)(d->len - d->pos)) {
        return N00B_QUIC_ERR_NEED_MORE_DATA;
    }
    /* RFC 8949 § 3.1 requires UTF-8.  We do a minimal structural
     * scan: each byte must form a legal lead/continuation pattern.
     * Full normalization / surrogate filtering is not done here —
     * the wire format doesn't constrain us to any specific
     * normalization form. */
    const uint8_t *p   = d->data + d->pos;
    size_t         rem = (size_t)arg;
    while (rem > 0) {
        uint8_t b = *p++;
        rem--;
        size_t cont;
        if (b < 0x80) {
            cont = 0;
        } else if ((b & 0xe0) == 0xc0) {
            cont = 1;
        } else if ((b & 0xf0) == 0xe0) {
            cont = 2;
        } else if ((b & 0xf8) == 0xf0) {
            cont = 3;
        } else {
            return N00B_QUIC_ERR_PROTOCOL;
        }
        if (cont > rem) {
            return N00B_QUIC_ERR_PROTOCOL;
        }
        for (size_t i = 0; i < cont; i++) {
            if ((p[i] & 0xc0) != 0x80) {
                return N00B_QUIC_ERR_PROTOCOL;
            }
        }
        p   += cont;
        rem -= cont;
    }

    n00b_string_t *s = n00b_string_from_raw((const char *)(d->data + d->pos),
                                            (int64_t)arg,
                                            .allocator = d->alloc);
    d->pos += (size_t)arg;
    out->kind     = N00B_CBOR_VT_STRING;
    out->u.string = s;
    return N00B_QUIC_OK;
}

/* ===========================================================================
 * Containers (array + map)
 * =========================================================================== */

static n00b_err_t
decode_array(n00b_cbor_decoder_t *d,
             uint64_t             count,
             n00b_cbor_value_t   *out)
{
    /* Pre-flight: each item is at least 1 byte; if count > remaining
     * the doc is structurally impossible. */
    if (count > (uint64_t)(d->len - d->pos)) {
        return N00B_QUIC_ERR_PROTOCOL;
    }
    out->kind         = N00B_CBOR_VT_ARRAY;
    out->u.array.count = (size_t)count;
    out->u.array.items = count == 0 ? nullptr
                                    : alloc_value_array(d, (size_t)count);

    for (uint64_t i = 0; i < count; i++) {
        n00b_cbor_value_t *child = nullptr;
        n00b_err_t e = decode_value(d, &child);
        if (e != N00B_QUIC_OK) {
            return e;
        }
        out->u.array.items[i] = child;
    }
    return N00B_QUIC_OK;
}

static n00b_err_t
decode_map(n00b_cbor_decoder_t *d,
           uint64_t             count,
           n00b_cbor_value_t   *out)
{
    /* Each pair is at least 2 bytes (one for key head, one for
     * value head).  Catch the obvious overflow. */
    if (count > (uint64_t)(d->len - d->pos) / 2 + 1) {
        return N00B_QUIC_ERR_PROTOCOL;
    }
    out->kind        = N00B_CBOR_VT_MAP;
    out->u.map.count = (size_t)count;
    out->u.map.pairs = count == 0 ? nullptr
                                  : alloc_pair_array(d, (size_t)count);

    for (uint64_t i = 0; i < count; i++) {
        n00b_cbor_value_t *k = nullptr;
        n00b_cbor_value_t *v = nullptr;
        n00b_err_t e = decode_value(d, &k);
        if (e != N00B_QUIC_OK) return e;
        e = decode_value(d, &v);
        if (e != N00B_QUIC_OK) return e;
        out->u.map.pairs[i].key = k;
        out->u.map.pairs[i].val = v;
    }
    return N00B_QUIC_OK;
}

static n00b_err_t
decode_tag(n00b_cbor_decoder_t *d,
           uint64_t             tag,
           n00b_cbor_value_t   *out)
{
    out->kind      = N00B_CBOR_VT_TAG;
    out->u.tag.tag = tag;
    return decode_value(d, &out->u.tag.inner);
}

/* ===========================================================================
 * Single-item dispatch
 * =========================================================================== */

static n00b_err_t
decode_value(n00b_cbor_decoder_t *d, n00b_cbor_value_t **out)
{
    if (++d->depth > N00B_CBOR_MAX_DEPTH) {
        return N00B_QUIC_ERR_PROTOCOL;
    }

    uint8_t  mt, ai;
    uint64_t arg;
    n00b_err_t e = read_head(d, &mt, &ai, &arg);
    if (e != N00B_QUIC_OK) {
        return e;
    }

    n00b_cbor_value_t *v = alloc_value(d);

    switch (mt) {
    case N00B_CBOR_MT_UINT:
        v->kind     = N00B_CBOR_VT_UINT;
        v->u.uint   = arg;
        break;
    case N00B_CBOR_MT_NEGINT:
        v->kind   = N00B_CBOR_VT_NEGINT;
        v->u.uint = arg;     /* magnitude; canonical value = -1 - arg */
        break;
    case N00B_CBOR_MT_BYTES:
        e = decode_byte_string(d, arg, v);
        break;
    case N00B_CBOR_MT_TEXT:
        e = decode_text_string(d, arg, v);
        break;
    case N00B_CBOR_MT_ARRAY:
        e = decode_array(d, arg, v);
        break;
    case N00B_CBOR_MT_MAP:
        e = decode_map(d, arg, v);
        break;
    case N00B_CBOR_MT_TAG:
        e = decode_tag(d, arg, v);
        break;
    case N00B_CBOR_MT_PRIMITIVE:
        e = decode_primitive(d, ai, arg, v);
        break;
    default:
        e = N00B_QUIC_ERR_PROTOCOL;
        break;
    }

    if (e != N00B_QUIC_OK) {
        return e;
    }

    --d->depth;
    *out = v;
    return N00B_QUIC_OK;
}

/* ===========================================================================
 * Public entry points
 * =========================================================================== */

n00b_result_t(n00b_cbor_value_t *)
n00b_cbor_decode_bytes(const uint8_t *data, size_t len)
{
    if (data == nullptr && len != 0) {
        return n00b_result_err(n00b_cbor_value_t *,
                               N00B_QUIC_ERR_NULL_ARG);
    }
    if (len == 0) {
        return n00b_result_err(n00b_cbor_value_t *,
                               N00B_QUIC_ERR_NEED_MORE_DATA);
    }
    if (len > N00B_CBOR_MAX_INPUT_BYTES) {
        return n00b_result_err(n00b_cbor_value_t *,
                               N00B_QUIC_ERR_FRAME_TOO_LARGE);
    }

    n00b_cbor_decoder_t d = {
        .data  = data,
        .len   = len,
        .pos   = 0,
        .depth = 0,
        .alloc = n00b_cbor_alloc(),
    };

    n00b_cbor_value_t *root = nullptr;
    n00b_err_t e = decode_value(&d, &root);
    if (e != N00B_QUIC_OK) {
        return n00b_result_err(n00b_cbor_value_t *, e);
    }
    if (d.pos != d.len) {
        return n00b_result_err(n00b_cbor_value_t *,
                               N00B_QUIC_ERR_PROTOCOL);
    }
    return n00b_result_ok(n00b_cbor_value_t *, root);
}

n00b_result_t(n00b_cbor_value_t *)
n00b_cbor_decode(n00b_buffer_t *input)
{
    if (input == nullptr) {
        return n00b_result_err(n00b_cbor_value_t *,
                               N00B_QUIC_ERR_NULL_ARG);
    }
    return n00b_cbor_decode_bytes((const uint8_t *)input->data,
                                  (size_t)input->byte_len);
}

n00b_result_t(n00b_cbor_value_t *)
n00b_cbor_decode_first_bytes(const uint8_t *data,
                             size_t         len,
                             size_t        *consumed)
{
    if (consumed == nullptr) {
        return n00b_result_err(n00b_cbor_value_t *,
                               N00B_QUIC_ERR_NULL_ARG);
    }
    *consumed = 0;
    if (data == nullptr && len != 0) {
        return n00b_result_err(n00b_cbor_value_t *,
                               N00B_QUIC_ERR_NULL_ARG);
    }
    if (len == 0) {
        return n00b_result_err(n00b_cbor_value_t *,
                               N00B_QUIC_ERR_NEED_MORE_DATA);
    }
    if (len > N00B_CBOR_MAX_INPUT_BYTES) {
        return n00b_result_err(n00b_cbor_value_t *,
                               N00B_QUIC_ERR_FRAME_TOO_LARGE);
    }

    n00b_cbor_decoder_t d = {
        .data  = data,
        .len   = len,
        .pos   = 0,
        .depth = 0,
        .alloc = n00b_cbor_alloc(),
    };

    n00b_cbor_value_t *root = nullptr;
    n00b_err_t e = decode_value(&d, &root);
    if (e != N00B_QUIC_OK) {
        return n00b_result_err(n00b_cbor_value_t *, e);
    }
    *consumed = d.pos;
    return n00b_result_ok(n00b_cbor_value_t *, root);
}

/* ===========================================================================
 * Typed extractors
 * =========================================================================== */

n00b_result_t(int64_t)
n00b_cbor_value_to_int64(n00b_cbor_value_t *v)
{
    if (!v) return n00b_result_err(int64_t, N00B_QUIC_ERR_NULL_ARG);
    switch (v->kind) {
    case N00B_CBOR_VT_UINT:
        if (v->u.uint > (uint64_t)INT64_MAX) {
            return n00b_result_err(int64_t, N00B_QUIC_ERR_PROTOCOL);
        }
        return n00b_result_ok(int64_t, (int64_t)v->u.uint);
    case N00B_CBOR_VT_NEGINT:
        /* Canonical value = -1 - magnitude.  Representable iff
         * magnitude <= INT64_MAX (which yields -1-INT64_MAX = INT64_MIN). */
        if (v->u.uint > (uint64_t)INT64_MAX) {
            return n00b_result_err(int64_t, N00B_QUIC_ERR_PROTOCOL);
        }
        return n00b_result_ok(int64_t, -1 - (int64_t)v->u.uint);
    case N00B_CBOR_VT_INT64:
        return n00b_result_ok(int64_t, v->u.int64);
    default:
        return n00b_result_err(int64_t, N00B_QUIC_ERR_BAD_TYPE);
    }
}

n00b_result_t(bool)
n00b_cbor_value_to_bool(n00b_cbor_value_t *v)
{
    if (!v) return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    if (v->kind != N00B_CBOR_VT_BOOL) {
        return n00b_result_err(bool, N00B_QUIC_ERR_BAD_TYPE);
    }
    return n00b_result_ok(bool, v->u.boolean);
}

n00b_result_t(double)
n00b_cbor_value_to_double(n00b_cbor_value_t *v)
{
    if (!v) return n00b_result_err(double, N00B_QUIC_ERR_NULL_ARG);
    switch (v->kind) {
    case N00B_CBOR_VT_DOUBLE:
        return n00b_result_ok(double, v->u.f64);
    case N00B_CBOR_VT_FLOAT32:
        return n00b_result_ok(double, (double)v->u.f32);
    case N00B_CBOR_VT_FLOAT16: {
        /* Use the half-to-double helper (declared in this TU). */
        extern double n00b_cbor_half_to_double_(uint16_t);
        return n00b_result_ok(double, n00b_cbor_half_to_double_(v->u.f16_bits));
    }
    default:
        return n00b_result_err(double, N00B_QUIC_ERR_BAD_TYPE);
    }
}

/* Public-from-this-TU helper for the float16 conversion (avoids
 * exposing it as a header function while still letting the typed
 * extractor reach it).  Internally just calls half_to_double. */
double
n00b_cbor_half_to_double_(uint16_t bits)
{
    return half_to_double(bits);
}

n00b_result_t(n00b_string_t *)
n00b_cbor_value_to_string(n00b_cbor_value_t *v)
{
    if (!v) return n00b_result_err(n00b_string_t *, N00B_QUIC_ERR_NULL_ARG);
    if (v->kind != N00B_CBOR_VT_STRING) {
        return n00b_result_err(n00b_string_t *, N00B_QUIC_ERR_BAD_TYPE);
    }
    return n00b_result_ok(n00b_string_t *, v->u.string);
}

n00b_result_t(n00b_buffer_t *)
n00b_cbor_value_to_buffer(n00b_cbor_value_t *v)
{
    if (!v) return n00b_result_err(n00b_buffer_t *, N00B_QUIC_ERR_NULL_ARG);
    if (v->kind != N00B_CBOR_VT_BYTES) {
        return n00b_result_err(n00b_buffer_t *, N00B_QUIC_ERR_BAD_TYPE);
    }
    return n00b_result_ok(n00b_buffer_t *, v->u.bytes);
}

/* ===========================================================================
 * decode_to(T, buf) helpers
 * =========================================================================== */

#define DEFINE_DECODE_TO(suffix, T, extractor)                                     \
    n00b_result_t(T) _n00b_cbor_decode_to_##suffix(n00b_buffer_t *buf)             \
    {                                                                              \
        auto r = n00b_cbor_decode(buf);                                            \
        if (n00b_result_is_err(r)) {                                               \
            return n00b_result_err(T, n00b_result_get_err(r));                     \
        }                                                                          \
        return extractor(n00b_result_get(r));                                      \
    }

DEFINE_DECODE_TO(int64,  int64_t,         n00b_cbor_value_to_int64)
DEFINE_DECODE_TO(bool,   bool,            n00b_cbor_value_to_bool)
DEFINE_DECODE_TO(double, double,          n00b_cbor_value_to_double)
DEFINE_DECODE_TO(string, n00b_string_t *, n00b_cbor_value_to_string)
DEFINE_DECODE_TO(buffer, n00b_buffer_t *, n00b_cbor_value_to_buffer)

/* ===========================================================================
 * Strict-mode decoder (Phase 4 § 4.7)
 *
 * The standard `n00b_cbor_decode` already enforces depth, total-bytes
 * cap, refuses indefinite-length items at any nesting level (via the
 * `ai == 31` check in `read_head`), and rejects reserved AI nibbles.
 *
 * Strict mode adds:
 *   - Tag allowlist enforcement.  Default = the small RPC set in
 *     `docs/quic/rpc_design.md` § 4: 0/1 (datetime), 2/3 (bignum),
 *     27/28 (n00b result).
 *   - Duplicate-key rejection on every map (RFC 8949 § 5.6 — strict
 *     decoders treat duplicates as protocol errors).
 *
 * We implement strict mode as a post-decode walk: parse normally with
 * the existing decoder, then traverse the resulting AST to enforce the
 * additional rules.  This keeps the strict logic small and reuses the
 * battle-tested base decoder.
 * =========================================================================== */

static const uint64_t default_strict_tag_allowlist[] = {
    N00B_CBOR_TAG_DATETIME_RFC3339,
    N00B_CBOR_TAG_EPOCH,
    N00B_CBOR_TAG_BIGNUM_POS,
    N00B_CBOR_TAG_BIGNUM_NEG,
    N00B_CBOR_TAG_RESULT_OK,
    N00B_CBOR_TAG_RESULT_ERR,
};

static bool
tag_is_allowed(uint64_t tag,
               const uint64_t *allow,
               size_t          allow_len)
{
    for (size_t i = 0; i < allow_len; i++) {
        if (allow[i] == tag) return true;
    }
    return false;
}

/* Compare two CBOR values for byte-equality.  Used only to detect
 * duplicate map keys in strict mode.  Equality is structural for
 * primitives, byte-level for strings/bytes, recursive for containers.
 * (RFC 8949 § 5.6 mentions byte-level equality of the encoded
 * representation; this AST-based approximation matches that for
 * canonically encoded inputs and is conservative for non-canonical
 * inputs — which we'd reject elsewhere anyway.) */
static bool
cbor_value_equal(const n00b_cbor_value_t *a, const n00b_cbor_value_t *b)
{
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case N00B_CBOR_VT_UINT:
    case N00B_CBOR_VT_NEGINT:
        return a->u.uint == b->u.uint;
    case N00B_CBOR_VT_INT64:
        return a->u.int64 == b->u.int64;
    case N00B_CBOR_VT_BOOL:
        return a->u.boolean == b->u.boolean;
    case N00B_CBOR_VT_DOUBLE:
        return a->u.f64 == b->u.f64;
    case N00B_CBOR_VT_FLOAT32:
        return a->u.f32 == b->u.f32;
    case N00B_CBOR_VT_FLOAT16:
        return a->u.f16_bits == b->u.f16_bits;
    case N00B_CBOR_VT_NULL:
    case N00B_CBOR_VT_UNDEFINED:
        return true;
    case N00B_CBOR_VT_SIMPLE:
        return a->u.simple == b->u.simple;
    case N00B_CBOR_VT_STRING: {
        n00b_string_t *sa = a->u.string, *sb = b->u.string;
        if (!sa || !sb) return sa == sb;
        if (sa->u8_bytes != sb->u8_bytes) return false;
        return memcmp(sa->data, sb->data, sa->u8_bytes) == 0;
    }
    case N00B_CBOR_VT_BYTES: {
        n00b_buffer_t *ba = a->u.bytes, *bb = b->u.bytes;
        if (!ba || !bb) return ba == bb;
        if (ba->byte_len != bb->byte_len) return false;
        return memcmp(ba->data, bb->data, (size_t)ba->byte_len) == 0;
    }
    case N00B_CBOR_VT_TAG:
        return a->u.tag.tag == b->u.tag.tag &&
               cbor_value_equal(a->u.tag.inner, b->u.tag.inner);
    case N00B_CBOR_VT_ARRAY:
        if (a->u.array.count != b->u.array.count) return false;
        for (size_t i = 0; i < a->u.array.count; i++) {
            if (!cbor_value_equal(a->u.array.items[i], b->u.array.items[i]))
                return false;
        }
        return true;
    case N00B_CBOR_VT_MAP:
        /* Maps are extremely rarely used as map keys; if it ever
         * happens just enforce structural equality. */
        if (a->u.map.count != b->u.map.count) return false;
        for (size_t i = 0; i < a->u.map.count; i++) {
            if (!cbor_value_equal(a->u.map.pairs[i].key,
                                   b->u.map.pairs[i].key) ||
                !cbor_value_equal(a->u.map.pairs[i].val,
                                   b->u.map.pairs[i].val))
                return false;
        }
        return true;
    }
    return false;
}

static n00b_err_t
strict_walk(const n00b_cbor_value_t       *v,
            const uint64_t                *allow,
            size_t                         allow_len,
            int                            depth,
            int                            max_depth)
{
    if (!v) return N00B_QUIC_OK;
    if (depth > max_depth) return N00B_QUIC_ERR_PROTOCOL;
    switch (v->kind) {
    case N00B_CBOR_VT_TAG:
        if (!tag_is_allowed(v->u.tag.tag, allow, allow_len)) {
            return N00B_QUIC_ERR_PROTOCOL;
        }
        return strict_walk(v->u.tag.inner, allow, allow_len,
                           depth + 1, max_depth);
    case N00B_CBOR_VT_ARRAY:
        for (size_t i = 0; i < v->u.array.count; i++) {
            n00b_err_t e = strict_walk(v->u.array.items[i], allow, allow_len,
                                       depth + 1, max_depth);
            if (e != N00B_QUIC_OK) return e;
        }
        return N00B_QUIC_OK;
    case N00B_CBOR_VT_MAP:
        /* O(n^2) duplicate check; acceptable for RPC bodies (small N).
         * For very-large maps this would want a hash side-table — out
         * of scope for v1. */
        for (size_t i = 0; i < v->u.map.count; i++) {
            for (size_t j = i + 1; j < v->u.map.count; j++) {
                if (cbor_value_equal(v->u.map.pairs[i].key,
                                      v->u.map.pairs[j].key)) {
                    return N00B_QUIC_ERR_PROTOCOL;
                }
            }
            n00b_err_t e = strict_walk(v->u.map.pairs[i].key, allow, allow_len,
                                       depth + 1, max_depth);
            if (e != N00B_QUIC_OK) return e;
            e = strict_walk(v->u.map.pairs[i].val, allow, allow_len,
                            depth + 1, max_depth);
            if (e != N00B_QUIC_OK) return e;
        }
        return N00B_QUIC_OK;
    default:
        return N00B_QUIC_OK;
    }
}

n00b_result_t(n00b_cbor_value_t *)
n00b_cbor_decode_strict_bytes(const uint8_t                 *data,
                              size_t                         len,
                              const n00b_cbor_strict_opts_t *opts)
{
    auto base = n00b_cbor_decode_bytes(data, len);
    if (n00b_result_is_err(base)) {
        return base;
    }
    n00b_cbor_value_t *root = n00b_result_get(base);

    const uint64_t *allow     = default_strict_tag_allowlist;
    size_t          allow_len = sizeof(default_strict_tag_allowlist) /
                                sizeof(default_strict_tag_allowlist[0]);
    if (opts && opts->tag_allowlist) {
        allow     = opts->tag_allowlist;
        allow_len = opts->tag_allowlist_len;
    }
    /* The base decoder already enforces N00B_CBOR_MAX_DEPTH; strict
     * mode lets the caller request a tighter sub-cap via opts.max_depth.
     * 0 → use the base cap. */
    int max_depth = (opts && opts->max_depth > 0)
                    ? opts->max_depth
                    : N00B_CBOR_MAX_DEPTH;

    n00b_err_t e = strict_walk(root, allow, allow_len, 0, max_depth);
    if (e != N00B_QUIC_OK) {
        return n00b_result_err(n00b_cbor_value_t *, e);
    }
    return n00b_result_ok(n00b_cbor_value_t *, root);
}

n00b_result_t(n00b_cbor_value_t *)
n00b_cbor_decode_strict(n00b_buffer_t                 *input,
                        const n00b_cbor_strict_opts_t *opts)
{
    if (!input) {
        return n00b_result_err(n00b_cbor_value_t *, N00B_QUIC_ERR_NULL_ARG);
    }
    return n00b_cbor_decode_strict_bytes((const uint8_t *)input->data,
                                          (size_t)input->byte_len, opts);
}
