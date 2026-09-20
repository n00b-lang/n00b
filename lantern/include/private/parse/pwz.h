#pragma once

#include "parse/pwz.h"
#include "scanner/token_stream.h"
#include "lib/arena.h"

typedef struct pwz_exp_t pwz_exp_t;
typedef struct pwz_cxt_t pwz_cxt_t;

typedef pwz_exp_t *pwz_exp_ptr_t;

core_list_decl(pwz_exp_ptr_t);

#define PWZ_MEM_IN_PROGRESS_BIT ((int32_t)(1u << 31))

#define PWZ_POS_BOTTOM ((int32_t)0x7FFFFFFE)

typedef struct pwz_cxt_node_t {
    pwz_cxt_t             *cxt;
    struct pwz_cxt_node_t *next;
} pwz_cxt_node_t;

typedef struct pwz_mem_t {
    pwz_cxt_node_t *parents;
    pwz_exp_t      *result;
    int32_t         start_pos;
    int32_t         end_pos;
    uint32_t        refcount;
} pwz_mem_t;

static inline bool
pwz_mem_in_progress(const pwz_mem_t *m)
{
    return (m->end_pos & PWZ_MEM_IN_PROGRESS_BIT) != 0;
}

static inline void
pwz_mem_set_in_progress(pwz_mem_t *m, bool v)
{
    if (v) {
        m->end_pos |= PWZ_MEM_IN_PROGRESS_BIT;
    }
    else {
        m->end_pos &= ~PWZ_MEM_IN_PROGRESS_BIT;
    }
}

static inline int32_t
pwz_mem_end_pos(const pwz_mem_t *m)
{
    return m->end_pos & ~PWZ_MEM_IN_PROGRESS_BIT;
}

typedef enum : uint8_t {
    PWZ_CXT_TOP,
    PWZ_CXT_SEQ,
    PWZ_CXT_ALT,
} pwz_cxt_kind_t;

typedef struct pwz_cxt_t {
    pwz_cxt_kind_t kind;
    uint8_t        _pad;
    int16_t        nt_id;
    int16_t        rule_ix;
    int16_t        nleft;
    union {
        struct {
            pwz_mem_t     *mem;
            pwz_exp_ptr_t *left;
            pwz_exp_ptr_t *right;
            int16_t        nright;
        } seq;
        struct {
            pwz_mem_t *mem;
        } alt;
    };
} pwz_cxt_t;

typedef enum {
    PWZ_TOK,
    PWZ_SEQ,
    PWZ_ALT,
    PWZ_CLASS,
    PWZ_ANY,
    PWZ_SET,
} pwz_exp_kind_t;

struct pwz_exp_t {
    pwz_mem_t     *mem;
    union {
        int32_t tid;
        int16_t nt_id;
        parse_char_class_t cc;
        const parse_match_set_t *set;
    };
    core_string_t contextual_spelling;
    int32_t       match_tid;
    pwz_exp_kind_t kind;
    int16_t         rule_ix;
    int16_t         nchildren;
    union {
        struct {
            core_string_t    name;
            pwz_exp_ptr_t  *children;
        } seq;
        struct {
            core_list_t(pwz_exp_ptr_t) alts;
        } alt;
    };
};

typedef struct {
    pwz_exp_t *result;
    pwz_mem_t *mem;
} pwz_zipper_t;

core_list_decl(pwz_zipper_t);

typedef parse_tree_t *parse_tree_ptr_t;
core_list_decl(parse_tree_ptr_t);

typedef struct {
    pwz_exp_t *exp;
    int32_t    pos;
    int32_t    score;
    int32_t    end_pos;
    bool       visiting;
    bool       done;
} pwz_score_entry_t;

core_list_decl(pwz_score_entry_t);

typedef enum {
    PWZ_STEP_DD,
    PWZ_STEP_DDP,
    PWZ_STEP_DU,
    PWZ_STEP_DUP,
    PWZ_STEP_DU_DONE,
} pwz_step_kind_t;

typedef struct {
    pwz_step_kind_t   kind;
    int32_t           pos;
    parse_token_info_t *tok;
    pwz_cxt_t        *cxt;
    pwz_exp_t        *exp;
    pwz_exp_t        *result;
    pwz_mem_t        *mem;
} pwz_step_t;

core_list_decl(pwz_step_t);

struct parse_pwz_parser_t {
    parse_grammar_t              *grammar;
    parse_nt_id_t                 start_nt;
    pwz_exp_t                   *start_exp;
    pwz_exp_ptr_t               *nt_exps;
    core_list_t(pwz_exp_ptr_t)   all_exps;

    core_arena_t                 parse_arena;
    pwz_mem_t                  *free_mems;
    core_list_t(pwz_zipper_t)    worklist;
    core_list_t(pwz_zipper_t)    worklist_swap;
    core_list_t(pwz_exp_ptr_t)   tops;
    core_list_t(pwz_step_t)      steps;
    core_list_t(pwz_score_entry_t) score_cache;
    int32_t                    *score_index;
    size_t                      score_index_cap;
    bool                        score_cache_enabled;

    pwz_exp_t                  *longest_top;
    int32_t                     longest_end;

    parse_token_stream_t         *stream;

    parse_tree_t           *result_tree;
    parse_tree_array_t      result_trees;

    pwz_mem_t                   *mem_bottom;
    pwz_exp_t                   *exp_bottom;

    int32_t                     error_pos;

    parse_pwz_disambig_fn         disambig_cb;
    void                       *disambig_ud;
    uint32_t                    score_evaluations;
    uint32_t                    score_predicate_calls;
    uint32_t                    score_tie_count;
};

/* Test/diagnostic hook; normal parsers keep score memoization enabled. */
void parse_pwz_set_score_cache_enabled(parse_pwz_parser_t *p, bool enabled);
