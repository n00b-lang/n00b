#pragma once

#include "abi_record_layout.h"
#include "parse/tree.h"

typedef struct {
    n00bb_record_policy_t policy;
    parse_tree_t *policy_node;
    // `align(N)`: the expression as written, and N in bytes once known (0
    // when the record has no `align`, or N isn't evaluated yet). A record
    // restored from a module interface has N and no expression.
    parse_tree_t *alignment_expression;
    parse_tree_t *alignment_node;
    uint32_t alignment_bytes;
    // The expression isn't a constant power of two; that was reported.
    bool alignment_invalid;
} n00bb_record_layout_request_t;

static inline bool
n00bb_record_request_aligned(const n00bb_record_layout_request_t *request)
{
    return request
        && (request->alignment_expression || request->alignment_bytes);
}
