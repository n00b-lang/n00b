#pragma once

#include "parse/types.h"
#include "parse/tree.h"
#include "parse/grammar.h"
#include "parse/annotation.h"

typedef struct parse_tree_walk_t parse_tree_walk_t;

typedef struct {
    parse_tree_walk_t  *walk;
    parse_grammar_t    *grammar;
    parse_tree_t *node;
    parse_tree_t *parent;
    parse_nt_node_t    *pn;
    parse_nonterm_t    *nt;
    parse_rule_t *rule;
    parse_annotation_t *annot;
    int32_t           depth;
    int32_t           child_index;
    void             *thunk;
    void             *user;
    parse_tree_t *replace;
} parse_visit_t;

typedef bool (*parse_visit_fn)(parse_visit_t *v);

parse_tree_walk_t *parse_tree_walk_new(parse_grammar_t *g, void *user);
void             parse_tree_walk_free(parse_tree_walk_t *w);
void            *parse_tree_walk_user(parse_tree_walk_t *w);

void parse_walk_on_node(parse_tree_walk_t *w, parse_visit_fn pre, parse_visit_fn post,
                      void *thunk);

bool parse_walk_on_nt(parse_tree_walk_t *w, core_string_t nt_name, parse_visit_fn pre,
                    parse_visit_fn post, void *thunk);

void parse_walk_on_annot_phase(parse_tree_walk_t *w, parse_annot_kind_t kind,
                             int32_t phase, parse_visit_fn pre,
                             parse_visit_fn post, void *thunk);

void parse_walk_on_annot(parse_tree_walk_t *w, parse_annot_kind_t kind,
                       parse_visit_fn pre, parse_visit_fn post, void *thunk);

void parse_walk_on_leaf(parse_tree_walk_t *w, parse_visit_fn fn, void *thunk);

void parse_tree_walk_run(parse_tree_walk_t *w, parse_tree_t *root);
/* Preserve a subtree root's position in its owning tree during the walk. */
void parse_tree_walk_run_with_parent(parse_tree_walk_t *w,
                                     parse_tree_t *root,
                                     parse_tree_t *parent,
                                     int32_t depth,
                                     int32_t child_index);

void parse_tree_walk_apply_node(parse_tree_walk_t *w, parse_tree_t *node,
                                parse_tree_t *parent, int32_t depth,
                                int32_t child_index,
                                const parse_annot_kind_t *kinds,
                                size_t kind_count, bool entering);

typedef struct {
    uint32_t magic;
    void   **v;
    size_t   n;
} parse_reduce_list_t;

typedef struct {
    parse_tree_walk_t  *walk;
    parse_grammar_t    *grammar;
    parse_tree_t *node;
    void            **kids;
    size_t            n_kids;
    void             *thunk;
    void             *user;
} parse_reduce_t;

parse_reduce_list_t *parse_reduce_as_list(void *result);
parse_reduce_list_t *parse_reduce_list_new(size_t n);

typedef void *(*parse_reduce_fn)(parse_reduce_t *r);

bool parse_walk_reduce_nt(parse_tree_walk_t *w, core_string_t nt_name,
                        parse_reduce_fn fn, void *thunk);
void parse_walk_reduce_default(parse_tree_walk_t *w, parse_reduce_fn fn,
                             void *thunk);
void parse_walk_reduce_leaf(parse_tree_walk_t *w, parse_reduce_fn fn, void *thunk);
void parse_walk_reduce_leaf_in(parse_tree_walk_t *w, int32_t unit,
                             parse_reduce_fn fn, void *thunk);

void *parse_tree_walk_reduce(parse_tree_walk_t *w, parse_tree_t *root);

void parse_tree_walk_stop(parse_tree_walk_t *w);
bool parse_tree_walk_stopped(parse_tree_walk_t *w);

parse_tree_t *parse_tree_walk_rewrite(parse_tree_walk_t  *w,
                                        parse_tree_t *root);

uint32_t parse_tree_walk_replaced(const parse_tree_walk_t *w);
