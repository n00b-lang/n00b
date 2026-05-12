/**
 * @file solver.h
 * @brief Regex transition-set solver: 256-bit `TSet` algebra and id interning.
 *
 * The `Solver` is the regex engine's canonical owner of `TSet`
 * values.  `TSet` is a 256-bit set over the byte alphabet (one bit per
 * possible u8 value), used to represent character classes / transition
 * predicates in the derivative-based engine.  Each distinct `TSet` is
 * interned to a small `TSetId` so the rest of the engine compares,
 * unions, intersects, and complements predicates by id rather than by
 * 32-byte value.
 *
 * Two well-known ids are established by `solver_new()` in fixed order:
 * `TSET_ID_EMPTY` (the all-zero set) is id 0; `TSET_ID_FULL` (the
 * all-ones set) is id 1.  Both have external linkage so other regex
 * TUs can reference them without going through a getter.
 *
 * The solver also pretty-prints `TSet` values back to a regex-syntax
 * fragment (`[a-z]`, `[^...]`, `.`, etc.) for diagnostic output.  The
 * pretty-printer is byte-oriented; output is a NUL-terminated `char *`
 * heap allocation managed by the n00b allocator (no explicit free
 * required — the GC reclaims it).
 *
 * This header is part of the regex engine's internal vocabulary; the
 * algorithmic names track upstream Rust closely and intentionally do
 * not carry the `n00b_` prefix.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "core/alloc.h"
#include "internal/regex/ids.h"

// Well-known interned ids; established by `solver_new()` in this fixed order.
// Defined with external linkage in solver.c so other engine TUs can reference
// them via `extern const`.
extern const TSetId TSET_ID_EMPTY;
extern const TSetId TSET_ID_FULL;

// Forward-declared: opaque TSet-keyed cache type.  The actual definition is
// produced by the typed-dict generator in solver.c (the Rust `FxHashMap<TSet,
// TSetId>` analog).
typedef struct TSetCache TSetCache;

/**
 * @brief Interning solver: maps each distinct 256-bit `TSet` to a small id.
 *
 * The struct is exposed (rather than opaque-with-pointer) because
 * `MetadataBuilder` embeds a `Solver` by value.  The fields are
 * algorithm-internal and should not be touched outside `solver.c`.
 *
 * The `array` is a contiguous run of `TSet` values indexed by id; it
 * grows geometrically.  The `cache` is the inverse map used to find
 * an existing id for a given value during interning.
 */
typedef struct Solver {
    TSetCache         *cache;
    TSet              *array;
    size_t             array_len;
    size_t             array_cap;
    /* Per-solver allocator (forwarded from RegexBuilder via
     * MetadataBuilder).  See gc-bits.md Step 5 — when non-null, every
     * allocation routes through this allocator instead of the runtime
     * default. */
    n00b_allocator_t  *allocator;
} Solver;

// ----- TSet operations (value semantics) -----

/** @brief Construct a `TSet` whose four words are all @p v. */
[[nodiscard]] TSet TSet_splat(uint64_t v);

/**
 * @brief Set the bits corresponding to the bytes in @p bytes.
 * @param bytes  Pointer to @p len input bytes.
 * @param len    Number of bytes to fold into the set.
 */
[[nodiscard]] TSet tset_from_bytes(const uint8_t *bytes, size_t len);

/** @brief Test whether byte @p b is in @p self. */
[[nodiscard]] bool TSet_contains_byte(const TSet *self, uint8_t b);

/** @brief Bitwise AND on two `TSet` values. */
[[nodiscard]] TSet TSet_bitand(TSet a, TSet b);

/** @brief Bitwise OR on two `TSet` values. */
[[nodiscard]] TSet TSet_bitor(TSet a, TSet b);

/** @brief Bitwise NOT on a `TSet` value. */
[[nodiscard]] TSet TSet_not(TSet a);

