#include "abi_link.h"

#include <string.h>

#include "abi_emit.h"
#include "abi_index_patch.h"
#include "lib/alloc.h"

static bool
same_key(const n00bb_abi_image_key_t *left,
         const n00bb_abi_image_key_t *right)
{
    return left->target_id == right->target_id
        && left->abi_profile_id == right->abi_profile_id
        && left->feature_bits == right->feature_bits
        && left->cpu_profile == right->cpu_profile
        && left->data_layout_profile == right->data_layout_profile;
}

static bool
link_only_inputs_resolved(const n00bb_abi_selected_objects_t *selected,
                          const n00bb_abi_image_key_t *key,
                          uint64_t *capabilities)
{
    for (size_t input = 0; input < selected->count; input++) {
        const n00bb_abi_object_view_t *view = &selected->views[input];
        if (!view->directory || !same_key(&view->header.key, key))
            return false;
        *capabilities |= view->header.artifact_capabilities;
        const n00bb_abi_directory_entry_t *requirements =
            n00bb_abi_object_table(view, N00B_ABI_TABLE_REQUIREMENT);
        if (requirements) {
            for (uint32_t i = 1; i < requirements->record_count; i++) {
                const n00b_abi_requirement_v1 *row =
                    n00bb_abi_object_row(view,
                        N00B_ABI_TABLE_REQUIREMENT, i);
                if (!row || row->symbol_kind != N00B_ABI_SYMBOL_MODULE)
                    return false;
            }
        }
        const uint32_t pending[] = {
            N00B_ABI_TABLE_CONSTRAINT,
            N00B_ABI_TABLE_SPECIALIZATION_LINK,
            N00B_ABI_TABLE_TRAIT_METHOD,
            N00B_ABI_TABLE_DISPATCH,
            N00B_ABI_TABLE_DISPATCH_ENTRY,
            N00B_ABI_TABLE_NAME_SLOT,
        };
        for (size_t i = 0; i < sizeof(pending) / sizeof(*pending); i++) {
            const n00bb_abi_directory_entry_t *table =
                n00bb_abi_object_table(view, pending[i]);
            if (table && table->record_count > 1) return false;
        }
    }
    return true;
}

bool
n00bb_abi_apply_link_patches(
    const n00bb_abi_selected_objects_t *selected,
    const n00bb_abi_index_plan_t *plan,
    const n00bb_abi_merged_rows_t *merged,
    n00bb_abi_native_sections_t *sections)
{
    if (!selected || !selected->views || !plan || !merged
        || !merged->tables || !sections
        || (sections->count && (!sections->sections || !sections->maps)))
        return false;
    for (size_t input = 0; input < selected->count; input++) {
        const n00bb_abi_object_view_t *view = &selected->views[input];
        const n00bb_abi_directory_entry_t *patch_table =
            n00bb_abi_object_table(view, N00B_ABI_TABLE_INDEX_PATCH);
        if (!patch_table || patch_table->record_count <= 1) continue;
        if (patch_table->record_bytes != N00BB_ABI_INDEX_PATCH_BYTES
            || patch_table->byte_length !=
                (uint64_t)patch_table->record_count
                    * N00BB_ABI_INDEX_PATCH_BYTES
            || patch_table->file_offset > view->metadata.size
            || patch_table->byte_length
                > view->metadata.size - patch_table->file_offset
            || !view->metadata.bytes) return false;
        n00bb_abi_patch_section_t *patch_sections = core_alloc_array(
            n00bb_abi_patch_section_t, sections->count);
        n00bb_abi_patch_table_t *patch_tables = core_alloc_array(
            n00bb_abi_patch_table_t, merged->table_count);
        size_t patch_section_count = 0, patch_table_count = 0;
        for (size_t i = 0; i < sections->count; i++) {
            if (sections->maps[i].input != input) continue;
            patch_sections[patch_section_count++] =
                (n00bb_abi_patch_section_t){
                    .native_index = sections->maps[i].local_section_index,
                    .bytes = (uint8_t *)sections->sections[i].bytes,
                    .byte_length = sections->sections[i].size,
                };
        }
        for (size_t i = 0; i < merged->table_count; i++) {
            const n00bb_abi_merged_table_t *table = &merged->tables[i];
            if (table->kind == N00B_ABI_TABLE_U32_STREAM
                || table->kind == N00B_ABI_TABLE_U64_STREAM
                || table->kind == N00B_ABI_TABLE_BYTE_STREAM
                || table->kind == N00B_ABI_TABLE_REQUIREMENT
                || table->kind == N00B_ABI_TABLE_INDEX_PATCH
                || table->kind == N00B_ABI_TABLE_CONSTRAINT) continue;
            const core_index_remap_t *remap =
                n00bb_abi_index_plan_remap(plan, input, table->kind);
            if (!remap) continue;
            patch_tables[patch_table_count++] =
                (n00bb_abi_patch_table_t){
                    .kind = table->kind,
                    .remap = remap,
                    .final_row_count = table->record_count,
                };
        }
        const uint8_t *patch_bytes = view->metadata.bytes
            + patch_table->file_offset + patch_table->record_bytes;
        size_t patch_byte_count = (size_t)(patch_table->record_count - 1)
            * patch_table->record_bytes;
        bool valid = n00bb_abi_index_patches_apply(patch_bytes,
            patch_byte_count, patch_sections, patch_section_count,
            patch_tables, patch_table_count, nullptr) ==
                N00BB_ABI_PATCH_OK;
        core_free(patch_sections);
        core_free(patch_tables);
        if (!valid) return false;
    }
    return true;
}

