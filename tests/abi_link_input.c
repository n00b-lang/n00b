#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi_link.h"
#include "abi_string.h"
#include "n00b-abi-v1.h"

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

int
main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    long length = ftell(file);
    assert(length > 0 && !fseek(file, 0, SEEK_SET));
    uint8_t *object = malloc((size_t)length);
    assert(object && fread(object, 1, (size_t)length, file)
        == (size_t)length && !fclose(file));
    core_target_desc_t target;
    assert(core_target_profile(host_triple(), 0, &target));
    n00bb_abi_object_view_t view = {0};
    assert(n00bb_abi_object_open(&target, object, (size_t)length, &view));
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
    n00bb_abi_string_view_t unchanged_name = {.bytes = object, .length = 1};
    assert(!n00bb_abi_object_string(&view,
        (const n00b_abi_string_v1 *)object, &unchanged_name)
        && unchanged_name.bytes == object && unchanged_name.length == 1);
    size_t metadata_offset = (size_t)(view.metadata.bytes - object);
    size_t checksum_byte = metadata_offset + view.header.total_extent - 1;
    n00bb_abi_object_close(&view);
    assert(!view.directory && !view.object);

    core_target_desc_t wrong_target;
    assert(core_target_profile(
        !strcmp(host_triple(), "arm64-macos") ? "x86_64-linux"
        : "arm64-macos", 0, &wrong_target));
    n00bb_abi_object_view_t unchanged = {.object = object};
    assert(!n00bb_abi_object_open(&wrong_target, object, (size_t)length,
                                  &unchanged)
        && unchanged.object == object && !unchanged.directory);
    object[checksum_byte] ^= 1;
    assert(!n00bb_abi_object_open(&target, object, (size_t)length,
                                  &unchanged)
        && unchanged.object == object && !unchanged.directory);
    object[checksum_byte] ^= 1;
    assert(n00bb_abi_object_open(&target, object, (size_t)length,
                                  &view));
    n00bb_abi_object_close(&view);
    free(object);
    return 0;
}
