#include "lib/object_emit.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/byteorder.h"
#include "lib/object_section.h"

#define S(x) CORE_STRING_STATIC(x)
#define NONE ((core_string_t){})

static void save_object(const char *path, const uint8_t *image, size_t size)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(image, 1, size, file) == size);
    assert(!fclose(file));
}

static bool bytes_equal(const uint8_t *bytes, size_t length,
                        core_string_t expected)
{
    return core_string_eq(core_string_borrow((const char *)bytes, length),
                          expected);
}

static void check_native_section(const uint8_t *image, size_t size,
                                 core_target_object_format_t format,
                                 uint32_t index, core_string_t name,
                                 core_string_t segment)
{
    uint32_t count = 0;
    core_object_section_t view = {0};
    assert(core_object_section_count(image, size, format, &count)
        && index <= count
        && core_object_section_at(image, size, format, index, &view)
        && view.native_index == index
        && view.name_length == name.u8_bytes
        && bytes_equal(view.name, view.name_length, name));
    if (segment.data)
        assert(view.segment_length == segment.u8_bytes
            && bytes_equal(view.segment, view.segment_length, segment));
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
            if (symbol.name.length != expected[i].name.u8_bytes
                || !bytes_equal(symbol.name.bytes, symbol.name.length,
                                expected[i].name))
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
    assert(core_target_profile(S("arm64-macos"), 0, &target));
    uint8_t data[16] = {0}, mutable_data[8] = {0};
    core_object_emit_section_t sections[] = {
        {S("__n00b_meta"), S("__DATA_CONST"), data, sizeof data, 8, false, false, false, false},
        {S("__data"), S("__DATA"), mutable_data, sizeof mutable_data, 16, true, false, false, false},
    };
    core_object_emit_symbol_t symbols[] = {
        {S("_external_symbol"), 0, 0, true},
        {S("_local_target"), 2, 0, false},
        {S("_defined_symbol"), 2, 0, true},
    };
    core_object_emit_absolute64_t relocations[] = {
        {1, 8, 0, 5, false},
        {2, 0, 1, 0, false},
    };
    uint8_t *image = NULL;
    size_t size = 0;
    assert(core_object_emit(&target, sections, 2, symbols, 3, relocations, 2,
                            &image, &size));
    assert(core_le_load_u32(image) == 0xfeedfacf);
    core_object_section_t section;
    assert(core_object_section_find(image, size, CORE_TARGET_OBJECT_MACH_O,
                                    S("__DATA_CONST"), S("__n00b_meta"),
                                    &section));
    uint32_t native_index = 0;
    assert(core_object_emit_native_section_index(&target, 2, 1,
                                                  &native_index));
    assert(native_index == section.native_index);
    check_native_section(image, size, CORE_TARGET_OBJECT_MACH_O,
                         native_index, S("__n00b_meta"), S("__DATA_CONST"));
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
    assert(bytes_equal(name.bytes, name.length, S("_external_symbol")));
    check_native_symbols(image, size, &target, &section,
        sizeof(sections) / sizeof(*sections), symbols,
        sizeof(symbols) / sizeof(*symbols));
    assert(core_object_section_find(image, size, CORE_TARGET_OBJECT_MACH_O,
                                    S("__DATA"), S("__data"), &section));
    assert(core_object_emit_native_section_index(&target, 2, 2,
                                                  &native_index));
    assert(native_index == section.native_index);
    check_native_section(image, size, CORE_TARGET_OBJECT_MACH_O,
                         native_index, S("__data"), S("__DATA"));
    assert(section.native_index == 2 && section.alignment == 16);
    assert(!core_object_emit_native_section_index(&target,
        (size_t)UINT8_MAX + 1, 1, &native_index));
    assert(core_object_absolute64_relocation_at(
        &section, CORE_TARGET_OBJECT_MACH_O, 0, &relocation));
    assert(relocation.target_is_section &&
           relocation.native_symbol_index == 2 &&
           relocation.addend_bits == 16);
    relocations[1].symbol_relocation = true;
    uint8_t *symbol_image = NULL;
    size_t symbol_size = 0;
    assert(core_object_emit(&target, sections, 2, symbols, 3, relocations, 2,
                            &symbol_image, &symbol_size));
    assert(core_object_section_find(symbol_image, symbol_size,
        CORE_TARGET_OBJECT_MACH_O, S("__DATA"), S("__data"), &section));
    assert(core_object_absolute64_relocation_at(&section,
        CORE_TARGET_OBJECT_MACH_O, 0, &relocation));
    assert(!relocation.target_is_section && relocation.addend_bits == 0);
    assert(core_object_symbol_name_at(symbol_image, symbol_size,
        CORE_TARGET_OBJECT_MACH_O, &section,
        relocation.native_symbol_index, &name));
    assert(bytes_equal(name.bytes, name.length, S("_local_target")));
    core_free(symbol_image);
    if (output_path) save_object(output_path, image, size);
    core_free(image);
}

