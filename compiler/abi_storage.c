#include "abi_storage.h"
#include "abi_string.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "ir/layout.h"
#include "ir/typelang.h"
#include "ir/shape.h"
#include "lib/alloc.h"
#include "lib/byteorder.h"
#include "lib/dict.h"
#include "lib/list.h"

core_list_decl(n00b_abi_storage_v1);
core_list_decl(n00b_abi_baked_node_v1);
core_list_decl(n00bb_abi_storage_relocation_t);

typedef struct {
    size_t source;
    size_t source_offset;
    size_t target;
    size_t target_offset;
    size_t next;
    bool duplicate;
} graph_edge_t;

static int
graph_edge_order(const void *left, const void *right)
{
    const graph_edge_t *a = left, *b = right;
    if (a->source != b->source) return a->source < b->source ? -1 : 1;
    if (a->source_offset != b->source_offset)
        return a->source_offset < b->source_offset ? -1 : 1;
    if (a->target != b->target) return a->target < b->target ? -1 : 1;
    if (a->target_offset != b->target_offset)
        return a->target_offset < b->target_offset ? -1 : 1;
    return 0;
}

typedef struct {
    uint8_t reachability;
    bool root;
    bool placed;
    uint32_t kind;
    uint64_t offset;
} graph_place_t;

static core_buffer_t *
graph_segment(n00bb_abi_storage_rows_t *rows, uint32_t kind,
              uint32_t **alignment)
{
    if (kind == N00B_ABI_STORAGE_READ_ONLY) {
        *alignment = &rows->read_only_alignment;
        return &rows->read_only;
    }
    if (kind == N00B_ABI_STORAGE_WRITABLE) {
        *alignment = &rows->writable_alignment;
        return &rows->writable;
    }
    return nullptr;
}
core_list_decl(n00bb_abi_storage_index_patch_t);

static bool
append_padding(core_buffer_t *buffer, size_t alignment)
{
    if (!alignment || (alignment & (alignment - 1))
        || buffer->byte_len > SIZE_MAX - (alignment - 1)) return false;
    size_t aligned = (buffer->byte_len + alignment - 1) & ~(alignment - 1);
    static const char zero[16] = {0};
    while (buffer->byte_len < aligned) {
        size_t count = aligned - buffer->byte_len;
        if (count > sizeof zero) count = sizeof zero;
        core_buffer_append(buffer, zero, count);
    }
    return true;
}

static uint64_t
storage_bits(ir_module_t *runtime, th_type *type, const n00b_abi_type_v1 *row)
{
    const ir_final_layout_t *layout = type
        ? ir_layout_final_visible(runtime, type, ir_module_target(runtime))
        : nullptr;
    return layout ? layout->intrinsic_bits : row->size_bits;
}

static uint32_t
storage_alignment(ir_module_t *runtime, th_type *type,
                  const n00b_abi_type_v1 *row)
{
    const ir_final_layout_t *layout = type
        ? ir_layout_final_visible(runtime, type, ir_module_target(runtime))
        : nullptr;
    return layout ? layout->alignment_bytes : row->alignment_bytes;
}

static bool
encode_scalar(ir_module_t *module, n00bb_abi_type_index_t *index,
              th_type *type,
              const ir_global_init_value_t *value,
              uint64_t size_bits, uint8_t bytes[16],
              uint32_t *descriptor_index)
{
    if (th_builtin_applied(type, TH_BUILTIN_TYPESPEC, 0)) {
        if (size_bits != 32) return false;
        if (!value) return true;
        uint32_t local_index = 0;
        if (value->kind != IR_GLOBAL_INIT_TYPE || !index
            || !value->descriptor_type
            || !n00bb_abi_type_index_find(index,
                value->descriptor_type, &local_index)
            || local_index > INT32_MAX) return false;
        core_le_store_u32(bytes, local_index);
        if (descriptor_index) *descriptor_index = local_index;
        return true;
    }
    if (th_builtin_applied(type, TH_BUILTIN_ATOMIC, 1)) {
        const th_arg *item = th_named_arg(type, 0);
        if (!item || item->kind != TH_ARG_TYPE) return false;
        type = item->type;
    }
    ir_machine_t machine = ir_machine_of(module, type);
    th_type *definition = machine.class == IR_CLASS_OPAQUE
        && th_kind_of(th_resolve(type)) == TH_NAMED
        ? ir_type_definition(module, type) : nullptr;
    if (definition && th_resolve(definition) != th_resolve(type))
        machine = ir_machine_of(module, definition);
    if (!size_bits || size_bits > 128 || size_bits % 8
        || !machine.width || machine.width > size_bits
        || machine.width > 128) return false;
    if (!value) return machine.class == IR_CLASS_BOOL
        || machine.class == IR_CLASS_SINT || machine.class == IR_CLASS_UINT
        || machine.class == IR_CLASS_FLOAT;
    switch (machine.class) {
    case IR_CLASS_BOOL:
        if (value->kind != IR_GLOBAL_INIT_BOOL || size_bits != 8) return false;
        bytes[0] = value->boolean ? 1 : 0;
        return true;
    case IR_CLASS_SINT:
    case IR_CLASS_UINT: {
        if (value->kind != IR_GLOBAL_INIT_INT) return false;
        ir_uint_t magnitude = value->integer.mag;
        bool negative = value->integer.neg;
        if (machine.class == IR_CLASS_UINT && negative) return false;
        if (machine.class == IR_CLASS_SINT) {
            ir_uint_t limit = (ir_uint_t)1 << (machine.width - 1);
            if (negative ? magnitude > limit : magnitude >= limit)
                return false;
        }
        else if (machine.width < 128 && magnitude >> machine.width)
            return false;
        ir_uint_t bits = negative ? (~magnitude + 1) : magnitude;
        for (size_t i = 0; i < size_bits / 8; i++) {
            bytes[i] = (uint8_t)bits;
            bits >>= 8;
        }
        return true;
    }
    case IR_CLASS_FLOAT:
        if (value->kind != IR_GLOBAL_INIT_FLOAT
            || size_bits != machine.width) return false;
        if (size_bits == 32) {
            float number = (float)value->floating;
            uint32_t bits;
            memcpy(&bits, &number, sizeof bits);
            core_le_store_u32(bytes, bits);
            return true;
        }
        if (size_bits == 64) {
            uint64_t bits;
            memcpy(&bits, &value->floating, sizeof bits);
            core_le_store_u64(bytes, bits);
            return true;
        }
        return false;
    default:
        return false;
    }
}

static bool
encode_string(n00bb_abi_name_pool_t *pool,
              const ir_global_init_value_t *value,
              uint8_t bytes[16], size_t *pool_offset)
{
    if (!value) return true;
    if (value->kind != IR_GLOBAL_INIT_STRING
        || (value->string.u8_bytes && !value->string.data)) return false;
    n00b_abi_string_encoding_v1 encoding = N00B_ABI_STRING_ASCII;
    for (size_t i = 0; i < value->string.u8_bytes; i++)
        if ((uint8_t)value->string.data[i] >= 0x80) {
            encoding = N00B_ABI_STRING_UTF8;
            break;
        }
    n00b_abi_string_v1 encoded = {0};
    if (!n00bb_abi_name_pool_value(pool, value->string,
        encoding, &encoded, pool_offset)) return false;
    memcpy(bytes, &encoded, sizeof encoded);
    return true;
}

static th_type *
array_element_type(th_type *type)
{
    if (th_kind_of(type) == TH_SEQUENCE) return th_seq_elem(type);
    if (!th_builtin_applied(type, TH_BUILTIN_ARRAY, 1)) return nullptr;
    const th_arg *argument = th_named_arg(type, 0);
    return argument && argument->kind == TH_ARG_TYPE ? argument->type : nullptr;
}

typedef struct {
    uint32_t rank;
    uint32_t flat_rank;
    uint64_t total_count;
    uint64_t *extents;
    bool *seen;
    bool *uniform;
} array_shape_facts_t;

typedef struct {
    uint32_t source_kind;
    uint64_t byte_offset;
    core_string_t global;
} pending_address_t;

core_list_decl(pending_address_t);

typedef struct {
    n00bb_abi_type_index_t *index;
    const n00bb_abi_type_identity_rows_t *types;
    ir_module_t *runtime;
    n00bb_abi_name_pool_t *pool;
    n00bb_abi_storage_rows_t *built;
    core_list_t(n00bb_abi_storage_relocation_t) *relocations;
    core_list_t(n00bb_abi_storage_relocation_t) *tls_fixups;
    core_list_t(n00bb_abi_storage_index_patch_t) *index_patches;
    core_list_t(pending_address_t) *addresses;
} array_writer_t;

