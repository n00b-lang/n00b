#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi_directory.h"
#include "n00b-abi-v1.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/object_section.h"
#include "lib/target.h"

static core_string_t
host_triple(void)
{
#if defined(__APPLE__) && defined(__aarch64__)
    return CORE_STRING_STATIC("arm64-macos");
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return CORE_STRING_STATIC("x86_64-windows");
#else
    return CORE_STRING_STATIC("x86_64-linux");
#endif
}

static uint8_t *
read_object(core_string_t path, size_t *length_out)
{
    char *c_path = core_string_to_cstr(path);
    FILE *file = fopen(c_path, "rb");
    core_free(c_path);
    assert(file && !fseek(file, 0, SEEK_END));
    long length = ftell(file);
    assert(length > 0 && !fseek(file, 0, SEEK_SET));
    uint8_t *bytes = malloc((size_t)length);
    assert(bytes && fread(bytes, 1, (size_t)length, file)
        == (size_t)length && !fclose(file));
    *length_out = (size_t)length;
    return bytes;
}

static bool
string_is(const n00b_abi_snapshot_v1 *snapshot,
          const n00b_abi_string_v1 *value, core_string_t text)
{
    const uint8_t *bytes = nullptr;
    uint64_t size = 0;
    return n00b_abi_v1_string_bytes(snapshot, value, &bytes, &size) == 1
        && core_string_eq(core_string_borrow((const char *)bytes,
                                             (size_t)size), text);
}

static void
check_ffi_module(const n00b_abi_snapshot_v1 *snapshot, core_string_t path,
                 const core_string_t *names, int name_count)
{
    uint32_t module_index = 0;
    uint32_t module_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_MODULE);
    for (uint32_t i = 1; i < module_count; i++)
        if (string_is(snapshot, &n00b_abi_v1_module(snapshot, i)->full_path,
                      path)) module_index = i;
    assert(module_index);
    bool has_module_scope = false;
    uint32_t scope_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_SCOPE);
    for (uint32_t i = 1; i < scope_count; i++) {
        const n00b_abi_scope_v1 *scope = n00b_abi_v1_scope(snapshot, i);
        has_module_scope = has_module_scope
            || (scope->module_index == module_index
                && string_is(snapshot, &scope->kind_name,
                             CORE_STRING_STATIC("module")));
    }
    assert(has_module_scope);
    uint32_t symbol_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_SYMBOL);
    for (int n = 0; n < name_count; n++) {
        core_string_t spec = names[n];
        int64_t equals = core_string_find_byte(spec, '=');
        int64_t at = core_string_find_byte(spec, '@');
        core_string_t symbol_name = core_string_slice(spec, 0,
            equals >= 0 ? (size_t)equals
                : at >= 0 ? (size_t)at : spec.u8_bytes);
        const n00b_abi_symbol_v1 *found = nullptr;
        for (uint32_t i = 1; i < symbol_count; i++) {
            const n00b_abi_symbol_v1 *symbol = n00b_abi_v1_symbol(snapshot, i);
            const n00b_abi_scope_v1 *scope = n00b_abi_v1_scope(snapshot,
                symbol->scope_index);
            if (symbol->module_index == module_index && scope
                && string_is(snapshot, &scope->kind_name,
                             CORE_STRING_STATIC("module"))
                && string_is(snapshot, &symbol->name, symbol_name))
                found = symbol;
        }
        if (!found) fprintf(stderr, "no symbol row for %.*s\n",
                            (int)symbol_name.u8_bytes, symbol_name.data);
        assert(found);
        if (at >= 0) {
            const n00b_abi_source_location_v1 *location =
                n00b_abi_v1_source_location(snapshot,
                    found->declaration_location_index);
            const n00b_abi_source_file_v1 *file = location
                ? n00b_abi_v1_record(snapshot, N00B_ABI_TABLE_SOURCE_FILE,
                    location->file_index, nullptr) : nullptr;
            core_buffer_t generated = {0};
            core_buffer_puts(&generated, "<generated:");
            core_buffer_append_string(&generated, path);
            core_buffer_putc(&generated, '>');
            core_string_t line = core_string_slice(spec, (size_t)at + 1,
                spec.u8_bytes - (size_t)at - 1);
            assert(location && file
                && location->start_line == core_string_strtoull(line, 10)
                && string_is(snapshot, &file->path,
                             core_buffer_string(&generated)));
            core_buffer_release(&generated);
        }
        if (equals < 0) continue;
        const n00b_abi_native_binding_v1 *binding = n00b_abi_v1_record(
            snapshot, N00B_ABI_TABLE_NATIVE_BINDING,
            found->native_binding_index, nullptr);
        core_buffer_t expected = {0};
        if (core_string_eq(host_triple(), CORE_STRING_STATIC("arm64-macos")))
            core_buffer_putc(&expected, '_');
        core_buffer_append_string(&expected, core_string_slice(spec,
            (size_t)equals + 1, spec.u8_bytes - (size_t)equals - 1));
        assert(found->native_binding_index && binding
            && binding->kind == N00B_ABI_NATIVE_EXTERNAL_NAME
            && string_is(snapshot, &binding->native_name,
                         core_buffer_string(&expected)));
        core_buffer_release(&expected);
    }
}

