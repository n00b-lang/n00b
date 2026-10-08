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
#include "n00b-abi-v1.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/byteorder.h"
#include "lib/diagnostic.h"
#include "lib/hash.h"
#include "lib/indexed_table.h"
#include "lib/list.h"
#include "lib/object_section.h"
#include "ir/typelang.h"
#include "parse/tree_util.h"

core_list_decl(core_object_emit_section_t);

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
    abi_native_bindings_t built = {
        .rows = core_alloc_array(n00b_abi_native_binding_v1,
                                 storage->row_count),
        .names = core_alloc_array(char *, storage->row_count),
        .count = storage->row_count,
    };
    memset(built.rows, 0, built.count * sizeof(*built.rows));
    memset(built.names, 0, built.count * sizeof(*built.names));
    bool valid = true;
    for (size_t i = 1; valid && i < built.count; i++) {
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
phase_has_only_trivial_initializer(const ir_module_t *module)
{
    for (size_t i = 0; i < ir_func_count(module); i++) {
        ir_func_t *function = ir_func_at((ir_module_t *)module, i);
        if (!function) return false;
        if (!ir_func_is_initializer(function)) continue;
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
publish_catalog_direct_layouts(n00bb_abi_type_index_t *index,
                               ir_module_t *comptime, ir_module_t *runtime,
                               const core_target_desc_t *target)
{
    ir_module_t *modules[] = {comptime, runtime};
    for (size_t m = 0; m < sizeof(modules) / sizeof(*modules); m++)
        for (size_t i = 0; i < n00bb_abi_type_index_count(index); i++) {
            th_type *type = n00bb_abi_type_index_at(index, (uint32_t)i);
            bool handled = false;
            if (!type || !n00bb_abi_publish_direct_layout(modules[m], type,
                    target, &handled)) return false;
        }
    return true;
}

static bool
append_imported_type_requirements(
    n00bb_abi_type_index_t *index,
    const n00bb_abi_type_identity_rows_t *types,
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
        if (!owner || !owner->decl_node) continue;
        const core_translation_unit_t *declaring_unit = nullptr;
        parse_token_stream_t *declaring_tokens = nullptr;
        if (!node_source(source_context, owner->decl_node,
                &declaring_unit, &declaring_tokens)
            || !declaring_unit || !declaring_tokens
            || declaring_unit == unit || !owner->name.data
            || !owner->name.u8_bytes || !owner->phases) return false;
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
            .required_phase_mask = (uint32_t)owner->phases,
            .requesting_module_index = 1,
        };
        bool valid = n00bb_abi_name_pool_encode(pool,
            core_string_borrow(path.data, path.byte_len),
            N00B_ABI_TABLE_REQUIREMENT, (uint32_t)next,
            offsetof(n00b_abi_requirement_v1, full_path),
            &requirements[next].full_path);
        core_buffer_release(&path);
        if (!valid) return false;
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
    uint32_t kind = core_link_declaration_kind(target);
    if (kind != PARSE_SYM_FUNCTION && kind != PARSE_SYM_VARIABLE
        && kind != PARSE_SYM_TYPEDEF)
        return true;
    parse_node_resolution_t resolution = parse_tree_resolution(
        core_link_reference_use_site(reference));
    if (resolution.kind != PARSE_NODE_RESOLUTION_SYMBOL
        || !resolution.target.symbol
        || resolution.target.symbol->kind != kind)
        return false;
    parse_sym_entry_t *symbol = resolution.target.symbol;
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
        core_link_declaration_t *target = core_link_reference_target(reference);
        bool inserted = false;
        if (!core_indexed_table_intern(seen, target, target, &inserted)) {
            valid = false;
            break;
        }
        if (!inserted) continue;
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
            .required_phase_mask = (uint32_t)symbol->phases,
            .requesting_module_index = 1,
        };
        valid = n00bb_abi_name_pool_encode(pool,
            core_string_borrow(path.data, path.byte_len),
            N00B_ABI_TABLE_REQUIREMENT, (uint32_t)next,
            offsetof(n00b_abi_requirement_v1, full_path),
            &requirements[next].full_path);
        core_buffer_release(&path);
        if (valid && kind == N00B_ABI_SYMBOL_FUNCTION)
            valid = n00bb_abi_type_variables_bind_import(index,
                type_context, symbol, type, (uint32_t)next, types);
    }
    core_indexed_table_free(seen);
    return valid;
}

