#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "n00b-abi-v1.h"
#include "lib/byteorder.h"

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
    ABI_VALUE(TYPE, type, kind, N00B_ABI_TYPE_NIL, N00B_ABI_TYPE_OBJECT),
    ABI_VALUE(TRACE_CHILD, trace_child, extent_form,
        N00B_ABI_EXTENT_NONE, N00B_ABI_EXTENT_SENTINEL),
    ABI_VALUE(TRACE_CHILD, trace_child, reserved_zero, 0, 0),
    ABI_VALUE(TYPE_ARGUMENT, type_argument, kind,
        N00B_ABI_TYPE_ARG_TYPE, N00B_ABI_TYPE_ARG_STRING),
    ABI_BITS(TYPE, type, flags, N00B_ABI_TYPE_FLAG_SIGNED
        | N00B_ABI_TYPE_FLAG_OPAQUE | N00B_ABI_TYPE_FLAG_PACKABLE
        | N00B_ABI_TYPE_FLAG_NON_CONCRETE | N00B_ABI_TYPE_FLAG_BITS
        | N00B_ABI_TYPE_FLAG_PLAIN_REF | N00B_ABI_TYPE_FLAG_TYPE_INDEX),
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
        N00B_ABI_VISIBILITY_PRIVATE, N00B_ABI_VISIBILITY_PACKAGE),
    ABI_VALUE(ARM, arm, field.visibility,
        N00B_ABI_VISIBILITY_PRIVATE, N00B_ABI_VISIBILITY_PACKAGE),
    ABI_VALUE(TAG_EXTRACTOR, tag_extractor, kind,
        N00B_ABI_TAG_DIRECT_BITS, N00B_ABI_TAG_BITPTR_DEPTH),
    ABI_BITS(FUNCTION, function, flags, N00B_ABI_FUNCTION_FLAG_WRAPPABLE),
    ABI_VALUE(PARAMETER, parameter, passing_mode,
        N00B_ABI_PASS_IN, N00B_ABI_PASS_VIEW),
    ABI_BITS(PARAMETER, parameter, flags,
        N00B_ABI_PARAMETER_FLAG_VARIADIC),
    ABI_VALUE(SYMBOL, symbol, kind,
        N00B_ABI_SYMBOL_VARIABLE, N00B_ABI_SYMBOL_FIELD),
    ABI_VALUE(SYMBOL, symbol, visibility,
        N00B_ABI_VISIBILITY_PRIVATE, N00B_ABI_VISIBILITY_PACKAGE),
    ABI_BITS(SYMBOL, symbol, flags, N00B_ABI_SYMBOL_FLAG_EXPORTED),
    ABI_VALUE(SYMBOL, symbol, binding_kind,
        N00B_ABI_BINDING_NOT_APPLICABLE, N00B_ABI_BINDING_CONST),
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
    ABI_BITS(REQUIREMENT, requirement, flags,
        N00B_ABI_REQUIREMENT_FLAG_BAKED),
    ABI_VALUE(REQUIREMENT, requirement, reserved_zero, 0, 0),
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
    if (kind == N00B_ABI_TABLE_TYPE) {
        uint32_t flags = core_le_load_u32(bytes
            + offsetof(n00b_abi_type_v1, flags));
        uint32_t type_kind = core_le_load_u32(bytes
            + offsetof(n00b_abi_type_v1, kind));
        if (((flags & N00B_ABI_TYPE_FLAG_PLAIN_REF)
             && type_kind != N00B_ABI_TYPE_REFERENCE)
            || ((flags & N00B_ABI_TYPE_FLAG_TYPE_INDEX)
                && type_kind != N00B_ABI_TYPE_PRIMITIVE)) {
            if (invalid_offset) *invalid_offset = offsetof(
                n00b_abi_type_v1, flags);
            return false;
        }
    }
    if (kind == N00B_ABI_TABLE_SOURCE_LOCATION) {
        if (row_bytes < sizeof(n00b_abi_source_location_v1)) return false;
        const n00b_abi_source_location_v1 *location = row;
        size_t offset = 0;
        if (!location->start_line) offset = offsetof(
            n00b_abi_source_location_v1, start_line);
        else if (!location->start_column) offset = offsetof(
            n00b_abi_source_location_v1, start_column);
        else if (!location->end_line
                 || location->end_line < location->start_line)
            offset = offsetof(n00b_abi_source_location_v1, end_line);
        else if (!location->end_column
                 || (location->end_line == location->start_line
                     && location->end_column < location->start_column))
            offset = offsetof(n00b_abi_source_location_v1, end_column);
        else if (location->reserved_zero)
            offset = offsetof(n00b_abi_source_location_v1, reserved_zero);
        else if (location->end_byte_offset < location->start_byte_offset)
            offset = offsetof(n00b_abi_source_location_v1, end_byte_offset);
        if (offset) {
            if (invalid_offset) *invalid_offset = offset;
            return false;
        }
    }
    if (kind == N00B_ABI_TABLE_SYMBOL) {
        if (row_bytes < sizeof(n00b_abi_symbol_v1)) return false;
        const n00b_abi_symbol_v1 *symbol = row;
        if ((symbol->flags & N00B_ABI_SYMBOL_FLAG_EXPORTED)
            && symbol->visibility != N00B_ABI_VISIBILITY_PUBLIC) {
            if (invalid_offset) *invalid_offset = offsetof(
                n00b_abi_symbol_v1, visibility);
            return false;
        }
    }
    return true;
}