static bool
global_address(const ir_global_init_value_t *value)
{
    return value && value->kind == IR_GLOBAL_INIT_SYMBOL && value->symbol
        && value->symbol->kind == IR_VAL_GLOBAL && value->symbol->name.data;
}

static bool
encode_global_address(array_writer_t *writer,
                      const ir_global_init_value_t *value, uint64_t bits,
                      uint32_t source_kind, uint64_t source_bit_offset)
{
    if (bits != 64 || source_bit_offset % 8) return false;
    core_list_push(*writer->addresses, ((pending_address_t){
        .source_kind = source_kind,
        .byte_offset = source_bit_offset / 8,
        .global = value->symbol->name,
    }));
    return true;
}

static core_buffer_t *
data_segment(array_writer_t *writer, uint32_t data_kind)
{
    return data_kind == N00B_ABI_STORAGE_TLS ? &writer->built->tls
                                             : &writer->built->aggregates;
}

static bool
record_type_index(array_writer_t *writer, uint32_t source_kind,
                  uint64_t byte_offset, uint32_t type_index)
{
    if (!writer->index_patches || byte_offset % sizeof(uint32_t)
        || type_index > INT32_MAX
        || writer->index_patches->len >= UINT32_MAX) return false;
    core_list_push(*writer->index_patches,
        ((n00bb_abi_storage_index_patch_t){
            .source_kind = source_kind,
            .byte_offset = byte_offset,
            .table_kind = N00B_ABI_TABLE_TYPE,
            .patch_kind = N00B_ABI_PATCH_INDEX,
            .local_index = type_index,
        }));
    return true;
}

static bool
scan_array_shape(const ir_global_init_value_t *value, th_type *type,
                 uint32_t depth, array_shape_facts_t *facts)
{
    th_type *element = array_element_type(type);
    if (!element || !value || value->kind != IR_GLOBAL_INIT_SEQUENCE
        || (value->aggregate.count && !value->aggregate.items)) return false;
    uint64_t count = value->aggregate.count;
    if (!facts->seen[depth]) {
        facts->seen[depth] = true;
        facts->uniform[depth] = true;
        facts->extents[depth] = count;
    }
    else if (facts->extents[depth] != count) facts->uniform[depth] = false;
    if (depth + 1 < facts->rank)
        for (size_t i = 0; i < value->aggregate.count; i++)
            if (!scan_array_shape(&value->aggregate.items[i], element,
                    depth + 1, facts)) return false;
    return true;
}

static bool
array_shape_matches(const ir_shape_t *declared,
                    const array_shape_facts_t *facts)
{
    if (!declared) return true;
    if (ir_shape_rank(declared) != facts->rank) return false;
    for (uint32_t i = 0; i < facts->rank; i++) {
        ir_shape_extent_t extent;
        if (!ir_shape_extent(declared, i, &extent)
            || (extent.kind == IR_SHAPE_EXTENT_STATIC
                && (i >= facts->flat_rank
                    || extent.static_value != facts->extents[i])))
            return false;
    }
    return true;
}

static bool
array_shape_build(th_type *type, const ir_global_init_value_t *value,
                  const ir_shape_t *declared, array_shape_facts_t *facts)
{
    th_type *element = type;
    while ((element = array_element_type(element))) {
        if (facts->rank == UINT32_MAX) return false;
        facts->rank++;
    }
    if (!facts->rank) return false;
    facts->extents = core_alloc_array(uint64_t, facts->rank);
    facts->seen = core_alloc_array(bool, facts->rank);
    facts->uniform = core_alloc_array(bool, facts->rank);
    if (!scan_array_shape(value, type, 0, facts)) return false;
    facts->total_count = 1;
    for (uint32_t i = 0; i < facts->rank; i++) {
        if (!facts->seen[i] || !facts->uniform[i]) break;
        uint64_t extent = facts->extents[i];
        if (extent && facts->total_count > UINT64_MAX / extent) return false;
        facts->total_count *= extent;
        facts->flat_rank++;
    }
    for (uint32_t i = facts->flat_rank; i < facts->rank; i++)
        facts->extents[i] = 0;
    return facts->flat_rank && array_shape_matches(declared, facts);
}

static void
array_shape_release(array_shape_facts_t *facts)
{
    core_free(facts->extents);
    core_free(facts->seen);
    core_free(facts->uniform);
}

static bool
encode_array(array_writer_t *writer, th_type *type,
             const ir_global_init_value_t *value,
             const ir_shape_t *declared_shape,
             uint32_t source_kind, uint64_t source_offset,
             uint32_t *shape_index);

static bool
encode_aggregate(array_writer_t *writer, th_type *type,
                 const ir_global_init_value_t *value,
                 uint32_t source_kind, uint64_t source_bit_offset,
                 uint8_t *bytes, size_t byte_count, size_t depth);

static bool
encode_inline_array_member(array_writer_t *writer,
                           const ir_final_member_layout_t *member,
                           const ir_global_init_value_t *item,
                           uint32_t source_kind, uint64_t source_bit_offset,
                           uint8_t *bytes, size_t depth)
{
    uint32_t element_index;
    th_type *element = member->inline_element_type;
    if (!element || item->kind != IR_GLOBAL_INIT_SEQUENCE
        || item->aggregate.count != member->inline_element_count
        || (item->aggregate.count && !item->aggregate.items)
        || !n00bb_abi_type_index_find(writer->index, element, &element_index)
        || element_index >= writer->types->count) return false;
    const n00b_abi_type_v1 *element_type = &writer->types->rows[element_index];
    uint64_t element_bits = storage_bits(writer->runtime, element,
                                         element_type);
    uint64_t stride = member->inline_element_stride_bits;
    if (element_bits > stride || element_bits > SIZE_MAX - 7
        || element_type->kind == N00B_ABI_TYPE_ARRAY
        || element_type->kind == N00B_ABI_TYPE_STRING) return false;
    size_t element_bytes = (size_t)((element_bits + 7) / 8);
    size_t buffer_bytes = element_bytes < 16 ? 16 : element_bytes;
    uint8_t *buffer = core_alloc_array(uint8_t, buffer_bytes);
    bool aggregate = element_type->kind == N00B_ABI_TYPE_RECORD
        || element_type->kind == N00B_ABI_TYPE_VARIANT
        || element_type->kind == N00B_ABI_TYPE_TUPLE;
    bool valid = true;
    for (size_t i = 0; valid && i < item->aggregate.count; i++) {
        const ir_global_init_value_t *value = &item->aggregate.items[i];
        uint64_t at = member->bit_offset + stride * i;
        uint32_t descriptor_index = UINT32_MAX;
        memset(buffer, 0, buffer_bytes);
        valid = aggregate
            ? encode_aggregate(writer, element, value, source_kind,
                source_bit_offset + at, buffer, element_bytes, depth + 1)
            : global_address(value)
            ? encode_global_address(writer, value, element_bits, source_kind,
                source_bit_offset + at)
            : encode_scalar(writer->runtime, writer->index, element, value,
                element_bits, buffer, &descriptor_index);
        if (valid && descriptor_index != UINT32_MAX)
            valid = (source_bit_offset + at) % 8 == 0
                && record_type_index(writer, source_kind,
                    (source_bit_offset + at) / 8, descriptor_index);
        for (uint64_t bit = 0; valid && bit < element_bits; bit++)
            if (buffer[bit / 8] & (1u << (bit % 8)))
                bytes[(at + bit) / 8] |= (uint8_t)(1u << ((at + bit) % 8));
    }
    core_free(buffer);
    return valid;
}

static bool
encode_typed_value(array_writer_t *writer, th_type *type,
                   const n00b_abi_type_v1 *row,
                   const ir_global_init_value_t *item,
                   uint32_t source_kind, uint64_t source_bit_offset,
                   uint8_t *bytes, size_t byte_count, size_t depth);

static bool
encode_option_payload(array_writer_t *writer, th_type *type,
                      const ir_global_init_value_t *item,
                      uint32_t source_kind, uint64_t source_bit_offset,
                      uint8_t *bytes, size_t byte_count, size_t depth)
{
    th_type *option = th_builtin_applied(th_resolve(type), TH_BUILTIN_OPTION,
                                         1);
    const th_arg *argument = option ? th_named_arg(option, 0) : nullptr;
    const ir_final_layout_t *layout = ir_layout_final_visible(
        writer->runtime, type, ir_module_target(writer->runtime));
    uint32_t payload_index;
    if (!argument || argument->kind != TH_ARG_TYPE || !layout
        || !layout->has_discriminant || depth > 64
        || !n00bb_abi_type_index_find(writer->index, argument->type,
                                      &payload_index)
        || payload_index >= writer->types->count) return false;
    const n00b_abi_type_v1 *payload_row = &writer->types->rows[payload_index];
    if (storage_bits(writer->runtime, argument->type, payload_row)
            > (uint64_t)byte_count * 8
        || !encode_typed_value(writer, argument->type, payload_row, item,
               source_kind, source_bit_offset, bytes, byte_count, depth + 1))
        return false;
    if (layout->discriminant_is_niche) return true;
    const ir_layout_access_t *presence = &layout->discriminant;
    uint64_t bit = presence->unit_bit_offset + presence->lsb;
    if (presence->value_width_bits != 1 || bit >= (uint64_t)byte_count * 8)
        return false;
    bytes[bit / 8] |= (uint8_t)(1u << (bit % 8));
    return true;
}

