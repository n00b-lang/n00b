#include "abi_emit.h"

#include <limits.h>
#include <string.h>

#include "abi_container.h"
#include "abi_string.h"
#include "abi_constraint_rows.h"
#include "abi_decl_index.h"
#include "abi_declarations.h"
#include "abi_enum_rows.h"
#include "abi_function_rows.h"
#include "abi_layout.h"
#include "abi_storage.h"
#include "abi_emit_strings.h"
#include "abi_type_index.h"
#include "abi_type_layout_rows.h"
#include "abi_type_rows.h"
#include "abi_type_signatures.h"
#include "abi_trait_method_rows.h"
#include "frame_roots.h"
#include "n00b-abi-v1.h"
#include "th/environment.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/byteorder.h"
#include "lib/diagnostic.h"
#include "lib/dict.h"
#include "lib/hash.h"
#include "lib/indexed_table.h"
#include "lib/list.h"
#include "lib/object_section.h"
#include "ir/typelang.h"
#include "parse/tree_util.h"
#include "signature_typevars.h"

core_list_decl(core_object_emit_section_t);

static bool abi_unit_error(core_translation_unit_t *unit,
                           const char *message);

typedef struct {
    n00b_abi_frame_layout_v1 *rows;
    uint32_t *function_frames;
    uint8_t *bitmap;
    size_t row_count;
    size_t bitmap_count;
    size_t active_count;
} abi_frame_rows_t;

typedef struct {
    size_t roots;
    size_t pointer_roots;
    size_t slots;
    size_t bitmap_bytes;
    uint32_t shadow_type_index;
    const ir_final_layout_t *layout;
} abi_frame_info_t;

static bool
collect_frame_payload_types(core_translation_unit_t *unit,
                            n00bb_abi_type_index_t *index,
                            ir_module_t *runtime,
                            const core_target_desc_t *target)
{
    for (size_t i = 0; i < ir_func_count(runtime); i++) {
        ir_func_t *function = ir_func_at(runtime, i);
        if (!function || !function->n_blocks) continue;
        size_t pointers = 0, roots = 0;
        if (!n00bb_frame_pointer_roots(function, nullptr, nullptr, &pointers)
            || !n00bb_frame_roots(function, nullptr, nullptr, &roots))
            return false;
        if (roots == pointers) continue;
        th_type *payload = n00bb_frame_payload_type(function, &roots);
        n00bb_abi_record_member_t *members = payload
            ? core_alloc_array(n00bb_abi_record_member_t, roots) : nullptr;
        bool valid = payload && members;
        const th_member *failed_member = nullptr;
        for (size_t j = 0; valid && j < roots; j++) {
            const th_member *member = th_record_member(payload, j);
            bool handled = false;
            valid = member && n00bb_abi_publish_direct_layout(runtime,
                member->type, target, &handled);
            if (valid && !ir_layout_final(runtime, member->type,
                                          target->triple)) {
                ir_module_t *owner = ir_type_definition_owner(runtime,
                    member->type);
                const ir_final_layout_t *layout = owner
                    ? ir_layout_final(owner, member->type, target->triple)
                    : nullptr;
                valid = layout && ir_layout_publish_final(runtime,
                    member->type, target->triple, layout);
            }
            valid = valid && ir_layout_final(runtime, member->type,
                                             target->triple);
            if (!valid) failed_member = member;
            if (valid) members[j] = (n00bb_abi_record_member_t){
                .name = member->name,
                .type = member->type,
                .ordinal = (uint32_t)j,
            };
        }
        if (valid && !ir_layout_final(runtime, payload, target->triple))
            valid = n00bb_abi_publish_record_layout(runtime, payload,
                target, N00BB_RECORD_ORDERED, true, members, roots);
        if (valid) valid = n00bb_abi_type_index_add(index, payload, nullptr);
        core_free(members);
        if (!valid) {
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "could not lay out frame payload for %s at %s: %s",
                function->name,
                failed_member ? failed_member->name : "<record>",
                failed_member
                    ? th_print(ir_module_types(runtime), failed_member->type)
                    : payload ? th_print(ir_module_types(runtime), payload)
                              : "<missing type>");
            bool result = abi_unit_error(unit, message.data);
            core_buffer_release(&message);
            return result;
        }
    }
    return true;
}

static bool
frame_info(n00bb_abi_type_index_t *index,
           const n00bb_abi_type_identity_rows_t *types,
           ir_func_t *function, abi_frame_info_t *out)
{
    size_t pointers = 0, roots = 0;
    if (!n00bb_frame_pointer_roots(function, nullptr, nullptr, &pointers)
        || !n00bb_frame_roots(function, nullptr, nullptr, &roots))
        return false;
    abi_frame_info_t info = {
        .roots = pointers,
        .pointer_roots = pointers,
        .slots = pointers,
    };
    if (roots != pointers) {
        th_type *payload = n00bb_frame_payload_type(function, &roots);
        uint32_t payload_index = 0;
        if (!payload || !n00bb_abi_type_index_find(index, payload,
                                                    &payload_index)
            || payload_index >= types->count) return false;
        const ir_final_layout_t *layout = ir_layout_final(function->owner,
            payload, ir_module_target(function->owner));
        uint64_t stride = 0;
        if (!layout || layout->alignment_bytes > 16
            || layout->n_members != roots
            || !ir_layout_final_stride_bits(layout, &stride)
            || stride / 64 > SIZE_MAX - (stride % 64 != 0)) return false;
        info.roots = roots;
        info.slots = (size_t)(stride / 64 + (stride % 64 != 0));
        info.shadow_type_index = payload_index;
        info.layout = layout;
    }
    if (info.slots > (UINT64_MAX - N00B_ABI_FRAME_ROOT_OFFSET) / 8)
        return false;
    info.bitmap_bytes = info.slots / 8 + (info.slots % 8 != 0);
    *out = info;
    return true;
}

typedef struct {
    ir_func_t *function;
    const ir_final_layout_t *layout;
    uint8_t *bitmap;
    size_t slots;
} abi_frame_bitmap_t;

static bool
frame_bitmap_root(void *opaque, n00bb_frame_root_kind_t kind, size_t index,
                  ir_value_t *value, ir_local_t *local,
                  ir_instr_t *instruction)
{
    (void)kind;
    (void)instruction;
    abi_frame_bitmap_t *bitmap = opaque;
    th_type *type = n00bb_frame_root_type(kind, value, local, instruction);
    if (!type || index >= bitmap->layout->n_members) return false;
    if (!n00bb_frame_native_pointer(bitmap->function->owner, type))
        return true;
    uint64_t bits = bitmap->layout->members[index].bit_offset;
    if (bits % 64 || bits / 64 >= bitmap->slots) return false;
    size_t slot = (size_t)(bits / 64);
    bitmap->bitmap[slot / 8] |= (uint8_t)(1u << (slot % 8));
    return true;
}

static void
frame_rows_release(abi_frame_rows_t *frames)
{
    core_free(frames->rows);
    core_free(frames->function_frames);
    core_free(frames->bitmap);
    *frames = (abi_frame_rows_t){0};
}

static bool
build_frame_rows(n00bb_abi_function_rows_t *functions,
                 n00bb_abi_type_index_t *index,
                 const n00bb_abi_type_identity_rows_t *types,
                 ir_module_t *runtime, abi_frame_rows_t *out)
{
    if (!functions || !functions->functions || !functions->ir_functions
        || !index || !types || !runtime || !out
        || functions->function_count > UINT32_MAX)
        return false;
    size_t bytes = 0;
    size_t active = 0;
    for (size_t i = 1; i < functions->function_count; i++) {
        ir_func_t *function = functions->ir_functions[i];
        if (!function || function->owner != runtime || !function->n_blocks)
            continue;
        abi_frame_info_t info;
        if (!frame_info(index, types, function, &info)) return false;
        if (info.bitmap_bytes > UINT32_MAX
            || bytes > UINT32_MAX - info.bitmap_bytes)
            return false;
        bytes += info.bitmap_bytes;
        active++;
    }
    abi_frame_rows_t frames = {
        .rows = core_alloc_array(n00b_abi_frame_layout_v1, active + 1),
        .function_frames = core_alloc_array(uint32_t,
            functions->function_count),
        .bitmap = bytes ? core_alloc_array(uint8_t, bytes) : nullptr,
        .row_count = 1,
    };
    if (!frames.rows || !frames.function_frames
        || (bytes && !frames.bitmap)) {
        frame_rows_release(&frames);
        return false;
    }
    for (size_t i = 1; i < functions->function_count; i++) {
        ir_func_t *function = functions->ir_functions[i];
        if (!function || function->owner != runtime || !function->n_blocks)
            continue;
        abi_frame_info_t info;
        if (!frame_info(index, types, function, &info)) {
            frame_rows_release(&frames);
            return false;
        }
        size_t width = info.bitmap_bytes;
        if (info.layout) {
            abi_frame_bitmap_t bitmap = {
                .function = function,
                .layout = info.layout,
                .bitmap = frames.bitmap + frames.bitmap_count,
                .slots = info.slots,
            };
            size_t filled = 0;
            if (!n00bb_frame_roots(function, frame_bitmap_root, &bitmap,
                                   &filled) || filled != info.roots) {
                frame_rows_release(&frames);
                return false;
            }
        } else if (width) {
            memset(frames.bitmap + frames.bitmap_count, 0xff, width);
            if (info.pointer_roots % 8)
                frames.bitmap[frames.bitmap_count + width - 1] =
                    (uint8_t)((1u << (info.pointer_roots % 8)) - 1);
        }
        frames.function_frames[i] = (uint32_t)frames.row_count;
        frames.rows[frames.row_count++] = (n00b_abi_frame_layout_v1){
            .function_index = (uint32_t)i,
            .shadow_frame_type_index = info.shadow_type_index,
            .pointer_bitmap_bytes = {
                .first = width ? (uint32_t)frames.bitmap_count : 0,
                .count = (uint32_t)width,
            },
            .frame_byte_extent = N00B_ABI_FRAME_ROOT_OFFSET
                + 8 * (uint64_t)info.slots,
        };
        frames.bitmap_count += width;
        frames.active_count++;
    }
    *out = frames;
    return true;
}

typedef struct {
    n00b_abi_tls_relocation_v1 *rows;
    size_t row_count;
    uint32_t *stream_indexes;
    size_t stream_count;
    uint32_t string_storage_first_index;
    uint32_t string_storage_count;
    uint32_t aggregate_storage_index;
} abi_tls_metadata_t;

static void
tls_metadata_release(abi_tls_metadata_t *metadata)
{
    core_free(metadata->rows);
    core_free(metadata->stream_indexes);
}

