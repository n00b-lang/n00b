#include <stdio.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#include "compiler_tu.h"
#include "compiler_link.h"
#include "compiler_layout.h"
#include "compiler_phases.h"
#include "compiler_abi_driver.h"
#include "compiler_object_io.h"
#include "abi_layout.h"
#include "abi_emit.h"
#include "abi_semantics.h"
#include "abi_static_graph.h"
#include "global_metadata.h"
#include "llvm_backend.h"
#include "n00b-abi-v1.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/list.h"
#include "lib/link_unit.h"
#include "lib/diagnostic.h"
#include "parse/tree.h"
#include "ir/ir.h"
#include "ir/layout.h"
#include "th/environment.h"

core_list_decl(uint32_t);

static bool
n00bb_comptime_is_empty(const ir_module_t *module)
{
    for (size_t i = 0; i < ir_func_count(module); i++) {
        ir_func_t *function = ir_func_at((ir_module_t *)module, i);
        if (!function || !ir_func_is_initializer(function)) continue;
        if (function->n_params || function->n_locals
            || function->n_blocks != 1 || !function->blocks[0]
            || function->blocks[0]->n_instrs != 1) return false;
        ir_instr_t *instruction = function->blocks[0]->instrs[0];
        if (!instruction || instruction->op != IR_OP_RETURN
            || instruction->n_operands) return false;
    }
    return true;
}

static bool
n00bb_literal_boolean_field(ir_module_t *module, th_type *type)
{
    const ir_final_layout_t *layout = type
        ? ir_layout_final(module, type, ir_module_target(module)) : nullptr;
    uint64_t stride_bits = 0;
    return layout && ir_machine_of(module, type).class == IR_CLASS_BOOL
        && layout->intrinsic_bits == sizeof(uint8_t) * 8
        && layout->alignment_bytes == 1
        && ir_layout_final_stride_bits(layout, &stride_bits)
        && stride_bits == layout->intrinsic_bits;
}

static bool
n00bb_phase_zero_option_type(th_type *type)
{
    th_type *atomic_element = n00bb_atomic_element_type(type);
    th_type *option_element = nullptr;
    return n00bb_option_type_element(
            atomic_element ? atomic_element : type, &option_element)
        && !n00bb_option_type_element(option_element, nullptr);
}

static bool
n00bb_literal_raw_element(ir_module_t *module, th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    const ir_final_layout_t *layout = type
        ? ir_layout_final(module, type, ir_module_target(module)) : nullptr;
    if (!layout || !layout->alignment_bytes || !layout->intrinsic_bits
        || layout->intrinsic_bits % 8) return false;
    ir_machine_t machine = ir_machine_of(module, type);
    if (machine.class == IR_CLASS_SINT || machine.class == IR_CLASS_UINT) {
        if (machine.width >= 8 && !(machine.width % 8)
            && layout->intrinsic_bits == machine.width) return true;
        if (machine.class == IR_CLASS_UINT && named_type_is(type, "char")
            && layout->intrinsic_bits >= machine.width) return true;
    }
    if (machine.class == IR_CLASS_FLOAT && machine.width == 64)
        return layout->intrinsic_bits == 64;

    const char *name = nullptr;
    size_t count = 0;
    if (!th_named_info(type, &name, nullptr, nullptr, &count)
        || !name) return false;
    if (!strcmp(name, "atomic") && count == 1) {
        const th_arg *argument = th_named_arg(type, 0);
        const ir_final_layout_t *child = argument
            && argument->kind == TH_ARG_TYPE
            ? ir_layout_final(module, argument->type,
                              ir_module_target(module)) : nullptr;
        uint64_t outer_stride = 0;
        uint64_t child_stride = 0;
        return child && layout->intrinsic_bits == child->intrinsic_bits
            && layout->alignment_bytes == child->alignment_bytes
            && ir_layout_final_stride_bits(layout, &outer_stride)
            && ir_layout_final_stride_bits(child, &child_stride)
            && outer_stride == child_stride
            && (n00bb_literal_raw_element(module, argument->type)
                || n00bb_literal_boolean_field(module, argument->type));
    }
    if (strcmp(name, TH_TUPLE) || !count
        || layout->n_members != count) return false;
    for (size_t i = 0; i < count; i++) {
        const th_arg *argument = th_named_arg(type, i);
        const ir_final_member_layout_t *member = &layout->members[i];
        const ir_final_layout_t *child = argument
            && argument->kind == TH_ARG_TYPE
            ? ir_layout_final(module, argument->type,
                ir_module_target(module)) : nullptr;
        if (!child || member->bit_offset % 8
            || member->bit_offset > layout->intrinsic_bits
            || child->intrinsic_bits
                > layout->intrinsic_bits - member->bit_offset
            || (!n00bb_literal_raw_element(module, argument->type)
                && !n00bb_literal_boolean_field(module, argument->type)))
            return false;
    }
    return true;
}

static bool
n00bb_literal_record_array_fields(ir_module_t *module, th_type *type,
                                  th_type **element, uint32_t *record_size,
                                  uint32_t *array_offset,
                                  uint32_t *array_index)
{
    const ir_final_layout_t *layout = ir_layout_final(module, type,
                                                      ir_module_target(module));
    uint64_t stride_bits = 0;
    if (!layout || layout->n_members < 2
        || !ir_layout_final_stride_bits(layout, &stride_bits)
        || stride_bits % 8 || stride_bits / 8 > UINT32_MAX)
        return false;
    *element = nullptr;
    *record_size = (uint32_t)(stride_bits / 8);
    for (size_t i = 0; i < layout->n_members; i++) {
        const ir_final_member_layout_t *member = &layout->members[i];
        th_type *field = member->type ? th_resolve(member->type) : nullptr;
        const ir_final_layout_t *field_layout = field
            ? ir_layout_final(module, field, ir_module_target(module))
            : nullptr;
        if (!field || !field_layout || member->bit_offset % 8
            || member->bit_offset > stride_bits
            || field_layout->intrinsic_bits > stride_bits - member->bit_offset)
            return false;
        if (th_kind_of(field) == TH_SEQUENCE) {
            if (*element || field_layout->intrinsic_bits
                    != sizeof(n00b_abi_array_value_v1) * 8)
                return false;
            *element = th_seq_elem(field);
            *array_offset = (uint32_t)(member->bit_offset / 8);
            *array_index = 0;
            for (size_t j = 0; j < layout->n_members; j++)
                if (layout->members[j].bit_offset < member->bit_offset)
                    (*array_index)++;
        } else {
            ir_machine_t machine = ir_machine_of(module, field);
            if (!n00bb_literal_raw_element(module, field)
                || (machine.class != IR_CLASS_SINT
                    && machine.class != IR_CLASS_UINT
                    && machine.class != IR_CLASS_FLOAT)) return false;
        }
    }
    return *element != nullptr;
}

static bool
n00bb_literal_string_offsets(ir_module_t *module, th_type *type,
                             uint64_t base, core_list_t(uint32_t) *offsets,
                             bool tuple_field)
{
    type = type ? th_resolve(type) : nullptr;
    const ir_final_layout_t *layout = type
        ? ir_layout_final(module, type, ir_module_target(module)) : nullptr;
    if (!layout || !layout->alignment_bytes || !layout->intrinsic_bits)
        return false;
    if (n00bb_literal_raw_element(module, type)) return true;
    if (tuple_field && n00bb_literal_boolean_field(module, type)) return true;
    const char *name = nullptr;
    size_t length = 0, count = 0;
    if (!th_named_info(type, &name, &length, nullptr, &count) || !name)
        return false;
    if (length == 6 && !memcmp(name, "string", 6) && !count) {
        if (layout->intrinsic_bits != sizeof(n00b_abi_string_v1) * 8
            || base > UINT32_MAX) return false;
        core_list_push(*offsets, (uint32_t)base);
        size_t at = offsets->len - 1;
        while (at && offsets->data[at - 1] > offsets->data[at]) {
            uint32_t prior = offsets->data[at - 1];
            offsets->data[at - 1] = offsets->data[at];
            offsets->data[at] = prior;
            at--;
        }
        return true;
    }
    if (length != strlen(TH_TUPLE) || memcmp(name, TH_TUPLE, length)
        || !count || layout->n_members != count) return false;
    for (size_t i = 0; i < count; i++) {
        const th_arg *argument = th_named_arg(type, i);
        const ir_final_member_layout_t *member = &layout->members[i];
        if (!argument || argument->kind != TH_ARG_TYPE
            || member->bit_offset % 8
            || member->bit_offset > layout->intrinsic_bits
            || member->bit_offset / 8 > UINT32_MAX
            || base > UINT32_MAX - member->bit_offset / 8
            || !n00bb_literal_string_offsets(module, argument->type,
                base + member->bit_offset / 8, offsets, true)) return false;
    }
    return true;
}

