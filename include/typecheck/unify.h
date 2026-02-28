/**
 * @file unify.h
 * @brief Union-find and structural unification for the type inference engine.
 *
 * @ingroup typecheck
 *
 * Provides the core unification algorithm: union-find with path compression,
 * structural matching by type kind, occurs-check for infinite types,
 * constraint checking on variable binding, and promotion-aware unification.
 *
 * ## Functions
 *
 * | Function                     | Purpose                                        |
 * |------------------------------|------------------------------------------------|
 * | `n00b_tc_find`               | Follow union-find chain to canonical rep       |
 * | `n00b_tc_is_var`             | Check if (after find) the type is a Var        |
 * | `n00b_tc_is_prim`            | Check if (after find) the type is a Prim       |
 * | `n00b_tc_prim_name`          | Get the name of a Prim type (after find)       |
 * | `n00b_tc_unify`              | Structural unification                         |
 * | `n00b_tc_unify_or_promote`   | Unify, falling back to numeric promotion       |
 * | `n00b_tc_unify_with_coercion`| Unify with promotion + ref-deref coercion      |
 */
#pragma once

#include "typecheck/types.h"

/**
 * @brief Follow union-find forward pointers to the canonical representative.
 *
 * Applies two-step path compression: on each hop, sets
 * `t->forward = t->forward->forward` before advancing.
 *
 * @param t  Type node to resolve.
 * @return   The canonical representative (has `forward == nullptr`).
 */
extern n00b_tc_type_t *n00b_tc_find(n00b_tc_type_t *t);

/**
 * @brief Check if a type resolves to a Var (unbound type variable).
 * @param t  Type node (find is called internally).
 */
extern bool n00b_tc_is_var(n00b_tc_type_t *t);

/**
 * @brief Check if a type resolves to a Prim (primitive type).
 * @param t  Type node (find is called internally).
 */
extern bool n00b_tc_is_prim(n00b_tc_type_t *t);

/**
 * @brief Get the name of a Prim type.
 *
 * @pre `n00b_tc_is_prim(t)` must be true.
 * @param t  Type node (find is called internally).
 * @return   The primitive's name string.
 */
extern n00b_string_t *n00b_tc_prim_name(n00b_tc_type_t *t);

/**
 * @brief Structurally unify two types.
 *
 * On success, one of the types becomes the canonical representative
 * (via forward pointers). On failure, pushes an error to `ctx->errors`.
 *
 * @param ctx  Type-checking context (for error reporting).
 * @param a    First type.
 * @param b    Second type.
 * @return     `true` if unification succeeded.
 */
extern bool n00b_tc_unify(n00b_tc_ctx_t *ctx, n00b_tc_type_t *a, n00b_tc_type_t *b);

/**
 * @brief Unify two types, falling back to numeric promotion.
 *
 * Tries exact unification first. If that fails and both sides resolve
 * to Prim types, checks the promotion graph (via Datalog). On success,
 * records an `N00B_TC_COERCE_PROMOTE` coercion.
 *
 * @param ctx  Type-checking context.
 * @param a    First type.
 * @param b    Second type.
 * @return     `true` if unification or promotion succeeded.
 */
extern bool n00b_tc_unify_or_promote(n00b_tc_ctx_t *ctx,
                                        n00b_tc_type_t *a,
                                        n00b_tc_type_t *b);

/**
 * @brief Unify with full coercion chain: exact, promotion, ref-deref.
 *
 * Tries in order:
 * 1. Exact unification
 * 2. Numeric promotion
 * 3. If `a` is `ref[T]`, try unifying `T` with `b` (auto-deref)
 *
 * @param ctx  Type-checking context.
 * @param a    First type.
 * @param b    Second type.
 * @return     `true` if any coercion succeeded.
 */
extern bool n00b_tc_unify_with_coercion(n00b_tc_ctx_t *ctx,
                                           n00b_tc_type_t *a,
                                           n00b_tc_type_t *b);