static bool
build_tls_metadata(n00bb_abi_storage_rows_t *storage,
                   size_t string_pool_bytes, uint32_t stream_base,
                   n00b_abi_module_v1 *module, abi_tls_metadata_t *out)
{
    size_t count = 0;
    bool aggregate_pool = false;
    for (size_t i = 0; i < storage->relocation_count; i++) {
        const n00bb_abi_storage_relocation_t *reloc =
            &storage->relocations[i];
        if (reloc->source_kind != N00B_ABI_STORAGE_TLS) continue;
        if (reloc->target_kind == N00BB_ABI_RELOC_AGGREGATE)
            aggregate_pool = true;
        else if (reloc->target_kind != N00BB_ABI_RELOC_STRING_POOL)
            return false;
        if (reloc->target_offset > INT64_MAX) return false;
        count++;
    }
    if (!count) {
        out->rows = core_alloc_array(n00b_abi_tls_relocation_v1, 1);
        out->row_count = 1;
        return out->rows != nullptr;
    }
    uint64_t *string_offsets = core_alloc_array(uint64_t, count);
    uint64_t *string_lengths = core_alloc_array(uint64_t, count);
    size_t string_count = 0;
    bool valid = string_offsets && string_lengths;
    for (size_t i = 0; valid && i < storage->relocation_count; i++) {
        const n00bb_abi_storage_relocation_t *reloc =
            &storage->relocations[i];
        if (reloc->source_kind != N00B_ABI_STORAGE_TLS
            || reloc->target_kind != N00BB_ABI_RELOC_STRING_POOL) continue;
        size_t j = 0;
        while (j < string_count
            && string_offsets[j] != reloc->target_offset) j++;
        if (j < string_count) continue;
        n00bb_abi_string_header_t header;
        valid = reloc->byte_offset <= storage->tls.byte_len
            && sizeof(n00b_abi_string_v1)
                <= storage->tls.byte_len - reloc->byte_offset
            && n00bb_abi_string_decode((const uint8_t *)storage->tls.data
                + reloc->byte_offset, sizeof(n00b_abi_string_v1), &header)
            && (header.tag == N00BB_ABI_STRING_LONG
                || header.tag == N00BB_ABI_STRING_SPAN)
            && reloc->target_offset <= string_pool_bytes
            && header.length <= string_pool_bytes - reloc->target_offset;
        if (valid) {
            string_offsets[string_count] = reloc->target_offset;
            string_lengths[string_count++] = header.length;
        }
    }
    size_t backing_count = string_count + (size_t)aggregate_pool;
    size_t static_count = storage->static_range.count;
    if (!valid || (string_count && !string_pool_bytes)
        || (aggregate_pool && !storage->aggregates.byte_len)
        || count >= UINT32_MAX
        || storage->row_count > UINT32_MAX - backing_count
        || static_count > UINT32_MAX - backing_count
        || count > UINT32_MAX - static_count - backing_count
        || stream_base > UINT32_MAX - static_count - backing_count - count
        || static_count > SIZE_MAX - backing_count - count) {
        core_free(string_offsets);
        core_free(string_lengths);
        return false;
    }

    out->row_count = count + 1;
    out->rows = core_alloc_array(n00b_abi_tls_relocation_v1,
                                  out->row_count);
    out->stream_count = static_count + backing_count + count;
    out->stream_indexes = core_alloc_array(uint32_t, out->stream_count);
    if (static_count)
        memcpy(out->stream_indexes, storage->stream_indexes,
               static_count * sizeof(uint32_t));
    storage->rows = core_realloc(storage->rows,
        (storage->row_count + backing_count) * sizeof(*storage->rows));
    size_t next = storage->row_count;
    out->string_storage_first_index = (uint32_t)next;
    out->string_storage_count = (uint32_t)string_count;
    for (size_t i = 0; i < string_count; i++) {
        storage->rows[next++] = (n00b_abi_storage_v1){
            .module_index = 1,
            .segment_kind = N00B_ABI_STORAGE_READ_ONLY,
            .byte_offset = string_offsets[i],
            .byte_length = string_lengths[i],
        };
        out->stream_indexes[static_count++] =
            out->string_storage_first_index + (uint32_t)i;
    }
    if (aggregate_pool) {
        out->aggregate_storage_index = (uint32_t)next;
        storage->rows[next++] = (n00b_abi_storage_v1){
            .module_index = 1,
            .segment_kind = N00B_ABI_STORAGE_READ_ONLY,
            .byte_length = storage->aggregates.byte_len,
        };
        out->stream_indexes[static_count++] = out->aggregate_storage_index;
    }
    size_t original_count = storage->row_count;
    storage->row_count = next;
    module->static_storage = (n00b_abi_range_v1){
        .first = stream_base, .count = (uint32_t)static_count,
    };
    size_t relocation_row = 1;
    size_t stream_position = static_count;
    for (size_t i = 1; i < original_count; i++) {
        n00b_abi_storage_v1 *row = &storage->rows[i];
        if (row->segment_kind != N00B_ABI_STORAGE_TLS) continue;
        size_t first = stream_position;
        for (size_t j = 0; j < storage->relocation_count; j++) {
            const n00bb_abi_storage_relocation_t *source =
                &storage->relocations[j];
            if (source->source_kind != N00B_ABI_STORAGE_TLS
                || source->byte_offset < row->byte_offset
                || source->byte_offset - row->byte_offset > row->byte_length
                || sizeof(void *) > row->byte_length
                    - (source->byte_offset - row->byte_offset)) continue;
            uint32_t target = out->aggregate_storage_index;
            if (source->target_kind == N00BB_ABI_RELOC_STRING_POOL) {
                size_t k = 0;
                while (k < string_count
                    && string_offsets[k] != source->target_offset) k++;
                if (k == string_count) valid = false;
                else target = out->string_storage_first_index + (uint32_t)k;
            }
            if (!valid) break;
            out->rows[relocation_row] = (n00b_abi_tls_relocation_v1){
                .template_storage_index = (uint32_t)i,
                .referenced_storage_index = target,
                .write_byte_offset = source->byte_offset - row->byte_offset,
                .addend_bytes = source->target_kind
                    == N00BB_ABI_RELOC_STRING_POOL
                    ? 0 : (int64_t)source->target_offset,
                .target_kind = N00B_ABI_STORAGE_READ_ONLY,
            };
            out->stream_indexes[stream_position++] =
                (uint32_t)relocation_row++;
        }
        if (stream_position != first) row->tls_relocations =
            (n00b_abi_range_v1){
                .first = stream_base + (uint32_t)first,
                .count = (uint32_t)(stream_position - first),
            };
    }
    core_free(string_offsets);
    core_free(string_lengths);
    return valid && relocation_row == out->row_count
        && stream_position == out->stream_count;
}

static bool
align_up(size_t value, size_t alignment, size_t *out)
{
    if (!alignment || (alignment & (alignment - 1))
        || value > SIZE_MAX - (alignment - 1)) return false;
    *out = (value + alignment - 1) & ~(alignment - 1);
    return true;
}

bool
n00bb_abi_pack_tables(const core_target_desc_t *target,
                      uint64_t artifact_capabilities,
                      const n00bb_abi_table_rows_t *tables,
                      size_t table_count, n00bb_abi_packed_tables_t *out)
{
    n00bb_abi_image_key_t key;
    uint16_t one = 1;
    if (!out || (table_count && !tables)
        || table_count > UINT32_MAX
        || table_count > (SIZE_MAX - N00BB_ABI_HEADER_BYTES)
                         / N00BB_ABI_DIRECTORY_BYTES
        || *(const uint8_t *)&one != 1
        || !n00bb_abi_image_key_for_target(target, &key)) return false;

    size_t schema_count = 0;
    const n00bb_abi_table_schema_t *schemas =
        n00bb_abi_v1_table_schemas(&schema_count);
    n00bb_abi_directory_entry_t *entries = core_alloc_array(
        n00bb_abi_directory_entry_t, table_count);
    size_t cursor = N00BB_ABI_HEADER_BYTES
        + table_count * N00BB_ABI_DIRECTORY_BYTES;
    uint32_t alignment = _Alignof(n00b_abi_header_v1);
    bool valid = true;
    for (size_t i = 0; i < table_count; i++) {
        const n00bb_abi_table_schema_t *schema = nullptr;
        for (size_t j = 0; j < schema_count; j++)
            if (schemas[j].kind == tables[i].kind) {
                schema = &schemas[j];
                break;
            }
        if (!schema || !schema->record_bytes || !schema->alignment_bytes
            || (tables[i].record_count && !tables[i].records)
            || tables[i].record_count > UINT32_MAX
            || tables[i].record_count > SIZE_MAX / schema->record_bytes
            || !align_up(cursor, schema->alignment_bytes, &cursor)) {
            valid = false;
            break;
        }
        size_t bytes = tables[i].record_count * schema->record_bytes;
        if (bytes > SIZE_MAX - cursor) {
            valid = false;
            break;
        }
        entries[i] = (n00bb_abi_directory_entry_t){
            .kind = schema->kind,
            .schema_version = schema->schema_version,
            .record_bytes = schema->record_bytes,
            .record_count = (uint32_t)tables[i].record_count,
            .file_offset = cursor,
            .byte_length = bytes,
        };
        cursor += bytes;
        if (alignment < schema->alignment_bytes)
            alignment = schema->alignment_bytes;
    }
    if (!valid) {
        core_free(entries);
        return false;
    }

    uint8_t *image = core_alloc_size(1, cursor);
    for (size_t i = 0; i < table_count; i++)
        if (entries[i].byte_length)
            memcpy(image + entries[i].file_offset, tables[i].records,
                   (size_t)entries[i].byte_length);
    n00bb_abi_image_header_t header = {
        .key = key,
        .artifact_capabilities = artifact_capabilities,
        .required_alignment = alignment,
        .directory_count = (uint32_t)table_count,
        .directory_offset = N00BB_ABI_HEADER_BYTES,
        .total_extent = cursor,
    };
    valid = n00bb_abi_container_write(image, cursor, &header,
        entries, schemas, schema_count, false);
    core_free(entries);
    if (!valid) {
        core_free(image);
        return false;
    }
    *out = (n00bb_abi_packed_tables_t){
        .bytes = image, .byte_count = cursor, .alignment_bytes = alignment,
    };
    return true;
}

void
n00bb_abi_packed_tables_release(n00bb_abi_packed_tables_t *packed)
{
    if (!packed) return;
    core_free(packed->bytes);
    *packed = (n00bb_abi_packed_tables_t){0};
}

static bool
publish_catalog_record_specializations(n00bb_abi_type_index_t *index,
    ir_module_t *module, const core_target_desc_t *target,
    n00bb_abi_type_owner_lookup_fn type_owner, void *type_owner_context);

bool
n00bb_abi_write_object(
    const core_target_desc_t *target,
    const n00bb_abi_packed_tables_t *packed,
    const core_object_emit_section_t *extra_sections, size_t extra_count,
    const core_object_emit_symbol_t *symbols, size_t symbol_count,
    const core_object_emit_absolute64_t *relocations,
    size_t relocation_count, uint8_t **out_image, size_t *out_size)
{
    if (!target || !packed || !packed->bytes || !packed->alignment_bytes
        || packed->byte_count < N00BB_ABI_HEADER_BYTES
        || !out_image || !out_size || extra_count >= UINT32_MAX
        || extra_count > SIZE_MAX / sizeof(core_object_emit_section_t) - 1
        || (extra_count && !extra_sections)) return false;
    n00bb_abi_image_key_t key;
    if (!n00bb_abi_image_key_for_target(target, &key)) return false;
    uint32_t entry_count = core_le_load_u32(packed->bytes + 60);
    if (entry_count > (packed->byte_count - N00BB_ABI_HEADER_BYTES)
                      / N00BB_ABI_DIRECTORY_BYTES) return false;
    n00bb_abi_directory_entry_t *entries = core_alloc_array(
        n00bb_abi_directory_entry_t, entry_count);
    size_t schema_count = 0;
    const n00bb_abi_table_schema_t *schemas =
        n00bb_abi_v1_table_schemas(&schema_count);
    n00bb_abi_image_header_t header;
    bool valid = n00bb_abi_container_read(packed->bytes, packed->byte_count,
        &key, schemas, schema_count, false, &header, entries, entry_count);
    if (!valid || header.required_alignment != packed->alignment_bytes) {
        core_free(entries);
        return false;
    }
    const char *name, *segment = nullptr;
    switch (target->object_format) {
    case CORE_TARGET_OBJECT_ELF:
        name = ".data.rel.ro.n00b_meta";
        break;
    case CORE_TARGET_OBJECT_MACH_O:
        name = "__n00b_meta";
        segment = "__DATA_CONST";
        break;
    case CORE_TARGET_OBJECT_COFF:
        name = ".n00b$M";
        break;
    default:
        return false;
    }
    size_t section_count = extra_count + 1;
    core_object_emit_section_t *sections = core_alloc_array(
        core_object_emit_section_t, section_count);
    sections[0] = (core_object_emit_section_t){
        .name = name, .segment = segment,
        .bytes = packed->bytes, .size = packed->byte_count,
        .alignment = packed->alignment_bytes,
    };
    if (extra_count)
        memcpy(sections + 1, extra_sections,
               extra_count * sizeof(*extra_sections));
    uint8_t *image = nullptr;
    size_t image_bytes = 0;
    bool written = core_object_emit(target, sections, section_count,
        symbols, symbol_count, relocations, relocation_count,
        &image, &image_bytes);
    core_free(sections);
    core_object_section_t metadata;
    if (written)
        written = core_object_section_find(image, image_bytes,
            target->object_format, segment, name, &metadata)
            && metadata.size == packed->byte_count
            && n00bb_abi_container_write((uint8_t *)metadata.bytes,
                metadata.size, &header, entries, schemas, schema_count,
                false);
    core_free(entries);
    if (!written) {
        core_free(image);
        return false;
    }
    *out_image = image;
    *out_size = image_bytes;
    return written;
}

static bool
abi_unit_error(core_translation_unit_t *unit, const char *message)
{
    core_diagnostics_report(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-B0007"),
        core_string_from_cstr(message));
    return false;
}

typedef struct {
    n00b_abi_native_binding_v1 *rows;
    char **names;
    size_t storage_count;
    size_t count;
} abi_native_bindings_t;

static void
native_bindings_release(abi_native_bindings_t *bindings)
{
    if (!bindings) return;
    for (size_t i = 0; i < bindings->count; i++)
        core_free(bindings->names[i]);
    core_free(bindings->names);
    core_free(bindings->rows);
    *bindings = (abi_native_bindings_t){0};
}

static char *
native_global_name(core_string_t package, core_string_t module,
                   core_string_t symbol, bool exported,
                   core_target_object_format_t format)
{
    if (!package.data || !module.data || !symbol.data
        || !module.u8_bytes || !symbol.u8_bytes
        || memchr(module.data, 0, module.u8_bytes)
        || memchr(symbol.data, 0, symbol.u8_bytes)) return nullptr;
    bool default_package = core_string_eq(package,
        CORE_STRING_STATIC("default"));
    bool builtin_package = core_string_eq(package,
        CORE_STRING_STATIC("builtin"));
    size_t prefix = !exported + (format == CORE_TARGET_OBJECT_MACH_O)
        + (default_package ? 1 : builtin_package ? 2 : 0);
    if (module.u8_bytes > SIZE_MAX - prefix - 2
        || symbol.u8_bytes > SIZE_MAX - prefix - module.u8_bytes - 2)
        return nullptr;
    size_t size = prefix + module.u8_bytes + 1 + symbol.u8_bytes;
    char *name = core_alloc_array(char, size + 1);
    size_t at = 0;
    if (format == CORE_TARGET_OBJECT_MACH_O) name[at++] = '_';
    if (!exported) name[at++] = '_';
    if (default_package || builtin_package) name[at++] = '$';
    if (builtin_package) name[at++] = '$';
    for (size_t i = 0; i < module.u8_bytes; i++)
        name[at++] = module.data[i] == '.' ? '$' : module.data[i];
    name[at++] = '$';
    memcpy(name + at, symbol.data, symbol.u8_bytes);
    name[size] = 0;
    return name;
}

void
n00bb_abi_index_table_release(n00bb_abi_index_table_t *table)
{
    if (!table) return;
    core_free(table->symbol);
    core_free(table->type_hashes);
    core_free(table->functions);
    *table = (n00bb_abi_index_table_t){0};
}

