#include "n00b-abi-v1.h"

#include <limits.h>
#include <stdint.h>

extern void n00b_gc_barrier_value(const void *address, uint32_t type_index);

uint32_t n00bb_bootstrap_type_component(uint32_t type_index, uint32_t ordinal)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    const n00b_abi_type_v1 *type = snapshot
        ? n00b_abi_v1_type(snapshot, type_index & UINT32_C(0x7fffffff))
        : NULL;
    uint32_t result = 0;
    if (!type) __builtin_trap();
    if (type->kind == N00B_ABI_TYPE_ARRAY
        || type->kind == N00B_ABI_TYPE_REFERENCE) {
        if (n00b_abi_v1_index_at(snapshot, type->components, ordinal,
                                &result) != 1) __builtin_trap();
        return result;
    }
    if (ordinal >= type->named_arguments.count) __builtin_trap();
    const n00b_abi_type_argument_v1 *argument = n00b_abi_v1_type_argument(
        snapshot, type->named_arguments.first + ordinal);
    if (!argument || argument->kind != N00B_ABI_TYPE_ARG_TYPE)
        __builtin_trap();
    return argument->type_index;
}

static uint32_t n00bb_bootstrap_unary_type(n00b_abi_type_kind_v1 kind,
                                            uint32_t element_index)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    uint32_t match = 0;
    if (n00b_abi_v1_find_unary_type(snapshot, kind, element_index,
                                    &match) != 1) __builtin_trap();
    return match;
}

uint32_t n00bb_bootstrap_array_type(uint32_t element_index)
{
    return n00bb_bootstrap_unary_type(N00B_ABI_TYPE_ARRAY, element_index);
}

uint32_t n00bb_bootstrap_reference_type(uint32_t element_index)
{
    return n00bb_bootstrap_unary_type(N00B_ABI_TYPE_REFERENCE,
                                       element_index);
}

uint32_t n00bb_bootstrap_named_type(const char *name,
                                    const uint32_t *arguments,
                                    uint32_t count)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    if (!snapshot || !name || (count && !arguments)) __builtin_trap();
    uint32_t match = 0;
    size_t length = 0;
    while (name[length]) length++;
    if (n00b_abi_v1_find_named_type(snapshot, (const uint8_t *)name,
            length, arguments, count, &match) != 1) __builtin_trap();
    return match;
}

typedef void *(*n00bb_box_allocator_t)(uint32_t, uint32_t, uint64_t,
                                        uint32_t, void *);

void n00bb_bootstrap_new_array_box(uint32_t type_index, uint64_t count,
                                   uint32_t rank, uint32_t flat_rank,
                                   const uint64_t *extents,
                                   n00bb_box_allocator_t allocate,
                                   n00b_abi_box_v1 *result)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    const n00b_abi_type_v1 *array_type = snapshot
        ? n00b_abi_v1_type(snapshot, type_index) : NULL;
    if (!array_type || array_type->kind != N00B_ABI_TYPE_ARRAY
        || !rank || !flat_rank || flat_rank > rank || !extents
        || !allocate || !result) __builtin_trap();
    uint32_t leaf_index = type_index;
    for (uint32_t i = 0; i < flat_rank; i++) {
        const n00b_abi_type_v1 *parent = n00b_abi_v1_type(snapshot,
                                                           leaf_index);
        if (!parent || parent->kind != N00B_ABI_TYPE_ARRAY
            || parent->components.count != 1
            || n00b_abi_v1_index_at(snapshot, parent->components, 0,
                &leaf_index) != 1) __builtin_trap();
    }
    const n00b_abi_type_v1 *leaf = n00b_abi_v1_type(snapshot, leaf_index);
    if (!leaf || !leaf->size_bits || !leaf->alignment_bytes
        || (leaf->alignment_bytes & (leaf->alignment_bytes - 1)))
        __builtin_trap();
    uint64_t alignment = leaf->alignment_bytes > 8
        ? leaf->alignment_bytes : 8;
    uint64_t descriptor_bytes = 0, data_offset = 0, data_bytes = 0;
    if (__builtin_mul_overflow((uint64_t)rank, sizeof(uint64_t),
            &descriptor_bytes)
        || __builtin_add_overflow(descriptor_bytes,
            sizeof(n00b_abi_array_descriptor_header_v1), &descriptor_bytes)
        || __builtin_add_overflow(descriptor_bytes, alignment - 1,
            &data_offset)) __builtin_trap();
    data_offset &= ~(alignment - 1);
    uint64_t packed_width = (leaf->flags & N00B_ABI_TYPE_FLAG_PACKABLE)
        ? leaf->semantic_width_bits : 0;
    if (packed_width) {
        uint64_t bits = 0;
        if (__builtin_mul_overflow(count, packed_width, &bits)
            || __builtin_add_overflow(bits, 7, &bits)) __builtin_trap();
        data_bytes = bits / 8;
    } else {
        uint64_t alignment_bits = alignment * 8;
        uint64_t stride_bits = 0;
        if (__builtin_add_overflow(leaf->size_bits, alignment_bits - 1,
                &stride_bits)) __builtin_trap();
        stride_bits = stride_bits / alignment_bits * alignment_bits;
        if (__builtin_mul_overflow(count, stride_bits / 8, &data_bytes))
            __builtin_trap();
    }
    uint64_t total_bytes = 0;
    if (__builtin_add_overflow(data_offset,
            data_bytes ? data_bytes : 1, &total_bytes)
        || alignment > UINT32_MAX
        || descriptor_bytes > SIZE_MAX) __builtin_trap();
    n00b_abi_array_descriptor_header_v1 *initial_shape =
        __builtin_alloca((size_t)descriptor_bytes);
    *initial_shape = (n00b_abi_array_descriptor_header_v1){
        .total_count = count, .rank = rank, .flat_rank = flat_rank};
    uint64_t *initial_extents = (void *)(initial_shape + 1);
    for (uint32_t i = 0; i < rank; i++)
        initial_extents[i] = i < flat_rank ? extents[i] : 0;
    uint8_t *storage = allocate(N00B_ABI_ALLOCATOR_GC, type_index,
        total_bytes, (uint32_t)alignment, initial_shape);
    if (!storage) __builtin_trap();
    n00b_abi_array_descriptor_header_v1 *descriptor = (void *)storage;
    uint8_t *data = storage + data_offset;
    n00b_abi_array_value_v1 *value = allocate(N00B_ABI_ALLOCATOR_GC,
        type_index, sizeof(*value), 8, NULL);
    if (!value) __builtin_trap();
    *value = (n00b_abi_array_value_v1){.data = data,
        .descriptor = descriptor};
    *result = (n00b_abi_box_v1){.type_field = type_index};
    result->payload.out_of_line = value;
}

void n00bb_bootstrap_array_prefix_box(const n00b_abi_box_v1 *source,
                                      uint64_t count,
                                      n00bb_box_allocator_t allocate,
                                      n00b_abi_box_v1 *result)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    uint32_t type_index = source ? source->type_field & UINT32_C(0x7fffffff)
                                 : 0;
    const n00b_abi_type_v1 *type = snapshot
        ? n00b_abi_v1_type(snapshot, type_index) : NULL;
    const n00b_abi_array_value_v1 *value = source
        ? source->payload.out_of_line : NULL;
    const n00b_abi_array_descriptor_header_v1 *descriptor = value
        ? value->descriptor : NULL;
    if (!type || type->kind != N00B_ABI_TYPE_ARRAY || !allocate || !result
        || !value || !descriptor || descriptor->rank != 1
        || descriptor->flat_rank != 1 || descriptor->reserved_zero
        || count > descriptor->total_count
        || count > ((const uint64_t *)(descriptor + 1))[0])
        __builtin_trap();
    uint64_t bytes = sizeof(n00b_abi_array_value_v1)
        + sizeof(n00b_abi_array_descriptor_header_v1) + sizeof(uint64_t);
    uint8_t *storage = allocate(N00B_ABI_ALLOCATOR_GC, type_index, bytes,
                                8, NULL);
    if (!storage) __builtin_trap();
    n00b_abi_array_value_v1 *prefix = (void *)storage;
    n00b_abi_array_descriptor_header_v1 *shape = (void *)(prefix + 1);
    uint64_t *extent = (void *)(shape + 1);
    *shape = *descriptor;
    shape->total_count = count;
    *extent = count;
    *prefix = (n00b_abi_array_value_v1){.data = value->data,
                                         .descriptor = shape};
    *result = (n00b_abi_box_v1){.type_field = type_index};
    result->payload.out_of_line = prefix;
}