typedef struct {
    core_translation_unit_t **members;
    size_t count;
    bool *visited;
    size_t *order;
    size_t ordered;
} abi_scc_order_t;

static bool
module_name_less(core_string_t left, core_string_t right)
{
    size_t common = left.u8_bytes < right.u8_bytes
        ? left.u8_bytes : right.u8_bytes;
    int comparison = memcmp(left.data, right.data, common);
    return comparison < 0 || (!comparison && left.u8_bytes < right.u8_bytes);
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
visit_scc_member(abi_scc_order_t *group, size_t at)
{
    group->visited[at] = true;
    group->order[group->ordered++] = at;
    core_translation_unit_t *unit = group->members[at];
    for (size_t i = 0; i < core_translation_unit_import_count(unit); i++) {
        core_translation_unit_t *imported = core_translation_unit_import(
            unit, i);
        for (size_t j = 0; j < group->count; j++) {
            if (group->members[j] != imported) continue;
            if (!group->visited[j] && !visit_scc_member(group, j))
                return false;
            break;
        }
    }
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
    size_t group_index = core_link_unit_scc_count(link);
    size_t count = 0;
    for (size_t i = 0; i < group_index; i++) {
        size_t members = core_link_unit_scc_member_count(link, i);
        for (size_t j = 0; j < members; j++) {
            if (core_link_unit_scc_member(link, i, j) != unit) continue;
            group_index = i;
            count = members;
            break;
        }
        if (count) break;
    }
    if (!count || count >= UINT32_MAX) return false;
    if (count == 1) {
        bool self_import = false;
        for (size_t i = 0; i < core_translation_unit_import_count(unit); i++)
            self_import |= core_translation_unit_import(unit, i) == unit;
        if (!self_import) return true;
    }
    core_translation_unit_t **members = core_alloc_array(
        core_translation_unit_t *, count);
    bool *visited = core_alloc_array(bool, count);
    size_t *order = core_alloc_array(size_t, count);
    bool valid = members && visited && order;
    size_t first = 0;
    for (size_t i = 0; valid && i < count; i++) {
        members[i] = core_link_unit_scc_member(link, group_index, i);
        if (!members[i] || !members[i]->module.identifier.data
            || !core_translation_unit_imports_finalized(members[i])) {
            valid = false;
            break;
        }
        if (module_name_less(members[i]->module.identifier,
                             members[first]->module.identifier)) first = i;
    }
    abi_scc_order_t group = {
        .members = members, .count = count,
        .visited = visited, .order = order,
    };
    if (valid) valid = visit_scc_member(&group, first)
        && group.ordered == count;
    core_buffer_t input = {0};
    for (size_t i = 0; valid && i < count; i++) {
        core_translation_unit_t *member = members[order[i]];
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
    core_free(order);
    core_free(visited);
    core_free(members);
    return valid;
}

bool
n00bb_abi_emit_unit(
    core_link_unit_t *link, core_translation_unit_t *unit,
    core_string_t package_id,
    const parse_symtab_t *symbols, parse_token_stream_t *tokens,
    const ir_module_t *comptime_ir, const ir_module_t *runtime_ir,
    const core_target_desc_t *target, uint64_t artifact_capabilities,
    n00bb_abi_declaration_lookup_fn lookup, void *lookup_context,
    n00bb_abi_node_source_fn node_source, void *source_context,
    n00bb_abi_type_owner_lookup_fn type_owner, void *type_owner_context,
    n00bb_abi_packed_tables_t *out_tables,
    uint8_t **out_object, size_t *out_object_bytes)
{
    if (!unit || !symbols || !tokens || !comptime_ir || !runtime_ir || !target
        || !target->triple || !unit->module.identifier.data
        || !out_tables || !out_object || !out_object_bytes
        || !package_id.data || core_translation_unit_has_fatal_diagnostics(unit))
        return false;
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
            && !callback && !alias
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
    if (!core_translation_unit_imports_finalized(unit)
        || !phase_has_only_trivial_initializer(comptime_ir)
        || !phase_has_only_trivial_initializer(runtime_ir)) {
        n00bb_abi_decl_index_free(declarations);
        return abi_unit_error(unit,
            "this ABI emitter path requires no executable initializer body");
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
        || !ir_module_target(runtime_ir) || !ir_module_target(comptime_ir)
        || strcmp(ir_module_target(runtime_ir), target->triple)
        || strcmp(ir_module_target(comptime_ir), target->triple)) {
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
    n00bb_abi_trait_method_rows_t trait_method_rows = {0};
    n00bb_abi_constraint_rows_t constraint_rows = {0};
    n00bb_abi_storage_rows_t storage_rows = {0};
    abi_tls_metadata_t tls_metadata = {0};
    abi_native_bindings_t native_bindings = {0};
    size_t reference_count = link ? core_link_unit_reference_count(link) : 0;
    bool requirement_capacity_valid = requirements
        && reference_count <= UINT32_MAX - import_count - 1
        && reference_count <= (SIZE_MAX / sizeof(*requirements))
            - import_count - 1;
    if (requirement_capacity_valid)
        requirements = core_realloc(requirements,
            (import_count + reference_count + 1) * sizeof(*requirements));
    bool valid = pool && type_index && requirement_capacity_valid
        && n00bb_abi_type_index_collect_module_types(type_index,
            (ir_module_t *)comptime_ir)
        && n00bb_abi_type_index_collect_module_types(type_index,
            (ir_module_t *)runtime_ir)
        && n00bb_abi_type_index_collect_symbols(type_index, symbols)
        && collect_imported_symbol_types(link, unit, type_index)
        && publish_catalog_direct_layouts(type_index,
            (ir_module_t *)comptime_ir, (ir_module_t *)runtime_ir, target)
        && n00bb_abi_type_identity_rows_build(type_index,
            (ir_module_t *)comptime_ir, (ir_module_t *)runtime_ir,
            symbols, type_owner, type_owner_context, pool, &type_rows)
        && append_imported_symbol_requirements(link, unit, type_index,
            ir_module_types((ir_module_t *)runtime_ir), &type_rows, pool,
            requirements, &requirement_count, N00B_ABI_SYMBOL_FUNCTION)
        && n00bb_abi_type_variables_bind(type_index,
            ir_module_types((ir_module_t *)runtime_ir), declarations,
            &type_rows)
        && n00bb_abi_type_components_build(type_index,
            (ir_module_t *)runtime_ir, prefix_stream_count,
            &type_rows, &components)
        && n00bb_abi_type_layout_rows_build(type_index,
            (ir_module_t *)comptime_ir, (ir_module_t *)runtime_ir,
            prefix_stream_count + (uint32_t)components.count,
            declarations, pool,
            &type_rows, &layout_rows)
        && n00bb_abi_enum_rows_build(type_index, declarations, pool,
            prefix_stream_count + (uint32_t)components.count
                + (uint32_t)layout_rows.stream_index_count,
            &type_rows, &enum_rows)
        && n00bb_abi_type_signatures_build(type_index, &type_rows, pool,
            prefix_stream_count + (uint32_t)components.count
                + (uint32_t)layout_rows.stream_index_count
                + (uint32_t)enum_rows.stream_index_count,
            &signature_rows)
        && n00bb_abi_declarations_build(declarations, type_index,
            &type_rows, unit, tokens, pool, node_source, source_context,
            prefix_stream_count + (uint32_t)components.count
                + (uint32_t)layout_rows.stream_index_count
                + (uint32_t)enum_rows.stream_index_count
                + (uint32_t)signature_rows.parameter_index_count,
            &declaration_rows)
        && n00bb_abi_function_rows_build(type_index, declarations,
            &type_rows, &signature_rows,
            prefix_stream_count + (uint32_t)components.count
                + (uint32_t)layout_rows.stream_index_count
                + (uint32_t)enum_rows.stream_index_count,
            prefix_stream_count + (uint32_t)components.count
                + (uint32_t)layout_rows.stream_index_count
                + (uint32_t)enum_rows.stream_index_count
                + (uint32_t)signature_rows.parameter_index_count
                + (uint32_t)declaration_rows.stream_index_count,
            (ir_module_t *)comptime_ir, (ir_module_t *)runtime_ir,
            pool, lookup, lookup_context, &function_rows)
        && n00bb_abi_trait_method_rows_build(type_index, &type_rows,
            declarations, &trait_method_rows)
        && n00bb_abi_constraint_rows_build(type_index, &type_rows,
            declarations, &function_rows, pool,
            prefix_stream_count + (uint32_t)components.count
                + (uint32_t)layout_rows.stream_index_count
                + (uint32_t)enum_rows.stream_index_count
                + (uint32_t)signature_rows.parameter_index_count
                + (uint32_t)declaration_rows.stream_index_count
                + (uint32_t)function_rows.stream_index_count,
            &constraint_rows)
        && n00bb_abi_storage_rows_build(type_index, declarations,
            &type_rows, (ir_module_t *)runtime_ir, pool,
            prefix_stream_count + (uint32_t)components.count
                + (uint32_t)layout_rows.stream_index_count
                + (uint32_t)enum_rows.stream_index_count
                + (uint32_t)signature_rows.parameter_index_count
                + (uint32_t)declaration_rows.stream_index_count
                + (uint32_t)function_rows.stream_index_count
                + (uint32_t)constraint_rows.stream_index_count,
            lookup, lookup_context, &storage_rows);
    if (valid) {
        size_t method_stream_base = prefix_stream_count + components.count
            + layout_rows.stream_index_count + enum_rows.stream_index_count
            + signature_rows.parameter_index_count
            + declaration_rows.stream_index_count
            + function_rows.stream_index_count
            + constraint_rows.stream_index_count
            + storage_rows.stream_count;
        valid = method_stream_base <= UINT32_MAX
            && n00bb_abi_trait_method_rows_attach(&type_rows,
                &trait_method_rows, (uint32_t)method_stream_base);
    }
    if (valid) {
        valid = type_rows.count <= UINT32_MAX - import_count - 1
            && reference_count <= UINT32_MAX - import_count - type_rows.count - 1
            && type_rows.count <= (SIZE_MAX / sizeof(*requirements))
                - import_count - 1
            && reference_count <= (SIZE_MAX / sizeof(*requirements))
                - import_count - type_rows.count - 1;
        if (valid) {
            requirements = core_realloc(requirements,
                (import_count + type_rows.count + reference_count + 1)
                    * sizeof(*requirements));
            valid = append_imported_type_requirements(type_index,
                &type_rows, unit, node_source, source_context,
                type_owner, type_owner_context, pool, requirements,
                &requirement_count)
                && append_imported_symbol_requirements(link, unit,
                    type_index, ir_module_types((ir_module_t *)runtime_ir),
                    &type_rows, pool, requirements, &requirement_count,
                    N00B_ABI_SYMBOL_VARIABLE);
        }
    }
    n00bb_abi_type_index_free(type_index);
    if (valid) {
        modules[1].static_storage = storage_rows.static_range;
        modules[1].tls_storage = storage_rows.tls_range;
    }
    valid = valid && n00bb_abi_name_pool_encode(pool, package_id,
        N00B_ABI_TABLE_PACKAGE, 1, offsetof(n00b_abi_package_v1, name),
        &packages[1].name)
        && n00bb_abi_name_pool_encode(pool, unit->module.identifier,
            N00B_ABI_TABLE_MODULE, 1,
            offsetof(n00b_abi_module_v1, full_path), &modules[1].full_path);
    for (size_t i = 0; valid && i < import_count; i++) {
        core_translation_unit_t *imported = core_translation_unit_import(
            unit, i);
        valid = imported && imported->module.identifier.data
            && n00bb_abi_name_pool_encode(pool,
                imported->module.identifier, N00B_ABI_TABLE_REQUIREMENT,
                (uint32_t)i + 1,
                offsetof(n00b_abi_requirement_v1, full_path),
                &requirements[i + 1].full_path);
    }
    if (valid)
        valid = build_native_bindings(&storage_rows, declarations,
            &declaration_rows, pool, package_id, unit->module.identifier,
            target->object_format, &native_bindings);
    if (valid) {
        size_t base = prefix_stream_count + components.count
            + layout_rows.stream_index_count + enum_rows.stream_index_count
            + signature_rows.parameter_index_count
            + declaration_rows.stream_index_count
            + function_rows.stream_index_count
            + constraint_rows.stream_index_count + storage_rows.stream_count
            + trait_method_rows.stream_index_count;
        valid = base <= UINT32_MAX && build_tls_metadata(&storage_rows,
            n00bb_abi_name_pool_size(pool), (uint32_t)base, &modules[1],
            &tls_metadata);
    }
    if (!valid) {
        core_free(requirements);
        native_bindings_release(&native_bindings);
        tls_metadata_release(&tls_metadata);
        n00bb_abi_storage_rows_release(&storage_rows);
        n00bb_abi_trait_method_rows_release(&trait_method_rows);
        n00bb_abi_constraint_rows_release(&constraint_rows);
        n00bb_abi_function_rows_release(&function_rows);
        n00bb_abi_declarations_release(&declaration_rows);
        n00bb_abi_type_signatures_release(&signature_rows);
        n00bb_abi_enum_rows_release(&enum_rows);
        n00bb_abi_type_layout_rows_release(&layout_rows);
        n00bb_abi_type_components_release(&components);
        n00bb_abi_type_identity_rows_release(&type_rows);
        n00bb_abi_name_pool_free(pool);
        n00bb_abi_decl_index_free(declarations);
        return abi_unit_error(unit,
            "could not build ABI type, declaration, or storage rows");
    }

    size_t stream_count = prefix_stream_count + components.count
        + layout_rows.stream_index_count
        + enum_rows.stream_index_count
        + signature_rows.parameter_index_count
        + declaration_rows.stream_index_count
        + function_rows.stream_index_count
        + constraint_rows.stream_index_count
        + storage_rows.stream_count
        + trait_method_rows.stream_index_count
        + tls_metadata.stream_count;
    uint32_t *stream = core_alloc_array(uint32_t, stream_count);
    stream[0] = package_modules[0];
    if (recursive) {
        stream[1] = 1;
        stream[2] = scc_ordinal;
    }
    if (components.count)
        memcpy(stream + prefix_stream_count, components.indexes,
               components.count * sizeof(*stream));
    if (layout_rows.stream_index_count)
        memcpy(stream + prefix_stream_count + components.count,
               layout_rows.stream_indexes,
               layout_rows.stream_index_count * sizeof(*stream));
    if (enum_rows.stream_index_count)
        memcpy(stream + prefix_stream_count + components.count
                   + layout_rows.stream_index_count,
               enum_rows.stream_indexes,
               enum_rows.stream_index_count * sizeof(*stream));
    if (signature_rows.parameter_index_count)
        memcpy(stream + prefix_stream_count + components.count
                   + layout_rows.stream_index_count
                   + enum_rows.stream_index_count,
               signature_rows.parameter_indexes,
               signature_rows.parameter_index_count * sizeof(*stream));
    if (declaration_rows.stream_index_count)
        memcpy(stream + prefix_stream_count + components.count
                   + layout_rows.stream_index_count
                   + enum_rows.stream_index_count
                   + signature_rows.parameter_index_count,
               declaration_rows.stream_indexes,
               declaration_rows.stream_index_count * sizeof(*stream));
    if (function_rows.stream_index_count)
        memcpy(stream + prefix_stream_count + components.count
                   + layout_rows.stream_index_count
                   + enum_rows.stream_index_count
                   + signature_rows.parameter_index_count
                   + declaration_rows.stream_index_count,
               function_rows.stream_indexes,
               function_rows.stream_index_count * sizeof(*stream));
    if (constraint_rows.stream_index_count)
        memcpy(stream + prefix_stream_count + components.count
                   + layout_rows.stream_index_count
                   + enum_rows.stream_index_count
                   + signature_rows.parameter_index_count
                   + declaration_rows.stream_index_count
                   + function_rows.stream_index_count,
               constraint_rows.stream_indexes,
               constraint_rows.stream_index_count * sizeof(*stream));
    if (storage_rows.stream_count)
        memcpy(stream + stream_count - tls_metadata.stream_count
                   - storage_rows.stream_count
                   - trait_method_rows.stream_index_count,
               storage_rows.stream_indexes,
               storage_rows.stream_count * sizeof(*stream));
    if (trait_method_rows.stream_index_count)
        memcpy(stream + stream_count - tls_metadata.stream_count
                   - trait_method_rows.stream_index_count,
               trait_method_rows.stream_indexes,
               trait_method_rows.stream_index_count * sizeof(*stream));
    if (tls_metadata.stream_count)
        memcpy(stream + stream_count - tls_metadata.stream_count,
               tls_metadata.stream_indexes,
               tls_metadata.stream_count * sizeof(*stream));
    core_list_t(core_object_emit_section_t) extra_sections = {0};
    uint32_t storage_input_indexes[4] = {0};
    uint32_t aggregate_input_index = 0;
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
                    ? ".rdata$A" : ".rodata.n00b_aggregate",
            .segment = target->object_format == CORE_TARGET_OBJECT_MACH_O
                ? "__DATA_CONST" : nullptr,
            .bytes = (const uint8_t *)storage_rows.aggregates.data,
            .size = storage_rows.aggregates.byte_len,
            .alignment = storage_rows.aggregate_alignment,
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
    for (size_t i = 1; valid && i < native_bindings.count; i++) {
        n00b_abi_storage_v1 *row = &storage_rows.rows[i];
        uint32_t input = row->segment_kind <= N00B_ABI_STORAGE_TLS
            ? storage_input_indexes[row->segment_kind] : 0;
        valid = input && core_object_emit_native_section_index(target,
            1 + extra_sections.len, input, &row->segment_index);
        if (valid) native_bindings.rows[i].section_index = row->segment_index;
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
    n00bb_abi_table_rows_t rows[] = {
        {N00B_ABI_TABLE_PACKAGE, packages, 2},
        {N00B_ABI_TABLE_MODULE, modules, 2},
        {N00B_ABI_TABLE_SCC, sccs, recursive ? 2 : 1},
        {N00B_ABI_TABLE_TYPE, type_rows.rows, type_rows.count},
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
        {N00B_ABI_TABLE_TRAIT_METHOD, trait_method_rows.rows,
         trait_method_rows.count},
        {N00B_ABI_TABLE_CONSTRAINT, constraint_rows.rows,
         constraint_rows.count},
        {N00B_ABI_TABLE_CALL_SIGNATURE, signature_rows.signatures,
         signature_rows.signature_count},
        {N00B_ABI_TABLE_PARAMETER, signature_rows.parameters,
         signature_rows.parameter_count},
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
        {N00B_ABI_TABLE_SHAPE, storage_rows.shapes,
         storage_rows.shape_count},
        {N00B_ABI_TABLE_U64_STREAM, storage_rows.extents,
         storage_rows.extent_count},
        {N00B_ABI_TABLE_NATIVE_BINDING, native_bindings.rows,
         native_bindings.count},
        {N00B_ABI_TABLE_U32_STREAM, stream, stream_count},
    };
    n00bb_abi_packed_tables_t packed = {0};
    valid = valid && n00bb_abi_pack_tables(target, artifact_capabilities,
        rows, sizeof(rows) / sizeof(*rows), &packed);
    core_free(stream);
    if (!valid) {
        core_list_free(extra_sections);
        core_free(requirements);
        native_bindings_release(&native_bindings);
        tls_metadata_release(&tls_metadata);
        n00bb_abi_storage_rows_release(&storage_rows);
        n00bb_abi_trait_method_rows_release(&trait_method_rows);
        n00bb_abi_constraint_rows_release(&constraint_rows);
        n00bb_abi_function_rows_release(&function_rows);
        n00bb_abi_declarations_release(&declaration_rows);
        n00bb_abi_type_signatures_release(&signature_rows);
        n00bb_abi_enum_rows_release(&enum_rows);
        n00bb_abi_type_layout_rows_release(&layout_rows);
        n00bb_abi_type_components_release(&components);
        n00bb_abi_type_identity_rows_release(&type_rows);
        n00bb_abi_name_pool_free(pool);
        n00bb_abi_decl_index_free(declarations);
        return abi_unit_error(unit, "could not pack ABI tables");
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
    core_object_emit_absolute64_t *relocations = nullptr;
    size_t relocation_count = 0;
    if (valid)
        valid = n00bb_abi_name_pool_relocations(pool, entries,
            sizeof(entries) / sizeof(*entries), &relocations,
            &relocation_count);
    if (valid && storage_rows.relocation_count) {
        if (storage_rows.relocation_count > SIZE_MAX - relocation_count)
            valid = false;
        else {
            size_t total = relocation_count + storage_rows.relocation_count;
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
                        : UINT32_MAX;
                size_t target_size = source->target_kind
                    == N00BB_ABI_RELOC_STRING_POOL
                    ? n00bb_abi_name_pool_size(pool)
                    : storage_rows.aggregates.byte_len;
                if (!section || target_symbol == UINT32_MAX
                    || source->target_offset >= target_size) {
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
            core_free(relocations);
            relocations = combined;
            relocation_count = total;
        }
    }
    uint8_t *object = nullptr;
    size_t object_bytes = 0;
    size_t symbol_count = (has_pool ? 1 : 0)
        + (has_aggregates ? 1 : 0)
        + native_bindings.count - 1;
    core_object_emit_symbol_t *object_symbols = symbol_count
        ? core_alloc_array(core_object_emit_symbol_t, symbol_count)
        : nullptr;
    if (has_pool) object_symbols[0] = string_symbol;
    if (has_aggregates) object_symbols[has_pool ? 1 : 0]
        = aggregate_symbol;
    for (size_t i = 1; i < native_bindings.count; i++) {
        const n00b_abi_storage_v1 *row = &storage_rows.rows[i];
        object_symbols[(has_pool ? 1 : 0) + (has_aggregates ? 1 : 0) + i - 1] =
            (core_object_emit_symbol_t){
                .name = native_bindings.names[i],
                .section_index = storage_input_indexes[row->segment_kind],
                .section_offset = row->byte_offset,
                .exported = true,
            };
    }
    if (valid)
        valid = n00bb_abi_write_object(target, &packed,
            extra_sections.data, extra_sections.len,
            object_symbols, symbol_count,
            relocations, relocation_count,
            &object, &object_bytes);
    core_list_free(extra_sections);
    core_free(object_symbols);
    core_free(relocations);
    core_free(requirements);
    native_bindings_release(&native_bindings);
    tls_metadata_release(&tls_metadata);
    n00bb_abi_storage_rows_release(&storage_rows);
    n00bb_abi_trait_method_rows_release(&trait_method_rows);
    n00bb_abi_constraint_rows_release(&constraint_rows);
    n00bb_abi_function_rows_release(&function_rows);
    n00bb_abi_declarations_release(&declaration_rows);
    n00bb_abi_type_signatures_release(&signature_rows);
    n00bb_abi_enum_rows_release(&enum_rows);
    n00bb_abi_type_layout_rows_release(&layout_rows);
    n00bb_abi_type_components_release(&components);
    n00bb_abi_type_identity_rows_release(&type_rows);
    n00bb_abi_name_pool_free(pool);
    n00bb_abi_decl_index_free(declarations);
    if (!valid) {
        n00bb_abi_packed_tables_release(&packed);
        return abi_unit_error(unit, "could not write ABI object");
    }
    *out_tables = packed;
    *out_object = object;
    *out_object_bytes = object_bytes;
    return true;
}
