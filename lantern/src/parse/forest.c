
#include "parse/forest.h"
#include "lib/array.h"

parse_forest_t
parse_forest_empty(parse_grammar_t *g)
{
    return (parse_forest_t){
        .trees   = core_array_new(parse_tree_ptr_t, 0),
        .grammar = g,
    };
}

parse_forest_t
parse_forest_new(parse_grammar_t          *g,
                       parse_tree_array_t  trees)
{
    return (parse_forest_t){
        .trees   = trees,
        .grammar = g,
    };
}

int32_t
parse_forest_count(parse_forest_t *f)
{
    if (!f || !f->trees.data) {
        return 0;
    }

    return (int32_t)f->trees.len;
}

bool
parse_forest_is_ambiguous(parse_forest_t *f)
{
    return parse_forest_count(f) > 1;
}

parse_tree_t *
parse_forest_tree(parse_forest_t *f, int32_t ix)
{
    if (!f || !f->trees.data || ix < 0 || (size_t)ix >= f->trees.len) {
        return nullptr;
    }

    return f->trees.data[ix];
}

parse_tree_t *
parse_forest_best(parse_forest_t *f)
{
    return parse_forest_tree(f, 0);
}

void
parse_forest_free(parse_forest_t *f)
{
    if (!f || !f->trees.data) {
        return;
    }

    for (size_t i = 0; i < f->trees.len; i++) {
        parse_tree_free(f->trees.data[i]);
    }

    core_array_free(f->trees);
    f->trees = (parse_tree_array_t){0};
}