static void coff_test(const char *output_path)
{
    core_target_desc_t target;
    assert(core_target_profile(S("x86_64-windows"), 0, &target));
    uint8_t data[16] = {0}, code[8] = {0};
    core_object_emit_section_t sections[] = {
        {S(".n00b$M"), NONE, data, sizeof data, 8, false, false, false, false},
        {S(".long_section_name"), NONE, code, sizeof code, 16, false, true, false, false},
    };
    core_object_emit_symbol_t symbols[] = {
        {S("external_symbol"), 0, 0, true},
        {S("local_target"), 2, 0, false},
        {S("start"), 2, 0, true},
    };
    core_object_emit_absolute64_t relocations[] = {
        {1, 8, 0, 5, false},
        {2, 0, 1, 0, false},
    };
    uint8_t *image = NULL;
    size_t size = 0;
    assert(core_object_emit(&target, sections, 2, symbols, 3, relocations, 2,
                            &image, &size));
    assert(core_le_load_u16(image) == 0x8664);
    core_object_section_t section;
    assert(core_object_section_find(image, size, CORE_TARGET_OBJECT_COFF,
                                    NONE, S(".n00b$M"), &section));
    uint32_t native_index = 0;
    assert(core_object_emit_native_section_index(&target, 2, 1,
                                                  &native_index));
    assert(native_index == section.native_index);
    check_native_section(image, size, CORE_TARGET_OBJECT_COFF,
                         native_index, S(".n00b$M"), NONE);
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
    assert(bytes_equal(name.bytes, name.length, S("external_symbol")));
    check_native_symbols(image, size, &target, &section,
        sizeof(sections) / sizeof(*sections), symbols,
        sizeof(symbols) / sizeof(*symbols));
    assert(core_object_section_find(image, size, CORE_TARGET_OBJECT_COFF,
                                    NONE, S(".long_section_name"), &section));
    assert(core_object_emit_native_section_index(&target, 2, 2,
                                                  &native_index));
    assert(native_index == section.native_index);
    check_native_section(image, size, CORE_TARGET_OBJECT_COFF,
                         native_index, S(".long_section_name"), NONE);
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
    assert(bytes_equal(name.bytes, name.length, S("local_target")));
    uint32_t symbol_table_offset = core_le_load_u32(image + 8);
    core_le_store_u32(image + 8, 0);
    assert(!core_object_section_find(image, size, CORE_TARGET_OBJECT_COFF,
                                     NONE, S(".long_section_name"), &section));
    core_le_store_u32(image + 8, symbol_table_offset);
    if (output_path) save_object(output_path, image, size);
    core_free(image);
}

static void coff_relocation_overflow_test(const char *output_path)
{
    core_target_desc_t target;
    assert(core_target_profile(S("x86_64-windows"), 0, &target));
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
        S(".n00b$M"), NONE, data, count * 8, 8, false, false, false, false,
    };
    core_object_emit_symbol_t symbol = {S("external_symbol"), 0, 0, true};
    uint8_t *image = NULL;
    size_t size = 0;
    assert(core_object_emit(&target, &section_input, 1, &symbol, 1,
                            relocations, count, &image, &size));
    core_object_section_t section;
    assert(core_object_section_find(image, size, CORE_TARGET_OBJECT_COFF,
                                    NONE, S(".n00b$M"), &section));
    assert(section.relocation_count == count);
    assert(section.flags & 0x01000000);
    core_object_absolute64_relocation_t relocation;
    assert(core_object_absolute64_relocation_at(
        &section, CORE_TARGET_OBJECT_COFF, count - 1, &relocation));
    assert(relocation.section_offset == (count - 1) * 8);
    uint32_t overflow_offset = core_le_load_u32(image + 20 + 24);
    core_le_store_u32(image + overflow_offset + 4, 1);
    assert(!core_object_section_find(image, size, CORE_TARGET_OBJECT_COFF,
                                     NONE, S(".n00b$M"), &section));
    core_le_store_u32(image + overflow_offset + 4, 0);
    if (output_path) save_object(output_path, image, size);
    core_free(image);
    core_free(relocations);
    core_free(data);
}

