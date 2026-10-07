#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi_link.h"
#include "abi_string.h"
#include "n00b-abi-v1.h"
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
        }
    }
    assert(shared_primitive);
    check_remapped_rows(&selected, plan);
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
    n00bb_abi_index_plan_free(plan);
    n00bb_abi_selected_objects_release(&selected);
    core_link_unit_free(link);
    core_translation_unit_release(&unit);
    free(object);
}

int
main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3 || argc == 4);
    if (argc == 4) {
        test_cycle_selection(argv + 1);
        return 0;
    }
    if (argc == 3) {
        assert(!strcmp(argv[1], "--self"));
        test_self_import_selection(argv[2]);
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
