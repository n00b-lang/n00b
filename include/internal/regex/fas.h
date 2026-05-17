/**
 * @file fas.h
 * @brief Forward Active-Set DFA (FAS): cached transition tables on top of LDFA.
 *
 * Internal regex-engine header, not part of the public n00b surface.
 * Names track upstream Rust closely and intentionally do not carry the
 * `n00b_` prefix — `FwdDFA`, `SlotEntries`, `FwdAction`, etc. are the
 * regex algorithmic vocabulary.
 *
 * `FwdDFA` builds the active-set DFA on top of an underlying `LDFA`,
 * storing one entry per `(asid, mt)` cell of the cached transition
 * table.  Each cell points into a deduplicated table of `FwdAction`
 * records, which encode (a) the next asid; (b) per-slot relocation
 * codes; (c) the per-target end-relative nullability; (d) the spawn
 * disposition.  Two `LDFA::scan_fwd_active_set` monomorphisations
 * (always-nullable / general) drive scanning over an input buffer.
 *
 * Companion source: `src/text/regex/engine/fas.c`.
 */
#pragma once

#include "n00b.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "adt/list.h"
#include "adt/result.h"
#include "internal/regex/algebra.h"
#include "internal/regex/nulls.h"

// ---------------------------------------------------------------------------
// Forward declarations of opaque cross-TU types.
// ---------------------------------------------------------------------------

/**
 * @brief Opaque LDFA (lazy DFA) handle.  Definition lives in engine.c.
 */
typedef struct LDFA LDFA;

// `RegexBuilder` and `NullState` come from algebra.h / nulls.h.

// ---------------------------------------------------------------------------
// Engine-side error code.  Returned through `n00b_result_t.err` (cast to int)
// per § 7.5 (D14 int-err idiom).
// ---------------------------------------------------------------------------

/** @brief Domain-specific return code for fallible engine operations. */
typedef enum n00b_regex_engine_err_t {
    N00B_REGEX_ENGINE_ERR_NONE = 0,
    N00B_REGEX_ENGINE_ERR_PARSE,
    N00B_REGEX_ENGINE_ERR_ALGEBRA,
    N00B_REGEX_ENGINE_ERR_CAPACITY_EXCEEDED,
    N00B_REGEX_ENGINE_ERR_PATTERN_TOO_LARGE,
    N00B_REGEX_ENGINE_ERR_UNSUPPORTED_PATTERN,
} n00b_regex_engine_err_t;

// ---------------------------------------------------------------------------
// Constants — values match upstream Rust.
// ---------------------------------------------------------------------------

constexpr uint32_t FAS_ACTION_MISSING = UINT32_MAX;
constexpr uint16_t FAS_DIED           = 0xFFFF;
constexpr uint16_t FAS_LOW_BIT        = 0x8000;
constexpr uint16_t FAS_SPAWN_NONE     = 0xFFFF;
constexpr uint16_t FAS_SPAWN_DEAD     = 0xFFFE;
constexpr size_t   FAS_STATE_CAP      = 1024;
constexpr uint32_t SLOT_NIL           = UINT32_MAX;
constexpr uint32_t FAS_NOT_NULLABLE   = UINT32_MAX;

// ---------------------------------------------------------------------------
// `NullsId` tag externs — defined by the engine init path elsewhere.
//
// These mirror `NullsId::EMPTY/CENTER0/ALWAYS0/BEGIN0/END0.0`; the actual
// numeric values are fixed in `nulls.c` (see `NULLS_ID_*`).  Engine TUs need
// the bare `EID_*` aliases for parity with upstream Rust.
// ---------------------------------------------------------------------------

extern const uint32_t EID_NONE;
extern const uint32_t EID_CENTER0;
extern const uint32_t EID_ALWAYS0;
extern const uint32_t EID_BEGIN0;
extern const uint32_t EID_END0;

// Engine constants from `engine.c`.
extern const uint32_t engine_DFA_DEAD;
extern const uint32_t engine_DFA_INITIAL;

// ---------------------------------------------------------------------------
// Growable buffers used by the FAS state machine — `n00b_list_t(T)` private
// (unlocked) lists.  Per § 7.5: the state map and action map hash key
// payloads walk the list contents via a custom `n00b_hash_fn` rather than
// peeking at a {data,len,cap} struct directly.  Element-typed aliases are
// provided so the surface API reads naturally.
// ---------------------------------------------------------------------------

/**
 * @brief `Vec<u16>` of LDFA state ids — used as a state-vector key in the
 *        state map and as the per-asid canonical state list.
 */
typedef n00b_list_t(uint16_t) fas_list_u16;

/**
 * @brief `Vec<u32>` of end-relative offsets — used in `FwdAction` and as the
 *        per-asid transition table.
 */
typedef n00b_list_t(uint32_t) fas_list_u32;

/**
 * @brief `Vec<usize>` — the per-byte `max[]` table over the input.
 */
typedef n00b_list_t(size_t) fas_list_usize;

// ---------------------------------------------------------------------------
// `SlotEntries` — intrusive linked-list head over an external `linker[]`
// array.  Tracks the live slots that still have a chance of matching at the
// current scan position; `max_e` records the longest end seen so the scan
// can drain into the per-byte `max[]` table.
// ---------------------------------------------------------------------------

typedef struct SlotEntries {
    uint32_t head;
    uint32_t tail;
    size_t   max_e;
} SlotEntries;

