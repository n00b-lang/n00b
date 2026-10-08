#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi_link.h"
#include "abi_index_patch.h"
#include "abi_string.h"
#include "n00b-abi-v1.h"
#include "lib/alloc.h"
#include "lib/byteorder.h"

static const char *
host_triple(void)
{
#if defined(__APPLE__) && defined(__aarch64__)
    return "arm64-macos";
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return "x86_64-windows";
#else
    return "x86_64-linux";
#endif
}

static size_t
count_string_pool_sections(const uint8_t *object, size_t object_bytes,
                           const core_target_desc_t *target)
{
    const char *name = target->object_format == CORE_TARGET_OBJECT_MACH_O
        ? "__n00b_strings"
        : target->object_format == CORE_TARGET_OBJECT_COFF
            ? ".rdata$S" : ".rodata.n00b_strings";
    uint32_t section_count = 0;
    assert(core_object_section_count(object, object_bytes,
        target->object_format, &section_count));
    size_t matches = 0;
    for (uint32_t i = 1; i <= section_count; i++) {
        core_object_section_t section;
        if (!core_object_section_at(object, object_bytes,
            target->object_format, i, &section)) continue;
        matches += section.name_length == strlen(name)
            && !memcmp(section.name, name, section.name_length)
            && (target->object_format != CORE_TARGET_OBJECT_MACH_O
                || (section.segment_length == strlen("__TEXT")
                    && !memcmp(section.segment, "__TEXT",
                        section.segment_length)));
    }
    return matches;
}