static bool
encode_typed_value(array_writer_t *writer, th_type *type,
                   const n00b_abi_type_v1 *row,
                   const ir_global_init_value_t *item,
                   uint32_t source_kind, uint64_t source_bit_offset,
                   uint8_t *bytes, size_t byte_count, size_t depth)
{
    uint64_t bits = storage_bits(writer->runtime, type, row);
    size_t pool_offset = 0;
    uint32_t descriptor_index = UINT32_MAX;
    bool valid;
    if (row->kind == N00B_ABI_TYPE_RECORD
        || row->kind == N00B_ABI_TYPE_VARIANT
        || row->kind == N00B_ABI_TYPE_TUPLE)
        valid = encode_aggregate(writer, type, item, source_kind,
            source_bit_offset, bytes, byte_count, depth + 1);
    else if (row->kind == N00B_ABI_TYPE_ARRAY)
        valid = source_bit_offset % 8 == 0
            && encode_array(writer, type, item, nullptr,
                source_kind, source_bit_offset / 8, nullptr);
    else if (row->kind == N00B_ABI_TYPE_STRING) {
        valid = bits == sizeof(n00b_abi_string_v1) * 8
            && byte_count >= sizeof(n00b_abi_string_v1)
            && encode_string(writer->pool, item, bytes, &pool_offset);
        if (valid && item->string.u8_bytes
            > N00BB_ABI_STRING_SHORT_CAPACITY) {
            valid = source_bit_offset % 8 == 0;
            if (valid)
                core_list_push(*writer->relocations,
                    ((n00bb_abi_storage_relocation_t){
                        .source_kind = source_kind,
                        .byte_offset = source_bit_offset / 8,
                        .target_kind = N00BB_ABI_RELOC_STRING_POOL,
                        .target_offset = pool_offset,
                    }));
        }
    }
    else if (item->kind == IR_GLOBAL_INIT_NIL)
        valid = true;
    else if (global_address(item))
        valid = encode_global_address(writer, item, bits, source_kind,
                                      source_bit_offset);
    else if (row->kind == N00B_ABI_TYPE_OPTION)
        valid = encode_option_payload(writer, type, item, source_kind,
            source_bit_offset, bytes, byte_count, depth);
    else valid = bits <= (uint64_t)byte_count * 8
        && encode_scalar(writer->runtime, writer->index, type, item, bits,
                         bytes, &descriptor_index);
    if (valid && descriptor_index != UINT32_MAX)
        valid = source_bit_offset % 8 == 0
            && record_type_index(writer, source_kind, source_bit_offset / 8,
                                 descriptor_index);
    return valid;
}

static bool
encode_aggregate_member(array_writer_t *writer,
                        const ir_final_layout_t *layout,
                        const ir_final_member_layout_t *member,
                        const ir_global_init_value_t *item,
                        uint32_t source_kind, uint64_t source_bit_offset,
                        uint8_t *bytes, size_t depth)
{
    if (member->inline_element_count)
        return encode_inline_array_member(writer, member, item, source_kind,
                                          source_bit_offset, bytes, depth);
    uint32_t child_index;
    if (!member->type || !n00bb_abi_type_index_find(writer->index,
            member->type, &child_index)
        || child_index >= writer->types->count
        || member->bit_offset > UINT64_MAX - source_bit_offset)
        return false;
    const n00b_abi_type_v1 *child_type = &writer->types->rows[child_index];
    uint64_t child_bits = storage_bits(writer->runtime, member->type,
                                       child_type);
    if (child_bits > SIZE_MAX - 7) return false;
    size_t child_bytes = (size_t)((child_bits + 7) / 8);
    size_t buffer_bytes = child_bytes < 16 ? 16 : child_bytes;
    uint8_t *child = core_alloc_array(uint8_t, buffer_bytes);
    memset(child, 0, buffer_bytes);
    uint64_t child_source = source_bit_offset + member->bit_offset;
    bool valid = encode_typed_value(writer, member->type, child_type, item,
        source_kind, child_source, child, child_bytes, depth);
    for (size_t r = 0; valid && r < member->n_occupied; r++) {
        ir_layout_bit_run_t run = member->occupied[r];
        if (run.offset < member->bit_offset
            || run.offset > layout->intrinsic_bits
            || run.width > layout->intrinsic_bits - run.offset
            || run.offset - member->bit_offset > child_bits
            || run.width > child_bits - (run.offset - member->bit_offset)) {
            valid = false;
            break;
        }
        for (uint64_t bit = 0; bit < run.width; bit++) {
            uint64_t from = run.offset - member->bit_offset + bit;
            uint64_t to = run.offset + bit;
            if (child[from / 8] & (1u << (from % 8)))
                bytes[to / 8] |= (uint8_t)(1u << (to % 8));
        }
    }
    core_free(child);
    return valid;
}

static bool
encode_aggregate(array_writer_t *writer, th_type *type,
                 const ir_global_init_value_t *value,
                 uint32_t source_kind, uint64_t source_bit_offset,
                 uint8_t *bytes, size_t byte_count, size_t depth)
{
    const ir_final_layout_t *layout = ir_layout_final_visible(writer->runtime,
        type, ir_module_target(writer->runtime));
    if (!layout || !value || depth > 64 || byte_count > SIZE_MAX / 8
        || layout->intrinsic_bits > byte_count * 8
        || (layout->n_members && !layout->members))
        return false;
    bool variant = layout->has_discriminant;
    bool tuple = th_builtin_kind_of(type) == TH_BUILTIN_TUPLE;
    if (variant ? value->kind != IR_GLOBAL_INIT_VARIANT
                || value->aggregate.count != 1 || !value->aggregate.items
                : value->kind != (tuple ? IR_GLOBAL_INIT_TUPLE
                                        : IR_GLOBAL_INIT_RECORD)
                || value->aggregate.count != layout->n_members
                || (layout->n_members && !value->aggregate.items))
        return false;
    bool *seen = variant ? nullptr : core_alloc_array(bool, layout->n_members);
    bool valid = true;
    for (size_t i = 0; valid && i < value->aggregate.count; i++) {
        const ir_global_init_value_t *item = &value->aggregate.items[i];
        size_t slot = tuple ? i : 0;
        while (!tuple && slot < layout->n_members
            && (!item->member.data || !layout->members[slot].name.data
                || !core_string_eq(item->member, layout->members[slot].name)))
            slot++;
        if (slot == layout->n_members || (!variant && seen[slot])
            || (tuple && (item->member.data
                || layout->members[slot].ordinal != i))) {
            valid = false;
            break;
        }
        if (!variant) seen[slot] = true;
        const ir_final_member_layout_t *member = &layout->members[slot];
        valid = encode_aggregate_member(writer, layout, member, item,
            source_kind, source_bit_offset, bytes, depth);
        if (valid && variant) {
            const ir_layout_access_t *tag = &layout->discriminant;
            uint64_t bit_offset = tag->unit_bit_offset + tag->lsb;
            uint64_t value_bits = (uint64_t)member->ordinal + 1;
            valid = !layout->discriminant_is_niche
                && tag->value_width_bits && tag->value_width_bits <= 32
                && tag->unit_bit_offset <= UINT64_MAX - tag->lsb
                && bit_offset <= layout->intrinsic_bits
                && tag->value_width_bits
                    <= layout->intrinsic_bits - bit_offset
                && value_bits < (UINT64_C(1) << tag->value_width_bits);
            for (uint32_t bit = 0; valid && bit < tag->value_width_bits;
                 bit++)
                if (value_bits & (UINT64_C(1) << bit))
                    bytes[(bit_offset + bit) / 8] |=
                        (uint8_t)(1u << ((bit_offset + bit) % 8));
        }
    }
    for (size_t i = 0; valid && i < layout->n_members; i++)
        if (!variant && !seen[i]) valid = false;
    core_free(seen);
    return valid;
}

