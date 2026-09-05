#pragma once

#include "parse/types.h"
#include "parse/parse_tree.h"
#include "parse/grammar.h"
#include "parse/annotation.h"

// Text of the leftmost token in a subtree, or an empty string. Callers must
// test .u8_bytes, not .data.
ncc_string_t ncc_tree_extract_first_identifier(ncc_parse_tree_t *node);

// Leftmost token in a subtree, or nullptr.
ncc_token_info_t *ncc_tree_find_first_terminal(ncc_parse_tree_t *node);

// Child non-terminal with the given name, seeing through group wrappers.
ncc_parse_tree_t *ncc_tree_find_child_by_nt_name(ncc_grammar_t    *g,
                                                 ncc_parse_tree_t *parent,
                                                 ncc_string_t      name);

// Nth non-terminal child, skipping terminals; group wrappers are transparent.
ncc_parse_tree_t *ncc_tree_get_nth_nt_child(ncc_parse_tree_t *node, int32_t n);

ncc_parse_tree_t *ncc_tree_resolve_child_ref(ncc_grammar_t    *g,
                                             ncc_parse_tree_t *parent,
                                             ncc_child_ref_t   ref);
