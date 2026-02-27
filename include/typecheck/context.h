/**
 * @file context.h
 * @brief Type-checking context — owns all types, caches built-in
 *        primitives, and embeds a Datalog engine for interface /
 *        promotion queries.
 *
 * @ingroup typecheck
 *
 * The context is the root object for a type-checking session.  Every
 * type allocated through the constructors in `construct.h` is
 * registered on a context, and the 16 built-in primitive types are
 * pre-created at construction time.
 *
 * ## Datalog relations
 *
 * | Relation         | Arity | Meaning                              |
 * |------------------|-------|--------------------------------------|
 * | `implements`     | 2     | `implements(type_name, iface_name)`  |
 * | `promotes`       | 2     | `promotes(from_name, to_name)`       |
 * | `iface_param`    | 3     | `iface_param(iface, param, type_id)` |
 *
 * A transitive rule is installed automatically:
 * ```
 * promotes(A, C) :- promotes(A, B), promotes(B, C).
 * ```
 */
#pragma once

#include "typecheck/types.h"
#include "logic/logic_program.h"

// List-of-struct declarations needed for the context.
n00b_list_decl(n00b_tc_iface_t);
n00b_list_decl(n00b_tc_impl_t);

/**
 * @brief Type-checking context.
 *
 * Owns every type node created during a session, caches the 16
 * built-in primitive types, and embeds a Datalog engine for
 * relational queries (implements, promotes, iface_param).
 */
struct n00b_tc_ctx_s {
    // -- Type ownership --------------------------------------------------
    n00b_list_t(n00b_tc_type_t *) *all_types;
    uint32_t                       next_var_id;

    // -- Built-in primitive cache ----------------------------------------
    n00b_tc_type_t *t_int;
    n00b_tc_type_t *t_i8,  *t_i16, *t_i32, *t_i64;
    n00b_tc_type_t *t_u8,  *t_u16, *t_u32, *t_u64;
    n00b_tc_type_t *t_f32, *t_f64;
    n00b_tc_type_t *t_bool, *t_string, *t_nil, *t_void;

    // -- Datalog engine (embedded) ---------------------------------------
    n00b_logic_t     logic;
    n00b_dl_rel_id_t rel_implements;  /**< implements(type, iface) */
    n00b_dl_rel_id_t rel_promotes;    /**< promotes(from, to)      */
    n00b_dl_rel_id_t rel_iface_param; /**< iface_param(iface, param, type_id) */

    // -- Interface + implementation registry -----------------------------
    n00b_list_t(n00b_tc_iface_t) *interfaces;
    n00b_list_t(n00b_tc_impl_t)  *implementations;

    // -- Error / coercion accumulator ------------------------------------
    n00b_list_t(n00b_tc_error_t)    *errors;
    n00b_list_t(n00b_tc_coercion_t) *coercions;

    bool logic_dirty; /**< True when new facts added since last run. */
};

// ============================================================================
// Lifecycle
// ============================================================================

/**
 * @brief Create a new type-checking context.
 *
 * Initialises the Datalog engine, registers the three relations,
 * installs the transitive promotion rule, and creates 16 built-in
 * primitive types.
 *
 * @return Heap-allocated context.  Free with n00b_tc_ctx_free().
 */
extern n00b_tc_ctx_t *n00b_tc_ctx_new(void);

/**
 * @brief Free a type-checking context and its Datalog engine.
 * @param ctx Context to free.
 */
extern void n00b_tc_ctx_free(n00b_tc_ctx_t *ctx);

// ============================================================================
// Type registration (called internally by constructors)
// ============================================================================

/**
 * @brief Register a freshly-allocated type on the context.
 *
 * Called by the constructors in `construct.c`.  Appends the type to
 * `ctx->all_types`.
 *
 * @param ctx  Owning context.
 * @param type Type node to register.
 */
extern void n00b_tc_ctx_register(n00b_tc_ctx_t *ctx, n00b_tc_type_t *type);

// ============================================================================
// Interface + implementation registration
// ============================================================================

/**
 * @brief Define a new interface with named type parameters.
 *
 * @param ctx    Context.
 * @param name   Interface name (e.g., `*r"Indexable"`).
 * @param ...    `n00b_tc_iface_param_t` values (struct-typed varargs).
 */
extern void n00b_tc_register_iface(n00b_tc_ctx_t *ctx, n00b_string_t name,
                                     n00b_tc_iface_param_t +);

/**
 * @brief Register a type as implementing an interface.
 *
 * Asserts `implements(type_name, iface_name)` in the Datalog engine
 * and records the concrete bindings for each interface parameter.
 *
 * @param ctx        Context.
 * @param type_name  Implementing type (e.g., `*r"dict"`).
 * @param iface_name Interface (e.g., `*r"Indexable"`).
 * @param ...        Concrete binding types as `n00b_tc_type_t *` varargs.
 */
extern void n00b_tc_register_impl(n00b_tc_ctx_t *ctx,
                                    n00b_string_t type_name,
                                    n00b_string_t iface_name,
                                    n00b_tc_type_t *+);

/**
 * @brief Register a direct promotion relationship.
 *
 * Asserts `promotes(from_name, to_name)` in the Datalog engine.
 * Transitive closure is computed automatically by the installed rule.
 *
 * @param ctx       Context.
 * @param from_name Source type name.
 * @param to_name   Target type name.
 */
extern void n00b_tc_register_promotion(n00b_tc_ctx_t *ctx,
                                         n00b_string_t from_name,
                                         n00b_string_t to_name);

// ============================================================================
// Queries
// ============================================================================

/**
 * @brief Query whether a type implements an interface.
 *
 * Runs the Datalog engine if dirty, then queries the `implements`
 * relation.
 *
 * @param ctx        Context.
 * @param type_name  Type to check.
 * @param iface_name Interface to check.
 * @return           `true` if the type implements the interface.
 */
extern bool n00b_tc_implements(n00b_tc_ctx_t *ctx,
                                 n00b_string_t type_name,
                                 n00b_string_t iface_name);

/**
 * @brief Query whether one type promotes to another (transitively).
 *
 * Runs the Datalog engine if dirty, then queries the `promotes`
 * relation.
 *
 * @param ctx       Context.
 * @param from_name Source type.
 * @param to_name   Target type.
 * @return          `true` if promotion is possible.
 */
extern bool n00b_tc_promotes_to(n00b_tc_ctx_t *ctx,
                                  n00b_string_t from_name,
                                  n00b_string_t to_name);

// ============================================================================
// Primitive lookup
// ============================================================================

/**
 * @brief Look up a built-in primitive type by name.
 *
 * Compares @p name against the 16 built-in primitive names and returns
 * the cached type pointer if found.
 *
 * @param ctx   Context with built-in cache.
 * @param name  Primitive name (e.g., `*r"int"`, `*r"bool"`).
 * @return      The cached primitive type, or `nullptr` if not a built-in.
 */
extern n00b_tc_type_t *n00b_tc_lookup_prim(n00b_tc_ctx_t *ctx,
                                              n00b_string_t name);

