#pragma once

#include <stdbool.h>
#include <string.h>

#include "n00b-abi-v1.h"

static inline bool
n00bb_abi_type_variable_fields_valid(const n00b_abi_type_v1 *type)
{
    if (!type) return false;
    if (type->kind != N00B_ABI_TYPE_VARIABLE)
        return !type->typevar_declaring_scope_index
            && !type->typevar_declaring_type_index && !type->typevar_ordinal
            && !type->typevar_import_requirement_index;
    // A variable an imported function declares has its declaring scope in
    // the declaring module's row only: an importing module's row names its
    // requirement instead, and says it leaves the scope out.
    n00b_abi_type_v1 expected = {
        .kind = N00B_ABI_TYPE_VARIABLE,
        .flags = N00B_ABI_TYPE_FLAG_NON_CONCRETE
            | (type->typevar_import_requirement_index
                ? N00B_ABI_TYPE_FLAG_DECLARATION_OMITTED : 0),
        .constraints = type->constraints,
        .typevar_declaring_scope_index = type->typevar_declaring_scope_index,
        .typevar_declaring_type_index = type->typevar_declaring_type_index,
        .typevar_ordinal = type->typevar_ordinal,
        .typevar_import_requirement_index =
            type->typevar_import_requirement_index,
    };
    return (type->typevar_declaring_scope_index != 0)
            + (type->typevar_import_requirement_index != 0)
            + (type->typevar_declaring_type_index != 0) == 1
        && !memcmp(type, &expected, sizeof(expected));
}

static inline bool
n00bb_abi_type_variable_final_valid(const n00b_abi_type_v1 *type)
{
    return n00bb_abi_type_variable_fields_valid(type)
        && !type->typevar_import_requirement_index;
}
