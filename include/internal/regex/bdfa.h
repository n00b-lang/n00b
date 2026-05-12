/**
 * @file bdfa.h
 * @brief Bounded DFA: builder, transition kernel, scan dispatcher.
 *
 * Internal regex-engine header, not part of the public n00b surface.
 * Algorithmic names track upstream Rust closely and intentionally do
 * not carry the `n00b_` prefix — the lowercase-snake `bdfa_*` naming
 * mirrors Rust's `BDFA` impl.
 *
 * Extracted from upstream commit 8f01936's reorganisation that split
 * the bounded DFA out of the main engine module into its own unit.
 * `bdfa_new` builds a BDFA from a `RegexBuilder` and a pattern node;
 * `bdfa_scan` walks input bytes through the cached transition table,
 * lazily filling holes via `bdfa_transition`.  Helpers shared with
 * the lazy DFA (`collect_sets`, `transition_term`, the partition-tree
 * minterm generator) live in `engine.h`.
 *
 * Companion source: `src/text/regex/engine/bdfa.c`.
 *
 * Related modules: `engine.h` (LDFA + shared helpers), `accel.h`
 * (FwdPrefixSearch flavours used by the prefix accelerator), `prefix.h`
 * (prefix-set extraction the BDFA consumes), `regex.h` (the engine-level
 * `Match` and matches container).
 */
#pragma once

#include "n00b.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "adt/list.h"
#include "adt/result.h"

#include "internal/regex/ids.h"
#include "internal/regex/algebra.h"  // RegexBuilder, NodeId, TSetId, ...

// Sibling regex headers — their parallel-landing subagents own the
// matching declarations; this header trusts them.
//
// `accel.h` provides `FwdPrefixSearch` (and the underlying SIMD-search
// variants) plus its accessors.  We forward-declare the type here and
// require the consumer to include `accel.h` if they need the full
// definition.
typedef struct FwdPrefixSearch FwdPrefixSearch;

// `regex.h` provides the engine-level `Match` (start,end pair) and the
// `n00b_list_t(Match)` container the scan kernel writes into.  We use
// `Match` and a pointer to the matches list below; consumers of this
// header that build matches must include `regex.h` for the full
// definition of `Match` and any push helpers.
typedef struct Match Match;

// =============================================================================
// PREFIX-mode dispatch tags
// =============================================================================
//
// The upstream Rust uses `bdfa_scan<const PREFIX: u8, const ISMATCH: bool>`.
// In C we dispatch on a runtime PREFIX argument; the values must match the
// Rust constants.  The caller (engine/regex.c) selects the correct value
// based on `bdfa_prefix_is_some` / `bdfa_prefix_is_literal`.

constexpr uint8_t PREFIX_NONE    = 0; /**< No prefix accelerator. */
constexpr uint8_t PREFIX_SEARCH  = 1; /**< Generic prefix search (FwdPrefixSearch). */
constexpr uint8_t PREFIX_LITERAL = 2; /**< Literal-byte prefix fast path. */

// =============================================================================
// BDFA — opaque to public callers
// =============================================================================
//
// The struct is owned by `bdfa.c`; consumers go through accessors below.
typedef struct BDFA BDFA;

// =============================================================================
// Constructor / destructor
// =============================================================================

/**
 * @brief Build a BDFA for @p pattern_node.
 * @return ok value carries the heap-allocated BDFA pointer.  err carries an
 *         `n00b_regex_algebra_err_t` value cast to int (per § 7.5 D14).
 */
n00b_result_t(BDFA *) bdfa_new(RegexBuilder *b, NodeId pattern_node);

/**
 * @brief Release every heap-owned field on @p self.  Safe with @p self == nullptr
 *        and on a partially-initialised BDFA.  Does not free @p self itself —
 *        used by `bdfa_new`'s error path (Drop equivalent) and by `bdfa_free`.
 */
void bdfa_drop(BDFA *self);

/**
 * @brief Public destructor.  Calls `bdfa_drop` and then frees the BDFA.
 */
void bdfa_free(BDFA *self);

// =============================================================================
// Transition kernel
// =============================================================================

/**
 * @brief Fetch (and lazily fill) the cached transition for (@p state, @p mt_idx).
 * @return ok value carries the packed transition (high 16 bits = match-rel,
 *         low 16 bits = next state id).  err carries an
 *         `n00b_regex_algebra_err_t` value cast to int.
 */
n00b_result_t(uint32_t) bdfa_transition(BDFA *self, RegexBuilder *b,
                                        uint16_t state, size_t mt_idx);

/**
 * @brief Counted-loop helper: highest `best` field across the COUNTED chain
 *        rooted at @p node.
 */
uint32_t bdfa_counted_best(NodeId node, const RegexBuilder *b);

// =============================================================================
// Field accessors (called from sibling engine code)
// =============================================================================

const uint32_t  *bdfa_table             (const BDFA *bd);
const uint8_t   *bdfa_minterms_lookup   (const BDFA *bd);
const uint32_t  *bdfa_match_end_off     (const BDFA *bd);
const uint32_t  *bdfa_match_rel         (const BDFA *bd);
const NodeId    *bdfa_states            (const BDFA *bd);
uint32_t         bdfa_mt_log            (const BDFA *bd);
uint16_t         bdfa_initial           (const BDFA *bd);
uint16_t         bdfa_after_prefix      (const BDFA *bd);
uint32_t         bdfa_prefix_len        (const BDFA *bd);
FwdPrefixSearch *bdfa_prefix_get        (const BDFA *bd);
bool             bdfa_prefix_is_some    (const BDFA *bd);
bool             bdfa_prefix_is_literal (const BDFA *bd);

/**
 * @brief Forward search using the prefix accelerator.
 * @return true and writes the position into @p *out_pos if found; false otherwise.
 */
bool bdfa_prefix_find_fwd(const BDFA *bd, const uint8_t *input,
                          size_t input_len, size_t pos, size_t *out_pos);

// =============================================================================
// Scan dispatcher
// =============================================================================

/**
 * @brief Runtime-PREFIX / runtime-ISMATCH scan kernel.
 *
 * Mirrors the upstream `bdfa_scan<const PREFIX: u8, const ISMATCH: bool>`,
 * with both flags lifted to runtime arguments.  When @p is_match is true the
 * walker stops on the first match (writing `*out_found = true`); otherwise
 * every match is appended to @p matches.
 *
 * @param prefix_mode  One of `PREFIX_NONE` / `PREFIX_SEARCH` / `PREFIX_LITERAL`.
 * @param matches      Caller-managed engine-internal match list.  Pushed-to in
 *                     find-all mode, untouched in is-match mode.
 * @return ok value carries the `out_found` flag.  err carries an
 *         `n00b_regex_algebra_err_t` value cast to int.
 */
n00b_result_t(bool) bdfa_scan(uint8_t prefix_mode, bool is_match,
                              BDFA *bounded, RegexBuilder *b,
                              const uint8_t *input, size_t len,
                              n00b_list_t(Match) *matches);
