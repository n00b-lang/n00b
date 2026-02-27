// n00b_compile.c — N00b-specific compilation pipeline orchestrator.
//
// Wires the generic slay annotation walk to n00b-specific callbacks
// (type-spec translator, tokenizer).

#include "n00b/n00b_compile.h"
#include "slay/infer.h"

n00b_annot_result_t *
n00b_compile_walk(n00b_grammar_t *g, n00b_parse_tree_t *tree)
{
    return n00b_annot_walk_tree_full_ex(g, tree, n00b_tc_translate_type_spec);
}

n00b_annot_result_t *
n00b_compile_walk_result(n00b_parse_result_t *result)
{
    if (!result || !n00b_parse_result_ok(result)) {
        return NULL;
    }

    n00b_grammar_t    *g    = n00b_parse_result_grammar(result);
    n00b_parse_tree_t *tree = n00b_parse_result_tree(result);

    return n00b_compile_walk(g, tree);
}