static bool
append_index_entry(n00bb_abi_storage_rows_t *storage, uint32_t table,
                   uint32_t local_index)
{
    uint64_t offset = storage->read_only.byte_len;
    uint8_t bytes[4];
    core_le_store_u32(bytes, local_index);
    core_buffer_append(&storage->read_only, (const char *)bytes, 4);
    storage->index_patches = core_realloc(storage->index_patches,
        (storage->index_patch_count + 1) * sizeof(*storage->index_patches));
    storage->index_patches[storage->index_patch_count++] =
        (n00bb_abi_storage_index_patch_t){
            .source_kind = N00B_ABI_STORAGE_READ_ONLY,
            .byte_offset = offset,
            .table_kind = table,
            .patch_kind = N00B_ABI_PATCH_INDEX,
            .local_index = local_index,
        };
    return true;
}

static bool
append_index_table(n00bb_abi_storage_rows_t *storage,
                   const n00bb_abi_type_identity_rows_t *types,
                   const n00bb_abi_function_rows_t *functions,
                   uint64_t *offset, n00bb_abi_index_table_t *out)
{
    while (storage->read_only.byte_len % 4)
        core_buffer_append(&storage->read_only, "\0", 1);
    if (storage->read_only_alignment < 4) storage->read_only_alignment = 4;
    *offset = storage->read_only.byte_len;
    size_t type_count = types->count ? types->count - 1 : 0;
    size_t function_count = functions->function_count
        ? functions->function_count - 1 : 0;
    out->type_hashes = core_alloc_array(uint64_t, type_count + 1);
    out->functions = core_alloc_array(ir_func_t *, function_count + 1);
    out->type_count = type_count;
    out->function_count = function_count;
    bool valid = out->type_hashes && out->functions;
    for (size_t i = 1; valid && i <= type_count; i++) {
        out->type_hashes[i - 1] = types->rows[i].typehash;
        valid = append_index_entry(storage, N00B_ABI_TABLE_TYPE, (uint32_t)i);
    }
    for (size_t i = 1; valid && i <= function_count; i++) {
        out->functions[i - 1] = functions->ir_functions[i];
        valid = append_index_entry(storage, N00B_ABI_TABLE_FUNCTION,
                                   (uint32_t)i);
    }
    if (!type_count && !function_count)
        core_buffer_append(&storage->read_only, "\0\0\0\0", 4);
    return valid;
}

static char *
index_table_symbol(core_string_t module, core_target_object_format_t format)
{
    core_buffer_t name = {0};
    core_buffer_printf(&name, "%s__n00b_index_table$",
        format == CORE_TARGET_OBJECT_MACH_O ? "_" : "");
    for (size_t i = 0; i < module.u8_bytes; i++)
        core_buffer_printf(&name, "%c",
            module.data[i] == '.' ? '$' : module.data[i]);
    return name.data;
}

static bool
build_native_bindings(const n00bb_abi_storage_rows_t *storage,
                      const n00bb_abi_decl_index_t *declarations,
                      n00bb_abi_declarations_t *rows,
                      n00bb_abi_name_pool_t *pool, core_string_t package,
                      core_string_t module,
                      core_target_object_format_t format,
                      abi_native_bindings_t *out)
{
    if (!storage || !storage->rows || !declarations || !rows
        || !rows->symbols || !pool || !out) return false;
    size_t externs = 0;
    for (size_t i = 1; i < rows->symbol_count; i++) {
        parse_sym_entry_t *symbol = n00bb_abi_decl_symbol_at(declarations,
            (uint32_t)i);
        externs += symbol && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.generated-extern"));
    }
    abi_native_bindings_t built = {
        .rows = core_alloc_array(n00b_abi_native_binding_v1,
                                 storage->row_count + externs),
        .names = core_alloc_array(char *, storage->row_count + externs),
        .storage_count = storage->row_count,
        .count = storage->row_count + externs,
    };
    memset(built.rows, 0, built.count * sizeof(*built.rows));
    memset(built.names, 0, built.count * sizeof(*built.names));
    bool valid = true;
    for (size_t i = 1; valid && i < built.storage_count; i++) {
        const n00b_abi_storage_v1 *value = &storage->rows[i];
        if (!value->symbol_index || value->symbol_index >= rows->symbol_count
            || value->byte_offset > INT64_MAX) {
            valid = false;
            break;
        }
        n00b_abi_symbol_v1 *symbol_row =
            &rows->symbols[value->symbol_index];
        parse_sym_entry_t *symbol = n00bb_abi_decl_symbol_at(declarations,
            value->symbol_index);
        if (!symbol || symbol_row->native_binding_index) {
            valid = false;
            break;
        }
        built.names[i] = native_global_name(package, module, symbol->name,
            symbol_row->flags & N00B_ABI_SYMBOL_FLAG_EXPORTED, format);
        if (!built.names[i]) { valid = false; break; }
        built.rows[i].kind = N00B_ABI_NATIVE_SECTION_OFFSET;
        built.rows[i].offset = (int64_t)value->byte_offset;
        valid = n00bb_abi_name_pool_encode(pool,
            core_string_from_cstr(built.names[i]),
            N00B_ABI_TABLE_NATIVE_BINDING, (uint32_t)i,
            offsetof(n00b_abi_native_binding_v1, native_name),
            &built.rows[i].native_name);
        if (valid) symbol_row->native_binding_index = (uint32_t)i;
    }
    size_t next = built.storage_count;
    for (size_t i = 1; valid && i < rows->symbol_count; i++) {
        parse_sym_entry_t *symbol = n00bb_abi_decl_symbol_at(declarations,
            (uint32_t)i);
        const char *native = symbol && parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.generated-extern"))
            ? parse_sym_binding(symbol, CORE_STRING_STATIC("n00bb.abi.native"))
            : nullptr;
        if (!native) continue;
        bool underscore = format == CORE_TARGET_OBJECT_MACH_O;
        size_t length = strlen(native);
        built.names[next] = core_alloc_array(char, length + underscore + 1);
        if (underscore) built.names[next][0] = '_';
        memcpy(built.names[next] + underscore, native, length + 1);
        built.rows[next].kind = N00B_ABI_NATIVE_EXTERNAL_NAME;
        valid = !rows->symbols[i].native_binding_index
            && n00bb_abi_name_pool_encode(pool,
                core_string_from_cstr(built.names[next]),
                N00B_ABI_TABLE_NATIVE_BINDING, (uint32_t)next,
                offsetof(n00b_abi_native_binding_v1, native_name),
                &built.rows[next].native_name);
        if (valid) rows->symbols[i].native_binding_index = (uint32_t)next;
        next++;
    }
    if (!valid) {
        native_bindings_release(&built);
        return false;
    }
    *out = built;
    return true;
}

static bool
append_storage_section(const core_target_desc_t *target, uint32_t kind,
                       const core_buffer_t *bytes, uint32_t alignment,
                       core_list_t(core_object_emit_section_t) *sections,
                       uint32_t input_indexes[4])
{
    if (!bytes->byte_len) return true;
    if (!alignment || !bytes->data || sections->len >= UINT32_MAX - 2)
        return false;
    const char *name = nullptr, *segment = nullptr;
    switch (target->object_format) {
    case CORE_TARGET_OBJECT_ELF:
        name = kind == N00B_ABI_STORAGE_READ_ONLY
            ? ".rodata.n00b_static"
            : kind == N00B_ABI_STORAGE_WRITABLE
                ? ".data.n00b_static" : ".tdata.n00b_static";
        break;
    case CORE_TARGET_OBJECT_MACH_O:
        name = kind == N00B_ABI_STORAGE_READ_ONLY
            ? "__n00b_ro" : kind == N00B_ABI_STORAGE_WRITABLE
                ? "__n00b_data" : "__n00b_tls";
        segment = kind == N00B_ABI_STORAGE_READ_ONLY
            ? "__DATA_CONST" : "__DATA";
        break;
    case CORE_TARGET_OBJECT_COFF:
        name = kind == N00B_ABI_STORAGE_READ_ONLY
            ? ".rdata$N" : kind == N00B_ABI_STORAGE_WRITABLE
                ? ".data$N" : ".tls$N";
        break;
    default:
        return false;
    }
    input_indexes[kind] = (uint32_t)(2 + sections->len);
    core_list_push(*sections, ((core_object_emit_section_t){
        .name = name, .segment = segment,
        .bytes = (const uint8_t *)bytes->data, .size = bytes->byte_len,
        .alignment = alignment,
        .writable = kind != N00B_ABI_STORAGE_READ_ONLY,
        .is_thread_local = kind == N00B_ABI_STORAGE_TLS,
    }));
    return true;
}

static bool
publish_catalog_direct_layouts(core_translation_unit_t *unit,
                               n00bb_abi_type_index_t *index,
                               ir_module_t *module,
                               const core_target_desc_t *target)
{
    for (size_t i = 0; i < n00bb_abi_type_index_count(index); i++) {
        th_type *type = n00bb_abi_type_index_at(index, (uint32_t)i);
        bool handled = false;
        if (!type || !n00bb_abi_publish_direct_layout(module, type,
                target, &handled)) {
            core_buffer_t message = {0};
            core_buffer_printf(&message, "could not publish ABI layout for %s",
                type ? th_print(ir_module_types(module), type)
                     : "<missing type>");
            bool result = abi_unit_error(unit, message.data);
            core_buffer_release(&message);
            return result;
        }
    }
    return true;
}

static bool
publish_catalog_record_specializations(n00bb_abi_type_index_t *index,
    ir_module_t *module, const core_target_desc_t *target,
    n00bb_abi_type_owner_lookup_fn type_owner, void *type_owner_context)
{
    bool progress;
    do {
        progress = false;
        for (size_t i = 1; i < n00bb_abi_type_index_count(index); i++) {
            th_type *type = n00bb_abi_type_index_at(index, (uint32_t)i);
            size_t arguments = 0;
            if (!type || !th_named_info(th_resolve(type), nullptr, nullptr,
                    nullptr, &arguments) || !arguments
                || !th_is_closed(ir_module_types(module), type)
                || ir_layout_final(module, type, target->triple)) continue;
            th_type *definition = ir_type_definition(module, type);
            if (!definition || th_kind_of(definition) != TH_RECORD) continue;
            parse_sym_entry_t *symbol = type_owner
                ? type_owner(type_owner_context, type) : nullptr;
            if (symbol && parse_sym_binding(symbol,
                    CORE_STRING_STATIC("n00bb.record-layout-request")))
                continue;
            size_t count = th_record_len(definition);
            if (count > UINT32_MAX) return false;
            bool variant = false;
            th_named_info(type, nullptr, nullptr, &variant, nullptr);
            n00bb_abi_record_member_t *members = count && !variant
                ? core_alloc_array(n00bb_abi_record_member_t, count)
                : nullptr;
            n00bb_abi_variant_arm_t *arms = count && variant
                ? core_alloc_array(n00bb_abi_variant_arm_t, count)
                : nullptr;
            bool ready = !count || (variant ? arms != nullptr
                                           : members != nullptr);
            for (size_t j = 0; ready && j < count; j++) {
                const th_member *field = th_record_member(definition, j);
                th_type *child = field ? field->type : nullptr;
                bool handled = false;
                uint64_t extent = 0;
                if (!child || (th_kind_of(th_resolve(child)) == TH_SEQUENCE
                        && th_seq_extent(child, &extent))
                    || !n00bb_abi_publish_direct_layout(module, child,
                        target, &handled)) {
                    ready = false;
                    break;
                }
                const ir_final_layout_t *layout = ir_layout_final(module,
                    child, target->triple);
                if (!layout) {
                    ir_module_t *child_owner = ir_type_definition_owner(
                        module, child);
                    const ir_final_layout_t *shared = child_owner
                        ? ir_layout_final(child_owner, child,
                            target->triple) : nullptr;
                    if (shared && ir_layout_publish_final(module, child,
                            target->triple, shared))
                        layout = ir_layout_final(module, child,
                            target->triple);
                }
                if (!layout) { ready = false; break; }
                if (variant)
                    arms[j] = (n00bb_abi_variant_arm_t){
                        .name = field->name, .type = child,
                        .ordinal = (uint32_t)j,
                    };
                else
                    members[j] = (n00bb_abi_record_member_t){
                        .name = field->name, .type = child,
                        .ordinal = (uint32_t)j,
                        .packed = n00bb_abi_layout_packable(layout)
                            && !layout->n_reserved,
                    };
            }
            if (ready) {
                ready = variant
                    ? n00bb_abi_publish_variant_layout(module, type,
                        target, arms, count)
                    : n00bb_abi_publish_record_layout(module, type,
                        target, N00BB_RECORD_DEFAULT, false, members, count);
                if (!ready) {
                    core_free(arms);
                    core_free(members);
                    return false;
                }
                progress = true;
            }
            core_free(arms);
            core_free(members);
        }
    } while (progress);
    return true;
}