int
main(int argc, char **argv)
{
    core_string_t *args = calloc((size_t)argc, sizeof(*args));
    assert(args);
    for (int i = 0; i < argc; i++) args[i] = core_string_from_cstr(argv[i]);
    bool ffi_module = argc >= 5
        && core_string_eq(args[3], CORE_STRING_STATIC("--ffi-module"));
    assert(argc == 3 || argc == 4 || argc == 5 || ffi_module);
    size_t object_bytes = 0;
    uint8_t *object = read_object(args[1], &object_bytes);
    core_target_desc_t target;
    assert(core_target_profile(host_triple(), 0, &target));
    uint32_t section_count = 0;
    assert(core_object_section_count(object, object_bytes,
        target.object_format, &section_count));
    core_string_t segment = {};
    core_string_t name;
    switch (target.object_format) {
    case CORE_TARGET_OBJECT_ELF:
        name = CORE_STRING_STATIC(".data.rel.ro.n00b_meta");
        break;
    case CORE_TARGET_OBJECT_MACH_O:
        segment = CORE_STRING_STATIC("__DATA_CONST");
        name = CORE_STRING_STATIC("__n00b_meta");
        break;
    case CORE_TARGET_OBJECT_COFF: name = CORE_STRING_STATIC(".n00b$M"); break;
    default: assert(false);
    }
    core_object_section_t metadata;
    assert(core_object_section_find(object, object_bytes,
        target.object_format, segment, name, &metadata));
    core_object_section_t *sections = calloc((size_t)section_count + 1,
        sizeof(*sections));
    uint8_t **loaded = calloc((size_t)section_count + 1, sizeof(*loaded));
    assert(sections && loaded);

    for (uint32_t i = 1; i <= section_count; i++) {
        if (!core_object_section_at(object, object_bytes,
                target.object_format, i, &sections[i])) continue;
        size_t bytes = sections[i].size;
        assert(bytes && bytes <= SIZE_MAX - 63);
        void *allocation = malloc(bytes + 63);
        assert(allocation);
        loaded[i] = (uint8_t *)(((uintptr_t)allocation + 63)
            & ~(uintptr_t)63);
        memcpy(loaded[i], sections[i].bytes, bytes);
    }
    for (uint32_t i = 1; i <= section_count; i++) {
        if (!loaded[i]) continue;
        const core_object_section_t *section = &sections[i];
        for (uint32_t j = 0; j < section->relocation_count; j++) {
            core_object_absolute64_relocation_t relocation;
            assert(core_object_absolute64_relocation_at(section,
                target.object_format, j, &relocation));
            uint32_t target_index;
            uint64_t offset;
            if (relocation.target_is_section) {
                target_index = relocation.native_symbol_index;
                assert(target_index && target_index <= section_count);
                offset = relocation.addend_bits
                    - sections[target_index].virtual_address;
            }
            else {
                core_object_symbol_t symbol;
                assert(core_object_symbol_at(object, object_bytes,
                    target.object_format, section,
                    relocation.native_symbol_index, &symbol));
                target_index = symbol.native_section_index;
                offset = symbol.section_offset + relocation.addend_bits;
            }
            /* Runtime relocations may name external symbols that are resolved
               when the library is linked into an executable. */
            if (!target_index && i != metadata.native_index) continue;
            assert(target_index && target_index <= section_count
                && loaded[target_index]
                && offset <= sections[target_index].size
                && relocation.section_offset <= section->size
                && sizeof(uintptr_t) <= section->size
                    - relocation.section_offset);
            uintptr_t address = (uintptr_t)(loaded[target_index] + offset);
            memcpy(loaded[i] + relocation.section_offset,
                   &address, sizeof(address));
        }
    }

    n00b_abi_image_descriptor_v1 image = {
        .descriptor_version = 1,
        .metadata = (const n00b_abi_header_v1 *)loaded[metadata.native_index],
        .metadata_bytes = metadata.size,
    };
    if (ffi_module) {
        n00b_abi_error_v1 error = {0};
        uint64_t generation = 0;
        assert(n00b_abi_v1_register_image(&image, &generation, &error)
            == N00B_ABI_OK);
        check_ffi_module(n00b_abi_v1_current_snapshot(), args[4],
                         args + 5, argc - 5);
        free(object);
        return 0;
    }
    bool bad_argument = argc == 4
        && core_string_eq(args[3], CORE_STRING_STATIC("--bad-named-argument"));
    bool missing_constructor = argc == 4
        && core_string_eq(args[3],
                          CORE_STRING_STATIC("--missing-named-constructor"));
    bool bad_shape = argc == 4
        && core_string_eq(args[3], CORE_STRING_STATIC("--bad-shape-total"));
    bool non_array_shape = argc == 4
        && core_string_eq(args[3], CORE_STRING_STATIC("--shape-on-non-array"));
    bool bad_field_visibility = argc == 4
        && core_string_eq(args[3],
                          CORE_STRING_STATIC("--bad-field-visibility"));
    bool bad_export_visibility = argc == 4
        && core_string_eq(args[3],
                          CORE_STRING_STATIC("--bad-export-visibility"));
    bool bad_storage_binding = argc == 4
        && core_string_eq(args[3],
                          CORE_STRING_STATIC("--bad-storage-binding"));
    if (bad_argument || missing_constructor || bad_shape
        || non_array_shape || bad_field_visibility
        || bad_export_visibility || bad_storage_binding) {
        const uint8_t *base = (const uint8_t *)image.metadata;
        const n00b_abi_header_v1 *header = image.metadata;
        n00bb_abi_directory_entry_t symbols = {0};
        n00bb_abi_directory_entry_t bindings = {0};
        if (non_array_shape || bad_storage_binding)
            for (uint32_t i = 0; i < header->directory_count; i++) {
                n00bb_abi_directory_entry_t entry;
                n00bb_abi_directory_decode_entry(base
                    + header->directory_offset
                    + (size_t)i * N00BB_ABI_DIRECTORY_BYTES, &entry);
                if (entry.kind == N00B_ABI_TABLE_SYMBOL) symbols = entry;
                if (entry.kind == N00B_ABI_TABLE_NATIVE_BINDING)
                    bindings = entry;
            }
        bool changed = false;
        for (uint32_t i = 0; i < header->directory_count; i++) {
            n00bb_abi_directory_entry_t entry;
            n00bb_abi_directory_decode_entry(base + header->directory_offset
                + (size_t)i * N00BB_ABI_DIRECTORY_BYTES, &entry);
            if (bad_argument && entry.kind == N00B_ABI_TABLE_TYPE_ARGUMENT) {
                assert(entry.record_count > 1 && entry.record_bytes
                    == sizeof(n00b_abi_type_argument_v1));
                n00b_abi_type_argument_v1 *arg = (void *)(
                    loaded[metadata.native_index] + entry.file_offset
                    + entry.record_bytes);
                arg->ordinal++;
                changed = true;
                break;
            }
            if (missing_constructor && entry.kind == N00B_ABI_TABLE_TYPE) {
                assert(entry.record_bytes == sizeof(n00b_abi_type_v1));
                for (uint32_t j = 0; j < entry.record_count; j++) {
                    n00b_abi_type_v1 *type = (void *)(
                        loaded[metadata.native_index] + entry.file_offset
                        + (size_t)j * entry.record_bytes);
                    if (!type->named_arguments.count) continue;
                    memset(&type->named_constructor, 0,
                           sizeof(type->named_constructor));
                    changed = true;
                    break;
                }
                break;
            }
            if (bad_shape && entry.kind == N00B_ABI_TABLE_SHAPE) {
                assert(entry.record_count > 1 && entry.record_bytes
                    == sizeof(n00b_abi_shape_v1));
                n00b_abi_shape_v1 *shape = (void *)(
                    loaded[metadata.native_index] + entry.file_offset
                    + entry.record_bytes);
                shape->total_count++;
                changed = true;
                break;
            }
            if (bad_field_visibility && entry.kind == N00B_ABI_TABLE_FIELD) {
                assert(entry.record_bytes == sizeof(n00b_abi_field_v1));
                for (uint32_t j = 1; j < entry.record_count; j++) {
                    n00b_abi_field_v1 *field = (void *)(
                        loaded[metadata.native_index] + entry.file_offset
                        + (size_t)j * entry.record_bytes);
                    if (!field->symbol_index) continue;
                    field->visibility = field->visibility
                        == N00B_ABI_VISIBILITY_PUBLIC
                            ? N00B_ABI_VISIBILITY_PRIVATE
                            : N00B_ABI_VISIBILITY_PUBLIC;
                    changed = true;
                    break;
                }
                break;
            }
            if (bad_export_visibility && entry.kind == N00B_ABI_TABLE_SYMBOL) {
                assert(entry.record_bytes == sizeof(n00b_abi_symbol_v1));
                for (uint32_t j = 1; j < entry.record_count; j++) {
                    n00b_abi_symbol_v1 *symbol = (void *)(
                        loaded[metadata.native_index] + entry.file_offset
                        + (size_t)j * entry.record_bytes);
                    if (!(symbol->flags & N00B_ABI_SYMBOL_FLAG_EXPORTED))
                        continue;
                    symbol->visibility = N00B_ABI_VISIBILITY_PRIVATE;
                    changed = true;
                    break;
                }
                break;
            }
            if (non_array_shape && entry.kind == N00B_ABI_TABLE_STORAGE) {
                assert(entry.record_bytes == sizeof(n00b_abi_storage_v1)
                    && symbols.record_bytes == sizeof(n00b_abi_symbol_v1));
                for (uint32_t j = 1; j < entry.record_count; j++) {
                    n00b_abi_storage_v1 *storage = (void *)(
                        loaded[metadata.native_index] + entry.file_offset
                        + (size_t)j * entry.record_bytes);
                    if (!storage->symbol_index
                        || !storage->instance_shape_index) continue;
                    assert(storage->symbol_index < symbols.record_count);
                    n00b_abi_symbol_v1 *symbol = (void *)(
                        loaded[metadata.native_index] + symbols.file_offset
                        + (size_t)storage->symbol_index
                            * symbols.record_bytes);
                    storage->type_index = 0;
                    symbol->type_index = 0;
                    changed = true;
                    break;
                }
                break;
            }
            if (bad_storage_binding
                && entry.kind == N00B_ABI_TABLE_STORAGE) {
                assert(entry.record_bytes == sizeof(n00b_abi_storage_v1)
                    && symbols.record_bytes == sizeof(n00b_abi_symbol_v1)
                    && bindings.record_bytes
                        == sizeof(n00b_abi_native_binding_v1));
                for (uint32_t j = 1; j < entry.record_count; j++) {
                    n00b_abi_storage_v1 *storage = (void *)(
                        loaded[metadata.native_index] + entry.file_offset
                        + (size_t)j * entry.record_bytes);
                    if (!storage->symbol_index) continue;
                    assert(storage->symbol_index < symbols.record_count);
                    const n00b_abi_symbol_v1 *symbol = (const void *)(
                        loaded[metadata.native_index] + symbols.file_offset
                        + (size_t)storage->symbol_index
                            * symbols.record_bytes);
                    if (!symbol->native_binding_index) continue;
                    assert(symbol->native_binding_index
                        < bindings.record_count);
                    n00b_abi_native_binding_v1 *binding = (void *)(
                        loaded[metadata.native_index] + bindings.file_offset
                        + (size_t)symbol->native_binding_index
                            * bindings.record_bytes);
                    assert(binding->offset < INT64_MAX);
                    binding->offset++;
                    changed = true;
                    break;
                }
                break;
            }
        }
        assert(changed);
        n00b_abi_error_v1 bad_error = {0};
        uint64_t bad_generation = 0;
        assert(n00b_abi_v1_register_image(&image, &bad_generation,
            &bad_error) == N00B_ABI_BAD_METADATA);
        if (bad_shape) assert(bad_error.table_kind == N00B_ABI_TABLE_SHAPE);
        if (non_array_shape)
            assert(bad_error.table_kind == N00B_ABI_TABLE_STORAGE);
        if (bad_field_visibility)
            assert(bad_error.table_kind == N00B_ABI_TABLE_TYPE);
        if (bad_export_visibility)
            assert(bad_error.table_kind == N00B_ABI_TABLE_SYMBOL);
        if (bad_storage_binding)
            assert(bad_error.table_kind == N00B_ABI_TABLE_STORAGE);
        assert(!n00b_abi_v1_current_snapshot());
        free(object);
        return 0;
    }
    n00b_abi_error_v1 error = {0};
    uint64_t generation = 0;
    uint32_t status = n00b_abi_v1_register_image(&image,
        &generation, &error);
    if (status != N00B_ABI_OK)
        fprintf(stderr, "real ABI object registration failed: status=%u table=%u row=%u\n",
            status, error.table_kind, error.record_index);
    assert(status == N00B_ABI_OK && generation == 1);
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    assert(snapshot && n00b_abi_v1_generation(snapshot) == generation);
    uint32_t module_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_MODULE);
    bool minimum_modules = core_string_starts_with(args[2],
                                                   CORE_STRING_STATIC(">="));
    size_t count_start = minimum_modules ? 2 : 0;
    uint32_t expected_modules = (uint32_t)core_string_strtoull(
        core_string_slice(args[2], count_start,
                          args[2].u8_bytes - count_start), 10) + 1;
    assert(minimum_modules ? module_count >= expected_modules
                           : module_count == expected_modules);
    for (uint32_t i = 1; i < module_count; i++) {
        const n00b_abi_module_v1 *module = n00b_abi_v1_module(snapshot, i);
        const uint8_t *path = nullptr;
        uint64_t length = 0;
        uint32_t found = 0;
        assert(module && n00b_abi_v1_string_bytes(snapshot,
            &module->full_path, &path, &length) == 1
            && path && length
            && n00b_abi_v1_find_module(snapshot, module->full_path,
                &found) == 1 && found == i);
    }
    uint32_t dispatch_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_DISPATCH);
    for (uint32_t d = 1; d < dispatch_count; d++) {
        const n00b_abi_dispatch_v1 *dispatch = n00b_abi_v1_record(snapshot,
            N00B_ABI_TABLE_DISPATCH, d, nullptr);
        assert(dispatch && dispatch->entries.count);
        for (uint32_t i = 0; i < dispatch->entries.count; i++) {
            uint32_t entry_index = 0, function_index = 0;
            assert(n00b_abi_v1_index_at(snapshot, dispatch->entries, i,
                &entry_index) == 1);
            const n00b_abi_dispatch_entry_v1 *entry = n00b_abi_v1_record(
                snapshot, N00B_ABI_TABLE_DISPATCH_ENTRY,
                entry_index, nullptr);
            assert(entry && n00b_abi_v1_type_specializes(snapshot,
                entry->receiver_type_index, dispatch->trait_type_index) == 1);
            assert(n00b_abi_v1_resolve_trait_method(snapshot,
                dispatch->trait_type_index, dispatch->method_ordinal,
                entry->receiver_type_index, &function_index) == 1
                && function_index == entry->function_index);
        }
    }
    if (argc == 5) {
        uint32_t matches = 0;
        uint32_t expected_args = (uint32_t)core_string_strtoull(args[4], 10);
        uint32_t type_count = n00b_abi_v1_table_count(snapshot,
            N00B_ABI_TABLE_TYPE);
        for (uint32_t i = 0; i < type_count; i++) {
            const n00b_abi_type_v1 *type = n00b_abi_v1_type(snapshot, i);
            const uint8_t *name = nullptr;
            uint64_t length = 0;
            n00b_abi_string_v1 empty = {0};
            if (!type || !memcmp(&type->named_constructor, &empty,
                                 sizeof(empty))) continue;
            assert(type && n00b_abi_v1_string_bytes(snapshot,
                &type->named_constructor, &name, &length) == 1);
            if (!core_string_eq(core_string_borrow((const char *)name,
                                                   (size_t)length), args[3]))
                continue;
            assert(type->named_arguments.count == expected_args);
            for (uint32_t j = 0; j < expected_args; j++) {
                const n00b_abi_type_argument_v1 *arg =
                    n00b_abi_v1_type_argument(snapshot,
                        type->named_arguments.first + j);
                assert(arg && arg->owner_type_index == i && arg->ordinal == j);
            }
            matches++;
        }
        assert(matches);
    }
    if (argc == 4) {
        uint32_t matches = 0;
        uint32_t type_count = n00b_abi_v1_table_count(snapshot,
            N00B_ABI_TABLE_TYPE);
        for (uint32_t i = 0; i < type_count; i++) {
            const n00b_abi_type_v1 *type = n00b_abi_v1_type(snapshot, i);
            const uint8_t *name = nullptr;
            uint64_t length = 0;
            assert(type && n00b_abi_v1_string_bytes(snapshot,
                &type->canonical_name, &name, &length) == 1);
            if (!core_string_eq(core_string_borrow((const char *)name,
                                                   (size_t)length), args[3]))
                continue;
            assert(type->layout_index);
            matches++;
        }
        assert(matches == 1);
    }
    /* The published snapshot borrows the relocated sections until exit. */
    free(object);
    return 0;
}
