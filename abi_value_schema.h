#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "n00b-abi-v1.h"

typedef struct {
    uint32_t row_kind;
    size_t offset;
    uint32_t minimum;
    uint32_t maximum;
    uint32_t allowed_bits;
} n00bb_abi_value_column_t;

#define ABI_VALUE(K, T, F, MIN, MAX) \
    {N00B_ABI_TABLE_##K, offsetof(n00b_abi_##T##_v1, F), MIN, MAX, 0}
#define ABI_BITS(K, T, F, BITS) \
    {N00B_ABI_TABLE_##K, offsetof(n00b_abi_##T##_v1, F), 0, 0, BITS}

static const n00bb_abi_value_column_t n00bb_abi_value_columns[] = {
    ABI_VALUE(PACKAGE, package, visibility,
        N00B_ABI_VISIBILITY_PRIVATE, N00B_ABI_VISIBILITY_PUBLIC),
    ABI_VALUE(TYPE, type, kind, N00B_ABI_TYPE_NIL, N00B_ABI_TYPE_TUPLE),
    ABI_BITS(TYPE, type, flags, N00B_ABI_TYPE_FLAG_SIGNED
        | N00B_ABI_TYPE_FLAG_OPAQUE | N00B_ABI_TYPE_FLAG_PACKABLE
        | N00B_ABI_TYPE_FLAG_NON_CONCRETE | N00B_ABI_TYPE_FLAG_BITS),
    ABI_BITS(ENUM_ITEM, enum_item, flags, N00B_ABI_ENUM_ITEM_FLAG_PRIMITIVE),
    ABI_VALUE(CONSTRAINT, constraint, kind,
        N00B_ABI_CONSTRAINT_UNIFIES, N00B_ABI_CONSTRAINT_TYPE_FAMILY),
    ABI_BITS(CONSTRAINT, constraint, flags,
        N00B_ABI_CONSTRAINT_FLAG_HAS_SCOPE),
    ABI_VALUE(CONSTRAINT, constraint, type_family,
        N00B_ABI_FAMILY_NONE, N00B_ABI_FAMILY_TRAIT),
    ABI_VALUE(LAYOUT, layout, policy,
        N00B_ABI_LAYOUT_DEFAULT, N00B_ABI_LAYOUT_C),
    ABI_VALUE(FIELD, field, visibility,
        N00B_ABI_VISIBILITY_PRIVATE, N00B_ABI_VISIBILITY_PUBLIC),
    ABI_VALUE(ARM, arm, field.visibility,
        N00B_ABI_VISIBILITY_PRIVATE, N00B_ABI_VISIBILITY_PUBLIC),
    ABI_VALUE(TAG_EXTRACTOR, tag_extractor, kind,
        N00B_ABI_TAG_DIRECT_BITS, N00B_ABI_TAG_BITPTR_DEPTH),
    ABI_BITS(FUNCTION, function, flags, N00B_ABI_FUNCTION_FLAG_WRAPPABLE),
    ABI_VALUE(PARAMETER, parameter, passing_mode,
        N00B_ABI_PASS_IN, N00B_ABI_PASS_CONSUME),
    ABI_BITS(PARAMETER, parameter, flags,
        N00B_ABI_PARAMETER_FLAG_VARIADIC),
    ABI_BITS(CODE_RANGE, code_range, phase_mask,
        N00B_ABI_PHASE_COMPTIME | N00B_ABI_PHASE_RUNTIME),
    ABI_VALUE(SYMBOL, symbol, kind,
        N00B_ABI_SYMBOL_VARIABLE, N00B_ABI_SYMBOL_FIELD),
    ABI_VALUE(SYMBOL, symbol, visibility,
        N00B_ABI_VISIBILITY_PRIVATE, N00B_ABI_VISIBILITY_PUBLIC),
    ABI_BITS(SYMBOL, symbol, flags, N00B_ABI_SYMBOL_FLAG_EXPORTED),
    ABI_VALUE(SYMBOL, symbol, binding_kind,
        N00B_ABI_BINDING_NOT_APPLICABLE, N00B_ABI_BINDING_CONST),
    ABI_BITS(SYMBOL, symbol, phase_mask,
        N00B_ABI_PHASE_COMPTIME | N00B_ABI_PHASE_RUNTIME),
    ABI_BITS(SYMBOL, symbol, mutable_phase_mask,
        N00B_ABI_PHASE_COMPTIME | N00B_ABI_PHASE_RUNTIME),
    ABI_BITS(SYMBOL, symbol, initializer_phase_mask,
        N00B_ABI_PHASE_COMPTIME | N00B_ABI_PHASE_RUNTIME),
    ABI_VALUE(SYMBOL, symbol, storage_duration,
        N00B_ABI_DURATION_UNSPECIFIED, N00B_ABI_DURATION_DYNAMIC),
    ABI_VALUE(NATIVE_BINDING, native_binding, kind,
        N00B_ABI_NATIVE_SECTION_OFFSET, N00B_ABI_NATIVE_EXTERNAL_NAME),
    ABI_VALUE(STORAGE, storage, segment_kind,
        N00B_ABI_STORAGE_READ_ONLY, N00B_ABI_STORAGE_TLS),
    ABI_VALUE(TLS_RELOCATION, tls_relocation, target_kind,
        N00B_ABI_STORAGE_READ_ONLY, N00B_ABI_STORAGE_TLS),
    ABI_VALUE(REQUIREMENT, requirement, symbol_kind,
        N00B_ABI_SYMBOL_VARIABLE, N00B_ABI_SYMBOL_FIELD),
    ABI_BITS(REQUIREMENT, requirement, required_phase_mask,
        N00B_ABI_PHASE_COMPTIME | N00B_ABI_PHASE_RUNTIME),
    ABI_VALUE(INDEX_PATCH, index_patch, patch_kind,
        N00B_ABI_PATCH_INDEX, N00B_ABI_PATCH_BOX_TYPE),
};

#undef ABI_VALUE
#undef ABI_BITS

static inline bool
n00bb_abi_row_values_valid(uint32_t kind, const void *row,
                           size_t row_bytes, size_t *invalid_offset)
{
    const uint8_t *bytes = row;
    if (!bytes) return false;
    for (size_t i = 0; i < sizeof(n00bb_abi_value_columns)
         / sizeof(n00bb_abi_value_columns[0]); i++) {
        const n00bb_abi_value_column_t *column = &n00bb_abi_value_columns[i];
        if (column->row_kind != kind) continue;
        size_t offset = column->offset;
        if (offset > row_bytes || sizeof(uint32_t) > row_bytes - offset) {
            if (invalid_offset) *invalid_offset = offset;
            return false;
        }
        uint32_t value = (uint32_t)bytes[offset]
            | (uint32_t)bytes[offset + 1] << 8
            | (uint32_t)bytes[offset + 2] << 16
            | (uint32_t)bytes[offset + 3] << 24;
        if ((column->allowed_bits && (value & ~column->allowed_bits))
            || (!column->allowed_bits && (value < column->minimum
                || value > column->maximum))) {
            if (invalid_offset) *invalid_offset = offset;
            return false;
        }
    }
    return true;
}
