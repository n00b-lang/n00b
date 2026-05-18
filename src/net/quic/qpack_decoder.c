/*
 * qpack_decoder.c — QPACK decoder + encoder-stream consumer.
 *
 * Decoder is split into two sources of input:
 *
 *   1. Encoder stream — adds entries to the dynamic table; we
 *      mirror the encoder's state.  Each Insert with Name Reference
 *      / Insert with Literal Name / Duplicate produces one new
 *      dynamic-table entry.  Set Dynamic Table Capacity adjusts
 *      the cap in place.
 *
 *   2. Field section — the encoded headers for one request/push
 *      stream.  Decodes to a list of (name, value) pairs.
 *
 * After processing each chunk of encoder-stream bytes we may emit
 * an "Insert Count Increment" message onto the decoder-stream
 * buffer (§ 4.4.2).  After decoding a field section we emit a
 * "Section Acknowledgment" (§ 4.4.1) — but only if the section
 * referenced any dynamic entries.
 */
#define N00B_USE_INTERNAL_API
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "internal/net/quic/qpack_internal.h"
#include "net/quic/qpack.h"
#include "net/quic/quic_types.h"

/* ===========================================================================
 * Construction / teardown
 * =========================================================================== */

n00b_qpack_decoder_t *
n00b_qpack_decoder_new(uint64_t max_table_capacity, uint64_t max_blocked_streams)
{
    if (max_table_capacity > N00B_QPACK_MAX_DYNAMIC_CAPACITY) {
        max_table_capacity = N00B_QPACK_MAX_DYNAMIC_CAPACITY;
    }
    n00b_qpack_decoder_t *dec = n00b_alloc_with_opts(n00b_qpack_decoder_t,
                                    &(n00b_alloc_opts_t){
                                        .allocator = n00b_qpack_alloc(),
                                    });
    dec->lock = n00b_data_lock_new();
    n00b_qpack_dyn_init(&dec->dyn, max_table_capacity);
    dec->max_blocked_streams = max_blocked_streams;
    dec->unreported_inserts = 0;
    /* The decoder unilaterally decides its own usable capacity by
     * default.  Tests / H3 SETTINGS will set this via the encoder
     * issuing a Set Dynamic Table Capacity instruction (handled in
     * consume_encoder_stream below). */
    n00b_qpack_dyn_set_capacity(&dec->dyn, max_table_capacity);
    return dec;
}

void
n00b_qpack_decoder_close(n00b_qpack_decoder_t *dec)
{
    if (!dec) return;
    n00b_qpack_dyn_destroy(&dec->dyn);
    
}

uint64_t
n00b_qpack_decoder_insert_count(n00b_qpack_decoder_t *dec)
{
    if (!dec) return 0;
    n00b_data_write_lock(dec->dyn.lock);
    uint64_t v = dec->dyn.insert_count;
    n00b_data_unlock(dec->dyn.lock);
    return v;
}

/* ===========================================================================
 * Encoder-stream consumer
 *
 * Each instruction:
 *   - 1xxxxxxx: Insert with Name Reference.  Bit 6 = T (1=static).
 *     Prefix-int 6 = name index.  Then a string (value).
 *   - 01xxxxxx: Insert with Literal Name.  Prefix-int 5 includes H bit
 *     for the name; then a string (value).
 *   - 001xxxxx: Set Dynamic Table Capacity.  Prefix-int 5 = capacity.
 *   - 000xxxxx: Duplicate.  Prefix-int 5 = relative index (0 = newest).
 * =========================================================================== */

