#pragma once

#include "abi_record_layout.h"
#include "parse/tree.h"

typedef struct {
    n00bb_record_policy_t policy;
    parse_tree_t *policy_node;
    parse_tree_t *alignment_expression;
    parse_tree_t *alignment_node;
} n00bb_record_layout_request_t;
