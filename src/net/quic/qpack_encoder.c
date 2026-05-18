/*
 * qpack_encoder.c — QPACK encoder + shared support code.
 *
 * Hosts:
 *   - Allocator helper (n00b_qpack_alloc / _dup).
 *   - Prefix-int + string codec (RFC 7541 § 5).
 *   - Dynamic-table primitives (alloc / insert / find / set_capacity).
 *   - Encoder API surface.
 *
 * Encoder strategy (intentionally simple for v1):
 *
 *   1. Iterate the field list.  For each field, try in order:
 *        a. Static table — exact match → indexed.
 *        b. Static table — name-only match → literal w/ name ref.
 *        c. Dynamic table — exact match → indexed (post-base or
 *           absolute).  We use absolute indexing (base = required
 *           insert count) for simplicity.
 *        d. Dynamic table — name-only match → literal w/ name ref.
 *        e. Otherwise → literal w/o name ref.
 *
 *   2. If the dynamic table has spare capacity AND the field is
 *      worth caching (heuristic: not already cached, both name +
 *      value plain ASCII, total ≤ capacity / 4), emit an
 *      Insert with Literal Name onto the encoder stream and use
 *      the new entry from the same field section.
 *
 *   3. Compute Required Insert Count (max of all dynamic ids
 *      referenced + 1) and emit the section prefix.
 *
 * Indexing:  This v1 encoder uses **absolute indexing** with
 * Base = Required Insert Count.  All dynamic-table references are
 * encoded as relative-to-base (subtract from base - 1).  Post-base
 * references are not used by the encoder, but the decoder accepts
 * them.  This sidesteps a class of head-of-line subtlety while
 * still being fully spec-compliant.
 */
#define N00B_USE_INTERNAL_API
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "internal/net/quic/qpack_internal.h"
#include "net/quic/qpack.h"
#include "net/quic/quic_types.h"

/* ===========================================================================
 * Allocator
 * =========================================================================== */