static bool
append_imported_type_requirements(
    n00bb_abi_type_index_t *index,
    th_ctx *type_context,
    n00bb_abi_type_identity_rows_t *types,
    const core_translation_unit_t *unit,
    n00bb_abi_node_source_fn node_source, void *source_context,
    n00bb_abi_type_owner_lookup_fn type_owner, void *type_owner_context,
    n00bb_abi_name_pool_t *pool, n00b_abi_requirement_v1 *requirements,
    size_t *count)
{
    if (!type_owner || !node_source) return true;
    for (uint32_t i = 1; i < types->count; i++) {
        const n00b_abi_type_v1 *type = &types->rows[i];
        if (type->owner_module_index) continue;
        if (type->kind != N00B_ABI_TYPE_RECORD
            && type->kind != N00B_ABI_TYPE_VARIANT
            && type->kind != N00B_ABI_TYPE_ENUM
            && type->kind != N00B_ABI_TYPE_FLAGSET
            && type->kind != N00B_ABI_TYPE_CALLBACK
            && type->kind != N00B_ABI_TYPE_TRAIT
            && type->kind != N00B_ABI_TYPE_WRAPPER) continue;
        th_type *graph_type = n00bb_abi_type_index_at(index, i);
        parse_sym_entry_t *owner = graph_type
            ? type_owner(type_owner_context, graph_type) : nullptr;
        if (owner && parse_sym_binding(owner,
                CORE_STRING_STATIC("n00bb.builtin-type"))) continue;
        if (!owner || (!owner->decl_node && !owner->owner_unit)) continue;
        uint32_t declaration_index;
        if (!owner->type || !n00bb_abi_type_index_find(index, owner->type,
                &declaration_index)) {
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "imported type declaration '%.*s' is absent from the ABI type catalog",
                (int)owner->name.u8_bytes, owner->name.data);
            core_diagnostics_report(core_translation_unit_diagnostics(
                (core_translation_unit_t *)unit), (core_translation_unit_t *)unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-B0007"),
                core_string_borrow(message.data, message.byte_len));
            core_buffer_release(&message);
            return false;
        }
        if (declaration_index != i) continue;
        const core_translation_unit_t *declaring_unit = owner->owner_unit;
        parse_token_stream_t *declaring_tokens = nullptr;
        if ((owner->decl_node
                && (!node_source(source_context, owner->decl_node,
                        &declaring_unit, &declaring_tokens)
                    || !declaring_tokens))
            || !declaring_unit || !owner->name.data
            || !owner->name.u8_bytes || !owner->phases) {
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "imported type '%.*s' has no source owner (owner=%.*s, current=%.*s, declaration=%s, phases=%u)",
                (int)owner->name.u8_bytes,
                owner->name.data ? owner->name.data : "",
                declaring_unit
                    ? (int)declaring_unit->module.identifier.u8_bytes : 0,
                declaring_unit
                    ? declaring_unit->module.identifier.data : "",
                (int)unit->module.identifier.u8_bytes,
                unit->module.identifier.data,
                owner->decl_node ? "present" : "missing",
                (unsigned)owner->phases);
            core_diagnostics_report(core_translation_unit_diagnostics(
                (core_translation_unit_t *)unit), (core_translation_unit_t *)unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-B0007"),
                core_string_borrow(message.data, message.byte_len));
            core_buffer_release(&message);
            return false;
        }
        if (declaring_unit == unit) continue;
        core_buffer_t path = {0};
        core_buffer_append(&path, declaring_unit->module.identifier.data,
            declaring_unit->module.identifier.u8_bytes);
        core_buffer_putc(&path, '.');
        core_buffer_append(&path, owner->name.data, owner->name.u8_bytes);
        size_t next = ++*count;
        requirements[next] = (n00b_abi_requirement_v1){
            .symbol_kind = N00B_ABI_SYMBOL_TYPE,
            .contract_type_index = i,
            .required_layout_index = type->layout_index,
            .requesting_module_index = 1,
        };
        bool valid = n00bb_abi_name_pool_encode(pool,
            core_string_borrow(path.data, path.byte_len),
            N00B_ABI_TABLE_REQUIREMENT, (uint32_t)next,
            offsetof(n00b_abi_requirement_v1, full_path),
            &requirements[next].full_path);
        const char *reason = nullptr;
        if (valid)
            valid = n00bb_abi_type_variables_bind_import(index,
                type_context, owner, owner->type, (uint32_t)next,
                types, &reason);
        th_type *definition = valid
            ? th_type_definition(type_context, owner->type) : nullptr;
        if (valid && definition && definition != owner->type)
            valid = n00bb_abi_type_variables_bind_import(index,
                type_context, owner, definition, (uint32_t)next,
                types, &reason);
        for (parse_sym_entry_t *member = (type->kind == N00B_ABI_TYPE_CALLBACK
                || type->kind == N00B_ABI_TYPE_TRAIT) && owner->member_scope
                ? owner->member_scope->first_in_scope : nullptr;
             valid && member; member = member->next_in_scope) {
            if (member->kind != PARSE_SYM_FUNCTION || !member->type)
                continue;
            valid = n00bb_abi_type_variables_bind_import(index,
                type_context, member, member->type, (uint32_t)next,
                types, &reason);
        }
        core_buffer_release(&path);
        if (!valid) {
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "cannot bind imported type variables for %.*s: %s",
                (int)owner->name.u8_bytes, owner->name.data,
                reason ? reason : "invalid import requirement");
            core_diagnostics_report(core_translation_unit_diagnostics(
                (core_translation_unit_t *)unit), (core_translation_unit_t *)unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-B0007"),
                core_string_borrow(message.data, message.byte_len));
            core_buffer_release(&message);
            return false;
        }
    }
    return true;
}

static core_hash_value_t
declaration_pointer_hash(void *value)
{
    return (core_hash_value_t)(uintptr_t)value;
}

static bool
declaration_pointer_equal(const void *left, const void *right)
{
    return left == right;
}

static bool
imported_module_symbol(const core_link_reference_t *reference,
    const core_translation_unit_t *unit, parse_sym_entry_t **out_symbol,
    uint32_t *out_kind)
{
    *out_symbol = nullptr;
    core_link_declaration_t *target = reference
        ? core_link_reference_target(reference) : nullptr;
    if (!reference || core_link_reference_referrer(reference) != unit
        || !target || core_link_declaration_owner(target) == unit)
        return true;
    if (core_string_eq(core_link_declaration_module(target),
            CORE_STRING_STATIC("builtins")))
        return true;
    uint32_t kind = core_link_declaration_kind(target);
    if (kind != PARSE_SYM_FUNCTION && kind != PARSE_SYM_VARIABLE
        && kind != PARSE_SYM_TYPEDEF)
        return true;
    parse_sym_entry_t *symbol = core_link_declaration_binding(target,
        CORE_STRING_STATIC("n00bb.semantic.symbol"));
    if (!symbol || symbol->kind != kind) {
        core_string_t module = core_link_reference_module(reference);
        core_string_t key = core_link_reference_key(reference);
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "ABI import '%.*s:%.*s' targets symbol kind %u, but its declaration binds kind %u",
            (int)module.u8_bytes, module.data,
            (int)key.u8_bytes, key.data, kind,
            symbol ? (unsigned)symbol->kind : 0);
        core_translation_unit_t *referrer =
            core_link_reference_referrer(reference);
        core_diagnostics_report(core_translation_unit_diagnostics(referrer),
            referrer, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-B0016"),
            core_string_borrow(message.data, message.byte_len));
        core_buffer_release(&message);
        return false;
    }
    if (!symbol->scope || !core_string_eq(symbol->scope->scope_tag,
            CORE_STRING_STATIC("module"))) return true;
    *out_symbol = symbol;
    *out_kind = kind == PARSE_SYM_FUNCTION ? N00B_ABI_SYMBOL_FUNCTION
        : kind == PARSE_SYM_TYPEDEF ? N00B_ABI_SYMBOL_TYPE
        : N00B_ABI_SYMBOL_VARIABLE;
    return true;
}

static bool
collect_imported_symbol_types(core_link_unit_t *link,
    const core_translation_unit_t *unit, n00bb_abi_type_index_t *index)
{
    if (!link) return true;
    for (size_t i = 0; i < core_link_unit_reference_count(link); i++) {
        core_link_reference_t *reference = core_link_unit_reference_at_index(
            link, i);
        parse_sym_entry_t *symbol = nullptr;
        uint32_t kind = 0;
        if (!imported_module_symbol(reference, unit, &symbol, &kind))
            return false;
        if (!symbol) continue;
        core_link_declaration_t *target = core_link_reference_target(reference);
        th_type *type = core_link_declaration_binding(target,
            CORE_STRING_STATIC("n00bb.semantic.type"));
        if (!type) return false;
        bool added = kind == N00B_ABI_SYMBOL_TYPE
            && th_kind_of(th_resolve(type)) == TH_TRAIT
            ? n00bb_abi_type_index_add_imported_trait(index, type)
            : n00bb_abi_type_index_add(index, type, nullptr);
        if (!added)
            return false;
    }
    return true;
}

static bool
collect_environment_traits(ir_module_t *module,
                           n00bb_abi_type_index_t *index)
{
    th_ctx *types = ir_module_types(module);
    const th_env *environment = th_ctx_environment(types);
    if (!environment) return true;
    for (size_t i = 0; i < th_env_declaration_count(environment); i++) {
        th_env_declaration_view_t declaration;
        if (!th_env_declaration_at(environment, i, &declaration))
            return false;
        if (declaration.kind == TH_ENV_DECLARATION_TRAIT
            && declaration.contract
            && th_kind_of(th_resolve(declaration.contract)) == TH_TRAIT
            && declaration.module
            && !strcmp(declaration.module, "builtins")
            && !n00bb_abi_type_index_add(index, declaration.contract,
                                         nullptr))
            return false;
    }
    return true;
}

static bool
collect_imported_nominal_declarations(n00bb_abi_type_index_t *index,
    n00bb_abi_type_owner_lookup_fn type_owner, void *context)
{
    if (!type_owner) return true;
    for (size_t i = 1; i < n00bb_abi_type_index_count(index); i++) {
        th_type *type = n00bb_abi_type_index_at(index, (uint32_t)i);
        parse_sym_entry_t *owner = type ? type_owner(context, type) : nullptr;
        if (owner && (owner->decl_node || owner->owner_unit) && owner->type
            && !n00bb_abi_type_index_add(index, owner->type, nullptr))
            return false;
    }
    return true;
}

static bool
append_imported_symbol_requirements(core_link_unit_t *link,
    const core_translation_unit_t *unit, n00bb_abi_type_index_t *index,
    th_ctx *type_context, n00bb_abi_type_identity_rows_t *types,
    n00bb_abi_name_pool_t *pool, n00b_abi_requirement_v1 *requirements,
    size_t *count, uint32_t selected_kind)
{
    if (!link) return true;
    core_indexed_table_t *seen = core_indexed_table_new(0, UINT32_MAX,
        declaration_pointer_hash, declaration_pointer_equal);
    if (!seen) return false;
    bool valid = true;
    for (size_t i = 0; valid && i < core_link_unit_reference_count(link);
         i++) {
        core_link_reference_t *reference = core_link_unit_reference_at_index(
            link, i);
        parse_sym_entry_t *symbol = nullptr;
        uint32_t kind = 0;
        if (!imported_module_symbol(reference, unit, &symbol, &kind)) {
            valid = false;
            break;
        }
        if (!symbol || kind != selected_kind) continue;
        bool inserted = false;
        if (!core_indexed_table_intern(seen, symbol, symbol, &inserted)) {
            valid = false;
            break;
        }
        if (!inserted) continue;
        core_link_declaration_t *target = core_link_reference_target(reference);
        th_type *type = core_link_declaration_binding(target,
            CORE_STRING_STATIC("n00bb.semantic.type"));
        uint32_t type_index;
        core_string_t module = core_link_declaration_module(target);
        if (!type || !symbol->name.data || !symbol->name.u8_bytes
            || !symbol->phases || !module.data || !module.u8_bytes
            || !n00bb_abi_type_index_find(index, type, &type_index)
            || type_index >= types->count
            || (kind == N00B_ABI_SYMBOL_FUNCTION
                && types->rows[type_index].kind != N00B_ABI_TYPE_FUNCTION)) {
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "cannot describe imported %.*s.%.*s in the ABI type catalog",
                (int)module.u8_bytes, module.data,
                (int)symbol->name.u8_bytes, symbol->name.data);
            core_diagnostics_report(core_translation_unit_diagnostics(
                (core_translation_unit_t *)unit), (core_translation_unit_t *)unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-B0007"),
                core_string_borrow(message.data, message.byte_len));
            core_buffer_release(&message);
            valid = false;
            break;
        }
        core_buffer_t path = {0};
        core_buffer_append(&path, module.data, module.u8_bytes);
        core_buffer_putc(&path, '.');
        core_buffer_append(&path, symbol->name.data,
            symbol->name.u8_bytes);
        size_t next = ++*count;
        requirements[next] = (n00b_abi_requirement_v1){
            .symbol_kind = kind,
            .contract_type_index = type_index,
            .required_layout_index = kind == N00B_ABI_SYMBOL_VARIABLE
                ? types->rows[type_index].layout_index : 0,
            .requesting_module_index = 1,
        };
        valid = n00bb_abi_name_pool_encode(pool,
            core_string_borrow(path.data, path.byte_len),
            N00B_ABI_TABLE_REQUIREMENT, (uint32_t)next,
            offsetof(n00b_abi_requirement_v1, full_path),
            &requirements[next].full_path);
        core_buffer_release(&path);
        const char *reason = nullptr;
        if (valid && kind == N00B_ABI_SYMBOL_FUNCTION)
            valid = n00bb_abi_type_variables_bind_import(index,
                type_context, symbol, type, (uint32_t)next, types, &reason);
        if (!valid && kind == N00B_ABI_SYMBOL_FUNCTION) {
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "cannot bind imported function type variables for %.*s.%.*s: %s",
                (int)module.u8_bytes, module.data,
                (int)symbol->name.u8_bytes, symbol->name.data,
                reason ? reason : "invalid import requirement");
            core_diagnostics_report(core_translation_unit_diagnostics(
                (core_translation_unit_t *)unit), (core_translation_unit_t *)unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-B0007"),
                core_string_borrow(message.data, message.byte_len));
            core_buffer_release(&message);
        }
    }
    core_indexed_table_free(seen);
    return valid;
}

