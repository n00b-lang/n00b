#pragma once

#include <stdbool.h>
#include <string.h>

#include "n00b-abi-v1.h"

static inline bool
n00bb_abi_type_variable_fields_valid(const n00b_abi_type_v1 *type)
{
    if (!type) return false;
    if (type->kind != N00B_ABI_TYPE_VARIABLE)
        return !type->typevar_owner_scope_index
            && !type->typevar_owner_type_index && !type->typevar_ordinal
            && !type->typevar_import_requirement_index;
    n00b_abi_type_v1 expected = {
        .kind = N00B_ABI_TYPE_VARIABLE,
        .flags = N00B_ABI_TYPE_FLAG_NON_CONCRETE,
        .constraints = type->constraints,
        .typevar_owner_scope_index = type->typevar_owner_scope_index,
        .typevar_owner_type_index = type->typevar_owner_type_index,
        .typevar_ordinal = type->typevar_ordinal,
        .typevar_import_requirement_index =
            type->typevar_import_requirement_index,
    };
    return (type->typevar_owner_scope_index != 0)
            + (type->typevar_import_requirement_index != 0)
            + (type->typevar_owner_type_index != 0) == 1
        && !memcmp(type, &expected, sizeof(expected));
}

static inline bool
n00bb_abi_type_variable_final_valid(const n00b_abi_type_v1 *type)
{
    return n00bb_abi_type_variable_fields_valid(type)
        && !type->typevar_import_requirement_index;
}
