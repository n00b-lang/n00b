#ifndef N00BB_ABI_MEMBER_FACTS_H
#define N00BB_ABI_MEMBER_FACTS_H

#include "n00b-abi-v1.h"

typedef struct {
    uint32_t parent;
    uint32_t ordinal;
    uint32_t symbol;
    uint32_t type;
    uint32_t symbol_kind;
    uint32_t visibility;
} n00bb_abi_member_facts_t;

static inline n00bb_abi_member_facts_t
n00bb_abi_member_facts(uint32_t kind, const void *row)
{
    if (kind == N00B_ABI_TABLE_ENUM_ITEM) {
        const n00b_abi_enum_item_v1 *item = row;
        return (n00bb_abi_member_facts_t){item->parent_type_index,
            item->declaration_ordinal, item->symbol_index,
            item->parent_type_index, N00B_ABI_SYMBOL_ENUM_ITEM, 0};
    }
    if (kind == N00B_ABI_TABLE_TRAIT_METHOD) {
        const n00b_abi_trait_method_v1 *method = row;
        return (n00bb_abi_member_facts_t){method->trait_type_index,
            method->declaration_ordinal, method->symbol_index,
            method->signature_type_index, N00B_ABI_SYMBOL_FUNCTION, 0};
    }
    const n00b_abi_field_v1 *field = kind == N00B_ABI_TABLE_ARM
        ? &((const n00b_abi_arm_v1 *)row)->field : row;
    return (n00bb_abi_member_facts_t){field->parent_type_index,
        field->declaration_ordinal, field->symbol_index,
        field->child_type_index, kind == N00B_ABI_TABLE_ARM
            ? N00B_ABI_SYMBOL_VARIANT_ARM : N00B_ABI_SYMBOL_FIELD,
        field->visibility};
}

#endif
