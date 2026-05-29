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
#include "slay/annotation.h"
#include "core/alloc.h"
#include "adt/result.h"

// ============================================================================
// Reconstruction primitives (called by emitted C source at runtime)
// ============================================================================
//
// Allocator threading (n00b-api-guidelines § 4.1): every reconstruction
// primitive that allocates accepts an optional `.allocator` kwarg
// (default `nullptr` ⇒ runtime default). Where these primitives call the
// slay grammar builder API (`n00b_grammar_new`, `n00b_nonterm`,
// `n00b_register_terminal`, `n00b_register_literal_type`,
// `n00b_grammar_set_terminal_category`, `n00b_grammar_finalize`), that
// API does NOT yet expose `.allocator`, so the kwarg cannot be forwarded
// there from this WP (changing the slay grammar API is out of scope for
// WP-018). It IS forwarded to every allocation these primitives make
// directly — notably the private match list in `n00b_grammar_image_add_rule`.
// See the per-function notes below.

/**
 * @brief Begin reconstructing a baked grammar.
 *
 * Allocates a fresh grammar and disables automatic error-rule
 * generation so the subsequent `n00b_grammar_finalize()` does not
 * re-inject error rules on top of the ones already baked into the
 * image. Returns the grammar the rest of the primitives populate.
 *
 * @kw allocator  Optional allocator (nullptr = runtime default). NOTE:
 *                the grammar itself is allocated by the slay
 *                `n00b_grammar_new()`, which does not yet accept
 *                `.allocator`; the kwarg is accepted for API consistency
 *                and future forwarding but cannot thread through to the
 *                grammar allocation until the slay API gains the kwarg.
 */
extern n00b_grammar_t *n00b_grammar_image_begin()
    _kargs { n00b_allocator_t *allocator = nullptr; };

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
 * @kw allocator       Optional allocator (nullptr = runtime default).
 *                     The NT is allocated by the slay `n00b_nonterm()`,
 *                     which does not yet accept `.allocator`; accepted
 *                     for API consistency, cannot forward until the slay
 *                     API gains the kwarg.
 */
extern void n00b_grammar_image_add_nt(n00b_grammar_t *g,
                                      int64_t         expected_id,
                                      n00b_string_t  *name,
                                      bool            group_nt,
                                      bool            start_nt)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/**
 * @brief Register a fixed-text terminal during reconstruction.
 *
 * Mirrors `n00b_register_terminal`; the returned id is hash-derived
 * from @p name so it matches the source grammar without being stored.
 *
 * @kw allocator  Optional allocator (nullptr = runtime default).
 *                Forwarding blocked: the underlying slay
 *                `n00b_register_terminal()` does not accept `.allocator`.
 */
extern void n00b_grammar_image_add_terminal(n00b_grammar_t *g,
                                            n00b_string_t  *name)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/**
 * @brief Register a named literal type during reconstruction.
 *
 * Mirrors `n00b_register_literal_type`; literal-type ids are dense and
 * assigned in registration order, so types must be replayed in the same
 * order they appear in the source grammar's `literal_type_map`.
 *
 * @kw allocator  Optional allocator (nullptr = runtime default).
 *                Forwarding blocked: the underlying slay
 *                `n00b_register_literal_type()` does not accept
 *                `.allocator`.
 */
extern void n00b_grammar_image_add_literal_type(n00b_grammar_t *g,
                                                n00b_string_t  *name)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/**
 * @brief Attach a terminal category during reconstruction.
 *
 * Mirrors `n00b_grammar_set_terminal_category`.
 *
 * @kw allocator  Optional allocator (nullptr = runtime default).
 *                Forwarding blocked: the underlying slay
 *                `n00b_grammar_set_terminal_category()` does not accept
 *                `.allocator`.
 */
extern void n00b_grammar_image_add_terminal_category(n00b_grammar_t *g,
                                                     int64_t         terminal_id,
                                                     n00b_string_t  *category)
    _kargs { n00b_allocator_t *allocator = nullptr; };

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
 * @kw allocator     Optional allocator (nullptr = runtime default).
 *                   Forwarded to the private match-contents list this
 *                   function allocates directly. (The owning NT's
 *                   `rule_ids` push goes through the slay grammar API,
 *                   which does not yet accept `.allocator`.)
 */
