#pragma once

#include "parse/types.h"
#include "parse/parse_tree.h"
#include "parse/grammar.h"
#include "parse/annotation.h"

bool ncc_alt_leads_with(ncc_grammar_t *g, ncc_string_t nt_name,
                        int32_t rule_ix, ncc_string_t word);

typedef bool (*ncc_leaf_fn)(ncc_parse_tree_t *leaf, void *arg);

ncc_parse_tree_t *ncc_tree_scan_leaves(ncc_parse_tree_t *root, bool reverse,
                                       ncc_leaf_fn fn, void *arg);

ncc_string_t ncc_tree_extract_first_identifier(ncc_parse_tree_t *node);

ncc_token_info_t *ncc_tree_find_first_terminal(ncc_parse_tree_t *node);

ncc_parse_tree_t *ncc_tree_find_child_by_nt_name(ncc_grammar_t    *g,
                                                 ncc_parse_tree_t *parent,
                                                 ncc_string_t      name);

ncc_parse_tree_t *ncc_tree_get_nth_nt_child(ncc_parse_tree_t *node, int32_t n);

ncc_parse_tree_t *ncc_tree_resolve_child_ref(ncc_grammar_t    *g,
                                             ncc_parse_tree_t *parent,
                                             ncc_child_ref_t   ref);

ncc_string_t ncc_tree_nt_name(ncc_grammar_t *g, ncc_parse_tree_t *n);
bool ncc_tree_is_nt(ncc_grammar_t *g, ncc_parse_tree_t *n, ncc_string_t name);

ncc_string_t ncc_tree_leaf_text(ncc_parse_tree_t *n);
int64_t      ncc_tree_leaf_tid(ncc_parse_tree_t *n);
bool         ncc_tree_leaf_is(ncc_parse_tree_t *n, ncc_string_t text);