n00b_result_t(size_t)
n00b_qpack_decoder_consume_encoder_stream(n00b_qpack_decoder_t *dec,
                                          const uint8_t        *data,
                                          size_t                data_len,
                                          n00b_buffer_t        *out_decoder_stream)
{
    if (!dec || (!data && data_len > 0)) {
        return n00b_result_err(size_t, N00B_QUIC_ERR_NULL_ARG);
    }
    n00b_data_write_lock(dec->lock);

    size_t off = 0;
    while (off < data_len) {
        size_t  start = off;
        uint8_t b     = data[off];

        if ((b & N00B_QPACK_ES_INSERT_NAMEREF_MASK)
            == N00B_QPACK_ES_INSERT_NAMEREF) {
            /* Insert with Name Reference. */
            bool    t_static = (b & 0x40u) != 0;
            uint64_t name_idx;
            int64_t  pn = n00b_qpack_prefix_int_decode(data + off,
                                                       data_len - off,
                                                       6, &name_idx);
            if (pn < 0) goto bad;
            if (pn == 0) break;
            off += (size_t)pn;

            uint8_t  hbit;
            uint8_t *value;
            size_t   value_len;
            int64_t  vn = n00b_qpack_string_decode(data + off,
                                                   data_len - off,
                                                   8, &hbit,
                                                   &value, &value_len);
            if (vn < 0) goto bad;
            if (vn == 0) { off = start; break; }
            off += (size_t)vn;

            const uint8_t *nm = nullptr;
            size_t         nm_len = 0;
            if (t_static) {
                if (name_idx >= N00B_QPACK_STATIC_TABLE_SIZE) goto bad;
                nm     = (const uint8_t *)n00b_qpack_static_table[name_idx].name;
                nm_len = n00b_qpack_static_table[name_idx].name_len;
            } else {
                /* Dynamic relative index from the post-base (most-recent) end. */
                n00b_data_write_lock(dec->dyn.lock);
                if (name_idx >= dec->dyn.count) {
                    n00b_data_unlock(dec->dyn.lock);
                    goto bad;
                }
                /* Newest entry has insert_id = insert_count - 1; relative
                 * index 0 = newest. */
                uint64_t abs_id = dec->dyn.insert_count - 1 - name_idx;
                size_t   old_first = (dec->dyn.head + dec->dyn.entries_cap
                                       - dec->dyn.count)
                                      % dec->dyn.entries_cap;
                size_t   slot = (old_first + (size_t)(abs_id - dec->dyn.first_insert_id))
                                 % dec->dyn.entries_cap;
                nm     = dec->dyn.entries[slot].name;
                nm_len = dec->dyn.entries[slot].name_len;
                n00b_data_unlock(dec->dyn.lock);
            }
            if (!n00b_qpack_dyn_insert(&dec->dyn, nm, nm_len, value, value_len)) {
                goto bad;
            }
            dec->unreported_inserts++;
        } else if ((b & N00B_QPACK_ES_INSERT_LITERAL_MASK)
                   == N00B_QPACK_ES_INSERT_LITERAL) {
            /* Insert with Literal Name. prefix_bits-on-name = 6 (H+5). */
            uint8_t  hbit_n;
            uint8_t *name;
            size_t   name_len;
            int64_t  nn = n00b_qpack_string_decode(data + off,
                                                   data_len - off,
                                                   6, &hbit_n,
                                                   &name, &name_len);
            if (nn < 0) goto bad;
            if (nn == 0) break;
            off += (size_t)nn;

            uint8_t  hbit_v;
            uint8_t *value;
            size_t   value_len;
            int64_t  vn = n00b_qpack_string_decode(data + off,
                                                   data_len - off,
                                                   8, &hbit_v,
                                                   &value, &value_len);
            if (vn < 0) goto bad;
            if (vn == 0) { off = start; break; }
            off += (size_t)vn;

            if (!n00b_qpack_dyn_insert(&dec->dyn, name, name_len, value, value_len)) {
                goto bad;
            }
            dec->unreported_inserts++;
        } else if ((b & N00B_QPACK_ES_SET_CAPACITY_MASK)
                   == N00B_QPACK_ES_SET_CAPACITY) {
            uint64_t cap;
            int64_t  pn = n00b_qpack_prefix_int_decode(data + off,
                                                       data_len - off,
                                                       5, &cap);
            if (pn < 0) goto bad;
            if (pn == 0) break;
            if (!n00b_qpack_dyn_set_capacity(&dec->dyn, cap)) goto bad;
            off += (size_t)pn;
        } else {
            /* Duplicate (000xxxxx). */
            uint64_t rel;
            int64_t  pn = n00b_qpack_prefix_int_decode(data + off,
                                                       data_len - off,
                                                       5, &rel);
            if (pn < 0) goto bad;
            if (pn == 0) break;
            off += (size_t)pn;

            n00b_data_write_lock(dec->dyn.lock);
            if (rel >= dec->dyn.count) {
                n00b_data_unlock(dec->dyn.lock);
                goto bad;
            }
            uint64_t abs_id = dec->dyn.insert_count - 1 - rel;
            size_t   old_first = (dec->dyn.head + dec->dyn.entries_cap
                                   - dec->dyn.count)
                                  % dec->dyn.entries_cap;
            size_t   slot = (old_first + (size_t)(abs_id - dec->dyn.first_insert_id))
                             % dec->dyn.entries_cap;
            n00b_qpack_dyn_entry_t e = dec->dyn.entries[slot];
            n00b_data_unlock(dec->dyn.lock);

            if (!n00b_qpack_dyn_insert(&dec->dyn, e.name, e.name_len,
                                       e.value, e.value_len)) {
                goto bad;
            }
            dec->unreported_inserts++;
        }
    }

    /* Emit Insert Count Increment if any inserts happened and the
     * caller asked for output. */
    if (out_decoder_stream && dec->unreported_inserts > 0) {
        n00b_qpack_prefix_int_encode(out_decoder_stream,
                                     N00B_QPACK_DS_INSERT_COUNT_INC, 6,
                                     dec->unreported_inserts);
        dec->unreported_inserts = 0;
    }

    n00b_data_unlock(dec->lock);
    return n00b_result_ok(size_t, off);

bad:
    n00b_data_unlock(dec->lock);
    return n00b_result_err(size_t, N00B_QUIC_ERR_PROTOCOL);
}

