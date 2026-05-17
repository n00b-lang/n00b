/**
 * @file engine.h
 * @brief Lazy DFA engine: minterm partitioning, derivative-driven state
 *        construction, scan / collect kernels.
 *
 * Internal regex-engine header, not part of the public n00b surface.
 * Names track upstream Rust `engine` / resharp-c's `engine.h` closely;
 * the algorithmic vocabulary stays un-prefixed (no `n00b_`) per the
 * regex port convention.
 *
 * The `LDFA` is the lazy on-demand DFA built from a hash-consed regex
 * AST.  Construction populates a static `begin_table[mt]` and a
 * dynamically-grown `center_table[state << mt_log | mt]`; transitions
 * not yet present are filled in by `engine_LDFA_lazy_transition` on
 * first miss.  Optional SIMD `skip_searchers` accelerate hot states
 * whose minterm is sparse.
 *
 * Companion source: `src/text/regex/engine/engine.c`.
 *
 * `Error *` returns from upstream Rust / resharp-c are translated to
 * `n00b_result_t(T)` returns whose `err` side carries an
 * `n00b_regex_algebra_err_t` value cast to `int` (per § 7.5 / D14).
 */
#pragma once

#include "n00b.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "adt/result.h"
#include "adt/option.h"
#include "adt/list.h"

#include "internal/regex/ids.h"
#include "internal/regex/algebra.h"
#include "internal/regex/nulls.h"
#include "internal/regex/accel.h"

// ---------------------------------------------------------------------------
// Forward declarations of sibling-module opaque types.
//
// `Solver`, `RegexBuilder` come from `algebra.h` / `solver.h`.  The
// SIMD-side opaque types (`RevTeddySearch` etc.) come from `accel.h`.
// `FwdDFA` is owned by `fas.h`; declared opaque here so consumers of
// `engine_LDFA_scan_fwd_active_set_*` need not pull `fas.h` in.
// ---------------------------------------------------------------------------

typedef struct FwdDFA FwdDFA;

// ---------------------------------------------------------------------------
// Public scalar constants — defined in engine.c with external linkage.
// ---------------------------------------------------------------------------

/** @brief `size_t` sentinel meaning "no match". */
extern const size_t   engine_NO_MATCH;
/** @brief Lazy-DFA cache slot has not been populated yet. */
extern const uint32_t engine_DFA_MISSING;
/** @brief State id for the dead state (any input transitions to itself). */
extern const uint32_t engine_DFA_DEAD;
/** @brief State id for the post-Begin initial state. */
extern const uint32_t engine_DFA_INITIAL;

// ---------------------------------------------------------------------------
// EID_* — derived from algebra's `NullsId` sentinels.  Defined in
// engine.c with external linkage so `fas.c` and `bdfa.c` can reference
// them; values match `NullsId_EMPTY`/`CENTER0`/`ALWAYS0`/`BEGIN0`/`END0`.
// ---------------------------------------------------------------------------

extern const uint32_t EID_NONE;
extern const uint32_t EID_CENTER0;
extern const uint32_t EID_ALWAYS0;
extern const uint32_t EID_BEGIN0;
extern const uint32_t EID_END0;

// ---------------------------------------------------------------------------
// Engine-local growable types — single-owner, n00b_list_t(T) backed.
//
// Per § 7.5: every growable Vec<T> uses `n00b_list_new_private(T)` so the
// engine never pays for a rwlock.  Cross-TU function signatures use
// `n00b_list_t(T)` directly; ncc's `typeid(...)` makes that one canonical
// type per element type so engine TUs share the wire format.  Hash-key
// payloads in fas.c walk the list contents via a custom `n00b_hash_fn`.
// ---------------------------------------------------------------------------

/** @brief `(lo, hi)` inclusive-range pair, mirrors `accel.h::ByteRange`. */
typedef struct U8Pair    { uint8_t lo; uint8_t hi; } U8Pair;

/**
 * @brief Sorted set of `NullState` — the per-state effects table cell.
 *
 * Per LDFA state effects-id, the algebra-side `nulls_builder_get_set_ref`
 * fills in the entries via `regex_builder_nulls_entry_vec`.  Stored as a
 * private (unlocked) `n00b_list_t(NullState)`; alias provided for the
 * cross-TU wire format.
 */
typedef n00b_list_t(NullState) NullStateList;