static uint8_t *
n00bb_literal_byte_mask(const ir_final_layout_t *layout,
                        uint64_t stride_bits)
{
    if (!layout || !stride_bits || stride_bits % 8
        || stride_bits / 8 > SIZE_MAX) return nullptr;
    uint8_t *mask = calloc((size_t)(stride_bits / 8), 1);
    if (!mask) return nullptr;
    for (size_t i = 0; i < layout->n_occupied; i++) {
        uint64_t first = layout->occupied[i].offset;
        uint64_t width = layout->occupied[i].width;
        if (first > stride_bits || width > stride_bits - first) {
            free(mask);
            return nullptr;
        }
        for (uint64_t bit = first; bit < first + width; bit++)
            mask[bit / 8] |= (uint8_t)(1u << (bit % 8));
    }
    return mask;
}

static bool
n00bb_capture_string_payload(int fd, off_t file_size, core_buffer_t *packed)
{
    uint64_t length;
    uint8_t encoding;
    if (!packed || !n00bb_read_exact(fd, &length, sizeof(length))
        || !n00bb_read_exact(fd, &encoding, sizeof(encoding))
        || encoding > N00B_ABI_STRING_UTF8) return false;
    off_t offset = lseek(fd, 0, SEEK_CUR);
    if (offset < 0 || offset > file_size
        || length > (uint64_t)(file_size - offset)
        || packed->byte_len > SIZE_MAX
            - sizeof(length) - sizeof(encoding) - 1
        || length > SIZE_MAX - packed->byte_len
            - sizeof(length) - sizeof(encoding) - 1) return false;
    core_buffer_append(packed, (const char *)&length, sizeof(length));
    core_buffer_putc(packed, (char)encoding);
    core_buffer_ensure(packed, (size_t)length + 1);
    if (!n00bb_read_exact(fd, packed->data + packed->byte_len,
                          (size_t)length)) return false;
    packed->byte_len += (size_t)length;
    packed->data[packed->byte_len] = 0;
    return true;
}

static bool
n00bb_capture_array_elements(int fd, off_t file_size, uint64_t count,
                             uint32_t stride,
                             const n00bb_llvm_array_producer_t *producer,
                             core_buffer_t *packed)
{
    off_t start = lseek(fd, 0, SEEK_CUR);
    bool reloc = producer->kind == N00BB_LLVM_LITERAL_RELOC
        || producer->kind == N00BB_LLVM_LITERAL_RECORD_RELOC
        || producer->kind == N00BB_LLVM_LITERAL_RECORD_FIELDS_RELOC;
    size_t fields = reloc ? producer->string_offset_count : 0;
    if (!stride || start < 0 || start > file_size
        || count > SIZE_MAX / stride
        || (reloc && (!fields || !producer->string_offsets
            || fields > (SIZE_MAX - stride)
                / (sizeof(uint64_t) + sizeof(uint8_t)))))
        return false;
    uint64_t minimum = stride + fields
        * (sizeof(uint64_t) + sizeof(uint8_t));
    if (count > (uint64_t)(file_size - start) / minimum
        || count > (SIZE_MAX - packed->byte_len - 1) / stride)
        return false;
    for (uint64_t i = 0; i < count; i++) {
        core_buffer_ensure(packed, stride + 1);
        if (!n00bb_read_exact(fd, packed->data + packed->byte_len,
                              stride)) return false;
        packed->byte_len += stride;
        packed->data[packed->byte_len] = 0;
        for (size_t f = 0; f < fields; f++)
            if (!n00bb_capture_string_payload(fd, file_size, packed))
                return false;
    }
    return true;
}

static bool
n00bb_capture_static_graph(int fd, off_t file_size, uint64_t count,
    const n00bb_llvm_array_producer_t *producer,
    n00bb_link_analysis_t *analysis, n00bb_phase_result_binding_t *binding)
{
    off_t start = lseek(fd, 0, SEEK_CUR);
    const size_t header_bytes = sizeof(uint64_t) * 3
        + sizeof(uint32_t) * 2;
    if (!count || count > SIZE_MAX / sizeof(size_t) || start < 0
        || start > file_size
        || count > (uint64_t)(file_size - start) / header_bytes)
        return false;
    size_t *indices = core_alloc_array(size_t, (size_t)count);
    if (!indices) return false;
    bool valid = true;
    for (uint64_t i = 0; valid && i < count; i++) {
        uint64_t address = 0, size = 0, typehash = 0;
        uint32_t alignment = 0, flags = 0;
        valid = n00bb_read_exact(fd, &address, sizeof(address))
            && n00bb_read_exact(fd, &size, sizeof(size))
            && n00bb_read_exact(fd, &alignment, sizeof(alignment))
            && n00bb_read_exact(fd, &typehash, sizeof(typehash))
            && n00bb_read_exact(fd, &flags, sizeof(flags));
        off_t position = valid ? lseek(fd, 0, SEEK_CUR) : -1;
        valid = valid && position >= 0 && position <= file_size
            && size && size <= SIZE_MAX && (typehash || flags == 1)
            && flags <= 1 && alignment
            && !(alignment & (alignment - 1))
            && size <= (uint64_t)(file_size - position)
            && (i == 0 ? address == 0 : address != 0);
        if (!valid) break;
        uint8_t *bytes = core_alloc_size(1, (size_t)size);
        valid = bytes && n00bb_read_exact(fd, bytes, (size_t)size);
        if (!valid) {
            core_free(bytes);
            break;
        }
        size_t existing = SIZE_MAX;
        if (address)
            for (size_t j = 0; j < analysis->static_graph_nodes.len; j++)
                if (analysis->static_graph_nodes.data[j].address == address) {
                    existing = j;
                    break;
                }
        char *symbol = nullptr;
        if (!i) {
            core_buffer_t name = {0};
            core_buffer_printf(&name,
                "__n00bb_comptime_literal_%016llx",
                (unsigned long long)core_xxh3_64bits_raw(
                    producer->identifier, strlen(producer->identifier)));
            symbol = name.data
                ? core_strndup(name.data, name.byte_len) : nullptr;
            core_buffer_release(&name);
            valid = symbol != nullptr;
        }
        if (existing != SIZE_MAX) {
            n00bb_static_graph_node_t *prior =
                &analysis->static_graph_nodes.data[existing].node;
            valid = prior->size == size && prior->alignment == alignment
                && analysis->static_graph_nodes.data[existing].typehash
                    == typehash
                && analysis->static_graph_nodes.data[existing].immutable
                    == (flags == 1)
                && !memcmp(prior->bytes, bytes, (size_t)size);
            if (valid && symbol) {
                bool named = prior->symbol && !strcmp(prior->symbol, symbol);
                for (size_t a = 0; !named && a < prior->alias_count; a++)
                    named = !strcmp(prior->aliases[a], symbol);
                if (!named) {
                    const char **aliases = core_realloc(
                        (void *)prior->aliases,
                        (prior->alias_count + 1) * sizeof(*aliases));
                    valid = aliases != nullptr;
                    if (valid) {
                        aliases[prior->alias_count++] = symbol;
                        prior->aliases = aliases;
                        symbol = nullptr;
                    }
                }
            }
            core_free(symbol);
            core_free(bytes);
            indices[i] = existing;
            continue;
        }
        if (!valid) {
            core_free(bytes);
            break;
        }
        indices[i] = analysis->static_graph_nodes.len;
        core_buffer_t alias_name = {0};
        core_buffer_printf(&alias_name, "__n00bb_graph_node_%zu", indices[i]);
        char *alias = alias_name.data
            ? core_strndup(alias_name.data, alias_name.byte_len) : nullptr;
        core_buffer_release(&alias_name);
        const char **aliases = alias
            ? core_alloc_array(const char *, 1) : nullptr;
        if (!aliases) {
            core_free(alias);
            core_free(symbol);
            core_free(bytes);
            valid = false;
            break;
        }
        aliases[0] = alias;
        core_list_push(analysis->static_graph_nodes,
            ((n00bb_captured_graph_node_t){
                .node = {.bytes = bytes, .size = (size_t)size,
                         .alignment = alignment, .symbol = symbol,
                         .aliases = aliases, .alias_count = 1},
                .address = address,
                .typehash = typehash,
                .immutable = flags == 1,
            }));
    }
    uint64_t edge_count = 0;
    if (valid) valid = n00bb_read_exact(fd, &edge_count,
                                        sizeof(edge_count));
    off_t position = valid ? lseek(fd, 0, SEEK_CUR) : -1;
    valid = valid && position >= 0 && position <= file_size
        && edge_count <= (uint64_t)(file_size - position)
            / (sizeof(uint64_t) * 4);
    for (uint64_t i = 0; valid && i < edge_count; i++) {
        uint64_t fields[4];
        valid = n00bb_read_exact(fd, fields, sizeof(fields))
            && fields[0] < count && fields[2] < count
            && fields[1] <= SIZE_MAX && fields[3] <= SIZE_MAX;
        if (!valid) break;
        core_list_push(analysis->static_graph_edges,
            ((n00bb_static_graph_edge_t){
                .source = indices[fields[0]],
                .source_offset = (size_t)fields[1],
                .target = indices[fields[2]],
                .target_offset = (size_t)fields[3],
            }));
    }
    if (valid) binding->graph_root_index = indices[0];
    core_free(indices);
    if (!valid) return false;
    binding->materialized_bytes = core_alloc_size(1, 1);
    if (!binding->materialized_bytes) return false;
    binding->materialized_kind = N00BB_LLVM_LITERAL_GRAPH;
    binding->materialized_count = 1;
    binding->materialized_stride = producer->element_stride;
    binding->owns_materialized = true;
    return true;
}