static uint8_t *
read_object(const char *path, size_t *object_bytes)
{
    FILE *file = fopen(path, "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    long length = ftell(file);
    assert(length > 0 && !fseek(file, 0, SEEK_SET));
    uint8_t *object = malloc((size_t)length);
    assert(object && fread(object, 1, (size_t)length, file)
        == (size_t)length && !fclose(file));
    *object_bytes = (size_t)length;
    return object;
}

static void
test_static_string_identity(void)
{
    n00b_abi_static_string_v1 rows[2][2] = {0};
    n00bb_abi_name_pool_t *pool = n00bb_abi_name_pool_new();
    assert(pool);
    assert(n00bb_abi_name_pool_encode_with_encoding(pool,
        CORE_STRING_STATIC("same"), N00B_ABI_STRING_BINARY,
        N00B_ABI_TABLE_STATIC_STRING, 1, 0, &rows[0][1].value)
        && n00bb_abi_name_pool_encode_with_encoding(pool,
            CORE_STRING_STATIC("same"), N00B_ABI_STRING_ASCII,
            N00B_ABI_TABLE_STATIC_STRING, 1, 0, &rows[1][1].value));
    n00bb_abi_directory_entry_t directories[2] = {0};
    n00bb_abi_object_view_t views[2] = {0};
    for (size_t i = 0; i < 2; i++) {
        directories[i] = (n00bb_abi_directory_entry_t){
            .kind = N00B_ABI_TABLE_STATIC_STRING,
            .record_count = 2,
            .record_bytes = sizeof(n00b_abi_static_string_v1),
        };
        views[i].metadata.bytes = (const uint8_t *)rows[i];
        views[i].metadata.size = sizeof(rows[i]);
        views[i].directory = &directories[i];
        views[i].directory_count = 1;
    }
    n00bb_abi_selected_objects_t selected = {.views = views, .count = 2};
    n00bb_abi_index_plan_t *plan = n00bb_abi_index_plan_build(&selected);
    uint32_t count, left, right;
    assert(plan && n00bb_abi_index_plan_count(plan,
        N00B_ABI_TABLE_STATIC_STRING, &count) && count == 3
        && n00bb_abi_index_plan_map(plan, 0,
            N00B_ABI_TABLE_STATIC_STRING, 1, &left)
        && n00bb_abi_index_plan_map(plan, 1,
            N00B_ABI_TABLE_STATIC_STRING, 1, &right)
        && left != right);
    n00bb_abi_index_plan_free(plan);
    assert(n00bb_abi_name_pool_encode_with_encoding(pool,
        CORE_STRING_STATIC("same"), N00B_ABI_STRING_BINARY,
        N00B_ABI_TABLE_STATIC_STRING, 1, 0, &rows[1][1].value));
    plan = n00bb_abi_index_plan_build(&selected);
    assert(plan && n00bb_abi_index_plan_count(plan,
        N00B_ABI_TABLE_STATIC_STRING, &count) && count == 2);
    n00bb_abi_index_plan_free(plan);
    n00bb_abi_name_pool_free(pool);
}

static bool
init_unit(core_translation_unit_t *unit, const char *name)
{
    core_module_spec_t spec = {0};
    core_source_file_t source = {0};
    bool valid = core_module_spec_init(&spec, core_string_from_cstr(name))
        && core_source_file_init(&source, core_string_from_cstr(name), "", 0)
        && core_translation_unit_init(unit, &spec, &source);
    core_module_spec_release(&spec);
    core_source_file_release(&source);
    return valid;
}

static void
check_remapped_rows(const n00bb_abi_selected_objects_t *selected,
                    const n00bb_abi_index_plan_t *plan)
{
    uint32_t stream_count;
    assert(n00bb_abi_index_plan_count(plan, N00B_ABI_TABLE_U32_STREAM,
        &stream_count));
    uint32_t *values = calloc(stream_count ? stream_count : 1,
                              sizeof(*values));
    uint32_t *kinds = calloc(stream_count ? stream_count : 1,
                             sizeof(*kinds));
    assert(values && kinds);
    n00bb_abi_u32_stream_build_t stream = {
        .values = values, .kinds = kinds, .count = stream_count,
    };
    n00bb_abi_name_pool_t *pool = n00bb_abi_name_pool_new();
    assert(pool);
    for (size_t input = 0; input < selected->count; input++) {
        const n00bb_abi_object_view_t *view = &selected->views[input];
        for (size_t table = 0; table < view->directory_count; table++) {
            const n00bb_abi_directory_entry_t *entry = &view->directory[table];
            if (entry->kind == N00B_ABI_TABLE_U32_STREAM
                || entry->kind == N00B_ABI_TABLE_U64_STREAM
                || entry->kind == N00B_ABI_TABLE_BYTE_STREAM) continue;
            void *row = malloc(entry->record_bytes);
            assert(row);
            for (uint32_t local = 0; local < entry->record_count; local++) {
                uint32_t final_index;
                assert(n00bb_abi_remap_row(selected, plan, input,
                    entry->kind, local, row, entry->record_bytes,
                    &stream)
                    && n00bb_abi_index_plan_map(plan, input,
                        entry->kind, local, &final_index)
                    && n00bb_abi_repool_row_strings(view, entry->kind,
                        local, final_index, row, entry->record_bytes,
                        pool));
            }
            free(row);
        }
        n00b_abi_package_v1 package;
        uint32_t module_index;
        assert(n00bb_abi_remap_row(selected, plan, input,
            N00B_ABI_TABLE_PACKAGE, 1, &package, sizeof(package), &stream)
            && n00bb_abi_index_plan_map(plan, input,
                N00B_ABI_TABLE_MODULE, 1, &module_index)
            && package.modules.count == 1
            && package.modules.first < stream.count
            && stream.values[package.modules.first] == module_index);
        const n00bb_abi_directory_entry_t *scc = n00bb_abi_object_table(
            view, N00B_ABI_TABLE_SCC);
        if (scc && scc->record_count > 1) {
            n00b_abi_scc_v1 group;
            assert(n00bb_abi_remap_row(selected, plan, input,
                N00B_ABI_TABLE_SCC, 1, &group, sizeof(group), &stream)
                && group.members.count == 1
                && group.members.first < stream.count
                && stream.values[group.members.first] == module_index);
        }
    }
    n00bb_abi_name_pool_free(pool);
    free(kinds);
    free(values);
}

static void
test_cycle_selection(char **paths)
{
    const char *names[] = {"cycle_long_a", "cycle_long_b", "cycle_long_c"};
    core_translation_unit_t units[3] = {0};
    n00bb_abi_object_input_t inputs[4] = {0};
    uint8_t *objects[3];
    for (size_t i = 0; i < 3; i++) {
        assert(init_unit(&units[i], names[i]));
        size_t bytes;
        objects[i] = read_object(paths[i], &bytes);
        inputs[2 - i] = (n00bb_abi_object_input_t){
            .module = core_string_from_cstr(names[i]),
            .object = objects[i], .object_bytes = bytes,
        };
    }
    inputs[3].module = CORE_STRING_STATIC("unrelated");
    assert(core_translation_unit_add_import(&units[0], &units[1])
        && core_translation_unit_add_import(&units[1], &units[2])
        && core_translation_unit_add_import(&units[2], &units[0]));
    core_link_unit_t *link = core_link_unit_new();
    assert(link && core_link_unit_add_root(link, &units[0])
        && core_link_unit_finalize_graph(link));
    core_target_desc_t target;
    assert(core_target_profile(host_triple(), 0, &target));
    n00bb_abi_selected_objects_t selected = {0};
    assert(n00bb_abi_select_objects(link, &target, inputs, 4, &selected)
        && selected.count == 3);
    for (size_t i = 0; i < selected.count; i++)
        assert(selected.views[i].object);
    n00bb_abi_index_plan_t *plan = n00bb_abi_index_plan_build(&selected);
    assert(plan);
    uint32_t packages, modules, sccs, types;
    assert(n00bb_abi_index_plan_count(plan, N00B_ABI_TABLE_PACKAGE,
        &packages) && packages == 2);
    assert(n00bb_abi_index_plan_count(plan, N00B_ABI_TABLE_MODULE,
        &modules) && modules == 4);
    assert(n00bb_abi_index_plan_count(plan, N00B_ABI_TABLE_SCC,
        &sccs) && sccs == 2);
    assert(n00bb_abi_index_plan_count(plan, N00B_ABI_TABLE_TYPE,
        &types) && types > 1);
    uint32_t stream_base = 0;
    for (size_t i = 0; i < selected.count; i++) {
        uint32_t mapped = UINT32_MAX;
        assert(n00bb_abi_index_plan_map(plan, i, N00B_ABI_TABLE_TYPE,
            0, &mapped) && mapped == 0);
        assert(n00bb_abi_index_plan_map(plan, i, N00B_ABI_TABLE_PACKAGE,
            1, &mapped) && mapped == 1);
        assert(n00bb_abi_index_plan_map(plan, i, N00B_ABI_TABLE_MODULE,
            1, &mapped) && mapped == i + 1);
        assert(n00bb_abi_index_plan_map(plan, i, N00B_ABI_TABLE_SCC,
            1, &mapped) && mapped == 1);
        const n00bb_abi_directory_entry_t *stream =
            n00bb_abi_object_table(&selected.views[i],
                N00B_ABI_TABLE_U32_STREAM);
        assert(stream && n00bb_abi_index_plan_map(plan, i,
            N00B_ABI_TABLE_U32_STREAM, 0, &mapped)
            && mapped == stream_base);
        stream_base += stream->record_count;
    }
    bool shared_primitive = false;
    const n00bb_abi_object_view_t *first = &selected.views[0];
    const n00bb_abi_object_view_t *second = &selected.views[1];
    const n00bb_abi_directory_entry_t *first_types =
        n00bb_abi_object_table(first, N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *second_types =
        n00bb_abi_object_table(second, N00B_ABI_TABLE_TYPE);
    assert(first_types && second_types);
    n00b_abi_physical_run_v1 *layout_probe = nullptr;
    for (uint32_t i = 1; i < first_types->record_count; i++) {
        const n00b_abi_type_v1 *a = n00bb_abi_object_row(first,
            N00B_ABI_TABLE_TYPE, i);
        if (a->kind != N00B_ABI_TYPE_PRIMITIVE) continue;
        for (uint32_t j = 1; j < second_types->record_count; j++) {
            const n00b_abi_type_v1 *b = n00bb_abi_object_row(second,
                N00B_ABI_TABLE_TYPE, j);
            if (b->kind != N00B_ABI_TYPE_PRIMITIVE
                || b->typehash != a->typehash) continue;
            uint32_t left, right;
            assert(n00bb_abi_index_plan_map(plan, 0, N00B_ABI_TABLE_TYPE,
                i, &left) && n00bb_abi_index_plan_map(plan, 1,
                N00B_ABI_TABLE_TYPE, j, &right) && left == right);
            shared_primitive = true;
            const n00b_abi_layout_v1 *layout = n00bb_abi_object_row(second,
                N00B_ABI_TABLE_LAYOUT, b->layout_index);
            if (!layout_probe && layout && layout->occupied_runs.count) {
                const uint8_t *slot = n00bb_abi_object_row(second,
                    N00B_ABI_TABLE_U32_STREAM,
                    layout->occupied_runs.first);
                if (slot) layout_probe = (void *)n00bb_abi_object_row(second,
                    N00B_ABI_TABLE_PHYSICAL_RUN, core_le_load_u32(slot));
            }
        }
    }
    assert(shared_primitive && layout_probe);
    layout_probe->width_bits ^= 1;
    assert(!n00bb_abi_index_plan_build(&selected));
    layout_probe->width_bits ^= 1;
    check_remapped_rows(&selected, plan);
    n00b_abi_module_v1 *module = (void *)n00bb_abi_object_row(first,
        N00B_ABI_TABLE_MODULE, 1);
    n00b_abi_module_v1 remapped_module;
    assert(module && module->package_index && module->runtime_config_index
        && module->source_file_index && module->scc_index);
    uint32_t *required[] = {&module->package_index,
        &module->runtime_config_index, &module->source_file_index};
    for (size_t i = 0; i < sizeof(required) / sizeof(*required); i++) {
        uint32_t saved = *required[i];
        *required[i] = 0;
        assert(!n00bb_abi_remap_row(&selected, plan, 0,
            N00B_ABI_TABLE_MODULE, 1, &remapped_module,
            sizeof(remapped_module), nullptr));
        *required[i] = saved;
    }
    uint32_t saved_scc = module->scc_index;
    module->scc_index = 0;
    assert(n00bb_abi_remap_row(&selected, plan, 0,
        N00B_ABI_TABLE_MODULE, 1, &remapped_module,
        sizeof(remapped_module), nullptr));
    module->scc_index = saved_scc;
    n00bb_abi_manifest_rows_t manifests = {0};
    assert(n00bb_abi_rebuild_manifests(&selected, plan, &manifests)
        && manifests.package_count == 2 && manifests.scc_count == 2
        && manifests.packages[1].modules.count == 3
        && manifests.sccs[1].members.count == 3
        && manifests.sccs[1].lexical_order.count == 3);
    for (size_t i = 0; i < selected.count; i++) {
        uint32_t module_index;
        assert(n00bb_abi_index_plan_map(plan, i, N00B_ABI_TABLE_MODULE,
            1, &module_index));
        assert(manifests.u32_stream[manifests.packages[1].modules.first + i]
            == module_index);
        assert(manifests.u32_stream[manifests.sccs[1].members.first + i]
            == module_index);
        const n00b_abi_scc_v1 *fragment = n00bb_abi_object_row(
            &selected.views[i], N00B_ABI_TABLE_SCC, 1);
        const uint8_t *ordinal_slot = n00bb_abi_object_row(
            &selected.views[i], N00B_ABI_TABLE_U32_STREAM,
            fragment->lexical_order.first);
        uint32_t ordinal = core_le_load_u32(ordinal_slot);
        assert(ordinal < 3
            && manifests.u32_stream[
                manifests.sccs[1].lexical_order.first + ordinal]
                == module_index);
    }
    n00bb_abi_manifest_rows_release(&manifests);
    n00bb_abi_merged_rows_t merged = {0};
    assert(n00bb_abi_merge_rows(&selected, plan, &merged));
    assert(n00bb_abi_rebase_native_references(&merged, nullptr, 0));
    n00bb_abi_native_sections_t native_sections = {0};
    assert(n00bb_abi_collect_native_sections(&selected, &target,
        &native_sections));
    for (size_t i = 0; i < native_sections.count; i++) {
        const n00bb_abi_native_section_map_t *map =
            &native_sections.maps[i];
        core_object_section_t source;
        assert(map->input < selected.count
            && map->local_section_index !=
                selected.views[map->input].metadata.native_index
            && core_object_section_at(selected.views[map->input].object,
                selected.views[map->input].object_bytes, target.object_format,
                map->local_section_index, &source)
            && source.name_length ==
                strlen(native_sections.sections[i].name)
            && !memcmp(source.name, native_sections.sections[i].name,
                source.name_length));
    }
    n00bb_abi_native_sections_release(&native_sections);
    const n00bb_abi_merged_table_t *merged_modules =
        n00bb_abi_merged_table(&merged, N00B_ABI_TABLE_MODULE);
    const n00bb_abi_merged_table_t *merged_u32 =
        n00bb_abi_merged_table(&merged, N00B_ABI_TABLE_U32_STREAM);
    const n00b_abi_package_v1 *merged_package =
        n00bb_abi_merged_row(&merged, N00B_ABI_TABLE_PACKAGE, 1);
    const n00b_abi_scc_v1 *merged_scc =
        n00bb_abi_merged_row(&merged, N00B_ABI_TABLE_SCC, 1);
    const n00bb_abi_merged_table_t *merged_requirements =
        n00bb_abi_merged_table(&merged, N00B_ABI_TABLE_REQUIREMENT);
    assert(merged_modules && merged_modules->record_count == 4
        && merged_u32 && merged_u32->record_count > stream_base
        && merged_package && merged_package->modules.count == 3
        && merged_scc && merged_scc->members.count == 3
        && merged_scc->lexical_order.count == 3
        && merged_requirements && merged_requirements->record_count == 4
        && n00bb_abi_merged_row(&merged, N00B_ABI_TABLE_TYPE, 0));
    for (size_t i = 0; i < selected.count; i++) {
        const n00b_abi_module_v1 *row = n00bb_abi_merged_row(&merged,
            N00B_ABI_TABLE_MODULE, (uint32_t)i + 1);
        const n00b_abi_requirement_v1 *requirement =
            n00bb_abi_merged_row(&merged, N00B_ABI_TABLE_REQUIREMENT,
                                  (uint32_t)i + 1);
        assert(row && row->package_index == 1 && row->scc_index == 1
            && row->imports.count == 1 && requirement
            && requirement->requesting_module_index == i + 1);
        const uint32_t *values = (const void *)merged_u32->records;
        assert(values[merged_package->modules.first + i] == i + 1
            && values[merged_scc->members.first + i] == i + 1
            && values[row->imports.first] == (i + 1) % 3 + 1);
    }
    assert(n00bb_abi_name_pool_size(merged.strings) > 0);
    n00bb_abi_merged_rows_release(&merged);
    assert(!merged.tables && !merged.strings);
    uint8_t *linked = nullptr;
    size_t linked_bytes = 0;
    assert(n00bb_abi_link_selected_objects(&selected, &target,
        &linked, &linked_bytes)
        && count_string_pool_sections(linked, linked_bytes, &target) == 1);
    n00bb_abi_object_view_t final_view = {0};
    assert(n00bb_abi_object_open(&target, linked, linked_bytes,
        &final_view));
    const n00bb_abi_directory_entry_t *final_modules =
        n00bb_abi_object_table(&final_view, N00B_ABI_TABLE_MODULE);
    const n00bb_abi_directory_entry_t *final_sccs =
        n00bb_abi_object_table(&final_view, N00B_ABI_TABLE_SCC);
    const n00b_abi_module_v1 *first_module = n00bb_abi_object_row(
        &final_view, N00B_ABI_TABLE_MODULE, 1);
    n00bb_abi_string_view_t first_name;
    core_translation_unit_t *first_unit =
        core_link_unit_translation_unit(link, 0);
    assert(final_modules && final_modules->record_count == 4
        && final_sccs && final_sccs->record_count == 2
        && !n00bb_abi_object_table(&final_view,
            N00B_ABI_TABLE_REQUIREMENT)
        && !n00bb_abi_object_table(&final_view,
            N00B_ABI_TABLE_INDEX_PATCH)
        && first_module && first_unit
        && n00bb_abi_object_string(&final_view,
            &first_module->full_path, &first_name)
        && first_name.length == first_unit->module.identifier.u8_bytes
        && !memcmp(first_name.bytes, first_unit->module.identifier.data,
            first_name.length));
    n00bb_abi_object_close(&final_view);
    uint8_t *wrapped = nullptr;
    size_t wrapped_bytes = 0;
    assert(n00bb_abi_link_objects(link, &target, inputs, 4,
        &wrapped, &wrapped_bytes));
    n00bb_abi_object_view_t wrapped_view = {0};
    assert(n00bb_abi_object_open(&target, wrapped, wrapped_bytes,
        &wrapped_view));
    const n00bb_abi_directory_entry_t *wrapped_modules =
        n00bb_abi_object_table(&wrapped_view, N00B_ABI_TABLE_MODULE);
    assert(wrapped_modules && wrapped_modules->record_count == 4);
    n00bb_abi_object_close(&wrapped_view);
    core_free(wrapped);
    core_free(linked);
    n00b_abi_requirement_v1 *bad_import = (void *)n00bb_abi_object_row(
        &selected.views[0], N00B_ABI_TABLE_REQUIREMENT, 1);
    n00b_abi_string_v1 saved_import = bad_import->full_path;
    bad_import->full_path = (n00b_abi_string_v1){0};
    assert(!n00bb_abi_merge_rows(&selected, plan, &merged)
        && !merged.tables && !merged.strings);
    bad_import->full_path = saved_import;
    uint32_t saved_kind = bad_import->symbol_kind;
    bad_import->symbol_kind = N00B_ABI_SYMBOL_FUNCTION;
    linked = nullptr;
    linked_bytes = 0;
    assert(!n00bb_abi_link_selected_objects(&selected, &target,
        &linked, &linked_bytes) && !linked && !linked_bytes);
    bad_import->symbol_kind = saved_kind;
    n00b_abi_package_v1 *conflicting_package = (void *)
        n00bb_abi_object_row(&selected.views[1], N00B_ABI_TABLE_PACKAGE, 1);
    uint32_t saved_visibility = conflicting_package->visibility;
    conflicting_package->visibility = UINT32_MAX;
    assert(!n00bb_abi_rebuild_manifests(&selected, plan, &manifests)
        && !manifests.packages && !manifests.u32_stream);
    conflicting_package->visibility = saved_visibility;
    n00bb_abi_index_plan_free(plan);
    n00bb_abi_selected_objects_release(&selected);
    assert(!selected.views && !selected.count);
    assert(!n00bb_abi_select_objects(link, &target, inputs, 2, &selected)
        && !selected.views);
    wrapped = nullptr;
    wrapped_bytes = 0;
    assert(!n00bb_abi_link_objects(link, &target, inputs, 2,
        &wrapped, &wrapped_bytes) && !wrapped && !wrapped_bytes);
    inputs[3] = inputs[0];
    assert(!n00bb_abi_select_objects(link, &target, inputs, 4, &selected)
        && !selected.views);
    core_link_unit_free(link);
    for (size_t i = 0; i < 3; i++) {
        core_translation_unit_release(&units[i]);
        free(objects[i]);
    }
}

static void
test_self_import_selection(const char *path)
{
    core_translation_unit_t unit = {0};
    assert(init_unit(&unit, "cycle_self")
        && core_translation_unit_add_import(&unit, &unit));
    core_link_unit_t *link = core_link_unit_new();
    assert(link && core_link_unit_add_root(link, &unit)
        && core_link_unit_finalize_graph(link));
    size_t object_bytes;
    uint8_t *object = read_object(path, &object_bytes);
    n00bb_abi_object_input_t input = {
        .module = unit.module.identifier,
        .object = object, .object_bytes = object_bytes,
    };
    core_target_desc_t target;
    assert(core_target_profile(host_triple(), 0, &target));
    n00bb_abi_selected_objects_t selected = {0};
    assert(n00bb_abi_select_objects(link, &target, &input, 1, &selected)
        && selected.count == 1);
    n00bb_abi_index_plan_t *plan = n00bb_abi_index_plan_build(&selected);
    n00bb_abi_manifest_rows_t manifests = {0};
    assert(plan && n00bb_abi_rebuild_manifests(&selected, plan, &manifests)
        && manifests.package_count == 2 && manifests.scc_count == 2
        && manifests.sccs[1].members.count == 1
        && manifests.sccs[1].lexical_order.count == 1
        && manifests.u32_stream[manifests.sccs[1].members.first] == 1
        && manifests.u32_stream[manifests.sccs[1].lexical_order.first] == 1);
    n00bb_abi_manifest_rows_release(&manifests);
    n00bb_abi_merged_rows_t merged = {0};
    assert(n00bb_abi_merge_rows(&selected, plan, &merged));
    const n00b_abi_module_v1 *merged_module = n00bb_abi_merged_row(
        &merged, N00B_ABI_TABLE_MODULE, 1);
    const n00bb_abi_merged_table_t *stream = n00bb_abi_merged_table(
        &merged, N00B_ABI_TABLE_U32_STREAM);
    assert(merged_module && merged_module->imports.count == 1
        && stream && merged_module->imports.first < stream->record_count
        && core_le_load_u32(stream->records
            + (size_t)merged_module->imports.first * stream->record_bytes)
               == 1);
    n00bb_abi_merged_rows_release(&merged);
    n00bb_abi_index_plan_free(plan);
    n00bb_abi_selected_objects_release(&selected);
    core_link_unit_free(link);
    core_translation_unit_release(&unit);
    free(object);
}

static uint32_t
find_named_type(const n00bb_abi_object_view_t *view, const char *spelling)
{
    const n00bb_abi_directory_entry_t *table = n00bb_abi_object_table(
        view, N00B_ABI_TABLE_TYPE);
    assert(table);
    for (uint32_t i = 1; i < table->record_count; i++) {
        const n00b_abi_type_v1 *type = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_TYPE, i);
        n00bb_abi_string_view_t name;
        if (n00bb_abi_object_string(view, &type->canonical_name, &name)
            && name.length == strlen(spelling)
            && !memcmp(name.bytes, spelling, name.length)) return i;
    }
    return 0;
}

static void
test_imported_record_selection(const char *root_path, const char *dep_path)
{
    core_translation_unit_t root = {0}, dep = {0};
    assert(init_unit(&root, "record_root")
        && init_unit(&dep, "record_dep")
        && core_translation_unit_add_import(&root, &dep));
    core_link_unit_t *link = core_link_unit_new();
    assert(link && core_link_unit_add_root(link, &root)
        && core_link_unit_finalize_graph(link));
    size_t root_bytes, dep_bytes;
    uint8_t *root_object = read_object(root_path, &root_bytes);
    uint8_t *dep_object = read_object(dep_path, &dep_bytes);
    n00bb_abi_object_input_t inputs[2] = {
        {.module = dep.module.identifier, .object = dep_object,
         .object_bytes = dep_bytes},
        {.module = root.module.identifier, .object = root_object,
         .object_bytes = root_bytes},
    };
    core_target_desc_t target;
    assert(core_target_profile(host_triple(), 0, &target));
    n00bb_abi_selected_objects_t selected = {0};
    assert(n00bb_abi_select_objects(link, &target, inputs, 2, &selected)
        && selected.count == 2);
    size_t root_input = SIZE_MAX, dep_input = SIZE_MAX;
    for (size_t i = 0; i < selected.count; i++) {
        if (selected.views[i].object == root_object) root_input = i;
        if (selected.views[i].object == dep_object) dep_input = i;
    }
    assert(root_input < 2 && dep_input < 2);
    const n00bb_abi_object_view_t *root_view = &selected.views[root_input];
    const n00bb_abi_object_view_t *dep_view = &selected.views[dep_input];
    uint32_t root_type = find_named_type(root_view,
        "record_dep.SharedRecord");
    uint32_t dep_type = find_named_type(dep_view,
        "record_dep.SharedRecord");
    const n00b_abi_type_v1 *reference = n00bb_abi_object_row(root_view,
        N00B_ABI_TABLE_TYPE, root_type);
    const n00b_abi_type_v1 *definition = n00bb_abi_object_row(dep_view,
        N00B_ABI_TABLE_TYPE, dep_type);
    assert(root_type && dep_type && reference && definition
        && !reference->owner_module_index && !reference->layout_index
        && definition->owner_module_index == 1
        && definition->layout_index);
    const n00bb_abi_directory_entry_t *requirements =
        n00bb_abi_object_table(root_view, N00B_ABI_TABLE_REQUIREMENT);
    const n00b_abi_requirement_v1 *type_requirement = requirements
        && requirements->record_count == 3
        ? n00bb_abi_object_row(root_view, N00B_ABI_TABLE_REQUIREMENT, 2)
        : nullptr;
    n00bb_abi_string_view_t required_name;
    assert(type_requirement
        && type_requirement->symbol_kind == N00B_ABI_SYMBOL_TYPE
        && type_requirement->contract_type_index == root_type
        && type_requirement->requesting_module_index == 1
        && type_requirement->required_phase_mask
        && n00bb_abi_object_string(root_view,
            &type_requirement->full_path, &required_name)
        && required_name.length == strlen("record_dep.SharedRecord")
        && !memcmp(required_name.bytes, "record_dep.SharedRecord",
            required_name.length));
    n00bb_abi_index_plan_t *plan = n00bb_abi_index_plan_build(&selected);
    uint32_t from_root, from_dep, dep_module;
    assert(plan && n00bb_abi_index_plan_map(plan, root_input,
        N00B_ABI_TABLE_TYPE, root_type, &from_root)
        && n00bb_abi_index_plan_map(plan, dep_input,
            N00B_ABI_TABLE_TYPE, dep_type, &from_dep)
        && from_root == from_dep
        && n00bb_abi_index_plan_map(plan, dep_input,
            N00B_ABI_TABLE_MODULE, 1, &dep_module));
    n00bb_abi_merged_rows_t merged = {0};
    assert(n00bb_abi_merge_rows(&selected, plan, &merged));
    const n00b_abi_type_v1 *final_type = n00bb_abi_merged_row(&merged,
        N00B_ABI_TABLE_TYPE, from_root);
    assert(final_type && final_type->layout_index
        && final_type->owner_module_index == dep_module
        && final_type->fields_or_arms.count == 1);
    n00bb_abi_merged_rows_release(&merged);
    uint8_t *linked = nullptr;
    size_t linked_bytes = 0;
    assert(n00bb_abi_link_selected_objects(&selected, &target,
        &linked, &linked_bytes));
    core_free(linked);
    n00b_abi_requirement_v1 *mutable_requirement = (void *)type_requirement;
    uint64_t saved_capabilities = mutable_requirement->required_capabilities;
    mutable_requirement->required_capabilities = UINT64_MAX;
    linked = nullptr;
    linked_bytes = 0;
    assert(!n00bb_abi_link_selected_objects(&selected, &target,
        &linked, &linked_bytes) && !linked && !linked_bytes);
    mutable_requirement->required_capabilities = saved_capabilities;
    uint32_t saved_contract = mutable_requirement->contract_type_index;
    mutable_requirement->contract_type_index = find_named_type(root_view,
        "record_root.SharedRecord");
    assert(mutable_requirement->contract_type_index
        && mutable_requirement->contract_type_index != saved_contract);
    assert(!n00bb_abi_link_selected_objects(&selected, &target,
        &linked, &linked_bytes) && !linked && !linked_bytes);
    mutable_requirement->contract_type_index = saved_contract;
    size_t duplicate_bytes;
    uint8_t *duplicate_object = read_object(dep_path, &duplicate_bytes);
    n00bb_abi_object_view_t duplicate_views[2] = {0};
    assert(n00bb_abi_object_open(&target, dep_object, dep_bytes,
        &duplicate_views[0])
        && n00bb_abi_object_open(&target, duplicate_object,
            duplicate_bytes, &duplicate_views[1]));
    n00bb_abi_selected_objects_t duplicated = {
        .views = duplicate_views, .count = 2,
    };
    n00bb_abi_index_plan_t *duplicate_plan = n00bb_abi_index_plan_build(
        &duplicated);
    assert(duplicate_plan);
    n00bb_abi_index_plan_free(duplicate_plan);
    uint32_t duplicate_type = find_named_type(&duplicate_views[1],
        "record_dep.SharedRecord");
    const n00b_abi_type_v1 *owned = n00bb_abi_object_row(
        &duplicate_views[1], N00B_ABI_TABLE_TYPE, duplicate_type);
    assert(duplicate_type && owned && owned->fields_or_arms.count == 1);
    const uint8_t *field_slot = n00bb_abi_object_row(&duplicate_views[1],
        N00B_ABI_TABLE_U32_STREAM, owned->fields_or_arms.first);
    assert(field_slot);
    n00b_abi_field_v1 *field = (void *)n00bb_abi_object_row(
        &duplicate_views[1], N00B_ABI_TABLE_FIELD,
        core_le_load_u32(field_slot));
    assert(field);
    field->logical_bit_offset ^= 1;
    assert(!n00bb_abi_index_plan_build(&duplicated));
    field->logical_bit_offset ^= 1;
    n00bb_abi_object_close(&duplicate_views[0]);
    n00bb_abi_object_close(&duplicate_views[1]);
    free(duplicate_object);
    n00bb_abi_index_plan_free(plan);
    n00bb_abi_selected_objects_release(&selected);
    core_link_unit_free(link);
    core_translation_unit_release(&root);
    core_translation_unit_release(&dep);
    free(root_object);
    free(dep_object);
}

static const n00b_abi_symbol_v1 *
find_symbol(const n00bb_abi_object_view_t *view, const char *wanted,
            uint32_t kind)
{
    const n00bb_abi_directory_entry_t *symbols = n00bb_abi_object_table(
        view, N00B_ABI_TABLE_SYMBOL);
    assert(symbols);
    for (uint32_t i = 1; i < symbols->record_count; i++) {
        const n00b_abi_symbol_v1 *symbol = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_SYMBOL, i);
        n00bb_abi_string_view_t name;
        assert(symbol && n00bb_abi_object_string(view, &symbol->name, &name));
        if (symbol->kind == kind && name.length == strlen(wanted)
            && !memcmp(name.bytes, wanted, name.length)) return symbol;
    }
    return nullptr;
}