static void thread_local_section_test(void)
{
    struct {
        core_string_t target;
        core_string_t name;
        core_string_t segment;
        core_target_object_format_t format;
    } cases[] = {
        {S("x86_64-linux"), S(".tdata.n00b"), NONE, CORE_TARGET_OBJECT_ELF},
        {S("arm64-macos"), S("__n00b_tls"), S("__DATA"),
         CORE_TARGET_OBJECT_MACH_O},
        {S("x86_64-windows"), S(".tls$N"), NONE, CORE_TARGET_OBJECT_COFF},
    };
    uint8_t bytes[16] = {1};
    uint8_t backing[8] = {2};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        core_target_desc_t target;
        assert(core_target_profile(cases[i].target, 0, &target));
        core_object_emit_section_t input = {
            .name = cases[i].name, .segment = cases[i].segment,
            .bytes = bytes, .size = sizeof bytes, .alignment = 8,
            .writable = true, .is_thread_local = true,
        };
        core_object_emit_section_t sections[] = {
            input,
            {.name = cases[i].format == CORE_TARGET_OBJECT_MACH_O
                         ? S("__const") : S(".rodata.n00b"),
             .segment = cases[i].format == CORE_TARGET_OBJECT_MACH_O
                            ? S("__DATA_CONST") : NONE,
             .bytes = backing, .size = sizeof backing, .alignment = 8},
        };
        core_object_emit_symbol_t symbols[] = {
            {.name = S("tls_backing"), .section_index = 2},
            {.name = S("tls_variable"), .section_index = 1,
             .exported = true},
        };
        core_object_emit_absolute64_t relocation = {
            .section_index = 1, .section_offset = 8, .symbol_index = 0,
        };
        uint8_t *image = NULL;
        size_t size = 0;
        assert(core_object_emit(&target, sections, 2, symbols, 2,
                                &relocation, 1, &image, &size));
        core_object_section_t section;
        assert(core_object_section_find(image, size, cases[i].format,
            cases[i].segment, cases[i].name, &section));
        assert(section.size == sizeof bytes && section.bytes[0] == 1);
        assert(section.relocation_count == 1);
        uint32_t backing_index;
        assert(core_object_emit_native_section_index(&target, 2, 2,
                                                     &backing_index));
        core_object_absolute64_relocation_t decoded;
        assert(core_object_absolute64_relocation_at(&section,
            cases[i].format, 0, &decoded));
        assert(decoded.section_offset == 8);
        if (cases[i].format == CORE_TARGET_OBJECT_MACH_O) {
            assert(decoded.target_is_section &&
                   decoded.native_symbol_index == backing_index);
        } else {
            core_object_symbol_t resolved;
            assert(!decoded.target_is_section &&
                   core_object_symbol_at(image, size, cases[i].format,
                                         &section, decoded.native_symbol_index,
                                         &resolved) &&
                   resolved.native_section_index == backing_index);
        }
        if (cases[i].format == CORE_TARGET_OBJECT_ELF) {
            assert((section.flags & 0x403) == 0x403);
            size_t header = (size_t)core_le_load_u64(image + 40);
            size_t symtab = (size_t)core_le_load_u64(image + header
                + 3 * 64 + 24);
            assert((image[symtab + 2 * 24 + 4] & 0x0f) == 6);
        }
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
            input.name = S(".data");
            assert(!core_object_emit(&target, &input, 1, NULL, 0, NULL, 0,
                                     &image, &size));
        }
    }
}

