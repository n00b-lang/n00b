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

static void check_native_section(const uint8_t *image, size_t size,
                                 core_target_object_format_t format,
                                 uint32_t index, const char *name,
                                 const char *segment)
{
    uint32_t count = 0;
    core_object_section_t view = {0};
    assert(core_object_section_count(image, size, format, &count)
        && index <= count
        && core_object_section_at(image, size, format, index, &view)
        && view.native_index == index
        && view.name_length == strlen(name)
        && !memcmp(view.name, name, view.name_length));
    if (segment)
        assert(view.segment_length == strlen(segment)
            && !memcmp(view.segment, segment, view.segment_length));
    assert(!core_object_section_at(image, size, format, 0, &view)
        && view.native_index == index
        && !core_object_section_at(image, size, format, count + 1, &view)
        && view.native_index == index);
}

static void check_native_symbols(const uint8_t *image, size_t size,
                                 const core_target_desc_t *target,
                                 const core_object_section_t *section,
                                 size_t section_count,
                                 const core_object_emit_symbol_t *expected,
                                 size_t expected_count)
{
    uint32_t first = target->object_format == CORE_TARGET_OBJECT_ELF ? 1 : 0;
    for (size_t i = 0; i < expected_count; i++) {
        bool found = false;
        for (size_t native = first; native < first + expected_count;
             native++) {
            core_object_symbol_t symbol;
            assert(core_object_symbol_at(image, size,
                target->object_format, section, (uint32_t)native, &symbol));
            size_t length = strlen(expected[i].name);
            if (symbol.name.length != length
                || memcmp(symbol.name.bytes, expected[i].name, length))
                continue;
            uint32_t expected_section = 0;
            if (expected[i].section_index)
                assert(core_object_emit_native_section_index(target,
                    section_count, expected[i].section_index,
                    &expected_section));
            assert(!found && symbol.exported == expected[i].exported
                && symbol.native_section_index == expected_section
                && symbol.section_offset == expected[i].section_offset);
            found = true;
        }
        assert(found);
    }
}

static void mach_test(const char *output_path)
{
    core_target_desc_t target;
    assert(core_target_profile("arm64-macos", 0, &target));
    uint8_t data[16] = {0}, mutable_data[8] = {0};
    core_object_emit_section_t sections[] = {
        {"__n00b_meta", "__DATA_CONST", data, sizeof data, 8, false, false, false},
        {"__data", "__DATA", mutable_data, sizeof mutable_data, 16, true, false, false},
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
                                    "__DATA_CONST", "__n00b_meta", &section));
    uint32_t native_index = 0;
    assert(core_object_emit_native_section_index(&target, 2, 1,
                                                  &native_index));
    assert(native_index == section.native_index);
    check_native_section(image, size, CORE_TARGET_OBJECT_MACH_O,
                         native_index, "__n00b_meta", "__DATA_CONST");
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
    check_native_symbols(image, size, &target, &section,
        sizeof(sections) / sizeof(*sections), symbols,
        sizeof(symbols) / sizeof(*symbols));
    assert(core_object_section_find(image, size, CORE_TARGET_OBJECT_MACH_O,
                                    "__DATA", "__data", &section));
    assert(core_object_emit_native_section_index(&target, 2, 2,
                                                  &native_index));
    assert(native_index == section.native_index);
    check_native_section(image, size, CORE_TARGET_OBJECT_MACH_O,
                         native_index, "__data", "__DATA");
    assert(section.native_index == 2 && section.alignment == 16);
    assert(!core_object_emit_native_section_index(&target,
        (size_t)UINT8_MAX + 1, 1, &native_index));
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
        {".n00b$M", NULL, data, sizeof data, 8, false, false, false},
        {".long_section_name", NULL, code, sizeof code, 16, false, true, false},
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
    uint32_t native_index = 0;
    assert(core_object_emit_native_section_index(&target, 2, 1,
                                                  &native_index));
    assert(native_index == section.native_index);
    check_native_section(image, size, CORE_TARGET_OBJECT_COFF,
                         native_index, ".n00b$M", NULL);
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
    check_native_symbols(image, size, &target, &section,
        sizeof(sections) / sizeof(*sections), symbols,
        sizeof(symbols) / sizeof(*symbols));
    assert(core_object_section_find(image, size, CORE_TARGET_OBJECT_COFF,
                                    NULL, ".long_section_name", &section));
    assert(core_object_emit_native_section_index(&target, 2, 2,
                                                  &native_index));
    assert(native_index == section.native_index);
    check_native_section(image, size, CORE_TARGET_OBJECT_COFF,
                         native_index, ".long_section_name", NULL);
    assert(section.native_index == 2 && section.alignment == 16 &&
           section.relocation_count == 1);
    assert(!core_object_emit_native_section_index(&target,
        (size_t)INT16_MAX + 1, 1, &native_index));
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
        ".n00b$M", NULL, data, count * 8, 8, false, false, false,
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