static void
test_linked_library_object(const char *path)
{
    size_t object_bytes;
    uint8_t *object = read_object(path, &object_bytes);
    core_target_desc_t target;
    assert(core_target_profile(host_triple(), 0, &target));
    n00bb_abi_object_view_t view = {0};
    assert(n00bb_abi_object_open(&target, object, object_bytes, &view));
    const n00bb_abi_directory_entry_t *modules =
        n00bb_abi_object_table(&view, N00B_ABI_TABLE_MODULE);
    assert(modules && modules->record_count == 3
        && !n00bb_abi_object_table(&view, N00B_ABI_TABLE_REQUIREMENT)
        && !n00bb_abi_object_table(&view, N00B_ABI_TABLE_INDEX_PATCH));
    uint32_t root_index = 0, dep_index = 0;
    for (uint32_t i = 1; i < modules->record_count; i++) {
        const n00b_abi_module_v1 *module = n00bb_abi_object_row(&view,
            N00B_ABI_TABLE_MODULE, i);
        n00bb_abi_string_view_t name;
        assert(module && n00bb_abi_object_string(&view,
            &module->full_path, &name));
        if (name.length == strlen("abi_value_root")
            && !memcmp(name.bytes, "abi_value_root", name.length))
            root_index = i;
        if (name.length == strlen("abi_value_dep")
            && !memcmp(name.bytes, "abi_value_dep", name.length))
            dep_index = i;
    }
    assert(root_index && dep_index && root_index != dep_index);
    const n00b_abi_module_v1 *root = n00bb_abi_object_row(&view,
        N00B_ABI_TABLE_MODULE, root_index);
    assert(root && root->imports.count == 1);
    const uint8_t *import = n00bb_abi_object_row(&view,
        N00B_ABI_TABLE_U32_STREAM, root->imports.first);
    assert(import && core_le_load_u32(import) == dep_index);
    const n00b_abi_symbol_v1 *reader = find_symbol(&view, "read_value",
        N00B_ABI_SYMBOL_FUNCTION);
    const n00b_abi_symbol_v1 *value = find_symbol(&view, "value",
        N00B_ABI_SYMBOL_VARIABLE);
    assert(reader && reader->module_index == root_index
        && value && value->module_index == dep_index);
    n00bb_abi_object_close(&view);
    free(object);
}

static void
test_imported_function_requirement(const char *root_path,
    const char *facade_path, const char *impl_path)
{
    const char *names[] = {"shared_link_first", "shared_link_dep.facade",
                           "shared_link_dep.impl"};
    const char *paths[] = {root_path, facade_path, impl_path};
    core_translation_unit_t units[3] = {0};
    uint8_t *objects[3];
    size_t object_bytes[3];
    n00bb_abi_object_input_t inputs[3];
    for (size_t i = 0; i < 3; i++) {
        assert(init_unit(&units[i], names[i]));
        objects[i] = read_object(paths[i], &object_bytes[i]);
        inputs[i] = (n00bb_abi_object_input_t){
            .module = units[i].module.identifier,
            .object = objects[i], .object_bytes = object_bytes[i],
        };
    }
    assert(core_translation_unit_add_import(&units[0], &units[1])
        && core_translation_unit_add_import(&units[1], &units[2]));
    core_link_unit_t *link = core_link_unit_new();
    assert(link && core_link_unit_add_root(link, &units[0])
        && core_link_unit_finalize_graph(link));
    core_target_desc_t target;
    assert(core_target_profile(host_triple(), 0, &target));
    n00bb_abi_selected_objects_t selected = {0};
    assert(n00bb_abi_select_objects(link, &target, inputs, 3, &selected)
        && selected.count == 3);
    const n00bb_abi_object_view_t *views[3] = {0};
    for (size_t i = 0; i < selected.count; i++)
        for (size_t j = 0; j < 3; j++)
            if (selected.views[i].object == objects[j])
                views[j] = &selected.views[i];
    assert(views[0] && views[1] && views[2]);
    const n00bb_abi_directory_entry_t *requirements =
        n00bb_abi_object_table(views[0], N00B_ABI_TABLE_REQUIREMENT);
    assert(requirements && requirements->record_count == 3);
    n00b_abi_requirement_v1 *function_requirement = (void *)
        n00bb_abi_object_row(views[0], N00B_ABI_TABLE_REQUIREMENT, 2);
    n00bb_abi_string_view_t path;
    assert(function_requirement
        && function_requirement->symbol_kind == N00B_ABI_SYMBOL_FUNCTION
        && function_requirement->requesting_module_index == 1
        && function_requirement->required_phase_mask
        && n00bb_abi_object_string(views[0],
            &function_requirement->full_path, &path)
        && path.length == strlen("shared_link_dep.impl.shared_link_value")
        && !memcmp(path.bytes, "shared_link_dep.impl.shared_link_value",
            path.length));
    const n00b_abi_type_v1 *contract = n00bb_abi_object_row(views[0],
        N00B_ABI_TABLE_TYPE, function_requirement->contract_type_index);
    assert(contract && contract->kind == N00B_ABI_TYPE_FUNCTION);
    assert(!find_symbol(views[1], "shared_link_value",
        N00B_ABI_SYMBOL_FUNCTION));
    const n00b_abi_symbol_v1 *definition = find_symbol(views[2],
        "shared_link_value", N00B_ABI_SYMBOL_FUNCTION);
    assert(definition && definition->flags & N00B_ABI_SYMBOL_FLAG_EXPORTED);
    uint8_t *linked = nullptr;
    size_t linked_bytes = 0;
    assert(n00bb_abi_link_selected_objects(&selected, &target,
        &linked, &linked_bytes));
    core_free(linked);
    linked = nullptr;
    linked_bytes = 0;
    uint32_t saved_type = function_requirement->contract_type_index;
    function_requirement->contract_type_index = 0;
    assert(!n00bb_abi_link_selected_objects(&selected, &target,
        &linked, &linked_bytes) && !linked && !linked_bytes);
    function_requirement->contract_type_index = saved_type;
    n00bb_abi_selected_objects_release(&selected);
    core_link_unit_free(link);
    for (size_t i = 0; i < 3; i++) {
        core_translation_unit_release(&units[i]);
        free(objects[i]);
    }
}

static void
test_imported_value_requirement(const char *root_path, const char *dep_path)
{
    core_translation_unit_t root = {0}, dep = {0};
    assert(init_unit(&root, "abi_value_root")
        && init_unit(&dep, "abi_value_dep")
        && core_translation_unit_add_import(&root, &dep));
    core_link_unit_t *link = core_link_unit_new();
    assert(link && core_link_unit_add_root(link, &root)
        && core_link_unit_finalize_graph(link));
    size_t root_bytes, dep_bytes;
    uint8_t *root_object = read_object(root_path, &root_bytes);
    uint8_t *dep_object = read_object(dep_path, &dep_bytes);
    n00bb_abi_object_input_t inputs[] = {
        {.module = root.module.identifier, .object = root_object,
            .object_bytes = root_bytes},
        {.module = dep.module.identifier, .object = dep_object,
            .object_bytes = dep_bytes},
    };
    core_target_desc_t target;
    assert(core_target_profile(host_triple(), 0, &target));
    n00bb_abi_selected_objects_t selected = {0};
    assert(n00bb_abi_select_objects(link, &target, inputs, 2, &selected)
        && selected.count == 2);
    const n00bb_abi_object_view_t *root_view = nullptr, *dep_view = nullptr;
    for (size_t i = 0; i < selected.count; i++) {
        if (selected.views[i].object == root_object)
            root_view = &selected.views[i];
        if (selected.views[i].object == dep_object)
            dep_view = &selected.views[i];
    }
    assert(root_view && dep_view);
    const n00bb_abi_directory_entry_t *requirements =
        n00bb_abi_object_table(root_view, N00B_ABI_TABLE_REQUIREMENT);
    assert(requirements && requirements->record_count == 3);
    n00b_abi_requirement_v1 *value_requirement = (void *)
        n00bb_abi_object_row(root_view, N00B_ABI_TABLE_REQUIREMENT, 2);
    n00bb_abi_string_view_t path;
    assert(value_requirement
        && value_requirement->symbol_kind == N00B_ABI_SYMBOL_VARIABLE
        && value_requirement->requesting_module_index == 1
        && value_requirement->required_layout_index
        && value_requirement->required_phase_mask & N00B_ABI_PHASE_RUNTIME
        && n00bb_abi_object_string(root_view,
            &value_requirement->full_path, &path)
        && path.length == strlen("abi_value_dep.value")
        && !memcmp(path.bytes, "abi_value_dep.value", path.length));
    const n00b_abi_symbol_v1 *symbol = find_symbol(dep_view, "value",
        N00B_ABI_SYMBOL_VARIABLE);
    assert(symbol && symbol->type_index
        && symbol->flags & N00B_ABI_SYMBOL_FLAG_EXPORTED);
    const n00bb_abi_directory_entry_t *storages =
        n00bb_abi_object_table(dep_view, N00B_ABI_TABLE_STORAGE);
    assert(storages && storages->record_count == 2);
    n00b_abi_storage_v1 *storage = (void *)n00bb_abi_object_row(dep_view,
        N00B_ABI_TABLE_STORAGE, 1);
    assert(storage && storage->symbol_index
        && n00bb_abi_object_row(dep_view, N00B_ABI_TABLE_SYMBOL,
            storage->symbol_index) == symbol
        && storage->type_index == symbol->type_index);
    uint8_t *linked = nullptr;
    size_t linked_bytes = 0;
    assert(n00bb_abi_link_selected_objects(&selected, &target,
        &linked, &linked_bytes));
    core_free(linked);
    linked = nullptr;
    linked_bytes = 0;
    uint32_t saved_symbol = storage->symbol_index;
    storage->symbol_index = 0;
    assert(!n00bb_abi_link_selected_objects(&selected, &target,
        &linked, &linked_bytes) && !linked && !linked_bytes);
    storage->symbol_index = saved_symbol;
    uint32_t saved_contract = value_requirement->contract_type_index;
    value_requirement->contract_type_index = 0;
    assert(!n00bb_abi_link_selected_objects(&selected, &target,
        &linked, &linked_bytes) && !linked && !linked_bytes);
    value_requirement->contract_type_index = saved_contract;
    uint32_t saved_layout = value_requirement->required_layout_index;
    value_requirement->required_layout_index = UINT32_MAX;
    assert(!n00bb_abi_link_selected_objects(&selected, &target,
        &linked, &linked_bytes) && !linked && !linked_bytes);
    value_requirement->required_layout_index = saved_layout;
    n00bb_abi_selected_objects_release(&selected);
    core_link_unit_free(link);
    core_translation_unit_release(&root);
    core_translation_unit_release(&dep);
    free(root_object);
    free(dep_object);
}

