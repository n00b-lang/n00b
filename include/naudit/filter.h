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
