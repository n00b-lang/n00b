#pragma once

#include <stddef.h>

#include "lib/list.h"
#include "lib/string.h"
#include "th/typehash.h"

#define N00BB_SIGNATURE_TYPEVARS_KEY \
    CORE_STRING_STATIC("n00bb.signature.typevars")
#define N00BB_BODY_TYPEVAR_MAP_KEY \
    CORE_STRING_STATIC("n00bb.body-typevar-map")
#define N00BB_DECLARED_WHERE_KEY \
    CORE_STRING_STATIC("n00bb.declared-where")

typedef struct {
    core_string_t name;
    th_type *type;
} n00bb_typevar_binding_t;
core_list_decl(n00bb_typevar_binding_t);

typedef struct {
    n00bb_typevar_binding_t *data;
    size_t len;
    size_t cap;
} n00bb_signature_typevars_t;

typedef struct {
    th_type **declaration;
    th_type **body;
    size_t count;
} n00bb_body_typevar_map_t;

typedef struct {
    th_trait_requirement *data;
    size_t len;
} n00bb_declared_where_t;
