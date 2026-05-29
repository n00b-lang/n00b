#pragma once

/**
 * @file grammar_image.h
 * @brief Build-time grammar baking + runtime reconstruction of a
 *        pre-compiled `n00b_grammar_t`.
 *
 * WP-018 (naudit static-image grammar caching). The runtime cost of
 * parsing a `.bnf` file via the BNF metagrammar (PWZ) dominates small
 * single-file naudit invocations (~1.5s fixed). This module lets the
 * grammar be *baked* at build time into emitted C source that, at
 * runtime, reconstructs an identical `n00b_grammar_t` by replaying the
 * grammar builder primitives directly — skipping the metagrammar parse
 * entirely.
 *
 * Two surfaces live here:
 *
 *  - The **emitter** (`n00b_grammar_image_emit`) walks a finalized
 *    `n00b_grammar_t` and writes C source that calls the reconstruction
 *    primitives below. It is used by the build-time bake tool
 *    (`naudit-grammar-bake`) and by the `container_kind grammar` path of
 *    `n00b-static-init-helper`.
 *
 *  - The **reconstruction primitives** (`n00b_grammar_image_*`) are the
 *    low-level builder calls the emitted code invokes at runtime to
 *    rebuild the grammar id-for-id. They are deliberately faithful to
 *    the in-memory layout (NT ids, rule order, group records) so a
 *    reconstructed grammar produces structurally-identical parse trees
 *    to a freshly-parsed one.
 *
 * Derived analysis state (first-sets, valid-token set, left-corner /
 * LR0 tables) is **not** baked: the emitted code calls
 * `n00b_grammar_finalize()` at the end, which recomputes it cheaply
 * (there is no parse on the reconstruction path). PWZ — naudit's only
 * parse mode (`N00B_PARSE_MODE_PWZ_ONLY`) — never reads the LR0 tables
 * and tolerates absent first-sets, so the recompute is the safe and
 * minimal choice (WP-018 DF-EB / DF-EC).
 */

#include "slay/grammar.h"

// ============================================================================
// Reconstruction primitives (called by emitted C source at runtime)
// ============================================================================

/**
 * @brief Begin reconstructing a baked grammar.
 *
 * Allocates a fresh grammar and disables automatic error-rule
 * generation so the subsequent `n00b_grammar_finalize()` does not
 * re-inject error rules on top of the ones already baked into the
 * image. Returns the grammar the rest of the primitives populate.
 */
extern n00b_grammar_t *n00b_grammar_image_begin(void);

/**
 * @brief Create the next non-terminal during reconstruction.
 *
 * NTs must be created in ascending id order; this asserts the created
 * id matches @p expected_id so a layout mismatch fails loudly rather
 * than silently producing a divergent grammar. @p name may be the
 * empty string for anonymous NTs.
 *
 * @param g            Grammar under reconstruction.
 * @param expected_id  The id this NT must receive (== current NT count).
 * @param name         NT name (empty string ⇒ anonymous).
 * @param group_nt     True for synthetic EBNF group NTs.
 * @param start_nt     True if this NT is the grammar start symbol.
 */
extern void n00b_grammar_image_add_nt(n00b_grammar_t *g,
                                      int64_t         expected_id,
                                      n00b_string_t  *name,
                                      bool            group_nt,
                                      bool            start_nt);

/**
 * @brief Register a fixed-text terminal during reconstruction.
 *
 * Mirrors `n00b_register_terminal`; the returned id is hash-derived
 * from @p name so it matches the source grammar without being stored.
 */
extern void n00b_grammar_image_add_terminal(n00b_grammar_t *g,
                                            n00b_string_t  *name);

/**
 * @brief Register a named literal type during reconstruction.
 *
 * Mirrors `n00b_register_literal_type`; literal-type ids are dense and
 * assigned in registration order, so types must be replayed in the same
 * order they appear in the source grammar's `literal_type_map`.
 */
extern void n00b_grammar_image_add_literal_type(n00b_grammar_t *g,
                                                n00b_string_t  *name);

/**
 * @brief Attach a terminal category during reconstruction.
 *
 * Mirrors `n00b_grammar_set_terminal_category`.
 */
extern void n00b_grammar_image_add_terminal_category(n00b_grammar_t *g,
                                                     int64_t         terminal_id,
                                                     n00b_string_t  *category);

/**
 * @brief Append the next rule during reconstruction.
 *
 * Pushes a rule with the given contents onto the grammar's global rule
 * list and records it on the owning NT, faithfully reproducing the
 * source rule ordering (including baked error rules). @p items is copied
 * into a fresh private list, so the caller's array may be stack-local.
 *
 * @param g          Grammar under reconstruction.
 * @param nt_id      Owning non-terminal id.
 * @param cost       Rule cost.
 * @param penalty    True if this is a penalty (error-recovery) rule.
 * @param link_ix    Penalty link index (-1 when not a penalty rule).
 * @param n          Number of match items.
 * @param items      Match items (copied).
 */
extern void n00b_grammar_image_add_rule(n00b_grammar_t *g,
                                        n00b_nt_id_t    nt_id,
                                        int32_t         cost,
                                        bool            penalty,
                                        int32_t         link_ix,
                                        int             n,
                                        n00b_match_t   *items);

/**
 * @brief Build a `N00B_MATCH_GROUP` match item during reconstruction.
 *
 * Allocates the backing `n00b_rule_group_t` with the given quantifier
 * bounds and contents NT id, mirroring `n00b_group_match_v` without
 * creating a fresh group NT (the NT already exists from the baked
 * `n00b_grammar_image_add_nt` calls).
 */
extern n00b_match_t n00b_grammar_image_group(int32_t min,
                                             int32_t max,
                                             int32_t gid,
                                             int64_t contents_id);

/**
 * @brief Finish reconstruction: set the tokenizer and finalize.
 *
 * Resolves @p tokenizer_name (when non-empty) to the registered scan
 * callback exactly as the BNF loader's `@tokenizer(...)` directive
 * does, then finalizes the grammar so derived analysis state is
 * recomputed. After this call the grammar is ready for
 * `n00b_grammar_parse`.
 */
extern void n00b_grammar_image_finish(n00b_grammar_t *g,
                                      n00b_string_t  *tokenizer_name,
                                      uint32_t        max_penalty);

// ============================================================================
// Emitter (build-time)
// ============================================================================

/**
 * @brief Emit C source reconstructing @p g.
 *
 * Walks the finalized grammar @p g and returns C source declaring a
 * `n00b_grammar_t *<symbol_prefix>_build(void)` function plus a
 * `[[gnu::constructor]]` that registers a lazy materializer under
 * @p grammar_name with `n00b_static_grammar_register`. The emitted code
 * uses only `[[gnu::...]]` attribute spellings (never bare
 * `__attribute__((...))`) per n00b-api-guidelines § 2.5.
 *
 * @param g              Finalized grammar to bake.
 * @param symbol_prefix  C identifier prefix for emitted symbols.
 * @param grammar_name   Lookup name registered with the runtime.
 * @return The emitted C source as a freshly-allocated n00b string, or
 *         nullptr on failure (e.g. @p g not finalized).
 */
extern n00b_string_t *n00b_grammar_image_emit(n00b_grammar_t *g,
                                              n00b_string_t  *symbol_prefix,
                                              n00b_string_t  *grammar_name);
