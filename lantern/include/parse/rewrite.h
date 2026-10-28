#pragma once

#include "parse/tree.h"
#include "parse/grammar.h"

parse_tree_t *parse_tree_clone(const parse_tree_t *tree);

parse_tree_t *parse_tree_make_nt(parse_grammar_t *g,
                                   core_string_t   nt_name,
                                   int32_t        rule_index);

parse_tree_t *parse_tree_make_synthetic_token(int64_t tid, core_string_t text);
