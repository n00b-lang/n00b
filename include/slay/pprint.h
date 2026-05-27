#pragma once

/**
 * @file pprint.h
 * @brief Pretty-print a slay parse tree to a libn00b string.
 *
 * Re-introduces Wadler/Lindig two-phase pretty-printing into slay.
 * Port origin: `slop/src/slay/pprint.c` (with the slay surface
 * adapted to libn00b's `n00b_grammar_t` / `n00b_parse_tree_t`).
 *
 * ## Algorithm overview
 *
 * The implementation runs in two phases:
 *
 *   - **Phase 1 — document command stream.** Walk the parse tree.
 *     For each terminal leaf emit a `DOC_TEXT` command carrying the
 *     token text. For each non-terminal interior node, look up its
 *     formatting annotations on the grammar
 *     (`@indent`, `@group`, `@concat`, `@hardline`,
 *     `@softline`, `@nospace`, `@align`, `@blankline`) and emit
 *     corresponding doc commands around / between children.
 *
 *   - **Phase 2 — layout resolution.** For each `DOC_GROUP_BEGIN`,
 *     measure the flat width of the enclosed sub-document. If the
 *     group fits in the remaining columns of the current line at
 *     `line_width`, render it flat (softlines become spaces);
 *     otherwise fall back to break mode (softlines become newlines
 *     at the current indent level). `DOC_INDENT` /  `DOC_DEDENT`
 *     bump the indent stack; hardlines and blanklines always break.
 *
 * Final characters are emitted into an `n00b_buffer_t` and the
 * buffer is converted to an `n00b_string_t *` for return.
 *
 * ## Annotation mapping
 *
 *   - `@indent` on an NT → increase indent level around its
 *     children, decrease afterwards. The dedent is emitted *before*
 *     the highest-indexed `@hardline` child so a closing delimiter
 *     ends up at the parent's indent level.
 *   - `@group` on an NT → wrap the NT's children in a
 *     group (try-flat-first).
 *   - `@concat` on an NT → suppress automatic spacing between the
 *     NT's children.
 *   - `@hardline(idx)` → unconditional newline before child `idx`.
 *   - `@softline(idx)` → newline if the surrounding group breaks,
 *     space otherwise, before child `idx`.
 *   - `@nospace(idx)` → suppress the heuristic space before child
 *     `idx`.
 *   - `@align(idx)` → re-align subsequent lines to the current
 *     column while emitting child `idx`.
 *   - `@blankline` on an NT → emit a blank line after the NT.
 *
 * Per-child precision (e.g. `@hardline($2)` meaning "hardline
 * before child index 2") is honored only when the annotation's
 * child reference uses `N00B_ROLE_BY_INDEX`. Name-based child
 * references (`N00B_ROLE_BY_NAME`) are ignored by the
 * pretty-printer at present; if a grammar needs them, this is the
 * place to extend.
 *
 * ## Error codes (negative; domain-specific to slay pprint)
 */

#include "slay/grammar.h"
#include "slay/parse_tree.h"
#include "core/string.h"
#include "adt/result.h"

// ============================================================================
// Error codes
// ============================================================================

/** @brief Null grammar or null tree passed to @ref n00b_pretty_print. */
#define N00B_ERR_PPRINT_NULL_INPUT  (-1)

/** @brief Internal failure during layout resolution (should not occur
 *         in well-formed input — present so the error path is typed). */
#define N00B_ERR_PPRINT_INTERNAL    (-2)

/**
 * @brief Return a short, static description for a pretty-print error code.
 *
 * @param err  Error code returned via `n00b_result_get_err`.
 * @return     Static description, or a generic "(unknown)" string.
 */
extern n00b_string_t *n00b_pretty_print_err_str(n00b_err_t err);

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Pretty-print a slay parse tree to a libn00b string.
 *
 * The grammar is consulted for non-terminal names and per-NT
 * annotations; the parse tree is walked depth-first to produce a
 * document command stream, then a Wadler/Lindig layout pass renders
 * the document into a final string at the requested width.
 *
 * @param g     Grammar the tree was parsed from (or that the tree
 *              still validates against after a transformation pass).
 *              Provides NT-name lookup and per-NT formatting
 *              annotations.
 * @param tree  Root of the parse tree to pretty-print.
 *
 * @return On success, `n00b_result_ok(n00b_string_t *, formatted)`.
 *         On null input, an err result with
 *         `N00B_ERR_PPRINT_NULL_INPUT`.
 *
 * @kw line_width   Target line width in columns before a group breaks.
 *                  Values <= 0 are treated as the default. (default: 80)
 * @kw indent_size  Per-level indent in columns when indenting with
 *                  spaces; per-level tab count when indenting with
 *                  tabs. Values <= 0 are treated as the default.
 *                  (default: 4)
 * @kw indent_tabs  If true, use a tab character per indent level;
 *                  otherwise use `indent_size` spaces per level.
 *                  (default: false)
 * @kw newline      Newline string to emit at line breaks. nullptr
 *                  selects the platform-neutral default ("\n").
 *                  (default: nullptr → "\n")
 * @kw allocator    Allocator for the returned string and all
 *                  internal scratch. nullptr = runtime default.
 *                  (default: nullptr)
 *
 * @pre  @p g and @p tree are non-nullptr.
 * @post On `ok`, the returned string is independently allocated
 *       under `.allocator`.
 */
extern n00b_result_t(n00b_string_t *) n00b_pretty_print(
    n00b_grammar_t *g, n00b_parse_tree_t *tree) _kargs
{
    int64_t           line_width  = 80;
    int64_t           indent_size = 4;
    bool              indent_tabs = false;
    n00b_string_t    *newline     = nullptr;
    n00b_allocator_t *allocator   = nullptr;
};
