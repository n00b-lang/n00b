// parse_forest.c - Engine-agnostic parse forest operations.

#include "slay/parse_forest.h"
#include "core/alloc.h"
#include "core/array.h"

// ============================================================================
// Construction
// ============================================================================

n00b_parse_forest_t
n00b_parse_forest_empty(n00b_grammar_t *g)
{
    return (n00b_parse_forest_t){
        .trees   = n00b_array_new(n00b_parse_tree_ptr_t, 0),
        .grammar = g,
    };
}

n00b_parse_forest_t
n00b_parse_forest_new(n00b_grammar_t          *g,
                       n00b_parse_tree_array_t  trees)
{
    return (n00b_parse_forest_t){
        .trees   = trees,
        .grammar = g,
    };
}

// ============================================================================
// Query
// ============================================================================

int32_t
n00b_parse_forest_count(n00b_parse_forest_t *f)
{
    if (!f || !f->trees.data) {
        return 0;
    }

    return (int32_t)f->trees.len;
}

bool
n00b_parse_forest_is_ambiguous(n00b_parse_forest_t *f)
{
    return n00b_parse_forest_count(f) > 1;
}

// ============================================================================
// Access
// ============================================================================

n00b_parse_tree_t *
n00b_parse_forest_tree(n00b_parse_forest_t *f, int32_t ix)
{
    if (!f || !f->trees.data || ix < 0 || (size_t)ix >= f->trees.len) {
        return NULL;
    }

    return f->trees.data[ix];
}

n00b_parse_tree_t *
n00b_parse_forest_best(n00b_parse_forest_t *f)
{
    return n00b_parse_forest_tree(f, 0);
}

// ============================================================================
// Walk
// ============================================================================

void *
n00b_parse_forest_walk(n00b_parse_forest_t *f,
                        int32_t              ix,
                        void                *thunk)
{
    n00b_parse_tree_t *tree = n00b_parse_forest_tree(f, ix);

    if (!tree || !f->grammar) {
        return NULL;
    }

    return n00b_parse_tree_walk(f->grammar, tree, thunk);
}

void *
n00b_parse_forest_walk_best(n00b_parse_forest_t *f,
                             void               *thunk)
{
    return n00b_parse_forest_walk(f, 0, thunk);
}

// ============================================================================
// Cleanup
// ============================================================================

void
n00b_parse_forest_free(n00b_parse_forest_t *f)
{
    if (!f || !f->trees.data) {
        return;
    }

    for (size_t i = 0; i < f->trees.len; i++) {
        n00b_parse_tree_free(f->trees.data[i]);
    }

    n00b_array_free(f->trees);
    f->trees = (n00b_parse_tree_array_t){0};
}
