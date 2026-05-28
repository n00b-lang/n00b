#pragma once

/**
 * @file naudit/filter.h
 * @brief Filter helper: register the naudit `match` n00b type +
 *        compile n00b filter expressions against it.
 *
 * Phase 3 (WP-009) ships the libn00b `eval` surface
 * (`include/n00b/eval.h`); this header is the naudit-side wrapper
 * that:
 *   1. Defines the opaque `n00b_naudit_match_t` C struct.
 *   2. Registers it as a proper n00b type via `n00b_type_register`,
 *      attaches extension methods (`text`, `nt`, `line`, `col`,
 *      `end_line`, `end_col`, `child_named`, `has_ancestor`) via
 *      `n00b_type_add_method`, and installs the C backing functions
 *      as FFI bindings via `n00b_ffi_install_simple`. Filter
 *      expressions in rule guidance then use dot-syntax
 *      (`arg.text`, `arg.nt`, ...) against the bound match.
 *   3. Provides `n00b_naudit_filter_compile` that wraps
 *      `n00b_eval_compile_predicate` with `arg_type_name = r"match"`.
 *   4. Provides `n00b_naudit_filter_apply` that builds a match
 *      handle from a parse-tree node + source text and invokes the
 *      predicate.
 *
 * Per D-005, naudit's own exported surface does not expose
 * `.allocator` kwargs (no `_kargs` block on these declarations,
 * matching the existing pre-sweep style).
 */

#include "naudit/naudit.h"
#include "n00b/eval.h"
#include "slay/parse_tree.h"
#include "core/string.h"

/**
 * @brief Opaque match handle.
 *
 * Built per-invocation by `n00b_naudit_filter_apply` and lives only
 * for the duration of the predicate call. The accessor methods
 * registered via `n00b_type_add_method` read the underlying parse-
 * tree node + source-text buffer.
 *
 * Layout intentionally lives in `src/naudit/filter.c` — consumers
 * never field-access it directly.
 */
typedef struct n00b_naudit_match n00b_naudit_match_t;

/**
 * @brief Register the `match` n00b type with extension methods.
 *
 * Idempotent (subsequent calls are no-ops). Safe to invoke before
 * any session creation; `n00b_naudit_filter_session_new` calls this
 * lazily so callers using the library entry point need not invoke
 * it explicitly.
 */
extern void n00b_naudit_match_type_register(void);

/**
 * @brief Allocate an embedded-eval session pre-registered for
 *        naudit filter expressions.
 *
 * Wraps `n00b_eval_session_new`; afterwards
 *   - ensures `n00b_naudit_match_type_register` has run; and
 *   - installs the match-accessor C functions via
 *     `n00b_ffi_install_simple` on the underlying codegen session
 *     so the JIT-emitted method-dispatch path resolves them.
 *
 * @return Ok(session) on success, Err carrying an `n00b_eval_err_t`
 *         on failure.
 */
extern n00b_result_t(n00b_eval_session_t *)
n00b_naudit_filter_session_new(void);

/**
 * @brief Compile a naudit filter expression into a JIT'd predicate.
 *
 * Equivalent to
 *   `n00b_eval_compile_predicate(s, expr_text, r"match")`,
 * plus future hooks for rule-name-aware diagnostics (the @p name
 * argument is currently unused by the implementation but reserved
 * for the Phase 4 rule-file integration point).
 *
 * The expression body references the bound match handle as `arg`,
 * with dot-syntax method calls resolving against the registered
 * `match` n00b type — e.g.,
 *   `r"arg.nt == r\"function_definition\""`,
 *   `r"arg.line > 0"`.
 */
extern n00b_result_t(n00b_eval_predicate_fn_t)
n00b_naudit_filter_compile(n00b_eval_session_t *s,
                           n00b_string_t       *name,
                           n00b_string_t       *expr_text);

/**
 * @brief Construct a match handle from a parse-tree node + the
 *        source-text buffer, then invoke @p fn.
 *
 * The handle is stack-allocated by this function — it does NOT
 * outlive the call. Filter expressions that capture the handle for
 * later use are not supported.
 *
 * This entry point is the Phase 3 surface. Phase 4 introduces
 * `n00b_naudit_match_new` + `n00b_naudit_filter_apply_handle` so
 * the engine can populate captures on the handle BEFORE the
 * predicate is invoked.
 *
 * @param fn          JIT'd predicate from
 *                    `n00b_naudit_filter_compile`.
 * @param match_node  The parse-tree node representing the match.
 * @param src_text    The full source text of the audited file, used
 *                    by `arg.text` to slice the substring backing
 *                    @p match_node.
 *
 * @return true iff @p fn returned true.
 */
extern bool n00b_naudit_filter_apply(
    n00b_eval_predicate_fn_t  fn,
    n00b_parse_tree_t        *match_node,
    n00b_string_t            *src_text);

/**
 * @brief Allocate a heap-resident match handle (WP-009 Phase 4).
 *
 * Per the WP-009 Phase 4 spec the engine needs to populate per-
 * match captures on the match handle BEFORE invoking the JIT'd
 * predicate. Phase 3's `n00b_naudit_filter_apply` stack-allocated
 * the handle inside the call, which prevented capture binding.
 *
 * `n00b_naudit_match_new` returns a fresh handle allocated via
 * `n00b_alloc` (GC-tracked) that the engine can mutate; the
 * predicate then runs against the populated handle through
 * `n00b_naudit_filter_apply_handle`.
 *
 * @param node      Parse-tree node representing the match. Must be
 *                  non-null; the handle's accessors guard on null
 *                  but the engine never passes null.
 * @param src_text  Full source text of the audited file. Used by
 *                  `arg.text` and `arg.capture(...).text` to slice
 *                  the substring backing each node.
 *
 * @return A fresh match handle. Never null on the happy path.
 */
extern n00b_naudit_match_t *
n00b_naudit_match_new(n00b_parse_tree_t *node,
                      n00b_string_t     *src_text);

/**
 * @brief Bind one capture entry on a match handle (WP-009 Phase 4).
 *
 * Called by the engine during the per-match capture-binding walk.
 * Multiple calls with the same @p name overwrite the prior binding
 * silently (callers prevent collisions by allocating distinct names
 * per declaration).
 *
 * @param handle   Match handle from `n00b_naudit_match_new`.
 * @param name     Capture name (without the leading `$`).
 * @param node     Bound parse-tree node (the descendant matched in
 *                 document order against the declared NT).
 */
extern void
n00b_naudit_match_bind_capture(n00b_naudit_match_t *handle,
                               n00b_string_t       *name,
                               n00b_parse_tree_t   *node);

/**
 * @brief Invoke @p fn against a pre-populated match handle.
 *
 * Companion to `n00b_naudit_match_new`. The engine pre-fills any
 * captures the rule declares, then calls this entry point. The
 * handle outlives the call (the engine may inspect it again after
 * the predicate returns).
 *
 * @param fn      JIT'd predicate from `n00b_naudit_filter_compile`.
 * @param handle  Pre-populated handle from `n00b_naudit_match_new`.
 *
 * @return true iff @p fn returned true. Returns false defensively
 *         when @p fn or @p handle is null.
 */
extern bool
n00b_naudit_filter_apply_handle(n00b_eval_predicate_fn_t  fn,
                                n00b_naudit_match_t      *handle);