static bool
n00bb_capture_literal_results(int fd, n00bb_link_analysis_t *analysis,
    const n00bb_llvm_array_producer_t *producers,
    const size_t *producer_bindings, size_t producer_count,
    size_t *completed,
    uint8_t **failure_bytes, size_t *failure_length)
{
    *completed = 0;
    *failure_bytes = nullptr;
    *failure_length = 0;
    struct stat info;
    if (lseek(fd, 0, SEEK_SET) < 0 || fstat(fd, &info)
        || info.st_size < 0) return false;
    for (size_t i = 0; i < producer_count; i++) {
        n00bb_phase_result_binding_t *binding =
            &analysis->phase_results.data[producer_bindings[i]];
        uint64_t count;
        if (!n00bb_read_exact(fd, &count, sizeof(count))) return false;
        if (count == UINT64_MAX) {
            uint64_t length;
            uint8_t encoding;
            if (!n00bb_read_exact(fd, &length, sizeof(length))
                || !n00bb_read_exact(fd, &encoding, sizeof(encoding))
                || encoding > N00B_ABI_STRING_UTF8
                || length > SIZE_MAX - 1) return false;
            uint8_t *message = core_alloc_size(1, (size_t)length + 1);
            if (!message || !n00bb_read_exact(fd, message,
                                                (size_t)length)) {
                core_free(message);
                return false;
            }
            message[length] = 0;
            *failure_bytes = message;
            *failure_length = (size_t)length;
            return false;
        }
        if (producers[i].kind == N00BB_LLVM_LITERAL_EFFECT) {
            if (count) return false;
            binding->materialized_kind = N00BB_LLVM_LITERAL_EFFECT;
            binding->owns_materialized = true;
            *completed = i + 1;
            continue;
        }
        if (producers[i].kind == N00BB_LLVM_LITERAL_GRAPH) {
            if (!n00bb_capture_static_graph(fd, info.st_size, count,
                    &producers[i], analysis, binding)) return false;
            *completed = i + 1;
            continue;
        }
        if (producers[i].kind == N00BB_LLVM_LITERAL_RECORD_FIELDS_ARRAY
            || producers[i].kind == N00BB_LLVM_LITERAL_RECORD_FIELDS_RELOC) {
            uint32_t record_size = producers[i].record_size;
            uint64_t array_count = 0;
            off_t start = lseek(fd, 0, SEEK_CUR);
            if (count != 1 || !record_size || start < 0
                || start > info.st_size
                || (uint64_t)(info.st_size - start)
                    < (uint64_t)record_size + sizeof(array_count))
                return false;
            core_buffer_t *packed = core_buffer_empty();
            if (!packed) return false;
            core_buffer_ensure(packed, (size_t)record_size + 1);
            bool valid = n00bb_read_exact(fd, packed->data, record_size)
                && n00bb_read_exact(fd, &array_count,
                                    sizeof(array_count));
            if (valid) {
                packed->byte_len = record_size;
                packed->data[packed->byte_len] = 0;
                valid = n00bb_capture_array_elements(fd, info.st_size,
                    array_count, producers[i].element_stride,
                    &producers[i], packed);
            }
            if (!valid) {
                core_buffer_free(packed);
                return false;
            }
            binding->materialized_byte_len = packed->byte_len;
            binding->materialized_bytes = (uint8_t *)core_buffer_take(packed);
            binding->materialized_count = array_count;
            binding->materialized_stride = producers[i].element_stride;
            binding->materialized_kind = producers[i].kind;
            binding->owns_materialized = true;
            if (producers[i].kind == N00BB_LLVM_LITERAL_RECORD_FIELDS_RELOC) {
                size_t fields = producers[i].string_offset_count;
                uint32_t *offsets = core_alloc_array(uint32_t, fields);
                if (!offsets) return false;
                memcpy(offsets, producers[i].string_offsets,
                       fields * sizeof(*offsets));
                binding->materialized_string_offsets = offsets;
                binding->materialized_string_offset_count = fields;
            }
            *completed = i + 1;
            continue;
        }
        if (producers[i].kind == N00BB_LLVM_LITERAL_NESTED_ARRAY
            || producers[i].kind == N00BB_LLVM_LITERAL_NESTED_RELOC) {
            bool reloc = producers[i].kind
                == N00BB_LLVM_LITERAL_NESTED_RELOC;
            uint32_t stride = producers[i].inner_stride;
            size_t fields = producers[i].string_offset_count;
            off_t start = lseek(fd, 0, SEEK_CUR);
            if (!stride || start < 0 || start > info.st_size
                || (reloc && (!fields || !producers[i].string_offsets))
                || count > (uint64_t)(info.st_size - start) / sizeof(uint64_t))
                return false;
            core_buffer_t *packed = core_buffer_empty();
            bool valid = packed != nullptr;
            for (uint64_t j = 0; valid && j < count; j++) {
                uint64_t child_count;
                valid = n00bb_read_exact(fd, &child_count,
                                          sizeof(child_count));
                off_t position = valid ? lseek(fd, 0, SEEK_CUR) : -1;
                valid = valid && position >= 0 && position <= info.st_size
                    && packed->byte_len <= SIZE_MAX
                        - sizeof(child_count) - 1
                    && child_count <= (uint64_t)(info.st_size - position)
                        / stride
                    && child_count <= (SIZE_MAX - sizeof(child_count)
                        - packed->byte_len - 1) / stride;
                if (!valid) break;
                core_buffer_append(packed, (const char *)&child_count,
                                   sizeof(child_count));
                for (uint64_t k = 0; valid && k < child_count; k++) {
                    if (packed->byte_len > SIZE_MAX - stride - 1) {
                        valid = false;
                        break;
                    }
                    core_buffer_ensure(packed, stride + 1);
                    valid = n00bb_read_exact(fd,
                        packed->data + packed->byte_len, stride);
                    if (!valid) break;
                    packed->byte_len += stride;
                    packed->data[packed->byte_len] = 0;
                    for (size_t f = 0; valid && reloc && f < fields; f++)
                        valid = n00bb_capture_string_payload(fd,
                            info.st_size, packed);
                }
            }
            if (!valid) {
                core_buffer_free(packed);
                return false;
            }
            binding->materialized_byte_len = packed->byte_len;
            binding->materialized_bytes = (uint8_t *)core_buffer_take(packed);
            binding->materialized_count = count;
            binding->materialized_stride = producers[i].element_stride;
            binding->materialized_inner_stride = stride;
            binding->materialized_kind = producers[i].kind;
            binding->owns_materialized = true;
            if (reloc) {
                uint32_t *offsets = core_alloc_array(uint32_t, fields);
                if (!offsets) return false;
                memcpy(offsets, producers[i].string_offsets,
                       fields * sizeof(*offsets));
                binding->materialized_string_offsets = offsets;
                binding->materialized_string_offset_count = fields;
            }
            *completed = i + 1;
            continue;
        }
        if (producers[i].kind == N00BB_LLVM_LITERAL_RELOC
            || producers[i].kind == N00BB_LLVM_LITERAL_RECORD_RELOC) {
            uint32_t stride = producers[i].element_stride;
            size_t fields = producers[i].string_offset_count;
            off_t start = lseek(fd, 0, SEEK_CUR);
            if (!stride || !fields || count > SIZE_MAX / stride
                || start < 0 || start > info.st_size
                || fields > (SIZE_MAX - stride)
                    / (sizeof(uint64_t) + sizeof(uint8_t))
                || count > (uint64_t)(info.st_size - start)
                    / (stride + fields
                        * (sizeof(uint64_t) + sizeof(uint8_t))))
                return false;
            core_buffer_t *packed = core_buffer_empty();
            bool valid = packed != nullptr;
            for (uint64_t j = 0; valid && j < count; j++) {
                if (packed->byte_len > SIZE_MAX - stride - 1) {
                    valid = false;
                    break;
                }
                core_buffer_ensure(packed, stride + 1);
                valid = n00bb_read_exact(fd, packed->data
                    + packed->byte_len, stride);
                if (!valid) break;
                packed->byte_len += stride;
                packed->data[packed->byte_len] = 0;
                for (size_t f = 0; valid && f < fields; f++)
                    valid = n00bb_capture_string_payload(fd, info.st_size,
                                                          packed);
            }
            if (!valid) {
                core_buffer_free(packed);
                return false;
            }
            uint32_t *offsets = core_alloc_array(uint32_t, fields);
            if (!offsets) {
                core_buffer_free(packed);
                return false;
            }
            memcpy(offsets, producers[i].string_offsets,
                   fields * sizeof(*offsets));
            binding->materialized_byte_len = packed->byte_len;
            binding->materialized_bytes = (uint8_t *)core_buffer_take(packed);
            binding->materialized_count = count;
            binding->materialized_stride = stride;
            binding->materialized_kind = producers[i].kind;
            binding->owns_materialized = true;
            binding->materialized_string_offsets = offsets;
            binding->materialized_string_offset_count = fields;
            *completed = i + 1;
            continue;
        }
        uint8_t encoding = 0;
        if (producers[i].kind == N00BB_LLVM_LITERAL_STRING
            && (!n00bb_read_exact(fd, &encoding, sizeof(encoding))
                || encoding > N00B_ABI_STRING_UTF8)) return false;
        off_t offset = lseek(fd, 0, SEEK_CUR);
        uint32_t stride = producers[i].element_stride;
        if (producers[i].kind == N00BB_LLVM_LITERAL_STRING) stride = 1;
        if (!stride || count > SIZE_MAX / stride || offset < 0
            || offset > info.st_size
            || count * stride > (uint64_t)(info.st_size - offset))
            return false;
        size_t length = (size_t)(count * stride);
        uint8_t *bytes = core_alloc_size(1, length ? length : 1);
        if (!bytes || !n00bb_read_exact(fd, bytes, length)) {
            core_free(bytes);
            return false;
        }
        binding->materialized_bytes = bytes;
        binding->materialized_byte_len = length;
        binding->materialized_count = count;
        binding->materialized_kind = producers[i].kind;
        binding->owns_materialized = true;
        binding->materialized_encoding = encoding;
        binding->materialized_stride = producers[i].element_stride;
        *completed = i + 1;
    }
    return lseek(fd, 0, SEEK_CUR) == info.st_size;
}