static void thread_local_section_test(void)
{
    struct {
        const char *target;
        const char *name;
        const char *segment;
        core_target_object_format_t format;
    } cases[] = {
        {"x86_64-linux", ".tdata.n00b", NULL, CORE_TARGET_OBJECT_ELF},
        {"arm64-macos", "__n00b_tls", "__DATA", CORE_TARGET_OBJECT_MACH_O},
        {"x86_64-windows", ".tls$N", NULL, CORE_TARGET_OBJECT_COFF},
    };
    uint8_t bytes[8] = {1};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        core_target_desc_t target;
        assert(core_target_profile(cases[i].target, 0, &target));
        core_object_emit_section_t input = {
            .name = cases[i].name, .segment = cases[i].segment,
            .bytes = bytes, .size = sizeof bytes, .alignment = 8,
            .writable = true, .is_thread_local = true,
        };
        uint8_t *image = NULL;
        size_t size = 0;
        assert(core_object_emit(&target, &input, 1, NULL, 0, NULL, 0,
                                &image, &size));
        core_object_section_t section;
        assert(core_object_section_find(image, size, cases[i].format,
            cases[i].segment, cases[i].name, &section));
        assert(section.size == sizeof bytes && section.bytes[0] == 1);
        if (cases[i].format == CORE_TARGET_OBJECT_ELF)
            assert((section.flags & 0x403) == 0x403);
        else if (cases[i].format == CORE_TARGET_OBJECT_MACH_O)
            assert((section.flags & 0xff) == 0x11);
        else assert((section.flags & 0xc0000040u) == 0xc0000040u);
        core_free(image);
        input.writable = false;
        assert(!core_object_emit(&target, &input, 1, NULL, 0, NULL, 0,
                                 &image, &size));
        input.writable = true;
        input.executable = true;
        assert(!core_object_emit(&target, &input, 1, NULL, 0, NULL, 0,
                                 &image, &size));
        input.executable = false;
        if (cases[i].format == CORE_TARGET_OBJECT_COFF) {
            input.name = ".data";
            assert(!core_object_emit(&target, &input, 1, NULL, 0, NULL, 0,
                                     &image, &size));
        }
    }
}