void n00bb_bootstrap_array_clear_slot(const n00b_abi_array_value_v1 *value,
                                      uint32_t type_index, uint64_t index)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    const n00b_abi_type_v1 *array = snapshot
        ? n00b_abi_v1_type(snapshot, type_index) : NULL;
    const n00b_abi_array_descriptor_header_v1 *shape = value
        ? value->descriptor : NULL;
    uint32_t element_index = 0;
    if (!array || array->kind != N00B_ABI_TYPE_ARRAY
        || array->components.count != 1
        || n00b_abi_v1_index_at(snapshot, array->components, 0,
                               &element_index) != 1
        || !value || !value->data || !shape || shape->rank != 1
        || shape->flat_rank != 1 || shape->reserved_zero
        || index >= shape->total_count
        || index >= ((const uint64_t *)(shape + 1))[0]) __builtin_trap();
    const n00b_abi_type_v1 *element = n00b_abi_v1_type(snapshot,
                                                       element_index);
    if (!element) __builtin_trap();
    uint8_t *data = value->data;
    if (element->flags & N00B_ABI_TYPE_FLAG_PACKABLE) {
        uint64_t width = element->semantic_width_bits;
        uint64_t position = 0, last = 0;
        if (!width || width > 128 || shape->flags & ~N00B_ABI_ARRAY_ORIGIN_BIT_MASK
            || __builtin_mul_overflow(index, width, &position)
            || __builtin_add_overflow(position,
                shape->flags & N00B_ABI_ARRAY_ORIGIN_BIT_MASK,
                &position)
            || __builtin_add_overflow(position, width - 1, &last))
            __builtin_trap();
        for (uint64_t bit = 0; bit < width; bit++) {
            uint64_t at = position + bit;
            data[at >> 3] &= (uint8_t)~(1u << (at & 7));
        }
        return;
    }
    uint64_t alignment = element->alignment_bytes;
    uint64_t width = element->size_bits;
    uint64_t stride_bits = 0, offset = 0;
    if (!alignment || alignment > UINT64_MAX / 8
        || (alignment & (alignment - 1))
        || __builtin_add_overflow(width, alignment * 8 - 1,
                                  &stride_bits)) __builtin_trap();
    stride_bits = stride_bits / (alignment * 8) * (alignment * 8);
    if (stride_bits & 7 || __builtin_mul_overflow(index,
            stride_bits / 8, &offset)
        || offset > UINT64_MAX - stride_bits / 8) __builtin_trap();
    for (uint64_t i = 0; i < stride_bits / 8; i++) data[offset + i] = 0;
}

void n00bb_bootstrap_new_box(uint32_t type_index,
                             n00bb_box_allocator_t allocate,
                             n00b_abi_box_v1 *result)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    const n00b_abi_type_v1 *type = snapshot
        ? n00b_abi_v1_type(snapshot, type_index) : NULL;
    if (!type || !allocate || !result || !type->alignment_bytes
        || type->size_bits > SIZE_MAX - 7) __builtin_trap();
    size_t bytes = (size_t)((type->size_bits + 7) / 8);
    *result = (n00b_abi_box_v1){.type_field = type_index};
    if (bytes <= sizeof(uint64_t)) return;
    result->payload.out_of_line = allocate(N00B_ABI_ALLOCATOR_GC,
        type_index, bytes, type->alignment_bytes, NULL);
    if (!result->payload.out_of_line) __builtin_trap();
}

void n00bb_bootstrap_new_ref_box(uint32_t type_index,
                                  n00bb_box_allocator_t allocate,
                                  n00b_abi_box_v1 *result)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    const n00b_abi_type_v1 *type = snapshot
        ? n00b_abi_v1_type(snapshot, type_index) : NULL;
    uint32_t ref_index = type
        ? n00bb_bootstrap_reference_type(type_index) : 0;
    const n00b_abi_type_v1 *reference = ref_index
        ? n00b_abi_v1_type(snapshot, ref_index) : NULL;
    if (!type || !reference || !allocate || !result
        || !type->alignment_bytes || type->size_bits > SIZE_MAX - 7)
        __builtin_trap();
    size_t bytes = (size_t)((type->size_bits + 7) / 8);
    void *storage = allocate(N00B_ABI_ALLOCATOR_GC, type_index,
        bytes ? bytes : 1, type->alignment_bytes, NULL);
    if (!storage) __builtin_trap();
    *result = (n00b_abi_box_v1){.type_field = ref_index};
    if (reference->size_bits == 64) {
        result->payload.inline_bits = (uintptr_t)storage;
        return;
    }
    if (reference->size_bits != 128 || !type->semantic_width_bits
        || type->semantic_width_bits > UINT16_MAX) __builtin_trap();
    n00b_abi_bitptr_v1 *bitptr = allocate(N00B_ABI_ALLOCATOR_GC,
        ref_index, sizeof(*bitptr), 8, NULL);
    if (!bitptr) __builtin_trap();
    *bitptr = (n00b_abi_bitptr_v1){
        .base = storage, .type_index = type_index,
        .start_bit = 0, .end_bit = (uint16_t)type->semantic_width_bits,
    };
    result->payload.out_of_line = bitptr;
}

void n00bb_bootstrap_aggregate_literal_box(uint32_t type_index,
                                        n00bb_box_allocator_t allocate,
                                        n00b_abi_box_v1 *result)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    const n00b_abi_type_v1 *type = snapshot
        ? n00b_abi_v1_type(snapshot, type_index) : NULL;
    if (!type || (type->kind != N00B_ABI_TYPE_RECORD
        && type->kind != N00B_ABI_TYPE_TUPLE))
        __builtin_trap();
    n00bb_bootstrap_new_box(type_index, allocate, result);
    size_t bytes = (size_t)((type->size_bits + 7) / 8);
    if (bytes > sizeof(uint64_t)) {
        uint8_t *storage = result->payload.out_of_line;
        for (size_t i = 0; i < bytes; i++) storage[i] = 0;
    }
}

void n00bb_bootstrap_store_box(void *address, uint32_t type_index,
                               const n00b_abi_box_v1 *source)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    const n00b_abi_type_v1 *type = snapshot
        ? n00b_abi_v1_type(snapshot, type_index) : NULL;
    if (!address || !source || !type || !type->alignment_bytes
        || type->size_bits > SIZE_MAX - 7
        || (source->type_field & UINT32_C(0x7fffffff)) != type_index)
        __builtin_trap();
    size_t bytes = (size_t)((type->size_bits + 7) / 8);
    const uint8_t *value = bytes <= sizeof(uint64_t)
        ? (const uint8_t *)&source->payload.inline_bits
        : source->payload.out_of_line;
    if (!value) __builtin_trap();
    n00b_gc_barrier_value(value, type_index);
    uint8_t *target = address;
    for (size_t i = 0; i < bytes; i++) target[i] = value[i];
}

void n00bb_bootstrap_barrier_box(uint32_t type_field, uint64_t payload)
{
    uint32_t index = type_field & UINT32_C(0x7fffffff);
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    const n00b_abi_type_v1 *type = snapshot
        ? n00b_abi_v1_type(snapshot, index) : NULL;
    if (!index || !type) __builtin_trap();
    bool indirect = (type_field & UINT32_C(0x80000000))
        || type->size_bits > 64;
    if (indirect) {
        if (!payload) __builtin_trap();
        n00b_gc_barrier_value((const void *)(uintptr_t)payload, index);
    } else {
        n00b_gc_barrier_value(&payload, index);
    }
}