static bool
n00bb_fold_bound_in_shape(ir_shape_t *shape, parse_tree_t *expression,
                          uint64_t extent, bool *matched)
{
    for (uint32_t d = 0; shape && d < ir_shape_rank(shape); d++) {
        ir_shape_extent_t bound;
        if (!ir_shape_extent(shape, d, &bound)) return false;
        if (bound.kind != IR_SHAPE_EXTENT_COMPTIME
            || bound.expression != expression) continue;
        if (!ir_shape_fold_comptime_extent(shape, d, extent)) return false;
        *matched = true;
    }
    return true;
}

static bool
n00bb_fold_array_bound_results(n00bb_link_analysis_t *analysis)
{
    for (size_t w = 0; w < analysis->array_bound_work.len; w++) {
        n00bb_array_bound_work_t *work = &analysis->array_bound_work.data[w];
        n00bb_phase_result_binding_t *result = nullptr;
        for (size_t r = 0; r < analysis->phase_results.len; r++)
            if (analysis->phase_results.data[r].producer == work->producer
                && analysis->phase_results.data[r].owns_materialized) {
                result = &analysis->phase_results.data[r];
                break;
            }
        uint64_t extent = 0;
        if (!result || result->materialized_kind
                != N00BB_LLVM_LITERAL_RAW_VALUE
            || result->materialized_byte_len != sizeof(extent)
            || result->materialized_count != 1) return false;
        memcpy(&extent, result->materialized_bytes, sizeof(extent));
        if (!extent) return false;

        bool matched = false;
        for (size_t u = 0; u < analysis->units.len; u++) {
            n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
            parse_symtab_t *table = state && state->annotations
                ? state->annotations->symtab : nullptr;
            for (parse_scope_t *scope = table ? table->all_scopes : nullptr;
                 scope; scope = scope->all_next)
                for (parse_sym_entry_t *symbol = scope->first_in_scope;
                     symbol; symbol = symbol->next_in_scope) {
                    const char *keys[] = {
                        "ir.array-shape", "ir.return-shape",
                    };
                    for (size_t k = 0; k < sizeof(keys) / sizeof(*keys); k++) {
                        ir_shape_t *shape = parse_sym_binding(symbol,
                            core_string_borrow(keys[k], strlen(keys[k])));
                        if (!n00bb_fold_bound_in_shape(shape,
                                work->expression, extent, &matched))
                            return false;
                    }
                }
            ir_module_t *modules[] = {
                state ? state->comptime_ir_module : nullptr,
                state ? state->ir_module : nullptr,
            };
            for (size_t m = 0; m < sizeof(modules) / sizeof(*modules); m++) {
                ir_module_t *module = modules[m];
                for (size_t g = 0; module && g < ir_global_count(module); g++)
                    if (!n00bb_fold_bound_in_shape(ir_global_shape(
                            ir_global_at(module, g)), work->expression,
                            extent, &matched)) return false;
                for (size_t f = 0; module && f < ir_func_count(module); f++) {
                    ir_func_t *function = ir_func_at(module, f);
                    for (uint32_t p = 0; p < function->n_params; p++)
                        if (!n00bb_fold_bound_in_shape(ir_value_shape(
                                function->params[p]), work->expression,
                                extent, &matched)) return false;
                    for (uint32_t l = 0; l < function->n_locals; l++)
                        if (!n00bb_fold_bound_in_shape(ir_local_shape(
                                function->locals[l]), work->expression,
                                extent, &matched)) return false;
                    for (uint32_t b = 0; b < function->n_blocks; b++) {
                        ir_block_t *block = function->blocks[b];
                        for (uint32_t i = 0; i < block->n_instrs; i++) {
                            ir_instr_t *instruction = block->instrs[i];
                            if (!n00bb_fold_bound_in_shape(ir_value_shape(
                                    instruction->dest), work->expression,
                                    extent, &matched)) return false;
                        }
                    }
                }
            }
        }
        if (!matched) return false;
    }
    return true;
}

