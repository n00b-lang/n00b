#include "lib/object_emit.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/byteorder.h"
#include "lib/object_section.h"

static void save_object(const char *path, const uint8_t *image, size_t size)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(image, 1, size, file) == size);
    assert(!fclose(file));
}

static void mach_test(const char *output_path)
{
    core_target_desc_t target;
    assert(core_target_profile("arm64-macos", 0, &target));
    uint8_t data[16] = {0}, mutable_data[8] = {0};
    core_object_emit_section_t sections[] = {
        {"__meta", "__N00B", data, sizeof data, 8, false, false},
        {"__data", "__DATA", mutable_data, sizeof mutable_data, 16, true, false},
    };
    core_object_emit_symbol_t symbols[] = {
        {"_external_symbol", 0, 0, true},
        {"_local_target", 2, 0, false},
        {"_defined_symbol", 2, 0, true},
    };
    core_object_emit_absolute64_t relocations[] = {
        {1, 8, 0, 5},
        {2, 0, 1, 0},
    };
    uint8_t *image = NULL;
    size_t size = 0;
    assert(core_object_emit(&target, sections, 2, symbols, 3, relocations, 2,
                            &image, &size));
    assert(core_le_load_u32(image) == 0xfeedfacf);
    core_object_section_t section;
    assert(core_object_section_find(image, size, CORE_TARGET_OBJECT_MACH_O,
                                    "__N00B", "__meta", &section));
    assert(section.native_index == 1 && section.size == 16 &&
           section.alignment == 8 && section.relocation_count == 1);
    core_object_absolute64_relocation_t relocation;
    assert(core_object_absolute64_relocation_at(
        &section, CORE_TARGET_OBJECT_MACH_O, 0, &relocation));
    assert(relocation.section_offset == 8 &&
           !relocation.target_is_section && relocation.addend_bits == 5);
    core_object_symbol_name_t name;
    assert(core_object_symbol_name_at(image, size,
                                      CORE_TARGET_OBJECT_MACH_O, &section,
                                      relocation.native_symbol_index, &name));
    assert(name.length == strlen("_external_symbol") &&
           !memcmp(name.bytes, "_external_symbol", name.length));
    assert(core_object_section_find(image, size, CORE_TARGET_OBJECT_MACH_O,
                                    "__DATA", "__data", &section));
    assert(section.native_index == 2 && section.alignment == 16);
    assert(core_object_absolute64_relocation_at(
        &section, CORE_TARGET_OBJECT_MACH_O, 0, &relocation));
    assert(relocation.target_is_section &&
           relocation.native_symbol_index == 2 &&
           relocation.addend_bits == 16);
    if (output_path) save_object(output_path, image, size);
    core_free(image);
}

static void coff_test(const char *output_path)
{
    core_target_desc_t target;
    assert(core_target_profile("x86_64-windows", 0, &target));
    uint8_t data[16] = {0}, code[8] = {0};
    core_object_emit_section_t sections[] = {
        {".n00b$M", NULL, data, sizeof data, 8, false, false},
        {".long_section_name", NULL, code, sizeof code, 16, false, true},
    };
    core_object_emit_symbol_t symbols[] = {
        {"external_symbol", 0, 0, true},
        {"local_target", 2, 0, false},
        {"start", 2, 0, true},
    };
    core_object_emit_absolute64_t relocations[] = {
        {1, 8, 0, 5},
        {2, 0, 1, 0},
    };
    uint8_t *image = NULL;
    size_t size = 0;
    assert(core_object_emit(&target, sections, 2, symbols, 3, relocations, 2,
                            &image, &size));
    assert(core_le_load_u16(image) == 0x8664);
    core_object_section_t section;
    assert(core_object_section_find(image, size, CORE_TARGET_OBJECT_COFF,
                                    NULL, ".n00b$M", &section));
    assert(section.native_index == 1 && section.size == 16 &&
           section.alignment == 8 && section.relocation_count == 1);
    assert(!(section.flags & 0x80000000u));
    core_object_absolute64_relocation_t relocation;
    assert(core_object_absolute64_relocation_at(
        &section, CORE_TARGET_OBJECT_COFF, 0, &relocation));
    assert(relocation.section_offset == 8 &&
           relocation.native_symbol_index == 0 &&
           relocation.addend_bits == 5);
    core_object_symbol_name_t name;
    assert(core_object_symbol_name_at(image, size, CORE_TARGET_OBJECT_COFF,
                                      &section, relocation.native_symbol_index,
                                      &name));
    assert(name.length == strlen("external_symbol") &&
           !memcmp(name.bytes, "external_symbol", name.length));
    assert(core_object_section_find(image, size, CORE_TARGET_OBJECT_COFF,
                                    NULL, ".long_section_name", &section));
    assert(section.native_index == 2 && section.alignment == 16 &&
           section.relocation_count == 1);
    assert(core_object_absolute64_relocation_at(
        &section, CORE_TARGET_OBJECT_COFF, 0, &relocation));
    assert(relocation.native_symbol_index == 1);
    assert(core_object_symbol_name_at(image, size, CORE_TARGET_OBJECT_COFF,
                                      &section, relocation.native_symbol_index,
                                      &name));
    assert(name.length == strlen("local_target") &&
           !memcmp(name.bytes, "local_target", name.length));
    uint32_t symbol_table_offset = core_le_load_u32(image + 8);
    core_le_store_u32(image + 8, 0);
    assert(!core_object_section_find(image, size, CORE_TARGET_OBJECT_COFF,
                                     NULL, ".long_section_name", &section));
    core_le_store_u32(image + 8, symbol_table_offset);
    if (output_path) save_object(output_path, image, size);
    core_free(image);
}