static void duplicate_section_names_test(void)
{
    const char *triples[] = {
        "x86_64-linux", "arm64-macos", "x86_64-windows",
    };
    for (size_t i = 0; i < sizeof(triples) / sizeof(*triples); i++) {
        core_target_desc_t target;
        assert(core_target_profile(triples[i], 0, &target));
        bool mach = target.object_format == CORE_TARGET_OBJECT_MACH_O;
        uint8_t first[16] = {1}, second[8] = {2};
        core_object_emit_section_t sections[] = {
            {.name = mach ? "__same" : ".same",
             .segment = mach ? "__DATA_CONST" : NULL,
             .bytes = first, .size = sizeof(first), .alignment = 8},
            {.name = mach ? "__same" : ".same",
             .segment = mach ? "__DATA_CONST" : NULL,
             .bytes = second, .size = sizeof(second), .alignment = 8},
        };
        core_object_emit_symbol_t symbol = {
            .name = "second", .section_index = 2,
        };
        core_object_emit_absolute64_t relocation = {
            .section_index = 1, .section_offset = 8, .symbol_index = 0,
        };
        uint8_t *image = NULL;
        size_t image_size = 0;
        assert(core_object_emit(&target, sections, 2, &symbol, 1,
            &relocation, 1, &image, &image_size));
        uint32_t first_index, second_index;
        core_object_section_t first_section, second_section;
        assert(core_object_emit_native_section_index(&target, 2, 1,
            &first_index));
        assert(core_object_emit_native_section_index(&target, 2, 2,
            &second_index));
        assert(first_index != second_index);
        assert(core_object_section_at(image, image_size,
            target.object_format, first_index, &first_section));
        assert(core_object_section_at(image, image_size,
            target.object_format, second_index, &second_section));
        assert(first_section.bytes[0] == 1);
        assert(second_section.bytes[0] == 2);
        core_object_absolute64_relocation_t decoded;
        assert(core_object_absolute64_relocation_at(&first_section,
            target.object_format, 0, &decoded));
        if (mach) assert(decoded.target_is_section
            && decoded.native_symbol_index == second_index);
        else {
            core_object_symbol_t resolved;
            assert(!decoded.target_is_section
                && core_object_symbol_at(image, image_size,
                    target.object_format, &first_section,
                    decoded.native_symbol_index, &resolved)
                && resolved.native_section_index == second_index);
        }
        core_free(image);
    }
}

int main(int argc, char **argv)
{
    core_target_desc_t target;
    assert(core_target_profile("x86_64-linux", 0, &target));
    uint8_t metadata[16] = {0};
    uint8_t code[8] = {0};
    core_object_emit_section_t sections[] = {
        {".n00b.meta", NULL, metadata, sizeof metadata, 8, false, false, false},
        {".text", NULL, code, sizeof code, 16, false, true, false},
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
    uint32_t native_index = 0;
    assert(core_object_emit_native_section_index(&target, 2, 1,
                                                  &native_index));
    assert(native_index == section.native_index);
    check_native_section(image, size, CORE_TARGET_OBJECT_ELF,
                         native_index, ".n00b.meta", NULL);
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
    check_native_symbols(image, size, &target, &section,
        sizeof(sections) / sizeof(*sections), symbols,
        sizeof(symbols) / sizeof(*symbols));
    assert(!core_object_symbol_name_at(image, size, CORE_TARGET_OBJECT_ELF,
                                       &section, UINT32_MAX, &name));
    assert(core_object_section_find(image, size, CORE_TARGET_OBJECT_ELF,
                                    NULL, ".text", &section));
    assert(core_object_emit_native_section_index(&target, 2, 2,
                                                  &native_index));
    assert(native_index == section.native_index);
    check_native_section(image, size, CORE_TARGET_OBJECT_ELF,
                         native_index, ".text", NULL);
    assert(section.native_index == 5 && section.alignment == 16 &&
           section.flags == 6);
    native_index = UINT32_MAX;
    assert(!core_object_emit_native_section_index(&target, 2, 0,
                                                   &native_index)
           && native_index == UINT32_MAX);
    assert(!core_object_emit_native_section_index(&target, 2, 3,
                                                   &native_index)
           && native_index == UINT32_MAX);
    assert(!core_object_emit_native_section_index(&target, UINT16_MAX, 1,
                                                   &native_index)
           && native_index == UINT32_MAX);
    core_target_desc_t unsupported_target = target;
    unsupported_target.pointer_bits = 32;
    assert(!core_object_emit_native_section_index(&unsupported_target, 2, 1,
                                                   &native_index)
           && native_index == UINT32_MAX);
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
    duplicate_section_names_test();
    thread_local_section_test();
    mach_test(argc >= 3 ? argv[2] : NULL);
    coff_test(argc >= 4 ? argv[3] : NULL);
    coff_relocation_overflow_test(argc >= 5 ? argv[4] : NULL);
    return 0;
}
