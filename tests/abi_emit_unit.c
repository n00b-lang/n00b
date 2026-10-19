#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "abi_container.h"
#include "abi_emit.h"
#include "abi_layout.h"
#include "abi_link.h"
#include "abi_storage.h"
#include "abi_string.h"
#include "builtin_types.h"
#include "n00b-abi-v1.h"
#include "ir/ir.h"
#include "ir/typelang.h"
#include "lib/alloc.h"
#include "lib/byteorder.h"
#include "lib/hash.h"
#include "lib/link_unit.h"
#include "lib/module.h"
#include "lib/object_section.h"

static const n00bb_abi_directory_entry_t *
find_entry(const n00bb_abi_directory_entry_t *entries, size_t count,
           uint32_t kind)
{
    for (size_t i = 0; i < count; i++)
        if (entries[i].kind == kind) return &entries[i];
    return nullptr;
}

static bool
contains_bytes(const uint8_t *haystack, size_t haystack_size,
               const char *needle, size_t needle_size)
{
    if (needle_size > haystack_size) return false;
    for (size_t i = 0; i <= haystack_size - needle_size; i++)
        if (!memcmp(haystack + i, needle, needle_size)) return true;
    return false;
}

static core_link_declaration_t *
selected_type_declaration(void *context, parse_sym_entry_t *symbol)
{
    (void)symbol;
    return context;
}

typedef struct {
    const core_translation_unit_t *unit;
    parse_token_stream_t *tokens;
} descriptor_source_t;

static bool
descriptor_node_source(void *context, parse_tree_t *node,
                       const core_translation_unit_t **unit,
                       parse_token_stream_t **tokens)
{
    (void)node;
    descriptor_source_t *source = context;
    *unit = source->unit;
    *tokens = source->tokens;
    return true;
}