n00b_allocator_t *
n00b_qpack_alloc(void)
{
    return (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;
}

uint8_t *
n00b_qpack_dup(const uint8_t *src, size_t len)
{
    if (len == 0) {
        /* allocate a 1-byte stub so the pointer is non-null but distinct */
        uint8_t *p = n00b_alloc_array_with_opts(uint8_t, 1,
                        &(n00b_alloc_opts_t){
                            .allocator = n00b_qpack_alloc(),
                            .no_scan   = true,
                        });
        return p;
    }
    uint8_t *p = n00b_alloc_array_with_opts(uint8_t, (int64_t)len,
                    &(n00b_alloc_opts_t){
                        .allocator = n00b_qpack_alloc(),
                        .no_scan   = true,
                    });
    memcpy(p, src, len);
    return p;
}

/* ===========================================================================
 * Prefix-int (RFC 7541 § 5.1)
 * =========================================================================== */

static void
buf_append_byte(n00b_buffer_t *b, uint8_t byte)
{
    size_t old = (size_t)b->byte_len;
    n00b_buffer_resize(b, (uint64_t)(old + 1));
    ((uint8_t *)b->data)[old] = byte;
}

size_t
n00b_qpack_prefix_int_encode(n00b_buffer_t *out,
                             uint8_t        first_byte,
                             uint8_t        prefix_bits,
                             uint64_t       value)
{
    uint64_t max_in_first = ((uint64_t)1u << prefix_bits) - 1u;
    if (value < max_in_first) {
        buf_append_byte(out, (uint8_t)(first_byte | (uint8_t)value));
        return 1;
    }
    buf_append_byte(out, (uint8_t)(first_byte | (uint8_t)max_in_first));
    value -= max_in_first;
    size_t n = 1;
    while (value >= 128) {
        buf_append_byte(out, (uint8_t)((value & 0x7fu) | 0x80u));
        value >>= 7;
        n++;
    }
    buf_append_byte(out, (uint8_t)value);
    return n + 1;
}

int64_t
n00b_qpack_prefix_int_decode(const uint8_t *in, size_t in_len,
                             uint8_t        prefix_bits,
                             uint64_t      *out_value)
{
    if (in_len == 0) return 0;
    uint64_t mask = ((uint64_t)1u << prefix_bits) - 1u;
    uint64_t value = (uint64_t)in[0] & mask;
    if (value < mask) {
        if (out_value) *out_value = value;
        return 1;
    }
    size_t off = 1;
    uint64_t m = 0;
    while (off < in_len) {
        uint8_t byte = in[off++];
        if (m >= 63) {
            /* would overflow uint64_t / our 62-bit budget */
            return -1;
        }
        value += ((uint64_t)(byte & 0x7fu)) << m;
        m += 7;
        if ((byte & 0x80u) == 0) {
            if (out_value) *out_value = value;
            return (int64_t)off;
        }
    }
    return 0;  /* truncated */
}

/* ===========================================================================
 * String codec
 * =========================================================================== */

size_t
n00b_qpack_string_encode(n00b_buffer_t *out,
                         uint8_t        prefix_first_byte,
                         uint8_t        prefix_bits,    /* INCLUDES H bit */
                         const uint8_t *src, size_t src_len)
{
    /* prefix_bits: the H bit occupies the topmost of these.  Length
     * occupies the remaining (prefix_bits - 1). */
    uint8_t len_bits = (uint8_t)(prefix_bits - 1u);
    size_t  raw_size = src_len;
    size_t  huff_size = n00b_qpack_huffman_encoded_size(src, src_len);

    /* Use Huffman if it shrinks (or at least breaks even). */
    bool use_huff = huff_size < raw_size && src_len > 0;
    /* H bit = bit (prefix_bits - 1) of the byte's "value bits" (i.e.,
     * the highest of the prefix_bits bits we own).  prefix_first_byte
     * already has the high (8 - prefix_bits) bits set; the H bit is
     * the next-highest bit. */
    uint8_t h_byte = prefix_first_byte;
    if (use_huff) {
        h_byte |= (uint8_t)(1u << len_bits);
    }

    size_t consumed;
    if (use_huff) {
        consumed = n00b_qpack_prefix_int_encode(out, h_byte, len_bits,
                                                (uint64_t)huff_size);
        size_t old = (size_t)out->byte_len;
        n00b_buffer_resize(out, (uint64_t)(old + huff_size));
        n00b_result_t(size_t) er = n00b_qpack_huffman_encode(src, src_len,
                                       (uint8_t *)out->data + old, huff_size);
        (void)er;
    } else {
        consumed = n00b_qpack_prefix_int_encode(out, h_byte, len_bits,
                                                (uint64_t)raw_size);
        if (raw_size > 0) {
            size_t old = (size_t)out->byte_len;
            n00b_buffer_resize(out, (uint64_t)(old + raw_size));
            memcpy((uint8_t *)out->data + old, src, raw_size);
            consumed += raw_size;
        }
    }
    return consumed;
}

int64_t
n00b_qpack_string_decode(const uint8_t *in, size_t in_len,
                         uint8_t        prefix_bits,
                         uint8_t       *out_h_bit,
                         uint8_t      **out_str,
                         size_t        *out_str_len)
{
    if (in_len == 0) return 0;
    uint8_t len_bits = (uint8_t)(prefix_bits - 1u);

    /* Extract H bit before decoding length. */
    uint8_t h_mask = (uint8_t)(1u << len_bits);
    uint8_t h_bit  = (in[0] & h_mask) ? 1 : 0;

    /* Mask the H bit out of the first byte for prefix-int decode.  We
     * do this via a synthetic prefix-int decode that ignores the H bit:
     * prefix_int_decode operates on the low `len_bits` bits, but the
     * caller must ensure the H bit isn't accidentally treated as part
     * of the length.  For this we copy the first byte with H cleared. */
    uint8_t scratch_first = (uint8_t)(in[0] & ~h_mask);
    /* Build a mini-buffer: scratch_first followed by the rest.  Easier:
     * call prefix_int_decode with prefix_bits = len_bits — it only looks
     * at the low len_bits anyway. */
    /* ACTUALLY: the prefix-int routine masks via ((1<<prefix_bits)-1) which
     * with len_bits already excludes the H bit.  No scratch needed. */
    (void)scratch_first;

    uint64_t length = 0;
    int64_t  pn = n00b_qpack_prefix_int_decode(in, in_len, len_bits, &length);
    if (pn <= 0) return pn;

    if (length > N00B_QPACK_MAX_FIELD_LINE) return -1;
    if ((size_t)pn + length > in_len) return 0;

    if (h_bit) {
        /* Decode Huffman. Output buf can be at most 8x input (loose
         * upper bound, since min code length is 5 bits). */
        size_t cap = length * 8 + 8;
        if (cap > N00B_QPACK_MAX_FIELD_LINE) cap = N00B_QPACK_MAX_FIELD_LINE;
        uint8_t *buf = n00b_alloc_array_with_opts(uint8_t, (int64_t)(cap + 1),
                        &(n00b_alloc_opts_t){
                            .allocator = n00b_qpack_alloc(),
                            .no_scan   = true,
                        });
        n00b_result_t(size_t) dr =
            n00b_qpack_huffman_decode(in + pn, (size_t)length, buf, cap);
        if (n00b_result_is_err(dr)) {
            return -1;
        }
        size_t out_len = n00b_result_get(dr);
        buf[out_len] = 0;
        if (out_str) *out_str = buf;
        if (out_str_len) *out_str_len = out_len;
    } else {
        uint8_t *buf = n00b_qpack_dup(in + pn, (size_t)length);
        if (out_str) *out_str = buf;
        if (out_str_len) *out_str_len = (size_t)length;
    }
    if (out_h_bit) *out_h_bit = h_bit;
    return pn + (int64_t)length;
}

/* ===========================================================================
 * Dynamic table
 * =========================================================================== */

/* RFC 9204 § 3.2.1: per-entry size = name_len + value_len + 32. */
static uint64_t
entry_size(size_t name_len, size_t value_len)
{
    return (uint64_t)(name_len + value_len + 32u);
}

void
n00b_qpack_dyn_init(n00b_qpack_dyn_table_t *t, uint64_t max_capacity)
{
    memset(t, 0, sizeof(*t));
    t->lock = n00b_data_lock_new();
    t->max_capacity = max_capacity;
    t->capacity     = 0;
}

void
n00b_qpack_dyn_destroy(n00b_qpack_dyn_table_t *t)
{
    if (!t) return;
    
    /* entries themselves come from conduit_pool; let the pool reclaim. */
    t->entries = nullptr;
    t->entries_cap = 0;
    t->count = 0;
}

static void
dyn_grow_ring_unlocked(n00b_qpack_dyn_table_t *t, size_t new_cap)
{
    if (new_cap <= t->entries_cap) return;
    n00b_qpack_dyn_entry_t *ne = n00b_alloc_array_with_opts(
        n00b_qpack_dyn_entry_t, (int64_t)new_cap,
        &(n00b_alloc_opts_t){ .allocator = n00b_qpack_alloc() });

    /* Copy live entries in chronological order to the front of the new ring. */
    size_t old_first = (t->head + t->entries_cap - t->count) % (t->entries_cap ? t->entries_cap : 1);
    for (size_t i = 0; i < t->count; i++) {
        ne[i] = t->entries[(old_first + i) % t->entries_cap];
    }
    t->entries     = ne;
    t->entries_cap = new_cap;
    t->head        = t->count;
}

static void
evict_oldest_unlocked(n00b_qpack_dyn_table_t *t)
{
    if (t->count == 0) return;
    size_t old_first = (t->head + t->entries_cap - t->count) % t->entries_cap;
    n00b_qpack_dyn_entry_t *e = &t->entries[old_first];
    t->used -= entry_size(e->name_len, e->value_len);
    t->count--;
    t->first_insert_id = e->insert_id + 1;
    /* The slot's pointers stay in the ring; conduit_pool keeps them alive. */
}

bool
n00b_qpack_dyn_insert(n00b_qpack_dyn_table_t *t,
                      const uint8_t          *name,
                      size_t                  name_len,
                      const uint8_t          *value,
                      size_t                  value_len)
{
    n00b_data_write_lock(t->lock);

    uint64_t e_sz = entry_size(name_len, value_len);
    if (e_sz > t->capacity) {
        n00b_data_unlock(t->lock);
        return false;
    }
    /* Evict until there's room. */
    while (t->used + e_sz > t->capacity && t->count > 0) {
        evict_oldest_unlocked(t);
    }
    /* Grow ring if needed (geometric). */
    if (t->count + 1 > t->entries_cap) {
        size_t new_cap = t->entries_cap == 0 ? 8 : t->entries_cap * 2;
        dyn_grow_ring_unlocked(t, new_cap);
    }
    /* Slot at head. */
    n00b_qpack_dyn_entry_t *e = &t->entries[t->head];
    e->name      = n00b_qpack_dup(name, name_len);
    e->name_len  = name_len;
    e->value     = n00b_qpack_dup(value, value_len);
    e->value_len = value_len;
    e->insert_id = t->insert_count;
    t->head = (t->head + 1) % t->entries_cap;
    t->count++;
    t->insert_count++;
    t->used += e_sz;

    n00b_data_unlock(t->lock);
    return true;
}

bool
n00b_qpack_dyn_set_capacity(n00b_qpack_dyn_table_t *t, uint64_t new_cap)
{
    n00b_data_write_lock(t->lock);
    if (new_cap > t->max_capacity) {
        n00b_data_unlock(t->lock);
        return false;
    }
    t->capacity = new_cap;
    while (t->used > t->capacity && t->count > 0) {
        evict_oldest_unlocked(t);
    }
    n00b_data_unlock(t->lock);
    return true;
}

bool
n00b_qpack_dyn_get_by_id(n00b_qpack_dyn_table_t *t,
                         uint64_t                id,
                         n00b_qpack_dyn_entry_t *out)
{
    n00b_data_write_lock(t->lock);
    bool ok = false;
    if (id >= t->first_insert_id && id < t->insert_count) {
        size_t old_first = (t->head + t->entries_cap - t->count) % t->entries_cap;
        size_t off = (size_t)(id - t->first_insert_id);
        size_t slot = (old_first + off) % t->entries_cap;
        if (out) *out = t->entries[slot];
        ok = true;
    }
    n00b_data_unlock(t->lock);
    return ok;
}

static bool
mem_eq(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
    if (alen != blen) return false;
    if (alen == 0) return true;
    return memcmp(a, b, alen) == 0;
}

bool
n00b_qpack_dyn_find(n00b_qpack_dyn_table_t *t,
                    const uint8_t          *name,
                    size_t                  name_len,
                    const uint8_t          *value,
                    size_t                  value_len,
                    uint64_t               *out_id,
                    bool                   *out_exact)
{
    n00b_data_write_lock(t->lock);
    bool found = false;
    bool exact = false;
    uint64_t id = 0;

    if (t->count > 0) {
        size_t old_first = (t->head + t->entries_cap - t->count) % t->entries_cap;
        /* Walk newest first to prefer recent inserts. */
        for (size_t i = t->count; i-- > 0; ) {
            size_t slot = (old_first + i) % t->entries_cap;
            n00b_qpack_dyn_entry_t *e = &t->entries[slot];
            if (mem_eq(e->name, e->name_len, name, name_len)) {
                if (mem_eq(e->value, e->value_len, value, value_len)) {
                    found = true;
                    exact = true;
                    id = e->insert_id;
                    break;
                }
                if (!found) {
                    found = true;
                    exact = false;
                    id = e->insert_id;
                }
            }
        }
    }
    if (out_id) *out_id = id;
    if (out_exact) *out_exact = exact;
    n00b_data_unlock(t->lock);
    return found;
}

/* ===========================================================================
 * Static-table search
 * =========================================================================== */

static bool
static_find(const uint8_t *name, size_t name_len,
            const uint8_t *value, size_t value_len,
            size_t *out_idx, bool *out_exact)
{
    bool found_name = false;
    size_t name_idx = 0;
    /* Linear scan; static table is 99 entries. */
    for (size_t i = 0; i < N00B_QPACK_STATIC_TABLE_SIZE; i++) {
        const n00b_qpack_static_entry_t *e = &n00b_qpack_static_table[i];
        if (mem_eq((const uint8_t *)e->name, e->name_len, name, name_len)) {
            if (mem_eq((const uint8_t *)e->value, e->value_len, value, value_len)) {
                if (out_idx) *out_idx = i;
                if (out_exact) *out_exact = true;
                return true;
            }
            if (!found_name) {
                found_name = true;
                name_idx = i;
            }
        }
    }
    if (found_name) {
        if (out_idx) *out_idx = name_idx;
        if (out_exact) *out_exact = false;
        return true;
    }
    return false;
}

/* ===========================================================================
 * Encoder
 * =========================================================================== */

n00b_qpack_encoder_t *
n00b_qpack_encoder_new(uint64_t max_table_capacity, uint64_t max_blocked_streams)
{
    if (max_table_capacity > N00B_QPACK_MAX_DYNAMIC_CAPACITY) {
        max_table_capacity = N00B_QPACK_MAX_DYNAMIC_CAPACITY;
    }
    n00b_qpack_encoder_t *enc = n00b_alloc_with_opts(n00b_qpack_encoder_t,
                                    &(n00b_alloc_opts_t){
                                        .allocator = n00b_qpack_alloc(),
                                    });
    enc->lock = n00b_data_lock_new();
    n00b_qpack_dyn_init(&enc->dyn, max_table_capacity);
    enc->max_blocked_streams = max_blocked_streams;
    enc->known_received_count = 0;
    enc->pending_head = nullptr;
    return enc;
}

void
n00b_qpack_encoder_close(n00b_qpack_encoder_t *enc)
{
    if (!enc) return;
    n00b_qpack_dyn_destroy(&enc->dyn);
    
    /* conduit_pool collects the rest. */
}

uint64_t
n00b_qpack_encoder_insert_count(n00b_qpack_encoder_t *enc)
{
    if (!enc) return 0;
    n00b_data_write_lock(enc->dyn.lock);
    uint64_t v = enc->dyn.insert_count;
    n00b_data_unlock(enc->dyn.lock);
    return v;
}

uint64_t
n00b_qpack_encoder_known_received_count(n00b_qpack_encoder_t *enc)
{
    if (!enc) return 0;
    n00b_data_write_lock(enc->lock);
    uint64_t v = enc->known_received_count;
    n00b_data_unlock(enc->lock);
    return v;
}

/* Set Dynamic Table Capacity instruction onto encoder stream. */
static void
emit_set_capacity(n00b_buffer_t *enc_stream, uint64_t cap)
{
    if (!enc_stream) return;
    n00b_qpack_prefix_int_encode(enc_stream,
                                 N00B_QPACK_ES_SET_CAPACITY, 5, cap);
}

/* Insert with Literal Name onto encoder stream. */
static void
emit_insert_literal(n00b_buffer_t *enc_stream,
                    const uint8_t *name, size_t name_len,
                    const uint8_t *value, size_t value_len)
{
    /* 01HNNNNN — N bits in {5,4} (5 length bits for name; H is the
     * topmost of those 5 plus the H bit that string_encode owns).
     * RFC 9204 § 4.3.2: prefix-bits-for-name = 5; name byte starts
     * 01 in top 2 bits.  String_encode uses (prefix_bits = 6 with H
     * bit topmost): we set prefix_first_byte = 0x40 (01) and
     * prefix_bits = 6 (H + 5-bit length). */
    n00b_qpack_string_encode(enc_stream, N00B_QPACK_ES_INSERT_LITERAL, 6,
                             name, name_len);
    /* Then the value: H + 7-bit length, with the H bit being the
     * top bit of the new byte. */
    n00b_qpack_string_encode(enc_stream, 0x00, 8, value, value_len);
}

/* Field-section literal-with-name-ref (§ 4.5.4).
 * 01NTxxxx where N=never-index, T=table (1 = static),
 * 4-bit index (with H + 7-bit length on value). */
static void
emit_lit_nameref(n00b_buffer_t *out, bool t_static,
                 uint64_t name_idx,
                 const uint8_t *value, size_t value_len)
{
    uint8_t first = N00B_QPACK_FL_LIT_NAMEREF;        /* 01000000 */
    if (t_static) first |= N00B_QPACK_FL_LIT_NAMEREF_T;  /* 00010000 */
    /* prefix_bits for the name index = 4 */
    n00b_qpack_prefix_int_encode(out, first, 4, name_idx);
    n00b_qpack_string_encode(out, 0x00, 8, value, value_len);
}

/* Field-section literal-without-name-ref (§ 4.5.6).
 * 001NHxxx — H is the H bit on the name string.  prefix_bits for
 * the name = 4 (with H topmost).  Value follows with H + 7-bit length. */
static void
emit_lit_literal(n00b_buffer_t *out,
                 const uint8_t *name, size_t name_len,
                 const uint8_t *value, size_t value_len)
{
    n00b_qpack_string_encode(out, N00B_QPACK_FL_LIT_LITERAL, 4,
                             name, name_len);
    n00b_qpack_string_encode(out, 0x00, 8, value, value_len);
}

/* Field-section indexed (§ 4.5.2): 1Txxxxxx.  T=1 static. */
static void
emit_indexed(n00b_buffer_t *out, bool t_static, uint64_t idx)
{
    uint8_t first = N00B_QPACK_FL_INDEXED;  /* 1xxxxxxx */
    if (t_static) first |= N00B_QPACK_FL_INDEXED_T;
    n00b_qpack_prefix_int_encode(out, first, 6, idx);
}

/* Encode the section prefix (RFC 9204 § 4.5.1):
 * - Required Insert Count: prefix-int 8 with the wrapped/encoded form.
 *   For simplicity we send 0 if no dynamic refs, else the absolute
 *   value (RFC § 4.5.1.1's "encoded RIC = (RIC mod (2*MAX_ENTRIES)) + 1"
 *   collapses to RIC + 1 for our v1 encoder when MaxEntries large enough).
 *   We use the simple form: encoded = required + 1 for required > 0,
 *   encoded = 0 for required = 0.  This is only correct when
 *   required < 2 * MaxEntries; for v1 with ≤ 1 MiB capacity that's
 *   well-bounded.
 * - S | Delta Base: S=0 (base ≥ required), Delta Base = 0
 *   (base == required).  Indexes in body are then "relative-to-base"
 *   = base - 1 - id where id < base.
 */
static void
write_section_prefix(n00b_buffer_t *prefix_buf,
                     uint64_t required_insert_count,
                     uint64_t base_delta,
                     bool     base_neg)
{
    uint64_t encoded_ric = required_insert_count == 0
                            ? 0
                            : required_insert_count + 1;
    n00b_qpack_prefix_int_encode(prefix_buf, 0x00, 8, encoded_ric);
    uint8_t s_bit = base_neg ? 0x80 : 0x00;
    n00b_qpack_prefix_int_encode(prefix_buf, s_bit, 7, base_delta);
}

/* Decide whether to cache a given field in the dynamic table.
 * Heuristics, not mandated by the spec:
 *   - Skip headers we know are highly variable (cookie, set-cookie,
 *     authorization, traceparent).
 *   - Skip if name+value too large (would evict everything else).
 */
static bool
should_cache(const n00b_qpack_field_t *f, uint64_t cap)
{
    if (cap == 0) return false;
    uint64_t e_sz = entry_size(f->name_len, f->value_len);
    if (e_sz * 4 > cap) return false;

    static const char * const skip[] = {
        "cookie", "set-cookie", "authorization", "traceparent",
        "tracestate", "if-none-match", "if-modified-since",
    };
    for (size_t i = 0; i < sizeof(skip)/sizeof(skip[0]); i++) {
        size_t sl = strlen(skip[i]);
        if (f->name_len == sl && memcmp(f->name, skip[i], sl) == 0) {
            return false;
        }
    }
    return true;
}

n00b_result_t(bool)
n00b_qpack_encode(n00b_qpack_encoder_t      *enc,
                  uint64_t                   stream_id,
                  const n00b_qpack_field_t  *fields,
                  size_t                     n_fields,
                  n00b_buffer_t             *out_field_section,
                  n00b_buffer_t             *out_encoder_stream)
{
    if (!enc || !fields || !out_field_section) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    }
    if (n_fields > N00B_QPACK_MAX_FIELDS_PER_SECTION) {
        return n00b_result_err(bool, N00B_QUIC_ERR_FRAME_TOO_LARGE);
    }
    for (size_t i = 0; i < n_fields; i++) {
        if (fields[i].name_len > N00B_QPACK_MAX_FIELD_LINE
            || fields[i].value_len > N00B_QPACK_MAX_FIELD_LINE) {
            return n00b_result_err(bool, N00B_QUIC_ERR_FRAME_TOO_LARGE);
        }
    }

    n00b_data_write_lock(enc->lock);

    /* Save buffer lengths so we can restore on error. */
    size_t save_fs  = (size_t)out_field_section->byte_len;
    size_t save_es  = out_encoder_stream
                       ? (size_t)out_encoder_stream->byte_len
                       : 0;

    /* If encoder stream is given and dynamic capacity hasn't been set,
     * leave it alone.  In a real H3 stack, capacity is set once after
     * SETTINGS exchange.  For unit tests, the encoder accepts a
     * pre-set capacity via set_capacity below; we don't auto-emit it. */

    /* Body buffer (we'll prepend the section prefix once we know
     * required_insert_count). */
    n00b_buffer_t *body = n00b_alloc_with_opts(n00b_buffer_t,
                            &(n00b_alloc_opts_t){
                                .allocator = n00b_qpack_alloc(),
                            });
    n00b_buffer_init(body, .allocator = n00b_qpack_alloc(), .no_lock = true);

    uint64_t base = 0;  /* will be set to dyn.insert_count after any inserts */

    /* Pre-snapshot insert count; references encoded relative to a
     * post-pass base = current insert_count + new_inserts. */
    n00b_data_write_lock(enc->dyn.lock);
    uint64_t pre_insert_count = enc->dyn.insert_count;
    uint64_t cap = enc->dyn.capacity;
    n00b_data_unlock(enc->dyn.lock);
    uint64_t new_inserts = 0;
    uint64_t max_referenced_id_plus_1 = 0;  /* required_insert_count */

    /* Per-field id referenced (max +1).  We accumulate references as
     * encoder-stream insertions are issued; for refs to entries that
     * already existed, we reference their existing id.  All ids are
     * absolute (lifetime). */

    /* Pass: iterate fields, decide encoding, emit to body, possibly
     * emit insertion to encoder_stream. */
    for (size_t i = 0; i < n_fields; i++) {
        const n00b_qpack_field_t *f = &fields[i];
        size_t s_idx;
        bool   s_exact;
        bool   s_hit = static_find(f->name, f->name_len,
                                   f->value, f->value_len,
                                   &s_idx, &s_exact);
        if (s_hit && s_exact) {
            emit_indexed(body, true, (uint64_t)s_idx);
            continue;
        }

        /* Try dynamic exact. */
        uint64_t d_id;
        bool     d_exact;
        bool     d_hit = n00b_qpack_dyn_find(&enc->dyn,
                                             f->name, f->name_len,
                                             f->value, f->value_len,
                                             &d_id, &d_exact);
        if (d_hit && d_exact) {
            uint64_t this_ric = d_id + 1;
            if (this_ric > max_referenced_id_plus_1)
                max_referenced_id_plus_1 = this_ric;
            /* Emit indexed dynamic, T=0, idx = base - 1 - d_id (relative).
             * We don't know the final base yet — but we DO know the index
             * relative to "current insert count + new_inserts" computed
             * below.  We defer emission... but all current literals are
             * straightforward.  Simpler: use Required Insert Count =
             * max referenced id + 1, set base = required, and use
             * relative index = base - 1 - d_id.  We don't know `base`
             * until after the pass; emit a placeholder and rewrite?
             *
             * Even simpler: since we use base = required_insert_count
             * (delta_base = 0, S = 0), we can compute relative index as
             * (required - 1 - d_id) at the END.  Here we record the
             * insertion as "indexed with absolute id = d_id" and emit
             * later.  But that requires a deferred-emit list.  Simpler
             * still: use post-base indexing if d_id >= base?  No — d_id
             * is from already-existing entries (< base).
             *
             * Tactical choice: do a two-pass — compute required_insert_count
             * first, then emit.  We restart the body buffer below. */
            goto two_pass;
        }
        if (s_hit) {
            /* Static name-ref + literal value. */
            emit_lit_nameref(body, true, (uint64_t)s_idx,
                             f->value, f->value_len);
            continue;
        }
        if (d_hit) {
            /* Dynamic name-ref + literal value. */
            uint64_t this_ric = d_id + 1;
            if (this_ric > max_referenced_id_plus_1)
                max_referenced_id_plus_1 = this_ric;
            goto two_pass;
        }

        /* Not in any table.  Optionally cache + reference. */
        if (out_encoder_stream
            && cap > 0
            && enc->max_blocked_streams > 0    /* allow blocking refs */
            && should_cache(f, cap)) {
            /* Decide first whether cache succeeds (i.e., capacity admits). */
            uint64_t e_sz = entry_size(f->name_len, f->value_len);
            if (e_sz <= cap) {
                emit_insert_literal(out_encoder_stream,
                                    f->name, f->name_len,
                                    f->value, f->value_len);
                bool ok = n00b_qpack_dyn_insert(&enc->dyn,
                                                f->name, f->name_len,
                                                f->value, f->value_len);
                if (ok) {
                    new_inserts++;
                    uint64_t this_ric = pre_insert_count + new_inserts;
                    if (this_ric > max_referenced_id_plus_1)
                        max_referenced_id_plus_1 = this_ric;
                    goto two_pass;
                }
            }
        }

        /* Fallthrough: literal w/o name ref. */
        emit_lit_literal(body, f->name, f->name_len,
                         f->value, f->value_len);
        continue;

two_pass:
        /* Restart: rebuild body in a single pass with known base.
         * IMPORTANT: do NOT roll back encoder_stream — the dyn
         * insertions are committed; their corresponding encoder-stream
         * bytes must remain so the decoder mirrors the same state.
         * We'll treat already-inserted entries as existing dynamic
         * entries in the second pass. */
        body->byte_len = 0;
        max_referenced_id_plus_1 = 0;
        new_inserts = 0;
        n00b_data_write_lock(enc->dyn.lock);
        uint64_t now = enc->dyn.insert_count;
        n00b_data_unlock(enc->dyn.lock);

        /* Pre-pass: compute required_insert_count (max ref id + 1
         * across all fields) AND insert any should-cache fields that
         * aren't already in the table.  This is the deferred-emit
         * fix (#189) — previously the two-pass path treated all
         * should-cache fields as literal-only, leaving the dynamic
         * table unwarmed for subsequent requests.  Now we insert
         * them here so the final pass can reference them as indexed
         * dynamic entries.  Encoder-stream bytes are emitted
         * immediately; the decoder will see them before processing
         * this field section (insert_count ≥ required_insert_count
         * is the unblocking criterion). */
        uint64_t ric = 0;
        for (size_t j = 0; j < n_fields; j++) {
            const n00b_qpack_field_t *g = &fields[j];
            size_t sj_idx; bool sj_ex;
            if (static_find(g->name, g->name_len, g->value, g->value_len,
                            &sj_idx, &sj_ex) && sj_ex) {
                continue;
            }
            uint64_t dj_id; bool dj_ex;
            if (n00b_qpack_dyn_find(&enc->dyn, g->name, g->name_len,
                                    g->value, g->value_len,
                                    &dj_id, &dj_ex)) {
                if (dj_id + 1 > ric) ric = dj_id + 1;
                continue;
            }
            /* Not in any table — try to cache it. */
            if (out_encoder_stream
                && cap > 0
                && enc->max_blocked_streams > 0
                && should_cache(g, cap)) {
                uint64_t e_sz = entry_size(g->name_len, g->value_len);
                if (e_sz <= cap) {
                    emit_insert_literal(out_encoder_stream,
                                        g->name, g->name_len,
                                        g->value, g->value_len);
                    bool ok = n00b_qpack_dyn_insert(&enc->dyn,
                                                    g->name, g->name_len,
                                                    g->value, g->value_len);
                    if (ok) {
                        /* Look up the newly-inserted id and fold
                         * into the ric. */
                        uint64_t newd_id;
                        bool     newd_ex;
                        if (n00b_qpack_dyn_find(&enc->dyn,
                                                g->name, g->name_len,
                                                g->value, g->value_len,
                                                &newd_id, &newd_ex)
                            && newd_ex
                            && newd_id + 1 > ric) {
                            ric = newd_id + 1;
                        }
                    }
                }
            }
        }

        uint64_t base_inner = ric;

        /* Final emit pass: literal/indexed only, no caching. */
        for (size_t j = 0; j < n_fields; j++) {
            const n00b_qpack_field_t *g = &fields[j];
            size_t sj_idx; bool sj_ex;
            bool sj_hit = static_find(g->name, g->name_len,
                                      g->value, g->value_len,
                                      &sj_idx, &sj_ex);
            if (sj_hit && sj_ex) {
                emit_indexed(body, true, (uint64_t)sj_idx);
                continue;
            }
            uint64_t dj_id; bool dj_ex;
            bool dj_hit = n00b_qpack_dyn_find(&enc->dyn, g->name, g->name_len,
                                              g->value, g->value_len,
                                              &dj_id, &dj_ex);
            if (dj_hit && dj_ex) {
                /* Indexed dynamic, T=0, relative idx = base - 1 - dj_id. */
                uint64_t rel = base_inner - 1 - dj_id;
                emit_indexed(body, false, rel);
                continue;
            }
            if (sj_hit) {
                emit_lit_nameref(body, true, (uint64_t)sj_idx,
                                 g->value, g->value_len);
                continue;
            }
            if (dj_hit) {
                /* Lit nameref dynamic: 0100Nxxx; T=0; rel = base-1-dj_id. */
                uint64_t rel = base_inner - 1 - dj_id;
                emit_lit_nameref(body, false, rel,
                                 g->value, g->value_len);
                continue;
            }
            emit_lit_literal(body, g->name, g->name_len,
                             g->value, g->value_len);
        }

        /* Section prefix: write to out_field_section first, then body. */
        write_section_prefix(out_field_section, base_inner, 0, false);
        size_t blen = (size_t)body->byte_len;
        size_t old  = (size_t)out_field_section->byte_len;
        n00b_buffer_resize(out_field_section, (uint64_t)(old + blen));
        memcpy((uint8_t *)out_field_section->data + old, body->data, blen);

        /* Track pending section ack (if any dynamic refs). */
        if (base_inner > 0) {
            n00b_qpack_pending_section_t *p = n00b_alloc_with_opts(
                n00b_qpack_pending_section_t,
                &(n00b_alloc_opts_t){ .allocator = n00b_qpack_alloc() });
            p->stream_id = stream_id;
            p->required_insert_count = base_inner;
            p->next = enc->pending_head;
            enc->pending_head = p;
        }
        (void)now;
        (void)base;
        n00b_data_unlock(enc->lock);
        return n00b_result_ok(bool, true);
    }

    /* No two-pass triggered: simple case, no dynamic refs, no
     * insertions.  required_insert_count = 0. */
    write_section_prefix(out_field_section, 0, 0, false);
    size_t blen = (size_t)body->byte_len;
    size_t old  = (size_t)out_field_section->byte_len;
    n00b_buffer_resize(out_field_section, (uint64_t)(old + blen));
    memcpy((uint8_t *)out_field_section->data + old, body->data, blen);

    (void)save_fs;
    (void)stream_id;
    (void)base;
    n00b_data_unlock(enc->lock);
    return n00b_result_ok(bool, true);
}

