#pragma once

#include <stdbool.h>

#include "n00b-abi-v1.h"

static inline bool
n00bb_abi_traced_leaf(const n00b_abi_type_v1 *type)
{
    switch (type->kind) {
    case N00B_ABI_TYPE_REFERENCE:
    case N00B_ABI_TYPE_OBJECT:
    case N00B_ABI_TYPE_FOREIGN:
    case N00B_ABI_TYPE_ARRAY:
    case N00B_ABI_TYPE_VIEW:
    case N00B_ABI_TYPE_STRING:
    case N00B_ABI_TYPE_BOX:
    case N00B_ABI_TYPE_CALLBACK:
    case N00B_ABI_TYPE_TRAIT:
    case N00B_ABI_TYPE_UNION:
        return true;
    case N00B_ABI_TYPE_PRIMITIVE:
        return (type->flags & N00B_ABI_TYPE_FLAG_TYPE_INDEX) != 0;
    default:
        return false;
    }
}
