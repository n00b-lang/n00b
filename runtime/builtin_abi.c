#define _GNU_SOURCE
#include <dlfcn.h>
#include "builtin_abi.h"

const n00b_abi_type_v1 *
n00b_builtin_abi_type(uint32_t index)
{
    return n00b_abi_v1_type(n00b_abi_v1_current_snapshot(), index);
}

const n00b_abi_type_v1 *
n00b_builtin_abi_type_for(uint32_t descriptor)
{
    return n00b_builtin_abi_type(descriptor);
}

const n00b_abi_field_v1 *
n00b_builtin_abi_field(uint32_t index)
{
    return n00b_abi_v1_field(n00b_abi_v1_current_snapshot(), index);
}

const n00b_abi_arm_v1 *
n00b_builtin_abi_arm(uint32_t index)
{
    uint32_t bytes = 0;
    const n00b_abi_arm_v1 *row = n00b_abi_v1_record(
        n00b_abi_v1_current_snapshot(), N00B_ABI_TABLE_ARM, index, &bytes);
    return bytes == sizeof(*row) ? row : NULL;
}

const n00b_abi_physical_run_v1 *
n00b_builtin_abi_physical_run(uint32_t index)
{
    uint32_t bytes = 0;
    const n00b_abi_physical_run_v1 *row = n00b_abi_v1_record(
        n00b_abi_v1_current_snapshot(), N00B_ABI_TABLE_PHYSICAL_RUN, index,
        &bytes);
    return bytes == sizeof(*row) ? row : NULL;
}

const n00b_abi_trace_v1 *
n00b_builtin_abi_trace(uint32_t index)
{
    return n00b_abi_v1_trace(n00b_abi_v1_current_snapshot(), index);
}

const n00b_abi_trace_child_v1 *
n00b_builtin_abi_trace_child(uint32_t index)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    uint32_t bytes = 0;
    const n00b_abi_trace_child_v1 *row = n00b_abi_v1_record(snapshot,
        N00B_ABI_TABLE_TRACE_CHILD, index, &bytes);
    return bytes == sizeof(*row) ? row : NULL;
}

const n00b_abi_trace_arm_v1 *
n00b_builtin_abi_trace_arm(uint32_t index)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    uint32_t bytes = 0;
    const n00b_abi_trace_arm_v1 *row = n00b_abi_v1_record(snapshot,
        N00B_ABI_TABLE_TRACE_ARM, index, &bytes);
    return bytes == sizeof(*row) ? row : NULL;
}

const n00b_abi_tag_extractor_v1 *
n00b_builtin_abi_tag_extractor(uint32_t index)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    uint32_t bytes = 0;
    const n00b_abi_tag_extractor_v1 *row = n00b_abi_v1_record(snapshot,
        N00B_ABI_TABLE_TAG_EXTRACTOR, index, &bytes);
    return bytes == sizeof(*row) ? row : NULL;
}

const n00b_abi_layout_v1 *
n00b_builtin_abi_layout(uint32_t index)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    uint32_t bytes = 0;
    const n00b_abi_layout_v1 *row = n00b_abi_v1_record(snapshot,
        N00B_ABI_TABLE_LAYOUT, index, &bytes);
    return bytes == sizeof(*row) ? row : NULL;
}

const n00b_abi_frame_layout_v1 *
n00b_builtin_abi_frame_layout(uint32_t index)
{
    return n00b_abi_v1_frame_layout(n00b_abi_v1_current_snapshot(), index);
}

uint32_t
n00b_builtin_abi_storage_count(void)
{
    return n00b_abi_v1_table_count(n00b_abi_v1_current_snapshot(),
                                    N00B_ABI_TABLE_STORAGE);
}

const n00b_abi_storage_v1 *
n00b_builtin_abi_storage(uint32_t index)
{
    return n00b_abi_v1_storage(n00b_abi_v1_current_snapshot(), index);
}

uint32_t
n00b_builtin_abi_baked_node_count(void)
{
    return n00b_abi_v1_table_count(n00b_abi_v1_current_snapshot(),
                                    N00B_ABI_TABLE_BAKED_NODE);
}

const n00b_abi_baked_node_v1 *
n00b_builtin_abi_baked_node(uint32_t index)
{
    return n00b_abi_v1_baked_node(n00b_abi_v1_current_snapshot(), index);
}

const n00b_abi_symbol_v1 *
n00b_builtin_abi_symbol(uint32_t index)
{
    return n00b_abi_v1_symbol(n00b_abi_v1_current_snapshot(), index);
}

const n00b_abi_function_v1 *
n00b_builtin_abi_function(uint32_t index)
{
    return n00b_abi_v1_function(n00b_abi_v1_current_snapshot(), index);
}

const n00b_abi_module_v1 *
n00b_builtin_abi_module(uint32_t index)
{
    return n00b_abi_v1_module(n00b_abi_v1_current_snapshot(), index);
}

const uint8_t *
n00b_builtin_abi_string_backing_at(const void *address)
{
    if (!address) return NULL;
    const n00b_abi_string_v1 *value = (const n00b_abi_string_v1 *)address;
    return value->words[1] >> N00B_ABI_STRING_TAG_SHIFT
        ? value->long_value.data : NULL;
}

const uint8_t *
n00b_builtin_abi_bytes(uint32_t first, uint32_t count)
{
    const uint8_t *data = NULL;
    return n00b_abi_v1_bytes(n00b_abi_v1_current_snapshot(),
        (n00b_abi_range_v1){.first = first, .count = count},
        &data) == 1 ? data : NULL;
}

int64_t
n00b_builtin_abi_range_index(uint32_t first, uint32_t count,
                             uint32_t ordinal)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    uint32_t result = 0;
    return n00b_abi_v1_index_at(snapshot,
        (n00b_abi_range_v1){.first = first, .count = count},
        ordinal, &result) == 1 ? (int64_t)result : -1;
}

int32_t
n00b_builtin_abi_type_specializes(uint32_t type, uint32_t general)
{
    return n00b_abi_v1_type_specializes(n00b_abi_v1_current_snapshot(), type,
                                        general);
}

uint32_t
n00b_builtin_abi_type_argument_type(uint32_t index)
{
    const n00b_abi_type_argument_v1 *argument = n00b_abi_v1_type_argument(
        n00b_abi_v1_current_snapshot(), index);
    return argument && argument->kind == N00B_ABI_TYPE_ARG_TYPE
        ? argument->type_index : UINT32_MAX;
}

uint32_t
n00b_builtin_abi_named_type(const uint8_t *name, uint64_t name_bytes,
                            const uint32_t *arguments, uint32_t count)
{
    uint32_t match = 0;
    return n00b_abi_v1_find_named_type(n00b_abi_v1_current_snapshot(), name,
        (size_t)name_bytes, arguments, count, &match) == 1 ? match : 0;
}

uint32_t
n00b_builtin_abi_unary_type(uint32_t kind, uint32_t element)
{
    uint32_t match = 0;
    return n00b_abi_v1_find_unary_type(n00b_abi_v1_current_snapshot(),
        (n00b_abi_type_kind_v1)kind, element, &match) == 1 ? match : 0;
}

uint64_t
n00b_builtin_abi_ref_address(const void *value)
{
    return (uint64_t)(uintptr_t)value;
}

const uint8_t *
n00b_builtin_native_symbol_name(uint64_t address)
{
    Dl_info info = {0};
    if (!dladdr((const void *)(uintptr_t)address, &info) || !info.dli_sname
        || (uint64_t)(uintptr_t)info.dli_saddr != address) return NULL;
    return (const uint8_t *)info.dli_sname;
}