static void
test_duplicate_component(const core_target_desc_t *target,
                         const char *path, const uint8_t *object,
                         size_t object_bytes)
{
    size_t duplicate_bytes;
    uint8_t *duplicate_object = read_object(path, &duplicate_bytes);
    n00bb_abi_object_view_t views[2] = {0};
    assert(n00bb_abi_object_open(target, object, object_bytes, &views[0])
        && n00bb_abi_object_open(target, duplicate_object,
            duplicate_bytes, &views[1]));
    n00bb_abi_selected_objects_t duplicated = {.views = views, .count = 2};
    n00bb_abi_index_plan_t *plan = n00bb_abi_index_plan_build(&duplicated);
    assert(plan);
    n00bb_abi_index_plan_free(plan);
    const n00bb_abi_directory_entry_t *types = n00bb_abi_object_table(
        &views[1], N00B_ABI_TABLE_TYPE);
    uint8_t *component = nullptr;
    for (uint32_t i = 1; i < types->record_count && !component; i++) {
        const n00b_abi_type_v1 *type = n00bb_abi_object_row(&views[1],
            N00B_ABI_TABLE_TYPE, i);
        if (type->kind == N00B_ABI_TYPE_ARRAY && type->components.count)
            component = (void *)n00bb_abi_object_row(&views[1],
                N00B_ABI_TABLE_U32_STREAM, type->components.first);
    }
    assert(component);
    uint32_t saved = core_le_load_u32(component);
    core_le_store_u32(component, saved ? 0 : 1);
    assert(!n00bb_abi_index_plan_build(&duplicated));
    core_le_store_u32(component, saved);
    n00bb_abi_object_close(&views[0]);
    n00bb_abi_object_close(&views[1]);
    free(duplicate_object);
}

static void
test_duplicate_member(const char *path, bool variant)
{
    size_t left_bytes, right_bytes;
    uint8_t *left_object = read_object(path, &left_bytes);
    uint8_t *right_object = read_object(path, &right_bytes);
    core_target_desc_t target;
    assert(core_target_profile(host_triple(), 0, &target));
    n00bb_abi_object_view_t views[2] = {0};
    assert(n00bb_abi_object_open(&target, left_object, left_bytes, &views[0])
        && n00bb_abi_object_open(&target, right_object, right_bytes,
                                  &views[1]));
    n00bb_abi_selected_objects_t duplicated = {.views = views, .count = 2};
    n00bb_abi_index_plan_t *plan = n00bb_abi_index_plan_build(&duplicated);
    assert(plan);
    n00bb_abi_index_plan_free(plan);
    const n00bb_abi_directory_entry_t *types = n00bb_abi_object_table(
        &views[1], N00B_ABI_TABLE_TYPE);
    uint32_t member_index = 0;
    for (uint32_t i = 1; i < types->record_count && !member_index; i++) {
        const n00b_abi_type_v1 *type = n00bb_abi_object_row(&views[1],
            N00B_ABI_TABLE_TYPE, i);
        if (type->fields_or_arms.count
            && (variant ? type->kind == N00B_ABI_TYPE_VARIANT
                : type->kind == N00B_ABI_TYPE_ENUM)) {
            const uint8_t *slot = n00bb_abi_object_row(&views[1],
                N00B_ABI_TABLE_U32_STREAM, type->fields_or_arms.first);
            if (slot) member_index = core_le_load_u32(slot);
        }
    }
    assert(member_index);
    if (variant) {
        n00b_abi_arm_v1 *arm = (void *)n00bb_abi_object_row(&views[1],
            N00B_ABI_TABLE_ARM, member_index);
        assert(arm);
        arm->tag_value ^= 1;
        assert(!n00bb_abi_index_plan_build(&duplicated));
        arm->tag_value ^= 1;
    }
    else {
        n00b_abi_enum_item_v1 *item = (void *)n00bb_abi_object_row(&views[1],
            N00B_ABI_TABLE_ENUM_ITEM, member_index);
        assert(item);
        item->value_low ^= 1;
        assert(!n00bb_abi_index_plan_build(&duplicated));
        item->value_low ^= 1;
    }
    n00bb_abi_object_close(&views[0]);
    n00bb_abi_object_close(&views[1]);
    free(left_object);
    free(right_object);
}

static void
test_imported_callback_reference(const char *path)
{
    size_t left_bytes, right_bytes;
    uint8_t *left_object = read_object(path, &left_bytes);
    uint8_t *right_object = read_object(path, &right_bytes);
    core_target_desc_t target;
    assert(core_target_profile(host_triple(), 0, &target));
    n00bb_abi_object_view_t views[2] = {0};
    assert(n00bb_abi_object_open(&target, left_object, left_bytes, &views[0])
        && n00bb_abi_object_open(&target, right_object, right_bytes,
                                  &views[1]));
    const n00bb_abi_directory_entry_t *types = n00bb_abi_object_table(
        &views[1], N00B_ABI_TABLE_TYPE);
    n00b_abi_type_v1 *reference = nullptr;
    for (uint32_t i = 1; i < types->record_count && !reference; i++) {
        n00b_abi_type_v1 *type = (void *)n00bb_abi_object_row(&views[1],
            N00B_ABI_TABLE_TYPE, i);
        if (type->kind == N00B_ABI_TYPE_CALLBACK
            && type->components.count && type->owner_module_index)
            reference = type;
    }
    assert(reference);
    uint32_t owner = reference->owner_module_index;
    uint32_t layout = reference->layout_index;
    n00b_abi_range_v1 components = reference->components;
    reference->owner_module_index = 0;
    reference->layout_index = 0;
    reference->components = (n00b_abi_range_v1){0};
    n00bb_abi_selected_objects_t duplicated = {.views = views, .count = 2};
    n00bb_abi_index_plan_t *plan = n00bb_abi_index_plan_build(&duplicated);
    assert(plan);
    n00bb_abi_index_plan_free(plan);
    reference->owner_module_index = owner;
    reference->layout_index = layout;
    reference->components = components;
    n00bb_abi_object_close(&views[0]);
    n00bb_abi_object_close(&views[1]);
    free(left_object);
    free(right_object);
}

static void
test_duplicate_signature(const char *path)
{
    size_t left_bytes, right_bytes;
    uint8_t *left_object = read_object(path, &left_bytes);
    uint8_t *right_object = read_object(path, &right_bytes);
    core_target_desc_t target;
    assert(core_target_profile(host_triple(), 0, &target));
    n00bb_abi_object_view_t views[2] = {0};
    assert(n00bb_abi_object_open(&target, left_object, left_bytes, &views[0])
        && n00bb_abi_object_open(&target, right_object, right_bytes,
                                  &views[1]));
    n00bb_abi_selected_objects_t duplicated = {.views = views, .count = 2};
    n00bb_abi_index_plan_t *plan = n00bb_abi_index_plan_build(&duplicated);
    assert(plan);
    n00bb_abi_index_plan_free(plan);
    const n00bb_abi_directory_entry_t *types = n00bb_abi_object_table(
        &views[1], N00B_ABI_TABLE_TYPE);
    n00b_abi_call_signature_v1 *signature = nullptr;
    for (uint32_t i = 1; i < types->record_count && !signature; i++) {
        const n00b_abi_type_v1 *type = n00bb_abi_object_row(&views[1],
            N00B_ABI_TABLE_TYPE, i);
        if (type->kind != N00B_ABI_TYPE_FUNCTION
            || !type->call_signature_index) continue;
        n00b_abi_call_signature_v1 *candidate = (void *)n00bb_abi_object_row(
            &views[1], N00B_ABI_TABLE_CALL_SIGNATURE,
            type->call_signature_index);
        if (candidate && candidate->positional_parameters.count
            && candidate->success_type_index
                != candidate->failure_type_index)
            signature = candidate;
    }
    assert(signature);
    const uint8_t *slot = n00bb_abi_object_row(&views[1],
        N00B_ABI_TABLE_U32_STREAM,
        signature->positional_parameters.first);
    assert(slot);
    n00b_abi_parameter_v1 *parameter = (void *)n00bb_abi_object_row(
        &views[1], N00B_ABI_TABLE_PARAMETER, core_le_load_u32(slot));
    assert(parameter);
    uint32_t saved_mode = parameter->passing_mode;
    parameter->passing_mode = saved_mode == N00B_ABI_PASS_IN
        ? N00B_ABI_PASS_MODIFY : N00B_ABI_PASS_IN;
    assert(!n00bb_abi_index_plan_build(&duplicated));
    parameter->passing_mode = saved_mode;
    uint32_t saved_failure = signature->failure_type_index;
    signature->failure_type_index = signature->success_type_index;
    assert(!n00bb_abi_index_plan_build(&duplicated));
    signature->failure_type_index = saved_failure;
    n00bb_abi_object_close(&views[0]);
    n00bb_abi_object_close(&views[1]);
    free(left_object);
    free(right_object);
}

static void
test_native_string_pool_consolidation(void)
{
    const char *triples[] = {
        "x86_64-linux", "arm64-macos", "x86_64-windows",
    };
    const core_string_t value =
        CORE_STRING_STATIC("the same complete immutable string");
    for (size_t target_index = 0;
         target_index < sizeof(triples) / sizeof(*triples);
         target_index++) {
        core_target_desc_t target;
        assert(core_target_profile(triples[target_index], 0, &target));
        bool mach = target.object_format == CORE_TARGET_OBJECT_MACH_O;
        const char *pool_name = mach ? "__n00b_strings"
            : target.object_format == CORE_TARGET_OBJECT_COFF
                ? ".rdata$S" : ".rodata.n00b_strings";
        n00bb_abi_name_pool_t *source_pool = n00bb_abi_name_pool_new();
        n00bb_abi_name_pool_t *merged_pool = n00bb_abi_name_pool_new();
        n00b_abi_string_v1 encoded;
        size_t source_offset;
        assert(source_pool && merged_pool
            && n00bb_abi_name_pool_value(source_pool, value,
                N00B_ABI_STRING_ASCII, &encoded, &source_offset)
            && source_offset == 0);
        n00bb_abi_native_sections_t sections = {
            .sections = core_alloc_array(core_object_emit_section_t, 4),
            .maps = core_alloc_array(n00bb_abi_native_section_map_t, 4),
            .count = 4,
        };
        n00bb_abi_native_relocations_t native = {
            .symbols = core_alloc_array(core_object_emit_symbol_t, 2),
            .symbol_count = 2,
            .relocations = core_alloc_array(core_object_emit_absolute64_t, 2),
            .relocation_count = 2,
        };
        assert(sections.sections && sections.maps
            && native.symbols && native.relocations);
        for (size_t input = 0; input < 2; input++) {
            size_t data_index = 2 * input, pool_index = data_index + 1;
            uint8_t *data = core_alloc_array(uint8_t,
                sizeof(n00b_abi_string_v1));
            uint8_t *bytes = core_alloc_array(uint8_t, value.u8_bytes);
            memcpy(data, &encoded, sizeof(encoded));
            memcpy(bytes, value.data, value.u8_bytes);
            sections.sections[data_index] =
                (core_object_emit_section_t){
                    .name = core_strndup(input ? ".data_b" : ".data_a", 7),
                    .segment = mach ? core_strndup("__DATA", 6) : nullptr,
                    .bytes = data, .size = sizeof(encoded), .alignment = 8,
                    .writable = true,
                };
            sections.sections[pool_index] =
                (core_object_emit_section_t){
                    .name = core_strndup(pool_name, strlen(pool_name)),
                    .segment = mach ? core_strndup("__TEXT", 6) : nullptr,
                    .bytes = bytes, .size = value.u8_bytes, .alignment = 1,
                };
            sections.maps[data_index] =
                (n00bb_abi_native_section_map_t){
                    .input = input, .local_section_index = 2,
                };
            sections.maps[pool_index] =
                (n00bb_abi_native_section_map_t){
                    .input = input, .local_section_index = 3,
                };
            native.symbols[input] = (core_object_emit_symbol_t){
                .name = core_strndup("n00b_string_pool", 16),
                .section_index = (uint32_t)(3 + 2 * input),
            };
            native.relocations[input] =
                (core_object_emit_absolute64_t){
                    .section_index = (uint32_t)(2 + 2 * input),
                    .symbol_index = (uint32_t)input,
                };
        }
        uint8_t *first_data = (uint8_t *)sections.sections[0].bytes;
        uint8_t selector = first_data[15];
        first_data[15] = 0xff;
        assert(!n00bb_abi_consolidate_native_strings(&target,
            merged_pool, &sections, &native)
            && sections.count == 4 && native.symbol_count == 2);
        first_data[15] = selector;
        assert(n00bb_abi_consolidate_native_strings(&target,
            merged_pool, &sections, &native)
            && sections.count == 2 && native.symbol_count == 0
            && native.relocation_count == 2
            && native.relocations[0].section_index == 2
            && native.relocations[1].section_index == 3
            && native.relocations[0].symbol_index == 0
            && native.relocations[1].symbol_index == 0
            && native.relocations[0].addend_bits == 0
            && native.relocations[1].addend_bits == 0
            && n00bb_abi_name_pool_size(merged_pool) == value.u8_bytes
            && !memcmp(n00bb_abi_name_pool_bytes(merged_pool),
                value.data, value.u8_bytes)
            && !memcmp(sections.sections[0].bytes, &encoded,
                sizeof(encoded)));
        n00bb_abi_native_relocations_release(&native);
        n00bb_abi_native_sections_release(&sections);
        n00bb_abi_name_pool_free(merged_pool);
        n00bb_abi_name_pool_free(source_pool);
    }
}

