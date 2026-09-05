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
    // Set only when the visitor was registered against an annotation kind.
    ncc_annotation_t *annot;
    int32_t           depth;
    // Position of this node among its parent's children, or -1 at the root.
    int32_t           child_index;
    void             *thunk;
    void             *user;
} ncc_visit_t;

// A pre-order visitor returning false suppresses the node's children. Its
// post-order counterpart still runs, so a visitor that pushed state in pre
// can always pop it in post.
typedef bool (*ncc_visit_fn)(ncc_visit_t *v);

ncc_tree_walk_t *ncc_tree_walk_new(ncc_grammar_t *g, void *user);
void             ncc_tree_walk_free(ncc_tree_walk_t *w);
void            *ncc_tree_walk_user(ncc_tree_walk_t *w);

// Fires on every interior node.
void ncc_walk_on_node(ncc_tree_walk_t *w, ncc_visit_fn pre, ncc_visit_fn post,
                      void *thunk);

// Fires on interior nodes produced by one non-terminal. Registering a name the
// grammar does not define is an error; the call returns false.
bool ncc_walk_on_nt(ncc_tree_walk_t *w, ncc_string_t nt_name, ncc_visit_fn pre,
                    ncc_visit_fn post, void *thunk);

// Fires once per matching annotation on the node, in grammar order, with
// v->annot set. Annotations declared on the non-terminal are seen before
// those declared on the matched rule.
//
// Phase orders registrations against each other rather than against the
// grammar: every phase-0 visitor runs over all of the node's annotations
// before any phase-1 visitor does. That is what lets a registration read what
// an earlier one produced no matter which order the two annotations were
// written in on the rule. Post-order dispatch runs phases in reverse.
void ncc_walk_on_annot_phase(ncc_tree_walk_t *w, ncc_annot_kind_t kind,
                             int32_t phase, ncc_visit_fn pre,
                             ncc_visit_fn post, void *thunk);

// Phase 0.
void ncc_walk_on_annot(ncc_tree_walk_t *w, ncc_annot_kind_t kind,
                       ncc_visit_fn pre, ncc_visit_fn post, void *thunk);

// Fires on every terminal (pre only; leaves have no children).
void ncc_walk_on_leaf(ncc_tree_walk_t *w, ncc_visit_fn fn, void *thunk);

// Group wrappers (from ?, * and + quantifiers) and error nodes name no
// non-terminal, so they reach node visitors but never nt or annot visitors.
// Pre-order dispatch runs node, then nt, then annot visitors; post-order runs
// them in the reverse order so registrations nest.
void ncc_tree_walk_run(ncc_tree_walk_t *w, ncc_parse_tree_t *root);

// Ends the walk after the current visitor. Nodes already entered still get
// their post-order visitors, so walk state unwinds cleanly.
void ncc_tree_walk_stop(ncc_tree_walk_t *w);
bool ncc_tree_walk_stopped(ncc_tree_walk_t *w);
