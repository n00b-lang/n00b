#pragma once

#include "parse/types.h"
#include "lib/tree.h"

ncc_tree_decl(ncc_nt_node_t, ncc_token_info_ptr_t);

typedef ncc_tree_t(ncc_nt_node_t, ncc_token_info_ptr_t) ncc_parse_tree_t;

typedef ncc_parse_tree_t *ncc_parse_tree_ptr_t;

ncc_array_decl(ncc_parse_tree_ptr_t);

typedef ncc_array_t(ncc_parse_tree_ptr_t) ncc_parse_tree_array_t;

static inline int64_t
ncc_pt_nt_id(ncc_parse_tree_t *t)
{
    if (!t || ncc_tree_is_leaf(t)) {
        return -1;
    }
    return ncc_tree_node_value(t).id;
}

static inline size_t
ncc_pt_num_children(ncc_parse_tree_t *t)
{
    if (!t) {
        return 0;
    }
    return ncc_tree_num_children(t);
}

bool ncc_parse_node_is_token(ncc_parse_tree_t *t);

ncc_token_info_t *ncc_parse_node_token(ncc_parse_tree_t *t);

ncc_option_t(ncc_string_t) ncc_parse_node_name(ncc_parse_tree_t *t);

void *ncc_parse_tree_walk(ncc_grammar_t *g, ncc_parse_tree_t *node, void *thunk);

void ncc_parse_tree_free(ncc_parse_tree_t *t);