void *n00bb_bootstrap_array_element(uint32_t type_field, uint64_t payload,
                                    int64_t index, uint32_t *element_index)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    const n00b_abi_type_v1 *array_type = snapshot
        ? n00b_abi_v1_type(snapshot, type_field & UINT32_C(0x7fffffff)) : NULL;
    if (!array_type || array_type->kind != N00B_ABI_TYPE_ARRAY
        || array_type->components.count != 1 || !payload || !element_index)
        __builtin_trap();
    uint32_t child = 0;
    if (n00b_abi_v1_index_at(snapshot, array_type->components, 0, &child) != 1)
        __builtin_trap();
    const n00b_abi_type_v1 *element = n00b_abi_v1_type(snapshot, child);
    const n00b_abi_array_value_v1 *array =
        (const n00b_abi_array_value_v1 *)(uintptr_t)payload;
    const n00b_abi_array_descriptor_header_v1 *descriptor = array->descriptor;
    if (!element || !descriptor || !descriptor->rank
        || descriptor->flat_rank != 1
        || descriptor->flags || !element->alignment_bytes
        || !element->size_bits || element->size_bits > UINT64_MAX - 7
        || ((element->flags & N00B_ABI_TYPE_FLAG_PACKABLE)
            && element->semantic_width_bits
            && element->semantic_width_bits < 8))
        __builtin_trap();
    uint64_t length = ((const uint64_t *)(descriptor + 1))[0];
    uint64_t position = index < 0
        ? length >= (uint64_t)(-(index + 1)) + 1
            ? length - ((uint64_t)(-(index + 1)) + 1) : UINT64_MAX
        : (uint64_t)index;
    if (position >= length) __builtin_trap();
    uint64_t alignment_bits = (uint64_t)element->alignment_bytes * 8;
    if (!alignment_bits || element->size_bits > UINT64_MAX - alignment_bits + 1)
        __builtin_trap();
    uint64_t stride_bits = (element->size_bits + alignment_bits - 1)
        / alignment_bits * alignment_bits;
    if (stride_bits % 8 || position > SIZE_MAX / (stride_bits / 8)
        || !array->data) __builtin_trap();
    *element_index = child;
    return (uint8_t *)array->data + (size_t)position * (size_t)(stride_bits / 8);
}

void *n00bb_bootstrap_array_slot(const n00b_abi_box_v1 *array,
                                 uint32_t type_index,
                                 int64_t index, uint32_t *element_index)
{
    if (!array || !array->payload.out_of_line
        || (array->type_field & UINT32_C(0x7fffffff)) != type_index)
        __builtin_trap();
    return n00bb_bootstrap_array_element(type_index,
        (uint64_t)(uintptr_t)array->payload.out_of_line, index,
        element_index);
}

void n00bb_bootstrap_copy_array_box(const n00b_abi_box_v1 *source,
                                    n00bb_box_allocator_t allocate,
                                    n00b_abi_box_v1 *result)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    uint32_t type_index = source
        ? source->type_field & UINT32_C(0x7fffffff) : 0;
    const n00b_abi_type_v1 *type = snapshot
        ? n00b_abi_v1_type(snapshot, type_index) : NULL;
    if (!type || type->kind != N00B_ABI_TYPE_ARRAY || !allocate || !result
        || !source->payload.out_of_line) __builtin_trap();
    const n00b_abi_array_value_v1 *array = source->payload.out_of_line;
    const n00b_abi_array_descriptor_header_v1 *descriptor =
        array->descriptor;
    if (!descriptor || !descriptor->rank || !descriptor->flat_rank
        || descriptor->flat_rank > descriptor->rank
        || (descriptor->flags & ~N00B_ABI_ARRAY_ORIGIN_BIT_MASK))
        __builtin_trap();
    uint32_t child_index = type_index;
    for (uint32_t level = 0; level < descriptor->flat_rank; level++) {
        const n00b_abi_type_v1 *parent = n00b_abi_v1_type(snapshot,
                                                           child_index);
        if (!parent || parent->kind != N00B_ABI_TYPE_ARRAY
            || n00b_abi_v1_index_at(snapshot, parent->components, 0,
                                   &child_index) != 1) __builtin_trap();
    }
    const n00b_abi_type_v1 *element = n00b_abi_v1_type(snapshot, child_index);
    if (!element || !element->alignment_bytes || !element->size_bits)
        __builtin_trap();
    uint64_t alignment = element->alignment_bytes > 8
        ? element->alignment_bytes : 8;
    size_t extents_bytes = 0, header_bytes = 0;
    if (alignment & (alignment - 1)
        || __builtin_mul_overflow((size_t)descriptor->rank,
            sizeof(uint64_t), &extents_bytes)
        || __builtin_add_overflow(sizeof(*descriptor), extents_bytes,
            &header_bytes)) __builtin_trap();
    if (header_bytes > SIZE_MAX - (alignment - 1)) __builtin_trap();
    size_t data_offset = (header_bytes + alignment - 1) & ~(alignment - 1);
    uint64_t count = descriptor->total_count;
    uint64_t packed_width = (element->flags & N00B_ABI_TYPE_FLAG_PACKABLE)
        ? element->semantic_width_bits : 0;
    size_t data_bytes;
    if (packed_width) {
        if (count > (SIZE_MAX - descriptor->flags - 7) / packed_width)
            __builtin_trap();
        data_bytes = (size_t)((count * packed_width
            + descriptor->flags + 7) / 8);
    } else {
        uint64_t alignment_bits = alignment * 8;
        if (element->size_bits > UINT64_MAX - alignment_bits + 1)
            __builtin_trap();
        uint64_t stride = ((element->size_bits + alignment_bits - 1)
            / alignment_bits) * alignment;
        if (count > SIZE_MAX / stride) __builtin_trap();
        data_bytes = (size_t)(count * stride);
    }
    if (data_offset > SIZE_MAX - (data_bytes ? data_bytes : 1))
        __builtin_trap();
    uint8_t *backing = allocate(N00B_ABI_ALLOCATOR_GC, type_index,
        data_offset + (data_bytes ? data_bytes : 1), (uint32_t)alignment,
        (void *)descriptor);
    if (!backing || (data_bytes && !array->data)) __builtin_trap();
    const uint8_t *old_header = (const uint8_t *)descriptor;
    for (size_t i = 0; i < header_bytes; i++) backing[i] = old_header[i];
    uint8_t *data = backing + data_offset;
    const uint8_t *old_data = array->data;
    for (size_t i = 0; i < data_bytes; i++) data[i] = old_data[i];
    if (!data_bytes) data[0] = 0;
    n00b_abi_array_value_v1 *copy = allocate(N00B_ABI_ALLOCATOR_GC,
        type_index, sizeof(*copy), 8, NULL);
    if (!copy) __builtin_trap();
    *copy = (n00b_abi_array_value_v1){.data = data,
        .descriptor = (n00b_abi_array_descriptor_header_v1 *)backing};
    *result = (n00b_abi_box_v1){.type_field = type_index};
    result->payload.out_of_line = copy;
}

static void n00bb_bootstrap_load_box_known(const void *address,
    uint32_t type_index, const n00b_abi_type_v1 *type,
    n00b_abi_box_v1 *result)
{
    if (!address || !result || !type || !type_index
        || !type->alignment_bytes || !type->size_bits
        || type->size_bits > SIZE_MAX - 7) __builtin_trap();
    if (type->kind == N00B_ABI_TYPE_BOX) {
        *result = *(const n00b_abi_box_v1 *)address;
        return;
    }
    size_t bytes = (size_t)((type->size_bits + 7) / 8);
    *result = (n00b_abi_box_v1){.type_field = type_index};
    uint8_t *target = bytes <= sizeof(uint64_t)
        ? (uint8_t *)&result->payload.inline_bits
        : n00b_allocate(N00B_ABI_ALLOCATOR_GC, type_index, bytes,
            type->alignment_bytes, NULL);
    if (!target) __builtin_trap();
    const uint8_t *source = address;
    for (size_t i = 0; i < bytes; i++) target[i] = source[i];
    if (bytes > sizeof(uint64_t)) result->payload.out_of_line = target;
}

