#include "abi_storage.h"
#include "abi_string.h"

#include <limits.h>
#include <string.h>

#include "ir/typelang.h"
#include "ir/shape.h"
#include "lib/alloc.h"
#include "lib/byteorder.h"
#include "lib/list.h"

core_list_decl(n00b_abi_storage_v1);
core_list_decl(uint32_t);
core_list_decl(n00bb_abi_storage_relocation_t);

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

static bool
encode_scalar(ir_module_t *module, th_type *type,
              const ir_global_init_value_t *value,
              uint64_t size_bits, uint8_t bytes[16])
{
    ir_machine_t machine = ir_machine_of(module, type);
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
        || (value->string.length && !value->string.bytes)) return false;
    n00b_abi_string_encoding_v1 encoding = N00B_ABI_STRING_ASCII;
    for (size_t i = 0; i < value->string.length; i++)
        if ((uint8_t)value->string.bytes[i] >= 0x80) {
            encoding = N00B_ABI_STRING_UTF8;
            break;
        }
    n00b_abi_string_v1 encoded = {0};
    if (!n00bb_abi_name_pool_value(pool,
        core_string_borrow(value->string.bytes, value->string.length),
        encoding, &encoded, pool_offset)) return false;
    memcpy(bytes, &encoded, sizeof encoded);
    return true;
}

static th_type *
array_element_type(th_type *type)
{
    if (th_kind_of(type) == TH_SEQUENCE) return th_seq_elem(type);
    if (th_kind_of(type) != TH_NAMED || th_named_nargs(type) != 1)
        return nullptr;
    const char *name = nullptr;
    size_t length = 0;
    if (!th_named_info(type, &name, &length, nullptr, nullptr) || !name
        || !((length == 5 && !memcmp(name, "Array", 5))
             || (length == 5 && !memcmp(name, "array", 5)))) return nullptr;
    const th_arg *argument = th_named_arg(type, 0);
    return argument && argument->kind == TH_ARG_TYPE ? argument->type : nullptr;
}

static bool
array_extent_matches(const ir_global_t *global, size_t count)
{
    if (!global->shape) return true;
    if (ir_shape_rank(global->shape) != 1) return false;
    ir_shape_extent_t extent;
    return ir_shape_extent(global->shape, 0, &extent)
        && (extent.kind != IR_SHAPE_EXTENT_STATIC
            || extent.static_value == count);
}