bool
n00bb_abi_link_selected_objects(
    const n00bb_abi_selected_objects_t *selected,
    const core_target_desc_t *target,
    uint8_t **out_object, size_t *out_object_bytes)
{
    n00bb_abi_image_key_t key;
    uint64_t capabilities = 0;
    if (!selected || !selected->views || !selected->count || !target
        || !out_object || !out_object_bytes
        || !n00bb_abi_image_key_for_target(target, &key)
        || !link_only_inputs_resolved(selected, &key, &capabilities))
        return false;

    n00bb_abi_index_plan_t *plan =
        n00bb_abi_index_plan_build(selected);
    n00bb_abi_merged_rows_t merged = {0};
    n00bb_abi_native_sections_t sections = {0};
    n00bb_abi_native_relocations_t native = {0};
    n00bb_abi_packed_tables_t packed = {0};
    n00bb_abi_table_rows_t *rows = nullptr;
    n00bb_abi_directory_entry_t *entries = nullptr;
    core_object_emit_absolute64_t *pool_relocations = nullptr;
    core_object_emit_absolute64_t *relocations = nullptr;
    core_object_emit_section_t *extra_sections = nullptr;
    core_object_emit_symbol_t *symbols = nullptr;
    uint8_t *object = nullptr;
    size_t object_bytes = 0;
    bool valid = plan
        && n00bb_abi_merge_rows(selected, plan, &merged)
        && n00bb_abi_collect_native_sections(selected, target, &sections)
        && n00bb_abi_apply_link_patches(selected, plan, &merged, &sections)
        && n00bb_abi_collect_native_relocations(selected, &sections,
                                                &native)
        && n00bb_abi_rebase_native_references(&merged, sections.maps,
                                              sections.count)
        && n00bb_abi_add_native_bindings(selected, &merged, &sections,
                                         &native);

    size_t row_count = 0;
    if (valid) {
        rows = core_alloc_array(n00bb_abi_table_rows_t,
                                merged.table_count);
        for (size_t i = 0; i < merged.table_count; i++) {
            const n00bb_abi_merged_table_t *table = &merged.tables[i];
            if (table->kind == N00B_ABI_TABLE_REQUIREMENT
                || table->kind == N00B_ABI_TABLE_INDEX_PATCH
                || table->kind == N00B_ABI_TABLE_CONSTRAINT) continue;
            rows[row_count++] = (n00bb_abi_table_rows_t){
                .kind = table->kind,
                .records = table->records,
                .record_count = table->record_count,
            };
        }
        valid = n00bb_abi_pack_tables(target, capabilities, rows,
                                      row_count, &packed);
    }

    size_t schema_count = 0;
    const n00bb_abi_table_schema_t *schemas =
        n00bb_abi_v1_table_schemas(&schema_count);
    n00bb_abi_image_header_t header;
    if (valid) {
        entries = core_alloc_array(n00bb_abi_directory_entry_t,
                                   row_count);
        valid = n00bb_abi_container_read(packed.bytes,
            packed.byte_count, &key, schemas, schema_count, false,
            &header, entries, row_count);
    }
    size_t pool_relocation_count = 0;
    if (valid)
        valid = n00bb_abi_name_pool_relocations(merged.strings, entries,
            row_count, &pool_relocations, &pool_relocation_count);

    size_t pool_size = n00bb_abi_name_pool_size(merged.strings);
    bool has_pool = pool_size != 0;
    if (valid) {
        valid = (has_pool || !pool_relocation_count)
            && sections.count < UINT32_MAX - 2
            && native.symbol_count < UINT32_MAX - (size_t)has_pool
            && native.relocation_count <= SIZE_MAX - pool_relocation_count;
    }
    if (valid) {
        size_t extra_count = sections.count + (size_t)has_pool;
        size_t symbol_count = native.symbol_count + (size_t)has_pool;
        size_t relocation_count = native.relocation_count
            + pool_relocation_count;
        extra_sections = core_alloc_array(core_object_emit_section_t,
                                          extra_count);
        symbols = core_alloc_array(core_object_emit_symbol_t,
                                   symbol_count);
        relocations = core_alloc_array(core_object_emit_absolute64_t,
                                       relocation_count);
        if (sections.count)
            memcpy(extra_sections, sections.sections,
                   sections.count * sizeof(*extra_sections));
        if (native.symbol_count)
            memcpy(symbols, native.symbols,
                   native.symbol_count * sizeof(*symbols));
        if (native.relocation_count)
            memcpy(relocations, native.relocations,
                   native.relocation_count * sizeof(*relocations));
        if (has_pool) {
            extra_sections[sections.count] =
                (core_object_emit_section_t){
                    .name = target->object_format == CORE_TARGET_OBJECT_MACH_O
                        ? "__n00b_strings"
                        : target->object_format == CORE_TARGET_OBJECT_COFF
                            ? ".rdata$S" : ".rodata.n00b_strings",
                    .segment = target->object_format
                        == CORE_TARGET_OBJECT_MACH_O ? "__TEXT" : nullptr,
                    .bytes = n00bb_abi_name_pool_bytes(merged.strings),
                    .size = pool_size,
                    .alignment = 1,
                };
            symbols[native.symbol_count] = (core_object_emit_symbol_t){
                .name = "n00b_string_pool",
                .section_index = (uint32_t)(2 + sections.count),
            };
        }
        for (size_t i = 0; i < pool_relocation_count; i++) {
            relocations[native.relocation_count + i] = pool_relocations[i];
            relocations[native.relocation_count + i].symbol_index =
                (uint32_t)native.symbol_count;
        }
        valid = n00bb_abi_write_object(target, &packed,
            extra_sections, extra_count, symbols, symbol_count,
            relocations, relocation_count, &object, &object_bytes);
    }

    core_free(relocations);
    core_free(symbols);
    core_free(extra_sections);
    core_free(pool_relocations);
    core_free(entries);
    core_free(rows);
    n00bb_abi_packed_tables_release(&packed);
    n00bb_abi_native_relocations_release(&native);
    n00bb_abi_native_sections_release(&sections);
    n00bb_abi_merged_rows_release(&merged);
    n00bb_abi_index_plan_free(plan);
    if (!valid) return false;
    *out_object = object;
    *out_object_bytes = object_bytes;
    return true;
}