void n00bb_bootstrap_load_box(const void *address, uint32_t type_index,
                              n00b_abi_box_v1 *result)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    const n00b_abi_type_v1 *type = snapshot
        ? n00b_abi_v1_type(snapshot, type_index) : NULL;
    n00bb_bootstrap_load_box_known(address, type_index, type, result);
}

static void n00bb_bootstrap_array_suffix_box(
    const n00b_abi_snapshot_v1 *snapshot,
    const n00b_abi_array_value_v1 *array,
    const n00b_abi_array_descriptor_header_v1 *descriptor,
    uint32_t child_index, uint64_t position, n00b_abi_box_v1 *result)
{
    uint32_t rank = 1;
    uint32_t leaf_index = child_index;
    const n00b_abi_type_v1 *leaf = n00b_abi_v1_type(snapshot, leaf_index);
    while (leaf && leaf->kind == N00B_ABI_TYPE_ARRAY) {
        if (leaf->components.count != 1
            || n00b_abi_v1_index_at(snapshot, leaf->components, 0,
                &leaf_index) != 1) __builtin_trap();
        leaf = n00b_abi_v1_type(snapshot, leaf_index);
        rank++;
    }
    if (!leaf || rank != descriptor->rank || rank < 2
        || descriptor->flat_rank < 2
        || descriptor->flat_rank > rank || !leaf->alignment_bytes)
        __builtin_trap();
    const uint64_t *extents = (const uint64_t *)(descriptor + 1);
    if (!extents[0] || descriptor->total_count % extents[0])
        __builtin_trap();
    uint64_t row_count = descriptor->total_count / extents[0];
    uint64_t packed_width = (leaf->flags & N00B_ABI_TYPE_FLAG_PACKABLE)
        ? leaf->semantic_width_bits : 0;
    uint64_t item_bits = packed_width;
    if (!item_bits) {
        uint64_t alignment_bits = (uint64_t)leaf->alignment_bytes * 8;
        if (!leaf->size_bits || !alignment_bits
            || leaf->size_bits > UINT64_MAX - alignment_bits + 1)
            __builtin_trap();
        item_bits = (leaf->size_bits + alignment_bits - 1)
            / alignment_bits * alignment_bits;
    }
    uint64_t bit_offset = 0;
    if (!item_bits || __builtin_mul_overflow(position, row_count,
            &bit_offset)
        || __builtin_mul_overflow(bit_offset, item_bits, &bit_offset)
        || __builtin_add_overflow(bit_offset,
            (uint64_t)descriptor->flags, &bit_offset)) __builtin_trap();
    uint32_t origin = packed_width
        ? (uint32_t)(bit_offset & N00B_ABI_ARRAY_ORIGIN_BIT_MASK) : 0;
    if (!packed_width && (bit_offset & 7)) __builtin_trap();
    uint64_t byte_offset = bit_offset / 8;
    if (byte_offset > SIZE_MAX || !array->data) __builtin_trap();
    size_t extents_bytes = 0, descriptor_bytes = 0, allocation_bytes = 0;
    if (__builtin_mul_overflow((size_t)(rank - 1), sizeof(uint64_t),
            &extents_bytes)
        || __builtin_add_overflow(sizeof(*descriptor), extents_bytes,
            &descriptor_bytes)
        || __builtin_add_overflow(sizeof(n00b_abi_array_value_v1),
            descriptor_bytes, &allocation_bytes)) __builtin_trap();
    uint8_t *storage = n00b_allocate(N00B_ABI_ALLOCATOR_GC, child_index,
        allocation_bytes, 8, NULL);
    if (!storage) __builtin_trap();
    n00b_abi_array_value_v1 *suffix = (void *)storage;
    n00b_abi_array_descriptor_header_v1 *suffix_descriptor =
        (void *)(storage + sizeof(*suffix));
    *suffix_descriptor = (n00b_abi_array_descriptor_header_v1){
        .total_count = row_count, .rank = rank - 1,
        .flat_rank = descriptor->flat_rank - 1, .flags = origin,
    };
    uint64_t *suffix_extents = (uint64_t *)(suffix_descriptor + 1);
    for (uint32_t i = 1; i < rank; i++) suffix_extents[i - 1] = extents[i];
    *suffix = (n00b_abi_array_value_v1){
        .data = (uint8_t *)array->data + (size_t)byte_offset,
        .descriptor = suffix_descriptor,
    };
    *result = (n00b_abi_box_v1){.type_field = child_index};
    result->payload.out_of_line = suffix;
}

void n00bb_bootstrap_array_read_box(const n00b_abi_box_v1 *source,
                                    int64_t index, n00b_abi_box_v1 *result)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    uint32_t array_index = source
        ? source->type_field & UINT32_C(0x7fffffff) : 0;
    const n00b_abi_type_v1 *array_type = snapshot
        ? n00b_abi_v1_type(snapshot, array_index) : NULL;
    if (!source || !result || !array_type
        || array_type->kind != N00B_ABI_TYPE_ARRAY
        || array_type->components.count != 1
        || !source->payload.out_of_line) __builtin_trap();
    uint32_t child_index = 0;
    if (n00b_abi_v1_index_at(snapshot, array_type->components, 0,
                            &child_index) != 1) __builtin_trap();
    const n00b_abi_type_v1 *element = n00b_abi_v1_type(snapshot, child_index);
    const n00b_abi_array_value_v1 *array = source->payload.out_of_line;
    const n00b_abi_array_descriptor_header_v1 *descriptor = array->descriptor;
    if (!element || !descriptor || !array->data || !descriptor->rank
        || !descriptor->flat_rank
        || (descriptor->flags & ~N00B_ABI_ARRAY_ORIGIN_BIT_MASK))
        __builtin_trap();
    uint64_t length = ((const uint64_t *)(descriptor + 1))[0];
    uint64_t position = index < 0
        ? length >= (uint64_t)(-(index + 1)) + 1
            ? length - ((uint64_t)(-(index + 1)) + 1) : UINT64_MAX
        : (uint64_t)index;
    if (position >= length) __builtin_trap();
    if (descriptor->flat_rank > 1) {
        n00bb_bootstrap_array_suffix_box(snapshot, array, descriptor,
                                          child_index, position, result);
        return;
    }
    uint32_t width = (element->flags & N00B_ABI_TYPE_FLAG_PACKABLE)
        ? element->semantic_width_bits : 0;
    if (!width) {
        uint64_t alignment_bits = (uint64_t)element->alignment_bytes * 8;
        if (descriptor->flat_rank != 1 || descriptor->flags
            || !element->alignment_bytes || !element->size_bits
            || element->size_bits > UINT64_MAX - alignment_bits + 1
            || !alignment_bits) __builtin_trap();
        uint64_t stride_bits = (element->size_bits + alignment_bits - 1)
            / alignment_bits * alignment_bits;
        if (stride_bits % 8 || position > SIZE_MAX / (stride_bits / 8))
            __builtin_trap();
        const uint8_t *address = (const uint8_t *)array->data
            + (size_t)position * (size_t)(stride_bits / 8);
        n00bb_bootstrap_load_box_known(address, child_index, element, result);
        return;
    }
    if (width > 128 || !element->alignment_bytes
        || element->size_bits < width || element->size_bits > SIZE_MAX - 7
        || position > (UINT64_MAX - descriptor->flags - width) / width)
        __builtin_trap();
    size_t bytes = (size_t)((element->size_bits + 7) / 8);
    *result = (n00b_abi_box_v1){.type_field = child_index};
    uint8_t *target = bytes <= sizeof(uint64_t)
        ? (uint8_t *)&result->payload.inline_bits
        : n00b_allocate(N00B_ABI_ALLOCATOR_GC, child_index, bytes,
            element->alignment_bytes, NULL);
    if (!target) __builtin_trap();
    for (size_t byte = 0; byte < bytes; byte++) target[byte] = 0;
    uint64_t start = position * width + descriptor->flags;
    const uint8_t *data = array->data;
    for (uint32_t bit = 0; bit < width; bit++) {
        uint64_t source_bit = start + bit;
        target[bit / 8] |= ((data[source_bit / 8]
            >> (source_bit & 7)) & 1u) << (bit & 7);
    }
    if (bytes > sizeof(uint64_t)) result->payload.out_of_line = target;
}