static bool
encode_array_items(array_writer_t *writer,
                   const array_shape_facts_t *shape,
                   const ir_global_init_value_t *value, th_type *type,
                   uint32_t depth, size_t data_offset, size_t stride,
                   size_t width, bool packed, uint32_t data_kind,
                   size_t *next)
{
    if (depth < shape->flat_rank) {
        th_type *element = array_element_type(type);
        if (!element || !value || value->kind != IR_GLOBAL_INIT_SEQUENCE
            || value->aggregate.count != shape->extents[depth]) return false;
        for (size_t i = 0; i < value->aggregate.count; i++)
            if (!encode_array_items(writer, shape,
                    &value->aggregate.items[i], element, depth + 1,
                    data_offset, stride, width, packed, data_kind, next))
                return false;
        return true;
    }
    size_t at = packed ? 0 : data_offset + (*next) * stride;
    if (depth < shape->rank) {
        if (!encode_array(writer, type, value, nullptr, data_kind, at,
                          nullptr)) return false;
    }
    else {
        uint32_t type_index;
        if (!n00bb_abi_type_index_find(writer->index, type, &type_index)
            || type_index >= writer->types->count) return false;
        const n00b_abi_type_v1 *row = &writer->types->rows[type_index];
        uint8_t inline_bytes[16] = {0};
        bool zero_option = row->kind == N00B_ABI_TYPE_OPTION
            && value->kind == IR_GLOBAL_INIT_NIL;
        bool aggregate = row->kind == N00B_ABI_TYPE_RECORD
            || row->kind == N00B_ABI_TYPE_VARIANT
            || row->kind == N00B_ABI_TYPE_TUPLE
            || (row->kind == N00B_ABI_TYPE_OPTION && !zero_option);
        bool heap_bytes = aggregate || (zero_option && stride > sizeof inline_bytes);
        if (aggregate && at > UINT64_MAX / 8) return false;
        uint8_t *bytes = heap_bytes ? core_alloc_array(uint8_t, stride)
                                   : inline_bytes;
        if (heap_bytes) memset(bytes, 0, stride);
        size_t pool_offset = 0;
        uint32_t descriptor_index = UINT32_MAX;
        bool string = row->kind == N00B_ABI_TYPE_STRING;
        bool valid = string ? (stride == 16
                && encode_string(writer->pool, value, bytes, &pool_offset))
            : aggregate ? encode_typed_value(writer, type, row, value,
                data_kind, (uint64_t)at * 8, bytes, stride, 0)
            : zero_option ? true
            : row->kind == N00B_ABI_TYPE_NIL && !row->size_bits
                ? value->kind == IR_GLOBAL_INIT_NIL
            : global_address(value)
                ? !packed && encode_global_address(writer, value,
                    storage_bits(writer->runtime, type, row), data_kind,
                    (uint64_t)at * 8)
            : encode_scalar(writer->runtime, writer->index, type, value,
                storage_bits(writer->runtime, type, row), bytes,
                &descriptor_index);
        if (!valid) {
            if (heap_bytes) core_free(bytes);
            return false;
        }
        char *data = data_segment(writer, data_kind)->data;
        if (packed) {
            size_t first_bit = (*next) * width;
            for (size_t bit = 0; bit < width; bit++)
                if (bytes[bit / 8] & (1u << (bit % 8)))
                    data[data_offset + (first_bit + bit) / 8]
                        |= (char)(1u << ((first_bit + bit) % 8));
        }
        else {
            memcpy(data + at, bytes, stride);
            if (string && value->string.u8_bytes
                > N00BB_ABI_STRING_SHORT_CAPACITY)
                core_list_push(*writer->relocations,
                    ((n00bb_abi_storage_relocation_t){
                        .source_kind = data_kind,
                        .byte_offset = at,
                        .target_kind = N00BB_ABI_RELOC_STRING_POOL,
                        .target_offset = pool_offset,
                    }));
        }
        if (heap_bytes) core_free(bytes);
        if (descriptor_index != UINT32_MAX
            && (packed || !record_type_index(writer, data_kind, at,
                                             descriptor_index)))
            return false;
    }
    (*next)++;
    return true;
}

static bool
encode_array(array_writer_t *writer, th_type *type,
             const ir_global_init_value_t *value,
             const ir_shape_t *declared_shape,
             uint32_t source_kind, uint64_t source_offset,
             uint32_t *shape_index)
{
    array_shape_facts_t shape = {0};
    bool valid = array_shape_build(type, value, declared_shape, &shape);
    if (!valid) {
        array_shape_release(&shape);
        return false;
    }
    th_type *item_type = type;
    for (uint32_t i = 0; i < shape.flat_rank; i++)
        item_type = array_element_type(item_type);
    uint32_t type_index;
    valid = item_type && n00bb_abi_type_index_find(writer->index,
        item_type, &type_index) && type_index < writer->types->count;
    if (!valid) goto done;
    const n00b_abi_type_v1 *row = &writer->types->rows[type_index];
    size_t alignment = storage_alignment(writer->runtime, item_type, row);
    uint64_t item_bits = storage_bits(writer->runtime, item_type, row);
    size_t extent = (size_t)(item_bits / 8 + (item_bits % 8 != 0));
    size_t stride = alignment && !(alignment & (alignment - 1))
        && extent <= SIZE_MAX - (alignment - 1)
        ? (extent + alignment - 1) & ~(alignment - 1) : 0;
    bool packed = shape.flat_rank == shape.rank
        && row->kind == N00B_ABI_TYPE_PRIMITIVE
        && (ir_machine_of(writer->runtime, item_type).class == IR_CLASS_BOOL
            || (row->flags & N00B_ABI_TYPE_FLAG_BITS));
    size_t width = row->semantic_width_bits;
    if (!alignment || (alignment & (alignment - 1))
        || (!stride && item_bits) || (row->kind != N00B_ABI_TYPE_RECORD
            && row->kind != N00B_ABI_TYPE_VARIANT
            && row->kind != N00B_ABI_TYPE_TUPLE && stride > 16)
        || (packed && (!width || width > 128
            || shape.total_count > (SIZE_MAX - 7) / width))
        || (!packed && (shape.total_count > SIZE_MAX
            || (stride && shape.total_count > SIZE_MAX / stride)))
        || (shape_index && (writer->built->shape_count >= UINT32_MAX
            || shape.rank > UINT32_MAX - writer->built->extent_count))) {
        valid = false;
        goto done;
    }
    size_t data_size = packed ? (shape.total_count * width + 7) / 8
                              : shape.total_count * stride;
    if (!data_size) data_size = 1;
    bool thread = source_kind == N00B_ABI_STORAGE_TLS;
    uint32_t data_kind = thread ? N00B_ABI_STORAGE_TLS
                                : N00BB_ABI_RELOC_AGGREGATE;
    core_buffer_t *buffer = data_segment(writer, data_kind);
    if (!append_padding(buffer, alignment)
        || data_size >= SIZE_MAX - buffer->byte_len) {
        valid = false;
        goto done;
    }
    size_t data_offset = buffer->byte_len;
    core_buffer_ensure(buffer, data_size + 1);
    memset(buffer->data + buffer->byte_len, 0, data_size);
    buffer->byte_len += data_size;
    buffer->data[buffer->byte_len] = 0;
    uint32_t *data_alignment = thread ? &writer->built->tls_alignment
                                      : &writer->built->aggregate_alignment;
    if (*data_alignment < alignment) *data_alignment = (uint32_t)alignment;
    core_buffer_t *descriptors = &writer->built->aggregates;
    if (!append_padding(descriptors, 8)
        || descriptors->byte_len > SIZE_MAX - 25
        || (size_t)shape.rank
            > (SIZE_MAX - descriptors->byte_len - 25) / 8) {
        valid = false;
        goto done;
    }
    size_t descriptor_offset = descriptors->byte_len;
    uint8_t header[24] = {0};
    core_le_store_u64(header, shape.total_count);
    core_le_store_u32(header + 8, shape.rank);
    core_le_store_u32(header + 12, shape.flat_rank);
    core_buffer_append(descriptors, (const char *)header, sizeof header);
    for (uint32_t i = 0; i < shape.rank; i++) {
        uint8_t extent[8];
        core_le_store_u64(extent, shape.extents[i]);
        core_buffer_append(descriptors, (const char *)extent, sizeof extent);
    }
    if (writer->built->aggregate_alignment < 8)
        writer->built->aggregate_alignment = 8;
    size_t next = 0;
    valid = encode_array_items(writer, &shape, value, type, 0,
        data_offset, stride, width, packed, data_kind, &next)
        && next == shape.total_count;
    if (!valid) goto done;
    if (shape_index) {
        n00bb_abi_storage_rows_t *built = writer->built;
        built->shapes = core_realloc(built->shapes,
            (built->shape_count + 1) * sizeof(*built->shapes));
        built->extents = core_realloc(built->extents,
            (built->extent_count + shape.rank) * sizeof(*built->extents));
        *shape_index = (uint32_t)built->shape_count;
        built->shapes[built->shape_count++] = (n00b_abi_shape_v1){
            .total_count = shape.total_count,
            .rank = shape.rank,
            .flat_rank = shape.flat_rank,
            .extents = {
                .first = (uint32_t)built->extent_count,
                .count = shape.rank,
            },
        };
        memcpy(built->extents + built->extent_count, shape.extents,
               shape.rank * sizeof(*built->extents));
        built->extent_count += shape.rank;
    }
    core_list_push(*(thread ? writer->tls_fixups : writer->relocations),
        ((n00bb_abi_storage_relocation_t){
            .source_kind = source_kind, .byte_offset = source_offset,
            .target_kind = data_kind, .target_offset = data_offset,
        }));
    core_list_push(*writer->relocations,
        ((n00bb_abi_storage_relocation_t){
            .source_kind = source_kind, .byte_offset = source_offset + 8,
            .target_kind = N00BB_ABI_RELOC_AGGREGATE,
            .target_offset = descriptor_offset,
        }));
done:
    array_shape_release(&shape);
    return valid;
}