extern void n00b_grammar_image_add_rule(n00b_grammar_t *g,
                                        n00b_nt_id_t    nt_id,
                                        int32_t         cost,
                                        bool            penalty,
                                        int32_t         link_ix,
                                        int             n,
                                        n00b_match_t   *items)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/**
 * @brief Re-attach a baked semantic annotation to a reconstructed rule.
 *
 * Rebuilds an `n00b_annotation_t` from its flattened components and
 * appends it to rule @p rule_ix's annotation list via
 * `n00b_rule_annotate`, faithfully reproducing the per-rule annotation
 * state the BNF loader produces (the loader attaches annotations
 * directly to rules — `attach_annot_to_rule` → `n00b_rule_annotate` —
 * so the round-trip is rule-level, not NT-pending-level: a fresh
 * parse+finalize leaves `pending_annotations` empty and the annotations
 * live only on the rules, and finalize redistributes nothing).
 *
 * The five `n00b_child_ref_t` arguments and the seven `n00b_string_t *`
 * fields are passed by value / pointer exactly as the source annotation
 * carried them (absent strings are `nullptr`, preserving the null-vs-
 * empty distinction); the emitter renders child-refs as the
 * `N00B_CHILD_IX` / `N00B_CHILD_NT` compound-literal macros.
 *
 * @param g                Grammar under reconstruction.
 * @param rule_ix          Global rule index the annotation attaches to.
 * @param kind             Annotation kind.
 * @param name_ref         `name_ref` child reference.
 * @param type_ref         `type_ref` child reference.
 * @param value_ref        `value_ref` child reference.
 * @param notrivia_ref     `notrivia_ref` child reference.
 * @param adt_keyword_ref  `adt_keyword_ref` child reference.
 * @param scope_tag        `scope_tag` (nullptr if absent).
 * @param type_spec        `type_spec` (nullptr if absent).
 * @param infer_expr       `infer_expr` (nullptr if absent).
 * @param adt_kind         `adt_kind` (nullptr if absent).
 * @param visibility_spec  `visibility_spec` (nullptr if absent).
 * @param op_kind          `op_kind` (nullptr if absent).
 * @param sym_kind         `sym_kind` (nullptr if absent).
 * @param capture_by_tag   `capture_by_tag` flag.
 * @kw allocator           Optional allocator (nullptr = runtime default).
 *                         Forwarding blocked: the underlying
 *                         `n00b_rule_annotate()` allocates the annotation
 *                         copy itself and does not accept `.allocator`.
 */
extern void n00b_grammar_image_add_annotation(n00b_grammar_t   *g,
                                              int32_t           rule_ix,
                                              n00b_annot_kind_t kind,
                                              n00b_child_ref_t  name_ref,
                                              n00b_child_ref_t  type_ref,
                                              n00b_child_ref_t  value_ref,
                                              n00b_child_ref_t  notrivia_ref,
                                              n00b_child_ref_t  adt_keyword_ref,
                                              n00b_string_t    *scope_tag,
                                              n00b_string_t    *type_spec,
                                              n00b_string_t    *infer_expr,
                                              n00b_string_t    *adt_kind,
                                              n00b_string_t    *visibility_spec,
                                              n00b_string_t    *op_kind,
                                              n00b_string_t    *sym_kind,
                                              bool              capture_by_tag)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/**
 * @brief Build a `N00B_MATCH_GROUP` match item during reconstruction.
 *
 * Allocates the backing `n00b_rule_group_t` with the given quantifier
 * bounds and contents NT id, mirroring `n00b_group_match_v` without
 * creating a fresh group NT (the NT already exists from the baked
 * `n00b_grammar_image_add_nt` calls).
 *
 * @kw allocator  Optional allocator (nullptr = runtime default).
 *                Forwarded to the `n00b_alloc` of the backing
 *                `n00b_rule_group_t`.
 */
extern n00b_match_t n00b_grammar_image_group(int32_t min,
                                             int32_t max,
                                             int32_t gid,
                                             int64_t contents_id)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/**
 * @brief Finish reconstruction: set the tokenizer and finalize.
 *
 * Resolves @p tokenizer_name (when non-empty) to the registered scan
 * callback exactly as the BNF loader's `@tokenizer(...)` directive
 * does, then finalizes the grammar so derived analysis state is
 * recomputed. After this call the grammar is ready for
 * `n00b_grammar_parse`.
 *
 * @kw allocator  Optional allocator (nullptr = runtime default).
 *                Forwarding blocked: the underlying slay
 *                `n00b_grammar_finalize()` does not accept `.allocator`.
 */
extern void n00b_grammar_image_finish(n00b_grammar_t *g,
                                      n00b_string_t  *tokenizer_name,
                                      uint32_t        max_penalty)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ============================================================================
// Emitter (build-time)
// ============================================================================

/**
 * @brief Error codes for `n00b_grammar_image_emit`.
 *
 * Negative to avoid collision with `errno` (n00b-api-guidelines § 5.1).
 */
typedef enum {
    N00B_GRAMMAR_IMAGE_OK            = 0,
    N00B_GRAMMAR_IMAGE_ERR_NULL_ARG  = -1, ///< A required argument was null.
    N00B_GRAMMAR_IMAGE_ERR_NOT_FINAL = -2, ///< @p g was not finalized.
} n00b_grammar_image_err_t;

/**
 * @brief Human-readable description for a `n00b_grammar_image_emit` error.
 *
 * @param err  An `n00b_grammar_image_err_t` value (passed as the generic
 *             `n00b_err_t` carried by `n00b_result_t`).
 * @return A static description string (never null).
 */
extern n00b_string_t *n00b_grammar_image_emit_err_str(n00b_err_t err);

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
 * @kw allocator         Optional allocator (nullptr = runtime default).
 *                       Forwarded to the internal `n00b_list_new_private`
 *                       parts accumulator and the `n00b_string_from_cstr`
 *                       fragments this emitter allocates. (`n00b_cformat`
 *                       uses checked variadics and has no `.allocator`
 *                       kwarg, so its allocations use the runtime
 *                       default; this is a slay/string-API limit, not an
 *                       omission.)
 * @return `n00b_result_ok` wrapping the emitted C source as a
 *         freshly-allocated n00b string, or `n00b_result_err` with a
 *         `n00b_grammar_image_err_t` code on failure (e.g. @p g not
 *         finalized or a required argument is null).
 */
extern n00b_result_t(n00b_string_t *)
n00b_grammar_image_emit(n00b_grammar_t *g,
                        n00b_string_t  *symbol_prefix,
                        n00b_string_t  *grammar_name)
    _kargs { n00b_allocator_t *allocator = nullptr; };