static void
check_type_descriptor_storage(void)
{
    core_module_spec_t spec = {0};
    core_source_file_t source = {0};
    core_translation_unit_t unit = {0};
    static const char source_text[] = "selected_type";
    assert(core_module_spec_init(&spec, CORE_STRING_STATIC("example"))
        && core_source_file_init(&source, CORE_STRING_STATIC("example.n00b"),
                                 source_text, sizeof(source_text) - 1)
        && core_translation_unit_init(&unit, &spec, &source));
    core_module_spec_release(&spec);
    assert(core_translation_unit_finalize_imports(&unit));
    core_link_unit_t *link = core_link_unit_new();
    assert(link && core_link_unit_add_root(link, &unit)
        && core_link_unit_collect(link));
    ir_ctx_t *context = ir_ctx_new();
    const ir_profile_t profile = {.name = "n00b"};
    ir_module_t *runtime = ir_module_new(context, "example", &profile,
                                        "x86_64-linux");
    assert(runtime);
    th_ctx *types = ir_ctx_types(context);
    th_type *typespec = th_name(types, "typespec");
    th_type *referred = th_bitint(types, 32, true);
    ir_global_t *global = ir_global_add(runtime, "selected_type", typespec);
    ir_global_init_value_t value = {
        .kind = IR_GLOBAL_INIT_TYPE, .descriptor_type = referred,
    };
    assert(global && ir_global_set_static_initializer(runtime, global,
                                                       &value)
        && ir_global_set_storage_duration(context, global,
                                         IR_STORAGE_DURATION_MODULE));
    core_link_declaration_t *declaration = nullptr;
    assert(core_link_unit_define(link, &unit,
        CORE_STRING_STATIC("selected_type"), global,
        CORE_LINK_PAYLOAD_BORROWED, nullptr, &declaration)
        && core_link_declaration_set_binding(link, declaration,
            CORE_STRING_STATIC("n00bb.ir.global"), 0, global));
    parse_symtab_t *symbols = parse_symtab_new();
    assert(symbols);
    parse_symtab_push_scope(symbols, CORE_STRING_STATIC("value"),
                            CORE_STRING_STATIC("module"));
    parse_scope_t *module_scope = parse_symtab_current_scope(symbols,
        CORE_STRING_STATIC("value"));
    assert(module_scope);
    module_scope->scope_tag = CORE_STRING_STATIC("module");
    parse_tree_t node = {0};
    core_tree_node_value(&node).end = 1;
    parse_token_info_t *token = parse_token_new();
    parse_token_set_origin(token, PARSE_TOKEN_SOURCE);
    parse_token_set_span(token, 0, sizeof(source_text) - 1);
    parse_token_set_position(token, 1, 1, 1, sizeof(source_text));
    parse_token_info_t *token_array[] = {token};
    parse_token_stream_t *tokens = scan_token_stream_from_array(token_array, 1);
    assert(tokens);
    parse_sym_entry_t *symbol = parse_symtab_add(symbols,
        CORE_STRING_STATIC("value"), CORE_STRING_STATIC("selected_type"),
        PARSE_SYM_VARIABLE, &node);
    assert(symbol);
    symbol->type = typespec;
    n00bb_abi_type_index_t *index = n00bb_abi_type_index_new(runtime);
    assert(index && n00bb_abi_type_index_collect_symbols(index, symbols,
                                                          nullptr, nullptr)
        && n00bb_abi_type_index_collect_module_types(index, runtime));
    uint32_t referred_index = 0;
    assert(n00bb_abi_type_index_find(index, referred, &referred_index)
        && referred_index != 0);
    core_target_desc_t target = {0};
    assert(core_target_profile("x86_64-linux", 0, &target));
    for (size_t i = 0; i < n00bb_abi_type_index_count(index); i++) {
        bool handled = false;
        assert(n00bb_abi_publish_direct_layout(runtime,
            n00bb_abi_type_index_at(index, (uint32_t)i), &target, &handled));
    }
    n00bb_abi_name_pool_t *names = n00bb_abi_name_pool_new();
    n00bb_abi_type_identity_rows_t types_rows = {0};
    assert(names && n00bb_abi_type_identity_rows_build(index,
        runtime, false, symbols, nullptr, nullptr, names, &types_rows));
    n00bb_abi_decl_index_t *declarations = n00bb_abi_decl_index_new(symbols);
    assert(declarations && n00bb_abi_decl_symbol_count(declarations) == 2);
    assert(n00bb_abi_decl_symbol_at(declarations, 1) == symbol);
    assert(core_link_declaration_binding(declaration,
        CORE_STRING_STATIC("n00bb.ir.global")) == global);
    assert(ir_global_storage_duration(global) == IR_STORAGE_DURATION_MODULE);
    assert(ir_global_can_read(global, IR_PHASE_RUNTIME));
    uint32_t type_index = 0;
    assert(n00bb_abi_type_index_find(index, typespec, &type_index)
        && type_index < types_rows.count
        && types_rows.rows[type_index].size_bits == 32
        && types_rows.rows[type_index].alignment_bytes == 4);
    n00bb_abi_storage_rows_t rows = {0};
    assert(n00bb_abi_storage_rows_build(index, declarations, &types_rows,
        nullptr, nullptr, runtime, names, 0, selected_type_declaration,
        declaration, nullptr, &rows));
    assert(rows.row_count == 2 && rows.index_patch_count == 1);
    const n00bb_abi_storage_index_patch_t *patch = &rows.index_patches[0];
    const n00b_abi_storage_v1 *storage = &rows.rows[1];
    assert(patch->local_index == referred_index
        && patch->table_kind == N00B_ABI_TABLE_TYPE
        && patch->patch_kind == N00B_ABI_PATCH_INDEX
        && patch->source_kind == storage->segment_kind
        && patch->byte_offset == storage->byte_offset
        && storage->byte_length == sizeof(uint32_t));
    const core_buffer_t *buffer = storage->segment_kind
        == N00B_ABI_STORAGE_READ_ONLY ? &rows.read_only : &rows.writable;
    assert(buffer->byte_len >= patch->byte_offset + sizeof(uint32_t)
        && core_le_load_u32((const uint8_t *)buffer->data
            + patch->byte_offset) == referred_index);
    descriptor_source_t source_owner = {.unit = &unit, .tokens = tokens};
    n00bb_abi_packed_tables_t packed = {0};
    uint8_t *object = nullptr;
    size_t object_bytes = 0;
    assert(n00bb_abi_emit_unit(link, &unit, CORE_STRING_STATIC("default"),
        symbols, tokens, runtime, &target, N00B_ABI_CAP_DEBUG,
        selected_type_declaration, declaration,
        descriptor_node_source, &source_owner, nullptr, nullptr, nullptr,
        &packed, nullptr, &object, &object_bytes));
    core_object_section_t metadata;
    assert(core_object_section_find(object, object_bytes,
        target.object_format, nullptr, ".data.rel.ro.n00b_meta", &metadata));
    n00bb_abi_image_key_t key = {
        .target_id = N00BB_ABI_TARGET_X86_64_LINUX,
        .abi_profile_id = N00BB_ABI_PROFILE_ID,
        .data_layout_profile = N00BB_ABI_DATA_LAYOUT_PROFILE,
    };
    size_t schema_count = 0;
    const n00bb_abi_table_schema_t *schemas =
        n00bb_abi_v1_table_schemas(&schema_count);
    n00bb_abi_directory_entry_t *entries = core_alloc_array(
        n00bb_abi_directory_entry_t, schema_count);
    n00bb_abi_image_header_t header;
    assert(n00bb_abi_container_read(metadata.bytes, metadata.size, &key,
        schemas, schema_count, false, &header, entries, schema_count));
    const n00bb_abi_directory_entry_t *patch_entry = find_entry(entries,
        header.directory_count, N00B_ABI_TABLE_INDEX_PATCH);
    assert(patch_entry && patch_entry->record_count >= 2);
    const n00b_abi_index_patch_v1 *emitted = nullptr;
    for (uint32_t i = 1; !emitted && i < patch_entry->record_count; i++) {
        const n00b_abi_index_patch_v1 *row = (const void *)(metadata.bytes
            + patch_entry->file_offset
            + (size_t)i * patch_entry->record_bytes);
        if (row->target_byte_offset == patch->byte_offset) emitted = row;
    }
    assert(emitted && emitted->referenced_table_kind == N00B_ABI_TABLE_TYPE
        && emitted->local_index == referred_index
        && emitted->target_byte_offset == patch->byte_offset
        && emitted->patch_kind == N00B_ABI_PATCH_INDEX
        && emitted->target_section_index != 0);

    core_module_spec_t prior_spec = {0};
    core_source_file_t prior_source = {0};
    core_translation_unit_t prior_unit = {0};
    assert(core_module_spec_init(&prior_spec, CORE_STRING_STATIC("prior"))
        && core_source_file_init(&prior_source,
            CORE_STRING_STATIC("prior.n00b"), "", 0)
        && core_translation_unit_init(&prior_unit, &prior_spec,
                                      &prior_source));
    core_module_spec_release(&prior_spec);
    assert(core_translation_unit_finalize_imports(&prior_unit));
    ir_module_t *prior_runtime = ir_module_new(context, "prior", &profile,
                                               target.triple);
    assert(prior_runtime
        && ir_type_intern(prior_runtime, th_bitint(types, 16, true)));
    parse_symtab_t *prior_symbols = parse_symtab_new();
    parse_token_stream_t *prior_tokens = scan_token_stream_from_array(
        nullptr, 0);
    n00bb_abi_packed_tables_t prior_packed = {0};
    uint8_t *prior_object = nullptr;
    size_t prior_object_bytes = 0;
    assert(prior_symbols && prior_tokens
        && n00bb_abi_emit_unit(nullptr, &prior_unit,
            CORE_STRING_STATIC("default"), prior_symbols, prior_tokens,
            prior_runtime, &target, N00B_ABI_CAP_DEBUG,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            &prior_packed, nullptr, &prior_object, &prior_object_bytes));
    core_link_unit_t *combined = core_link_unit_new();
    assert(combined && core_link_unit_add_root(combined, &prior_unit)
        && core_link_unit_add_root(combined, &unit)
        && core_link_unit_finalize_graph(combined));
    n00bb_abi_object_input_t inputs[] = {
        {CORE_STRING_STATIC("prior"), prior_object, prior_object_bytes},
        {CORE_STRING_STATIC("example"), object, object_bytes},
    };
    n00bb_abi_selected_objects_t selected = {0};
    assert(n00bb_abi_select_objects(combined, &target, inputs, 2,
                                    &selected)
        && selected.count == 2);
    size_t descriptor_input = selected.views[0].object == object ? 0 : 1;
    assert(selected.views[descriptor_input].object == object);
    n00bb_abi_index_plan_t *plan = n00bb_abi_index_plan_build(&selected);
    uint32_t final_type_index = 0;
    assert(plan && n00bb_abi_index_plan_map(plan, descriptor_input,
        N00B_ABI_TABLE_TYPE, referred_index, &final_type_index)
        && final_type_index != referred_index);
    uint8_t *linked_object = nullptr;
    size_t linked_bytes = 0;
    assert(n00bb_abi_link_objects(combined, &target, inputs, 2,
        &linked_object, &linked_bytes));
    core_object_section_t linked_storage;
    assert(core_object_section_find(linked_object, linked_bytes,
        target.object_format, nullptr, ".data.n00b_static",
        &linked_storage)
        && linked_storage.size >= patch->byte_offset + sizeof(uint32_t)
        && core_le_load_u32(linked_storage.bytes + patch->byte_offset)
            == final_type_index);
    n00bb_abi_object_view_t linked_view = {0};
    assert(n00bb_abi_object_open(&target, linked_object, linked_bytes,
        &linked_view)
        && !n00bb_abi_object_table(&linked_view,
            N00B_ABI_TABLE_INDEX_PATCH));
    n00bb_abi_object_close(&linked_view);
    core_free(linked_object);
    n00bb_abi_index_plan_free(plan);
    n00bb_abi_selected_objects_release(&selected);
    core_link_unit_free(combined);
    n00bb_abi_packed_tables_release(&prior_packed);
    core_free(prior_object);
    parse_symtab_free(prior_symbols);
    scan_token_stream_free(prior_tokens);
    core_translation_unit_release(&prior_unit);

    core_free(entries);
    n00bb_abi_packed_tables_release(&packed);
    core_free(object);
    n00bb_abi_storage_rows_release(&rows);
    n00bb_abi_decl_index_free(declarations);
    n00bb_abi_type_identity_rows_release(&types_rows);
    n00bb_abi_name_pool_free(names);
    n00bb_abi_type_index_free(index);
    parse_symtab_free(symbols);
    scan_token_stream_free(tokens);
    ir_ctx_free(context);
    core_link_unit_free(link);
    core_translation_unit_release(&unit);
}

