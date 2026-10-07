#include "lib/object_section.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/byteorder.h"

static void elf_test(void)
{
    uint8_t image[512] = {0};
    memcpy(image, "\x7f" "ELF", 4);
    image[4] = 2;
    image[5] = 1;
    image[6] = 1;
    core_le_store_u16(image + 16, 1);
    core_le_store_u64(image + 40, 128);
    core_le_store_u16(image + 58, 64);
    core_le_store_u16(image + 60, 4);
    core_le_store_u16(image + 62, 1);
    memcpy(image + 64, "\0.shstrtab\0.n00b.meta\0.rela.n00b.meta\0", 39);
    memcpy(image + 104, "n00b", 4);
    const size_t shstr = 128 + 64;
    core_le_store_u32(image + shstr, 1);
    core_le_store_u32(image + shstr + 4, 3);
    core_le_store_u64(image + shstr + 24, 64);
    core_le_store_u64(image + shstr + 32, 39);
    const size_t meta = shstr + 64;
    core_le_store_u32(image + meta, 11);
    core_le_store_u32(image + meta + 4, 1);
    core_le_store_u64(image + meta + 8, 2);
    core_le_store_u64(image + meta + 24, 104);
    core_le_store_u64(image + meta + 32, 4);
    core_le_store_u64(image + meta + 48, 8);
    const size_t rela = meta + 64;
    core_le_store_u32(image + rela, 22);
    core_le_store_u32(image + rela + 4, 4);
    core_le_store_u64(image + rela + 24, 108);
    core_le_store_u64(image + rela + 32, 24);
    core_le_store_u32(image + rela + 44, 2);
    core_le_store_u64(image + rela + 56, 24);

    core_object_section_t section = {0};
    assert(core_object_section_find(image, sizeof image, CORE_TARGET_OBJECT_ELF,
                                    NULL, ".n00b.meta", &section));
    assert(section.native_index == 2 && section.size == 4);
    assert(section.bytes == image + 104 && section.alignment == 8);
    assert(section.relocations == image + 108 && section.relocation_count == 1);
    assert(section.relocation_entry_size == 24);
    assert(!core_object_section_find(image, 120, CORE_TARGET_OBJECT_ELF,
                                     NULL, ".n00b.meta", &section));
    assert(section.native_index == 2);
    assert(!core_object_section_find(image, sizeof image, CORE_TARGET_OBJECT_ELF,
                                     NULL, ".missing", &section));
    core_le_store_u32(image + rela + 44, 99);
    assert(core_object_section_find(image, sizeof image, CORE_TARGET_OBJECT_ELF,
                                    NULL, ".n00b.meta", &section));
    assert(section.relocation_count == 0);
}

static void macho_test(void)
{
    uint8_t image[256] = {0};
    core_le_store_u32(image, 0xfeedfacf);
    core_le_store_u32(image + 12, 1);
    core_le_store_u32(image + 16, 1);
    core_le_store_u32(image + 20, 152);
    core_le_store_u32(image + 32, 0x19);
    core_le_store_u32(image + 36, 152);
    core_le_store_u32(image + 96, 1);
    memcpy(image + 104, "__meta", 6);
    memcpy(image + 120, "__N00B", 6);
    core_le_store_u64(image + 144, 4);
    core_le_store_u32(image + 152, 184);
    core_le_store_u32(image + 156, 3);
    core_le_store_u32(image + 160, 188);
    core_le_store_u32(image + 164, 1);
    memcpy(image + 184, "n00b", 4);
    core_object_section_t section = {0};
    assert(core_object_section_find(image, sizeof image, CORE_TARGET_OBJECT_MACH_O,
                                    "__N00B", "__meta", &section));
    assert(section.native_index == 1 && section.size == 4);
    assert(section.bytes == image + 184 && section.alignment == 8);
    assert(section.relocations == image + 188 && section.relocation_count == 1);
    assert(!core_object_section_find(image, 187, CORE_TARGET_OBJECT_MACH_O,
                                     "__N00B", "__meta", &section));
    assert(!core_object_section_find(image, sizeof image, CORE_TARGET_OBJECT_MACH_O,
                                     "__OTHER", "__meta", &section));
}

static void coff_test(void)
{
    uint8_t image[128] = {0};
    core_le_store_u16(image, 0x8664);
    core_le_store_u16(image + 2, 1);
    memcpy(image + 20, ".n00b$M", 7);
    core_le_store_u32(image + 36, 4);
    core_le_store_u32(image + 40, 64);
    core_le_store_u32(image + 44, 68);
    core_le_store_u16(image + 52, 1);
    core_le_store_u32(image + 56, 0x40500040);
    memcpy(image + 64, "n00b", 4);
    core_object_section_t section = {0};
    assert(core_object_section_find(image, sizeof image, CORE_TARGET_OBJECT_COFF,
                                    NULL, ".n00b$M", &section));
    assert(section.native_index == 1 && section.size == 4);
    assert(section.bytes == image + 64 && section.alignment == 16);
    assert(section.relocations == image + 68 && section.relocation_count == 1);
    assert(!core_object_section_find(image, 77, CORE_TARGET_OBJECT_COFF,
                                     NULL, ".n00b$M", &section));
    assert(!core_object_section_find(image, sizeof image, CORE_TARGET_OBJECT_COFF,
                                     NULL, ".missing", &section));
}

static void real_object_test(const char *path, core_target_object_format_t format,
                             const char *segment, const char *name)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(!fseek(file, 0, SEEK_END));
    long length = ftell(file);
    assert(length > 0);
    assert(!fseek(file, 0, SEEK_SET));
    uint8_t *image = malloc((size_t)length);
    assert(image);
    assert(fread(image, 1, (size_t)length, file) == (size_t)length);
    assert(!fclose(file));
    core_object_section_t section = {0};
    assert(core_object_section_find(image, (size_t)length, format, segment,
                                    name, &section));
    assert(section.native_index && section.size == 8);
    assert(section.relocation_count == 1);
    assert(section.relocations >= image);
    assert(section.relocations + section.relocation_entry_size <=
           image + length);
    free(image);
}

int main(int argc, char **argv)
{
    elf_test();
    macho_test();
    coff_test();
    if (argc == 4) {
        real_object_test(argv[1], CORE_TARGET_OBJECT_ELF, NULL, ".n00b.meta");
        real_object_test(argv[2], CORE_TARGET_OBJECT_MACH_O, "__N00B", "__meta");
        real_object_test(argv[3], CORE_TARGET_OBJECT_COFF, NULL, ".n00b$M");
    }
    return 0;
}