static bool
append_scc_input(core_buffer_t *buffer, const char *bytes, size_t count)
{
    if (count && !bytes) return false;
    uint8_t length[8];
    core_le_store_u64(length, (uint64_t)count);
    core_buffer_append(buffer, (const char *)length, sizeof(length));
    core_buffer_append(buffer, bytes, count);
    return true;
}

static bool
append_scc_source(core_buffer_t *buffer, const core_source_file_t *source)
{
    const char *contents = core_source_file_contents(source);
    size_t size = core_source_file_size(source);
    if (size && !contents) return false;
    uint8_t digest[8];
    core_le_store_u64(digest, core_hash_raw(contents ? contents : "", size));
    core_buffer_append(buffer, (const char *)digest, sizeof(digest));
    return true;
}

static bool
recursive_scc_fragment(core_link_unit_t *link,
                       core_translation_unit_t *unit,
                       n00b_abi_scc_v1 *manifest, uint32_t *ordinal,
                       bool *recursive)
{
    *recursive = false;
    if (!link) return true;
    size_t group_index;
    if (!core_link_unit_scc_of(link, unit, &group_index)) return false;
    size_t count = core_link_unit_scc_member_count(link, group_index);
    if (!count || count >= UINT32_MAX) return false;
    if (count == 1) {
        bool self_import = false;
        for (size_t i = 0; i < core_translation_unit_import_count(unit); i++)
            self_import |= core_translation_unit_import(unit, i) == unit;
        if (!self_import) return true;
    }
    bool valid = true;
    core_buffer_t input = {0};
    for (size_t i = 0; valid && i < count; i++) {
        core_translation_unit_t *member = core_link_unit_scc_member(
            link, group_index, i);
        if (!member || !member->module.identifier.data
            || !core_translation_unit_imports_finalized(member)) {
            valid = false;
            break;
        }
        core_string_t name = member->module.identifier;
        /* A source edit conservatively invalidates the entire cycle. */
        valid = append_scc_input(&input, name.data, name.u8_bytes)
            && append_scc_source(&input, &member->source);
        for (size_t j = 0; valid
             && j < core_translation_unit_import_count(member); j++) {
            core_translation_unit_t *imported = core_translation_unit_import(
                member, j);
            core_string_t import_name = imported->module.identifier;
            valid = append_scc_input(&input, import_name.data,
                                     import_name.u8_bytes);
        }
        if (member == unit) *ordinal = (uint32_t)i;
    }
    if (valid) {
        core_buffer_t identity = {0};
        core_buffer_puts(&identity, "n00b.scc.interface.v1");
        core_buffer_append(&identity, input.data, input.byte_len);
        manifest->interface_identity = core_hash_raw(identity.data,
                                                     identity.byte_len);
        core_buffer_release(&identity);
        core_buffer_puts(&identity, "n00b.scc.freshness.v1");
        core_buffer_append(&identity, input.data, input.byte_len);
        manifest->freshness_identity = core_hash_raw(identity.data,
                                                     identity.byte_len);
        core_buffer_release(&identity);
        *recursive = true;
    }
    core_buffer_release(&input);
    return valid;
}

typedef struct {
    const core_translation_unit_t *unit;
    n00bb_abi_node_source_fn source;
    void *context;
} abi_symbol_owner_t;

static bool
symbol_in_unit(void *opaque, const parse_sym_entry_t *symbol,
               bool *include)
{
    const abi_symbol_owner_t *owner = opaque;
    parse_tree_t *node = symbol->decl_node;
    if (!node && symbol->definition_count)
        node = symbol->definition_nodes[0];
    if (!node && n00bb_abi_decl_generated_symbol(symbol)) {
        *include = true;
        return true;
    }
    if (!node) {
        *include = false;
        return true;
    }
    const core_translation_unit_t *source_unit = nullptr;
    parse_token_stream_t *tokens = nullptr;
    if (!owner->source(owner->context, node, &source_unit, &tokens))
        return false;
    *include = source_unit == owner->unit;
    return true;
}

static bool
abi_stream_advance(size_t *offset, size_t count)
{
    if (count > UINT32_MAX - *offset) return false;
    *offset += count;
    return true;
}

bool
n00bb_abi_emit_unit(
    core_link_unit_t *link, core_translation_unit_t *unit,
    core_string_t package_id,
    const parse_symtab_t *symbols, parse_token_stream_t *tokens,
    const ir_module_t *runtime_ir,
    const core_target_desc_t *target, uint64_t artifact_capabilities,
    n00bb_abi_declaration_lookup_fn lookup, void *lookup_context,
    n00bb_abi_node_source_fn node_source, void *source_context,
    n00bb_abi_type_owner_lookup_fn type_owner, void *type_owner_context,
    const n00bb_abi_graph_source_t *graphs,
    n00bb_abi_packed_tables_t *out_tables,
    n00bb_abi_index_table_t *out_index_table,
    uint8_t **out_object, size_t *out_object_bytes)
{
    if (!unit || !symbols || !tokens || !runtime_ir || !target
        || !target->triple || !unit->module.identifier.data
        || !out_tables || !out_object || !out_object_bytes
        || !package_id.data || core_translation_unit_has_fatal_diagnostics(unit))
        return false;
    uint64_t index_table_offset = 0;
    n00bb_abi_index_table_t index_table = {0};
    if (link && !node_source)
        return abi_unit_error(unit,
            "linked ABI emission requires source ownership for syntax nodes");
    n00bb_abi_decl_index_t *declarations = n00bb_abi_decl_index_new(symbols);
    if (!declarations)
        return abi_unit_error(unit, "could not index local ABI declarations");
    for (size_t i = 1; i < n00bb_abi_decl_symbol_count(declarations); i++) {
        parse_sym_entry_t *symbol = n00bb_abi_decl_symbol_at(declarations,
                                                               (uint32_t)i);
        bool record = symbol && symbol->kind == PARSE_SYM_TYPEDEF
            && core_string_eq(symbol->adt_kind,
                              CORE_STRING_STATIC("record"));
        bool enumeration = symbol && symbol->kind == PARSE_SYM_TYPEDEF
            && core_string_eq(symbol->adt_kind,
                              CORE_STRING_STATIC("enum"));
        bool flagset = symbol && symbol->kind == PARSE_SYM_TYPEDEF
            && (core_string_eq(symbol->adt_kind,
                               CORE_STRING_STATIC("flagset"))
                || (symbol->member_scope
                    && core_string_eq(symbol->member_scope->scope_tag,
                                      CORE_STRING_STATIC("flagset"))));
        bool trait = symbol && symbol->kind == PARSE_SYM_TYPEDEF
            && (core_string_eq(symbol->adt_kind,
                               CORE_STRING_STATIC("trait"))
                || (symbol->member_scope
                    && core_string_eq(symbol->member_scope->scope_tag,
                                      CORE_STRING_STATIC("trait"))));
        bool callback = symbol && symbol->kind == PARSE_SYM_TYPEDEF
            && core_string_eq(symbol->adt_kind,
                              CORE_STRING_STATIC("callback"));
        bool alias = symbol && symbol->kind == PARSE_SYM_TYPEDEF
            && core_string_eq(symbol->adt_kind,
                              CORE_STRING_STATIC("alias"));
        bool nominal = symbol && symbol->kind == PARSE_SYM_TYPEDEF
            && core_string_eq(symbol->adt_kind,
                              CORE_STRING_STATIC("nominal"));
        bool variant = symbol && symbol->kind == PARSE_SYM_TYPEDEF
            && ((symbol->member_scope
                 && core_string_eq(symbol->member_scope->scope_tag,
                                   CORE_STRING_STATIC("variant")))
                || core_string_eq(symbol->adt_kind,
                                  CORE_STRING_STATIC("variant")));
        bool arm = symbol && (symbol->kind == PARSE_SYM_TAG
                || symbol->kind == PARSE_SYM_VARIABLE)
            && symbol->owner_type
            && ((symbol->owner_type->member_scope
                 && core_string_eq(symbol->owner_type->member_scope->scope_tag,
                                   CORE_STRING_STATIC("variant")))
                || core_string_eq(symbol->owner_type->adt_kind,
                                  CORE_STRING_STATIC("variant")));
        bool field = symbol && symbol->kind == PARSE_SYM_VARIABLE
            && symbol->is_field && symbol->owner_type
            && core_string_eq(symbol->owner_type->adt_kind,
                              CORE_STRING_STATIC("record"));
        bool enum_item = symbol && symbol->kind == PARSE_SYM_ENUM_CONST
            && symbol->owner_type
            && (core_string_eq(symbol->owner_type->adt_kind,
                               CORE_STRING_STATIC("enum"))
                || core_string_eq(symbol->owner_type->adt_kind,
                                  CORE_STRING_STATIC("flagset"))
                || (symbol->owner_type->member_scope
                    && core_string_eq(
                        symbol->owner_type->member_scope->scope_tag,
                        CORE_STRING_STATIC("flagset"))));
        bool function = symbol && symbol->kind == PARSE_SYM_FUNCTION;
        bool parameter = symbol && symbol->kind == PARSE_SYM_PARAM;
        bool local = symbol && symbol->kind == PARSE_SYM_VARIABLE
            && !symbol->is_field;
        if (!record && !field && !enumeration && !flagset && !enum_item && !trait
            && !callback && !alias && !nominal
            && !variant && !arm
            && !function && !parameter && !local) {
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "ABI emission does not support declaration '%.*s'",
                (int)symbol->name.u8_bytes, symbol->name.data);
            n00bb_abi_decl_index_free(declarations);
            bool result = abi_unit_error(unit, message.data);
            core_buffer_release(&message);
            return result;
        }
    }
    if (!core_translation_unit_imports_finalized(unit)) {
        n00bb_abi_decl_index_free(declarations);
        return abi_unit_error(unit,
            "ABI emission requires finalized imports");
    }
    size_t import_count = core_translation_unit_import_count(unit);
    if (import_count && (!link || !core_link_unit_graph_is_finalized(link)
        || import_count >= UINT32_MAX)) {
        n00bb_abi_decl_index_free(declarations);
        return abi_unit_error(unit,
            "ABI emission for imports requires a finalized link graph");
    }
    n00b_abi_scc_v1 sccs[2] = {0};
    uint32_t scc_ordinal = 0;
    bool recursive = false;
    if (link && !recursive_scc_fragment(link, unit, &sccs[1],
                                        &scc_ordinal, &recursive)) {
        n00bb_abi_decl_index_free(declarations);
        return abi_unit_error(unit,
            "could not establish recursive import group identity");
    }
    const char *runtime_name = ir_module_name(runtime_ir);
    if (!runtime_name || strlen(runtime_name) != unit->module.identifier.u8_bytes
        || memcmp(runtime_name, unit->module.identifier.data,
                  unit->module.identifier.u8_bytes)
        || !ir_module_target(runtime_ir)
        || strcmp(ir_module_target(runtime_ir), target->triple)) {
        n00bb_abi_decl_index_free(declarations);
        return abi_unit_error(unit,
            "ABI unit identity or target does not match its IR modules");
    }

    n00b_abi_package_v1 packages[2] = {0};
    n00b_abi_module_v1 modules[2] = {0};
    n00b_abi_runtime_config_v1 runtime_configs[2] = {0};
    n00b_abi_requirement_v1 *requirements = core_alloc_array(
        n00b_abi_requirement_v1, import_count + 1);
    size_t requirement_count = import_count;
    for (size_t i = 0; i < import_count; i++) {
        requirements[i + 1].symbol_kind = N00B_ABI_SYMBOL_MODULE;
        requirements[i + 1].requesting_module_index = 1;
        requirements[i + 1].contract_type_index =
            N00B_ABI_V1_NO_TYPE_INDEX;
    }
    uint32_t package_modules[1] = {1};
    uint32_t prefix_stream_count = 1 + (recursive ? 2 : 0);
    if (recursive) {
        sccs[1].members = (n00b_abi_range_v1){.first = 1, .count = 1};
        sccs[1].lexical_order = (n00b_abi_range_v1){.first = 2, .count = 1};
    }
    packages[1].visibility = N00B_ABI_VISIBILITY_PRIVATE;
    packages[1].modules = (n00b_abi_range_v1){.first = 0, .count = 1};
    modules[1].package_index = 1;
    modules[1].scc_index = recursive ? 1 : 0;
    modules[1].runtime_config_index = 1;
    modules[1].source_file_index = 1;

    n00bb_abi_name_pool_t *pool = n00bb_abi_name_pool_new();
    n00bb_abi_type_index_t *type_index = n00bb_abi_type_index_new(
        (ir_module_t *)runtime_ir);
    n00bb_abi_type_identity_rows_t type_rows = {0};
    n00bb_abi_type_components_t components = {0};
    n00bb_abi_type_layout_rows_t layout_rows = {0};
    n00bb_abi_enum_rows_t enum_rows = {0};
    n00bb_abi_type_signatures_t signature_rows = {0};
    n00bb_abi_declarations_t declaration_rows = {0};
    n00bb_abi_function_rows_t function_rows = {0};
    abi_frame_rows_t frame_rows = {0};
    n00bb_abi_trait_method_rows_t trait_method_rows = {0};
    n00bb_abi_constraint_rows_t constraint_rows = {0};
    n00bb_abi_storage_rows_t storage_rows = {0};
    n00bb_abi_baked_functions_t baked_functions = {0};
    abi_tls_metadata_t tls_metadata = {0};
    abi_native_bindings_t native_bindings = {0};
    n00bb_abi_packed_tables_t packed = {0};
    core_list_t(core_object_emit_section_t) extra_sections = {0};
    uint32_t *stream = nullptr;
    n00b_abi_parameter_v1 *parameters = nullptr;
    n00b_abi_index_patch_v1 *index_patches = nullptr;
    uint8_t *tls_descriptors = nullptr;
    size_t tls_descriptor_count = 0;
    core_object_emit_absolute64_t *relocations = nullptr;
    core_object_emit_symbol_t *object_symbols = nullptr;
    char **tls_template_names = nullptr;
    uint8_t *object = nullptr;
    size_t object_bytes = 0;
    size_t reference_count = link ? core_link_unit_reference_count(link) : 0;
    bool requirement_capacity_valid = requirements
        && reference_count <= UINT32_MAX - import_count - 1
        && reference_count <= (SIZE_MAX / sizeof(*requirements))
            - import_count - 1;
    if (requirement_capacity_valid)
        requirements = core_realloc(requirements,
            (import_count + reference_count + 1) * sizeof(*requirements));
    const char *failed_stage = "setup";
    const char *typevar_failure = nullptr;
    uint32_t failed_layout_type = UINT32_MAX;
    size_t stream_base = prefix_stream_count;
    size_t signature_stream_base = 0;
    size_t function_stream_base = 0;
    abi_symbol_owner_t symbol_owner = {
        .unit = unit, .source = node_source, .context = source_context,
    };
