/**
 * @file include/text/regex/ctx.h
 * @brief Regex subsystem runtime state.
 *
 * Holds the parser-side caches that the resharp-c → n00b port needs to
 * keep alive across regex compiles: the named pairset cache
 * (`\p{Script=Latin}` lookups), the precomputed `\w` table, and the
 * simple-fold equivalence index (case-insensitive matching).
 *
 * All three carry pointers into heap-allocated buffers — they cannot
 * live in static memory, since n00b's GC scans only registered roots
 * (and this bundle is reached via `n00b_runtime_t::regex_ctx`).
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/thread.h"
#include "core/mutex.h"

/**
 * @brief Two-codepoint range, layout-compatible with
 *        `n00b_codepoint_pair_t` (which uses `lo`/`hi`).
 *
 * Kept under `rs_*` because the algorithmic vocabulary of the regex
 * port (mirroring upstream Rust `regex-syntax`) is unprefixed by
 * convention.  Pointer reinterpret with `n00b_codepoint_pair_t` is
 * sound — same two `uint32_t` fields in the same order.
 */
typedef struct rs_uchar_pair_t {
    uint32_t start;
    uint32_t end;
} rs_uchar_pair_t;

/**
 * @brief Named pairset — `(name, ranges, len)` triple as referenced by
 *        `\p{...}` parsing.  `name` is the canonical (interned) form;
 *        `pairs` borrows from n00b unicode (program-lifetime).
 */
typedef struct rs_named_pairset_t {
    const char            *name;
    const rs_uchar_pair_t *pairs;
    size_t                 pairs_len;
} rs_named_pairset_t;

/**
 * @brief Cache entry pairing a strdup'd canonical key with its set.
 */
typedef struct n00b_regex_cached_pairset_t {
    char              *name;     ///< strdup'd canonical key
    rs_named_pairset_t set;
} n00b_regex_cached_pairset_t;

/**
 * @brief Simple-fold equivalence entry: for a given codepoint `key`,
 *        the array of codepoints it fold-matches.
 */
typedef struct n00b_regex_case_fold_entry_t {
    uint32_t  key;
    uint32_t *fold;     ///< owned by the cache
    uint8_t   fold_len;
} n00b_regex_case_fold_entry_t;

struct n00b_regex_ctx_t {
    // -------- Named pairset cache (\p{Script=Latin}, \p{Block=...}) ---------
    n00b_regex_cached_pairset_t *named_cache;
    size_t                       named_cache_len;
    size_t                       named_cache_cap;
    n00b_mutex_t                 named_cache_mutex;
    _Atomic uint32_t             named_cache_mutex_init;

    // -------- Perl word class \w (Alphabetic ∪ Mark ∪ Nd ∪ Pc ∪ Join_Control)
    rs_uchar_pair_t             *perl_word_pairs;
    size_t                       perl_word_pairs_n;
    _Atomic uint32_t             perl_word_init;

    // -------- Simple-fold equivalence (case-insensitive matching) -----------
    n00b_regex_case_fold_entry_t *case_fold_table;
    size_t                        case_fold_table_len;
    _Atomic uint32_t              case_fold_init;
};

typedef struct n00b_regex_ctx_t n00b_regex_ctx_t;
