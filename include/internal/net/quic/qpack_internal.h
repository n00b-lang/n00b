/**
 * @file qpack_internal.h
 * @brief Internal QPACK structures shared across the encoder /
 *        decoder / static / huffman translation units.
 *
 * Not part of the public API.  Anything in here may change without
 * notice.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "n00b.h"
#include "core/buffer.h"
#include "core/data_lock.h"
#include "net/quic/qpack.h"
#include "net/quic/quic_types.h"

/* ===========================================================================
 * Encoder-stream / decoder-stream / field-section opcodes
 *
 * Captured here as named constants to keep the encoder + decoder
 * source files honest about which RFC 9204 § 4.x section they're
 * implementing.
 * =========================================================================== */

/* Encoder stream — RFC 9204 § 4.3 */
#define N00B_QPACK_ES_INSERT_NAMEREF      ((uint8_t)0x80u)  /* 1xxxxxxx */
#define N00B_QPACK_ES_INSERT_NAMEREF_MASK ((uint8_t)0x80u)
#define N00B_QPACK_ES_INSERT_LITERAL      ((uint8_t)0x40u)  /* 01xxxxxx */
#define N00B_QPACK_ES_INSERT_LITERAL_MASK ((uint8_t)0xc0u)
#define N00B_QPACK_ES_SET_CAPACITY        ((uint8_t)0x20u)  /* 001xxxxx */
#define N00B_QPACK_ES_SET_CAPACITY_MASK   ((uint8_t)0xe0u)
#define N00B_QPACK_ES_DUPLICATE           ((uint8_t)0x00u)  /* 000xxxxx */
#define N00B_QPACK_ES_DUPLICATE_MASK      ((uint8_t)0xe0u)

/* Decoder stream — RFC 9204 § 4.4 */
#define N00B_QPACK_DS_SECTION_ACK         ((uint8_t)0x80u)  /* 1xxxxxxx */
#define N00B_QPACK_DS_SECTION_ACK_MASK    ((uint8_t)0x80u)
#define N00B_QPACK_DS_STREAM_CANCEL       ((uint8_t)0x40u)  /* 01xxxxxx */
#define N00B_QPACK_DS_STREAM_CANCEL_MASK  ((uint8_t)0xc0u)
#define N00B_QPACK_DS_INSERT_COUNT_INC    ((uint8_t)0x00u)  /* 00xxxxxx */
#define N00B_QPACK_DS_INSERT_COUNT_INC_MASK ((uint8_t)0xc0u)

/* Field-section field-line opcodes — RFC 9204 § 4.5 */
#define N00B_QPACK_FL_INDEXED              ((uint8_t)0x80u) /* 1Txxxxxx */
#define N00B_QPACK_FL_INDEXED_MASK         ((uint8_t)0x80u)
#define N00B_QPACK_FL_INDEXED_T            ((uint8_t)0x40u) /* T-bit: static */
#define N00B_QPACK_FL_LIT_NAMEREF          ((uint8_t)0x40u) /* 01NTxxxx */
#define N00B_QPACK_FL_LIT_NAMEREF_MASK     ((uint8_t)0xc0u)
#define N00B_QPACK_FL_LIT_NAMEREF_N        ((uint8_t)0x20u) /* never-index */
#define N00B_QPACK_FL_LIT_NAMEREF_T        ((uint8_t)0x10u) /* static */
#define N00B_QPACK_FL_LIT_LITERAL          ((uint8_t)0x20u) /* 001Nxxxx */
#define N00B_QPACK_FL_LIT_LITERAL_MASK     ((uint8_t)0xe0u)
#define N00B_QPACK_FL_LIT_LITERAL_N        ((uint8_t)0x10u)
#define N00B_QPACK_FL_INDEXED_POSTBASE     ((uint8_t)0x10u) /* 0001xxxx */
#define N00B_QPACK_FL_INDEXED_POSTBASE_MASK ((uint8_t)0xf0u)
#define N00B_QPACK_FL_LIT_POSTBASE         ((uint8_t)0x00u) /* 0000Nxxx */
#define N00B_QPACK_FL_LIT_POSTBASE_MASK    ((uint8_t)0xf0u)
#define N00B_QPACK_FL_LIT_POSTBASE_N       ((uint8_t)0x08u)

