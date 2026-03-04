#pragma once

// Shared transform helper utilities.

#include "xform/transform.h"
#include "xform/type_normalize.h"

// Check if a non-terminal node has the given name.
bool ncc_xform_nt_name_is(ncc_parse_tree_t *node, const char *name);

// Find a direct child with the given NT name, transparently unwrapping
// group wrapper nodes ($$group_*). Returns NULL if not found.
ncc_parse_tree_t *ncc_xform_find_child_nt(ncc_parse_tree_t *node,
                                              const char        *name);

// Get token text from a leaf node. Returns NULL if not a leaf or no value.
const char *ncc_xform_leaf_text(ncc_parse_tree_t *node);

// Check if a leaf token's text matches a C string.
bool ncc_xform_leaf_text_eq(ncc_parse_tree_t *node, const char *text);

// Collect type atoms from typeid/typestr/typehash argument structure.
// Walks <typeid_atom> and <typeid_continuation> children.
// Returns a malloc'd combined canonical type string. Caller must free.
char *ncc_xform_extract_type_string(ncc_xform_ctx_t  *ctx,
                                      ncc_parse_tree_t *atom_node,
                                      ncc_parse_tree_t *cont_node);

// Get line/column from first leaf token in a subtree.
void ncc_xform_first_leaf_pos(ncc_parse_tree_t *node,
                                uint32_t *line, uint32_t *col);

// Walk all leaves of a subtree and concatenate their text with spaces.
// Returns a malloc'd string. Caller must free.
char *ncc_xform_node_to_text(ncc_parse_tree_t *node);
