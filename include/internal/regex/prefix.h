/**
 * @file prefix.h
 * @brief Forward / reverse literal-prefix extraction for the regex engine.
 *
 * Faithful per-file translation of upstream Rust resharp `prefix`, with
 * primitives translated to n00b idioms (allocate-by-type, typed dicts,
 * `n00b_result_t(T)` for fallible returns).  This file is internal to
 * the regex engine; nothing here is part of the public n00b surface.
 *
 * Algorithmic names (`PrefixSet`, `PrefixSets`, `PrefixKind`,
 * `calc_prefix_sets`, `select_prefix`, `try_rev_prefix`, …) stay
 * un-prefixed because they form the regex algorithmic vocabulary that
 * tracks upstream Rust closely.
 *
 * Companion source: `src/text/regex/engine/prefix.c`.
 *
 * Cross-file dependencies:
 *   - algebra.h  (Phase 5)  — `RegexBuilder`, `NodeId`, `Nullability`.
 *   - solver.h   (Phase 5)  — `Solver`, `TSet`, `TSetId`, `ByteRange`.
 *   - nulls.h    (Phase 5)  — `NULLS_ID_*` sentinels, `Nullability` consts.
 *   - accel.h    (Phase 7)  — `FwdPrefixSearch`, `FwdLiteralSearch`,
 *                              `FwdRangeSearch`, `RevTeddySearch` opaques.
 *   - ids.h      (Phase 3)  — `NodeId`, `TSetId` newtypes.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "adt/result.h"
#include "internal/regex/accel.h"
#include "internal/regex/algebra.h"
#include "internal/regex/ids.h"
#include "internal/regex/nulls.h"
#include "internal/regex/solver.h"

// ---------------------------------------------------------------------------
// Owning growable arrays used by the prefix module.  Caller-managed
// (zero-init the struct, free `data` via `n00b_free`).  No locking —
// every consumer is single-owner / single-thread within one regex build.
// ---------------------------------------------------------------------------

/** @brief Caller-managed growable array of `TSetId` (was `Vec<TSetId>`). */
typedef struct TSetIdVec {
    TSetId *data;
    size_t  len;
    size_t  cap;
} TSetIdVec;

/** @brief Caller-managed growable byte buffer (was `Vec<u8>`). */
typedef struct ByteVec {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} ByteVec;

// ---------------------------------------------------------------------------
// Direction enum (Rust `enum Direction`).
// ---------------------------------------------------------------------------

/** @brief Scan direction — used by the cost model. */
typedef enum : uint8_t {
    DIRECTION_FWD = 0,
    DIRECTION_REV = 1,
} Direction;

// ---------------------------------------------------------------------------
// Body-shape classifier output.
// ---------------------------------------------------------------------------

/** @brief Classification of a regex body's tail shape (cost-model input). */
typedef enum : uint8_t {
    NODE_SHAPE_TRAILING_STAR = 0,
    NODE_SHAPE_BOUNDED       = 1,
    NODE_SHAPE_UNBOUNDED     = 2,
} NodeShape;

// ---------------------------------------------------------------------------
// Cost-model constants exposed for cross-TU reference.
// ---------------------------------------------------------------------------

/** @brief Frequency threshold below which a prefix is rare enough to use as a skip. */
constexpr uint32_t SKIP_FREQ_THRESHOLD = 75000u;

/** @brief Sum of `n00b_simd_BYTE_FREQ[0..256]` in the corpus. */
constexpr uint64_t TOTAL_BYTE_FREQ     = 252052ull;

// ---------------------------------------------------------------------------
// External tables / predicates owned by the SIMD subtree (Phase 8/10).
// Declared here as `extern` so engine TUs can reference them without
// depending on a SIMD-private header.
// ---------------------------------------------------------------------------

extern const uint16_t n00b_simd_BYTE_FREQ[256];
extern bool           n00b_simd_has_simd(void);

// ---------------------------------------------------------------------------
// PrefixSet / PrefixSets aggregates.
// ---------------------------------------------------------------------------

/** @brief One prefix candidate: a sequence of `TSetId`s and its scan-cost. */
typedef struct PrefixSet {
    TSetIdVec sets;
    uint64_t  cost;        /**< `UINT64_MAX` for empty / unset. */
} PrefixSet;

/** @brief Grouped prefix-extraction outputs for a node. */
typedef struct PrefixSets {
    PrefixSet fwd_potential;
    PrefixSet fwd_potential_stripped;
    PrefixSet rev_anchored;
    PrefixSet rev_potential;
} PrefixSets;

// ---------------------------------------------------------------------------
// PrefixKind — tagged union (Rust enum carrying an optional `FwdPrefixSearch`).
// ---------------------------------------------------------------------------