#define ABI_STAGE(name, call) ((call) || ((failed_stage = name), false))
    bool valid = pool && type_index && requirement_capacity_valid
        && ABI_STAGE("module type collection",
            n00bb_abi_type_index_collect_module_types(type_index,
                (ir_module_t *)runtime_ir))
        && ABI_STAGE("symbol type collection",
            n00bb_abi_type_index_collect_symbols(type_index, symbols,
                node_source ? symbol_in_unit : nullptr, &symbol_owner))
        && ABI_STAGE("imported type collection",
            collect_imported_symbol_types(link, unit, type_index))
        && ABI_STAGE("trait type collection",
            collect_environment_traits((ir_module_t *)runtime_ir,
                                       type_index))
        && ABI_STAGE("imported nominal declarations",
            collect_imported_nominal_declarations(type_index, type_owner,
                type_owner_context))
        && ABI_STAGE("direct type layouts",
            publish_catalog_direct_layouts(unit, type_index,
                (ir_module_t *)runtime_ir, target))
        && ABI_STAGE("imported record specialization layouts",
            publish_catalog_record_specializations(type_index,
                (ir_module_t *)runtime_ir, target, type_owner,
                type_owner_context))
        && ABI_STAGE("frame payload type collection",
            collect_frame_payload_types(unit, type_index,
                (ir_module_t *)runtime_ir, target))
        && ABI_STAGE("baked graph type collection",
            !graphs || n00bb_abi_graph_types_collect(type_index,
                declarations, (ir_module_t *)runtime_ir, lookup,
                lookup_context, graphs))
        && ABI_STAGE("type identity rows",
            n00bb_abi_type_identity_rows_build(type_index,
                (ir_module_t *)runtime_ir, false, symbols, type_owner, type_owner_context, pool,
                &type_rows))
        && ABI_STAGE("imported function requirements",
            append_imported_symbol_requirements(link, unit, type_index,
                ir_module_types((ir_module_t *)runtime_ir), &type_rows, pool,
                requirements, &requirement_count, N00B_ABI_SYMBOL_FUNCTION))
        && ABI_STAGE("baked function requirements",
            n00bb_abi_graph_function_requirements(type_index, &type_rows,
                declarations, (ir_module_t *)runtime_ir, lookup,
                lookup_context, graphs, pool, &requirements,
                &requirement_count, &baked_functions))
        && ABI_STAGE(typevar_failure ? typevar_failure : "type variable binding",
            n00bb_abi_type_variables_bind(type_index,
                ir_module_types((ir_module_t *)runtime_ir), declarations,
                &type_rows, &typevar_failure))
        && ABI_STAGE("type components",
            n00bb_abi_type_components_build(type_index,
                (ir_module_t *)runtime_ir, prefix_stream_count,
                &type_rows, &components))
        && ABI_STAGE("component stream indexes",
            abi_stream_advance(&stream_base, components.count))
        && ABI_STAGE("type layouts",
            n00bb_abi_type_layout_rows_build(type_index,
                (ir_module_t *)runtime_ir, false, (uint32_t)stream_base,
                declarations, pool, &type_rows, &layout_rows,
                &failed_layout_type))
        && ABI_STAGE("layout stream indexes",
            abi_stream_advance(&stream_base,
                layout_rows.stream_index_count))
        && ABI_STAGE("enum items",
            n00bb_abi_enum_rows_build(type_index, declarations, pool,
                (uint32_t)stream_base, &type_rows, &enum_rows))
        && ABI_STAGE("enum stream indexes",
            abi_stream_advance(&stream_base, enum_rows.stream_index_count));
    signature_stream_base = stream_base;
    valid = valid && ABI_STAGE("callable signatures",
            n00bb_abi_type_signatures_build(type_index, &type_rows, pool,
                (uint32_t)stream_base, &signature_rows))
        && ABI_STAGE("signature stream indexes",
            abi_stream_advance(&stream_base,
                signature_rows.parameter_index_count))
        && ABI_STAGE("declarations",
            n00bb_abi_declarations_build(declarations, type_index,
                &type_rows, unit, tokens, pool, node_source, source_context,
                (uint32_t)stream_base, &declaration_rows))
        && ABI_STAGE("declaration stream indexes",
            abi_stream_advance(&stream_base,
                declaration_rows.stream_index_count));
    function_stream_base = stream_base;
    valid = valid && ABI_STAGE("function rows",
            n00bb_abi_function_rows_build(type_index, declarations,
                &type_rows, &signature_rows,
                (uint32_t)signature_stream_base,
                (uint32_t)function_stream_base,
                (ir_module_t *)runtime_ir, pool, lookup, lookup_context,
                &function_rows))
        && ABI_STAGE("function stream indexes",
            abi_stream_advance(&stream_base,
                function_rows.stream_index_count))
        && ABI_STAGE("frame layouts",
            build_frame_rows(&function_rows, type_index, &type_rows,
                             (ir_module_t *)runtime_ir, &frame_rows))
        && ABI_STAGE("trait methods",
            n00bb_abi_trait_method_rows_build(type_index, &type_rows,
                declarations, th_ctx_environment(ir_module_types(
                    (ir_module_t *)runtime_ir)), &trait_method_rows));
    if (valid) {
        failed_stage = "imported type requirements";
        valid = type_rows.count <= UINT32_MAX - requirement_count - 1
            && reference_count
                <= UINT32_MAX - requirement_count - type_rows.count - 1
            && type_rows.count <= (SIZE_MAX / sizeof(*requirements))
                - requirement_count - 1
            && reference_count <= (SIZE_MAX / sizeof(*requirements))
                - requirement_count - type_rows.count - 1;
        if (valid) {
            requirements = core_realloc(requirements,
                (requirement_count + type_rows.count + reference_count + 1)
                    * sizeof(*requirements));
            valid = append_imported_type_requirements(type_index,
                ir_module_types((ir_module_t *)runtime_ir), &type_rows,
                unit, node_source, source_context,
                type_owner, type_owner_context, pool, requirements,
                &requirement_count);
        }
    }
    valid = valid && ABI_STAGE("type constraints",
            n00bb_abi_constraint_rows_build(type_index, &type_rows,
                declarations, &function_rows, pool, (uint32_t)stream_base,
                (uint32_t)signature_rows.parameter_count,
                &constraint_rows))
        && ABI_STAGE("constraint stream indexes",
            abi_stream_advance(&stream_base,
                constraint_rows.stream_index_count))
        && ABI_STAGE("static storage",
            n00bb_abi_storage_rows_build(type_index, declarations,
                &type_rows, &function_rows, &baked_functions,
                (ir_module_t *)runtime_ir, pool, (uint32_t)stream_base,
                lookup, lookup_context, graphs, &storage_rows));