static void
test_link_index_patch(void)
{
    typedef struct {
        n00b_abi_type_v1 types[2];
        n00b_abi_index_patch_v1 patches[2];
    } patch_fixture_t;
    patch_fixture_t metadata[2] = {0};
    n00bb_abi_directory_entry_t directories[2][2] = {0};
    n00bb_abi_object_view_t views[2] = {0};
    for (size_t i = 0; i < 2; i++) {
        metadata[i].types[0].kind = N00B_ABI_TYPE_NIL;
        metadata[i].types[0].typehash = 1;
        metadata[i].types[1].kind = N00B_ABI_TYPE_PRIMITIVE;
        metadata[i].types[1].typehash = 100 + i;
        directories[i][0] = (n00bb_abi_directory_entry_t){
            .kind = N00B_ABI_TABLE_TYPE,
            .record_bytes = sizeof(n00b_abi_type_v1),
            .record_count = 2,
            .file_offset = offsetof(patch_fixture_t, types),
        };
        directories[i][1] = (n00bb_abi_directory_entry_t){
            .kind = N00B_ABI_TABLE_INDEX_PATCH,
            .record_bytes = sizeof(n00b_abi_index_patch_v1),
            .record_count = i ? 2 : 1,
            .file_offset = offsetof(patch_fixture_t, patches),
            .byte_length = (i ? 2 : 1)
                * sizeof(n00b_abi_index_patch_v1),
        };
        views[i].metadata.bytes = (const uint8_t *)&metadata[i];
        views[i].metadata.size = sizeof(metadata[i]);
        views[i].directory = directories[i];
        views[i].directory_count = 2;
    }
    metadata[1].patches[1] = (n00b_abi_index_patch_v1){
        .target_section_index = 7,
        .referenced_table_kind = N00B_ABI_TABLE_TYPE,
        .local_index = 1,
        .patch_kind = N00BB_ABI_PATCH_INDEX,
    };
    n00bb_abi_selected_objects_t selected = {.views = views, .count = 2};
    n00bb_abi_index_plan_t *plan =
        n00bb_abi_index_plan_build(&selected);
    uint32_t final_index = 0;
    assert(plan && n00bb_abi_index_plan_map(plan, 1,
        N00B_ABI_TABLE_TYPE, 1, &final_index) && final_index == 2);
    n00bb_abi_merged_table_t type_table = {
        .kind = N00B_ABI_TABLE_TYPE,
        .record_count = 3,
    };
    n00bb_abi_merged_rows_t merged = {
        .tables = &type_table, .table_count = 1,
    };
    uint8_t copied_bytes[4];
    core_le_store_u32(copied_bytes, 1);
    core_object_emit_section_t section = {
        .name = ".data", .bytes = copied_bytes, .size = sizeof(copied_bytes),
        .alignment = 4,
    };
    n00bb_abi_native_section_map_t map = {
        .input = 1, .local_section_index = 7,
    };
    n00bb_abi_native_sections_t copied = {
        .sections = &section, .maps = &map, .count = 1,
    };
    assert(n00bb_abi_apply_link_patches(&selected, plan, &merged, &copied)
        && core_le_load_u32(copied_bytes) == 2
        && metadata[1].patches[1].local_index == 1);
    core_le_store_u32(copied_bytes, 1);
    metadata[1].patches[1].local_index = 2;
    assert(!n00bb_abi_apply_link_patches(&selected, plan, &merged, &copied)
        && core_le_load_u32(copied_bytes) == 1);
    n00bb_abi_index_plan_free(plan);
}

static void
test_specialization_remap(void)
{
    typedef struct {
        n00b_abi_type_v1 types[2];
        n00b_abi_specialization_link_v1 links[2];
    } specialization_fixture_t;
    specialization_fixture_t metadata = {0};
    metadata.types[0].kind = N00B_ABI_TYPE_NIL;
    metadata.types[0].typehash = 1;
    metadata.types[1].kind = N00B_ABI_TYPE_PRIMITIVE;
    metadata.types[1].typehash = 2;
    metadata.types[1].specialization_start_ix = 1;
    metadata.types[1].specialization_end_ix = 2;
    metadata.links[1] = 1;
    n00bb_abi_directory_entry_t directory[] = {
        {.kind = N00B_ABI_TABLE_TYPE,
         .record_bytes = sizeof(n00b_abi_type_v1), .record_count = 2,
         .file_offset = offsetof(specialization_fixture_t, types)},
        {.kind = N00B_ABI_TABLE_SPECIALIZATION_LINK,
         .record_bytes = sizeof(n00b_abi_specialization_link_v1),
         .record_count = 2,
         .file_offset = offsetof(specialization_fixture_t, links)},
    };
    n00bb_abi_object_view_t view = {
        .metadata = {.bytes = (const uint8_t *)&metadata,
                     .size = sizeof(metadata)},
        .directory = directory,
        .directory_count = sizeof(directory) / sizeof(*directory),
    };
    n00bb_abi_selected_objects_t selected = {.views = &view, .count = 1};
    n00bb_abi_index_plan_t *plan = n00bb_abi_index_plan_build(&selected);
    assert(plan);
    n00b_abi_type_v1 type;
    n00b_abi_specialization_link_v1 link;
    assert(n00bb_abi_remap_row(&selected, plan, 0, N00B_ABI_TABLE_TYPE,
        1, &type, sizeof(type), nullptr)
        && type.specialization_start_ix == 1
        && type.specialization_end_ix == 2
        && n00bb_abi_remap_row(&selected, plan, 0,
            N00B_ABI_TABLE_SPECIALIZATION_LINK, 1, &link, sizeof(link),
            nullptr) && link == 1);
    metadata.types[1].specialization_start_ix = 0;
    metadata.types[1].specialization_end_ix = 1;
    assert(!n00bb_abi_remap_row(&selected, plan, 0, N00B_ABI_TABLE_TYPE,
        1, &type, sizeof(type), nullptr));
    metadata.links[1] = 0;
    assert(n00bb_abi_remap_row(&selected, plan, 0,
        N00B_ABI_TABLE_SPECIALIZATION_LINK, 1, &link, sizeof(link),
        nullptr) && link == 0);
    metadata.types[1].kind = N00B_ABI_TYPE_VARIABLE;
    metadata.types[1].specialization_start_ix = 0;
    metadata.types[1].specialization_end_ix = 0;
    assert(!n00bb_abi_remap_row(&selected, plan, 0, N00B_ABI_TABLE_TYPE,
        1, &type, sizeof(type), nullptr));
    n00bb_abi_index_plan_free(plan);
}

static void
test_type_backlinks_remap(void)
{
    typedef struct {
        n00b_abi_type_v1 types[3];
        n00b_abi_layout_v1 layouts[2];
        n00b_abi_trace_v1 traces[2];
        n00b_abi_field_v1 fields[2];
        uint32_t members[1];
    } backlink_fixture_t;
    backlink_fixture_t metadata = {0};
    metadata.types[0].kind = N00B_ABI_TYPE_NIL;
    metadata.types[0].typehash = 1;
    metadata.types[1].kind = N00B_ABI_TYPE_RECORD;
    metadata.types[1].typehash = 2;
    metadata.types[1].layout_index = 1;
    metadata.types[1].tracing_index = 1;
    metadata.types[1].fields_or_arms.count = 1;
    metadata.types[2].kind = N00B_ABI_TYPE_PRIMITIVE;
    metadata.types[2].typehash = 3;
    metadata.layouts[1].type_index = 1;
    metadata.traces[1].type_index = 1;
    metadata.fields[1].parent_type_index = 1;
    metadata.members[0] = 1;
    n00bb_abi_directory_entry_t directory[] = {
        {.kind = N00B_ABI_TABLE_TYPE,
         .record_bytes = sizeof(n00b_abi_type_v1), .record_count = 3,
         .file_offset = offsetof(backlink_fixture_t, types)},
        {.kind = N00B_ABI_TABLE_LAYOUT,
         .record_bytes = sizeof(n00b_abi_layout_v1), .record_count = 2,
         .file_offset = offsetof(backlink_fixture_t, layouts)},
        {.kind = N00B_ABI_TABLE_TRACE,
         .record_bytes = sizeof(n00b_abi_trace_v1), .record_count = 2,
         .file_offset = offsetof(backlink_fixture_t, traces)},
        {.kind = N00B_ABI_TABLE_FIELD,
         .record_bytes = sizeof(n00b_abi_field_v1), .record_count = 2,
         .file_offset = offsetof(backlink_fixture_t, fields)},
        {.kind = N00B_ABI_TABLE_U32_STREAM,
         .record_bytes = sizeof(uint32_t), .record_count = 1,
         .file_offset = offsetof(backlink_fixture_t, members)},
    };
    n00bb_abi_object_view_t view = {
        .metadata = {.bytes = (const uint8_t *)&metadata,
                     .size = sizeof(metadata)},
        .directory = directory,
        .directory_count = sizeof(directory) / sizeof(*directory),
    };
    n00bb_abi_selected_objects_t selected = {.views = &view, .count = 1};
    n00bb_abi_index_plan_t *plan = n00bb_abi_index_plan_build(&selected);
    assert(plan);
    uint32_t values[1] = {0}, kinds[1] = {0};
    n00bb_abi_u32_stream_build_t stream = {
        .values = values, .kinds = kinds, .count = 1,
    };
    n00b_abi_type_v1 type;
    assert(n00bb_abi_remap_row(&selected, plan, 0,
        N00B_ABI_TABLE_TYPE, 1, &type, sizeof(type), &stream));
    metadata.layouts[1].type_index = 2;
    assert(!n00bb_abi_remap_row(&selected, plan, 0,
        N00B_ABI_TABLE_TYPE, 1, &type, sizeof(type), &stream));
    metadata.layouts[1].type_index = 1;
    metadata.traces[1].type_index = 2;
    assert(!n00bb_abi_remap_row(&selected, plan, 0,
        N00B_ABI_TABLE_TYPE, 1, &type, sizeof(type), &stream));
    metadata.traces[1].type_index = 1;
    metadata.fields[1].parent_type_index = 2;
    assert(!n00bb_abi_remap_row(&selected, plan, 0,
        N00B_ABI_TABLE_TYPE, 1, &type, sizeof(type), &stream));
    metadata.fields[1].parent_type_index = 1;
    metadata.types[1].fields_or_arms = (n00b_abi_range_v1){.first = 1};
    assert(!n00bb_abi_remap_row(&selected, plan, 0,
        N00B_ABI_TABLE_TYPE, 1, &type, sizeof(type), &stream));
    n00bb_abi_index_plan_free(plan);
}

static void
test_metadata_string_uses_relocation_pool(void)
{
    const char *triples[] = {
        "x86_64-linux", "arm64-macos", "x86_64-windows",
    };
    for (size_t i = 0; i < sizeof(triples) / sizeof(*triples); i++) {
        core_target_desc_t target;
        assert(core_target_profile(triples[i], 0, &target));
        n00bb_abi_name_pool_t *pool = n00bb_abi_name_pool_new();
        n00b_abi_package_v1 packages[2] = {0};
        const core_string_t wanted =
            CORE_STRING_STATIC("the-correct-long-package-name");
        assert(pool && n00bb_abi_name_pool_encode(pool, wanted,
            N00B_ABI_TABLE_PACKAGE, 1,
            offsetof(n00b_abi_package_v1, name), &packages[1].name));
        n00bb_abi_image_key_t key;
        n00bb_abi_image_header_t header = {0};
        n00bb_abi_directory_entry_t entry = {
            .kind = N00B_ABI_TABLE_PACKAGE,
            .schema_version = 1,
            .record_bytes = sizeof(n00b_abi_package_v1),
            .record_count = 2,
            .file_offset = N00BB_ABI_HEADER_BYTES + N00BB_ABI_DIRECTORY_BYTES,
            .byte_length = sizeof(packages),
        };
        size_t schema_count;
        const n00bb_abi_table_schema_t *schemas =
            n00bb_abi_v1_table_schemas(&schema_count);
        uint8_t metadata[N00BB_ABI_HEADER_BYTES
            + N00BB_ABI_DIRECTORY_BYTES + sizeof(packages)] = {0};
        memcpy(metadata + entry.file_offset, packages, sizeof(packages));
        assert(n00bb_abi_image_key_for_target(&target, &key));
        header.key = key;
        header.required_alignment = _Alignof(n00b_abi_package_v1);
        header.directory_count = 1;
        header.directory_offset = N00BB_ABI_HEADER_BYTES;
        header.total_extent = sizeof(metadata);
        assert(n00bb_abi_container_write(metadata, sizeof(metadata),
            &header, &entry, schemas, schema_count, false));
        core_object_emit_absolute64_t *relocations = nullptr;
        size_t relocation_count = 0;
        assert(n00bb_abi_name_pool_relocations(pool, &entry, 1,
            &relocations, &relocation_count) && relocation_count == 1);

        bool mach = target.object_format == CORE_TARGET_OBJECT_MACH_O;
        const char *section_name = mach ? "__n00b_strings"
            : target.object_format == CORE_TARGET_OBJECT_COFF
                ? ".rdata$S" : ".rodata.n00b_strings";
        core_object_emit_section_t sections[] = {
            {.name = mach ? "__n00b_meta"
                    : target.object_format == CORE_TARGET_OBJECT_COFF
                        ? ".n00b$M" : ".data.rel.ro.n00b_meta",
             .segment = mach ? "__DATA_CONST" : nullptr,
             .bytes = metadata, .size = sizeof(metadata),
             .alignment = header.required_alignment},
            {.name = section_name, .segment = mach ? "__TEXT" : nullptr,
             .bytes = (const uint8_t *)"wrong-pool", .size = 10,
             .alignment = 1},
            {.name = section_name, .segment = mach ? "__TEXT" : nullptr,
             .bytes = n00bb_abi_name_pool_bytes(pool),
             .size = n00bb_abi_name_pool_size(pool), .alignment = 1},
        };
        core_object_emit_symbol_t symbol = {
            .name = "n00b_string_pool", .section_index = 3,
        };
        uint8_t *object = nullptr;
        size_t object_bytes = 0;
        assert(core_object_emit(&target, sections, 3,
            &symbol, 1, relocations, relocation_count,
            &object, &object_bytes));
        core_object_section_t object_metadata;
        assert(core_object_section_find(object, object_bytes,
            target.object_format, mach ? "__DATA_CONST" : nullptr,
            sections[0].name, &object_metadata)
            && n00bb_abi_container_write((uint8_t *)object_metadata.bytes,
                object_metadata.size, &header, &entry, schemas,
                schema_count, false));
        n00bb_abi_object_view_t view = {0};
        n00bb_abi_string_view_t decoded;
        assert(n00bb_abi_object_open(&target, object, object_bytes, &view));
        const n00b_abi_package_v1 *package = n00bb_abi_object_row(&view,
            N00B_ABI_TABLE_PACKAGE, 1);
        assert(package && n00bb_abi_object_string(&view, &package->name,
            &decoded) && decoded.length == wanted.u8_bytes
            && !memcmp(decoded.bytes, wanted.data, wanted.u8_bytes));
        n00bb_abi_object_close(&view);
        core_free(object);
        core_free(relocations);
        n00bb_abi_name_pool_free(pool);
    }
}