static bool
encode_array(n00bb_abi_type_index_t *index,
             const n00bb_abi_type_identity_rows_t *types,
             ir_module_t *runtime, n00bb_abi_name_pool_t *pool,
             const ir_global_t *global, n00bb_abi_storage_rows_t *built,
             core_list_t(n00bb_abi_storage_relocation_t) *relocations,
             uint32_t source_kind, uint64_t source_offset,
             uint32_t *shape_index)
{
    const ir_global_init_value_t *value = global->static_initializer;
    if (!global->has_static_initializer || !value
        || value->kind != IR_GLOBAL_INIT_SEQUENCE
        || value->aggregate.count > UINT32_MAX
        || (value->aggregate.count && !value->aggregate.items)
        || !array_extent_matches(global, value->aggregate.count)) return false;
    th_type *element = array_element_type(global->type);
    uint32_t element_index;
    if (!element || !n00bb_abi_type_index_find(index, element,
            &element_index) || element_index >= types->count) return false;
    const n00b_abi_type_v1 *element_row = &types->rows[element_index];
    size_t count = value->aggregate.count;
    size_t alignment = element_row->alignment_bytes;
    size_t stride = (size_t)(element_row->size_bits / 8);
    bool packed = element_row->kind == N00B_ABI_TYPE_PRIMITIVE
        && (ir_machine_of(runtime, element).class == IR_CLASS_BOOL
            || (element_row->flags & N00B_ABI_TYPE_FLAG_BITS));
    size_t width = element_row->semantic_width_bits;
    if (!alignment || (alignment & (alignment - 1))
        || element_row->size_bits % 8 || stride > 16 || !stride
        || (packed && (!width || width > 128
            || count > (SIZE_MAX - 7) / width))
        || (!packed && count > SIZE_MAX / stride)
        || built->shape_count >= UINT32_MAX
        || built->extent_count >= UINT32_MAX) return false;
    size_t data_size = packed ? (count * width + 7) / 8 : count * stride;
    if (!data_size) data_size = 1;
    core_buffer_t *buffer = &built->aggregates;
    if (!append_padding(buffer, alignment)
        || data_size >= SIZE_MAX - buffer->byte_len) return false;
    size_t data_offset = buffer->byte_len;
    core_buffer_ensure(buffer, data_size + 1);
    memset(buffer->data + buffer->byte_len, 0, data_size);
    buffer->byte_len += data_size;
    buffer->data[buffer->byte_len] = 0;
    if (!append_padding(buffer, 8)
        || buffer->byte_len > UINT64_MAX - 32) return false;
    size_t descriptor_offset = buffer->byte_len;
    uint8_t descriptor[32] = {0};
    core_le_store_u64(descriptor, count);
    core_le_store_u32(descriptor + 8, 1);
    core_le_store_u32(descriptor + 12, 1);
    core_le_store_u64(descriptor + 24, count);
    core_buffer_append(buffer, (const char *)descriptor, sizeof descriptor);
    if (built->aggregate_alignment < alignment)
        built->aggregate_alignment = (uint32_t)alignment;
    if (built->aggregate_alignment < 8) built->aggregate_alignment = 8;
    for (size_t i = 0; i < count; i++) {
        const ir_global_init_value_t *item = &value->aggregate.items[i];
        uint8_t bytes[16] = {0};
        size_t pool_offset = 0;
        bool string = element_row->kind == N00B_ABI_TYPE_STRING;
        if (string ? (stride != 16 || !encode_string(pool, item, bytes,
                           &pool_offset))
                   : !encode_scalar(runtime, element, item,
                                    element_row->size_bits, bytes))
            return false;
        if (packed) {
            size_t first_bit = i * width;
            for (size_t bit = 0; bit < width; bit++)
                if (bytes[bit / 8] & (1u << (bit % 8)))
                    buffer->data[data_offset + (first_bit + bit) / 8]
                        |= (char)(1u << ((first_bit + bit) % 8));
        }
        else {
            size_t at = data_offset + i * stride;
            memcpy(buffer->data + at, bytes, stride);
            if (string && item->string.length
                > N00BB_ABI_STRING_SHORT_CAPACITY)
                core_list_push(*relocations,
                    ((n00bb_abi_storage_relocation_t){
                        .source_kind = N00BB_ABI_RELOC_AGGREGATE,
                        .byte_offset = at,
                        .target_kind = N00BB_ABI_RELOC_STRING_POOL,
                        .target_offset = pool_offset,
                    }));
        }
    }
    n00b_abi_shape_v1 *shapes = core_realloc(built->shapes,
        (built->shape_count + 1) * sizeof(*shapes));
    uint64_t *extents = core_realloc(built->extents,
        (built->extent_count + 1) * sizeof(*extents));
    built->shapes = shapes;
    built->extents = extents;
    *shape_index = (uint32_t)built->shape_count;
    shapes[built->shape_count++] = (n00b_abi_shape_v1){
        .total_count = count, .rank = 1, .flat_rank = 1,
        .extents = {.first = (uint32_t)built->extent_count, .count = 1},
    };
    extents[built->extent_count++] = count;
    core_list_push(*relocations, ((n00bb_abi_storage_relocation_t){
        .source_kind = source_kind, .byte_offset = source_offset,
        .target_kind = N00BB_ABI_RELOC_AGGREGATE,
        .target_offset = data_offset,
    }));
    core_list_push(*relocations, ((n00bb_abi_storage_relocation_t){
        .source_kind = source_kind, .byte_offset = source_offset + 8,
        .target_kind = N00BB_ABI_RELOC_AGGREGATE,
        .target_offset = descriptor_offset,
    }));
    return true;
}