/** @brief Equality test on two `TSet` values. */
[[nodiscard]] bool TSet_eq(TSet a, TSet b);

// ----- (u8, u8) range, used for pretty-printing -----

/** @brief Inclusive byte range [start, end]. */
typedef struct ByteRange {
    uint8_t start;
    uint8_t end;
} ByteRange;

/**
 * @brief Sorted, unique sequence of `ByteRange`s.
 *
 * Stand-in for Rust's `BTreeSet<(u8, u8)>` — used only by the
 * pretty-printer.  Backing storage grows geometrically; no rwlock
 * (single-threaded scratch).
 */
typedef struct ByteRangeSet {
    ByteRange *data;
    size_t     len;
    size_t     cap;
} ByteRangeSet;

/** @brief Initialise an empty `ByteRangeSet`. */
void ByteRangeSet_init(ByteRangeSet *s);

/** @brief Drop @p s's backing storage and zero the struct. */
void ByteRangeSet_free(ByteRangeSet *s);

/** @brief Insert @p r if absent; preserves sorted-unique order. */
void ByteRangeSet_insert(ByteRangeSet *s, ByteRange r);

[[nodiscard]] bool             ByteRangeSet_is_empty(const ByteRangeSet *s);
[[nodiscard]] size_t           ByteRangeSet_len(const ByteRangeSet *s);
[[nodiscard]] const ByteRange *ByteRangeSet_first(const ByteRangeSet *s);
[[nodiscard]] const ByteRange *ByteRangeSet_last(const ByteRangeSet *s);

// ----- Solver -----

/** @brief Allocate and initialise a fresh `Solver` with EMPTY/FULL pre-interned.
 *
 *  @param allocator  Routes every allocation the solver makes through this
 *                    allocator (or nullptr to use the runtime default).
 *                    See gc-bits.md Step 5.
 */
[[nodiscard]] Solver *solver_new(n00b_allocator_t *allocator);

/** @brief Equivalent to `solver_new(nullptr)`; mirrors Rust's `Default::default`. */
[[nodiscard]] Solver *solver_default(void);

/**
 * @brief Drop the solver's owned storage.  Safe to call with @p self == nullptr.
 */
void solver_free(Solver *self);

/** @brief Get the `TSet` value associated with @p set_id (by value). */
[[nodiscard]] TSet         solver_get_set(const Solver *self, TSetId set_id);

/** @brief Get a pointer into the solver's array for @p set_id (read-only). */
[[nodiscard]] const TSet  *solver_get_set_ref(const Solver *self, TSetId set_id);

/** @brief Intern @p inst, returning its id (allocating on cache miss). */
[[nodiscard]] TSetId       solver_get_id(Solver *self, TSet inst);

/**
 * @brief Test whether word @p idx of the set named by @p set_id has any
 *        bit in @p bit set.
 */
[[nodiscard]] bool         solver_has_bit_set(Solver *self, TSetId set_id,
                                              size_t idx, uint64_t bit);

/**
 * @brief Collapse @p tset's set bits into a sorted `ByteRangeSet`.
 *
 * Each maximal run of consecutive set bits becomes one inclusive range.
 */
[[nodiscard]] ByteRangeSet solver_pp_collect_ranges(const TSet *tset);

/**
 * @brief Pretty-print the set named by @p tset to a regex syntax fragment.
 *
 * The result is a NUL-terminated `char *` allocated through the n00b
 * runtime; no explicit free is required (managed by the GC).
 */
[[nodiscard]] char        *solver_pp(const Solver *self, TSetId tset);

/**
 * @brief Return the priority-ordered first set bit of @p tset (UTF-8 codepoint).
 *
 * Falls back to U+22A5 (UP TACK) for the empty set.
 */
[[nodiscard]] uint32_t     solver_pp_first(const Solver *self, const TSet *tset);