/** @brief Discriminator tag for `PrefixKind`. */
typedef enum : uint8_t {
    PREFIX_KIND_ANCHORED_REV     = 0,
    PREFIX_KIND_ANCHORED_FWD     = 1,
    PREFIX_KIND_ANCHORED_FWD_LB  = 2,
    PREFIX_KIND_POTENTIAL_START  = 3,
} PrefixKindTag;

/**
 * @brief Selected prefix kind plus optional forward-search payload.
 *
 * The `fwd` field is populated for the three forward-bearing variants
 * (`ANCHORED_FWD`, `ANCHORED_FWD_LB`); ownership matches the upstream
 * Rust enum carrier.
 */
typedef struct PrefixKind {
    PrefixKindTag    tag;
    FwdPrefixSearch *fwd;
} PrefixKind;

[[nodiscard]] PrefixKindTag    prefix_kind_tag(const PrefixKind *self);
[[nodiscard]] FwdPrefixSearch *prefix_kind_fwd_search(const PrefixKind *self);
[[nodiscard]] bool             prefix_kind_is_fwd(const PrefixKind *self);
[[nodiscard]] bool             prefix_kind_is_rev(const PrefixKind *self);

/**
 * @brief Drop the heap-allocated `PrefixKind` wrapper.
 *
 * The inner `fwd` pointer is borrowed-vs-owned per builder semantics —
 * this disposer frees only the wrapper.
 */
void prefix_kind_free(PrefixKind *self);

/**
 * @brief Drop a `RevTeddySearch` returned via the prefix-select path.
 *
 * Used when ownership has not been transferred to an LDFA.
 */
void prefix_rev_skip_free(void *rev_skip);

// ---------------------------------------------------------------------------
// Composite payload + result types.
// ---------------------------------------------------------------------------

/**
 * @brief Optional `FwdPrefixSearch *` (the upstream `Result<Option<…>, E>` ok arm).
 *
 * `has_value == false` means the search was not built (no useful prefix);
 * `value` is then nullptr.
 */
typedef struct OptFwdPrefix {
    bool             has_value;
    FwdPrefixSearch *value;
} OptFwdPrefix;

/**
 * @brief Optional rev-prefix selection: kind + Teddy search payload.
 *
 * `has_value == false` means no rev prefix was selected.
 */
typedef struct OptPrefixRev {
    bool            has_value;
    PrefixKind      kind;
    RevTeddySearch *search;
} OptPrefixRev;

/**
 * @brief Composite output of `select_prefix` (kind and/or skip).
 *
 * Each of `has_kind` and `has_skip` is independent — the upstream
 * `Result<(Option<PrefixKind>, Option<RevTeddySearch>), E>` ok arm.
 */
typedef struct PrefixSelect {
    bool            has_kind;
    PrefixKind      kind;
    bool            has_skip;
    RevTeddySearch *skip;
} PrefixSelect;

// ---------------------------------------------------------------------------
// Public API (mirrors upstream `pub` / `pub(crate)` Rust functions).
//
// Every fallible call returns `n00b_result_t(T)`.  The err side carries
// an `n00b_regex_algebra_err_t` value cast to `int` (per § 7.5, D14).
// ---------------------------------------------------------------------------

/**
 * @brief Compute a prefix-set vector starting at @p start.
 *
 * The caller takes ownership of the returned vector's `data` buffer and
 * frees it with `n00b_free`.  When @p strip_prefix is true, prefix-safe
 * stripping is applied to the start node before traversal.
 */
n00b_result_t(TSetIdVec)
    calc_prefix_sets_inner(RegexBuilder *self, NodeId start, bool strip_prefix);

/** @brief Convenience wrapper: `nonbegins` + `strip_prefix_safe` then inner. */
n00b_result_t(TSetIdVec)
    calc_prefix_sets(RegexBuilder *self, NodeId rev_start);

/** @brief Prune-and-strip wrapper around `calc_potential_start`. */
n00b_result_t(TSetIdVec)
    calc_potential_start_prune(RegexBuilder *self,
                               NodeId        node,
                               size_t        max_prefix_len,
                               size_t        max_frontier_size,
                               bool          exclude_initial);

/**
 * @brief BFS-style potential-start derivation across @p initial_node.
 *
 * Walks the derivative graph, reducing the alphabet at each step into
 * `union_set`, and stops on any non-trivial bound (frontier too wide,
 * prefix length cap reached, nullable encountered, no progress).  The
 * returned vector contains one set per accepted step.
 */
n00b_result_t(TSetIdVec)
    calc_potential_start(RegexBuilder *self,
                         NodeId        initial_node,
                         size_t        max_prefix_len,
                         size_t        max_frontier_size,
                         bool          exclude_initial);

/**
 * @brief Compute the four `PrefixSet` candidates plus their costs.
 *
 * In-place by-value variant used internally by `select_prefix`.  Each
 * candidate's `sets.data` is owned by the returned struct; release via
 * the helpers below or by freeing each `sets.data` directly.
 */