static void coff_relocation_overflow_test(const char *output_path)
{
    core_target_desc_t target;
    assert(core_target_profile("x86_64-windows", 0, &target));
    const size_t count = UINT16_MAX;
    uint8_t *data = core_alloc_size(count, 8);
    core_object_emit_absolute64_t *relocations =
        core_alloc_array(core_object_emit_absolute64_t, count);
    for (size_t i = 0; i < count; i++) {
        relocations[i] = (core_object_emit_absolute64_t){
            .section_index = 1,
            .section_offset = i * 8,
            .symbol_index = 0,
        };
    }
    core_object_emit_section_t section_input = {
        ".n00b$M", NULL, data, count * 8, 8, false, false,
    };
    core_object_emit_symbol_t symbol = {"external_symbol", 0, 0, true};
    uint8_t *image = NULL;
    size_t size = 0;
    assert(core_object_emit(&target, &section_input, 1, &symbol, 1,
                            relocations, count, &image, &size));
    core_object_section_t section;
    assert(core_object_section_find(image, size, CORE_TARGET_OBJECT_COFF,
                                    NULL, ".n00b$M", &section));
    assert(section.relocation_count == count);
    assert(section.flags & 0x01000000);
    core_object_absolute64_relocation_t relocation;
    assert(core_object_absolute64_relocation_at(
        &section, CORE_TARGET_OBJECT_COFF, count - 1, &relocation));
    assert(relocation.section_offset == (count - 1) * 8);
    uint32_t overflow_offset = core_le_load_u32(image + 20 + 24);
    core_le_store_u32(image + overflow_offset + 4, 1);
    assert(!core_object_section_find(image, size, CORE_TARGET_OBJECT_COFF,
                                     NULL, ".n00b$M", &section));
    core_le_store_u32(image + overflow_offset + 4, 0);
    if (output_path) save_object(output_path, image, size);
    core_free(image);
    core_free(relocations);
    core_free(data);
}

int main(int argc, char **argv)
{
    core_target_desc_t target;
    assert(core_target_profile("x86_64-linux", 0, &target));
    uint8_t metadata[16] = {0};
    uint8_t code[8] = {0};
    core_object_emit_section_t sections[] = {
        {".n00b.meta", NULL, metadata, sizeof metadata, 8, false, false},
        {".text", NULL, code, sizeof code, 16, false, true},
    };
    core_object_emit_symbol_t symbols[] = {
        {"external_symbol", 0, 0, true},
        {"local_target", 2, 0, false},
        {"start", 2, 0, true},
    };
    core_object_emit_absolute64_t relocations[] = {
        {1, 8, 0, 5},
        {2, 0, 1, 0},
    };
    uint8_t *image = NULL;
    size_t size = 0;
    assert(core_object_emit(&target, sections, 2, symbols, 3, relocations, 2,
                            &image, &size));
    assert(image && size > 64 && core_le_load_u64(image + 40) < size);
    assert(core_le_load_u16(image + 18) == 62);
    core_object_section_t section;
    assert(core_object_section_find(image, size, CORE_TARGET_OBJECT_ELF,
                                    NULL, ".n00b.meta", &section));
    assert(section.native_index == 4 && section.size == 16 &&
           section.alignment == 8 && section.flags == 2);
    core_object_absolute64_relocation_t relocation;
    assert(core_object_absolute64_relocation_at(
        &section, CORE_TARGET_OBJECT_ELF, 0, &relocation));
    assert(relocation.section_offset == 8 &&
           relocation.native_symbol_index == 2 &&
           relocation.addend_bits == 5);
    core_object_symbol_name_t name;
    assert(core_object_symbol_name_at(image, size, CORE_TARGET_OBJECT_ELF,
                                      &section, relocation.native_symbol_index,
                                      &name));
    assert(name.length == strlen("external_symbol") &&
           !memcmp(name.bytes, "external_symbol", name.length));
    assert(!core_object_symbol_name_at(image, size, CORE_TARGET_OBJECT_ELF,
                                       &section, UINT32_MAX, &name));
    assert(core_object_section_find(image, size, CORE_TARGET_OBJECT_ELF,
                                    NULL, ".text", &section));
    assert(section.native_index == 5 && section.alignment == 16 &&
           section.flags == 6);
    assert(core_object_absolute64_relocation_at(
        &section, CORE_TARGET_OBJECT_ELF, 0, &relocation));
    assert(relocation.native_symbol_index == 1);
    assert(core_object_symbol_name_at(image, size, CORE_TARGET_OBJECT_ELF,
                                      &section, relocation.native_symbol_index,
                                      &name));
    assert(name.length == strlen("local_target") &&
           !memcmp(name.bytes, "local_target", name.length));

    relocations[1].section_index = 1;
    relocations[1].section_offset = 8;
    uint8_t *unchanged = image;
    size_t unchanged_size = size;
    assert(!core_object_emit(&target, sections, 2, symbols, 3, relocations, 2,
                             &unchanged, &unchanged_size));
    assert(unchanged == image && unchanged_size == size);

    if (argc >= 2) save_object(argv[1], image, size);
    core_free(image);
    mach_test(argc >= 3 ? argv[2] : NULL);
    coff_test(argc >= 4 ? argv[3] : NULL);
    coff_relocation_overflow_test(argc >= 5 ? argv[4] : NULL);
    return 0;
}