void n00bb_bootstrap_array_borrow_box(const n00b_abi_box_v1 *source,
                                      int64_t index, n00b_abi_box_v1 *result)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    uint32_t array_index = source
        ? source->type_field & UINT32_C(0x7fffffff) : 0;
    const n00b_abi_type_v1 *array_type = snapshot
        ? n00b_abi_v1_type(snapshot, array_index) : NULL;
    uint32_t child_index = 0;
    if (!source || !result || !array_type
        || array_type->kind != N00B_ABI_TYPE_ARRAY
        || array_type->components.count != 1
        || n00b_abi_v1_index_at(snapshot, array_type->components, 0,
            &child_index) != 1) __builtin_trap();
    const n00b_abi_type_v1 *element = n00b_abi_v1_type(snapshot, child_index);
    const n00b_abi_array_value_v1 *array = source->payload.out_of_line;
    const n00b_abi_array_descriptor_header_v1 *descriptor = array
        ? array->descriptor : NULL;
    if (!element || !descriptor) __builtin_trap();
    if (element->size_bits <= 64
        || (element->flags & N00B_ABI_TYPE_FLAG_PACKABLE)
        || descriptor->rank != 1 || descriptor->flat_rank != 1) {
        n00bb_bootstrap_array_read_box(source, index, result);
        return;
    }
    uint32_t found = 0;
    void *address = n00bb_bootstrap_array_element(source->type_field,
        (uint64_t)(uintptr_t)array, index, &found);
    if (found != child_index) __builtin_trap();
    if (element->kind == N00B_ABI_TYPE_BOX) {
        *result = *(const n00b_abi_box_v1 *)address;
        return;
    }
    *result = (n00b_abi_box_v1){.type_field = child_index};
    result->payload.out_of_line = address;
}

void n00bb_bootstrap_array_ref_box(const n00b_abi_box_v1 *source,
                                   int64_t index,
                                   n00bb_box_allocator_t allocate,
                                   n00b_abi_box_v1 *result)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    uint32_t array_index = source
        ? source->type_field & UINT32_C(0x7fffffff) : 0;
    const n00b_abi_type_v1 *array_type = snapshot
        ? n00b_abi_v1_type(snapshot, array_index) : NULL;
    if (!source || !result || !allocate || !array_type
        || array_type->kind != N00B_ABI_TYPE_ARRAY
        || array_type->components.count != 1
        || !source->payload.out_of_line) __builtin_trap();
    uint32_t child_index = 0;
    if (n00b_abi_v1_index_at(snapshot, array_type->components, 0,
            &child_index) != 1) __builtin_trap();
    const n00b_abi_type_v1 *element = n00b_abi_v1_type(snapshot, child_index);
    const n00b_abi_array_value_v1 *array = source->payload.out_of_line;
    const n00b_abi_array_descriptor_header_v1 *descriptor = array->descriptor;
    if (!element || !descriptor || !array->data || !descriptor->rank
        || descriptor->flat_rank != 1
        || (descriptor->flags & ~N00B_ABI_ARRAY_ORIGIN_BIT_MASK))
        __builtin_trap();
    uint64_t length = ((const uint64_t *)(descriptor + 1))[0];
    uint64_t position = index < 0
        ? length >= (uint64_t)(-(index + 1)) + 1
            ? length - ((uint64_t)(-(index + 1)) + 1) : UINT64_MAX
        : (uint64_t)index;
    if (position >= length) __builtin_trap();
    uint32_t ref_index = n00bb_bootstrap_reference_type(child_index);
    const n00b_abi_type_v1 *ref_type = n00b_abi_v1_type(snapshot, ref_index);
    uint32_t width = ref_type && ref_type->size_bits == 128
        ? element->semantic_width_bits : 0;
    *result = (n00b_abi_box_v1){.type_field = ref_index};
    if (!width) {
        uint32_t found = 0;
        void *address = n00bb_bootstrap_array_element(source->type_field,
            (uint64_t)(uintptr_t)source->payload.out_of_line, index, &found);
        if (found != child_index || !ref_type || ref_type->size_bits != 64)
            __builtin_trap();
        result->payload.inline_bits = (uintptr_t)address;
        return;
    }
    uint64_t bit_position = 0;
    if (!ref_type || ref_type->size_bits != 128 || width > UINT16_MAX - 7
        || __builtin_mul_overflow(position, width, &bit_position)
        || __builtin_add_overflow(bit_position, descriptor->flags,
            &bit_position)) __builtin_trap();
    n00b_abi_bitptr_v1 *bitptr = allocate(N00B_ABI_ALLOCATOR_GC,
        ref_index, sizeof(*bitptr), 8, NULL);
    if (!bitptr) __builtin_trap();
    uint16_t start = (uint16_t)(bit_position & 7);
    *bitptr = (n00b_abi_bitptr_v1){
        .base = (uint8_t *)array->data + bit_position / 8,
        .type_index = child_index, .start_bit = start,
        .end_bit = (uint16_t)(start + width),
    };
    result->payload.out_of_line = bitptr;
}

void n00bb_bootstrap_ref_read_box(const n00b_abi_box_v1 *source,
                                  n00b_abi_box_v1 *result)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    uint32_t ref_index = source
        ? source->type_field & UINT32_C(0x7fffffff) : 0;
    const n00b_abi_type_v1 *ref_type = snapshot
        ? n00b_abi_v1_type(snapshot, ref_index) : NULL;
    uint32_t child_index = 0;
    if (!source || !result || !ref_type
        || ref_type->kind != N00B_ABI_TYPE_REFERENCE
        || ref_type->components.count != 1
        || n00b_abi_v1_index_at(snapshot, ref_type->components, 0,
            &child_index) != 1) __builtin_trap();
    if (ref_type->size_bits == 64) {
        n00bb_bootstrap_load_box(
            (const void *)(uintptr_t)source->payload.inline_bits,
            child_index, result);
        return;
    }
    const n00b_abi_type_v1 *element = n00b_abi_v1_type(snapshot, child_index);
    const n00b_abi_bitptr_v1 *bitptr = source->payload.out_of_line;
    uint32_t width = element ? element->semantic_width_bits : 0;
    if (ref_type->size_bits != 128 || !bitptr || !bitptr->base
        || bitptr->type_index != child_index || !width || width > 128
        || bitptr->start_bit > 7
        || bitptr->end_bit != bitptr->start_bit + width
        || element->size_bits > SIZE_MAX - 7 || !element->alignment_bytes)
        __builtin_trap();
    size_t bytes = (size_t)((element->size_bits + 7) / 8);
    *result = (n00b_abi_box_v1){.type_field = child_index};
    uint8_t *target = bytes <= sizeof(uint64_t)
        ? (uint8_t *)&result->payload.inline_bits
        : n00b_allocate(N00B_ABI_ALLOCATOR_GC, child_index, bytes,
            element->alignment_bytes, NULL);
    if (!target) __builtin_trap();
    for (size_t i = 0; i < bytes; i++) target[i] = 0;
    const uint8_t *data = bitptr->base;
    for (uint32_t bit = 0; bit < width; bit++) {
        uint32_t source_bit = bitptr->start_bit + bit;
        target[bit / 8] |= ((data[source_bit / 8]
            >> (source_bit & 7)) & 1u) << (bit & 7);
    }
    if (bytes > sizeof(uint64_t)) result->payload.out_of_line = target;
}