/* ===========================================================================
 * Field-section decoder
 * =========================================================================== */

/* Decode the section prefix per RFC 9204 § 4.5.1.
 * Computes:
 *   - required_insert_count (decoded form)
 *   - base (= required + delta_base if S=0, required - delta_base - 1 if S=1)
 */
static int64_t
decode_section_prefix(const uint8_t *in, size_t in_len,
                      uint64_t       total_inserted,
                      uint64_t       max_table_capacity,
                      uint64_t      *out_required,
                      uint64_t      *out_base,
                      bool          *out_has_dyn_refs)
{
    uint64_t encoded_ric;
    int64_t  pn = n00b_qpack_prefix_int_decode(in, in_len, 8, &encoded_ric);
    if (pn <= 0) return pn;
    /* RFC 9204 § 4.5.1.1: decode EncodedRIC → RIC via the
     * modular-arithmetic algorithm.  EncodedRIC = 0 always means
     * RIC = 0 (no dynamic references in this section).  Otherwise:
     *
     *   MaxEntries = floor(MaxTableCapacity / 32)
     *   FullRange  = 2 * MaxEntries
     *   MaxValue   = total_inserted + MaxEntries
     *   MaxWrapped = floor(MaxValue / FullRange) * FullRange
     *   RIC        = MaxWrapped + EncodedRIC - 1
     *   if RIC > MaxValue:
     *       if RIC <= FullRange: protocol error
     *       RIC -= FullRange
     *
     * Without this we'd just compute `RIC = EncodedRIC - 1`, which is
     * correct only for encoders that never wrap (i.e., RIC ≤ MaxEntries
     * throughout the connection).  Real-world encoders (h2o, nghttp3)
     * wrap once their dynamic table fills, so the simple inverse
     * yields a wrong RIC and we'd reject valid field sections.  This
     * is the #190 fix. */
    uint64_t ric;
    if (encoded_ric == 0) {
        ric = 0;
    } else {
        uint64_t max_entries = max_table_capacity / 32;
        if (max_entries == 0) {
            /* No dynamic table → can't have dyn refs. */
            return -1;
        }
        uint64_t full_range  = 2 * max_entries;
        if (encoded_ric > full_range) {
            return -1;
        }
        uint64_t max_value   = total_inserted + max_entries;
        uint64_t max_wrapped = (max_value / full_range) * full_range;
        ric                  = max_wrapped + encoded_ric - 1;
        if (ric > max_value) {
            if (ric <= full_range) {
                return -1;
            }
            ric -= full_range;
        }
    }
    if (ric > total_inserted) {
        /* Reference to entries we haven't received yet; would normally
         * block.  For simplicity we treat as protocol error; the
         * blocking-streams story arrives with the H3 stack. */
        return -1;
    }

    uint64_t db;
    bool     s_neg = (in[pn] & 0x80u) != 0;
    int64_t  bn = n00b_qpack_prefix_int_decode(in + pn, in_len - pn, 7, &db);
    if (bn <= 0) return bn;

    uint64_t base;
    if (!s_neg) {
        base = ric + db;
    } else {
        if (db + 1 > ric) return -1;
        base = ric - db - 1;
    }
    if (out_required) *out_required = ric;
    if (out_base) *out_base = base;
    if (out_has_dyn_refs) *out_has_dyn_refs = (ric > 0);
    return pn + bn;
}