#undef ABI_STAGE
    if (valid) {
        modules[1].initializer = function_rows.initializer_index;
    }
    if (valid) {
        failed_stage = "index table";
        index_table.symbol = index_table_symbol(unit->module.identifier,
                                                target->object_format);
        valid = index_table.symbol && append_index_table(&storage_rows,
            &type_rows, &function_rows, &index_table_offset, &index_table);
    }
    if (valid) {
        failed_stage = "trait method membership";
        valid = abi_stream_advance(&stream_base, storage_rows.stream_count)
            && n00bb_abi_trait_method_rows_attach(&type_rows,
                &trait_method_rows, (uint32_t)stream_base);
    }
    if (valid) {
        failed_stage = "imported variable requirements";
        valid = append_imported_symbol_requirements(link, unit,
            type_index, ir_module_types((ir_module_t *)runtime_ir),
            &type_rows, pool, requirements, &requirement_count,
            N00B_ABI_SYMBOL_VARIABLE);
    }
    if (valid && !n00bb_abi_type_variables_validate(&type_rows,
                                                    &typevar_failure)) {
        failed_stage = typevar_failure ? typevar_failure
                                       : "type variable validation";
        valid = false;
    }
    if (valid) {
        modules[1].static_storage = storage_rows.static_range;
        modules[1].tls_storage = storage_rows.tls_range;
    }
    if (valid) failed_stage = "package and module names";
    valid = valid && n00bb_abi_name_pool_encode(pool, package_id,
        N00B_ABI_TABLE_PACKAGE, 1, offsetof(n00b_abi_package_v1, name),
        &packages[1].name)
        && n00bb_abi_name_pool_encode(pool, unit->module.identifier,
            N00B_ABI_TABLE_MODULE, 1,
            offsetof(n00b_abi_module_v1, full_path), &modules[1].full_path);
    for (size_t i = 0; valid && i < import_count; i++) {
        failed_stage = "import names";
        core_translation_unit_t *imported = core_translation_unit_import(
            unit, i);
        valid = imported && imported->module.identifier.data
            && n00bb_abi_name_pool_encode(pool,
                imported->module.identifier, N00B_ABI_TABLE_REQUIREMENT,
                (uint32_t)i + 1,
                offsetof(n00b_abi_requirement_v1, full_path),
                &requirements[i + 1].full_path);
    }
    if (valid) {
        failed_stage = "native bindings";
        valid = build_native_bindings(&storage_rows, declarations,
            &declaration_rows, pool, package_id, unit->module.identifier,
            target->object_format, &native_bindings);
    }
    size_t index_binding = 0;
    if (valid) {
        failed_stage = "index table binding";
        index_binding = native_bindings.count++;
        native_bindings.rows = core_realloc(native_bindings.rows,
            native_bindings.count * sizeof(*native_bindings.rows));
        native_bindings.names = core_realloc(native_bindings.names,
            native_bindings.count * sizeof(*native_bindings.names));
        native_bindings.rows[index_binding] = (n00b_abi_native_binding_v1){
            .kind = N00B_ABI_NATIVE_SECTION_OFFSET,
            .offset = (int64_t)index_table_offset,
        };
        native_bindings.names[index_binding] =
            core_strdup(index_table.symbol);
        valid = native_bindings.names[index_binding]
            && n00bb_abi_name_pool_encode(pool,
                core_string_from_cstr(native_bindings.names[index_binding]),
                N00B_ABI_TABLE_NATIVE_BINDING, (uint32_t)index_binding,
                offsetof(n00b_abi_native_binding_v1, native_name),
                &native_bindings.rows[index_binding].native_name);
    }
    if (valid) {
        failed_stage = "TLS metadata";
        valid = abi_stream_advance(&stream_base,
                trait_method_rows.stream_index_count)
            && build_tls_metadata(&storage_rows,
                n00bb_abi_name_pool_size(pool), (uint32_t)stream_base,
                &modules[1], &tls_metadata);
    }
    if (!valid) {
        core_buffer_t message = {0};
        core_buffer_printf(&message, "could not build ABI %s", failed_stage);
        if (!strcmp(failed_stage, "type identity rows")
            && type_rows.failed_type_index) {
            th_type *failed_type = n00bb_abi_type_index_at(type_index,
                type_rows.failed_type_index);
            const char *spelling = failed_type
                ? th_print(ir_module_types((ir_module_t *)runtime_ir),
                           failed_type) : nullptr;
            if (spelling) core_buffer_printf(&message, " for %s", spelling);
            if (type_rows.failed_reason)
                core_buffer_printf(&message, ": %s", type_rows.failed_reason);
        }
        if (!strcmp(failed_stage, "type layouts")
            && failed_layout_type != UINT32_MAX) {
            th_type *failed_type = n00bb_abi_type_index_at(type_index,
                failed_layout_type);
            const char *spelling = failed_type
                ? th_print(ir_module_types((ir_module_t *)runtime_ir),
                           failed_type) : nullptr;
            if (spelling) core_buffer_printf(&message, " for %s", spelling);
        }
        abi_unit_error(unit, message.data);
        core_buffer_release(&message);
        goto cleanup;
    }

    n00bb_abi_type_index_free(type_index);
    type_index = nullptr;

    valid = abi_stream_advance(&stream_base, tls_metadata.stream_count);
    size_t frame_stream_base = stream_base;
    valid = valid && abi_stream_advance(&stream_base,
                                        frame_rows.active_count);
    if (!valid) {
        abi_unit_error(unit, "ABI stream index range exceeds 32 bits");
        goto cleanup;
    }
    size_t stream_count = stream_base;
    stream = core_alloc_array(uint32_t, stream_count);
    uint32_t prefix_values[3] = {package_modules[0], 1, scc_ordinal};
    const struct {
        const uint32_t *indexes;
        size_t count;
    } pieces[] = {
        {prefix_values, prefix_stream_count},
        {components.indexes, components.count},
        {layout_rows.stream_indexes, layout_rows.stream_index_count},
        {enum_rows.stream_indexes, enum_rows.stream_index_count},
        {signature_rows.parameter_indexes,
         signature_rows.parameter_index_count},
        {declaration_rows.stream_indexes,
         declaration_rows.stream_index_count},
        {function_rows.stream_indexes, function_rows.stream_index_count},
        {constraint_rows.stream_indexes, constraint_rows.stream_index_count},
        {storage_rows.stream_indexes, storage_rows.stream_count},
        {trait_method_rows.stream_indexes,
         trait_method_rows.stream_index_count},
        {tls_metadata.stream_indexes, tls_metadata.stream_count},
    };
    size_t stream_ordinal = 0;
    for (size_t i = 0; i < sizeof(pieces) / sizeof(*pieces); i++) {
        if (pieces[i].count)
            memcpy(stream + stream_ordinal, pieces[i].indexes,
                   pieces[i].count * sizeof(*stream));
        stream_ordinal += pieces[i].count;
    }
    if (stream_ordinal != frame_stream_base) {
        valid = false;
        abi_unit_error(unit, "ABI stream sections do not match their indexes");
        goto cleanup;
    }
    size_t frame_ordinal = 0;
    for (size_t i = 1; i < function_rows.function_count; i++) {
        uint32_t frame_index = frame_rows.function_frames[i];
        if (!frame_index) continue;
        size_t position = frame_stream_base + frame_ordinal++;
        function_rows.functions[i].frame_layouts = (n00b_abi_range_v1){
            .first = (uint32_t)position, .count = 1,
        };
        stream[position] = frame_index;
    }
    uint32_t storage_input_indexes[4] = {0};
    uint32_t aggregate_input_index = 0;
    uint32_t tls_descriptor_input_index = 0;
    core_object_emit_symbol_t string_symbol = {0};
    core_object_emit_symbol_t aggregate_symbol = {0};
    bool has_pool = n00bb_abi_name_pool_size(pool) != 0;
    bool has_aggregates = storage_rows.aggregates.byte_len != 0;
    if (has_pool) {
        core_list_push(extra_sections, ((core_object_emit_section_t){
            .name = target->object_format == CORE_TARGET_OBJECT_MACH_O
                ? "__n00b_strings"
                : target->object_format == CORE_TARGET_OBJECT_COFF
                    ? ".rdata$S" : ".rodata.n00b_strings",
            .segment = target->object_format == CORE_TARGET_OBJECT_MACH_O
                ? "__TEXT" : nullptr,
            .bytes = n00bb_abi_name_pool_bytes(pool),
            .size = n00bb_abi_name_pool_size(pool),
            .alignment = 1,
        }));
        string_symbol = (core_object_emit_symbol_t){
            .name = "n00b_string_pool", .section_index = 2,
        };
    }
    if (has_aggregates) {
        aggregate_input_index = (uint32_t)(2 + extra_sections.len);
        core_list_push(extra_sections, ((core_object_emit_section_t){
            .name = target->object_format == CORE_TARGET_OBJECT_MACH_O
                ? "__n00b_aggregate"
                : target->object_format == CORE_TARGET_OBJECT_COFF
                    ? ".data$A" : ".data.n00b_aggregate",
            .segment = target->object_format == CORE_TARGET_OBJECT_MACH_O
                ? "__DATA" : nullptr,
            .bytes = (const uint8_t *)storage_rows.aggregates.data,
            .size = storage_rows.aggregates.byte_len,
            .alignment = storage_rows.aggregate_alignment,
            .writable = true,
        }));
        aggregate_symbol = (core_object_emit_symbol_t){
            .name = "n00b_aggregate_pool",
            .section_index = aggregate_input_index,
        };
    }
        valid = append_storage_section(target, N00B_ABI_STORAGE_READ_ONLY,
            &storage_rows.read_only, storage_rows.read_only_alignment,
            &extra_sections, storage_input_indexes)
        && append_storage_section(target, N00B_ABI_STORAGE_WRITABLE,
            &storage_rows.writable, storage_rows.writable_alignment,
            &extra_sections, storage_input_indexes)
        && append_storage_section(target, N00B_ABI_STORAGE_TLS,
            &storage_rows.tls, storage_rows.tls_alignment,
            &extra_sections, storage_input_indexes);
    if (valid && target->object_format == CORE_TARGET_OBJECT_MACH_O) {
        for (size_t i = 1; i < native_bindings.storage_count; i++)
            tls_descriptor_count += storage_rows.rows[i].segment_kind
                == N00B_ABI_STORAGE_TLS;
        valid = tls_descriptor_count <= SIZE_MAX / 24
            && tls_descriptor_count <= INT64_MAX / 24
            && (!tls_descriptor_count || extra_sections.len < UINT32_MAX - 2);
        if (valid && tls_descriptor_count) {
            tls_descriptors = core_alloc_array(uint8_t,
                tls_descriptor_count * 24);
            memset(tls_descriptors, 0, tls_descriptor_count * 24);
            tls_descriptor_input_index = (uint32_t)(2 + extra_sections.len);
            core_list_push(extra_sections, ((core_object_emit_section_t){
                .name = "__n00b_tlv", .segment = "__DATA",
                .bytes = tls_descriptors, .size = tls_descriptor_count * 24,
                .alignment = 8, .writable = true,
                .is_thread_local_descriptor = true,
            }));
        }
    }
    size_t tls_descriptor_ordinal = 0;
    for (size_t i = 1; valid && i < native_bindings.storage_count; i++) {
        n00b_abi_storage_v1 *row = &storage_rows.rows[i];
        uint32_t input = row->segment_kind <= N00B_ABI_STORAGE_TLS
            ? storage_input_indexes[row->segment_kind] : 0;
        valid = input && core_object_emit_native_section_index(target,
            1 + extra_sections.len, input, &row->segment_index);
        if (!valid) break;
        if (row->segment_kind == N00B_ABI_STORAGE_TLS
            && tls_descriptor_input_index) {
            native_bindings.rows[i].offset =
                (int64_t)(tls_descriptor_ordinal++ * 24);
            valid = core_object_emit_native_section_index(target,
                1 + extra_sections.len, tls_descriptor_input_index,
                &native_bindings.rows[i].section_index);
        } else {
            native_bindings.rows[i].section_index = row->segment_index;
        }
    }
    if (valid && index_binding)
        valid = storage_input_indexes[N00B_ABI_STORAGE_READ_ONLY]
            && core_object_emit_native_section_index(target,
                1 + extra_sections.len,
                storage_input_indexes[N00B_ABI_STORAGE_READ_ONLY],
                &native_bindings.rows[index_binding].section_index);
    for (size_t i = 1; valid && i < storage_rows.baked_node_count; i++) {
        n00b_abi_baked_node_v1 *row = &storage_rows.baked_nodes[i];
        uint32_t input = row->segment_kind <= N00B_ABI_STORAGE_TLS
            ? storage_input_indexes[row->segment_kind] : 0;
        valid = input && core_object_emit_native_section_index(target,
            1 + extra_sections.len, input, &row->segment_index);
    }
    for (uint32_t i = 0; valid && i < tls_metadata.string_storage_count; i++)
        valid = has_pool && core_object_emit_native_section_index(target,
            1 + extra_sections.len, 2,
            &storage_rows.rows[tls_metadata.string_storage_first_index + i]
                .segment_index);
    if (valid && tls_metadata.aggregate_storage_index)
        valid = aggregate_input_index && core_object_emit_native_section_index(
            target, 1 + extra_sections.len, aggregate_input_index,
            &storage_rows.rows[tls_metadata.aggregate_storage_index]
                .segment_index);
    if (valid && storage_rows.index_patch_count) {
        index_patches = core_alloc_array(n00b_abi_index_patch_v1,
            storage_rows.index_patch_count + 1);
        for (size_t i = 0; valid && i < storage_rows.index_patch_count; i++) {
            const n00bb_abi_storage_index_patch_t *source =
                &storage_rows.index_patches[i];
            uint32_t input = source->source_kind == N00BB_ABI_RELOC_AGGREGATE
                ? aggregate_input_index
                : source->source_kind <= N00B_ABI_STORAGE_TLS
                    ? storage_input_indexes[source->source_kind] : 0;
            uint32_t native = 0;
            valid = input && core_object_emit_native_section_index(target,
                1 + extra_sections.len, input, &native);
            if (valid) index_patches[i + 1] = (n00b_abi_index_patch_v1){
                .target_section_index = native,
                .referenced_table_kind = source->table_kind,
                .target_byte_offset = source->byte_offset,
                .local_index = source->local_index,
                .patch_kind = source->patch_kind,
            };
        }
    }
    size_t parameter_count = signature_rows.parameter_count
        + constraint_rows.call_parameter_count;
    valid = valid
        && constraint_rows.call_parameter_count
            <= SIZE_MAX - signature_rows.parameter_count
        && parameter_count <= UINT32_MAX;
    if (valid) {
        parameters = core_alloc_array(n00b_abi_parameter_v1,
                                      parameter_count);
        valid = parameters != nullptr;
    }
    if (valid) {
        memcpy(parameters, signature_rows.parameters,
               signature_rows.parameter_count * sizeof(*parameters));
        if (constraint_rows.call_parameter_count)
            memcpy(parameters + signature_rows.parameter_count,
                   constraint_rows.call_parameters,
                   constraint_rows.call_parameter_count
                       * sizeof(*parameters));
    }
    n00bb_abi_table_rows_t rows[] = {
        {N00B_ABI_TABLE_PACKAGE, packages, 2},
        {N00B_ABI_TABLE_MODULE, modules, 2},
        {N00B_ABI_TABLE_SCC, sccs, recursive ? 2 : 1},
        {N00B_ABI_TABLE_TYPE, type_rows.rows, type_rows.count},
        {N00B_ABI_TABLE_TYPE_ARGUMENT, type_rows.arguments,
         type_rows.argument_count},
        {N00B_ABI_TABLE_LAYOUT, layout_rows.layouts,
         layout_rows.layout_count},
        {N00B_ABI_TABLE_PHYSICAL_RUN, layout_rows.runs,
         layout_rows.run_count},
        {N00B_ABI_TABLE_PACKED_RUN, layout_rows.packed_runs,
         layout_rows.packed_run_count},
        {N00B_ABI_TABLE_FIELD, layout_rows.fields,
         layout_rows.field_count},
        {N00B_ABI_TABLE_ARM, layout_rows.arms,
         layout_rows.arm_count},
        {N00B_ABI_TABLE_TAG_EXTRACTOR, layout_rows.tag_extractors,
         layout_rows.tag_extractor_count},
        {N00B_ABI_TABLE_TRACE, layout_rows.traces,
         layout_rows.trace_count},
        {N00B_ABI_TABLE_TRACE_CHILD, layout_rows.trace_children,
         layout_rows.trace_child_count},
        {N00B_ABI_TABLE_TRACE_ARM, layout_rows.trace_arms,
         layout_rows.trace_arm_count},
        {N00B_ABI_TABLE_ENUM_ITEM, enum_rows.items,
         enum_rows.item_count},
        {N00B_ABI_TABLE_FUNCTION, function_rows.functions,
         function_rows.function_count},
        {N00B_ABI_TABLE_FRAME_LAYOUT, frame_rows.rows,
         frame_rows.row_count},
        {N00B_ABI_TABLE_TRAIT_METHOD, trait_method_rows.rows,
         trait_method_rows.count},
        {N00B_ABI_TABLE_CONSTRAINT, constraint_rows.rows,
         constraint_rows.count},
        {N00B_ABI_TABLE_CALL_SIGNATURE, signature_rows.signatures,
         signature_rows.signature_count},
        {N00B_ABI_TABLE_CALL_SHAPE, constraint_rows.call_shapes,
         constraint_rows.call_shape_count},
        {N00B_ABI_TABLE_PARAMETER, parameters, parameter_count},
        {N00B_ABI_TABLE_SCOPE, declaration_rows.scopes,
         declaration_rows.scope_count},
        {N00B_ABI_TABLE_SYMBOL, declaration_rows.symbols,
         declaration_rows.symbol_count},
        {N00B_ABI_TABLE_SOURCE_LOCATION, declaration_rows.locations,
         declaration_rows.location_count},
        {N00B_ABI_TABLE_SOURCE_FILE, declaration_rows.source_files,
         declaration_rows.source_file_count},
        {N00B_ABI_TABLE_RUNTIME_CONFIG, runtime_configs, 2},
        {N00B_ABI_TABLE_REQUIREMENT, requirements, requirement_count + 1},
        {N00B_ABI_TABLE_STORAGE, storage_rows.rows,
         storage_rows.row_count},
        {N00B_ABI_TABLE_TLS_RELOCATION, tls_metadata.rows,
         tls_metadata.row_count},
        {N00B_ABI_TABLE_BAKED_NODE, storage_rows.baked_nodes,
         storage_rows.baked_node_count},
        {N00B_ABI_TABLE_SHAPE, storage_rows.shapes,
         storage_rows.shape_count},
        {N00B_ABI_TABLE_U64_STREAM, storage_rows.extents,
         storage_rows.extent_count},
        {N00B_ABI_TABLE_NATIVE_BINDING, native_bindings.rows,
         native_bindings.count},
        {N00B_ABI_TABLE_U32_STREAM, stream, stream_count},
        {N00B_ABI_TABLE_BYTE_STREAM, frame_rows.bitmap,
         frame_rows.bitmap_count},
        {N00B_ABI_TABLE_INDEX_PATCH, index_patches,
         index_patches ? storage_rows.index_patch_count + 1 : 0},
    };
    valid = valid && n00bb_abi_pack_tables(target, artifact_capabilities,
        rows, sizeof(rows) / sizeof(*rows) - !index_patches, &packed);
    if (!valid) {
        abi_unit_error(unit, "could not pack ABI tables");
        goto cleanup;
    }

    n00bb_abi_image_key_t key;
    n00bb_abi_directory_entry_t entries[sizeof(rows) / sizeof(*rows)];
    n00bb_abi_image_header_t header;
    size_t schema_count = 0;
    const n00bb_abi_table_schema_t *schemas =
        n00bb_abi_v1_table_schemas(&schema_count);
    valid = n00bb_abi_image_key_for_target(target, &key)
        && n00bb_abi_container_read(packed.bytes, packed.byte_count,
            &key, schemas, schema_count, false, &header, entries,
            sizeof(entries) / sizeof(*entries));
    size_t relocation_count = 0;
    size_t storage_base_symbols =
        (storage_input_indexes[N00B_ABI_STORAGE_READ_ONLY] != 0)
        + (storage_input_indexes[N00B_ABI_STORAGE_WRITABLE] != 0);
    size_t storage_base_first = (has_pool ? 1 : 0)
        + (has_aggregates ? 1 : 0);
    const char *object_failure = "native object emission";
    size_t native_first = (has_pool ? 1 : 0) + (has_aggregates ? 1 : 0)
        + storage_base_symbols + native_bindings.storage_count - 1
        + (tls_descriptor_count ? tls_descriptor_count + 1 : 0);
    size_t native_count = 0;
    const char **native_names = storage_rows.relocation_count
        ? core_alloc_array(const char *, storage_rows.relocation_count)
        : nullptr;
    if (valid)
        valid = n00bb_abi_name_pool_relocations(pool, entries,
            sizeof(entries) / sizeof(*entries), &relocations,
            &relocation_count);
    size_t descriptor_relocation_count = tls_descriptor_count * 2;
    if (valid && (storage_rows.relocation_count
                  || descriptor_relocation_count)) {
        if (storage_rows.relocation_count > SIZE_MAX - relocation_count
            || descriptor_relocation_count > SIZE_MAX - relocation_count
                - storage_rows.relocation_count)
            valid = false;
        else {
            size_t total = relocation_count + storage_rows.relocation_count
                + descriptor_relocation_count;
            core_object_emit_absolute64_t *combined = core_alloc_array(
                core_object_emit_absolute64_t, total);
            if (relocation_count)
                memcpy(combined, relocations,
                       relocation_count * sizeof(*combined));
            for (size_t i = 0; i < storage_rows.relocation_count; i++) {
                const n00bb_abi_storage_relocation_t *source =
                    &storage_rows.relocations[i];
                uint32_t section = source->source_kind
                    <= N00B_ABI_STORAGE_TLS
                    ? storage_input_indexes[source->source_kind]
                    : source->source_kind == N00BB_ABI_RELOC_AGGREGATE
                        ? aggregate_input_index : 0;
                uint32_t target_symbol = source->target_kind
                    == N00BB_ABI_RELOC_STRING_POOL && has_pool ? 0
                    : source->target_kind == N00BB_ABI_RELOC_AGGREGATE
                        && has_aggregates ? (has_pool ? 1 : 0)
                    : source->target_kind == N00B_ABI_STORAGE_READ_ONLY
                        && storage_input_indexes[N00B_ABI_STORAGE_READ_ONLY]
                        ? (uint32_t)storage_base_first
                    : source->target_kind == N00B_ABI_STORAGE_WRITABLE
                        && storage_input_indexes[N00B_ABI_STORAGE_WRITABLE]
                        ? (uint32_t)(storage_base_first
                            + (storage_input_indexes[
                                N00B_ABI_STORAGE_READ_ONLY] != 0))
                        : UINT32_MAX;
                if (source->target_kind == N00BB_ABI_RELOC_NATIVE) {
                    size_t found = 0;
                    while (found < native_count
                        && strcmp(native_names[found], source->native))
                        found++;
                    if (found == native_count)
                        native_names[native_count++] = source->native;
                    combined[relocation_count + i] =
                        (core_object_emit_absolute64_t){
                            .section_index = section,
                            .section_offset = source->byte_offset,
                            .symbol_index = (uint32_t)(native_first + found),
                        };
                    if (!section) {
                        object_failure = "native symbol relocation source";
                        valid = false;
                        break;
                    }
                    continue;
                }
                size_t target_size = source->target_kind
                    == N00BB_ABI_RELOC_STRING_POOL
                    ? n00bb_abi_name_pool_size(pool)
                    : source->target_kind == N00BB_ABI_RELOC_AGGREGATE
                        ? storage_rows.aggregates.byte_len
                    : source->target_kind == N00B_ABI_STORAGE_READ_ONLY
                        ? storage_rows.read_only.byte_len
                    : source->target_kind == N00B_ABI_STORAGE_WRITABLE
                        ? storage_rows.writable.byte_len : 0;
                if (!section || target_symbol == UINT32_MAX
                    || source->target_offset >= target_size) {
                    object_failure = "static storage relocation target";
                    valid = false;
                    break;
                }
                combined[relocation_count + i] =
                    (core_object_emit_absolute64_t){
                        .section_index = section,
                        .section_offset = source->byte_offset,
                        .symbol_index = target_symbol,
                        .addend_bits = source->target_offset,
                    };
            }
            size_t ordinal = 0;
            size_t template_symbols = (has_pool ? 1 : 0)
                + (has_aggregates ? 1 : 0) + storage_base_symbols
                + native_bindings.storage_count - 1;
            size_t bootstrap_symbol = template_symbols
                + tls_descriptor_count;
            for (size_t i = 1; valid && i < native_bindings.storage_count;
                 i++) {
                const n00b_abi_storage_v1 *row = &storage_rows.rows[i];
                if (row->segment_kind != N00B_ABI_STORAGE_TLS) continue;
                size_t at = relocation_count + storage_rows.relocation_count
                    + ordinal * 2;
                combined[at] = (core_object_emit_absolute64_t){
                    .section_index = tls_descriptor_input_index,
                    .section_offset = ordinal * 24,
                    .symbol_index = (uint32_t)bootstrap_symbol,
                };
                combined[at + 1] = (core_object_emit_absolute64_t){
                    .section_index = tls_descriptor_input_index,
                    .section_offset = ordinal * 24 + 16,
                    .symbol_index = (uint32_t)(template_symbols + ordinal),
                    .symbol_relocation = true,
                };
                ordinal++;
            }
            core_free(relocations);
            relocations = combined;
            relocation_count = total;
        }
    }
    size_t symbol_count = (has_pool ? 1 : 0)
        + (has_aggregates ? 1 : 0)
        + storage_base_symbols
        + native_bindings.storage_count - 1
        + (tls_descriptor_count ? tls_descriptor_count + 1 : 0)
        + native_count;
    char **native_symbol_names = native_count
        ? core_alloc_array(char *, native_count) : nullptr;
    object_symbols = symbol_count
        ? core_alloc_array(core_object_emit_symbol_t, symbol_count)
        : nullptr;
    tls_template_names = tls_descriptor_count
        ? core_alloc_array(char *, tls_descriptor_count) : nullptr;
    if (has_pool) object_symbols[0] = string_symbol;
    if (has_aggregates) object_symbols[has_pool ? 1 : 0]
        = aggregate_symbol;
    size_t storage_base_ordinal = storage_base_first;
    if (storage_input_indexes[N00B_ABI_STORAGE_READ_ONLY])
        object_symbols[storage_base_ordinal++] =
            (core_object_emit_symbol_t){
                .name = "__n00bb_read_only_storage_base",
                .section_index =
                    storage_input_indexes[N00B_ABI_STORAGE_READ_ONLY],
            };
    if (storage_input_indexes[N00B_ABI_STORAGE_WRITABLE])
        object_symbols[storage_base_ordinal++] =
            (core_object_emit_symbol_t){
                .name = "__n00bb_writable_storage_base",
                .section_index =
                    storage_input_indexes[N00B_ABI_STORAGE_WRITABLE],
            };
    for (size_t i = 1; i < native_bindings.storage_count; i++) {
        const n00b_abi_storage_v1 *row = &storage_rows.rows[i];
        object_symbols[storage_base_first + storage_base_symbols + i - 1] =
            (core_object_emit_symbol_t){
                .name = native_bindings.names[i],
                .section_index = row->segment_kind == N00B_ABI_STORAGE_TLS
                    && tls_descriptor_input_index
                    ? tls_descriptor_input_index
                    : storage_input_indexes[row->segment_kind],
                .section_offset = (uint64_t)native_bindings.rows[i].offset,
                .exported = true,
            };
    }
    if (tls_descriptor_count) {
        size_t ordinal = 0;
        for (size_t i = 1; i < native_bindings.storage_count; i++) {
            const n00b_abi_storage_v1 *row = &storage_rows.rows[i];
            if (row->segment_kind != N00B_ABI_STORAGE_TLS) continue;
            const char *native_name = native_bindings.names[i];
            size_t length = strlen(native_name);
            tls_template_names[ordinal] = core_alloc_array(char,
                length + sizeof("$tlv$init"));
            memcpy(tls_template_names[ordinal], native_name, length);
            memcpy(tls_template_names[ordinal] + length, "$tlv$init",
                sizeof("$tlv$init"));
            object_symbols[symbol_count - native_count
                           - tls_descriptor_count - 1
                           + ordinal] = (core_object_emit_symbol_t){
                .name = tls_template_names[ordinal],
                .section_index = storage_input_indexes[N00B_ABI_STORAGE_TLS],
                .section_offset = row->byte_offset,
            };
            ordinal++;
        }
        object_symbols[symbol_count - native_count - 1] =
            (core_object_emit_symbol_t){
                .name = "__tlv_bootstrap", .exported = true,
            };
    }
    bool underscore = target->object_format == CORE_TARGET_OBJECT_MACH_O;
    for (size_t i = 0; valid && i < native_count; i++) {
        size_t length = strlen(native_names[i]);
        native_symbol_names[i] = core_alloc_array(char, length + 2);
        if (underscore) native_symbol_names[i][0] = '_';
        memcpy(native_symbol_names[i] + underscore, native_names[i],
               length + 1);
        object_symbols[native_first + i] = (core_object_emit_symbol_t){
            .name = native_symbol_names[i], .exported = true,
        };
    }
    if (valid)
        valid = n00bb_abi_write_object(target, &packed,
            extra_sections.data, extra_sections.len,
            object_symbols, symbol_count,
            relocations, relocation_count,
            &object, &object_bytes);
    for (size_t i = 0; native_symbol_names && i < native_count; i++)
        core_free(native_symbol_names[i]);
    core_free(native_symbol_names);
    core_free(native_names);
    if (!valid) {
        core_buffer_t message = {0};
        core_buffer_printf(&message, "could not write ABI object: %s",
            object_failure);
        abi_unit_error(unit, message.data);
        core_buffer_release(&message);
    }