void n00bb_bootstrap_ref_write_box(const n00b_abi_box_v1 *target,
                                   const n00b_abi_box_v1 *value)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    uint32_t ref_index = target
        ? target->type_field & UINT32_C(0x7fffffff) : 0;
    const n00b_abi_type_v1 *ref_type = snapshot
        ? n00b_abi_v1_type(snapshot, ref_index) : NULL;
    uint32_t child_index = 0;
    if (!target || !value || !ref_type
        || ref_type->kind != N00B_ABI_TYPE_REFERENCE
        || ref_type->components.count != 1
        || n00b_abi_v1_index_at(snapshot, ref_type->components, 0,
            &child_index) != 1
        || (value->type_field & UINT32_C(0x7fffffff)) != child_index)
        __builtin_trap();
    if (ref_type->size_bits == 64) {
        n00bb_bootstrap_store_box(
            (void *)(uintptr_t)target->payload.inline_bits,
            child_index, value);
        return;
    }
    const n00b_abi_type_v1 *element = n00b_abi_v1_type(snapshot, child_index);
    const n00b_abi_bitptr_v1 *bitptr = target->payload.out_of_line;
    uint32_t width = element ? element->semantic_width_bits : 0;
    if (ref_type->size_bits != 128 || !bitptr || !bitptr->base
        || bitptr->type_index != child_index || !width || width > 128
        || bitptr->start_bit > 7
        || bitptr->end_bit != bitptr->start_bit + width
        || element->size_bits > SIZE_MAX - 7 || !element->alignment_bytes)
        __builtin_trap();
    size_t bytes = (size_t)((element->size_bits + 7) / 8);
    const uint8_t *bits = bytes <= sizeof(uint64_t)
        ? (const uint8_t *)&value->payload.inline_bits
        : value->payload.out_of_line;
    if (!bits) __builtin_trap();
    uint8_t *data = bitptr->base;
    for (uint32_t bit = 0; bit < width; bit++) {
        uint32_t target_bit = bitptr->start_bit + bit;
        uint8_t mask = (uint8_t)(1u << (target_bit & 7));
        if ((bits[bit / 8] >> (bit & 7)) & 1u)
            data[target_bit / 8] |= mask;
        else data[target_bit / 8] &= (uint8_t)~mask;
    }
}

void *n00bb_bootstrap_ref_address_box(const n00b_abi_box_v1 *source)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    const n00b_abi_type_v1 *ref_type = source && snapshot
        ? n00b_abi_v1_type(snapshot,
            source->type_field & UINT32_C(0x7fffffff)) : NULL;
    if (!ref_type || ref_type->kind != N00B_ABI_TYPE_REFERENCE
        || ref_type->size_bits != 64 || !source->payload.inline_bits)
        __builtin_trap();
    return (void *)(uintptr_t)source->payload.inline_bits;
}

int32_t n00bb_bootstrap_ref_equal_box(const n00b_abi_box_v1 *left,
                                      const n00b_abi_box_v1 *right)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    const n00b_abi_type_v1 *left_type = left && snapshot
        ? n00b_abi_v1_type(snapshot,
            left->type_field & UINT32_C(0x7fffffff)) : NULL;
    const n00b_abi_type_v1 *right_type = right && snapshot
        ? n00b_abi_v1_type(snapshot,
            right->type_field & UINT32_C(0x7fffffff)) : NULL;
    if (!left_type || !right_type
        || left_type->kind != N00B_ABI_TYPE_REFERENCE
        || right_type->kind != N00B_ABI_TYPE_REFERENCE)
        __builtin_trap();
    if (left_type->size_bits != right_type->size_bits) return 0;
    if (left_type->size_bits == 64)
        return left->payload.inline_bits == right->payload.inline_bits;
    if (left_type->size_bits != 128) __builtin_trap();
    const n00b_abi_bitptr_v1 *a = left->payload.out_of_line;
    const n00b_abi_bitptr_v1 *b = right->payload.out_of_line;
    if (!a || !b) __builtin_trap();
    return a->base == b->base && a->type_index == b->type_index
        && a->start_bit == b->start_bit && a->end_bit == b->end_bit;
}

void n00bb_bootstrap_array_ref_from_ref_box(
    const n00b_abi_box_v1 *reference, int64_t index,
    n00bb_box_allocator_t allocate, n00b_abi_box_v1 *result)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    const n00b_abi_type_v1 *type = reference && snapshot
        ? n00b_abi_v1_type(snapshot,
            reference->type_field & UINT32_C(0x7fffffff)) : NULL;
    uint32_t array_index = 0;
    if (!type || type->kind != N00B_ABI_TYPE_REFERENCE
        || type->components.count != 1
        || n00b_abi_v1_index_at(snapshot, type->components, 0,
            &array_index) != 1) __builtin_trap();
    n00b_abi_box_v1 array = {
        .type_field = array_index,
        .payload.out_of_line = n00bb_bootstrap_ref_address_box(reference),
    };
    n00bb_bootstrap_array_ref_box(&array, index, allocate, result);
}

void n00bb_bootstrap_fresh_ref_value_box(const n00b_abi_box_v1 *reference,
                                          n00b_abi_box_v1 *result)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    const n00b_abi_type_v1 *ref_type = reference && snapshot
        ? n00b_abi_v1_type(snapshot,
            reference->type_field & UINT32_C(0x7fffffff)) : NULL;
    uint32_t child_index = 0;
    if (!ref_type || !result || ref_type->kind != N00B_ABI_TYPE_REFERENCE
        || ref_type->components.count != 1
        || n00b_abi_v1_index_at(snapshot, ref_type->components, 0,
            &child_index) != 1) __builtin_trap();
    const n00b_abi_type_v1 *child = n00b_abi_v1_type(snapshot, child_index);
    if (!child) __builtin_trap();
    *result = (n00b_abi_box_v1){.type_field = child_index};
    if (child->size_bits > 64) {
        if (ref_type->size_bits != 64) __builtin_trap();
        result->payload.out_of_line =
            n00bb_bootstrap_ref_address_box(reference);
    }
}