static ir_global_t *
storage_global(parse_sym_entry_t *symbol,
               n00bb_abi_declaration_lookup_fn lookup, void *lookup_context)
{
    ir_global_t *global = parse_sym_binding(symbol,
        CORE_STRING_STATIC("n00bb.generated-literal"));
    if (global) return global;
    core_link_declaration_t *declaration = lookup(lookup_context, symbol);
    return declaration ? core_link_declaration_binding(declaration,
        CORE_STRING_STATIC("n00bb.ir.global")) : nullptr;
}

static bool
graph_type_index(const n00bb_abi_type_identity_rows_t *types,
                 uint64_t typehash, uint32_t *out)
{
    *out = 0;
    if (!typehash) return true;
    for (size_t i = 1; i < types->count; i++)
        if (types->rows[i].typehash == typehash) {
            *out = (uint32_t)i;
            return true;
        }
    return false;
}

static bool
graph_placement_plan(const n00bb_abi_graph_source_t *graphs,
    const n00bb_abi_decl_index_t *declarations, ir_module_t *runtime,
    n00bb_abi_declaration_lookup_fn lookup, void *lookup_context,
    graph_place_t **out_places, graph_edge_t **out_edges)
{
    *out_places = nullptr;
    *out_edges = nullptr;
    if (!graphs || !graphs->node_count) return true;
    if (!graphs->root || !graphs->node || !graphs->edge || !lookup
        || graphs->node_count > SIZE_MAX / (2 * sizeof(size_t)))
        return false;
    graph_place_t *places = core_alloc_array(graph_place_t,
                                             graphs->node_count);
    graph_edge_t *edges = core_alloc_array(graph_edge_t,
        graphs->edge_count ? graphs->edge_count : 1);
    size_t *heads = core_alloc_array(size_t, graphs->node_count);
    size_t *queue = core_alloc_array(size_t, graphs->node_count * 2);
    bool valid = places && edges && heads && queue;
    if (valid) {
        memset(places, 0, graphs->node_count * sizeof(*places));
        memset(edges, 0, graphs->edge_count * sizeof(*edges));
        for (size_t i = 0; i < graphs->node_count; i++) heads[i] = SIZE_MAX;
    }
    for (size_t i = 0; valid && i < graphs->edge_count; i++) {
        graph_edge_t *edge = &edges[i];
        valid = graphs->edge(graphs->context, i, &edge->source,
            &edge->source_offset, &edge->target, &edge->target_offset)
            && edge->source < graphs->node_count
            && edge->target < graphs->node_count;
    }
    if (valid) qsort(edges, graphs->edge_count, sizeof(*edges),
                     graph_edge_order);
    for (size_t i = 0; valid && i < graphs->edge_count; i++) {
        graph_edge_t *edge = &edges[i];
        if (i && edge->source == edges[i - 1].source
            && edge->source_offset == edges[i - 1].source_offset) {
            valid = edge->target == edges[i - 1].target
                && edge->target_offset == edges[i - 1].target_offset;
            edge->duplicate = true;
        }
        if (valid && !edge->duplicate) {
            edge->next = heads[edge->source];
            heads[edge->source] = i;
        }
    }
    size_t queued = 0, read = 0;
    for (size_t i = 1; valid && i < n00bb_abi_decl_symbol_count(declarations);
         i++) {
        parse_sym_entry_t *symbol = n00bb_abi_decl_symbol_at(declarations,
            (uint32_t)i);
        if (!symbol || symbol->kind != PARSE_SYM_VARIABLE) continue;
        ir_global_t *global = storage_global(symbol, lookup, lookup_context);
        if (!global || global->owner != runtime || global->is_extern
            || !ir_global_can_read(global, IR_PHASE_RUNTIME)
            || !global->has_static_initializer
            || global->static_initializer->kind != IR_GLOBAL_INIT_PHASE_RESULT)
            continue;
        size_t root;
        if (!graphs->root(graphs->context,
                global->static_initializer->phase_result.identifier, &root))
            continue;
        if (root >= graphs->node_count) { valid = false; break; }
        uint8_t state = global->mutability == IR_MUT_CONST ? 1 : 3;
        places[root].root = true;
        if ((places[root].reachability | state)
            != places[root].reachability) {
            places[root].reachability |= state;
            queue[queued++] = root;
        }
    }
    while (valid && read < queued) {
        size_t source = queue[read++];
        for (size_t i = heads[source]; i != SIZE_MAX; i = edges[i].next) {
            size_t target = edges[i].target;
            uint8_t state = places[source].reachability;
            if ((places[target].reachability | state)
                == places[target].reachability) continue;
            places[target].reachability |= state;
            if (queued == graphs->node_count * 2) {
                valid = false;
                break;
            }
            queue[queued++] = target;
        }
    }
    core_free(queue);
    core_free(heads);
    if (!valid) {
        core_free(places);
        core_free(edges);
        return false;
    }
    *out_places = places;
    *out_edges = edges;
    return true;
}

static bool
graph_type_add(n00bb_abi_type_index_t *index, ir_module_t *runtime,
               core_string_t name, uint64_t typehash)
{
    if (!name.data) return !typehash;
    th_type *type = ir_type_parse(runtime, name);
    return type && th_hash(ir_module_types(runtime), type) == typehash
        && n00bb_abi_type_index_add(index, type, nullptr);
}

bool
n00bb_abi_graph_types_collect(
    n00bb_abi_type_index_t *index,
    const n00bb_abi_decl_index_t *declarations, ir_module_t *runtime,
    n00bb_abi_declaration_lookup_fn lookup, void *lookup_context,
    const n00bb_abi_graph_source_t *graphs)
{
    graph_place_t *places = nullptr;
    graph_edge_t *edges = nullptr;
    if (!graph_placement_plan(graphs, declarations, runtime, lookup,
            lookup_context, &places, &edges)) return false;
    bool valid = true;
    for (size_t i = 0; valid && places && i < graphs->node_count; i++) {
        if (!(places[i].reachability & 1)) continue;
        const uint8_t *bytes = nullptr;
        size_t size = 0;
        uint32_t alignment = 0;
        uint64_t typehash = 0;
        valid = graphs->node(graphs->context, i, &bytes, &size, &alignment,
                &typehash)
            && graph_type_add(index, runtime,
                graphs->type_name(graphs->context, i), typehash);
    }
    for (size_t i = 0; valid && places && i < graphs->slot_count; i++) {
        n00bb_abi_graph_slot_t slot;
        valid = graphs->slot(graphs->context, i, &slot)
            && slot.node < graphs->node_count;
        if (!valid || !(places[slot.node].reachability & 1)) continue;
        if (slot.table == N00B_ABI_TABLE_TYPE)
            valid = graph_type_add(index, runtime, slot.type_name,
                                   slot.typehash);
        else if (slot.table != N00B_ABI_TABLE_NATIVE_BINDING
                 && !core_string_eq(slot.module, ir_module_name(runtime)))
            valid = slot.function_type
                && n00bb_abi_type_index_add(index, slot.function_type,
                                            nullptr);
    }
    core_free(places);
    core_free(edges);
    return valid;
}

static core_string_t
graph_function_path(const n00bb_abi_graph_slot_t *slot)
{
    core_buffer_t path = {0};
    core_buffer_printf(&path, "%.*s.%.*s", (int)slot->module.u8_bytes,
                       slot->module.data, (int)slot->function.u8_bytes,
                       slot->function.data);
    return core_buffer_string(&path);
}

static bool
baked_function_row(const n00bb_abi_baked_functions_t *baked,
                   const n00bb_abi_graph_slot_t *slot, uint32_t *out)
{
    core_string_t path = baked ? graph_function_path(slot)
                               : (core_string_t){};
    bool found = false;
    for (size_t i = 0; path.data && i < baked->count && !found; i++)
        if (core_string_eq(baked->paths[i], path)) {
            *out = baked->first_requirement + (uint32_t)i;
            found = true;
        }
    core_free(path.data);
    return found;
}

