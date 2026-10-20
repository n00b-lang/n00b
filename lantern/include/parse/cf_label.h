#pragma once

#include "parse/types.h"
#include "parse/tree.h"
#include "parse/symtab.h"

typedef enum {
    PARSE_CF_BRANCH,
    PARSE_CF_LOOP,
    PARSE_CF_SWITCH,
    PARSE_CF_JUMP,
    PARSE_CF_CAPTURE,
    PARSE_CF_ASSIGNS,
    PARSE_CF_VARREF,
    PARSE_CF_UNWRAP_RESULT,
    PARSE_CF_CALL,
} parse_cf_kind_t;

typedef struct {
    parse_cf_kind_t     kind;
    parse_tree_t *self;
    parse_tree_t *cond;
    parse_tree_t *then_body;
    parse_tree_t *else_body;
    core_string_t      jump_kind;
    core_string_t      tag;
    bool              capture_by_tag;
} parse_cf_label_t;
