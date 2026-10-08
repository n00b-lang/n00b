#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi_directory.h"
#include "n00b-abi-v1.h"
#include "lib/object_section.h"
#include "lib/target.h"

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
read_object(const char *path, size_t *length_out)
{
    FILE *file = fopen(path, "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    long length = ftell(file);
    assert(length > 0 && !fseek(file, 0, SEEK_SET));
    uint8_t *bytes = malloc((size_t)length);
    assert(bytes && fread(bytes, 1, (size_t)length, file)
        == (size_t)length && !fclose(file));
    *length_out = (size_t)length;
    return bytes;
}

int
main(int argc, char **argv)
{
    assert(argc == 3 || argc == 4 || argc == 5);
    size_t object_bytes = 0;
    uint8_t *object = read_object(argv[1], &object_bytes);
    core_target_desc_t target;
    assert(core_target_profile(host_triple(), 0, &target));
    uint32_t section_count = 0;
    assert(core_object_section_count(object, object_bytes,
        target.object_format, &section_count));
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

    const char *segment = nullptr;
    const char *name;
    switch (target.object_format) {
    case CORE_TARGET_OBJECT_ELF: name = ".data.rel.ro.n00b_meta"; break;
    case CORE_TARGET_OBJECT_MACH_O:
        segment = "__DATA_CONST";
        name = "__n00b_meta";
        break;
    case CORE_TARGET_OBJECT_COFF: name = ".n00b$M"; break;
    default: assert(false);
    }
    core_object_section_t metadata;
    assert(core_object_section_find(object, object_bytes,
        target.object_format, segment, name, &metadata));
    n00b_abi_image_descriptor_v1 image = {
        .descriptor_version = 1,
        .metadata = (const n00b_abi_header_v1 *)loaded[metadata.native_index],
        .metadata_bytes = metadata.size,
    };
    bool bad_argument = argc == 4
        && !strcmp(argv[3], "--bad-named-argument");
    bool missing_constructor = argc == 4
        && !strcmp(argv[3], "--missing-named-constructor");
    if (bad_argument || missing_constructor) {
        const uint8_t *base = (const uint8_t *)image.metadata;
        const n00b_abi_header_v1 *header = image.metadata;
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
        }
        assert(changed);
        n00b_abi_error_v1 bad_error = {0};
        uint64_t bad_generation = 0;
        assert(n00b_abi_v1_register_image(&image, &bad_generation,
            &bad_error) == N00B_ABI_BAD_METADATA);
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
    assert(module_count == (uint32_t)strtoul(argv[2], nullptr, 10) + 1);
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
    if (argc == 5) {
        uint32_t matches = 0;
        uint32_t expected_args = (uint32_t)strtoul(argv[4], nullptr, 10);
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
            if (length != strlen(argv[3])
                || memcmp(name, argv[3], (size_t)length)) continue;
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
            if (length != strlen(argv[3])
                || memcmp(name, argv[3], (size_t)length)) continue;
            assert(type->layout_index);
            matches++;
        }
        assert(matches == 1);
    }
    /* The published snapshot borrows the relocated sections until exit. */
    free(object);
    return 0;
}
