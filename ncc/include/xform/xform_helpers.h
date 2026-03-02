#pragma once

// Shared transform helper utilities.

#include "xform/transform.h"
#include "xform/type_normalize.h"

// Check if a non-terminal node has the given name.
bool n00b_xform_nt_name_is(n00b_parse_tree_t *node, const char *name);

// Find a direct child with the given NT name, transparently unwrapping
// group wrapper nodes ($$group_*). Returns NULL if not found.
n00b_parse_tree_t *n00b_xform_find_child_nt(n00b_parse_tree_t *node,
                                              const char        *name);

// Get token text from a leaf node. Returns NULL if not a leaf or no value.
const char *n00b_xform_leaf_text(n00b_parse_tree_t *node);

// Check if a leaf token's text matches a C string.
bool n00b_xform_leaf_text_eq(n00b_parse_tree_t *node, const char *text);

// Collect type atoms from typeid/typestr/typehash argument structure.
// Walks <typeid_atom> and <typeid_continuation> children.
// Returns a malloc'd combined canonical type string. Caller must free.
char *n00b_xform_extract_type_string(n00b_xform_ctx_t  *ctx,
                                      n00b_parse_tree_t *atom_node,
                                      n00b_parse_tree_t *cont_node);

// Get line/column from first leaf token in a subtree.
void n00b_xform_first_leaf_pos(n00b_parse_tree_t *node,
                                uint32_t *line, uint32_t *col);

// Walk all leaves of a subtree and concatenate their text with spaces.
// Returns a malloc'd string. Caller must free.
char *n00b_xform_node_to_text(n00b_parse_tree_t *node);
