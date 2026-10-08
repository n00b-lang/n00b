#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "n00b-abi-v1.h"

typedef struct {
    uint32_t row_kind;
    size_t offset;
    uint32_t target_kind;
    bool optional_type;
    bool required;
} abi_ref_column_t;

#define ABI_REF(K, T, F, TARGET) \
    {N00B_ABI_TABLE_##K, offsetof(n00b_abi_##T##_v1, F), \
     N00B_ABI_TABLE_##TARGET, false, false}
#define ABI_REQUIRED_REF(K, T, F, TARGET) \
    {N00B_ABI_TABLE_##K, offsetof(n00b_abi_##T##_v1, F), \
     N00B_ABI_TABLE_##TARGET, false, true}
#define ABI_OPTIONAL_TYPE(K, T, F) \
    {N00B_ABI_TABLE_##K, offsetof(n00b_abi_##T##_v1, F), \
     N00B_ABI_TABLE_TYPE, true, false}

static const abi_ref_column_t abi_index_columns[] = {
    ABI_REF(PACKAGE, package, parent_package_index, PACKAGE),
    ABI_REQUIRED_REF(MODULE, module, package_index, PACKAGE),
    ABI_REF(MODULE, module, scc_index, SCC),
    ABI_REF(MODULE, module, comptime_initializer, FUNCTION),
    ABI_REF(MODULE, module, runtime_initializer, FUNCTION),
    ABI_REQUIRED_REF(MODULE, module, runtime_config_index, RUNTIME_CONFIG),
    ABI_REQUIRED_REF(MODULE, module, source_file_index, SOURCE_FILE),
    ABI_REF(TYPE, type, layout_index, LAYOUT),
    ABI_REF(TYPE, type, owner_module_index, MODULE),
    ABI_REF(TYPE, type, tracing_index, TRACE),
    ABI_REF(TYPE, type, call_signature_index, CALL_SIGNATURE),
    ABI_REF(TYPE, type, typevar_owner_scope_index, SCOPE),
    ABI_REQUIRED_REF(ENUM_ITEM, enum_item, parent_type_index, TYPE),
    ABI_REF(ENUM_ITEM, enum_item, symbol_index, SYMBOL),
    ABI_OPTIONAL_TYPE(CONSTRAINT, constraint, target_type_index),
    ABI_OPTIONAL_TYPE(CONSTRAINT, constraint, result_type_index),
    ABI_REF(CONSTRAINT, constraint, call_shape_index, CALL_SHAPE),
    ABI_REF(LAYOUT, layout, type_index, TYPE),
    ABI_REF(LAYOUT, layout, tag_extractor_index, TAG_EXTRACTOR),
    ABI_REQUIRED_REF(FIELD, field, parent_type_index, TYPE),
    ABI_REF(FIELD, field, child_type_index, TYPE),
    ABI_REF(FIELD, field, symbol_index, SYMBOL),
    ABI_REQUIRED_REF(ARM, arm, field.parent_type_index, TYPE),
    ABI_REF(ARM, arm, field.child_type_index, TYPE),
    ABI_REF(ARM, arm, field.symbol_index, SYMBOL),
    ABI_REQUIRED_REF(TRACE, trace, type_index, TYPE),
    ABI_REF(TRACE, trace, child_trace_index, TRACE),
    ABI_REF(TRACE, trace, tag_extractor_index, TAG_EXTRACTOR),
    ABI_REQUIRED_REF(TRACE_CHILD, trace_child, child_type_index, TYPE),
    ABI_REF(TRACE_ARM, trace_arm, child_trace_index, TRACE),
    ABI_REQUIRED_REF(FUNCTION, function, module_index, MODULE),
    ABI_REQUIRED_REF(FUNCTION, function, scope_index, SCOPE),
    ABI_REF(FUNCTION, function, symbol_index, SYMBOL),
    ABI_REF(FUNCTION, function, success_type_index, TYPE),
    ABI_REF(FUNCTION, function, failure_type_index, TYPE),
    ABI_OPTIONAL_TYPE(FUNCTION, function, variadic_type_index),
    ABI_REQUIRED_REF(CALL_SIGNATURE, call_signature, type_index, TYPE),
    ABI_REF(CALL_SIGNATURE, call_signature, success_type_index, TYPE),
    ABI_REF(CALL_SIGNATURE, call_signature, failure_type_index, TYPE),
    ABI_REF(CALL_SIGNATURE, call_signature, rest_parameter_index, PARAMETER),
    ABI_REF(CALL_SHAPE, call_shape, success_type_index, TYPE),
    ABI_OPTIONAL_TYPE(CALL_SHAPE, call_shape, failure_type_index),
    ABI_REF(CALL_SHAPE, call_shape, rest_parameter_index, PARAMETER),
    ABI_REF(PARAMETER, parameter, type_index, TYPE),
    ABI_REQUIRED_REF(CODE_RANGE, code_range, function_index, FUNCTION),
    ABI_REQUIRED_REF(SCOPE, scope, module_index, MODULE),
    ABI_REF(SCOPE, scope, function_index, FUNCTION),
    ABI_REF(SCOPE, scope, parent_scope_index, SCOPE),
    ABI_REF(SCOPE, scope, source_location_index, SOURCE_LOCATION),
    ABI_REF(SYMBOL, symbol, type_index, TYPE),
    ABI_REQUIRED_REF(SYMBOL, symbol, module_index, MODULE),
    ABI_REQUIRED_REF(SYMBOL, symbol, scope_index, SCOPE),
    ABI_REF(SYMBOL, symbol, declaration_location_index, SOURCE_LOCATION),
    ABI_REF(SYMBOL, symbol, native_binding_index, NATIVE_BINDING),
    ABI_REQUIRED_REF(TRAIT_METHOD, trait_method, trait_type_index, TYPE),
    ABI_REQUIRED_REF(TRAIT_METHOD, trait_method, signature_type_index, TYPE),
    ABI_REF(TRAIT_METHOD, trait_method, symbol_index, SYMBOL),
    ABI_REQUIRED_REF(DISPATCH, dispatch, trait_type_index, TYPE),
    ABI_REF(DISPATCH_ENTRY, dispatch_entry, receiver_type_index, TYPE),
    ABI_REQUIRED_REF(DISPATCH_ENTRY, dispatch_entry, function_index, FUNCTION),
    ABI_REQUIRED_REF(FRAME_LAYOUT, frame_layout, function_index, FUNCTION),
    ABI_REF(FRAME_LAYOUT, frame_layout, shadow_frame_type_index, TYPE),
    ABI_REQUIRED_REF(SOURCE_LOCATION, source_location, file_index, SOURCE_FILE),
    ABI_REQUIRED_REF(STORAGE, storage, module_index, MODULE),
    ABI_REF(STORAGE, storage, symbol_index, SYMBOL),
    ABI_REF(STORAGE, storage, type_index, TYPE),
    ABI_REF(STORAGE, storage, instance_shape_index, SHAPE),
    ABI_REQUIRED_REF(TLS_RELOCATION, tls_relocation, template_storage_index, STORAGE),
    ABI_REQUIRED_REF(TLS_RELOCATION, tls_relocation, referenced_storage_index, STORAGE),
    ABI_REF(NAME_SLOT, name_slot, symbol_index, SYMBOL),
    ABI_OPTIONAL_TYPE(REQUIREMENT, requirement, contract_type_index),
    ABI_REF(REQUIREMENT, requirement, required_layout_index, LAYOUT),
    ABI_REQUIRED_REF(REQUIREMENT, requirement, requesting_module_index, MODULE),
};

#undef ABI_REF
#undef ABI_REQUIRED_REF
#undef ABI_OPTIONAL_TYPE

typedef struct {
    uint32_t row_kind;
    size_t offset;
    uint32_t stream_kind;
    uint32_t target_kind;
} abi_range_column_t;

#define ABI_RANGE(K, T, F, STREAM, TARGET) \
    {N00B_ABI_TABLE_##K, offsetof(n00b_abi_##T##_v1, F), \
     N00B_ABI_TABLE_##STREAM, N00B_ABI_TABLE_##TARGET}
#define ABI_RAW_RANGE(K, T, F, STREAM) \
    {N00B_ABI_TABLE_##K, offsetof(n00b_abi_##T##_v1, F), \
     N00B_ABI_TABLE_##STREAM, 0}

static const abi_range_column_t abi_range_columns[] = {
    ABI_RANGE(PACKAGE, package, modules, U32_STREAM, MODULE),
    ABI_RANGE(MODULE, module, imports, U32_STREAM, MODULE),
    ABI_RANGE(MODULE, module, static_storage, U32_STREAM, STORAGE),
    ABI_RANGE(MODULE, module, tls_storage, U32_STREAM, STORAGE),
    ABI_RANGE(SCC, scc, members, U32_STREAM, MODULE),
    ABI_RAW_RANGE(SCC, scc, lexical_order, U32_STREAM),
    ABI_RANGE(TYPE, type, components, U32_STREAM, TYPE),
    ABI_RAW_RANGE(TYPE, type, fields_or_arms, U32_STREAM),
    ABI_RANGE(TYPE, type, constraints, U32_STREAM, CONSTRAINT),
    ABI_RANGE(CONSTRAINT, constraint, arguments, U32_STREAM, TYPE),
    ABI_RAW_RANGE(SHAPE, shape, extents, U64_STREAM),
    ABI_RANGE(LAYOUT, layout, occupied_runs, U32_STREAM, PHYSICAL_RUN),
    ABI_RANGE(LAYOUT, layout, reserved_runs, U32_STREAM, PHYSICAL_RUN),
    ABI_RANGE(FIELD, field, packed_runs, U32_STREAM, PACKED_RUN),
    ABI_RANGE(FIELD, field, physical_runs, U32_STREAM, PHYSICAL_RUN),
    ABI_RANGE(ARM, arm, field.packed_runs, U32_STREAM, PACKED_RUN),
    ABI_RANGE(ARM, arm, field.physical_runs, U32_STREAM, PHYSICAL_RUN),
    ABI_RANGE(TRACE, trace, fixed_children, U32_STREAM, TRACE_CHILD),
    ABI_RANGE(TRACE, trace, arms, U32_STREAM, TRACE_ARM),
    ABI_RANGE(TRACE_ARM, trace_arm, children, U32_STREAM, TRACE_CHILD),
    ABI_RANGE(FUNCTION, function, parameters, U32_STREAM, PARAMETER),
    ABI_RANGE(FUNCTION, function, keywords, U32_STREAM, PARAMETER),
    ABI_RANGE(FUNCTION, function, constraints, U32_STREAM, CONSTRAINT),
    ABI_RANGE(FUNCTION, function, code_ranges, U32_STREAM, CODE_RANGE),
    ABI_RANGE(FUNCTION, function, frame_layouts, U32_STREAM, FRAME_LAYOUT),
    ABI_RANGE(CALL_SIGNATURE, call_signature, positional_parameters,
        U32_STREAM, PARAMETER),
    ABI_RANGE(CALL_SIGNATURE, call_signature, keyword_parameters,
        U32_STREAM, PARAMETER),
    ABI_RANGE(CALL_SHAPE, call_shape, positional_parameters,
        U32_STREAM, PARAMETER),
    ABI_RANGE(CALL_SHAPE, call_shape, keyword_parameters,
        U32_STREAM, PARAMETER),
    ABI_RANGE(SCOPE, scope, children, U32_STREAM, SCOPE),
    ABI_RANGE(SCOPE, scope, symbols, U32_STREAM, SYMBOL),
    ABI_RANGE(SYMBOL, symbol, definitions, U32_STREAM, SOURCE_LOCATION),
    ABI_RANGE(SYMBOL, symbol, uses, U32_STREAM, SOURCE_LOCATION),
    ABI_RANGE(DISPATCH, dispatch, entries, U32_STREAM, DISPATCH_ENTRY),
    ABI_RAW_RANGE(FRAME_LAYOUT, frame_layout, pointer_bitmap_bytes,
        BYTE_STREAM),
    ABI_RANGE(STORAGE, storage, tls_relocations, U32_STREAM, TLS_RELOCATION),
};

#undef ABI_RANGE
#undef ABI_RAW_RANGE