/* Resolve a dynamic-table reference relative to base. */
static bool
dyn_lookup_relative(n00b_qpack_decoder_t *dec, uint64_t base, uint64_t rel,
                    n00b_qpack_dyn_entry_t *out)
{
    if (rel + 1 > base) return false;
    uint64_t abs_id = base - 1 - rel;
    return n00b_qpack_dyn_get_by_id(&dec->dyn, abs_id, out);
}

/* Resolve a post-base reference. */
static bool
dyn_lookup_postbase(n00b_qpack_decoder_t *dec, uint64_t base, uint64_t rel,
                    n00b_qpack_dyn_entry_t *out)
{
    uint64_t abs_id = base + rel;
    return n00b_qpack_dyn_get_by_id(&dec->dyn, abs_id, out);
}

n00b_result_t(bool)
n00b_qpack_decode(n00b_qpack_decoder_t *dec,
                  uint64_t              stream_id,
                  const uint8_t        *data,
                  size_t                data_len,
                  n00b_qpack_field_t   *out_fields,
                  size_t                out_fields_cap,
                  size_t               *out_n_fields,
                  n00b_buffer_t        *out_decoder_stream)
{
    if (!dec || !data || !out_fields || !out_n_fields) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    }
    *out_n_fields = 0;

    n00b_data_write_lock(dec->lock);

    n00b_data_write_lock(dec->dyn.lock);
    uint64_t total_inserted     = dec->dyn.insert_count;
    uint64_t max_table_capacity = dec->dyn.capacity;
    n00b_data_unlock(dec->dyn.lock);

    uint64_t required = 0;
    uint64_t base = 0;
    bool     has_dyn = false;
    int64_t  prn = decode_section_prefix(data, data_len, total_inserted,
                                         max_table_capacity,
                                         &required, &base, &has_dyn);
    if (prn < 0) {
        n00b_data_unlock(dec->lock);
        return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
    }
    if (prn == 0) {
        n00b_data_unlock(dec->lock);
        return n00b_result_err(bool, N00B_QUIC_ERR_NEED_MORE_DATA);
    }

    if (required > total_inserted) {
        n00b_data_unlock(dec->lock);
        return n00b_result_err(bool, N00B_QUIC_ERR_NEED_MORE_DATA);
    }

    size_t off = (size_t)prn;
    size_t n_out = 0;

    while (off < data_len) {
        if (n_out >= out_fields_cap) {
            n00b_data_unlock(dec->lock);
            return n00b_result_err(bool, N00B_QUIC_ERR_FRAME_TOO_LARGE);
        }
        uint8_t b = data[off];

        if ((b & N00B_QPACK_FL_INDEXED_MASK) == N00B_QPACK_FL_INDEXED) {
            /* Indexed: 1Txxxxxx, 6 bits for index. */
            bool t_static = (b & N00B_QPACK_FL_INDEXED_T) != 0;
            uint64_t idx;
            int64_t pn = n00b_qpack_prefix_int_decode(data + off,
                                                      data_len - off,
                                                      6, &idx);
            if (pn <= 0) goto bad;
            off += (size_t)pn;

            n00b_qpack_field_t f = {0};
            if (t_static) {
                if (idx >= N00B_QPACK_STATIC_TABLE_SIZE) goto bad;
                f.name      = (const uint8_t *)n00b_qpack_static_table[idx].name;
                f.name_len  = n00b_qpack_static_table[idx].name_len;
                f.value     = (const uint8_t *)n00b_qpack_static_table[idx].value;
                f.value_len = n00b_qpack_static_table[idx].value_len;
            } else {
                n00b_qpack_dyn_entry_t e;
                if (!dyn_lookup_relative(dec, base, idx, &e)) goto bad;
                f.name      = e.name;
                f.name_len  = e.name_len;
                f.value     = e.value;
                f.value_len = e.value_len;
            }
            out_fields[n_out++] = f;
        } else if ((b & N00B_QPACK_FL_LIT_NAMEREF_MASK)
                   == N00B_QPACK_FL_LIT_NAMEREF) {
            /* 01NTxxxx — 4 bits for name index. */
            bool     t_static = (b & N00B_QPACK_FL_LIT_NAMEREF_T) != 0;
            uint64_t idx;
            int64_t  pn = n00b_qpack_prefix_int_decode(data + off,
                                                       data_len - off,
                                                       4, &idx);
            if (pn <= 0) goto bad;
            off += (size_t)pn;

            uint8_t  hbit;
            uint8_t *value;
            size_t   value_len;
            int64_t  vn = n00b_qpack_string_decode(data + off,
                                                   data_len - off,
                                                   8, &hbit,
                                                   &value, &value_len);
            if (vn <= 0) goto bad;
            off += (size_t)vn;

            n00b_qpack_field_t f = {0};
            if (t_static) {
                if (idx >= N00B_QPACK_STATIC_TABLE_SIZE) goto bad;
                f.name     = (const uint8_t *)n00b_qpack_static_table[idx].name;
                f.name_len = n00b_qpack_static_table[idx].name_len;
            } else {
                n00b_qpack_dyn_entry_t e;
                if (!dyn_lookup_relative(dec, base, idx, &e)) goto bad;
                f.name     = e.name;
                f.name_len = e.name_len;
            }
            f.value     = value;
            f.value_len = value_len;
            out_fields[n_out++] = f;
        } else if ((b & N00B_QPACK_FL_LIT_LITERAL_MASK)
                   == N00B_QPACK_FL_LIT_LITERAL) {
            /* 001Nxxxx — name string with H + 3-bit-length prefix. */
            uint8_t  hbit_n;
            uint8_t *name;
            size_t   name_len;
            int64_t  nn = n00b_qpack_string_decode(data + off,
                                                   data_len - off,
                                                   4, &hbit_n,
                                                   &name, &name_len);
            if (nn <= 0) goto bad;
            off += (size_t)nn;

            uint8_t  hbit_v;
            uint8_t *value;
            size_t   value_len;
            int64_t  vn = n00b_qpack_string_decode(data + off,
                                                   data_len - off,
                                                   8, &hbit_v,
                                                   &value, &value_len);
            if (vn <= 0) goto bad;
            off += (size_t)vn;

            n00b_qpack_field_t f = {0};
            f.name      = name;
            f.name_len  = name_len;
            f.value     = value;
            f.value_len = value_len;
            out_fields[n_out++] = f;
        } else if ((b & N00B_QPACK_FL_INDEXED_POSTBASE_MASK)
                   == N00B_QPACK_FL_INDEXED_POSTBASE) {
            /* 0001xxxx — post-base indexed, 4 bits. */
            uint64_t rel;
            int64_t  pn = n00b_qpack_prefix_int_decode(data + off,
                                                       data_len - off,
                                                       4, &rel);
            if (pn <= 0) goto bad;
            off += (size_t)pn;

            n00b_qpack_dyn_entry_t e;
            if (!dyn_lookup_postbase(dec, base, rel, &e)) goto bad;
            n00b_qpack_field_t f = {
                .name = e.name, .name_len = e.name_len,
                .value = e.value, .value_len = e.value_len,
            };
            out_fields[n_out++] = f;
        } else {
            /* 0000Nxxx — post-base literal w/ name ref.  3 bits for index. */
            uint64_t rel;
            int64_t  pn = n00b_qpack_prefix_int_decode(data + off,
                                                       data_len - off,
                                                       3, &rel);
            if (pn <= 0) goto bad;
            off += (size_t)pn;

            uint8_t  hbit_v;
            uint8_t *value;
            size_t   value_len;
            int64_t  vn = n00b_qpack_string_decode(data + off,
                                                   data_len - off,
                                                   8, &hbit_v,
                                                   &value, &value_len);
            if (vn <= 0) goto bad;
            off += (size_t)vn;

            n00b_qpack_dyn_entry_t e;
            if (!dyn_lookup_postbase(dec, base, rel, &e)) goto bad;
            n00b_qpack_field_t f = {
                .name = e.name, .name_len = e.name_len,
                .value = value, .value_len = value_len,
            };
            out_fields[n_out++] = f;
        }
    }

    *out_n_fields = n_out;

    /* Emit a Section Acknowledgment iff this section referenced
     * dynamic entries. */
    if (out_decoder_stream && has_dyn) {
        n00b_qpack_prefix_int_encode(out_decoder_stream,
                                     N00B_QPACK_DS_SECTION_ACK, 7,
                                     stream_id);
    }
    /* Also flush any unreported inserts as an Insert Count Increment.
     * (Real H3 stacks tend to keep these separate to coalesce.) */
    if (out_decoder_stream && dec->unreported_inserts > 0) {
        n00b_qpack_prefix_int_encode(out_decoder_stream,
                                     N00B_QPACK_DS_INSERT_COUNT_INC, 6,
                                     dec->unreported_inserts);
        dec->unreported_inserts = 0;
    }

    n00b_data_unlock(dec->lock);
    return n00b_result_ok(bool, true);

bad:
    n00b_data_unlock(dec->lock);
    return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
}