/**
 * @brief Materialise the byte ranges of @p tset into a fresh array.
 *
 * On return, `*out` points at @p out_len `ByteRange`s — managed by
 * the n00b runtime, so the caller does not free.  When the set is
 * empty, `*out` is nullptr and `*out_len` is 0.
 */
void                       solver_byte_ranges(const Solver *self, TSetId tset,
                                              ByteRange **out, size_t *out_len);

/** @brief The full byte set: every bit set. */
[[nodiscard]] TSet         solver_full(void);

/** @brief The empty byte set: no bits set. */
[[nodiscard]] TSet         solver_empty(void);

/** @brief Intern `set1 ∪ set2` and return its id. */
[[nodiscard]] TSetId       solver_or_id(Solver *self, TSetId set1, TSetId set2);

/** @brief Intern `set1 ∩ set2` and return its id. */
[[nodiscard]] TSetId       solver_and_id(Solver *self, TSetId set1, TSetId set2);

/** @brief Intern `¬set_id` and return its id. */
[[nodiscard]] TSetId       solver_not_id(Solver *self, TSetId set_id);

/** @brief True iff `set1 ∩ set2 ≠ ∅`. */
[[nodiscard]] bool         solver_is_sat_id(Solver *self, TSetId set1, TSetId set2);

/** @brief True iff `set1 ∩ set2 = ∅`. */
[[nodiscard]] bool         solver_unsat_id(Solver *self, TSetId set1, TSetId set2);

/** @brief Number of bytes (popcount) in the set named by @p set_id. */
[[nodiscard]] uint32_t     solver_byte_count(const Solver *self, TSetId set_id);

/**
 * @brief Materialise the bytes of the set named by @p set_id into a fresh array.
 *
 * On return, `*out` points at `*out_len` bytes — managed by the n00b
 * runtime, so the caller does not free.
 */
void                       solver_collect_bytes(const Solver *self, TSetId set_id,
                                                uint8_t **out, size_t *out_len);

/**
 * @brief If @p set_id names a singleton, write the byte to @p out_byte and
 *        return true; otherwise return false.
 */
[[nodiscard]] bool         solver_single_byte(const Solver *self, TSetId set_id,
                                              uint8_t *out_byte);

/** @brief True iff @p set1 == `TSET_ID_EMPTY`. */
[[nodiscard]] bool         solver_is_empty_id(const Solver *self, TSetId set1);

/** @brief True iff @p set1 == `TSET_ID_FULL`. */
[[nodiscard]] bool         solver_is_full_id(const Solver *self, TSetId set1);

/** @brief True iff `small_id ⊆ large_id` (i.e., `small \ large = ∅`). */
[[nodiscard]] bool         solver_contains_id(Solver *self, TSetId large_id,
                                              TSetId small_id);

/** @brief Intern the singleton set `{byte}` and return its id. */
[[nodiscard]] TSetId       solver_u8_to_set_id(Solver *self, uint8_t byte);

/**
 * @brief Intern the inclusive byte range [start, end] and return its id.
 *
 * Returns the empty set's id when @p start > @p end (mirrors Rust's
 * empty `start..=end` semantics).
 */
[[nodiscard]] TSetId       solver_range_to_set_id(Solver *self, uint8_t start, uint8_t end);

// ----- Value-level helpers (no interning) -----

[[nodiscard]] TSet  solver_and(const TSet *set1, const TSet *set2);
[[nodiscard]] bool  solver_is_sat(const TSet *set1, const TSet *set2);
[[nodiscard]] TSet  solver_or(const TSet *set1, const TSet *set2);
[[nodiscard]] TSet  solver_not(const TSet *set);
[[nodiscard]] bool  solver_is_full(const TSet *set);
[[nodiscard]] bool  solver_is_empty(const TSet *set);
[[nodiscard]] bool  solver_contains(const TSet *large, const TSet *small);
[[nodiscard]] TSet  solver_u8_to_set(uint8_t byte);
[[nodiscard]] TSet  solver_range_to_set(uint8_t start, uint8_t end);