cleanup:
    core_free(index_patches);
    core_free(parameters);
    core_free(stream);
    core_list_free(extra_sections);
    core_free(tls_descriptors);
    if (tls_template_names)
        for (size_t i = 0; i < tls_descriptor_count; i++)
            core_free(tls_template_names[i]);
    core_free(tls_template_names);
    core_free(object_symbols);
    core_free(relocations);
    core_free(requirements);
    native_bindings_release(&native_bindings);
    if (valid && out_index_table) *out_index_table = index_table;
    else n00bb_abi_index_table_release(&index_table);
    tls_metadata_release(&tls_metadata);
    n00bb_abi_storage_rows_release(&storage_rows);
    n00bb_abi_baked_functions_release(&baked_functions);
    n00bb_abi_trait_method_rows_release(&trait_method_rows);
    n00bb_abi_constraint_rows_release(&constraint_rows);
    frame_rows_release(&frame_rows);
    n00bb_abi_function_rows_release(&function_rows);
    n00bb_abi_declarations_release(&declaration_rows);
    n00bb_abi_type_signatures_release(&signature_rows);
    n00bb_abi_enum_rows_release(&enum_rows);
    n00bb_abi_type_layout_rows_release(&layout_rows);
    n00bb_abi_type_components_release(&components);
    n00bb_abi_type_identity_rows_release(&type_rows);
    n00bb_abi_name_pool_free(pool);
    n00bb_abi_decl_index_free(declarations);
    n00bb_abi_type_index_free(type_index);
    if (!valid) {
        n00bb_abi_packed_tables_release(&packed);
        core_free(object);
        return false;
    }
    *out_tables = packed;
    *out_object = object;
    *out_object_bytes = object_bytes;
    return true;
}
