#pragma once

#include "parse/types.h"
#include "parse/parse_tree.h"
#include "parse/grammar.h"
#include "parse/annotation.h"

typedef struct ncc_tree_walk_t ncc_tree_walk_t;

typedef struct {
    ncc_tree_walk_t  *walk;
    ncc_grammar_t    *grammar;
    ncc_parse_tree_t *node;
    ncc_parse_tree_t *parent;
    ncc_nt_node_t    *pn;
    ncc_nonterm_t    *nt;
    ncc_parse_rule_t *rule;
    ncc_annotation_t *annot;
    int32_t           depth;
    int32_t           child_index;
    void             *thunk;
    void             *user;
} ncc_visit_t;

typedef bool (*ncc_visit_fn)(ncc_visit_t *v);

ncc_tree_walk_t *ncc_tree_walk_new(ncc_grammar_t *g, void *user);
void             ncc_tree_walk_free(ncc_tree_walk_t *w);
void            *ncc_tree_walk_user(ncc_tree_walk_t *w);

void ncc_walk_on_node(ncc_tree_walk_t *w, ncc_visit_fn pre, ncc_visit_fn post,
                      void *thunk);

bool ncc_walk_on_nt(ncc_tree_walk_t *w, ncc_string_t nt_name, ncc_visit_fn pre,
                    ncc_visit_fn post, void *thunk);

void ncc_walk_on_annot_phase(ncc_tree_walk_t *w, ncc_annot_kind_t kind,
                             int32_t phase, ncc_visit_fn pre,
                             ncc_visit_fn post, void *thunk);

void ncc_walk_on_annot(ncc_tree_walk_t *w, ncc_annot_kind_t kind,
                       ncc_visit_fn pre, ncc_visit_fn post, void *thunk);

void ncc_walk_on_leaf(ncc_tree_walk_t *w, ncc_visit_fn fn, void *thunk);

void ncc_tree_walk_run(ncc_tree_walk_t *w, ncc_parse_tree_t *root);

void ncc_tree_walk_stop(ncc_tree_walk_t *w);
bool ncc_tree_walk_stopped(ncc_tree_walk_t *w);