static void
test_native_relocation_transfer(void)
{
    const char *triples[] = {
        "x86_64-linux", "arm64-macos", "x86_64-windows",
    };
    for (size_t target_index = 0;
         target_index < sizeof(triples) / sizeof(*triples);
         target_index++) {
        core_target_desc_t target;
        assert(core_target_profile(triples[target_index], 0, &target));
        bool mach = target.object_format == CORE_TARGET_OBJECT_MACH_O;
        uint8_t metadata[8] = {0}, data[24] = {0}, pool[8] = {0};
        core_object_emit_section_t inputs[] = {
            {.name = mach ? "__meta" : ".meta",
             .segment = mach ? "__DATA_CONST" : nullptr,
             .bytes = metadata, .size = sizeof(metadata), .alignment = 8},
            {.name = mach ? "__data" : ".data",
             .segment = mach ? "__DATA" : nullptr,
             .bytes = data, .size = sizeof(data), .alignment = 8,
             .writable = true},
            {.name = mach ? "__pool" : ".rodata",
             .segment = mach ? "__DATA_CONST" : nullptr,
             .bytes = pool, .size = sizeof(pool), .alignment = 8},
        };
        core_object_emit_symbol_t symbols[] = {
            {.name = "external", .exported = true},
            {.name = "local_pool", .section_index = 3},
            {.name = "public_data", .section_index = 2,
             .exported = true},
        };
        core_object_emit_absolute64_t relocations[] = {
            {.section_index = 2, .symbol_index = 0, .addend_bits = 5},
            {.section_index = 2, .section_offset = 8,
             .symbol_index = 1, .addend_bits = 3},
            {.section_index = 2, .section_offset = 16,
             .symbol_index = 1, .addend_bits = 7},
        };
        uint8_t *object = nullptr;
        size_t object_bytes = 0;
        assert(core_object_emit(&target, inputs,
            sizeof(inputs) / sizeof(*inputs), symbols,
            sizeof(symbols) / sizeof(*symbols), relocations,
            sizeof(relocations) / sizeof(*relocations),
            &object, &object_bytes));
        uint32_t metadata_index;
        assert(core_object_emit_native_section_index(&target,
            sizeof(inputs) / sizeof(*inputs), 1, &metadata_index));
        n00bb_abi_object_view_t view = {
            .object = object, .object_bytes = object_bytes,
            .format = target.object_format,
            .metadata.native_index = metadata_index,
        };
        n00bb_abi_selected_objects_t selected = {
            .views = &view, .count = 1,
        };
        n00bb_abi_native_sections_t native = {0};
        n00bb_abi_native_relocations_t transferred = {0};
        assert(n00bb_abi_collect_native_sections(&selected, &target,
            &native) && native.count == 2
            && n00bb_abi_collect_native_relocations(&selected, &native,
                &transferred)
            && transferred.symbol_count == 2
            && transferred.relocation_count == 3);
        assert(transferred.relocations[0].symbol_index !=
            transferred.relocations[1].symbol_index
            && transferred.relocations[1].symbol_index ==
                transferred.relocations[2].symbol_index
            && transferred.relocations[0].addend_bits == 5
            && transferred.relocations[1].addend_bits == 3
            && transferred.relocations[2].addend_bits == 7);
        core_object_emit_section_t output_sections[] = {
            inputs[0], native.sections[0], native.sections[1],
        };
        uint8_t *linked = nullptr;
        size_t linked_bytes = 0;
        assert(core_object_emit(&target, output_sections,
            sizeof(output_sections) / sizeof(*output_sections),
            transferred.symbols, transferred.symbol_count,
            transferred.relocations, transferred.relocation_count,
            &linked, &linked_bytes));
        core_object_section_t output_data;
        assert(core_object_section_find(linked, linked_bytes,
            target.object_format, mach ? "__DATA" : nullptr,
            mach ? "__data" : ".data", &output_data)
            && output_data.relocation_count == 3);
        for (uint32_t i = 0; i < output_data.relocation_count; i++) {
            core_object_absolute64_relocation_t decoded;
            assert(core_object_absolute64_relocation_at(&output_data,
                target.object_format, i, &decoded)
                && decoded.section_offset == relocations[i].section_offset);
        }
        core_free(linked);
        n00bb_abi_native_relocations_release(&transferred);
        n00bb_abi_native_sections_release(&native);
        core_free(object);
    }
}

static void
test_string_storage_link(const char *path)
{
    size_t source_bytes;
    uint8_t *source = read_object(path, &source_bytes);
    core_translation_unit_t unit = {0};
    core_target_desc_t target;
    assert(init_unit(&unit, "string_storage")
        && core_target_profile(host_triple(), 0, &target));
    core_link_unit_t *link = core_link_unit_new();
    assert(link && core_link_unit_add_root(link, &unit)
        && core_link_unit_finalize_graph(link));
    n00bb_abi_object_input_t input = {
        .module = unit.module.identifier,
        .object = source, .object_bytes = source_bytes,
    };
    n00bb_abi_selected_objects_t selected = {0};
    assert(n00bb_abi_select_objects(link, &target, &input, 1, &selected));
    uint8_t *linked = nullptr;
    size_t linked_bytes = 0;
    assert(n00bb_abi_link_selected_objects(&selected, &target,
        &linked, &linked_bytes)
        && count_string_pool_sections(linked, linked_bytes, &target) == 1);
    n00bb_abi_object_view_t view = {0};
    assert(n00bb_abi_object_open(&target, linked, linked_bytes, &view));
    bool mach = target.object_format == CORE_TARGET_OBJECT_MACH_O;
    const char *pool_name = mach ? "__n00b_strings"
        : target.object_format == CORE_TARGET_OBJECT_COFF
            ? ".rdata$S" : ".rodata.n00b_strings";
    core_object_section_t pool;
    assert(core_object_section_find(linked, linked_bytes,
        target.object_format, mach ? "__TEXT" : nullptr,
        pool_name, &pool));
    const char *literal = "a longer immutable string value";
    size_t offsets[2] = {0};
    bool found[2] = {false, false};
    const n00bb_abi_directory_entry_t *table = n00bb_abi_object_table(
        &view, N00B_ABI_TABLE_STORAGE);
    assert(table);
    for (uint32_t i = 1; i < table->record_count; i++) {
        const n00b_abi_storage_v1 *storage = n00bb_abi_object_row(
            &view, N00B_ABI_TABLE_STORAGE, i);
        const n00b_abi_symbol_v1 *symbol = storage
            ? n00bb_abi_object_row(&view, N00B_ABI_TABLE_SYMBOL,
                storage->symbol_index) : nullptr;
        n00bb_abi_string_view_t name;
        assert(symbol && n00bb_abi_object_string(&view,
            &symbol->name, &name));
        int which = name.length == strlen("long_label")
            && !memcmp(name.bytes, "long_label", name.length) ? 0
            : name.length == strlen("repeated_label")
                && !memcmp(name.bytes, "repeated_label", name.length)
                ? 1 : -1;
        if (which < 0) continue;
        core_object_section_t section;
        assert(!found[which] && storage->segment_index
            && core_object_section_at(linked, linked_bytes,
                target.object_format, storage->segment_index, &section)
            && storage->byte_offset <= section.size
            && sizeof(n00b_abi_string_v1)
                <= section.size - storage->byte_offset);
        n00bb_abi_string_header_t header;
        assert(n00bb_abi_string_decode(
            section.bytes + storage->byte_offset,
            sizeof(n00b_abi_string_v1), &header)
            && header.tag == N00BB_ABI_STRING_LONG
            && header.length == strlen(literal));
        bool relocated = false;
        for (uint32_t j = 0; j < section.relocation_count; j++) {
            core_object_absolute64_relocation_t relocation;
            assert(core_object_absolute64_relocation_at(&section,
                target.object_format, j, &relocation));
            if (relocation.section_offset != storage->byte_offset)
                continue;
            assert(!relocated);
            uint64_t offset;
            if (relocation.target_is_section) {
                assert(relocation.native_symbol_index == pool.native_index
                    && relocation.addend_bits >= pool.virtual_address);
                offset = relocation.addend_bits - pool.virtual_address;
            }
            else {
                core_object_symbol_t target_symbol;
                assert(core_object_symbol_at(linked, linked_bytes,
                    target.object_format, &section,
                    relocation.native_symbol_index, &target_symbol)
                    && target_symbol.native_section_index
                        == pool.native_index);
                offset = target_symbol.section_offset
                    + relocation.addend_bits;
            }
            assert(offset <= pool.size
                && strlen(literal) <= pool.size - offset
                && !memcmp(pool.bytes + offset, literal,
                    strlen(literal)));
            offsets[which] = (size_t)offset;
            relocated = true;
        }
        assert(relocated);
        found[which] = true;
    }
    assert(found[0] && found[1] && offsets[0] == offsets[1]);
    n00bb_abi_object_close(&view);
    core_free(linked);
    n00bb_abi_selected_objects_release(&selected);
    core_link_unit_free(link);
    core_translation_unit_release(&unit);
    free(source);
}

static void
test_semantic_rows_link(const char *path, bool trait)
{
    size_t source_bytes;
    uint8_t *source = read_object(path, &source_bytes);
    core_translation_unit_t unit = {0};
    core_target_desc_t target;
    const char *name = trait ? "trait_methods" : "where_excludes";
    assert(init_unit(&unit, name)
        && core_target_profile(host_triple(), 0, &target));
    core_link_unit_t *link = core_link_unit_new();
    assert(link && core_link_unit_add_root(link, &unit)
        && core_link_unit_finalize_graph(link));
    n00bb_abi_object_input_t input = {
        .module = unit.module.identifier,
        .object = source, .object_bytes = source_bytes,
    };
    n00bb_abi_selected_objects_t selected = {0};
    assert(n00bb_abi_select_objects(link, &target, &input, 1, &selected));
    uint8_t *linked = nullptr;
    size_t linked_bytes = 0;
    assert(n00bb_abi_link_selected_objects(&selected, &target,
        &linked, &linked_bytes));
    n00bb_abi_object_view_t view = {0};
    assert(n00bb_abi_object_open(&target, linked, linked_bytes, &view));
    const n00bb_abi_directory_entry_t *types = n00bb_abi_object_table(
        &view, N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *stream = n00bb_abi_object_table(
        &view, N00B_ABI_TABLE_U32_STREAM);
    assert(types && stream);
    if (trait) {
        const n00bb_abi_directory_entry_t *methods =
            n00bb_abi_object_table(&view, N00B_ABI_TABLE_TRAIT_METHOD);
        assert(methods && methods->record_count == 3);
        const n00b_abi_type_v1 *owner = nullptr;
        uint32_t owner_index = 0;
        for (uint32_t i = 1; i < types->record_count; i++) {
            const n00b_abi_type_v1 *type = n00bb_abi_object_row(&view,
                N00B_ABI_TABLE_TYPE, i);
            if (type->kind != N00B_ABI_TYPE_TRAIT
                || !type->owner_module_index) continue;
            assert(!owner);
            owner = type;
            owner_index = i;
        }
        assert(owner && owner->fields_or_arms.count == 2
            && owner->fields_or_arms.first < stream->record_count
            && owner->fields_or_arms.count <= stream->record_count
                - owner->fields_or_arms.first);
        const char *names[] = {"read", "reset"};
        for (uint32_t ordinal = 0; ordinal < 2; ordinal++) {
            const uint8_t *slot = n00bb_abi_object_row(&view,
                N00B_ABI_TABLE_U32_STREAM,
                owner->fields_or_arms.first + ordinal);
            uint32_t index = core_le_load_u32(slot);
            assert(index && index < methods->record_count);
            const n00b_abi_trait_method_v1 *method =
                n00bb_abi_object_row(&view,
                    N00B_ABI_TABLE_TRAIT_METHOD, index);
            const n00b_abi_symbol_v1 *symbol = n00bb_abi_object_row(
                &view, N00B_ABI_TABLE_SYMBOL, method->symbol_index);
            n00bb_abi_string_view_t text;
            assert(method->trait_type_index == owner_index
                && method->declaration_ordinal == ordinal
                && method->signature_type_index < types->record_count
                && symbol && symbol->type_index
                    == method->signature_type_index
                && n00bb_abi_object_string(&view, &symbol->name, &text)
                && text.length == strlen(names[ordinal])
                && !memcmp(text.bytes, names[ordinal], text.length));
        }
        uint8_t *changed = malloc(source_bytes);
        assert(changed);
        memcpy(changed, source, source_bytes);
        n00bb_abi_object_view_t changed_view = {0};
        assert(n00bb_abi_object_open(&target, changed, source_bytes,
            &changed_view));
        n00bb_abi_object_view_t duplicate_views[] = {
            selected.views[0], changed_view,
        };
        n00bb_abi_selected_objects_t duplicates = {
            .views = duplicate_views, .count = 2,
        };
        n00bb_abi_index_plan_t *plan =
            n00bb_abi_index_plan_build(&duplicates);
        assert(plan);
        n00bb_abi_index_plan_free(plan);
        n00b_abi_trait_method_v1 *changed_method = (void *)
            n00bb_abi_object_row(&changed_view,
                N00B_ABI_TABLE_TRAIT_METHOD, 1);
        assert(changed_method);
        changed_method->declaration_ordinal++;
        assert(!n00bb_abi_index_plan_build(&duplicates));
        n00bb_abi_object_close(&changed_view);
        free(changed);
    }
    else {
        const n00bb_abi_directory_entry_t *constraints =
            n00bb_abi_object_table(&view, N00B_ABI_TABLE_CONSTRAINT);
        const n00b_abi_function_v1 *function = n00bb_abi_object_row(
            &view, N00B_ABI_TABLE_FUNCTION, 1);
        assert(constraints && constraints->record_count > 1
            && function && function->constraints.count == 1
            && function->constraints.first < stream->record_count);
        const uint8_t *slot = n00bb_abi_object_row(&view,
            N00B_ABI_TABLE_U32_STREAM, function->constraints.first);
        uint32_t index = core_le_load_u32(slot);
        assert(index && index < constraints->record_count);
        const n00b_abi_constraint_v1 *constraint = n00bb_abi_object_row(
            &view, N00B_ABI_TABLE_CONSTRAINT, index);
        assert(constraint && constraint->kind
            == N00B_ABI_CONSTRAINT_EXCLUDES
            && constraint->target_type_index < types->record_count);
        const n00b_abi_type_v1 *excluded = n00bb_abi_object_row(
            &view, N00B_ABI_TABLE_TYPE,
            constraint->target_type_index);
        assert(excluded && excluded->kind == N00B_ABI_TYPE_STRING);
    }
    n00bb_abi_object_close(&view);
    core_free(linked);
    n00bb_abi_selected_objects_release(&selected);
    core_link_unit_free(link);
    core_translation_unit_release(&unit);
    free(source);
}

static uint32_t
generic_function_type(const n00bb_abi_object_view_t *view, const char *name)
{
    const n00bb_abi_directory_entry_t *functions = n00bb_abi_object_table(
        view, N00B_ABI_TABLE_FUNCTION);
    assert(functions);
    for (uint32_t i = 1; i < functions->record_count; i++) {
        const n00b_abi_function_v1 *function = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_FUNCTION, i);
        const n00b_abi_symbol_v1 *symbol = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_SYMBOL, function->symbol_index);
        n00bb_abi_string_view_t text;
        assert(symbol && n00bb_abi_object_string(view, &symbol->name, &text));
        if (text.length == strlen(name)
            && !memcmp(text.bytes, name, text.length))
            return symbol->type_index;
    }
    assert(false);
    return 0;
}

