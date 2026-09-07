#pragma once

#include "parse/types.h"
#include "lib/tree.h"

core_tree_decl(parse_nt_node_t, parse_token_info_ptr_t);

typedef core_tree_t(parse_nt_node_t, parse_token_info_ptr_t) parse_tree_t;

typedef parse_tree_t *parse_tree_ptr_t;

core_array_decl(parse_tree_ptr_t);

typedef core_array_t(parse_tree_ptr_t) parse_tree_array_t;

bool parse_node_is_token(parse_tree_t *t);

parse_token_info_t *parse_node_token(parse_tree_t *t);

core_option_t(core_string_t) parse_node_name(parse_tree_t *t);

void *parse_tree_walk(parse_grammar_t *g, parse_tree_t *node, void *thunk);

void parse_tree_free(parse_tree_t *t);
