/**
 * @file nulls.h
 * @brief Nullability algebra: sorted-set primitive `Nulls` and the
 *        de-duplicating `NullsBuilder` used to intern them.
 *
 * Tracks upstream Rust `nulls` module closely.  Names stay un-prefixed
 * (no `n00b_`) because this is the regex algorithmic vocabulary; the
 * header lives under `include/internal/regex/` and is not part of the
 * public n00b surface.
 *
 * `NullState` is a `(mask, rel)` pair.  `Nulls` is a *sorted set* of
 * `NullState` — sortedness is observable behaviour (see CONTRACT on
 * `Nulls` below).  `NullsBuilder` interns set values to compact
 * `NullsId` handles, with sentinel ids exposed for the well-known
 * sets (EMPTY, CENTER0, ALWAYS0, BEGIN0, END0).
 */
#pragma once

#include "n00b.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// `Nullability` is owned by algebra.h (regex/algebra.h after Phase 5).
// `NullsId` lives in regex/ids.h.
#include "internal/regex/algebra.h"
#include "internal/regex/ids.h"

// ---------------------------------------------------------------------------
// Nullability flag-set constants.
// ---------------------------------------------------------------------------
constexpr Nullability NULLABILITY_NEVER       = { 0b000 };
constexpr Nullability NULLABILITY_CENTER      = { 0b001 };
constexpr Nullability NULLABILITY_ALWAYS      = { 0b111 };
constexpr Nullability NULLABILITY_BEGIN       = { 0b010 };
constexpr Nullability NULLABILITY_END         = { 0b100 };
constexpr Nullability NULLABILITY_NONBEGIN    = { 0b011 };
constexpr Nullability NULLABILITY_EMPTYSTRING = { 0b110 };