static void
test_generic_type_identity(const char *path)
{
    size_t source_bytes;
    uint8_t *source = read_object(path, &source_bytes);
    core_target_desc_t target;
    assert(core_target_profile(host_triple(), 0, &target));
    n00bb_abi_object_view_t original = {0};
    assert(n00bb_abi_object_open(&target, source, source_bytes, &original));
    uint32_t first = generic_function_type(&original, "first");
    uint32_t second = generic_function_type(&original, "second");
    const n00b_abi_type_v1 *a = n00bb_abi_object_row(&original,
        N00B_ABI_TABLE_TYPE, first);
    const n00b_abi_type_v1 *b = n00bb_abi_object_row(&original,
        N00B_ABI_TABLE_TYPE, second);
    assert(first != second && a && b && a->typehash == b->typehash
        && (a->flags & N00B_ABI_TYPE_FLAG_NON_CONCRETE)
        && (b->flags & N00B_ABI_TYPE_FLAG_NON_CONCRETE));

    core_translation_unit_t unit = {0};
    assert(init_unit(&unit, "generic_collision"));
    core_link_unit_t *link = core_link_unit_new();
    assert(link && core_link_unit_add_root(link, &unit)
        && core_link_unit_finalize_graph(link));
    n00bb_abi_object_input_t input = {
        .module = unit.module.identifier,
        .object = source, .object_bytes = source_bytes,
    };
    uint8_t *linked = nullptr;
    size_t linked_bytes = 0;
    assert(n00bb_abi_link_objects(link, &target, &input, 1,
        &linked, &linked_bytes));
    n00bb_abi_object_view_t final = {0};
    assert(n00bb_abi_object_open(&target, linked, linked_bytes, &final));
    assert(generic_function_type(&final, "first")
        != generic_function_type(&final, "second"));
    n00bb_abi_object_close(&final);
    core_free(linked);
    core_link_unit_free(link);
    core_translation_unit_release(&unit);
    n00bb_abi_object_close(&original);
    free(source);
}