bool
n00bb_abi_graph_function_requirements(
    n00bb_abi_type_index_t *index,
    const n00bb_abi_type_identity_rows_t *types,
    const n00bb_abi_decl_index_t *declarations, ir_module_t *runtime,
    n00bb_abi_declaration_lookup_fn lookup, void *lookup_context,
    const n00bb_abi_graph_source_t *graphs, n00bb_abi_name_pool_t *pool,
    n00b_abi_requirement_v1 **requirements, size_t *requirement_count,
    n00bb_abi_baked_functions_t *out)
{
    *out = (n00bb_abi_baked_functions_t){
        .first_requirement = (uint32_t)*requirement_count + 1,
    };
    if (!graphs) return true;
    graph_place_t *places = nullptr;
    graph_edge_t *edges = nullptr;
    if (!graph_placement_plan(graphs, declarations, runtime, lookup,
            lookup_context, &places, &edges)) return false;
    bool valid = true;
    for (size_t i = 0; valid && places && i < graphs->slot_count; i++) {
        n00bb_abi_graph_slot_t slot;
        uint32_t existing = 0;
        valid = graphs->slot(graphs->context, i, &slot)
            && slot.node < graphs->node_count;
        if (!valid || !(places[slot.node].reachability & 1)
            || slot.table != N00B_ABI_TABLE_FUNCTION
            || core_string_eq(slot.module, ir_module_name(runtime))
            || baked_function_row(out, &slot, &existing)) continue;
        uint32_t type_index = 0;
        core_string_t path = graph_function_path(&slot);
        valid = path.data && slot.function_type
            && n00bb_abi_type_index_find(index, slot.function_type,
                                         &type_index)
            && type_index < types->count
            && types->rows[type_index].kind == N00B_ABI_TYPE_FUNCTION
            && *requirement_count < UINT32_MAX - 1;
        n00b_abi_requirement_v1 *grown = valid
            ? core_realloc(*requirements,
                (*requirement_count + 2) * sizeof(**requirements))
            : nullptr;
        core_string_t *paths = grown
            ? core_realloc(out->paths, (out->count + 1) * sizeof(core_string_t))
            : nullptr;
        if (grown) *requirements = grown;
        if (paths) out->paths = paths;
        valid = grown && paths;
        if (!valid) {
            core_free(path.data);
            break;
        }
        size_t next = ++*requirement_count;
        grown[next] = (n00b_abi_requirement_v1){
            .symbol_kind = N00B_ABI_SYMBOL_FUNCTION,
            .contract_type_index = type_index,
            .requesting_module_index = 1,
            .flags = N00B_ABI_REQUIREMENT_FLAG_BAKED,
        };
        out->paths[out->count++] = path;
        valid = n00bb_abi_name_pool_encode(pool, path,
            N00B_ABI_TABLE_REQUIREMENT, (uint32_t)next,
            offsetof(n00b_abi_requirement_v1, full_path),
            &grown[next].full_path);
    }
    core_free(places);
    core_free(edges);
    return valid;
}

void
n00bb_abi_baked_functions_release(n00bb_abi_baked_functions_t *baked)
{
    for (size_t i = 0; baked && i < baked->count; i++)
        core_free(baked->paths[i].data);
    if (baked) core_free(baked->paths);
    if (baked) *baked = (n00bb_abi_baked_functions_t){0};
}

static bool
graph_slot_index(const n00bb_abi_type_identity_rows_t *types,
                 const n00bb_abi_function_rows_t *functions,
                 const n00bb_abi_baked_functions_t *baked,
                 const n00bb_abi_graph_slot_t *slot, uint32_t *table,
                 uint32_t *out)
{
    *table = slot->table;
    if (slot->table == N00B_ABI_TABLE_TYPE)
        return graph_type_index(types, slot->typehash, out) && *out;
    for (size_t i = 1; functions && i < functions->function_count; i++) {
        ir_func_t *function = functions->ir_functions[i];
        if (function && function->name.data && function->owner
            && core_string_eq(function->name, slot->function)
            && core_string_eq(ir_module_name(function->owner), slot->module)) {
            *out = (uint32_t)i;
            return true;
        }
    }
    *table = N00B_ABI_TABLE_REQUIREMENT;
    return baked_function_row(baked, slot, out);
}

static bool
graph_slot_patch(n00bb_abi_storage_rows_t *built,
                 const graph_place_t *place, const n00bb_abi_graph_slot_t *slot,
                 uint32_t table, uint32_t local_index,
                 core_list_t(n00bb_abi_storage_index_patch_t) *patches)
{
    uint32_t *unused_alignment = nullptr;
    core_buffer_t *buffer = graph_segment(built, place->kind,
                                          &unused_alignment);
    uint64_t offset = place->offset + slot->offset;
    if (!buffer || offset % sizeof(uint32_t) || local_index > INT32_MAX
        || offset > buffer->byte_len
        || buffer->byte_len - offset < sizeof(uint32_t)) return false;
    uint8_t *address = (uint8_t *)buffer->data + offset;
    uint32_t value = local_index;
    if (slot->kind == N00B_ABI_PATCH_BOX_TYPE)
        value |= core_le_load_u32(address) & UINT32_C(0x80000000);
    else if (slot->kind != N00B_ABI_PATCH_INDEX) return false;
    core_le_store_u32(address, value);
    core_list_push(*patches, ((n00bb_abi_storage_index_patch_t){
        .source_kind = place->kind,
        .byte_offset = offset,
        .table_kind = table,
        .patch_kind = slot->kind,
        .local_index = local_index,
    }));
    return true;
}