static void n00bb_bootstrap_array_replace_row(
    const n00b_abi_snapshot_v1 *snapshot,
    n00b_abi_array_value_v1 *destination,
    const n00b_abi_array_descriptor_header_v1 *descriptor,
    uint32_t child_index, uint64_t position,
    const n00b_abi_box_v1 *value)
{
    if (!value->payload.out_of_line || descriptor->flat_rank < 2
        || descriptor->flat_rank > descriptor->rank) __builtin_trap();
    const n00b_abi_array_value_v1 *source = value->payload.out_of_line;
    const n00b_abi_array_descriptor_header_v1 *source_descriptor =
        source->descriptor;
    const uint64_t *extents = (const uint64_t *)(descriptor + 1);
    if (!source_descriptor || !source->data || !extents[0]
        || descriptor->total_count % extents[0]
        || source_descriptor->rank != descriptor->rank - 1
        || source_descriptor->flat_rank != descriptor->flat_rank - 1
        || (source_descriptor->flags & ~N00B_ABI_ARRAY_ORIGIN_BIT_MASK))
        __builtin_trap();
    uint64_t row_count = descriptor->total_count / extents[0];
    const uint64_t *source_extents =
        (const uint64_t *)(source_descriptor + 1);
    if (source_descriptor->total_count != row_count) __builtin_trap();
    for (uint32_t i = 1; i < descriptor->rank; i++)
        if (source_extents[i - 1] != extents[i]) __builtin_trap();

    uint32_t leaf_index = child_index;
    for (uint32_t i = 1; i < descriptor->flat_rank; i++) {
        const n00b_abi_type_v1 *parent = n00b_abi_v1_type(snapshot,
                                                           leaf_index);
        if (!parent || parent->kind != N00B_ABI_TYPE_ARRAY
            || parent->components.count != 1
            || n00b_abi_v1_index_at(snapshot, parent->components, 0,
                &leaf_index) != 1) __builtin_trap();
    }
    const n00b_abi_type_v1 *leaf = n00b_abi_v1_type(snapshot, leaf_index);
    if (!leaf || !leaf->size_bits || !leaf->alignment_bytes)
        __builtin_trap();
    uint64_t packed_width = (leaf->flags & N00B_ABI_TYPE_FLAG_PACKABLE)
        ? leaf->semantic_width_bits : 0;
    uint64_t item_bits = packed_width;
    if (!item_bits) {
        uint64_t alignment_bits = (uint64_t)leaf->alignment_bytes * 8;
        if (leaf->size_bits > UINT64_MAX - alignment_bits + 1)
            __builtin_trap();
        item_bits = (leaf->size_bits + alignment_bits - 1)
            / alignment_bits * alignment_bits;
    }
    uint64_t row_bits = 0, destination_start = 0;
    if (!item_bits || __builtin_mul_overflow(row_count, item_bits,
            &row_bits)
        || __builtin_mul_overflow(position, row_bits,
            &destination_start)
        || __builtin_add_overflow(destination_start, descriptor->flags,
            &destination_start)) __builtin_trap();
    uint64_t source_start = source_descriptor->flags;
    uint8_t *target = destination->data;
    const uint8_t *from = source->data;
    uintptr_t target_byte = (uintptr_t)target + destination_start / 8;
    uintptr_t source_byte = (uintptr_t)from + source_start / 8;
    bool reverse = target_byte > source_byte
        || (target_byte == source_byte
            && (destination_start & 7) > (source_start & 7));
    if (!packed_width) {
        if ((destination_start & 7) || (source_start & 7)
            || (row_bits & 7) || row_bits / 8 > SIZE_MAX) __builtin_trap();
        uint64_t stride = item_bits / 8;
        for (uint64_t i = 0; i < row_count; i++)
            n00b_gc_barrier_value(from + source_start / 8 + i * stride,
                                  leaf_index);
        size_t bytes = (size_t)(row_bits / 8);
        uint8_t *dest_bytes = target + destination_start / 8;
        const uint8_t *source_bytes = from + source_start / 8;
        if (reverse) {
            for (size_t i = bytes; i; i--)
                dest_bytes[i - 1] = source_bytes[i - 1];
        } else {
            for (size_t i = 0; i < bytes; i++)
                dest_bytes[i] = source_bytes[i];
        }
        return;
    }
    for (uint64_t i = 0; i < row_bits; i++) {
        uint64_t offset = reverse ? row_bits - 1 - i : i;
        uint64_t source_bit = source_start + offset;
        uint64_t target_bit = destination_start + offset;
        uint8_t mask = (uint8_t)(1u << (target_bit & 7));
        if ((from[source_bit / 8] >> (source_bit & 7)) & 1u)
            target[target_bit / 8] |= mask;
        else target[target_bit / 8] &= (uint8_t)~mask;
    }
}

void n00bb_bootstrap_array_write_box(n00b_abi_box_v1 *source,
                                     int64_t index,
                                     const n00b_abi_box_v1 *value)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    uint32_t array_index = source
        ? source->type_field & UINT32_C(0x7fffffff) : 0;
    const n00b_abi_type_v1 *array_type = snapshot
        ? n00b_abi_v1_type(snapshot, array_index) : NULL;
    if (!source || !value || !array_type
        || array_type->kind != N00B_ABI_TYPE_ARRAY
        || array_type->components.count != 1
        || !source->payload.out_of_line) __builtin_trap();
    uint32_t child_index = 0;
    if (n00b_abi_v1_index_at(snapshot, array_type->components, 0,
            &child_index) != 1
        || (value->type_field & UINT32_C(0x7fffffff)) != child_index)
        __builtin_trap();
    const n00b_abi_type_v1 *element = n00b_abi_v1_type(snapshot, child_index);
    n00b_abi_array_value_v1 *array = source->payload.out_of_line;
    const n00b_abi_array_descriptor_header_v1 *descriptor = array->descriptor;
    if (!element || !descriptor || !array->data || !descriptor->rank
        || !descriptor->flat_rank
        || (descriptor->flags & ~N00B_ABI_ARRAY_ORIGIN_BIT_MASK))
        __builtin_trap();
    uint64_t length = ((const uint64_t *)(descriptor + 1))[0];
    uint64_t position = index < 0
        ? length >= (uint64_t)(-(index + 1)) + 1
            ? length - ((uint64_t)(-(index + 1)) + 1) : UINT64_MAX
        : (uint64_t)index;
    if (position >= length) __builtin_trap();
    if (descriptor->flat_rank > 1) {
        n00bb_bootstrap_array_replace_row(snapshot, array, descriptor,
                                           child_index, position, value);
        return;
    }
    uint32_t width = (element->flags & N00B_ABI_TYPE_FLAG_PACKABLE)
        ? element->semantic_width_bits : 0;
    if (!width) {
        uint32_t found = 0;
        void *address = n00bb_bootstrap_array_element(source->type_field,
            (uint64_t)(uintptr_t)source->payload.out_of_line, index, &found);
        if (found != child_index) __builtin_trap();
        n00bb_bootstrap_store_box(address, child_index, value);
        return;
    }
    if (width > 128 || !element->alignment_bytes
        || element->size_bits < width || element->size_bits > SIZE_MAX - 7
        || position > (UINT64_MAX - descriptor->flags - width) / width)
        __builtin_trap();
    size_t bytes = (size_t)((element->size_bits + 7) / 8);
    const uint8_t *bits = bytes <= sizeof(uint64_t)
        ? (const uint8_t *)&value->payload.inline_bits
        : value->payload.out_of_line;
    if (!bits) __builtin_trap();
    uint64_t start = position * width + descriptor->flags;
    uint8_t *data = array->data;
    for (uint32_t bit = 0; bit < width; bit++) {
        uint64_t target_bit = start + bit;
        uint8_t mask = (uint8_t)(1u << (target_bit & 7));
        if ((bits[bit / 8] >> (bit & 7)) & 1u)
            data[target_bit / 8] |= mask;
        else data[target_bit / 8] &= (uint8_t)~mask;
    }
}

void n00bb_bootstrap_array_write_ref_box(const n00b_abi_box_v1 *reference,
                                         int64_t index,
                                         const n00b_abi_box_v1 *value)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    const n00b_abi_type_v1 *type = reference && snapshot
        ? n00b_abi_v1_type(snapshot,
            reference->type_field & UINT32_C(0x7fffffff)) : NULL;
    uint32_t array_index = 0;
    if (!type || type->kind != N00B_ABI_TYPE_REFERENCE
        || type->components.count != 1
        || n00b_abi_v1_index_at(snapshot, type->components, 0,
            &array_index) != 1) __builtin_trap();
    n00b_abi_box_v1 array = {
        .type_field = array_index,
        .payload.out_of_line = n00bb_bootstrap_ref_address_box(reference),
    };
    n00bb_bootstrap_array_write_box(&array, index, value);
}

typedef struct {
    const uint8_t *address;
    const n00b_abi_type_v1 *type;
    uint32_t type_index;
    uint64_t bit_offset;
    uint64_t width;
} n00bb_field_location_t;

