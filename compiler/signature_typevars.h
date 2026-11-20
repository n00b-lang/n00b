#pragma once

#include <stddef.h>

#include "lib/list.h"
#include "lib/string.h"
#include "parse/symtab.h"
#include "th/typehash.h"

#define N00BB_SIGNATURE_TYPEVARS_KEY \
    CORE_STRING_STATIC("n00bb.signature.typevars")
#define N00BB_DECLARED_WHERE_KEY \
    CORE_STRING_STATIC("n00bb.declared-where")
#define N00BB_TYPE_BOUNDS_KEY CORE_STRING_STATIC("n00bb.type-bounds")

core_list_decl(th_trait_requirement);

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
    th_trait_requirement *data;
    size_t len;
} n00bb_declared_where_t;

static inline core_list_t(th_trait_requirement)
n00bb_declared_bounds(parse_sym_entry_t *symbol)
{
    core_list_t(th_trait_requirement) out =
        core_list_new(th_trait_requirement);
    n00bb_declared_where_t *declared = symbol
        ? parse_sym_binding(symbol, N00BB_DECLARED_WHERE_KEY) : nullptr;
    core_list_t(th_trait_requirement) *types = symbol
        ? parse_sym_binding(symbol, N00BB_TYPE_BOUNDS_KEY) : nullptr;
    for (size_t i = 0; declared && i < declared->len; i++)
        core_list_push(out, declared->data[i]);
    for (size_t i = 0; types && i < types->len; i++)
        core_list_push(out, types->data[i]);
    return out;
}