// ---------------------------------------------------------------------------
// Optional accelerator handles — `Option<RevTeddySearch *>` etc.  Stored
// directly in `LDFA` rather than `n00b_option_t(T)` so the wire format is
// stable across engine / SIMD modules and so the typed-id name appears
// in struct-field grep output.
// ---------------------------------------------------------------------------

/** @brief `Option<RevTeddySearch *>` — borrowed/owned per phase-2 design. */
typedef struct OptionRevTeddySearch {
    bool            present;
    RevTeddySearch *value;
} OptionRevTeddySearch;

/** @brief `Option<FwdPrefixSearch *>`. */
typedef struct OptionFwdPrefixSearch {
    bool             present;
    FwdPrefixSearch *value;
} OptionFwdPrefixSearch;

// ---------------------------------------------------------------------------
// 256-byte minterm lookup — bundled so `engine_minterms_lookup` returns a
// value rather than writing through an out-pointer.
// ---------------------------------------------------------------------------

/** @brief Per-byte minterm-index lookup. */
typedef struct U8Lookup256 { uint8_t v[256]; } U8Lookup256;

// ---------------------------------------------------------------------------
// Opaque set / map adapter handles used by `LDFA`.
//
// The actual layouts are file-local to `engine.c` (`n00b_dict_t(NodeId,
// uint16_t)`, `n00b_dict_t(NodeId, bool)`, etc.) per § 7.5 D11; these
// names appear in `LDFA` so cross-TU pointer plumbing stays compile-clean.
// ---------------------------------------------------------------------------

/** @brief `n00b_dict_t(NodeId, uint16_t)` — derivative-target → state-id. */
typedef struct NodeU16Map NodeU16Map;
/** @brief `n00b_dict_t(TSetId, bool)` — TSetId set used for minterm seed. */
typedef struct TSetIdSet  TSetIdSet;

// ---------------------------------------------------------------------------
// LDFA — Lazy on-demand DFA.
// ---------------------------------------------------------------------------

/**
 * @brief Lazy on-demand DFA.
 *
 * Heap-allocated by `engine_LDFA_new` / `engine_LDFA_new_fwd`; released
 * by `engine_LDFA_free`.  The `_drop` half (release of every owned
 * field, leaving the `LDFA *` itself alive) is exposed for callers that
 * embed the struct or want to bail mid-construction without leaking.
 */
typedef struct LDFA {
    uint16_t                       pruned;           /**< State id of the begin-pruned root. */
    NodeIdMap                     *prune_memo;       /**< `FxHashMap<NodeId, NodeId>` shape memo. */
    n00b_list_t(uint16_t)          begin_table;      /**< Per-minterm initial transition. */
    n00b_list_t(uint16_t)          center_table;     /**< Cached transition cells. */
    n00b_list_t(uint16_t)          effects_id;       /**< Per-state nulls-id index. */
    n00b_list_t(NullStateList)     effects;          /**< Indexed by `effects_id` / `center_effect_id`. */
    n00b_list_t(uint16_t)          center_effect_id; /**< Per-state center-only nulls-id. */
    uint32_t                       mt_log;           /**< `next_pow2_log(num_minterms)`. */
    uint8_t                        mt_lookup[256];   /**< Per-byte minterm index. */
    n00b_list_t(TSetId)            minterms;         /**< Partition refinement of the alphabet. */
    n00b_list_t(NodeId)            state_nodes;      /**< Per-state regex node id. */
    NodeU16Map                    *node_to_state;    /**< Inverse of `state_nodes`. */
    n00b_list_t(uint8_t)           skip_ids;         /**< Per-state skip-searcher index (1-based). */
    n00b_list_t(MintermSearchValue) skip_searchers;  /**< Skip-searcher table. */
    OptionRevTeddySearch           prefix_skip;      /**< Optional `pruned`-state prefix searcher. */
    size_t                         max_capacity;     /**< Cap on `state_nodes.len`. */
    bool                           is_forward;       /**< True for fwd LDFA, false for rev. */
    bool                           has_anchors;      /**< Initial node contains anchors. */
    /**
     * Per-LDFA allocator, forwarded from the parent RegexBuilder.
     * Internal lists / dicts / state arrays allocate through this so
     * the bulk of compile-time allocation lands in the per-regex pool
     * (gc-bits.md Step 5).
     */
    n00b_allocator_t              *allocator;
} LDFA;

// ---------------------------------------------------------------------------
// BDFA — owned by `bdfa.h`; forward-declared here so callers that only
// touch the LDFA need not include the BDFA header.
// ---------------------------------------------------------------------------