static void thread_local_descriptor_section_test(void)
{
    core_target_desc_t target;
    assert(core_target_profile(S("arm64-macos"), 0, &target));
    uint8_t descriptor[24] = {0};
    core_object_emit_section_t section = {
        .name = S("__thread_vars"), .segment = S("__DATA"),
        .bytes = descriptor, .size = sizeof descriptor, .alignment = 8,
        .writable = true, .is_thread_local_descriptor = true,
    };
    uint8_t *image = NULL;
    size_t size = 0;
    assert(core_object_emit(&target, &section, 1, NULL, 0, NULL, 0,
                            &image, &size));
    core_object_section_t emitted;
    assert(core_object_section_find(image, size, CORE_TARGET_OBJECT_MACH_O,
        S("__DATA"), S("__thread_vars"), &emitted));
    assert((emitted.flags & 0xff) == 0x13);
    core_free(image);
    section.is_thread_local = true;
    assert(!core_object_emit(&target, &section, 1, NULL, 0, NULL, 0,
                             &image, &size));
    section.is_thread_local = false;
    assert(core_target_profile(S("x86_64-linux"), 0, &target));
    assert(!core_object_emit(&target, &section, 1, NULL, 0, NULL, 0,
                             &image, &size));
}

static void duplicate_section_names_test(void)
{
    const core_string_t triples[] = {
        S("x86_64-linux"), S("arm64-macos"), S("x86_64-windows"),
    };
    for (size_t i = 0; i < sizeof(triples) / sizeof(*triples); i++) {
        core_target_desc_t target;
        assert(core_target_profile(triples[i], 0, &target));
        bool mach = target.object_format == CORE_TARGET_OBJECT_MACH_O;
        uint8_t first[16] = {1}, second[8] = {2};
        core_object_emit_section_t sections[] = {
            {.name = mach ? S("__same") : S(".same"),
             .segment = mach ? S("__DATA_CONST") : NONE,
             .bytes = first, .size = sizeof(first), .alignment = 8},
            {.name = mach ? S("__same") : S(".same"),
             .segment = mach ? S("__DATA_CONST") : NONE,
             .bytes = second, .size = sizeof(second), .alignment = 8},
        };
        core_object_emit_symbol_t symbol = {
            .name = S("second"), .section_index = 2,
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
    assert(core_target_profile(S("x86_64-linux"), 0, &target));
    uint8_t metadata[16] = {0};
    uint8_t code[8] = {0};
    core_object_emit_section_t sections[] = {
        {S(".n00b.meta"), NONE, metadata, sizeof metadata, 8, false, false, false, false},
        {S(".text"), NONE, code, sizeof code, 16, false, true, false, false},
    };
    core_object_emit_symbol_t symbols[] = {
        {S("external_symbol"), 0, 0, true},
        {S("local_target"), 2, 0, false},
        {S("start"), 2, 0, true},
    };
    core_object_emit_absolute64_t relocations[] = {
        {1, 8, 0, 5, false},
        {2, 0, 1, 0, false},
    };
    uint8_t *image = NULL;
    size_t size = 0;
    assert(core_object_emit(&target, sections, 2, symbols, 3, relocations, 2,
                            &image, &size));
    assert(image && size > 64 && core_le_load_u64(image + 40) < size);
    assert(core_le_load_u16(image + 18) == 62);
    core_object_section_t section;
    assert(core_object_section_find(image, size, CORE_TARGET_OBJECT_ELF,
                                    NONE, S(".n00b.meta"), &section));
    uint32_t native_index = 0;
    assert(core_object_emit_native_section_index(&target, 2, 1,
                                                  &native_index));
    assert(native_index == section.native_index);
    check_native_section(image, size, CORE_TARGET_OBJECT_ELF,
                         native_index, S(".n00b.meta"), NONE);
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
    assert(bytes_equal(name.bytes, name.length, S("external_symbol")));
    check_native_symbols(image, size, &target, &section,
        sizeof(sections) / sizeof(*sections), symbols,
        sizeof(symbols) / sizeof(*symbols));
    assert(!core_object_symbol_name_at(image, size, CORE_TARGET_OBJECT_ELF,
                                       &section, UINT32_MAX, &name));
    assert(core_object_section_find(image, size, CORE_TARGET_OBJECT_ELF,
                                    NONE, S(".text"), &section));
    assert(core_object_emit_native_section_index(&target, 2, 2,
                                                  &native_index));
    assert(native_index == section.native_index);
    check_native_section(image, size, CORE_TARGET_OBJECT_ELF,
                         native_index, S(".text"), NONE);
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
    assert(bytes_equal(name.bytes, name.length, S("local_target")));

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
    thread_local_descriptor_section_test();
    mach_test(argc >= 3 ? argv[2] : NULL);
    coff_test(argc >= 4 ? argv[3] : NULL);
    coff_relocation_overflow_test(argc >= 5 ? argv[4] : NULL);
    return 0;
}
