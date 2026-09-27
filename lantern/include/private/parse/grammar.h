#pragma once

#include "parse/grammar.h"
#include "lib/list.h"
#include "lib/dict.h"
#include "lib/cache.h"

#include <pthread.h>

core_list_decl(parse_match_t);
core_list_decl(parse_terminal_t);
core_list_decl(parse_rule_t);
core_list_decl(parse_nonterm_t);

typedef parse_annotation_t *parse_annotation_ptr_t;
core_list_decl(parse_annotation_ptr_t);

typedef parse_nonterm_t *parse_nonterm_ptr_t;
core_list_decl(parse_nonterm_ptr_t);

struct parse_terminal_t {
    core_string_t value;
    void         *user_data;
    int64_t       id;
};

struct parse_nonterm_t {
    core_string_t                    name;
    core_list_t(int32_t)             rule_ids;
    int32_t                         unit;
    bool                            defined_here;
    parse_walk_action_t               action;
    void                            *user_data;
    core_list_t(parse_annotation_ptr_t) annotations;
    core_dict_t             *first_set;
    int64_t                          id;
    bool                             group_nt;
    bool                             empty_is_error;
    bool                             error_nulled;
    bool                             finalized;
    bool                             nullable;
    bool                             no_error_rule;
    bool                             first_has_any;
};

struct parse_rule_t {
    parse_nt_id_t                          nt_id;
    core_list_t(parse_match_t)             contents;
    core_dict_t                  *first_set;
    int32_t                               cost;
    int32_t                               link_ix;
    core_string_t                         doc;
    core_list_t(parse_annotation_ptr_t)     annotations;
    core_string_t                         disambig_predicate;
    int32_t                               disambig_cost;
    bool                                  penalty_rule;
    bool                                  first_has_any;
    void                                 *thunk;
};

struct parse_rule_group_t {
    int64_t contents_id;
    int32_t min;
    int32_t max;
    int32_t gid;
};

struct parse_grammar_t {
    core_list_t(parse_terminal_t)   named_terms;
    core_list_t(parse_rule_t) rules;
    core_list_t(parse_nonterm_ptr_t) nt_list;
    core_dict_t           *nt_map;
    core_dict_t           *terminal_map;
    core_string_t                   tokenizer_name;
    uint32_t                       max_penalty;
    bool                           error_rules;
    bool                           finalized;
    bool                           has_adjacency;
    bool                           hide_penalty_rewrites;
    bool                           hide_groups;
    int                            suspend_penalty_hiding;
    bool                           suspend_group_hiding;
    parse_walk_action_t             default_action;
    void                          *tokenize_cb;
    const void                    *lex_spec;
    void                          *lex_compiled;
    bool                           lex_spec_owned;
    bool                           lex_resolved;

    core_list_t(core_string_t) terminal_categories;
    bool                      has_terminal_categories;
    core_list_t(int32_t)      contextual_sources;

    core_list_t(parse_annotation_ptr_t) annotations;

    core_cache_t annot_cache;
    pthread_mutex_t annot_cache_lock;
};

static inline int32_t
parse_contextual_source_tid(const parse_grammar_t *g, int64_t terminal_id)
{
    int64_t ix = terminal_id - PARSE_TOK_START_ID - 1;

    if (!g || ix < 0 || (size_t)ix >= g->contextual_sources.len) {
        return 0;
    }

    return g->contextual_sources.data[ix];
}

void parse_grammar_prepare_contextual(parse_grammar_t *g,
                                      const core_string_t *spellings,
                                      core_string_t source_name);

static inline parse_nonterm_t *
parse_get_nonterm(parse_grammar_t *g, int64_t id)
{
    if (id < 0 || (size_t)id >= core_list_len(g->nt_list)) {
        return nullptr;
    }
    return g->nt_list.data[id];
}

static inline int32_t
parse_nt_unit(parse_grammar_t *g, core_string_t name)
{
    parse_nonterm_t *nt = parse_find_nonterm(g, name);

    return nt ? nt->unit : 0;
}

static inline parse_terminal_t *
parse_get_terminal(parse_grammar_t *g, int64_t id)
{
    int64_t ix = id - PARSE_TOK_START_ID - 1;
    if (ix < 0 || (size_t)ix >= core_list_len(g->named_terms)) {
        return nullptr;
    }
    return &g->named_terms.data[ix];
}

static inline parse_rule_t *
parse_get_rule(parse_grammar_t *g, int32_t ix)
{
    if (ix < 0 || (size_t)ix >= core_list_len(g->rules)) {
        return nullptr;
    }
    return &g->rules.data[ix];
}

static inline bool
parse_is_non_terminal(parse_match_t *m)
{
    return m->kind == PARSE_MATCH_NT || m->kind == PARSE_MATCH_GROUP;
}

static inline parse_rule_t *
parse_get_node_rule(parse_grammar_t *g, parse_nt_node_t *pn)
{
    parse_nonterm_t *nt = parse_get_nonterm(g, pn->id);

    if (!nt
            || pn->rule_index < 0
            || (size_t)pn->rule_index >= core_list_len(nt->rule_ids)) {
        return nullptr;
    }

    int32_t global_ix = core_list_get(nt->rule_ids, pn->rule_index);

    return parse_get_rule(g, global_ix);
}

static inline bool
parse_hide_groups(parse_grammar_t *g)
{
    return g->hide_groups && !g->suspend_group_hiding;
}

static inline bool
parse_hide_penalties(parse_grammar_t *g)
{
    return g->hide_penalty_rewrites && !g->suspend_penalty_hiding;
}
