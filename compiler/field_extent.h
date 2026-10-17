#pragma once

#include "lib/string.h"
#include "parse/symtab.h"
#include "parse/tree.h"

#define N00BB_FIELD_EXTENT_KEY CORE_STRING_STATIC("n00bb.field-extent")

typedef enum {
    N00BB_FIELD_EXTENT_COUNT = 1,
    N00BB_FIELD_EXTENT_UNTIL = 2,
} n00bb_field_extent_kind_t;

typedef struct {
    n00bb_field_extent_kind_t kind;
    parse_sym_entry_t *count_field;
    uint64_t sentinel;
    parse_tree_t *node;
} n00bb_field_extent_t;

#define N00BB_FIELD_UNION_KEY CORE_STRING_STATIC("n00bb.field-union")

typedef struct {
    uint32_t group;
    parse_sym_entry_t *discriminant;
    uint64_t tag;
    parse_tree_t *node;
} n00bb_field_union_t;