/* Public: explicitly set the dynamic table capacity.  This is called
 * by the H3 layer after SETTINGS exchange.  Not in qpack.h because
 * for v1 we expose it as part of the encoder's configuration only;
 * the test harness uses it directly. */
n00b_result_t(bool)
n00b_qpack_encoder_set_capacity(n00b_qpack_encoder_t *enc,
                                uint64_t              new_capacity,
                                n00b_buffer_t        *out_encoder_stream)
{
    if (!enc) return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    if (!n00b_qpack_dyn_set_capacity(&enc->dyn, new_capacity)) {
        return n00b_result_err(bool, N00B_QUIC_ERR_INVALID_ARG);
    }
    if (out_encoder_stream) {
        emit_set_capacity(out_encoder_stream, new_capacity);
    }
    return n00b_result_ok(bool, true);
}

/* ===========================================================================
 * Decoder-stream consumer
 * =========================================================================== */

n00b_result_t(size_t)
n00b_qpack_encoder_consume_decoder_stream(n00b_qpack_encoder_t *enc,
                                          const uint8_t        *data,
                                          size_t                data_len)
{
    if (!enc || (!data && data_len > 0)) {
        return n00b_result_err(size_t, N00B_QUIC_ERR_NULL_ARG);
    }
    n00b_data_write_lock(enc->lock);

    size_t off = 0;
    while (off < data_len) {
        uint8_t b = data[off];
        if ((b & N00B_QPACK_DS_SECTION_ACK_MASK)
            == N00B_QPACK_DS_SECTION_ACK) {
            uint64_t stream_id;
            int64_t n = n00b_qpack_prefix_int_decode(data + off,
                                                     data_len - off,
                                                     7, &stream_id);
            if (n < 0) {
                n00b_data_unlock(enc->lock);
                return n00b_result_err(size_t, N00B_QUIC_ERR_PROTOCOL);
            }
            if (n == 0) break;
            /* Find pending section for this stream.  RFC 9204 § 4.4.1:
             * the ack confirms the most-recent unacknowledged section
             * for the given stream.  Multiple in-flight sections per
             * stream are extremely rare in practice; we walk the list. */
            n00b_qpack_pending_section_t **pp = &enc->pending_head;
            while (*pp) {
                if ((*pp)->stream_id == stream_id) {
                    if ((*pp)->required_insert_count
                        > enc->known_received_count) {
                        enc->known_received_count =
                            (*pp)->required_insert_count;
                    }
                    *pp = (*pp)->next;
                    break;
                }
                pp = &(*pp)->next;
            }
            off += (size_t)n;
        } else if ((b & N00B_QPACK_DS_STREAM_CANCEL_MASK)
                   == N00B_QPACK_DS_STREAM_CANCEL) {
            uint64_t stream_id;
            int64_t n = n00b_qpack_prefix_int_decode(data + off,
                                                     data_len - off,
                                                     6, &stream_id);
            if (n < 0) {
                n00b_data_unlock(enc->lock);
                return n00b_result_err(size_t, N00B_QUIC_ERR_PROTOCOL);
            }
            if (n == 0) break;
            /* Drop all pending sections for this stream. */
            n00b_qpack_pending_section_t **pp = &enc->pending_head;
            while (*pp) {
                if ((*pp)->stream_id == stream_id) {
                    *pp = (*pp)->next;
                } else {
                    pp = &(*pp)->next;
                }
            }
            off += (size_t)n;
        } else {
            /* Insert Count Increment: 00xxxxxx, prefix bits = 6 */
            uint64_t inc;
            int64_t n = n00b_qpack_prefix_int_decode(data + off,
                                                     data_len - off,
                                                     6, &inc);
            if (n < 0) {
                n00b_data_unlock(enc->lock);
                return n00b_result_err(size_t, N00B_QUIC_ERR_PROTOCOL);
            }
            if (n == 0) break;
            /* Increment must be > 0 per RFC; tolerate 0. */
            enc->known_received_count += inc;
            off += (size_t)n;
        }
    }
    n00b_data_unlock(enc->lock);
    return n00b_result_ok(size_t, off);
}