n00b_result_t(PrefixSets)
    prefix_sets_compute_internal(RegexBuilder *self, NodeId node, NodeId rev_start);

/** @brief Lowest per-set frequency sum (`UINT64_MAX` if empty). */
[[nodiscard]] uint64_t prefix_sets_rarity(RegexBuilder *self,
                                          const TSetId *sets, size_t sets_len);

// ---------------------------------------------------------------------------
// Test-facing API: heap-allocated `PrefixSets` + accessors.
//
// `prefix_sets_compute` returns nullptr on error and writes the err code
// to `*err_out` (when non-nullptr).  On success returns a heap struct
// the caller releases with `prefix_sets_free`.  Accessor functions copy
// the underlying `TSetIdVec` into a freshly allocated buffer that the
// caller releases with `n00b_free`.
// ---------------------------------------------------------------------------

[[nodiscard]] PrefixSets *prefix_sets_compute(RegexBuilder *self,
                                              NodeId        node,
                                              NodeId        rev_start,
                                              int          *err_out);
void prefix_sets_free(PrefixSets *self);
void prefix_sets_rev_anchored (const PrefixSets *self, TSetId **out, size_t *out_len);
void prefix_sets_rev_potential(const PrefixSets *self, TSetId **out, size_t *out_len);
void prefix_sets_fwd_potential(const PrefixSets *self, TSetId **out, size_t *out_len);

// ---------------------------------------------------------------------------
// Forward / reverse search builders.
// ---------------------------------------------------------------------------

/** @brief Build a `LITERAL` `FwdPrefixSearch` from a strict-literal prefix. */
n00b_result_t(OptFwdPrefix)
    build_strict_literal_prefix(RegexBuilder *self, NodeId node);

/** @brief Build the best forward-prefix search for @p node (literal/Teddy/range). */
n00b_result_t(OptFwdPrefix)
    build_fwd_prefix(RegexBuilder *self, NodeId node);

/**
 * @brief Build a reverse-Teddy search across @p sets, or nullptr when
 *        the cost model rejects the candidate.
 *
 * Caller takes ownership of the returned `RevTeddySearch *` (or
 * receives nullptr).  No allocation occurs on the rejection path.
 */
[[nodiscard]] RevTeddySearch *build_rev_prefix_search(RegexBuilder *self,
                                                     const TSetId *sets,
                                                     size_t        sets_len);

/** @brief Try to build a rev-prefix kind + Teddy search for @p rev_node. */
n00b_result_t(OptPrefixRev)
    try_rev_prefix(RegexBuilder *self, NodeId rev_node);

/**
 * @brief Top-level prefix-selection entry point.
 *
 * Computes both forward and reverse prefix candidates, applies the cost
 * model, and returns the winner (kind and/or rev-skip).  When SIMD is
 * unavailable, returns an empty selection.  @p max_cap is reserved for
 * the upstream `convergence_prefix` cargo-feature path (currently unused).
 */
n00b_result_t(PrefixSelect)
    select_prefix(RegexBuilder *self,
                  NodeId        node,
                  NodeId        rev_start,
                  bool          has_look,
                  uint32_t      min_len,
                  size_t        max_cap,
                  bool          no_fwd_prefix);

// ---------------------------------------------------------------------------
// Engine.c-facing aliases / accessors (mirror resharp-c's "phase-2"
// translator-invented names — kept for ABI parity with the engine TU
// that has not yet been ported).  Returns `nullptr` on success and an
// allocated err-code-bearing pointer otherwise; callers cast.
// ---------------------------------------------------------------------------

/**
 * @brief Adapter for the engine's expected ABI.
 *
 * Returns nullptr on success and writes the selected prefix + rev-skip
 * into `*out`.  Returns a non-nullptr `int *` carrying the err code on
 * failure; caller frees with `n00b_free`.
 */
void *prefix_select_prefix(RegexBuilder *self,
                           NodeId        node,
                           NodeId        ts_rev_start,
                           bool          has_look,
                           uint32_t      min_len,
                           size_t        max_cap,
                           bool          no_fwd_prefix,
                           PrefixSelect *out);

extern const uint32_t prefix_SKIP_FREQ_THRESHOLD;

/** @brief Phase-2 alias: `calc_potential_start` re-exported under a stable name. */
n00b_result_t(TSetIdVec)
    prefix_calc_potential_start(RegexBuilder *self,
                                NodeId        initial_node,
                                size_t        max_prefix_len,
                                size_t        max_frontier_size,
                                bool          exclude_initial);

/** @brief Phase-2 alias: `calc_prefix_sets_inner` re-exported under a stable name. */
n00b_result_t(TSetIdVec)
    prefix_calc_prefix_sets_inner(RegexBuilder *self, NodeId start, bool strip_prefix);