static bool
n00bb_decode_comptime_value(n00bb_link_analysis_t *analysis,
    ir_module_t *module, th_type *type, const uint8_t *raw, size_t raw_len,
    const uint8_t *packed, size_t packed_len, size_t *position,
    ir_global_init_value_t *out, size_t depth)
{
    type = type ? th_resolve(type) : nullptr;
    const ir_final_layout_t *layout = type
        ? ir_layout_final(module, type, ir_module_target(module)) : nullptr;
    if (!layout || !out || !raw || depth > 64
        || layout->intrinsic_bits > raw_len * 8) return false;
    const char *name = nullptr;
    size_t name_length = 0, arity = 0;
    bool named = th_named_info(type, &name, &name_length, nullptr, &arity);
    if (named && name_length == 6 && !memcmp(name, "string", 6)
        && !arity) {
        uint64_t length;
        if (!position || *position > packed_len
            || packed_len - *position < sizeof(length) + 1) return false;
        memcpy(&length, packed + *position, sizeof(length));
        *position += sizeof(length);
        uint8_t encoding = packed[(*position)++];
        if (encoding == N00B_ABI_STRING_BINARY
            || length > packed_len - *position || length > SIZE_MAX - 1)
            return false;
        char *bytes = ir_alloc(analysis->ir_context, (size_t)length + 1);
        if (!bytes) return false;
        memcpy(bytes, packed + *position, (size_t)length);
        bytes[length] = 0;
        *position += (size_t)length;
        *out = (ir_global_init_value_t){
            .kind = IR_GLOBAL_INIT_STRING,
            .string = {.bytes = bytes, .length = (size_t)length},
        };
        return true;
    }
    if (layout->n_members && layout->members) {
        bool tuple = named && name && !strcmp(name, TH_TUPLE);
        size_t count = layout->n_members;
        ir_global_init_value_t *items = ir_alloc(analysis->ir_context,
            count * sizeof(*items));
        if (!items) return false;
        for (size_t i = 0; i < count; i++) {
            const ir_final_member_layout_t *member = &layout->members[i];
            if (!member->type || member->bit_offset % 8
                || member->bit_offset / 8 > raw_len
                || !n00bb_decode_comptime_value(analysis, module,
                    member->type, raw + member->bit_offset / 8,
                    raw_len - member->bit_offset / 8, packed, packed_len,
                    position, &items[i], depth + 1)) return false;
            if (!tuple) items[i].member = member->name;
        }
        *out = (ir_global_init_value_t){
            .kind = tuple ? IR_GLOBAL_INIT_TUPLE : IR_GLOBAL_INIT_RECORD,
            .aggregate = {.items = items, .count = count},
        };
        return true;
    }
    ir_machine_t machine = ir_machine_of(module, type);
    if (machine.class == IR_CLASS_BOOL && raw_len
        && (raw[0] == 0 || raw[0] == 1)) {
        *out = (ir_global_init_value_t){
            .kind = IR_GLOBAL_INIT_BOOL, .boolean = raw[0] != 0};
        return true;
    }
    if (machine.class == IR_CLASS_FLOAT && machine.width == 64
        && raw_len >= sizeof(double)) {
        double floating;
        memcpy(&floating, raw, sizeof(floating));
        *out = (ir_global_init_value_t){
            .kind = IR_GLOBAL_INIT_FLOAT, .floating = floating};
        return true;
    }
    if ((machine.class == IR_CLASS_SINT || machine.class == IR_CLASS_UINT)
        && machine.width && machine.width <= 128
        && (machine.width + 7) / 8 <= raw_len) {
        ir_uint_t bits = 0;
        size_t bytes = (machine.width + 7) / 8;
        for (size_t i = 0; i < bytes; i++)
            bits |= (ir_uint_t)raw[i] << (i * 8);
        ir_uint_t mask = machine.width == 128 ? ~(ir_uint_t)0
            : ((ir_uint_t)1 << machine.width) - 1;
        bits &= mask;
        bool negative = machine.class == IR_CLASS_SINT
            && (bits & ((ir_uint_t)1 << (machine.width - 1))) != 0;
        *out = (ir_global_init_value_t){
            .kind = IR_GLOBAL_INIT_INT,
            .integer = {.mag = negative ? ((~bits + 1) & mask) : bits,
                        .neg = negative},
        };
        return true;
    }
    return false;
}

static bool
n00bb_fold_comptime_record_array(n00bb_link_analysis_t *analysis,
    n00bb_global_result_binding_t *binding,
    const n00bb_phase_result_binding_t *result)
{
    ir_module_t *module = binding->target_module;
    th_type *type = th_resolve(binding->target_global->type);
    const ir_final_layout_t *layout = ir_layout_final(module, type,
        ir_module_target(module));
    bool record_fields = result->materialized_kind
        == N00BB_LLVM_LITERAL_RECORD_FIELDS_ARRAY
        || result->materialized_kind
            == N00BB_LLVM_LITERAL_RECORD_FIELDS_RELOC;
    uint64_t record_bits = 0;
    if (!layout || !layout->n_members || !result->materialized_stride
        || (record_fields && (!ir_layout_final_stride_bits(layout,
                &record_bits) || record_bits % 8
            || record_bits / 8 > result->materialized_byte_len))
        || result->materialized_count > SIZE_MAX / sizeof(ir_global_init_value_t))
        return false;
    size_t record_size = record_fields ? (size_t)(record_bits / 8) : 0;
    size_t count = (size_t)result->materialized_count;
    ir_global_init_value_t *fields = ir_alloc(analysis->ir_context,
        layout->n_members * sizeof(*fields));
    if (!fields) return false;
    size_t position = record_size;
    size_t arrays = 0;
    for (size_t m = 0; m < layout->n_members; m++) {
        const ir_final_member_layout_t *member = &layout->members[m];
        th_type *field = member->type ? th_resolve(member->type) : nullptr;
        if (!member->name || !field) return false;
        if (th_kind_of(field) == TH_SEQUENCE) {
            if (++arrays != 1) return false;
            th_type *element = th_seq_elem(field);
            ir_global_init_value_t *items = ir_alloc(analysis->ir_context,
                (count ? count : 1) * sizeof(*items));
            if (!element || !items) return false;
            for (size_t i = 0; i < count; i++) {
                size_t stride = result->materialized_stride;
                if (position > result->materialized_byte_len
                    || stride > result->materialized_byte_len - position)
                    return false;
                const uint8_t *raw = result->materialized_bytes + position;
                position += stride;
                if (!n00bb_decode_comptime_value(analysis, module, element,
                        raw, stride, result->materialized_bytes,
                        result->materialized_byte_len, &position, &items[i], 0))
                    return false;
            }
            fields[m] = (ir_global_init_value_t){
                .member = member->name,
                .kind = IR_GLOBAL_INIT_SEQUENCE,
                .aggregate = {.items = items, .count = count},
            };
        } else {
            const ir_final_layout_t *field_layout = ir_layout_final(module,
                field, ir_module_target(module));
            uint64_t field_bits = 0;
            size_t scalar_position = 0;
            if (!record_fields || !field_layout
                || !ir_layout_final_stride_bits(field_layout, &field_bits)
                || field_bits % 8 || member->bit_offset % 8
                || member->bit_offset / 8 > record_size
                || field_bits / 8
                    > record_size - member->bit_offset / 8
                || !n00bb_decode_comptime_value(analysis, module, field,
                    result->materialized_bytes + member->bit_offset / 8,
                    (size_t)(field_bits / 8),
                    result->materialized_bytes + member->bit_offset / 8,
                    (size_t)(field_bits / 8), &scalar_position,
                    &fields[m], 0)) return false;
            fields[m].member = member->name;
        }
    }
    if (arrays != 1 || position != result->materialized_byte_len)
        return false;
    ir_global_init_value_t record = {
        .kind = IR_GLOBAL_INIT_RECORD,
        .aggregate = {.items = fields, .count = layout->n_members},
    };
    return ir_global_materialize_phase_result(module,
        binding->target_global, binding->identifier, &record);
}