bool
n00bb_abi_storage_rows_build(
    n00bb_abi_type_index_t *index,
    const n00bb_abi_decl_index_t *declarations,
    const n00bb_abi_type_identity_rows_t *types,
    const n00bb_abi_function_rows_t *functions,
    const n00bb_abi_baked_functions_t *baked_functions,
    ir_module_t *runtime, n00bb_abi_name_pool_t *pool,
    uint32_t stream_base,
    n00bb_abi_declaration_lookup_fn lookup, void *lookup_context,
    const n00bb_abi_graph_source_t *graphs,
    n00bb_abi_storage_rows_t *out)
{
    if (!index || !declarations || !types || !types->rows || !runtime || !pool
        || !out) return false;
    core_list_t(n00b_abi_storage_v1) rows = {0};
    core_list_t(uint32_t) statics = {0}, locals = {0};
    core_list_t(n00bb_abi_storage_relocation_t) relocations = {0};
    core_list_t(n00bb_abi_storage_relocation_t) tls_fixups = {0};
    core_list_t(n00bb_abi_storage_index_patch_t) index_patches = {0};
    core_list_t(pending_address_t) addresses = {0};
    core_dict_t global_rows;
    core_dict_init(&global_rows, nullptr, nullptr);
    n00bb_abi_storage_rows_t built = {0};
    graph_place_t *graph_places = nullptr;
    graph_edge_t *graph_edges = nullptr;
    if (!graph_placement_plan(graphs, declarations, runtime, lookup,
            lookup_context, &graph_places, &graph_edges)) return false;
    built.shapes = core_alloc_array(n00b_abi_shape_v1, 1);
    built.shape_count = 1;
    core_list_push(rows, ((n00b_abi_storage_v1){0}));
    size_t matched_globals = 0;
    bool valid = true;
    for (size_t i = 1; valid && i < n00bb_abi_decl_symbol_count(declarations);
         i++) {
        parse_sym_entry_t *symbol = n00bb_abi_decl_symbol_at(declarations,
                                                              (uint32_t)i);
        if (!symbol || symbol->kind != PARSE_SYM_VARIABLE) continue;
        if (!lookup) {
            valid = false;
            break;
        }
        ir_global_t *global = storage_global(symbol, lookup, lookup_context);
        if (!global || global->owner != runtime || global->is_extern
            || !ir_global_can_read(global, IR_PHASE_RUNTIME)) continue;
        ir_storage_duration_t duration = ir_global_storage_duration(global);
        if (duration != IR_STORAGE_DURATION_MODULE
            && duration != IR_STORAGE_DURATION_THREAD) {
            valid = false;
            break;
        }
        if (rows.len >= UINT32_MAX
            || i >= UINT32_MAX || statics.len > UINT32_MAX - stream_base
            || locals.len > UINT32_MAX - stream_base - statics.len) {
            valid = false;
            break;
        }
        th_type *type = n00bb_abi_type_index_symbol_type(index, symbol);
        uint32_t type_index = 0;
        if (!type || !ir_type_eq(runtime, type, global->type)
            || !n00bb_abi_type_index_find(index, type, &type_index)
            || type_index >= types->count) {
            valid = false;
            break;
        }
        const n00b_abi_type_v1 *type_row = &types->rows[type_index];
        const ir_final_layout_t *layout = ir_layout_final_visible(runtime,
            global->type, ir_module_target(runtime));
        uint32_t alignment = layout ? layout->alignment_bytes
                                    : type_row->alignment_bytes;
        uint64_t bits = layout ? layout->intrinsic_bits : type_row->size_bits;
        uint8_t bytes[16] = {0};
        uint32_t descriptor_index = UINT32_MAX;
        size_t pool_offset = 0;
        const ir_global_init_value_t *initializer =
            global->has_static_initializer ? global->static_initializer
                                           : nullptr;
        bool is_string = type_row->kind == N00B_ABI_TYPE_STRING;
        bool is_array = type_row->kind == N00B_ABI_TYPE_ARRAY;
        bool is_composite = type_row->kind == N00B_ABI_TYPE_RECORD
            || type_row->kind == N00B_ABI_TYPE_VARIANT
            || type_row->kind == N00B_ABI_TYPE_TUPLE
            || (type_row->kind == N00B_ABI_TYPE_OPTION && initializer
                && initializer->kind != IR_GLOBAL_INIT_NIL
                && initializer->kind != IR_GLOBAL_INIT_PHASE_RESULT);
        size_t graph_root = SIZE_MAX;
        const uint8_t *graph_bytes = nullptr;
        size_t graph_size = 0;
        uint32_t graph_alignment = 0;
        uint64_t graph_typehash = 0;
        bool phase_graph = initializer && graphs && graph_places
            && initializer->kind == IR_GLOBAL_INIT_PHASE_RESULT
            && graphs->root(graphs->context,
                initializer->phase_result.identifier, &graph_root);
        if (phase_graph && (!graphs->node(graphs->context, graph_root,
                &graph_bytes, &graph_size, &graph_alignment, &graph_typehash)
            || graph_root >= graphs->node_count
            || graph_typehash != type_row->typehash)) {
            valid = false;
            break;
        }
        bool atomic_option = false;
        if (type_row->kind == N00B_ABI_TYPE_ATOMIC) {
            const th_arg *wrapped = th_named_arg(global->type, 0);
            uint32_t wrapped_index = 0;
            atomic_option = wrapped && wrapped->kind == TH_ARG_TYPE
                && n00bb_abi_type_index_find(index, wrapped->type,
                                             &wrapped_index)
                && wrapped_index < types->count
                && types->rows[wrapped_index].kind == N00B_ABI_TYPE_OPTION;
        }
        bool zero_option = (type_row->kind == N00B_ABI_TYPE_OPTION
                            || atomic_option)
            && initializer && initializer->kind == IR_GLOBAL_INIT_NIL;
        bool uninitialized = !initializer;
        bool address = global_address(initializer);
        if (!alignment || (alignment & (alignment - 1))
            || bits > SIZE_MAX - 7
            || (phase_graph ? false
                : address ? bits != 64
                : is_string
                ? (bits != 128 || alignment != 8
                    || !encode_string(pool, initializer, bytes, &pool_offset))
                : is_array ? (bits != 128 || alignment != 8)
                : uninitialized ? false
                : is_composite ? false
                : zero_option ? false
                : !encode_scalar(runtime, index, global->type, initializer,
                                 bits, bytes, &descriptor_index))) {
            valid = false;
            break;
        }
        uint32_t kind = duration == IR_STORAGE_DURATION_THREAD
            ? N00B_ABI_STORAGE_TLS
            : global->mutability == IR_MUT_CONST
                ? N00B_ABI_STORAGE_READ_ONLY
                : N00B_ABI_STORAGE_WRITABLE;
        bool long_string = is_string && initializer
            && initializer->kind == IR_GLOBAL_INIT_STRING
            && initializer->string.u8_bytes
                > N00BB_ABI_STRING_SHORT_CAPACITY;
        core_buffer_t *buffer = kind == N00B_ABI_STORAGE_TLS
            ? &built.tls : kind == N00B_ABI_STORAGE_READ_ONLY
                ? &built.read_only : &built.writable;
        uint32_t *section_alignment = kind == N00B_ABI_STORAGE_TLS
            ? &built.tls_alignment : kind == N00B_ABI_STORAGE_READ_ONLY
                ? &built.read_only_alignment : &built.writable_alignment;
        size_t byte_length = (size_t)(bits / 8 + (bits % 8 != 0));
        if (phase_graph && (graph_size != byte_length
            || graph_alignment > alignment
            || graph_places[graph_root].placed)) {
            valid = false;
            break;
        }
        valid = append_padding(buffer, alignment)
            && byte_length <= SIZE_MAX - buffer->byte_len
            && byte_length <= UINT64_MAX - buffer->byte_len;
        if (!valid) break;
        n00b_abi_storage_v1 row = {
            .module_index = 1,
            .symbol_index = (uint32_t)i,
            .type_index = type_index,
            .segment_kind = kind,
            .byte_offset = buffer->byte_len,
            .byte_length = byte_length,
        };
        array_writer_t writer = {
            .index = index, .types = types, .runtime = runtime,
            .pool = pool, .built = &built, .relocations = &relocations,
            .tls_fixups = &tls_fixups, .index_patches = &index_patches,
            .addresses = &addresses,
        };
        if (address && !encode_global_address(&writer, initializer, bits,
                kind, row.byte_offset * 8)) {
            valid = false;
            break;
        }
        core_dict_put(&global_rows, ir_global_identity(global),
                      (void *)(uintptr_t)rows.len);
        if (phase_graph) {
            core_buffer_append(buffer, (const char *)graph_bytes,
                byte_length);
            graph_places[graph_root].placed = true;
            graph_places[graph_root].kind = kind;
            graph_places[graph_root].offset = row.byte_offset;
        }
        else if (is_composite && initializer) {
            size_t record_bytes = byte_length < 16 ? 16 : byte_length;
            uint8_t *record = core_alloc_array(uint8_t, record_bytes);
            memset(record, 0, record_bytes);
            core_buffer_append(buffer, (const char *)record, byte_length);
            valid = row.byte_offset <= UINT64_MAX / 8
                && encode_typed_value(&writer, global->type, type_row,
                    initializer, kind, row.byte_offset * 8, record,
                    byte_length, 0);
            if (valid)
                memcpy(buffer->data + row.byte_offset, record, byte_length);
            core_free(record);
            if (!valid) break;
        }
        else if (zero_option || uninitialized) {
            static const char zero[16] = {0};
            for (size_t left = byte_length; left;) {
                size_t count = left < sizeof zero ? left : sizeof zero;
                core_buffer_append(buffer, zero, count);
                left -= count;
            }
        }
        else core_buffer_append(buffer, (const char *)bytes, byte_length);
        if (is_array && initializer && !phase_graph
            && !encode_array(&writer, global->type, initializer,
                    global->shape, kind, row.byte_offset,
                    &row.instance_shape_index)) {
            valid = false;
            break;
        }
        if (kind == N00B_ABI_STORAGE_TLS)
            row.byte_length = buffer->byte_len - row.byte_offset;
        if (descriptor_index != UINT32_MAX
            && !record_type_index(&writer, kind, row.byte_offset,
                                  descriptor_index)) {
            valid = false;
            break;
        }
        if (long_string)
            core_list_push(relocations,
                ((n00bb_abi_storage_relocation_t){
                    .source_kind = kind,
                    .byte_offset = row.byte_offset,
                    .target_kind = N00BB_ABI_RELOC_STRING_POOL,
                    .target_offset = pool_offset,
                }));
        if (*section_alignment < alignment) *section_alignment = alignment;
        if (kind == N00B_ABI_STORAGE_TLS)
            core_list_push(locals, (uint32_t)rows.len);
        else core_list_push(statics, (uint32_t)rows.len);
        core_list_push(rows, row);
        matched_globals++;
    }
    for (size_t i = 0; valid && i < addresses.len; i++) {
        const pending_address_t *address = &addresses.data[i];
        ir_global_t *target = ir_global_find(runtime, address->global);
        bool found = false;
        uintptr_t slot = target ? (uintptr_t)core_dict_get(&global_rows,
            ir_global_identity(target), &found) : 0;
        const n00b_abi_storage_v1 *row = found ? &rows.data[slot] : nullptr;
        valid = row && (row->segment_kind == N00B_ABI_STORAGE_READ_ONLY
                        || row->segment_kind == N00B_ABI_STORAGE_WRITABLE);
        if (valid)
            core_list_push(relocations, ((n00bb_abi_storage_relocation_t){
                .source_kind = address->source_kind,
                .byte_offset = address->byte_offset,
                .target_kind = row->segment_kind,
                .target_offset = row->byte_offset,
            }));
    }
    for (size_t i = 0; valid && graph_places && i < graphs->node_count; i++) {
        graph_place_t *place = &graph_places[i];
        if (!(place->reachability & 1)) continue;
        uint32_t kind = place->reachability & 2
            || (graphs->flags(graphs->context, i)
                & N00B_ABI_BAKED_NODE_FOREIGN)
            ? N00B_ABI_STORAGE_WRITABLE : N00B_ABI_STORAGE_READ_ONLY;
        if (place->placed) {
            valid = !((place->reachability & 2)
                && place->kind == N00B_ABI_STORAGE_READ_ONLY);
            continue;
        }
        const uint8_t *bytes = nullptr;
        size_t size = 0;
        uint32_t alignment = 0;
        uint64_t typehash = 0;
        valid = graphs->node(graphs->context, i, &bytes,
            &size, &alignment, &typehash) && bytes && size;
        if (typehash == 0) kind = N00B_ABI_STORAGE_READ_ONLY;
        uint32_t *section_alignment = nullptr;
        core_buffer_t *buffer = valid ? graph_segment(&built, kind,
            &section_alignment) : nullptr;
        valid = buffer
            && append_padding(buffer, alignment)
            && size <= SIZE_MAX - buffer->byte_len;
        if (!valid) break;
        place->placed = true;
        place->kind = kind;
        place->offset = buffer->byte_len;
        core_buffer_append(buffer, (const char *)bytes, size);
        if (*section_alignment < alignment)
            *section_alignment = alignment;
    }
    core_list_t(n00b_abi_baked_node_v1) baked = {0};
    core_list_push(baked, ((n00b_abi_baked_node_v1){0}));
    for (size_t i = 0; valid && graph_places && i < graphs->node_count; i++) {
        graph_place_t *place = &graph_places[i];
        if (!place->placed) continue;
        const uint8_t *bytes = nullptr;
        size_t size = 0;
        uint32_t alignment = 0, type_index = 0;
        uint64_t typehash = 0;
        valid = graphs->node(graphs->context, i, &bytes, &size, &alignment,
                &typehash)
            && graph_type_index(types, typehash, &type_index);
        uint32_t flags = valid ? graphs->flags(graphs->context, i) : 0;
        if (!typehash) flags |= N00B_ABI_BAKED_NODE_IMMUTABLE;
        if (valid) core_list_push(baked, ((n00b_abi_baked_node_v1){
            .module_index = 1,
            .type_index = type_index,
            .segment_kind = place->kind,
            .flags = flags,
            .byte_offset = place->offset,
            .byte_length = size,
        }));
    }
    for (size_t i = 0; valid && graph_places && i < graphs->slot_count; i++) {
        n00bb_abi_graph_slot_t slot;
        uint32_t local_index = 0;
        valid = graphs->slot(graphs->context, i, &slot)
            && slot.node < graphs->node_count;
        if (!valid || !graph_places[slot.node].placed) continue;
        if (slot.table == N00B_ABI_TABLE_NATIVE_BINDING) {
            uint32_t *unused_alignment = nullptr;
            const graph_place_t *place = &graph_places[slot.node];
            core_buffer_t *buffer = graph_segment(&built, place->kind,
                &unused_alignment);
            uint64_t offset = place->offset + slot.offset;
            valid = buffer && slot.native.data && offset <= buffer->byte_len
                && buffer->byte_len - offset >= sizeof(uint64_t);
            if (!valid) break;
            memset(buffer->data + offset, 0, sizeof(uint64_t));
            core_list_push(relocations,
                ((n00bb_abi_storage_relocation_t){
                    .source_kind = place->kind,
                    .byte_offset = offset,
                    .target_kind = N00BB_ABI_RELOC_NATIVE,
                    .native = slot.native,
                }));
            continue;
        }
        uint32_t table = 0;
        valid = graph_slot_index(types, functions, baked_functions, &slot,
                                 &table, &local_index)
            && graph_slot_patch(&built, &graph_places[slot.node], &slot,
                                table, local_index, &index_patches);
    }
    for (size_t i = 0; valid && graph_places && i < graphs->edge_count; i++) {
        graph_edge_t *edge = &graph_edges[i];
        if (edge->duplicate) continue;
        graph_place_t *source = &graph_places[edge->source];
        graph_place_t *target = &graph_places[edge->target];
        if (!(source->reachability & 1)) continue;
        const uint8_t *source_bytes = nullptr, *target_bytes = nullptr;
        size_t source_size = 0, target_size = 0;
        uint32_t source_align = 0, target_align = 0;
        uint64_t source_hash = 0, target_hash = 0;
        uint32_t *unused_alignment = nullptr;
        core_buffer_t *buffer = graph_segment(&built, source->kind,
            &unused_alignment);
        valid = source->placed && target->placed && buffer
            && graphs->node(graphs->context, edge->source, &source_bytes,
                &source_size, &source_align, &source_hash)
            && graphs->node(graphs->context, edge->target, &target_bytes,
                &target_size, &target_align, &target_hash)
            && edge->source_offset <= source_size
            && sizeof(uint64_t) <= source_size - edge->source_offset
            && edge->target_offset < target_size
            && source->offset <= UINT64_MAX - edge->source_offset
            && target->offset <= UINT64_MAX - edge->target_offset;
        if (!valid) break;
        uint64_t source_offset = source->offset + edge->source_offset;
        uint64_t target_offset = target->offset + edge->target_offset;
        if (source_offset > buffer->byte_len
            || sizeof(uint64_t) > buffer->byte_len - source_offset) {
            valid = false;
            break;
        }
        memset(buffer->data + source_offset, 0, sizeof(uint64_t));
        core_list_push(relocations,
            ((n00bb_abi_storage_relocation_t){
                .source_kind = source->kind,
                .byte_offset = source_offset,
                .target_kind = target->kind,
                .target_offset = target_offset,
            }));
    }
    for (size_t i = 0; valid && i < ir_global_count(runtime); i++) {
        ir_global_t *global = ir_global_at(runtime, i);
        if (!global) valid = false;
        else if (!global->is_extern
                 && ir_global_can_read(global, IR_PHASE_RUNTIME)) {
            if (!matched_globals) valid = false;
            else matched_globals--;
        }
    }
    valid = valid && !matched_globals
        && statics.len <= UINT32_MAX - stream_base
        && locals.len <= UINT32_MAX - stream_base - statics.len;
    if (valid) {
        built.stream_count = statics.len + locals.len;
        built.stream_indexes = core_alloc_array(uint32_t, built.stream_count);
        if (statics.len)
            memcpy(built.stream_indexes, statics.data,
                   statics.len * sizeof(uint32_t));
        if (locals.len)
            memcpy(built.stream_indexes + statics.len, locals.data,
                   locals.len * sizeof(uint32_t));
        if (statics.len) built.static_range = (n00b_abi_range_v1){
            .first = stream_base, .count = (uint32_t)statics.len,
        };
        if (locals.len) built.tls_range = (n00b_abi_range_v1){
            .first = stream_base + (uint32_t)statics.len,
            .count = (uint32_t)locals.len,
        };
        built.rows = rows.data;
        built.row_count = rows.len;
        built.relocations = relocations.data;
        built.relocation_count = relocations.len;
        built.tls_fixups = tls_fixups.data;
        built.tls_fixup_count = tls_fixups.len;
        built.index_patches = index_patches.data;
        built.index_patch_count = index_patches.len;
        built.baked_nodes = baked.data;
        built.baked_node_count = baked.len;
        *out = built;
    }
    else {
        core_list_free(baked);
        core_list_free(rows);
        core_list_free(relocations);
        core_list_free(tls_fixups);
        core_list_free(index_patches);
        n00bb_abi_storage_rows_release(&built);
    }
    core_free(graph_places);
    core_free(graph_edges);
    core_list_free(statics);
    core_list_free(locals);
    core_list_free(addresses);
    core_dict_free(&global_rows);
    return valid;
}

void
n00bb_abi_storage_rows_release(n00bb_abi_storage_rows_t *rows)
{
    if (!rows) return;
    core_free(rows->rows);
    core_free(rows->stream_indexes);
    core_free(rows->relocations);
    core_free(rows->tls_fixups);
    core_free(rows->index_patches);
    core_free(rows->baked_nodes);
    core_free(rows->shapes);
    core_free(rows->extents);
    core_buffer_release(&rows->read_only);
    core_buffer_release(&rows->writable);
    core_buffer_release(&rows->tls);
    core_buffer_release(&rows->aggregates);
    *rows = (n00bb_abi_storage_rows_t){0};
}