/* Huffman bit on string literal first byte (the H bit). */
#define N00B_QPACK_H_BIT                   ((uint8_t)0x80u)

/* ===========================================================================
 * Static table (RFC 9204 Appendix A)
 * =========================================================================== */

typedef struct {
    const char *name;
    size_t      name_len;
    const char *value;       /* may be empty for "name-only" entries */
    size_t      value_len;
} n00b_qpack_static_entry_t;

extern const n00b_qpack_static_entry_t
    n00b_qpack_static_table[N00B_QPACK_STATIC_TABLE_SIZE];

/* ===========================================================================
 * Huffman tables (RFC 7541 Appendix B)
 *
 * Encoder side: per symbol, the code (≤30 bits) and length.  The
 * EOS symbol (256) is never emitted; padding is the high bits of
 * EOS' code (0x3fffffff).
 *
 * Decoder side: a state-machine table generated from the codes
 * (8-bit-at-a-time stride for a manageable size).
 * =========================================================================== */

typedef struct {
    uint32_t code;     /* right-aligned bits */
    uint8_t  bits;     /* number of valid bits (1..30) */
} n00b_qpack_huffman_sym_t;

extern const n00b_qpack_huffman_sym_t n00b_qpack_huffman_table[257];

/* ===========================================================================
 * Dynamic table — shared between encoder/decoder modules.
 *
 * Entries are stored in insertion order; the absolute insertion id
 * of an entry is `base_insert_count + slot_index` where slot_index
 * is the entry's position in the ring.  Eviction removes the
 * oldest entry.
 *
 * Each entry's "RFC size" (per § 3.2.1) is name_len + value_len + 32.
 * =========================================================================== */

typedef struct {
    uint8_t *name;
    size_t   name_len;
    uint8_t *value;
    size_t   value_len;
    uint64_t insert_id;   /* absolute insertion id */
} n00b_qpack_dyn_entry_t;

typedef struct {
    n00b_rwlock_t         *lock;
    n00b_qpack_dyn_entry_t *entries;     /* ring; size_t entries_cap */
    size_t                  entries_cap;
    size_t                  head;         /* next write slot */
    size_t                  count;        /* live entries */
    uint64_t                insert_count; /* total inserted lifetime */
    uint64_t                first_insert_id; /* head of FIFO */
    uint64_t                capacity;     /* RFC bytes; advertised cap */
    uint64_t                max_capacity; /* hard ceiling */
    uint64_t                used;         /* bytes occupied */
} n00b_qpack_dyn_table_t;

/* Initialize an empty dynamic table with the given capacity ceiling. */
extern void
n00b_qpack_dyn_init(n00b_qpack_dyn_table_t *t, uint64_t max_capacity);

/* Free internal storage. */
extern void
n00b_qpack_dyn_destroy(n00b_qpack_dyn_table_t *t);

/* Try to insert; evicts oldest entries as needed.  Returns false if
 * capacity is too small to hold the entry even after evicting all
 * existing entries.  On success, the entry is duplicated into the
 * dyn table's own conduit-pool storage. */
extern bool
n00b_qpack_dyn_insert(n00b_qpack_dyn_table_t *t,
                      const uint8_t          *name,
                      size_t                  name_len,
                      const uint8_t          *value,
                      size_t                  value_len);

/* Adjust capacity (RFC § 3.2.3); evicts as needed. */
extern bool
n00b_qpack_dyn_set_capacity(n00b_qpack_dyn_table_t *t, uint64_t new_cap);

/* Look up the entry whose absolute insert_id == id.  Returns false
 * if id is outside [first_insert_id, insert_count). */
extern bool
n00b_qpack_dyn_get_by_id(n00b_qpack_dyn_table_t *t,
                         uint64_t                id,
                         n00b_qpack_dyn_entry_t *out);

/* Find an entry matching (name, value).  Returns id and exact-match
 * flag; on an exact match `*out_exact = true`, on a name-only match
 * `*out_exact = false`.  Returns false if no match. */