static void
check_empty_unit(const char *triple, uint32_t target_id,
                 const char *metadata_segment, const char *metadata_name,
                 const char *pool_segment, const char *pool_name)
{
    static const char module_name[] = "default.long_identity_module_name";
    static const char source_path[] = "/tmp/project/long_identity_module_name.n00b";
    static const char source_text[] = "# empty\n";
    core_target_desc_t target;
    assert(core_target_profile(triple, 0, &target));
    core_module_spec_t spec = {0};
    assert(core_module_spec_init(&spec, CORE_STRING_STATIC(module_name)));
    core_source_file_t source = {0};
    assert(core_source_file_init(&source, CORE_STRING_STATIC(source_path),
                                 source_text, sizeof(source_text) - 1));
    core_translation_unit_t unit = {0};
    assert(core_translation_unit_init(&unit, &spec, &source));
    core_module_spec_release(&spec);
    assert(core_translation_unit_finalize_imports(&unit));

    ir_ctx_t *context = ir_ctx_new();
    const ir_profile_t profile = {.name = "n00b"};
    ir_module_t *runtime = ir_module_new(context, module_name,
                                        &profile, target.triple);
    assert(runtime);
    parse_symtab_t *symbols = parse_symtab_new();
    assert(symbols);
    parse_token_stream_t *tokens = scan_token_stream_from_array(nullptr, 0);
    assert(tokens);
    n00bb_abi_packed_tables_t packed = {0};
    uint8_t *object = nullptr;
    size_t object_bytes = 0;
    assert(n00bb_abi_emit_unit(nullptr, &unit,
        CORE_STRING_STATIC("default"),
        symbols, tokens, runtime,
        &target, N00B_ABI_CAP_DEBUG, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr,
        &packed, nullptr, &object, &object_bytes));
    assert(!core_translation_unit_has_fatal_diagnostics(&unit));

    core_object_section_t metadata;
    assert(core_object_section_find(object, object_bytes,
        target.object_format, metadata_segment, metadata_name, &metadata));
    n00bb_abi_image_key_t key = {
        .target_id = target_id,
        .abi_profile_id = N00BB_ABI_PROFILE_ID,
        .data_layout_profile = N00BB_ABI_DATA_LAYOUT_PROFILE,
    };
    size_t schema_count = 0;
    const n00bb_abi_table_schema_t *schemas =
        n00bb_abi_v1_table_schemas(&schema_count);
    n00bb_abi_directory_entry_t *entries = core_alloc_array(
        n00bb_abi_directory_entry_t, schema_count);
    n00bb_abi_image_header_t header;
    assert(n00bb_abi_container_read(metadata.bytes, metadata.size,
        &key, schemas, schema_count, false, &header, entries, schema_count));
    size_t entry_count = header.directory_count;
    const n00bb_abi_directory_entry_t *package_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_PACKAGE);
    const n00bb_abi_directory_entry_t *module_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_MODULE);
    const n00bb_abi_directory_entry_t *source_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_SOURCE_FILE);
    const n00bb_abi_directory_entry_t *type_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *layout_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_LAYOUT);
    const n00bb_abi_directory_entry_t *run_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_PHYSICAL_RUN);
    const n00bb_abi_directory_entry_t *packed_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_PACKED_RUN);
    const n00bb_abi_directory_entry_t *field_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_FIELD);
    const n00bb_abi_directory_entry_t *enum_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_ENUM_ITEM);
    const n00bb_abi_directory_entry_t *signature_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_CALL_SIGNATURE);
    const n00bb_abi_directory_entry_t *function_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_FUNCTION);
    const n00bb_abi_directory_entry_t *parameter_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_PARAMETER);
    const n00bb_abi_directory_entry_t *scope_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_SCOPE);
    const n00bb_abi_directory_entry_t *symbol_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_SYMBOL);
    const n00bb_abi_directory_entry_t *location_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_SOURCE_LOCATION);
    const n00bb_abi_directory_entry_t *config_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_RUNTIME_CONFIG);
    assert(package_entry && module_entry && source_entry && type_entry
        && layout_entry && run_entry && packed_entry && field_entry
        && enum_entry
        && signature_entry && function_entry
        && parameter_entry && scope_entry && symbol_entry
        && location_entry && config_entry);
    assert(layout_entry->record_count == 1 && run_entry->record_count == 1
        && packed_entry->record_count == 1 && field_entry->record_count == 1
        && enum_entry->record_count == 1
        && signature_entry->record_count == 1
        && function_entry->record_count == 1
        && parameter_entry->record_count == 1
        && scope_entry->record_count == 1
        && symbol_entry->record_count == 1
        && location_entry->record_count == 1);
    const n00b_abi_package_v1 *package = (const void *)(metadata.bytes
        + package_entry->file_offset + package_entry->record_bytes);
    const n00b_abi_module_v1 *module = (const void *)(metadata.bytes
        + module_entry->file_offset + module_entry->record_bytes);
    const n00b_abi_source_file_v1 *file = (const void *)(metadata.bytes
        + source_entry->file_offset + source_entry->record_bytes);
    const n00b_abi_type_v1 *nil = (const void *)(metadata.bytes
        + type_entry->file_offset);
    n00bb_abi_string_header_t decoded;
    assert(type_entry->record_count == 1);
    assert(nil->kind == N00B_ABI_TYPE_NIL && nil->size_bits == 0
        && nil->alignment_bytes == 1
        && nil->typehash == th_hash(ir_ctx_types(context),
                                   th_name(ir_ctx_types(context), "nil")));
    assert(n00bb_abi_string_decode((const uint8_t *)&nil->canonical_name,
        sizeof(nil->canonical_name), &decoded));
    assert(decoded.tag == N00BB_ABI_STRING_SHORT && decoded.length == 3);
    assert(!memcmp(nil->canonical_name.short_value.bytes, "nil", 3));
    assert(package->modules.first == 0 && package->modules.count == 1);
    assert(module->package_index == 1 && module->source_file_index == 1
        && module->runtime_config_index == 1 && module->scc_index == 0);
    assert(file->source_identity == core_xxh3_64bits_raw(
        source_text, sizeof(source_text) - 1));
    assert(n00bb_abi_string_decode((const uint8_t *)&package->name,
        sizeof(package->name), &decoded));
    assert(decoded.tag == N00BB_ABI_STRING_SHORT && decoded.length == 7);
    assert(n00bb_abi_string_decode((const uint8_t *)&module->full_path,
        sizeof(module->full_path), &decoded));
    assert(decoded.tag == N00BB_ABI_STRING_LONG
        && decoded.length == sizeof(module_name) - 1);
    assert(n00bb_abi_string_decode((const uint8_t *)&file->path,
        sizeof(file->path), &decoded));
    assert(decoded.tag == N00BB_ABI_STRING_LONG
        && decoded.length == sizeof(source_path) - 1);
    assert(metadata.relocation_count == 3);

    static const char table_name[] =
        "__n00b_index_table$default$long_identity_module_name";
    core_object_section_t pool;
    assert(core_object_section_find(object, object_bytes,
        target.object_format, pool_segment, pool_name, &pool));
    assert(pool.size == sizeof(module_name) - 1 + sizeof(source_path) - 1
        + sizeof(table_name) - 1
        + (target.object_format == CORE_TARGET_OBJECT_MACH_O));
    assert(contains_bytes(pool.bytes, pool.size,
        table_name, sizeof(table_name) - 1));
    assert(contains_bytes(pool.bytes, pool.size,
        module_name, sizeof(module_name) - 1));
    assert(contains_bytes(pool.bytes, pool.size,
        source_path, sizeof(source_path) - 1));
    uint64_t module_pointer = module_entry->file_offset
        + module_entry->record_bytes + offsetof(n00b_abi_module_v1, full_path);
    uint64_t source_pointer = source_entry->file_offset
        + source_entry->record_bytes + offsetof(n00b_abi_source_file_v1, path);
    bool found_module = false, found_source = false;
    for (uint32_t i = 0; i < metadata.relocation_count; i++) {
        core_object_absolute64_relocation_t relocation;
        assert(core_object_absolute64_relocation_at(&metadata,
            target.object_format, i, &relocation));
        found_module |= relocation.section_offset == module_pointer;
        found_source |= relocation.section_offset == source_pointer;
    }
    assert(found_module && found_source);

    parse_tree_t declaration_node = {0};
    assert(parse_symtab_add(symbols, CORE_STRING_STATIC("value"),
        CORE_STRING_STATIC("declared"), PARSE_SYM_VARIABLE,
        &declaration_node));
    n00bb_abi_packed_tables_t rejected = {0};
    uint8_t *rejected_object = nullptr;
    size_t rejected_bytes = 0;
    assert(!n00bb_abi_emit_unit(nullptr, &unit,
        CORE_STRING_STATIC("default"),
        symbols, tokens, runtime, &target, N00B_ABI_CAP_DEBUG,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        &rejected, nullptr, &rejected_object, &rejected_bytes));
    assert(!rejected.bytes && !rejected_object && !rejected_bytes);
    assert(core_translation_unit_has_fatal_diagnostics(&unit));

    core_free(entries);
    n00bb_abi_packed_tables_release(&packed);
    core_free(object);
    parse_symtab_free(symbols);
    scan_token_stream_free(tokens);
    ir_ctx_free(context);
    core_translation_unit_release(&unit);
}

int
main(void)
{
    n00bb_register_builtin_types();
    check_type_descriptor_storage();
    check_empty_unit("x86_64-linux", N00BB_ABI_TARGET_X86_64_LINUX,
        nullptr, ".data.rel.ro.n00b_meta", nullptr,
        ".rodata.n00b_strings");
    check_empty_unit("arm64-macos", N00BB_ABI_TARGET_ARM64_MACOS,
        "__DATA_CONST", "__n00b_meta", "__TEXT", "__n00b_strings");
    check_empty_unit("x86_64-windows", N00BB_ABI_TARGET_X86_64_WINDOWS,
        nullptr, ".n00b$M", nullptr, ".rdata$S");
    return 0;
}