// ---------------------------------------------------------------------------
// `FwdAction` — one entry of the FAS action table.
//
//   * `next_asid` is the resulting asid after applying the action.
//   * `old_acts[i]` encodes how source slot `i` maps onto a target slot,
//     dies (`FAS_DIED`), or merges into an existing target (`FAS_LOW_BIT`).
//   * `new_end_rel[j]` carries the end-relative offset of target slot `j`
//     (or `FAS_NOT_NULLABLE`).
//   * `spawn` is the FAS_SPAWN_* code for the implicit init transition.
//
// Both nested list buffers are owned by the surrounding `FwdDFA`
// (specifically by its `actions` array); the action map hash key walks
// them with a custom `n00b_hash_fn` per `dict.h:51,177` (`.fn` field).
// ---------------------------------------------------------------------------

typedef struct FwdAction {
    uint32_t      next_asid;
    fas_list_u16  old_acts;
    fas_list_u32  new_end_rel;
    uint16_t      spawn;
} FwdAction;

/** @brief `Vec<SlotEntries>` — per-slot scratch state across a scan. */
typedef n00b_list_t(SlotEntries) fas_slot_list;

// ---------------------------------------------------------------------------
// Opaque maps.  Real definitions in fas.c.
//
// `fas_state_map`  : map<Vec<u16>, u32> — keyed by content of the state
//                    vector via `n00b_hash_raw`.  Owns its key copies and
//                    frees them on destroy.
// `fas_action_map` : map<FwdAction, u32> — keyed by the composite content of
//                    `(next_asid, old_acts.bytes, new_end_rel.bytes, spawn)`
//                    so the table's size is bounded by the (asid, mt) product.
//                    A naïve `memcmp` on the FwdAction struct would alias by
//                    buffer pointer and let the table grow without bound.
//                    Owns its key copies and frees them on destroy.
// ---------------------------------------------------------------------------

typedef struct fas_state_map  fas_state_map;
typedef struct fas_action_map fas_action_map;

// ---------------------------------------------------------------------------
// `FwdDFA` — the cached FAS-DFA itself.
// ---------------------------------------------------------------------------

typedef struct FwdDFA {
    n00b_list_t(fas_list_u16) states;       /**< canonical state vectors per asid */
    fas_state_map            *state_map;
    fas_list_u32              trans;        /**< (asid, mt) -> action_id, flat */
    n00b_list_t(FwdAction)    actions;      /**< interned action table */
    fas_action_map           *action_map;
    size_t                    stride;
    uint32_t                  initial_asid;
    bool                      always_nullable;
    bool                      keep_spawn_on_merge;
    fas_list_usize            max;          /**< per-byte best end pos */
    fas_list_u32              linker;       /**< intrusive next-link buffer */
    fas_slot_list             regs;
    fas_slot_list             new_regs;
} FwdDFA;

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

[[nodiscard]] SlotEntries SlotEntries_default(void);
void                      SlotEntries_clear(SlotEntries *self);
[[nodiscard]] bool        SlotEntries_is_empty(const SlotEntries *self);
void                      SlotEntries_extend_e(SlotEntries *self, size_t ce);
void                      SlotEntries_drain_to_max(SlotEntries *self,
                                                   const uint32_t *linker,
                                                   size_t *max,
                                                   size_t max_len);

[[nodiscard]] FwdDFA *fas_FwdDFA_new(const LDFA *ldfa, bool keep_spawn_on_merge);
void                  fas_FwdDFA_free(FwdDFA *self);

// `LDFA::scan_fwd_active_set<const ALWAYS_NULLABLE: bool>` — Rust const-generic
// monomorphised into two functions.  `matches` is opaque (the public regex
// API decides its representation in Phase 9); each match is recorded via the
// cross-TU `matches_push` extern below.  Result `ok` value is unused (always 0).
n00b_result_t(int) LDFA_scan_fwd_active_set_always_nullable(
    LDFA          *self,
    RegexBuilder  *b,
    FwdDFA        *fas,
    const uint8_t *data,
    size_t         data_len,
    const size_t  *nulls,
    size_t         nulls_len,
    void          *matches);

n00b_result_t(int) LDFA_scan_fwd_active_set_general(
    LDFA          *self,
    RegexBuilder  *b,
    FwdDFA        *fas,
    const uint8_t *data,
    size_t         data_len,
    const size_t  *nulls,
    size_t         nulls_len,
    void          *matches);

// ---------------------------------------------------------------------------
// Externs supplied by sibling engine TUs (engine.c).  Phase 7 lands those
// in parallel; the signatures here are the contract.
// ---------------------------------------------------------------------------

extern uint32_t engine_LDFA_mt_log(const LDFA *self);
extern uint16_t engine_LDFA_effects_id_at(const LDFA *self, size_t idx);
extern uint16_t engine_LDFA_mt_lookup_at(const LDFA *self, size_t idx);
extern uint16_t engine_LDFA_begin_table_at(const LDFA *self, size_t idx);

/** @brief Number of `NullState` entries for effects-id @p eid. */
extern size_t engine_LDFA_effects_len(const LDFA *self, uint32_t eid);
/** @brief Read the `i`-th `NullState` for effects-id @p eid into @p *out. */
extern void   engine_LDFA_effects_get(const LDFA *self, uint32_t eid, size_t i,
                                      NullState *out);

/**
 * @brief LDFA::lazy_transition — Rust's `Result<u16, Error>`.
 *
 * Returns `n00b_result_t(uint32_t)` per § 7.5: the ok side carries the
 * transition target, the err side carries an `n00b_regex_engine_err_t`
 * cast to `int`.
 */
extern n00b_result_t(uint32_t) engine_LDFA_lazy_transition(LDFA *self,
                                                           RegexBuilder *b,
                                                           uint32_t s,
                                                           uint32_t mt);

/**
 * @brief Push one match into the caller's match buffer.
 *
 * The match buffer is opaque (`void *matches`); the public regex API
 * decides its representation in Phase 9.  This callback is provided by
 * the public layer.
 */
extern void matches_push(void *matches, size_t start, size_t end);
