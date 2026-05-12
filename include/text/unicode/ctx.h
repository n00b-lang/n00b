/**
 * @file include/text/unicode/ctx.h
 * @brief Unicode subsystem runtime state.
 *
 * Holds the Phase 4.5 range-enumeration / by-name / Age / segmentation
 * tables in a single bundle attached to `n00b_runtime_t`.  These tables
 * carry pointers into heap-allocated codepoint-pair backings, so they
 * cannot live in static memory: the n00b GC scans the bundle (via the
 * runtime root) to keep the backings alive.
 *
 * Per-subsystem lazy init: each subsystem has its own atomic state
 * machine and mutex.  First call to a `*_ranges` accessor builds the
 * tables; subsequent calls return cached pointers.
 *
 * The atomic init flags (`*_inited`) and CAS-state words (`*_mutex_state`)
 * are scalars with no pointers; they could in principle live in BSS,
 * but we keep them in the bundle so the entire subsystem's state is
 * in one place.
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// `core/mutex.h` transitively pulls `core/lock_common.h`, which uses
// `n00b_thread_id()` defined in `core/thread.h`.  Include thread.h
// first so this header is self-contained.
#include "core/thread.h"
#include "core/mutex.h"
#include "text/unicode/types.h"

/**
 * @brief One range table for a single property value.
 *
 * `ranges` points into a per-subsystem `backing` buffer (`n00b_alloc_array`-
 * allocated, kept alive by GC root coverage of `n00b_unicode_ctx_t`).
 * `len` is the number of pairs.
 */
typedef struct n00b_unicode_range_slice_t {
    n00b_codepoint_pair_t *ranges;
    size_t                 len;
} n00b_unicode_range_slice_t;

// Number of values per fixed-cardinality property.  Dynamic-cardinality
// properties (script, block, age) use runtime counts (`n00b_unicode_*_count`).
#define N00B_UNICODE_GC_N_VALUES   (N00B_UNICODE_GC_CN + 1)
#define N00B_UNICODE_BIDI_N_VALUES (N00B_UNICODE_BIDI_PDI + 1)
#define N00B_UNICODE_PROP_N_VALUES (N00B_UNICODE_PROP_EXTENDED_PICTOGRAPHIC + 1)

// Number of derived General_Category entries (Letter / Mark / Number /
// Punctuation / Symbol / Separator / Other / Cased_Letter, plus aliases).
// Forward-declared here; the actual table lives in properties.c.
#define N00B_UNICODE_DERIVED_GC_MAX 16

struct n00b_unicode_ctx_t {
    // -------- General_Category (\p{Lu}, \p{Ll}, ...) ------------------------
    n00b_unicode_range_slice_t  gc_slices[N00B_UNICODE_GC_N_VALUES];
    n00b_codepoint_pair_t      *gc_backing;
    n00b_mutex_t                gc_mutex;
    _Atomic int                 gc_mutex_state;
    _Atomic bool                gc_inited;

    // -------- Script (\p{Script=Latin}, ...) --------------------------------
    n00b_unicode_range_slice_t *script_slices;  // [n00b_unicode_script_count]
    n00b_codepoint_pair_t      *script_backing;
    n00b_mutex_t                script_mutex;
    _Atomic int                 script_mutex_state;
    _Atomic bool                script_inited;

    // -------- Script_Extensions (\p{scx=Latn}, ...) -------------------------
    n00b_unicode_range_slice_t *scx_slices;     // [n00b_unicode_script_count]
    n00b_codepoint_pair_t      *scx_backing;
    n00b_mutex_t                scx_mutex;
    _Atomic int                 scx_mutex_state;
    _Atomic bool                scx_inited;

    // -------- Block (\p{Block=BasicLatin}, ...) -----------------------------
    n00b_unicode_range_slice_t *block_slices;   // [n00b_unicode_block_count + 1]
    n00b_codepoint_pair_t      *block_backing;
    n00b_mutex_t                block_mutex;
    _Atomic int                 block_mutex_state;
    _Atomic bool                block_inited;

    // -------- Bidi_Class (\p{bc=L}, ...) ------------------------------------
    n00b_unicode_range_slice_t  bidi_slices[N00B_UNICODE_BIDI_N_VALUES];
    n00b_codepoint_pair_t      *bidi_backing;
    n00b_mutex_t                bidi_mutex;
    _Atomic int                 bidi_mutex_state;
    _Atomic bool                bidi_inited;

    // -------- Binary properties (\p{Alphabetic}, \p{White_Space}, ...) ------
    n00b_unicode_range_slice_t  prop_slices[N00B_UNICODE_PROP_N_VALUES];
    n00b_codepoint_pair_t      *prop_backing;
    n00b_mutex_t                prop_mutex;
    _Atomic int                 prop_mutex_state;
    _Atomic bool                prop_inited;

    // -------- Derived GC (\p{L}, \p{Letter}, \pL, \p{Any}, ...) -------------
    // `derived_gc_cache[i]` is the union range table for `derived_gc_defs[i]`,
    // lazily built on first hit.  `derived_gc_cached[i]` is the per-entry
    // ready flag (built / not built yet).
    n00b_unicode_range_slice_t  derived_gc_cache[N00B_UNICODE_DERIVED_GC_MAX];
    _Atomic bool                derived_gc_cached[N00B_UNICODE_DERIVED_GC_MAX];
    n00b_mutex_t                derived_gc_mutex;
    _Atomic int                 derived_gc_mutex_state;

    // -------- Age (\p{Age=12.0}, ...) ---------------------------------------
    // `age_cache` and `age_cached` are parallel arrays of length
    // `n00b_unicode_age_count`, allocated together on first use.
    n00b_unicode_range_slice_t *age_cache;
    _Atomic bool               *age_cached;
    n00b_mutex_t                age_mutex;
    _Atomic int                 age_mutex_state;
    _Atomic bool                age_init_done;

    // -------- Grapheme_Cluster_Break (\p{gcb=Extend}, ...) ------------------
    n00b_unicode_range_slice_t *gcb_slices;
    n00b_codepoint_pair_t      *gcb_backing;
    _Atomic int                 gcb_state;

    // -------- Word_Break (\p{wb=ALetter}, ...) ------------------------------
    n00b_unicode_range_slice_t *wb_slices;
    n00b_codepoint_pair_t      *wb_backing;
    _Atomic int                 wb_state;

    // -------- Sentence_Break (\p{sb=ATerm}, ...) ----------------------------
    n00b_unicode_range_slice_t *sb_slices;
    n00b_codepoint_pair_t      *sb_backing;
    _Atomic int                 sb_state;

    // -------- Line_Break (\p{lb=SP}, ...) -----------------------------------
    n00b_unicode_range_slice_t *lb_slices;
    n00b_codepoint_pair_t      *lb_backing;
    _Atomic int                 lb_state;
};

typedef struct n00b_unicode_ctx_t n00b_unicode_ctx_t;