/** @brief Bounded ahead-of-time DFA.  Defined in `bdfa.h`. */
typedef struct BDFA BDFA;

// ===========================================================================
// Public engine functions (cross-TU).
// ===========================================================================

/**
 * @brief Walk @p start_id's sub-DAG and collect every `KIND_PRED` node's
 *        `TSetId` into a fresh `TSetIdSet`.  Caller frees via the dict-set
 *        free helper (file-local to engine.c — exposed through the
 *        accessor below for cross-TU use).
 */
TSetIdSet *collect_sets(const RegexBuilder *b, NodeId start_id);

/**
 * @brief Walk a `TRegex` under the path predicate @p set and return the
 *        leaf NodeId reached.  Thin wrapper around
 *        `regex_builder_transition_term`; kept for parity with upstream
 *        Rust / resharp-c module surface.
 */
NodeId transition_term(RegexBuilder *b, TRegexId der, TSetId set);

/**
 * @brief Refine the alphabet by @p sets and return the resulting minterm
 *        partition as a sorted `Vec<TSetId>` (`TSET_ID_FULL` first by
 *        contract).  Used by both LDFA (engine.c) and BDFA (bdfa.c).
 */
n00b_list_t(TSetId) engine_generate_minterms(TSetIdSet *sets, Solver *solver);

/**
 * @brief Build a 256-byte lookup table mapping each byte to its minterm
 *        index in @p minterms.  Index 0 is reserved for "byte not in any
 *        non-trivial minterm"; index 1..N-1 cover `minterms[1..N]`.
 */
U8Lookup256 engine_minterms_lookup  (const n00b_list_t(TSetId) *minterms, Solver *solver);

/**
 * @brief Drop the helper `TSetIdSet` returned by `collect_sets`.  Defined
 *        in engine.c since the dict-set layout is file-local there.
 */
void TSetIdSet_free(TSetIdSet *set);

// ---------------------------------------------------------------------------
// LDFA construction / destruction.  Fallible operations return
// `n00b_result_t(LDFA *)`; on err the LDFA pointer is `nullptr` and the
// `err` side holds an `n00b_regex_algebra_err_t` cast to `int`.
// ---------------------------------------------------------------------------

/**
 * @brief Build a reverse LDFA from @p initial.  Caller frees via
 *        `engine_LDFA_free`.
 */
n00b_result_t(LDFA *) engine_LDFA_new(RegexBuilder *b, NodeId initial,
                                       size_t max_capacity);

/**
 * @brief Build a forward LDFA from @p initial.  Identical to
 *        `engine_LDFA_new` except the `is_forward` flag flips the
 *        prune-fwd / prune-rev choice during state expansion.
 */
n00b_result_t(LDFA *) engine_LDFA_new_fwd(RegexBuilder *b, NodeId initial,
                                          size_t max_capacity);

/**
 * @brief Release every heap-owned field on @p self.  Idempotent and safe
 *        on a partially-initialised LDFA.  Does not free @p self itself —
 *        use `engine_LDFA_free` for that.
 */
void engine_LDFA_drop(LDFA *self);

/**
 * @brief Public destructor: calls `engine_LDFA_drop` and then `n00b_free`s
 *        the `LDFA *`.  Safe with @p self == nullptr.
 */
void engine_LDFA_free(LDFA *self);

// ---------------------------------------------------------------------------
// LDFA hot-path entry points.
// ---------------------------------------------------------------------------

/** @brief Compute the linear `center_table` index for `(state, mt)`. */
size_t   engine_LDFA_dfa_delta(const LDFA *self, uint16_t state_id, uint32_t mt);

/** @brief Resize bookkeeping tables to cover state @p state_id. */
void     engine_LDFA_ensure_capacity(LDFA *self, uint16_t state_id);

/** @brief Look up @p node's state-id, registering a fresh one if absent. */
uint16_t engine_LDFA_get_or_register(LDFA *self, RegexBuilder *b, NodeId node);

/**
 * @brief Resolve `(state_id, minterm_idx)` to its successor, expanding
 *        the LDFA on a cache miss.  Returns the successor in `ok`.
 */
n00b_result_t(uint32_t) engine_LDFA_lazy_transition(LDFA *self, RegexBuilder *b,
                                                     uint32_t state_id,
                                                     uint32_t minterm_idx);

/**
 * @brief Best-effort BFS expansion of the lazy DFA up to @p threshold
 *        new states.  Surfaces no errors — failures halt expansion.
 */