extern bool
n00b_qpack_dyn_find(n00b_qpack_dyn_table_t *t,
                    const uint8_t          *name,
                    size_t                  name_len,
                    const uint8_t          *value,
                    size_t                  value_len,
                    uint64_t               *out_id,
                    bool                   *out_exact);

/* ===========================================================================
 * Prefix-int (RFC 7541 § 5.1)
 * =========================================================================== */

/**
 * @brief Encode a prefix-int into @p out (incremental append).
 *
 * @param out         Buffer to append to.
 * @param first_byte  Byte that already has the high N bits set; the
 *                    low (8-N) bits are written by this function.
 * @param prefix_bits N — the number of low bits available.
 * @param value       Value to encode.
 * @return            Total bytes written (≥1).
 */
extern size_t
n00b_qpack_prefix_int_encode(n00b_buffer_t *out,
                             uint8_t        first_byte,
                             uint8_t        prefix_bits,
                             uint64_t       value);

/**
 * @brief Decode a prefix-int.
 *
 * @param in          Source bytes.
 * @param in_len      Bytes in @p in.
 * @param prefix_bits N — number of bits in the first byte to use.
 * @param out_value   Decoded value.
 * @return            Bytes consumed (>0) on success;
 *                    0 if the input is truncated;
 *                    negative on malformed input (out_value
 *                    overflowed the 62-bit cap).
 */
extern int64_t
n00b_qpack_prefix_int_decode(const uint8_t *in, size_t in_len,
                             uint8_t        prefix_bits,
                             uint64_t      *out_value);

/* ===========================================================================
 * Encoded string (literal name / value)
 *
 * Wire shape: prefix-int with N bits, top of which is the H bit
 * (Huffman-encoded if set).  Then the bytes — Huffman or raw.
 * =========================================================================== */

extern size_t
n00b_qpack_string_encode(n00b_buffer_t *out,
                         uint8_t        prefix_first_byte,
                         uint8_t        prefix_bits,    /* includes H bit */
                         const uint8_t *src, size_t src_len);

/**
 * @brief Decode an encoded string (with H bit + length).
 *
 * Allocates an output buffer from conduit_pool; on success writes a
 * pointer to it via @p out_str (caller must NOT free; conduit_pool
 * collected with the QPACK handle).
 *
 * @return Bytes consumed on success; 0 if truncated; -1 if malformed.
 */
extern int64_t
n00b_qpack_string_decode(const uint8_t *in, size_t in_len,
                         uint8_t        prefix_bits,
                         uint8_t       *out_h_bit,
                         uint8_t      **out_str,
                         size_t        *out_str_len);

/* ===========================================================================
 * Encoder + decoder structs
 * =========================================================================== */

/* Track per-stream pending acks for the encoder (so it can increment
 * known_received_count when a Section Ack arrives). */
typedef struct n00b_qpack_pending_section {
    uint64_t                              stream_id;
    uint64_t                              required_insert_count;
    struct n00b_qpack_pending_section    *next;
} n00b_qpack_pending_section_t;

struct n00b_qpack_encoder {
    n00b_rwlock_t               *lock;
    n00b_qpack_dyn_table_t       dyn;
    uint64_t                     max_blocked_streams;
    uint64_t                     known_received_count;
    n00b_qpack_pending_section_t *pending_head;
};

struct n00b_qpack_decoder {
    n00b_rwlock_t         *lock;
    n00b_qpack_dyn_table_t dyn;
    uint64_t               max_blocked_streams;
    /* Number of inserts since the last Insert Count Increment we
     * emitted; we batch increments on encoder-stream consume calls. */
    uint64_t               unreported_inserts;
};

/* ===========================================================================
 * Allocator helper — used everywhere in the QPACK module.
 * =========================================================================== */
extern n00b_allocator_t *n00b_qpack_alloc(void);

/**
 * @brief Helper: duplicate bytes into the conduit pool.
 *
 * Returns a freshly-allocated pointer; bytes can be released only
 * by tearing down the conduit_pool (i.e., they outlive any
 * reasonable per-request scope).
 */
extern uint8_t *n00b_qpack_dup(const uint8_t *src, size_t len);
