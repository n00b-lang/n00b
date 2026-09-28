#pragma once

#include "parse/types.h"
#include "parse/tree.h"
#include "parse/grammar.h"
#include "parse/annotation.h"

bool parse_alt_leads_with(parse_grammar_t *g, core_string_t nt_name,
                        int32_t rule_ix, core_string_t word);

typedef bool (*parse_leaf_fn)(parse_tree_t *leaf, void *arg);

parse_tree_t *parse_tree_scan_leaves(parse_tree_t *root, bool reverse,
                                       parse_leaf_fn fn, void *arg);

core_string_t parse_tree_extract_first_identifier(parse_tree_t *node);

parse_token_info_t *parse_tree_find_first_terminal(parse_tree_t *node);

parse_tree_t *parse_tree_find_child_by_nt_name(parse_grammar_t    *g,
                                                 parse_tree_t *parent,
                                                 core_string_t      name);

parse_tree_t *parse_tree_get_nth_nt_child(parse_tree_t *node, int32_t n);

parse_tree_t *parse_tree_resolve_child_ref(parse_grammar_t    *g,
                                             parse_tree_t *parent,
                                             parse_child_ref_t   ref);

core_string_t parse_tree_nt_name(parse_grammar_t *g, parse_tree_t *n);
bool parse_tree_is_nt(parse_grammar_t *g, parse_tree_t *n, core_string_t name);
const parse_rule_t *parse_tree_rule(parse_grammar_t *g, parse_tree_t *n);

core_string_t parse_tree_leaf_text(parse_tree_t *n);
int64_t      parse_tree_leaf_tid(parse_tree_t *n);
bool         parse_tree_leaf_is(parse_tree_t *n, core_string_t text);