int
main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3 || argc == 4 || argc == 5);
    test_native_string_pool_consolidation();
    test_link_index_patch();
    test_specialization_remap();
    test_type_backlinks_remap();
    test_metadata_string_uses_relocation_pool();
    test_native_relocation_transfer();
    test_static_string_identity();
    if (argc == 2 && !strcmp(argv[1], "--strings")) return 0;
    if (argc == 5 && !strcmp(argv[1], "--function-import")) {
        test_imported_function_requirement(argv[2], argv[3], argv[4]);
        return 0;
    }
    if (argc == 4) {
        if (!strcmp(argv[1], "--value-import")) {
            test_imported_value_requirement(argv[2], argv[3]);
            return 0;
        }
        if (!strcmp(argv[1], "--records")) {
            test_imported_record_selection(argv[2], argv[3]);
            return 0;
        }
        test_cycle_selection(argv + 1);
        return 0;
    }
    if (argc == 3) {
        if (!strcmp(argv[1], "--self"))
            test_self_import_selection(argv[2]);
        else if (!strcmp(argv[1], "--linked-library"))
            test_linked_library_object(argv[2]);
        else if (!strcmp(argv[1], "--string-storage"))
            test_string_storage_link(argv[2]);
        else if (!strcmp(argv[1], "--constraint"))
            test_semantic_rows_link(argv[2], false);
        else if (!strcmp(argv[1], "--trait-methods"))
            test_semantic_rows_link(argv[2], true);
        else if (!strcmp(argv[1], "--generic-collision"))
            test_generic_type_identity(argv[2]);
        else if (!strcmp(argv[1], "--variant"))
            test_duplicate_member(argv[2], true);
        else if (!strcmp(argv[1], "--enum"))
            test_duplicate_member(argv[2], false);
        else if (!strcmp(argv[1], "--callback"))
            test_imported_callback_reference(argv[2]);
        else if (!strcmp(argv[1], "--signature"))
            test_duplicate_signature(argv[2]);
        else assert(false);
        return 0;
    }
    size_t length;
    uint8_t *object = read_object(argv[1], &length);
    core_target_desc_t target;
    assert(core_target_profile(host_triple(), 0, &target));
    n00bb_abi_object_view_t view = {0};
    assert(n00bb_abi_object_open(&target, object, length, &view));
    const n00bb_abi_directory_entry_t *types = n00bb_abi_object_table(
        &view, N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *modules = n00bb_abi_object_table(
        &view, N00B_ABI_TABLE_MODULE);
    const n00bb_abi_directory_entry_t *shapes = n00bb_abi_object_table(
        &view, N00B_ABI_TABLE_SHAPE);
    assert(types && types->record_count > 1
        && modules && modules->record_count == 2
        && shapes && shapes->record_count == 2
        && !n00bb_abi_object_table(&view, UINT32_MAX));
    const n00b_abi_type_v1 *nil = n00bb_abi_object_row(&view,
        N00B_ABI_TABLE_TYPE, 0);
    const n00b_abi_module_v1 *module = n00bb_abi_object_row(&view,
        N00B_ABI_TABLE_MODULE, 1);
    const n00b_abi_shape_v1 *shape = n00bb_abi_object_row(&view,
        N00B_ABI_TABLE_SHAPE, 1);
    const n00b_abi_native_binding_v1 *binding = n00bb_abi_object_row(&view,
        N00B_ABI_TABLE_NATIVE_BINDING, 1);
    n00bb_abi_string_view_t name = {0};
    assert(nil && nil->kind == N00B_ABI_TYPE_NIL
        && module && module->package_index == 1
        && shape && shape->rank == 1 && shape->total_count == 2
        && !n00bb_abi_object_row(&view, N00B_ABI_TABLE_TYPE,
                                 types->record_count));
    assert(n00bb_abi_object_string(&view, &module->full_path, &name)
        && name.length == strlen("array_storage")
        && !memcmp(name.bytes, "array_storage", name.length));
    assert(binding && n00bb_abi_object_string(&view,
        &binding->native_name, &name)
        && name.length > N00BB_ABI_STRING_SHORT_CAPACITY
        && name.length >= strlen("values")
        && !memcmp(name.bytes + name.length - strlen("values"),
                   "values", strlen("values")));
    test_duplicate_component(&target, argv[1], object, length);
    n00bb_abi_name_pool_t *pool = n00bb_abi_name_pool_new();
    assert(pool);
    n00b_abi_module_v1 pooled_module = *module;
    n00b_abi_native_binding_v1 pooled_binding = *binding;
    assert(n00bb_abi_repool_row_strings(&view, N00B_ABI_TABLE_MODULE,
        1, 1, &pooled_module, sizeof(pooled_module), pool)
        && n00bb_abi_repool_row_strings(&view,
            N00B_ABI_TABLE_NATIVE_BINDING, 1, 1,
            &pooled_binding, sizeof(pooled_binding), pool));
    n00bb_abi_string_header_t pooled_header;
    assert(n00bb_abi_string_decode((const uint8_t *)&pooled_module.full_path,
        sizeof(pooled_module.full_path), &pooled_header)
        && pooled_header.tag == N00BB_ABI_STRING_SHORT
        && pooled_header.length == strlen("array_storage"));
    assert(n00bb_abi_string_decode(
        (const uint8_t *)&pooled_binding.native_name,
        sizeof(pooled_binding.native_name), &pooled_header)
        && pooled_header.tag == N00BB_ABI_STRING_LONG
        && pooled_header.length == name.length
        && n00bb_abi_name_pool_size(pool) == name.length
        && !memcmp(n00bb_abi_name_pool_bytes(pool), name.bytes, name.length));
    core_object_emit_absolute64_t *relocations = nullptr;
    size_t relocation_count = 0;
    assert(n00bb_abi_name_pool_relocations(pool, view.directory,
        view.directory_count, &relocations, &relocation_count)
        && relocation_count == 1);
    const n00bb_abi_directory_entry_t *bindings =
        n00bb_abi_object_table(&view, N00B_ABI_TABLE_NATIVE_BINDING);
    assert(relocations[0].section_offset == bindings->file_offset
        + bindings->record_bytes
        + offsetof(n00b_abi_native_binding_v1, native_name)
        && relocations[0].addend_bits == 0);
    free(relocations);
    n00bb_abi_name_pool_free(pool);
    n00b_abi_native_binding_v1 *mutable_binding = (void *)binding;
    uint64_t saved_selector = mutable_binding->native_name.long_value.selector;
    mutable_binding->native_name.long_value.selector =
        (saved_selector & ~(UINT64_C(3) << N00B_ABI_STRING_TAG_SHIFT))
        | ((uint64_t)N00B_ABI_STRING_SPAN << N00B_ABI_STRING_TAG_SHIFT);
    assert(n00bb_abi_object_string(&view, &binding->native_name, &name));
    pool = n00bb_abi_name_pool_new();
    pooled_binding = *binding;
    assert(pool && n00bb_abi_repool_row_strings(&view,
        N00B_ABI_TABLE_NATIVE_BINDING, 1, 1, &pooled_binding,
        sizeof(pooled_binding), pool)
        && n00bb_abi_string_decode(
            (const uint8_t *)&pooled_binding.native_name,
            sizeof(pooled_binding.native_name), &pooled_header)
        && pooled_header.tag == N00BB_ABI_STRING_SPAN
        && pooled_header.length == name.length);
    mutable_binding->native_name.long_value.selector = saved_selector;
    n00bb_abi_name_pool_free(pool);
    n00bb_abi_string_view_t unchanged_name = {.bytes = object, .length = 1};
    assert(!n00bb_abi_object_string(&view,
        (const n00b_abi_string_v1 *)object, &unchanged_name)
        && unchanged_name.bytes == object && unchanged_name.length == 1);
    core_translation_unit_t unit = {0};
    assert(init_unit(&unit, "array_storage"));
    core_link_unit_t *link = core_link_unit_new();
    assert(link && core_link_unit_add_root(link, &unit)
        && core_link_unit_finalize_graph(link));
    n00bb_abi_object_input_t input = {
        .module = unit.module.identifier,
        .object = object, .object_bytes = length,
    };
    n00bb_abi_selected_objects_t selected = {0};
    assert(n00bb_abi_select_objects(link, &target, &input, 1, &selected)
        && selected.count == 1 && selected.views[0].object == object);
    uint8_t *combined = nullptr;
    size_t combined_bytes = 0;
    assert(n00bb_abi_link_selected_objects(&selected, &target,
        &combined, &combined_bytes)
        && count_string_pool_sections(combined, combined_bytes,
            &target) == 1);
    n00bb_abi_object_view_t combined_view = {0};
    assert(n00bb_abi_object_open(&target, combined, combined_bytes,
        &combined_view)
        && !n00bb_abi_object_table(&combined_view,
            N00B_ABI_TABLE_REQUIREMENT));
    const n00b_abi_storage_v1 *combined_storage =
        n00bb_abi_object_row(&combined_view, N00B_ABI_TABLE_STORAGE, 1);
    const n00b_abi_native_binding_v1 *combined_binding =
        n00bb_abi_object_row(&combined_view,
            N00B_ABI_TABLE_NATIVE_BINDING, 1);
    core_object_section_t combined_section;
    assert(combined_storage && combined_storage->segment_index
        && core_object_section_at(combined, combined_bytes,
            target.object_format, combined_storage->segment_index,
            &combined_section)
        && combined_storage->byte_offset < combined_section.size
        && combined_binding
        && n00bb_abi_object_string(&combined_view,
            &combined_binding->native_name, &name)
        && name.length > N00BB_ABI_STRING_SHORT_CAPACITY);
    uint32_t combined_section_count = 0;
    assert(core_object_section_count(combined, combined_bytes,
        target.object_format, &combined_section_count));
    bool has_native_relocations = false;
    for (uint32_t i = 1; i <= combined_section_count; i++) {
        if (i == combined_view.metadata.native_index) continue;
        core_object_section_t section;
        if (core_object_section_at(combined, combined_bytes,
            target.object_format, i, &section))
            has_native_relocations |= section.relocation_count != 0;
    }
    assert(has_native_relocations);
    n00bb_abi_object_close(&combined_view);
    core_free(combined);
    n00bb_abi_index_plan_t *plan = n00bb_abi_index_plan_build(&selected);
    uint32_t mapped;
    assert(plan && n00bb_abi_index_plan_map(plan, 0,
        N00B_ABI_TABLE_MODULE, 1, &mapped) && mapped == 1
        && !n00bb_abi_index_plan_map(plan, 0,
            N00B_ABI_TABLE_MODULE, 2, &mapped));
    check_remapped_rows(&selected, plan);
    n00bb_abi_manifest_rows_t manifests = {0};
    assert(n00bb_abi_rebuild_manifests(&selected, plan, &manifests)
        && manifests.package_count == 2 && manifests.scc_count == 1
        && manifests.packages[1].modules.count == 1
        && manifests.u32_stream[manifests.packages[1].modules.first] == 1);
    n00bb_abi_manifest_rows_release(&manifests);
    n00bb_abi_merged_rows_t merged = {0};
    assert(n00bb_abi_merge_rows(&selected, plan, &merged));
    const n00b_abi_shape_v1 *merged_shape = n00bb_abi_merged_row(
        &merged, N00B_ABI_TABLE_SHAPE, 1);
    const n00bb_abi_merged_table_t *u64 = n00bb_abi_merged_table(
        &merged, N00B_ABI_TABLE_U64_STREAM);
    const n00b_abi_module_v1 *merged_module = n00bb_abi_merged_row(
        &merged, N00B_ABI_TABLE_MODULE, 1);
    assert(merged_shape && merged_shape->rank == 1
        && merged_shape->total_count == 2
        && u64 && merged_shape->extents.count == 1
        && merged_shape->extents.first < u64->record_count
        && core_le_load_u64(u64->records
            + (size_t)merged_shape->extents.first * u64->record_bytes) == 2
        && merged_module && merged_module->package_index == 1);
    const n00bb_abi_merged_table_t *storage = n00bb_abi_merged_table(
        &merged, N00B_ABI_TABLE_STORAGE);
    const n00bb_abi_merged_table_t *merged_bindings = n00bb_abi_merged_table(
        &merged, N00B_ABI_TABLE_NATIVE_BINDING);
    assert(storage && merged_bindings && storage->record_count > 1
        && merged_bindings->record_count > 1 && storage->origins
        && merged_bindings->origins);
    n00bb_abi_native_sections_t native_sections = {0};
    assert(n00bb_abi_collect_native_sections(&selected, &target,
        &native_sections) && native_sections.count > 0);
    n00bb_abi_native_relocations_t native_relocations = {0};
    assert(n00bb_abi_collect_native_relocations(&selected, &native_sections,
        &native_relocations) && native_relocations.relocation_count > 0);
    size_t source_relocation_count = 0;
    for (size_t i = 0; i < native_sections.count; i++) {
        core_object_section_t source;
        assert(core_object_section_at(object, length, target.object_format,
            native_sections.maps[i].local_section_index, &source));
        source_relocation_count += source.relocation_count;
    }
    assert(native_relocations.relocation_count == source_relocation_count);
    for (size_t i = 0; i < native_relocations.relocation_count; i++) {
        const core_object_emit_absolute64_t *rel =
            &native_relocations.relocations[i];
        assert(rel->section_index >= 2
            && rel->section_index <= native_sections.count + 1
            && rel->symbol_index < native_relocations.symbol_count);
    }
    n00bb_abi_merged_rows_t exports_merged = {0};
    assert(n00bb_abi_merge_rows(&selected, plan, &exports_merged)
        && n00bb_abi_rebase_native_references(&exports_merged,
            native_sections.maps, native_sections.count));
    const n00bb_abi_merged_table_t *export_bindings =
        n00bb_abi_merged_table(&exports_merged,
            N00B_ABI_TABLE_NATIVE_BINDING);
    assert(export_bindings && export_bindings->record_count > 1);
    const n00bb_abi_row_origin_t *first_origin =
        &export_bindings->origins[1];
    const n00b_abi_native_binding_v1 *first_source =
        n00bb_abi_object_row(&selected.views[first_origin->input],
            N00B_ABI_TABLE_NATIVE_BINDING, first_origin->local);
    n00bb_abi_string_view_t first_name;
    assert(first_source && n00bb_abi_object_string(
        &selected.views[first_origin->input], &first_source->native_name,
        &first_name));
    n00bb_abi_native_relocations_t unresolved = {
        .symbols = core_alloc_array(core_object_emit_symbol_t, 1),
        .symbol_count = 1,
    };
    unresolved.symbols[0] = (core_object_emit_symbol_t){
        .name = core_strndup((const char *)first_name.bytes,
                             first_name.length),
        .exported = true,
    };
    assert(n00bb_abi_add_native_bindings(&selected, &exports_merged,
        &native_sections, &unresolved)
        && unresolved.symbols[0].section_index >= 2);
    n00bb_abi_native_relocations_release(&unresolved);
    n00bb_abi_native_relocations_t conflicting = {
        .symbols = core_alloc_array(core_object_emit_symbol_t, 1),
        .symbol_count = 1,
    };
    conflicting.symbols[0] = (core_object_emit_symbol_t){
        .name = core_strndup((const char *)first_name.bytes,
                             first_name.length),
        .section_index = 1, .exported = true,
    };
    assert(!n00bb_abi_add_native_bindings(&selected, &exports_merged,
        &native_sections, &conflicting)
        && conflicting.symbol_count == 1
        && conflicting.symbols[0].section_index == 1);
    n00bb_abi_native_relocations_release(&conflicting);
    size_t referenced_symbol_count = native_relocations.symbol_count;
    assert(n00bb_abi_add_native_bindings(&selected, &exports_merged,
        &native_sections, &native_relocations)
        && native_relocations.symbol_count > referenced_symbol_count);
    for (uint32_t i = 1; i < export_bindings->record_count; i++) {
        const n00bb_abi_row_origin_t *origin = &export_bindings->origins[i];
        const n00b_abi_native_binding_v1 *row =
            n00bb_abi_merged_row(&exports_merged,
                N00B_ABI_TABLE_NATIVE_BINDING, i);
        const n00b_abi_native_binding_v1 *source = n00bb_abi_object_row(
            &selected.views[origin->input], N00B_ABI_TABLE_NATIVE_BINDING,
            origin->local);
        n00bb_abi_string_view_t name;
        assert(row && source && n00bb_abi_object_string(
            &selected.views[origin->input], &source->native_name, &name));
        bool found = false;
        for (size_t j = 0; j < native_relocations.symbol_count; j++) {
            const core_object_emit_symbol_t *symbol =
                &native_relocations.symbols[j];
            if (strlen(symbol->name) != name.length
                || memcmp(symbol->name, name.bytes, name.length)) continue;
            uint32_t expected_ordinal = 0;
            for (size_t k = 0; k < native_sections.count; k++)
                if (native_sections.maps[k].final_section_index
                    == row->section_index)
                    expected_ordinal = (uint32_t)(k + 2);
            assert(expected_ordinal && symbol->exported
                && symbol->section_index == expected_ordinal
                && symbol->section_offset == (uint64_t)row->offset);
            found = true;
        }
        assert(found);
    }
    n00bb_abi_merged_rows_release(&exports_merged);
    n00bb_abi_native_relocations_release(&native_relocations);
    size_t map_count = native_sections.count;
    n00bb_abi_native_section_map_t *maps = calloc(
        map_count + 1, sizeof(*maps));
    assert(maps);
    for (size_t i = 0; i < map_count; i++) {
        const n00bb_abi_native_section_map_t *map =
            &native_sections.maps[i];
        core_object_section_t source_section;
        uint32_t expected_index;
        assert(map->input == 0 && map->base_offset == 0
            && core_object_section_at(object, length, target.object_format,
                map->local_section_index, &source_section)
            && source_section.name_length ==
                strlen(native_sections.sections[i].name)
            && !memcmp(source_section.name,
                native_sections.sections[i].name,
                source_section.name_length)
            && source_section.size == native_sections.sections[i].size
            && (!source_section.size || !memcmp(source_section.bytes,
                native_sections.sections[i].bytes, source_section.size))
            && core_object_emit_native_section_index(&target,
                1 + map_count, (uint32_t)(2 + i), &expected_index)
            && map->final_section_index == expected_index);
        maps[i] = *map;
        maps[i].final_section_index += 17;
        maps[i].base_offset = 32;
    }
    const n00bb_abi_merged_table_t *native_tables[] = {
        storage, merged_bindings,
    };
    for (size_t ti = 0; ti < sizeof(native_tables) / sizeof(*native_tables);
         ti++) {
        const n00bb_abi_merged_table_t *table = native_tables[ti];
        for (uint32_t i = 1; i < table->record_count; i++) {
            const void *row = table->records + (size_t)i * table->record_bytes;
            uint32_t local_section = table->kind == N00B_ABI_TABLE_STORAGE
                ? ((const n00b_abi_storage_v1 *)row)->segment_index
                : ((const n00b_abi_native_binding_v1 *)row)->section_index;
            assert(local_section && table->origins[i].present);
            bool found = false;
            for (size_t j = 0; j < map_count; j++)
                if (maps[j].input == table->origins[i].input
                    && maps[j].local_section_index == local_section)
                    found = true;
            assert(found);
        }
    }
    assert(map_count && !n00bb_abi_rebase_native_references(
        &merged, nullptr, 0));
    const n00b_abi_storage_v1 *first_storage =
        n00bb_abi_merged_row(&merged, N00B_ABI_TABLE_STORAGE, 1);
    bool duplicated = false;
    for (size_t i = 0; i < map_count; i++) {
        if (maps[i].input != storage->origins[1].input
            || maps[i].local_section_index != first_storage->segment_index)
            continue;
        maps[map_count] = maps[i];
        duplicated = true;
        break;
    }
    assert(duplicated);
    assert(!n00bb_abi_rebase_native_references(
        &merged, maps, map_count + 1));
    assert(n00bb_abi_rebase_native_references(&merged, maps, map_count));
    for (size_t ti = 0; ti < sizeof(native_tables) / sizeof(*native_tables);
         ti++) {
        const n00bb_abi_merged_table_t *table = native_tables[ti];
        for (uint32_t i = 1; i < table->record_count; i++) {
            const n00bb_abi_row_origin_t *origin = &table->origins[i];
            const void *before = n00bb_abi_object_row(
                &selected.views[origin->input], table->kind, origin->local);
            const void *after = table->records
                + (size_t)i * table->record_bytes;
            assert(before);
            if (table->kind == N00B_ABI_TABLE_STORAGE) {
                const n00b_abi_storage_v1 *a = after, *b = before;
                uint32_t expected = 0;
                for (size_t j = 0; j < map_count; j++)
                    if (maps[j].input == origin->input
                        && maps[j].local_section_index == b->segment_index)
                        expected = maps[j].final_section_index;
                assert(expected && a->segment_index == expected
                    && a->byte_offset == b->byte_offset + 32);
            }
            else {
                const n00b_abi_native_binding_v1 *a = after, *b = before;
                uint32_t expected = 0;
                for (size_t j = 0; j < map_count; j++)
                    if (maps[j].input == origin->input
                        && maps[j].local_section_index == b->section_index)
                        expected = maps[j].final_section_index;
                assert(expected && a->section_index == expected
                    && a->offset == b->offset + 32);
            }
        }
    }
    free(maps);
    n00bb_abi_native_sections_release(&native_sections);
    n00bb_abi_merged_rows_release(&merged);
    n00b_abi_module_v1 remapped_module;
    n00b_abi_module_v1 *mutable_module = (void *)
        n00bb_abi_object_row(&selected.views[0], N00B_ABI_TABLE_MODULE, 1);
    uint32_t saved_package = mutable_module->package_index;
    mutable_module->package_index = UINT32_MAX;
    assert(!n00bb_abi_remap_row(&selected, plan, 0,
        N00B_ABI_TABLE_MODULE, 1, &remapped_module,
        sizeof(remapped_module), nullptr));
    mutable_module->package_index = saved_package;
    const n00b_abi_package_v1 *package = n00bb_abi_object_row(
        &selected.views[0], N00B_ABI_TABLE_PACKAGE, 1);
    uint8_t *module_slot = (void *)n00bb_abi_object_row(
        &selected.views[0], N00B_ABI_TABLE_U32_STREAM,
        package->modules.first);
    uint32_t saved_module = core_le_load_u32(module_slot);
    core_le_store_u32(module_slot, UINT32_MAX);
    uint32_t *stream_values = calloc(1, sizeof(uint32_t));
    uint32_t *stream_kinds = calloc(1, sizeof(uint32_t));
    assert(stream_values && stream_kinds);
    n00bb_abi_u32_stream_build_t stream = {
        .values = stream_values, .kinds = stream_kinds, .count = 1,
    };
    n00b_abi_package_v1 remapped_package;
    assert(!n00bb_abi_remap_row(&selected, plan, 0,
        N00B_ABI_TABLE_PACKAGE, 1, &remapped_package,
        sizeof(remapped_package), &stream));
    core_le_store_u32(module_slot, saved_module);
    free(stream_kinds);
    free(stream_values);
    n00bb_abi_index_plan_free(plan);
    n00bb_abi_selected_objects_release(&selected);
    core_link_unit_free(link);
    core_translation_unit_release(&unit);
    size_t metadata_offset = (size_t)(view.metadata.bytes - object);
    size_t checksum_byte = metadata_offset + view.header.total_extent - 1;
    n00bb_abi_object_close(&view);
    assert(!view.directory && !view.object);

    core_target_desc_t wrong_target;
    assert(core_target_profile(
        !strcmp(host_triple(), "arm64-macos") ? "x86_64-linux"
        : "arm64-macos", 0, &wrong_target));
    n00bb_abi_object_view_t unchanged = {.object = object};
    assert(!n00bb_abi_object_open(&wrong_target, object, length,
                                  &unchanged)
        && unchanged.object == object && !unchanged.directory);
    object[checksum_byte] ^= 1;
    assert(!n00bb_abi_object_open(&target, object, length,
                                  &unchanged)
        && unchanged.object == object && !unchanged.directory);
    object[checksum_byte] ^= 1;
    assert(n00bb_abi_object_open(&target, object, length,
                                  &view));
    n00bb_abi_object_close(&view);
    free(object);
    return 0;
}