void engine_LDFA_precompile(LDFA *self, RegexBuilder *b, size_t threshold);

/** @brief Cycle-detect the non-nullable derivative graph (BFS-bounded). */
bool engine_LDFA_has_nonnullable_cycle(LDFA *self, RegexBuilder *b, size_t budget);

/**
 * @brief Populate every cell of `(state_id, *)` row in `center_table`.
 *
 * Returns ok on success (or if the row is already populated); err on
 * algebra-side derivative failure or if the cap would be exceeded by
 * an intermediate state insertion.
 */
n00b_result_t(int) engine_LDFA_create_state(LDFA *self, RegexBuilder *b,
                                             uint32_t state_id);

/**
 * @brief When `prefix_skip` is set, ensure the `pruned` state has the
 *        all-skip searcher installed so collect_rev sees the prefix
 *        accelerator on its first call.
 */
void engine_LDFA_ensure_pruned_skip(LDFA *self);

/**
 * @brief Slow-path fwd scan (no SIMD skip).  `out_pos` ok value is
 *        `engine_NO_MATCH` if nothing matched, the end offset otherwise.
 */
n00b_result_t(size_t) engine_LDFA_scan_fwd_slow(LDFA *self, RegexBuilder *b,
                                                  size_t pos_begin,
                                                  const uint8_t *data,
                                                  size_t data_len);

/**
 * @brief Find every match in @p data, appending `Match` records to
 *        @p matches.  Mirrors the resharp Rust `find_iter` collect-all
 *        semantics.  Per-call ownership of @p matches stays with the
 *        caller (n00b regex API uses an `n00b_list_t(Match)` here).
 */
n00b_result_t(int) engine_LDFA_scan_fwd_all(LDFA *self, RegexBuilder *b,
                                              const n00b_list_t(size_t) *nulls,
                                              const uint8_t *data,
                                              size_t data_len,
                                              n00b_list_t(Match) *matches);

/**
 * @brief Walk @p len bytes from @p pos, returning the resolved state in
 *        the `ok` value.  On err the state is unspecified.
 */
n00b_result_t(uint32_t) engine_LDFA_walk_input(LDFA *self, RegexBuilder *b,
                                                 size_t pos, size_t len,
                                                 const uint8_t *data,
                                                 size_t data_len);

/**
 * @brief Continue a fwd scan from a known DFA @p state at @p pos_begin
 *        for at most `data_len - pos_begin` bytes.  `ok` value is
 *        `engine_NO_MATCH` on no match, the end offset otherwise.
 */
n00b_result_t(size_t) engine_LDFA_scan_fwd_from(LDFA *self, RegexBuilder *b,
                                                  uint32_t state,
                                                  size_t pos_begin,
                                                  const uint8_t *data,
                                                  size_t data_len);

/**
 * @brief Walk backwards from `(end - 1)` to @p begin to find the earliest
 *        accepting position.  `ok` value is `engine_NO_MATCH` on no match,
 *        the start offset otherwise.
 */
n00b_result_t(size_t) engine_LDFA_scan_rev_from(LDFA *self, RegexBuilder *b,
                                                  size_t end, size_t begin,
                                                  const uint8_t *data,
                                                  size_t data_len);

/** @brief True iff the LDFA has a SIMD skip / prefix searcher available. */
bool engine_LDFA_can_skip(const LDFA *self);

/**
 * @brief Reverse-scan from @p start_pos collecting every center-nullable
 *        position into @p nulls.  Caller-owned grown vec.
 */
n00b_result_t(int) engine_LDFA_collect_rev(LDFA *self, RegexBuilder *b,
                                            size_t start_pos,
                                            const uint8_t *data,
                                            size_t data_len,
                                            n00b_list_t(size_t) *nulls);

/** @brief As `engine_LDFA_collect_rev`, but returns at the first hit. */
n00b_result_t(int) engine_LDFA_collect_rev_first(LDFA *self, RegexBuilder *b,
                                                  size_t start_pos,
                                                  const uint8_t *data,
                                                  size_t data_len,
                                                  n00b_list_t(size_t) *nulls);

/** @brief Fast-path for a 1-byte input (common-case from public API). */
n00b_result_t(int) engine_LDFA_len_1_rev(LDFA *self, uint32_t curr,
                                          n00b_list_t(size_t) *nulls);