[[nodiscard]] static inline bool nullability_eq(Nullability a, Nullability b) {
    return a.v == b.v;
}
[[nodiscard]] static inline bool nullability_has(Nullability self, Nullability flag) {
    return (self.v & flag.v) != 0;
}
[[nodiscard]] static inline Nullability nullability_and(Nullability self, Nullability other) {
    return (Nullability){ (uint8_t)(self.v & other.v) };
}
[[nodiscard]] static inline Nullability nullability_or(Nullability self, Nullability other) {
    return (Nullability){ (uint8_t)(self.v | other.v) };
}
[[nodiscard]] static inline Nullability nullability_not(Nullability self) {
    return (Nullability){ (uint8_t)(~self.v) };
}
[[nodiscard]] static inline int nullability_cmp(Nullability a, Nullability b) {
    if (a.v < b.v) return -1;
    if (a.v > b.v) return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// NullState — `(mask, rel)` pair.  Tag matches the forward-declaration in
// internal/regex/algebra.h so the two headers can be included together.
// ---------------------------------------------------------------------------
typedef struct NullState {
    Nullability mask;
    uint32_t    rel;
} NullState;

[[nodiscard]] NullState null_state_new(Nullability mask, uint32_t rel);
[[nodiscard]] NullState null_state_new0(Nullability mask);
[[nodiscard]] bool      null_state_eq(const NullState *a, const NullState *b);
[[nodiscard]] bool      null_state_is_center_nullable(const NullState *self);
[[nodiscard]] bool      null_state_is_mask_nullable(const NullState *self,
                                                    Nullability mask);
// Rust's Ord places higher rel first (other.rel.cmp(&self.rel)), then
// breaks ties with self.mask.cmp(&other.mask).  Mirrored here.
[[nodiscard]] int       null_state_cmp(const NullState *a, const NullState *b);

// ---------------------------------------------------------------------------
// NullsId sentinels (definitions in nulls.c).
// ---------------------------------------------------------------------------
extern const NullsId NULLS_ID_EMPTY;
extern const NullsId NULLS_ID_CENTER0;
extern const NullsId NULLS_ID_ALWAYS0;
extern const NullsId NULLS_ID_BEGIN0;
extern const NullsId NULLS_ID_END0;

[[nodiscard]] static inline bool nulls_id_eq(NullsId a, NullsId b) {
    return a.v == b.v;
}
[[nodiscard]] static inline bool nulls_id_gt(NullsId a, NullsId b) {
    return a.v > b.v;
}

// ---------------------------------------------------------------------------
// Nulls — sorted set of NullState (Rust BTreeSet<NullState>).
//
// CONTRACT (load-bearing — do not weaken):
//   * `Nulls` is a *sorted set* of `NullState` ordered by
//     `null_state_cmp`.
//   * `null_state_cmp(a,b) == 0` iff `a.mask == b.mask && a.rel == b.rel`,
//     i.e. the comparator discriminates on the full key.  The dedup
//     simplification in `nulls_builder_or_id` (see `nulls.c`) relies
//     on this: if `null_state_cmp` is ever loosened so distinct
//     (mask,rel) pairs can compare equal, `or_id` will silently drop
//     entries Rust would have kept.
//   * `nulls_for_each` walks elements in ascending `null_state_cmp`
//     order; `nulls_for_each_rev` walks the same set in descending
//     order.
// ---------------------------------------------------------------------------
typedef struct Nulls Nulls;

extern Nulls   *nulls_new(void);
extern Nulls   *nulls_clone(const Nulls *src);
extern void     nulls_free(Nulls *set);
extern void     nulls_insert(Nulls *set, NullState state);
extern size_t   nulls_len(const Nulls *set);
extern bool     nulls_eq(const Nulls *a, const Nulls *b);
extern uint64_t nulls_hash(const Nulls *set);
// Iteration: caller-supplied callback walks ascending order.
extern void     nulls_for_each(const Nulls *set,
                               void (*cb)(const NullState *state, void *ctx),
                               void *ctx);
// Reverse iteration (used by or_id).
extern void     nulls_for_each_rev(const Nulls *set,
                                   void (*cb)(const NullState *state, void *ctx),
                                   void *ctx);
// Union (Rust `&self | &other` on BTreeSet).
extern Nulls   *nulls_union(const Nulls *a, const Nulls *b);

// Indexed accessors.
extern const NullState *nulls_set_get(const Nulls *self, size_t i);
extern size_t           nulls_set_len(const Nulls *self);

// ---------------------------------------------------------------------------
// Operation / Key — internal cache key for binary ops.
// `Key` is laid out with no padding so it can be hashed via
// `n00b_hash_raw` over its raw bytes.
// ---------------------------------------------------------------------------
typedef enum : uint8_t {
    OPERATION_OR    = 0,
    OPERATION_INTER = 1,
} Operation;

typedef struct {
    uint32_t op;       // Operation cast to u32 (no padding).
    NullsId  left;
    NullsId  right;
} Key;

static_assert(sizeof(Key) == 12, "Key must be 12 bytes (no padding)");

// ---------------------------------------------------------------------------
// NullsBuilder — owns the cache, dedup map, and the indexed array.
// ---------------------------------------------------------------------------

// FxHashMap<Nulls, NullsId>:
// CONTRACT: amortized O(1) lookup/insert, hashed by `nulls_hash`,
// compared by `nulls_eq`.
typedef struct NullsCache  NullsCache;

// FxHashMap<Key, NullsId>:
// CONTRACT: amortized O(1) lookup/insert, hashed over the 12-byte
// `Key { op, left, right }` POD.  `created_map_get` returns a
// *borrowed* pointer into a thread-local single-slot scratchpad —
// callers must not free it and must not retain it across the next
// get/insert.
typedef struct CreatedMap  CreatedMap;

// Vec<Nulls *> — append-only handle table.
typedef struct NullsArray  NullsArray;

extern NullsCache *nulls_cache_new(void);
extern void        nulls_cache_free(NullsCache *m);
extern void        nulls_cache_insert(NullsCache *m, const Nulls *key, NullsId id);
// Returns true and writes *out if present.
extern bool        nulls_cache_get(const NullsCache *m, const Nulls *key, NullsId *out);
extern size_t      nulls_cache_len(const NullsCache *m);

extern CreatedMap *created_map_new(void);
extern void        created_map_free(CreatedMap *m);
extern void        created_map_insert(CreatedMap *m, Key key, NullsId id);
// Returns pointer to a thread-local stored id slot if present, else nullptr.
extern const NullsId *created_map_get(const CreatedMap *m, const Key *key);

extern NullsArray *nulls_array_new(void);
extern void        nulls_array_free(NullsArray *a);
extern void        nulls_array_push(NullsArray *a, Nulls *set);
extern Nulls      *nulls_array_get(const NullsArray *a, size_t index);
extern size_t      nulls_array_len(const NullsArray *a);

// Tuple accessors used by the engine's null-table dump.
extern size_t    nulls_entry_len(const NullsArray *a, uint32_t id);
extern NullState nulls_entry_get(const NullsArray *a, uint32_t id, size_t idx);

typedef struct NullsBuilder {
    NullsCache       *cache;
    CreatedMap       *created;
    NullsArray       *array; // public in Rust; exposed here for parity.
    /* Per-builder allocator forwarded from RegexBuilder -> MetadataBuilder
     * -> NullsBuilder.  See gc-bits.md Step 5. */
    n00b_allocator_t *allocator;
} NullsBuilder;

[[nodiscard]] NullsBuilder nulls_builder_default(void);

/** @brief Construct a fresh NullsBuilder.
 *  @param allocator  Per-builder allocator; nullptr falls back to the
 *                    runtime default.  See gc-bits.md Step 5. */
[[nodiscard]] NullsBuilder nulls_builder_new(n00b_allocator_t *allocator);
void                       nulls_builder_drop(NullsBuilder *self);

[[nodiscard]] const Nulls *nulls_builder_get_set_ref(const NullsBuilder *self,
                                                     NullsId set_id);
// `nulls_builder_get_id` was previously exposed in resharp-c.  It has
// move semantics on its `Nulls *inst` argument (consumes it on cache
// hit via `nulls_free`), which is a UAF/double-free footgun for any
// external caller passing a borrowed pointer.  Demoted to file-local
// `static` in `nulls.c`.

[[nodiscard]] NullsId nulls_builder_or_id(NullsBuilder *self, NullsId set1, NullsId set2);
[[nodiscard]] NullsId nulls_builder_and_id(NullsBuilder *self, NullsId set1, NullsId set2);
[[nodiscard]] NullsId nulls_builder_and_mask(NullsBuilder *self, NullsId set1,
                                             Nullability mask);
[[nodiscard]] NullsId nulls_builder_not_id(NullsBuilder *self, NullsId set_id);
[[nodiscard]] NullsId nulls_builder_add_rel(NullsBuilder *self, NullsId set_id,
                                            uint32_t rel);

// Singleton constructor — wraps the file-local `nulls_builder_get_id`
// without exposing its move-semantics footgun.
[[nodiscard]] NullsId nulls_builder_get_id_singleton(NullsBuilder *self,
                                                    NullState s);

// Engine-side aliases (camelCase, used by engine.c).
extern const NullsId NullsId_EMPTY;
extern const NullsId NullsId_CENTER0;
extern const NullsId NullsId_ALWAYS0;
extern const NullsId NullsId_BEGIN0;
extern const NullsId NullsId_END0;

// Snake-case alias for `nulls_insert` used by parser/lib.c.
extern void null_state_set_insert(Nulls *set, NullState s);