static bool
n00bb_fold_comptime_global_results(n00bb_link_analysis_t *analysis)
{
    for (size_t g = 0; g < analysis->global_results.len; g++) {
        n00bb_global_result_binding_t *binding =
            &analysis->global_results.data[g];
        n00bb_phase_result_binding_t *result = nullptr;
        for (size_t r = 0; r < analysis->phase_results.len; r++)
            if (analysis->phase_results.data[r].producer
                    == binding->producer
                && analysis->phase_results.data[r].owns_materialized) {
                result = &analysis->phase_results.data[r];
                break;
            }
        if (!result || !result->materialized_bytes) return false;
        if (result->materialized_kind == N00BB_LLVM_LITERAL_GRAPH) {
            if (result->materialized_count != 1
                || result->graph_root_index >= analysis->static_graph_nodes.len
                || analysis->static_graph_nodes.data[result->graph_root_index]
                    .typehash != th_hash(ir_module_types(
                        binding->target_module), binding->target_global->type))
                return false;
            continue;
        }
        if (result->materialized_kind == N00BB_LLVM_LITERAL_RECORD_ARRAY
            || result->materialized_kind == N00BB_LLVM_LITERAL_RECORD_RELOC
            || result->materialized_kind
                == N00BB_LLVM_LITERAL_RECORD_FIELDS_ARRAY
            || result->materialized_kind
                == N00BB_LLVM_LITERAL_RECORD_FIELDS_RELOC) {
            if (!n00bb_fold_comptime_record_array(analysis, binding,
                                                   result)) return false;
            continue;
        }
        if (result->materialized_count != 1
            || result->materialized_kind != N00BB_LLVM_LITERAL_RAW_VALUE)
            return false;

        th_type *value_type = binding->target_global->type;
        th_type *atomic_element = n00bb_atomic_element_type(value_type);
        ir_machine_t machine = ir_machine_of(binding->target_module,
            atomic_element ? atomic_element : value_type);
        size_t bytes = result->materialized_byte_len;
        ir_global_init_value_t value = {0};
        if (n00bb_phase_zero_option_type(value_type)) {
            for (size_t i = 0; i < bytes; i++)
                if (result->materialized_bytes[i]) return false;
            value.kind = IR_GLOBAL_INIT_NIL;
        }
        else if (machine.class == IR_CLASS_BOOL && bytes == 1) {
            if (result->materialized_bytes[0] > 1) return false;
            value.kind = IR_GLOBAL_INIT_BOOL;
            value.boolean = result->materialized_bytes[0] != 0;
        }
        else if (machine.class == IR_CLASS_FLOAT && machine.width == 64
                 && bytes == sizeof(double)) {
            value.kind = IR_GLOBAL_INIT_FLOAT;
            memcpy(&value.floating, result->materialized_bytes,
                   sizeof(value.floating));
        }
        else if ((machine.class == IR_CLASS_SINT
                  || machine.class == IR_CLASS_UINT)
                 && machine.width && machine.width <= 128
                 && machine.width % 8 == 0
                 && bytes == machine.width / 8) {
            ir_uint_t bits = 0;
            for (size_t i = 0; i < bytes; i++)
                bits |= (ir_uint_t)result->materialized_bytes[i] << (i * 8);
            ir_uint_t mask = machine.width == 128
                ? ~(ir_uint_t)0
                : ((ir_uint_t)1 << machine.width) - 1;
            bits &= mask;
            bool negative = machine.class == IR_CLASS_SINT
                && (bits & ((ir_uint_t)1 << (machine.width - 1))) != 0;
            value.kind = IR_GLOBAL_INIT_INT;
            value.integer = (ir_lit_t){
                .mag = negative ? ((~bits + 1) & mask) : bits,
                .neg = negative,
            };
        }
        else return false;
        if (!ir_global_materialize_phase_result(binding->target_module,
                binding->target_global, binding->identifier, &value))
            return false;
    }
    return true;
}

bool
n00bb_finalize_comptime_layout(n00bb_link_analysis_t *analysis)
{
    if (!analysis->layout_deferred) return true;
    if (n00bb_has_deferred_shape_layout(analysis, true)) return false;
    if (!select_storage_placement(analysis)) return false;
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        ir_module_t *modules[] = {
            state->comptime_ir_module, state->ir_module,
        };
        ir_diags_t *diagnostics = ir_diags_new_attached(
            analysis->ir_context, nullptr,
            core_translation_unit_diagnostics(unit), unit);
        for (size_t m = 0; m < sizeof(modules) / sizeof(*modules); m++) {
            ir_props_t unmet = 0;
            if (ir_pass_run(&ir_pass_layout, modules[m], nullptr,
                    &unmet, diagnostics) != IR_PASS_OK)
                return false;
        }
    }
    analysis->layout_deferred = false;
    return true;
}