bool
n00bb_abi_storage_rows_build(
    n00bb_abi_type_index_t *index,
    const n00bb_abi_decl_index_t *declarations,
    const n00bb_abi_type_identity_rows_t *types,
    ir_module_t *runtime, n00bb_abi_name_pool_t *pool,
    uint32_t stream_base,
    n00bb_abi_declaration_lookup_fn lookup, void *lookup_context,
    n00bb_abi_storage_rows_t *out)
{
    if (!index || !declarations || !types || !types->rows || !runtime || !pool
        || !out) return false;
    core_list_t(n00b_abi_storage_v1) rows = {0};
    core_list_t(uint32_t) statics = {0}, locals = {0};
    core_list_t(n00bb_abi_storage_relocation_t) relocations = {0};
    n00bb_abi_storage_rows_t built = {0};
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
        core_link_declaration_t *declaration = lookup(lookup_context, symbol);
        ir_global_t *global = declaration ? core_link_declaration_binding(
            declaration, CORE_STRING_STATIC("n00bb.ir.global")) : nullptr;
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
        uint32_t alignment = type_row->alignment_bytes;
        uint64_t bits = type_row->size_bits;
        uint8_t bytes[16] = {0};
        size_t pool_offset = 0;
        bool is_string = type_row->kind == N00B_ABI_TYPE_STRING;
        bool is_array = type_row->kind == N00B_ABI_TYPE_ARRAY;
        const ir_global_init_value_t *initializer =
            global->has_static_initializer ? global->static_initializer
                                           : nullptr;
        if (!alignment || (alignment & (alignment - 1))
            || (is_string
                ? (bits != 128 || alignment != 8
                    || !encode_string(pool, initializer, bytes, &pool_offset))
                : is_array ? (bits != 128 || alignment != 8)
                    : !encode_scalar(runtime, global->type, initializer,
                                     bits, bytes))) {
            valid = false;
            break;
        }
        uint32_t kind = duration == IR_STORAGE_DURATION_THREAD
            ? N00B_ABI_STORAGE_TLS
            : global->mutability == IR_MUT_CONST
                ? N00B_ABI_STORAGE_READ_ONLY
                : N00B_ABI_STORAGE_WRITABLE;
        bool long_string = is_string && global->has_static_initializer
            && global->static_initializer->string.length
                > N00BB_ABI_STRING_SHORT_CAPACITY;
        if (long_string && kind == N00B_ABI_STORAGE_TLS) {
            valid = false;
            break;
        }
        core_buffer_t *buffer = kind == N00B_ABI_STORAGE_TLS
            ? &built.tls : kind == N00B_ABI_STORAGE_READ_ONLY
                ? &built.read_only : &built.writable;
        uint32_t *section_alignment = kind == N00B_ABI_STORAGE_TLS
            ? &built.tls_alignment : kind == N00B_ABI_STORAGE_READ_ONLY
                ? &built.read_only_alignment : &built.writable_alignment;
        valid = append_padding(buffer, alignment)
            && buffer->byte_len <= UINT64_MAX - bits / 8;
        if (!valid) break;
        n00b_abi_storage_v1 row = {
            .module_index = 1,
            .symbol_index = (uint32_t)i,
            .type_index = type_index,
            .segment_kind = kind,
            .byte_offset = buffer->byte_len,
            .byte_length = bits / 8,
        };
        core_buffer_append(buffer, (const char *)bytes, (size_t)row.byte_length);
        if (is_array && initializer
            && !encode_array(index, types, runtime, pool, global, &built,
                    &relocations, kind, row.byte_offset,
                    &row.instance_shape_index)) {
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
        *out = built;
    }
    else {
        core_list_free(rows);
        core_list_free(relocations);
        n00bb_abi_storage_rows_release(&built);
    }
    core_list_free(statics);
    core_list_free(locals);
    return valid;
}

void
n00bb_abi_storage_rows_release(n00bb_abi_storage_rows_t *rows)
{
    if (!rows) return;
    core_free(rows->rows);
    core_free(rows->stream_indexes);
    core_free(rows->relocations);
    core_free(rows->shapes);
    core_free(rows->extents);
    core_buffer_release(&rows->read_only);
    core_buffer_release(&rows->writable);
    core_buffer_release(&rows->tls);
    core_buffer_release(&rows->aggregates);
    *rows = (n00bb_abi_storage_rows_t){0};
}