static inline __attribute__((always_inline)) n00bb_field_location_t
field_location(const n00b_abi_box_v1 *box,
                                             const char *name,
                                             uint32_t ordinal)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    const n00b_abi_type_v1 *record = box && snapshot
        ? n00b_abi_v1_type(snapshot,
            box->type_field & UINT32_C(0x7fffffff)) : NULL;
    const uint8_t *base = NULL;
    if (record && record->kind == N00B_ABI_TYPE_REFERENCE) {
        uint32_t child_index = 0;
        if (record->components.count != 1
            || n00b_abi_v1_index_at(snapshot, record->components, 0,
                &child_index) != 1) __builtin_trap();
        record = n00b_abi_v1_type(snapshot, child_index);
        base = n00bb_bootstrap_ref_address_box(box);
    }
    if (!record || (record->kind != N00B_ABI_TYPE_RECORD
        && record->kind != N00B_ABI_TYPE_TUPLE)
        || (!name && ordinal == UINT32_MAX))
        __builtin_trap();
    if (!base) base = record->size_bits <= 64
        ? (const uint8_t *)&box->payload.inline_bits
        : box->payload.out_of_line;
    if (!base) __builtin_trap();
    if (ordinal != UINT32_MAX
        && ordinal >= record->fields_or_arms.count) __builtin_trap();
    uint32_t first = ordinal == UINT32_MAX ? 0 : ordinal;
    uint32_t end = ordinal == UINT32_MAX
        ? record->fields_or_arms.count : ordinal + 1;
    for (uint32_t i = first; i < end; i++) {
        uint32_t field_index = 0;
        if (n00b_abi_v1_index_at(snapshot, record->fields_or_arms, i,
                                &field_index) != 1) __builtin_trap();
        const n00b_abi_field_v1 *field = n00b_abi_v1_field(snapshot,
                                                           field_index);
        if (!field) __builtin_trap();
        if (name) {
            const uint8_t *field_name = NULL;
            uint64_t name_bytes = 0;
            if (n00b_abi_v1_string_bytes(snapshot, &field->name,
                    &field_name, &name_bytes) != 1) __builtin_trap();
            size_t at = 0;
            while (at < name_bytes && name[at]
                   && field_name[at] == (uint8_t)name[at]) at++;
            if (at != name_bytes || name[at]) continue;
        }
        const n00b_abi_type_v1 *child = n00b_abi_v1_type(snapshot,
            field->child_type_index);
        if (!child || !field->physical_runs.count) __builtin_trap();
        uint32_t run_index = 0;
        if (n00b_abi_v1_index_at(snapshot, field->physical_runs, 0,
                                &run_index) != 1) __builtin_trap();
        const n00b_abi_physical_run_v1 *run = n00b_abi_v1_record(snapshot,
            N00B_ABI_TABLE_PHYSICAL_RUN, run_index, NULL);
        if (!run || run->byte_offset > SIZE_MAX - (uintptr_t)base
            || run->byte_offset > UINT64_MAX / 8)
            __builtin_trap();
        uint64_t first_bit = run->byte_offset * 8 + run->bit_offset;
        uint64_t next_bit = 0;
        if (!run->width_bits || __builtin_add_overflow(first_bit,
                run->width_bits, &next_bit)) __builtin_trap();
        uint64_t width = run->width_bits;
        if (field->physical_runs.count != 1) {
            for (uint32_t part = 1; part < field->physical_runs.count; part++) {
                uint32_t part_index = 0;
                if (n00b_abi_v1_index_at(snapshot, field->physical_runs, part,
                        &part_index) != 1) __builtin_trap();
                const n00b_abi_physical_run_v1 *piece = n00b_abi_v1_record(
                    snapshot, N00B_ABI_TABLE_PHYSICAL_RUN, part_index, NULL);
                if (!piece || !piece->width_bits
                    || piece->byte_offset > UINT64_MAX / 8
                    || piece->byte_offset * 8 + piece->bit_offset != next_bit
                    || __builtin_add_overflow(next_bit, piece->width_bits,
                        &next_bit)) __builtin_trap();
            }
            width = next_bit - first_bit;
        }
        return (n00bb_field_location_t){
            .address = base + (size_t)run->byte_offset,
            .type = child,
            .type_index = field->child_type_index,
            .bit_offset = run->bit_offset,
            .width = width,
        };
    }
    __builtin_trap();
}

static void field_ref_box(const n00b_abi_box_v1 *box,
                          const char *name, uint32_t ordinal,
                          n00bb_box_allocator_t allocate,
                          n00b_abi_box_v1 *result)
{
    if (!allocate || !result) __builtin_trap();
    n00bb_field_location_t field = field_location(box, name, ordinal);
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    uint32_t reference_index = n00bb_bootstrap_reference_type(field.type_index);
    const n00b_abi_type_v1 *reference = n00b_abi_v1_type(snapshot,
                                                         reference_index);
    if (!reference) __builtin_trap();
    *result = (n00b_abi_box_v1){.type_field = reference_index};
    if (reference->size_bits == 64) {
        if (field.bit_offset || field.width != field.type->size_bits)
            __builtin_trap();
        result->payload.inline_bits = (uintptr_t)field.address;
        return;
    }
    if (reference->size_bits != 128 || !field.type->semantic_width_bits
        || field.width != field.type->semantic_width_bits
        || field.bit_offset > 7
        || field.width > UINT16_MAX - field.bit_offset)
        __builtin_trap();
    n00b_abi_bitptr_v1 *bitptr = allocate(N00B_ABI_ALLOCATOR_GC,
        reference_index, sizeof(*bitptr), 8, NULL);
    if (!bitptr) __builtin_trap();
    *bitptr = (n00b_abi_bitptr_v1){
        .base = (uint8_t *)field.address,
        .type_index = field.type_index,
        .start_bit = (uint16_t)field.bit_offset,
        .end_bit = (uint16_t)(field.bit_offset + field.width),
    };
    result->payload.out_of_line = bitptr;
}

static void field_read_box(const n00b_abi_box_v1 *box,
                           const char *name, uint32_t ordinal,
                           n00b_abi_box_v1 *result)
{
    if (!result) __builtin_trap();
    n00bb_field_location_t field = field_location(box, name, ordinal);
    if (!field.bit_offset && field.width == field.type->size_bits
        && !(field.width & 7)) {
        n00bb_bootstrap_load_box(field.address, field.type_index, result);
        return;
    }
    uint64_t width = field.width;
    if (!width || (width != field.type->size_bits
            && width != field.type->semantic_width_bits)
        || field.bit_offset > 7 || width > UINT64_MAX - field.bit_offset
        || field.type->size_bits > SIZE_MAX - 7
        || !field.type->alignment_bytes) __builtin_trap();
    size_t bytes = (size_t)((field.type->size_bits + 7) / 8);
    *result = (n00b_abi_box_v1){.type_field = field.type_index};
    uint8_t *target = bytes <= sizeof(uint64_t)
        ? (uint8_t *)&result->payload.inline_bits
        : n00b_allocate(N00B_ABI_ALLOCATOR_GC, field.type_index, bytes,
                        field.type->alignment_bytes, NULL);
    if (!target) __builtin_trap();
    for (size_t i = 0; i < bytes; i++) target[i] = 0;
    for (uint64_t bit = 0; bit < width; bit++) {
        uint64_t source_bit = field.bit_offset + bit;
        target[bit / 8] |= ((field.address[source_bit / 8]
            >> (source_bit & 7)) & 1u) << (bit & 7);
    }
    if (bytes > sizeof(uint64_t)) result->payload.out_of_line = target;
}

void n00bb_bootstrap_field_ref_box(const n00b_abi_box_v1 *box,
                                   const char *name,
                                   n00bb_box_allocator_t allocate,
                                   n00b_abi_box_v1 *result)
{
    field_ref_box(box, name, UINT32_MAX, allocate, result);
}

void n00bb_bootstrap_field_ref_box_index(const n00b_abi_box_v1 *box,
                                         uint32_t ordinal,
                                         n00bb_box_allocator_t allocate,
                                         n00b_abi_box_v1 *result)
{
    field_ref_box(box, NULL, ordinal, allocate, result);
}

void n00bb_bootstrap_field_read_box(const n00b_abi_box_v1 *box,
                                    const char *name,
                                    n00bb_box_allocator_t allocate,
                                    n00b_abi_box_v1 *result)
{
    if (!allocate) __builtin_trap();
    field_read_box(box, name, UINT32_MAX, result);
}

void n00bb_bootstrap_field_read_box_index(const n00b_abi_box_v1 *box,
                                          uint32_t ordinal,
                                          n00bb_box_allocator_t allocate,
                                          n00b_abi_box_v1 *result)
{
    if (!allocate) __builtin_trap();
    field_read_box(box, NULL, ordinal, result);
}