bool
n00bb_execute_comptime(core_link_unit_t *link,
                        n00bb_link_analysis_t *analysis, bool debug_enabled,
                        bool native_cpu,
                        const char *compiler_path,
                        core_diagnostics_t *diagnostics)
{
    core_list_t(core_translation_unit_ptr_t) order =
        core_list_new(core_translation_unit_ptr_t);
    bool ok = n00bb_collect_initializer_order(link,
        "n00bb.comptime-module-initializer-order", &order);
    if (!ok) {
        core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-B0010"),
            CORE_STRING_STATIC("compile-time initializer order is unavailable"));
        core_list_free(order);
        return false;
    }
    bool has_work = analysis->phase_results.len != 0;
    for (size_t i = 0; i < order.len; i++) {
        n00bb_tu_state_t *state = tu_state(order.data[i]);
        if (!state || !state->comptime_ir_module || !state->ir_module) {
            ok = false;
            break;
        }
        if (!n00bb_comptime_is_empty(state->comptime_ir_module))
            has_work = true;
    }
    if (!ok || !has_work) {
        core_list_free(order);
        return ok;
    }
    size_t count = order.len;
    char **names = calloc(count, sizeof(*names));
    char **objects = calloc(count + 1, sizeof(*objects));
    size_t binding_count = analysis->phase_results.len;
    size_t *producer_bindings = binding_count
        ? calloc(binding_count, sizeof(*producer_bindings)) : nullptr;
    size_t producer_count = 0;
    for (size_t p = 0; producer_bindings
         && p < analysis->literal_producers.len; p++) {
        ir_func_t *producer = analysis->literal_producers.data[p];
        for (size_t i = 0; i < binding_count; i++) {
            if (analysis->phase_results.data[i].producer != producer)
                continue;
            producer_bindings[producer_count++] = i;
            break;
        }
    }
    for (size_t i = 0; producer_bindings && i < binding_count; i++) {
        ir_func_t *producer = analysis->phase_results.data[i].producer;
        bool seen = false;
        for (size_t j = 0; j < producer_count; j++)
            seen |= analysis->phase_results.data[
                producer_bindings[j]].producer == producer;
        if (!seen) producer_bindings[producer_count++] = i;
    }
    n00bb_llvm_array_producer_t *producers = producer_count
        ? calloc(producer_count, sizeof(*producers)) : nullptr;
    ok = names && objects && (!binding_count || producer_bindings)
        && (!producer_count || producers);
    n00bb_abi_comptime_type_t *catalog_types = nullptr;
    n00bb_llvm_type_index_t *comptime_type_indices = nullptr;
    size_t catalog_type_count = 0;
    size_t catalog_metadata_bytes = 0;
    uint8_t *catalog_object = nullptr;
    size_t catalog_object_bytes = 0;
    const char *catalog_failure = nullptr;
    if (ok) ok = n00bb_emit_link_comptime_catalog(link, analysis,
            &catalog_types, &catalog_type_count, &catalog_object,
            &catalog_object_bytes, &catalog_metadata_bytes,
            &catalog_failure);
    if (ok) {
        comptime_type_indices = calloc(catalog_type_count,
            sizeof(*comptime_type_indices));
        ok = comptime_type_indices != nullptr;
        for (size_t i = 0; ok && i < catalog_type_count; i++)
            comptime_type_indices[i] = (n00bb_llvm_type_index_t){
                .typehash = catalog_types[i].typehash,
                .index = catalog_types[i].index,
            };
    }
    if (ok) objects[count] = n00bb_write_temporary_object(
        catalog_object, catalog_object_bytes);
    ok = ok && objects[count];
    core_free(catalog_object);
    core_free(catalog_types);
    size_t initializer_count = 0;
    for (size_t i = 0; ok && i < count; i++) {
        n00bb_tu_state_t *state = tu_state(order.data[i]);
        if (!ir_func_find(state->comptime_ir_module,
                          "__n00bb_comptime_init")) continue;
        core_buffer_t name = {0};
        core_buffer_printf(&name, "%s.__n00bb_comptime_init",
            ir_module_name(state->comptime_ir_module));
        names[initializer_count] = core_strndup(name.data, name.byte_len);
        core_buffer_release(&name);
        ok = names[initializer_count++] != nullptr;
    }
    for (size_t i = 0; ok && i < producer_count; i++) {
        n00bb_phase_result_binding_t *binding =
            &analysis->phase_results.data[producer_bindings[i]];
        ir_func_t *producer = binding->producer;
        th_type *result = producer ? th_resolve(th_func_ret(producer->type))
                                   : nullptr;
        bool effect_only = result && ir_machine_of(producer->owner,
            result).class == IR_CLASS_VOID;
        const char *result_name = nullptr;
        size_t result_name_length = 0;
        size_t result_arity = 0;
        bool is_string = result && th_named_info(result, &result_name,
            &result_name_length, nullptr, &result_arity)
            && result_name_length == 6
            && !memcmp(result_name, "string", 6) && !result_arity;
        th_type *element = result && th_kind_of(result) == TH_SEQUENCE
            ? th_seq_elem(result) : nullptr;
        const ir_final_layout_t *record_layout = result
            ? ir_layout_final(producer->owner, result,
                ir_module_target(producer->owner)) : nullptr;
        th_type *record_element = nullptr;
        if (!element && record_layout && record_layout->n_members == 1
            && record_layout->members[0].bit_offset == 0
            && record_layout->members[0].type
            && th_kind_of(th_resolve(record_layout->members[0].type))
                == TH_SEQUENCE
            && record_layout->intrinsic_bits
                == sizeof(n00b_abi_array_value_v1) * 8)
            record_element = th_seq_elem(th_resolve(
                record_layout->members[0].type));
        th_type *record_fields_element = nullptr;
        uint32_t record_size = 0, array_offset = 0, array_index = 0;
        bool record_fields = !element && !record_element && result
            && n00bb_literal_record_array_fields(producer->owner, result,
                &record_fields_element, &record_size, &array_offset,
                &array_index);
        if (!record_fields) record_fields_element = nullptr;
        th_type *raw_type = element ? element
            : record_element ? record_element
            : record_fields_element ? record_fields_element : result;
        core_list_t(uint32_t) string_offsets = core_list_new(uint32_t);
        bool reloc_result = !is_string && raw_type
            && n00bb_literal_string_offsets(producer->owner, raw_type, 0,
                                             &string_offsets, false)
            && string_offsets.len != 0;
        th_type *inner_element = element && th_kind_of(th_resolve(element))
            == TH_SEQUENCE ? th_seq_elem(element) : nullptr;
        const ir_final_layout_t *inner_layout = inner_element
            ? ir_layout_final(producer->owner, inner_element,
                              ir_module_target(producer->owner)) : nullptr;
        core_list_t(uint32_t) inner_string_offsets =
            core_list_new(uint32_t);
        bool nested_reloc = inner_element
            && n00bb_literal_string_offsets(producer->owner,
                inner_element, 0, &inner_string_offsets, false)
            && inner_string_offsets.len != 0;
        uint64_t inner_stride_bits = 0;
        bool nested_array = inner_element
            && (n00bb_literal_raw_element(producer->owner, inner_element)
                || nested_reloc)
            && ir_layout_final_stride_bits(inner_layout,
                                           &inner_stride_bits)
            && !(inner_stride_bits % 8) && inner_stride_bits / 8 <= UINT32_MAX;
        bool bool_value = !element && !effect_only && !is_string
            && ir_machine_of(producer->owner, result).class == IR_CLASS_BOOL;
        bool zero_option_result = !element
            && n00bb_phase_zero_option_type(result);
        bool raw_element = raw_type
            && n00bb_literal_raw_element(producer->owner, raw_type);
        bool graph_result = !effect_only && !is_string && !element
            && !record_element && !record_fields && !reloc_result
            && result && !raw_element;
        if (!effect_only && !is_string && !reloc_result && !nested_array
            && (element || record_element || record_fields) && !raw_element)
            graph_result = true;
        const ir_final_layout_t *layout = graph_result ? record_layout
            : raw_type ? ir_layout_final(producer->owner, raw_type,
                                        ir_module_target(producer->owner))
                       : nullptr;
        uint64_t stride_bits = 0;
        bool has_stride = ir_layout_final_stride_bits(layout, &stride_bits);
        uint64_t stride = bool_value ? 1
            : has_stride ? stride_bits / 8 : 0;
        if (!effect_only && !is_string
            && ((!has_stride && !bool_value) || !stride
                || (has_stride && stride_bits % 8)
                || stride > UINT32_MAX
                || string_offsets.len > UINT32_MAX
                || (!bool_value && !reloc_result && !nested_array
                    && !graph_result
                    && !zero_option_result
                    && !raw_element))) {
            core_list_free(string_offsets);
            core_list_free(inner_string_offsets);
            core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_FATAL,
                CORE_STRING_STATIC("N00BB-B0013"),
                CORE_STRING_STATIC("native build cannot materialize this literal result without relocations"));
            ok = false;
            break;
        }
        core_buffer_t symbol = {0};
        core_buffer_printf(&symbol, "%s.%s",
            ir_module_name(producer->owner), producer->name);
        producers[i].symbol = core_strndup(symbol.data, symbol.byte_len);
        producers[i].identifier = binding->identifier;
        producers[i].function = producer;
        producers[i].after_initializers = true;
        for (size_t p = 0; p < analysis->literal_producers.len; p++)
            if (analysis->literal_producers.data[p] == producer) {
                producers[i].after_initializers = false;
                break;
            }
        producers[i].kind = effect_only ? N00BB_LLVM_LITERAL_EFFECT
            : is_string ? N00BB_LLVM_LITERAL_STRING
            : graph_result ? N00BB_LLVM_LITERAL_GRAPH
            : record_element && reloc_result
                ? N00BB_LLVM_LITERAL_RECORD_RELOC
            : record_element ? N00BB_LLVM_LITERAL_RECORD_ARRAY
            : record_fields && reloc_result
                ? N00BB_LLVM_LITERAL_RECORD_FIELDS_RELOC
            : record_fields ? N00BB_LLVM_LITERAL_RECORD_FIELDS_ARRAY
            : reloc_result ? N00BB_LLVM_LITERAL_RELOC
            : nested_array && nested_reloc
                ? N00BB_LLVM_LITERAL_NESTED_RELOC
            : nested_array ? N00BB_LLVM_LITERAL_NESTED_ARRAY
            : element ? N00BB_LLVM_LITERAL_ARRAY
                      : N00BB_LLVM_LITERAL_RAW_VALUE;
        producers[i].element_stride = effect_only || is_string
            ? 0 : (uint32_t)stride;
        if (record_fields) {
            producers[i].record_size = record_size;
            producers[i].array_field_offset = array_offset;
            producers[i].array_field_index = array_index;
        }
        if (nested_array)
            producers[i].inner_stride = (uint32_t)(inner_stride_bits / 8);
        if (reloc_result) {
            producers[i].string_offsets = string_offsets.data;
            producers[i].string_offset_count = string_offsets.len;
            producers[i].byte_mask = n00bb_literal_byte_mask(layout,
                                                              stride_bits);
            ok = ok && producers[i].byte_mask != nullptr;
        }
        else core_list_free(string_offsets);
        if (nested_array && nested_reloc) {
            producers[i].string_offsets = inner_string_offsets.data;
            producers[i].string_offset_count = inner_string_offsets.len;
        }
        else core_list_free(inner_string_offsets);
        if (nested_array) {
            producers[i].byte_mask = n00bb_literal_byte_mask(inner_layout,
                                                       inner_stride_bits);
            ok = ok && producers[i].byte_mask != nullptr;
        }
        if ((reloc_result || nested_array) && !producers[i].byte_mask)
            core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_FATAL,
                CORE_STRING_STATIC("N00BB-B0013"),
                CORE_STRING_STATIC("native build cannot materialize a bit-packed literal result"));
        for (size_t b = 0; b < binding_count; b++)
            if (analysis->phase_results.data[b].producer == producer
                && analysis->phase_results.data[b].consumer
                && ir_func_execution_phase(
                    analysis->phase_results.data[b].consumer)
                    == IR_PHASE_COMPTIME)
                producers[i].comptime_consumer = true;
        core_buffer_release(&symbol);
        ok = ok && producers[i].symbol != nullptr;
    }
    for (size_t i = 0; ok && i < count; i++) {
        n00bb_tu_state_t *state = tu_state(order.data[i]);
        n00bb_llvm_backend_t *backend = n00bb_llvm_backend_prepare(
            state->comptime_ir_module, state->ir_module,
            debug_enabled, native_cpu,
            core_translation_unit_diagnostics(order.data[i]));
        if (!backend) {
            ok = false;
            break;
        }
        ok = n00bb_llvm_backend_bind_reachable_functions(backend, true,
            analysis->comptime_reachable_functions.data,
            analysis->comptime_reachable_functions.len);
        if (ok) ok = n00bb_llvm_backend_bind_type_indices(backend,
            comptime_type_indices, catalog_type_count);
        core_buffer_t object = {0};
        bool is_root = order.data[i] == core_link_unit_root(link, 0);
        if (ok) ok = n00bb_llvm_backend_emit_object(backend, true,
            is_root ? (const char *const *)names : nullptr,
            is_root ? initializer_count : 0,
            is_root ? producers : nullptr,
            is_root ? producer_count : 0,
            is_root ? catalog_metadata_bytes : 0,
            &object, diagnostics);
        n00bb_llvm_backend_free(backend);
        if (ok) objects[i] = n00bb_write_temporary_object(
            object.data, object.byte_len);
        core_buffer_release(&object);
        ok = ok && objects[i];
    }
    char executable_path[] = "/tmp/n00bb-comptime-XXXXXX";
    bool path_created = false;
    if (ok) {
        int fd = mkstemp(executable_path);
        path_created = fd >= 0;
        if (path_created) {
            close(fd);
            unlink(executable_path);
        }
        ok = path_created;
    }
    if (ok) {
#ifdef __APPLE__
        const char *linker = "/usr/bin/clang";
#else
        const char *linker = "clang";
#endif
        char *archive = n00bb_archive_path(compiler_path,
            "libn00b-abi-runtime.a");
        char *comptime_archive = n00bb_archive_path(compiler_path,
            "libn00b-comptime-allocate.a");
        char **arguments = calloc(count + 7,
                                  sizeof(*arguments));
        ok = arguments && archive && comptime_archive;
        if (ok) {
            arguments[0] = (char *)linker;
            for (size_t i = 0; i < count; i++)
                arguments[i + 1] = objects[i];
            size_t next = count + 1;
            arguments[next++] = objects[count];
            arguments[next++] = comptime_archive;
            arguments[next++] = archive;
            arguments[next++] = "-o";
            arguments[next] = executable_path;
            pid_t child = 0;
            int launched = posix_spawnp(&child, linker, nullptr, nullptr,
                                        arguments, environ);
            int status = 0;
            ok = !launched && waitpid(child, &status, 0) == child
                && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        free(arguments);
        core_free(archive);
        core_free(comptime_archive);
    }
    int result_fd = -1;
    if (ok && producer_count) {
        char result_path[] = "/tmp/n00bb-comptime-results-XXXXXX";
        result_fd = mkstemp(result_path);
        if (result_fd >= 0) unlink(result_path);
        ok = result_fd >= 0;
    }
    bool comptime_ran = false;
    bool comptime_succeeded = false;
    int execution_status = 0;
    if (ok) {
        char *arguments[] = {executable_path, nullptr};
        pid_t child = 0;
        posix_spawn_file_actions_t actions;
        bool actions_ready = producer_count
            && !posix_spawn_file_actions_init(&actions);
        ok = !producer_count || (actions_ready
            && !posix_spawn_file_actions_adddup2(&actions, result_fd,
                                                 N00BB_COMPTIME_RESULT_FD));
        int launched = ok ? posix_spawn(&child, executable_path,
            actions_ready ? &actions : nullptr, nullptr, arguments, environ)
            : -1;
        if (actions_ready) posix_spawn_file_actions_destroy(&actions);
        comptime_ran = ok && !launched;
        ok = comptime_ran
            && waitpid(child, &execution_status, 0) == child
            && WIFEXITED(execution_status)
            && WEXITSTATUS(execution_status) == 0;
        comptime_succeeded = ok;
    }
    size_t completed = 0;
    uint8_t *failure_bytes = nullptr;
    size_t failure_length = 0;
    bool captured = comptime_ran && producer_count
        && n00bb_capture_literal_results(result_fd, analysis, producers,
            producer_bindings, producer_count, &completed, &failure_bytes,
            &failure_length);
    if (captured) {
        for (size_t i = 0; i < binding_count; i++) {
            n00bb_phase_result_binding_t *binding =
                &analysis->phase_results.data[i];
            if (binding->owns_materialized) continue;
            for (size_t j = 0; j < producer_count; j++) {
                n00bb_phase_result_binding_t *source =
                    &analysis->phase_results.data[producer_bindings[j]];
                if (source->producer != binding->producer) continue;
                binding->materialized_bytes = source->materialized_bytes;
                binding->materialized_byte_len =
                    source->materialized_byte_len;
                binding->materialized_string_offsets =
                    source->materialized_string_offsets;
                binding->materialized_string_offset_count =
                    source->materialized_string_offset_count;
                binding->materialized_count = source->materialized_count;
                binding->materialized_stride = source->materialized_stride;
                binding->materialized_inner_stride =
                    source->materialized_inner_stride;
                binding->materialized_kind = source->materialized_kind;
                binding->materialized_encoding =
                    source->materialized_encoding;
                break;
            }
        }
        if (!n00bb_fold_array_bound_results(analysis)) {
            core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_FATAL,
                CORE_STRING_STATIC("N00BB-B0013"),
                CORE_STRING_STATIC(
                    "compile-time array bound could not be folded into its shape"));
            ok = false;
        }
        if (ok && !n00bb_fold_comptime_global_results(analysis)) {
            core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_FATAL,
                CORE_STRING_STATIC("N00BB-B0013"),
                CORE_STRING_STATIC(
                    "compile-time global could not be folded into its runtime initializer"));
            ok = false;
        }
    }
    if (comptime_ran && !comptime_succeeded) {
        n00bb_phase_result_binding_t *binding = failure_bytes
            && completed < producer_count
            ? &analysis->phase_results.data[
                producer_bindings[completed]] : nullptr;
        core_translation_unit_t *unit = nullptr;
        if (binding && binding->consumer)
            for (size_t i = 0; i < analysis->units.len; i++) {
                core_translation_unit_t *candidate = analysis->units.data[i];
                n00bb_tu_state_t *state = tu_state(candidate);
                if (state && state->ir_module == binding->consumer->owner) {
                    unit = candidate;
                    break;
                }
            }
        ir_span_t span = binding ? (binding->instruction
            ? binding->instruction->span : binding->span) : (ir_span_t){0};
        core_buffer_t failure_message = {0};
        if (!failure_length) {
            if (WIFSIGNALED(execution_status))
                core_buffer_printf(&failure_message,
                    "compile-time execution was terminated by signal %d after %zu/%zu results",
                    WTERMSIG(execution_status), completed, producer_count);
            else
                core_buffer_printf(&failure_message,
                    "compile-time execution exited with status %d after %zu/%zu results",
                    WIFEXITED(execution_status)
                        ? WEXITSTATUS(execution_status) : -1,
                    completed, producer_count);
        }
        core_diagnostics_report_at(diagnostics, unit, CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-B0014"),
            binding && failure_length
                ? core_string_borrow((char *)failure_bytes, failure_length)
                : core_string_borrow(failure_message.data,
                    failure_message.byte_len),
            (core_diag_location_t){
                .line = span.start_line, .column = span.start_col,
                .end_line = span.end_line, .end_column = span.end_col,
                .has_location = span.start_line != 0,
            });
        core_buffer_release(&failure_message);
    }
    if (ok && producer_count && !captured) {
        core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-B0013"),
            CORE_STRING_STATIC("compile-time literal results are incomplete or invalid"));
        ok = false;
    }
    core_free(failure_bytes);
    if (result_fd >= 0) close(result_fd);
    if (path_created) unlink(executable_path);
    for (size_t i = 0; i <= count; i++) {
        if (objects && objects[i]) {
            unlink(objects[i]);
            core_free(objects[i]);
        }
    }
    for (size_t i = 0; i < count; i++)
        if (names && names[i]) core_free(names[i]);
    free(objects);
    free(comptime_type_indices);
    free(names);
    for (size_t i = 0; i < producer_count; i++) {
        core_free((void *)producers[i].symbol);
        core_free((void *)producers[i].string_offsets);
        free((void *)producers[i].byte_mask);
    }
    free(producers);
    free(producer_bindings);
    core_list_free(order);
    if (!ok && !core_diagnostics_failed(diagnostics)) {
        core_buffer_t message = {0};
        core_buffer_printf(&message, "could not build compile-time executable%s%s",
            catalog_failure ? " at " : "",
            catalog_failure ? catalog_failure : "");
        core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-B0015"),
            core_string_borrow(message.data, message.byte_len));
        core_buffer_release(&message);
    }
    return ok;
}
