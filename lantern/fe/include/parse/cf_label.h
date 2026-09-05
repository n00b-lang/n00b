#pragma once

#include "parse/types.h"
#include "parse/parse_tree.h"
#include "parse/symtab.h"

typedef enum {
    NCC_CF_BRANCH,
    NCC_CF_LOOP,
    NCC_CF_SWITCH,
    NCC_CF_JUMP,
    NCC_CF_CAPTURE,
    NCC_CF_ASSIGNS,
    NCC_CF_VARREF,
    NCC_CF_UNWRAP_RESULT,
    NCC_CF_CALL,
} ncc_cf_kind_t;

typedef struct {
    ncc_cf_kind_t     kind;
    ncc_parse_tree_t *self;
    ncc_parse_tree_t *cond;
    ncc_parse_tree_t *then_body;
    ncc_parse_tree_t *else_body;
    ncc_string_t      jump_kind;
    ncc_string_t      tag;
    bool              capture_by_tag;
} ncc_cf_label_t;
