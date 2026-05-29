#pragma once

/**
 * @file pwz_internal.h
 * @internal
 * @brief Private types for the PWZ parser engine.
 */

#include "slay/pwz.h"
#include "slay/annotation.h"
#include "parsers/token_stream.h"
#include "core/pool.h"

// ============================================================================
// PWZ expression graph (built once from grammar)
// ============================================================================

typedef enum {
    PWZ_TOK,
    PWZ_SEQ,
    PWZ_ALT,
    PWZ_CLASS,
    PWZ_ANY,
} pwz_exp_kind_t;

typedef struct pwz_mem_t pwz_mem_t;
typedef struct pwz_exp_t pwz_exp_t;

typedef pwz_exp_t *pwz_exp_ptr_t;

struct pwz_exp_t {
    pwz_mem_t     *mem;
    pwz_exp_kind_t kind;
    union {
        struct {
            int64_t tid;
        } tok;
        struct {
            const char     *name;
            int64_t         nt_id;
            int32_t         rule_ix;
            pwz_exp_ptr_t  *children;  // n00b_alloc_array, fixed at creation
            int32_t         nchildren;
        } seq;
        struct {
            int64_t                    nt_id;
            n00b_list_t(pwz_exp_ptr_t) alts;  // growable via n00b_list_push
        } alt;
        struct {
            n00b_char_class_t cc;
        } cls;
    };
};

// ============================================================================
// Memo records (per-parse, GC-managed)
// ============================================================================

#define PWZ_POS_BOTTOM (-1)

typedef struct pwz_cxt_t pwz_cxt_t;

typedef struct pwz_cxt_node_t {
    pwz_cxt_t             *cxt;
    struct pwz_cxt_node_t *next;
} pwz_cxt_node_t;

struct pwz_mem_t {
    int32_t         start_pos;
    int32_t         end_pos;
    pwz_cxt_node_t *parents;
    pwz_exp_t      *result;
    bool            in_progress;
};

// ============================================================================
// Contexts (per-parse, GC-managed)
// ============================================================================

typedef enum {
    PWZ_CXT_TOP,
    PWZ_CXT_SEQ,
    PWZ_CXT_ALT,
} pwz_cxt_kind_t;

typedef struct pwz_cxt_t {
    pwz_cxt_kind_t kind;
    union {
        struct {
            pwz_mem_t     *mem;
            const char    *name;
            int64_t        nt_id;
            int32_t        rule_ix;
            pwz_exp_ptr_t *left;   // n00b_alloc_array, fixed at creation
            int32_t        nleft;
            pwz_exp_ptr_t *right;  // pointer into grammar exp children
            int32_t        nright;
        } seq;
        struct {
            pwz_mem_t *mem;
        } alt;
    };
} pwz_cxt_t;

// ============================================================================
// Zippers & worklist
// ============================================================================

typedef struct {
    pwz_exp_t *result;
    pwz_mem_t *mem;
} pwz_zipper_t;

typedef n00b_parse_tree_t *n00b_parse_tree_ptr_t;

// ============================================================================
// Parser state (full definition)
// ============================================================================

struct n00b_pwz_parser_t {
    n00b_grammar_t              *grammar;
    pwz_exp_t                   *start_exp;
    pwz_exp_ptr_t               *nt_exps;     // n00b_alloc_array, indexed by NT id
    n00b_list_t(pwz_exp_ptr_t)   all_exps;    // all grammar exp nodes (for memo reset)

    // Per-parse state (GC-managed, cleared on reset)
    n00b_list_t(pwz_zipper_t)    worklist;
    n00b_list_t(pwz_zipper_t)    worklist_swap;
    n00b_list_t(pwz_exp_ptr_t)   tops;

    n00b_token_stream_t              *stream;

    n00b_parse_tree_t           *result_tree;
    n00b_parse_tree_array_t      result_trees;

    pwz_mem_t                   *mem_bottom;
    pwz_exp_t                   *exp_bottom;

    /*
     * WP-017: per-parser pool for the high-churn intermediate
     * state (pwz_mem_t / pwz_cxt_t / pwz_cxt_node_t / pwz_exp_t
     * result-exps + the per-step child / new_left arrays).
     * Previously GC-managed; the GC walked these every cycle for
     * nothing, and on real input that dominated parse cost.
     * Pool is HIDDEN from GC (the only outbound pointers from
     * pool memory go to other pool memory or to the grammar exp
     * graph, which is reachable via p->all_exps independently).
     * Lazily initialized via ensure_pool() on first allocation
     * so contexts that just want the grammar graph don't pay
     * the cost. Destroyed by n00b_pwz_free. Mirrors ncc's
     * per-parse arena algorithmically but uses n00b's pool API.
     */
    n00b_pool_t                  parse_pool;
    n00b_allocator_t            *parse_allocator;
    bool                         pool_initialized;
};