/**
 * @brief Result of `engine_LDFA_scan_fwd_first_null_from` — bundles the
 *        three out values upstream Rust returned via tuple destructuring.
 */
typedef struct EngineFirstNullOut {
    uint32_t state;
    size_t   pos;
    bool     hit_null;
} EngineFirstNullOut;

/**
 * @brief Advance the DFA from @p state / @p pos_begin until a CENTER-
 *        nullable state is found or input is exhausted.  `hit_null=true`
 *        means the resolved state has CENTER nullability set;
 *        `hit_null=false` means input exhausted (or `engine_DFA_DEAD`
 *        reached, in which case `state <= engine_DFA_DEAD`).
 */
n00b_result_t(EngineFirstNullOut)
    engine_LDFA_scan_fwd_first_null_from(LDFA *self, RegexBuilder *b,
                                         uint32_t state, size_t pos_begin,
                                         const uint8_t *data, size_t data_len);

// ---------------------------------------------------------------------------
// Free helpers used by stream / public TUs.
// ---------------------------------------------------------------------------

/** @brief True iff @p state has any null with @p mask in its effects table. */
bool engine_has_any_null(const n00b_list_t(uint16_t) *effects_id,
                         const n00b_list_t(NullStateList) *effects,
                         uint32_t state, Nullability mask);

/**
 * @brief Externally-callable wrapper around the file-local
 *        `collect_max_fwd`.  Used by `stream.c` and `public.c`.
 */
void engine_collect_max_fwd_pub(const n00b_list_t(uint16_t) *effects_id,
                                const n00b_list_t(NullStateList) *effects,
                                uint32_t state, size_t pos, Nullability mask,
                                size_t *best);

/** @brief Reverse counterpart to `engine_collect_max_fwd_pub`. */
void engine_collect_max_rev_pub(const n00b_list_t(uint16_t) *effects_id,
                                const n00b_list_t(NullStateList) *effects,
                                uint32_t state, size_t pos, Nullability mask,
                                size_t *best);

// ---------------------------------------------------------------------------
// LDFA field accessors used by `lib.c` / `fas.c`.
// ---------------------------------------------------------------------------

const uint8_t          *engine_LDFA_mt_lookup     (const LDFA *self);
const uint16_t         *engine_LDFA_begin_table   (const LDFA *self);
uint16_t                engine_LDFA_pruned        (const LDFA *self);
const n00b_list_t(uint16_t)      *engine_LDFA_effects_id    (const LDFA *self);
size_t                  engine_LDFA_effects_id_len(const LDFA *self);
const n00b_list_t(NullStateList) *engine_LDFA_effects       (const LDFA *self);
size_t                  engine_LDFA_state_nodes_len(const LDFA *self);
void                    engine_LDFA_set_prefix_skip(LDFA *self,
                                                    RevTeddySearch *skip);
RevTeddySearch         *engine_LDFA_get_prefix_skip(const LDFA *self);
uint32_t                engine_LDFA_mt_log        (const LDFA *self);
uint16_t                engine_LDFA_effects_id_at (const LDFA *self, size_t idx);
uint16_t                engine_LDFA_mt_lookup_at  (const LDFA *self, size_t idx);
uint16_t                engine_LDFA_begin_table_at(const LDFA *self, size_t idx);
size_t                  engine_LDFA_effects_len   (const LDFA *self, uint32_t eid);
void                    engine_LDFA_effects_get   (const LDFA *self, uint32_t eid,
                                                   size_t i, NullState *out);

// ---------------------------------------------------------------------------
// `engine_LDFA_scan_fwd_active_set_*` adapters around fas.c's
// `LDFA_scan_fwd_active_set_{always_nullable,general}`.  Fas.c provides
// the two monomorphisations; engine.c wraps them for parity with the
// resharp module surface.
// ---------------------------------------------------------------------------

n00b_result_t(int) engine_LDFA_scan_fwd_active_set_true(LDFA *self,
                                                        RegexBuilder *b,
                                                        FwdDFA *fas,
                                                        const uint8_t *input,
                                                        size_t input_len,
                                                        const n00b_list_t(size_t) *nulls,
                                                        n00b_list_t(Match) *matches);

n00b_result_t(int) engine_LDFA_scan_fwd_active_set_false(LDFA *self,
                                                         RegexBuilder *b,
                                                         FwdDFA *fas,
                                                         const uint8_t *input,
                                                         size_t input_len,
                                                         const n00b_list_t(size_t) *nulls,
                                                         n00b_list_t(Match) *matches);
