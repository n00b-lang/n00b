#include "compiler/objfile/elf_rewrite.h"

#include <string.h>

#include "compiler/objfile/elf_layout.h"
#include "text/strings/string_ops.h"

#define N00B_ELF_SHN_LORESERVE 0xff00u
#define N00B_ELF_PN_XNUM       0xffffu
#define N00B_ELF64_EHDR_SIZE   64u
#define N00B_ELF64_PHDR_SIZE   56u
#define N00B_ELF64_SHDR_SIZE   64u
#define N00B_ELF_REWRITE_MAX_PATCHES 8u

typedef struct raw_shdr {
    uint32_t name;
    uint32_t type;
    uint64_t flags;
    uint64_t addr;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t addralign;
    uint64_t entsize;
} raw_shdr_t;

typedef struct chalk_mark_shape {
    uint64_t target_index;
    uint32_t target_name_index;
    uint64_t removed_name_start;
    uint64_t removed_name_end;
    uint64_t removed_name_size;
    uint64_t compact_strtab_size;
    uint64_t new_strtab_size;
    uint64_t new_shtab_size;
    uint32_t replacement_name_index;
    uint16_t new_shstrndx;
} chalk_mark_shape_t;

static bool
section_name_write_size(n00b_string_t *section_name, uint64_t *out);

static bool
checked_add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (UINT64_MAX - a < b) {
        return false;
    }

    *out = a + b;
    return true;
}

static bool
checked_mul_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a != 0 && b > UINT64_MAX / a) {
        return false;
    }

    *out = a * b;
    return true;
}

static bool
validate_packager_bounds(uint64_t obj_size,
                         uint64_t obj_offset,
                         uint64_t file_size,
                         uint64_t *out_end)
{
    uint64_t end;

    if (!checked_add_u64(obj_size, obj_offset, &end)) {
        return false;
    }

    if (end <= obj_size || end > file_size) {
        return false;
    }

    if (out_end != nullptr) {
        *out_end = end;
    }
    return true;
}

static bool
is_big_endian(n00b_elf_binary_t *bin)
{
    return bin->header.ident[EI_DATA] == ELFDATA2MSB;
}

static uint32_t
raw_u32(const uint8_t *p, bool big_endian)
{
    if (big_endian) {
        return ((uint32_t)p[0] << 24)
             | ((uint32_t)p[1] << 16)
             | ((uint32_t)p[2] << 8)
             | (uint32_t)p[3];
    }

    return ((uint32_t)p[3] << 24)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[1] << 8)
         | (uint32_t)p[0];
}

static uint64_t
raw_u64(const uint8_t *p, bool big_endian)
{
    if (big_endian) {
        return ((uint64_t)raw_u32(p, true) << 32) | raw_u32(p + 4, true);
    }

    return ((uint64_t)raw_u32(p + 4, false) << 32) | raw_u32(p, false);
}

static int
packager_errcode_for_profile(n00b_elf_rewrite_target_profile_reason_t reason)
{
    switch (reason) {
    case N00B_ELF_REWRITE_PROFILE_OK:                         return 0;
    case N00B_ELF_REWRITE_PROFILE_INVALID_EHSIZE:             return 11;
    case N00B_ELF_REWRITE_PROFILE_INVALID_SHENTSIZE:          return 15;
    case N00B_ELF_REWRITE_PROFILE_INVALID_PHENTSIZE:          return 16;
    case N00B_ELF_REWRITE_PROFILE_UNSUPPORTED_SHNLO:          return 17;
    case N00B_ELF_REWRITE_PROFILE_SHTAB_SIZE:                 return 18;
    case N00B_ELF_REWRITE_PROFILE_INVALID_SHTAB_BOUNDS:       return 19;
    case N00B_ELF_REWRITE_PROFILE_ZERO_PHNUM:                 return 20;
    case N00B_ELF_REWRITE_PROFILE_UNSUPPORTED_PXNUM:          return 21;
    case N00B_ELF_REWRITE_PROFILE_INVALID_PHTAB:              return 22;
    case N00B_ELF_REWRITE_PROFILE_INVALID_PHTAB_BOUNDS:       return 23;
    case N00B_ELF_REWRITE_PROFILE_NO_STRTAB:                  return 24;
    case N00B_ELF_REWRITE_PROFILE_STRTAB_SIZE_WRAP:           return 25;
    case N00B_ELF_REWRITE_PROFILE_UNSUPPORTED_STRTAB_SHNLO:   return 26;
    case N00B_ELF_REWRITE_PROFILE_INVALID_STRTAB_INDEX:       return 27;
    case N00B_ELF_REWRITE_PROFILE_INVALID_STRTAB_TYPE:        return 28;
    case N00B_ELF_REWRITE_PROFILE_STRTAB_SIZE:                return 29;
    case N00B_ELF_REWRITE_PROFILE_SECTION_NAME_INDEX:         return 35;
    }

    return -1;
}

static n00b_elf_rewrite_target_profile_t
profile_with_reason(n00b_elf_binary_t *bin,
                    n00b_elf_rewrite_target_profile_reason_t reason)
{
    n00b_elf_rewrite_target_profile_t profile = {
        .reason           = reason,
        .packager_errcode = packager_errcode_for_profile(reason),
    };

    if (bin != nullptr) {
        profile.section_count = bin->header.shnum;
        profile.segment_count = bin->header.phnum;
        if (bin->stream != nullptr && bin->stream->buf != nullptr) {
            profile.file_size = bin->stream->buf->byte_len;
        }
    }

    return profile;
}

static bool
read_raw_shdr(n00b_elf_binary_t *bin,
              uint64_t           index,
              raw_shdr_t        *out)
{
    n00b_buffer_t *buf = bin->stream->buf;
    uint64_t       off;
    uint64_t       shoff = bin->header.shoff;
    bool           big   = is_big_endian(bin);

    if (!checked_mul_u64(index, N00B_ELF64_SHDR_SIZE, &off)
        || !checked_add_u64(shoff, off, &off)
        || off > UINT64_MAX - N00B_ELF64_SHDR_SIZE
        || off + N00B_ELF64_SHDR_SIZE > buf->byte_len) {
        return false;
    }

    const uint8_t *p = (const uint8_t *)buf->data + off;
    *out = (raw_shdr_t){
        .name      = raw_u32(p + 0, big),
        .type      = raw_u32(p + 4, big),
        .flags     = raw_u64(p + 8, big),
        .addr      = raw_u64(p + 16, big),
        .offset    = raw_u64(p + 24, big),
        .size      = raw_u64(p + 32, big),
        .link      = raw_u32(p + 40, big),
        .info      = raw_u32(p + 44, big),
        .addralign = raw_u64(p + 48, big),
        .entsize   = raw_u64(p + 56, big),
    };
    return true;
}

static bool
zero_padding_gap_covers(n00b_elf_layout_t *layout, uint64_t start, uint64_t end)
{
    if (start == end) {
        return true;
    }

    if (layout == nullptr || end > layout->file_size) {
        return false;
    }

    auto gap_result = n00b_elf_layout_find_file_gap(layout,
                                                    start,
                                                    end,
                                                    end - start,
                                                    1);
    if (n00b_result_is_err(gap_result)) {
        return false;
    }

    n00b_option_t(n00b_elf_layout_gap_t) gap_opt = n00b_result_get(gap_result);
    if (!n00b_option_is_set(gap_opt)) {
        return false;
    }

    n00b_elf_layout_gap_t gap = n00b_option_get(gap_opt);
    return gap.kind == N00B_ELF_LAYOUT_GAP_ZERO_PADDING
        && gap.start == start
        && gap.end >= end;
}

static n00b_err_t
layout_err_to_rewrite_err(n00b_err_t err)
{
    return err == N00B_ELF_LAYOUT_ERR_OVERFLOW
        ? N00B_ELF_REWRITE_ERR_OVERFLOW
        : N00B_ELF_REWRITE_ERR_ADMISSION;
}

static bool
range_contains(uint64_t start, uint64_t end, uint64_t value)
{
    return value >= start && value < end;
}

static bool
ranges_overlap(uint64_t a_start, uint64_t a_end, uint64_t b_start, uint64_t b_end)
{
    return a_start < b_end && b_start < a_end;
}

static bool
policy_allows_append_after_overlay(n00b_elf_rewrite_admit_policy_t policy)
{
    return (policy.flags & N00B_ELF_REWRITE_ADMIT_POLICY_PRESERVE_OVERLAY) != 0
        && (policy.flags & N00B_ELF_REWRITE_ADMIT_POLICY_APPEND_AFTER_OVERLAY)
               != 0;
}

static uint64_t
effective_section_alignment(uint64_t requested)
{
    return requested == 0 ? 1 : requested;
}

static void
write_u16(uint8_t *p, uint16_t value, bool big_endian)
{
    if (big_endian) {
        p[0] = (uint8_t)(value >> 8);
        p[1] = (uint8_t)value;
        return;
    }

    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void
write_u32(uint8_t *p, uint32_t value, bool big_endian)
{
    if (big_endian) {
        p[0] = (uint8_t)(value >> 24);
        p[1] = (uint8_t)(value >> 16);
        p[2] = (uint8_t)(value >> 8);
        p[3] = (uint8_t)value;
        return;
    }

    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void
write_u64(uint8_t *p, uint64_t value, bool big_endian)
{
    if (big_endian) {
        write_u32(p, (uint32_t)(value >> 32), true);
        write_u32(p + 4, (uint32_t)value, true);
        return;
    }

    write_u32(p, (uint32_t)value, false);
    write_u32(p + 4, (uint32_t)(value >> 32), false);
}

static n00b_buffer_t *
new_zero_buffer(uint64_t size, n00b_allocator_t *allocator)
{
    if (size > (uint64_t)INT64_MAX || size > (uint64_t)SIZE_MAX) {
        return nullptr;
    }

    n00b_buffer_t *buf = n00b_buffer_new((int64_t)size,
                                         .allocator = allocator);
    memset(buf->data, 0, (size_t)size);
    buf->byte_len = (size_t)size;
    return buf;
}

static bool
section_name_is_chalk_mark(n00b_string_t *name)
{
    return name != nullptr && n00b_unicode_str_eq(name, r".chalk.mark");
}

static bool
section_is_metadata_chalk_mark(n00b_elf_section_t *sec)
{
    if (sec == nullptr || !section_name_is_chalk_mark(sec->name)
        || sec->flags != 0) {
        return false;
    }

    return sec->type == SHT_PROGBITS || sec->type == SHT_NOTE;
}

static bool
find_single_chalk_mark(n00b_elf_binary_t *bin, uint64_t *index_out)
{
    bool     found = false;
    uint64_t found_index = 0;

    for (uint32_t i = 0; i < bin->num_sections; i++) {
        if (!section_name_is_chalk_mark(bin->sections[i].name)) {
            continue;
        }

        if (found) {
            return false;
        }

        found = true;
        found_index = i;
    }

    if (!found) {
        return false;
    }

    *index_out = found_index;
    return true;
}

static bool
find_nul_in_strtab(n00b_elf_binary_t                 *bin,
                   n00b_elf_rewrite_target_profile_t *profile,
                   uint64_t                           start,
                   uint64_t                          *end_out)
{
    if (start >= profile->shstrtab_complete_size) {
        return false;
    }

    uint64_t raw_end;
    if (!checked_add_u64(profile->shstrtab_offset,
                         profile->shstrtab_reported_size,
                         &raw_end)
        || raw_end > bin->stream->buf->byte_len) {
        return false;
    }

    const uint8_t *bytes =
        (const uint8_t *)bin->stream->buf->data + profile->shstrtab_offset;
    for (uint64_t i = start; i < profile->shstrtab_reported_size; i++) {
        if (bytes[i] == 0) {
            *end_out = i;
            return true;
        }
    }

    if (profile->shstrtab_requires_terminator
        && profile->shstrtab_reported_size == profile->shstrtab_complete_size - 1
        && start <= profile->shstrtab_reported_size) {
        *end_out = profile->shstrtab_reported_size;
        return true;
    }

    return false;
}

static bool
name_index_survives_removed_span(uint32_t index, chalk_mark_shape_t *shape)
{
    if ((uint64_t)index < shape->removed_name_start) {
        return true;
    }

    return (uint64_t)index >= shape->removed_name_end;
}

static n00b_result_t(bool)
chalk_mark_payload_range_is_exclusive(n00b_elf_binary_t  *bin,
                                      n00b_elf_section_t *old_mark,
                                      chalk_mark_shape_t *shape,
                                      uint64_t            old_payload_end,
                                      n00b_allocator_t   *allocator)
{
    auto layout_result = n00b_elf_layout_build(bin, .allocator = allocator);
    if (n00b_result_is_err(layout_result)) {
        return n00b_result_err(bool,
                               layout_err_to_rewrite_err(
                                   n00b_result_get_err(layout_result)));
    }

    n00b_elf_layout_t *layout = n00b_result_get(layout_result);
    auto collision_result =
        n00b_elf_layout_file_collision(layout,
                                       old_mark->offset,
                                       old_payload_end,
                                       .allocator = allocator);
    if (n00b_result_is_err(collision_result)) {
        return n00b_result_err(bool,
                               layout_err_to_rewrite_err(
                                   n00b_result_get_err(collision_result)));
    }

    n00b_elf_layout_collision_t collision =
        n00b_result_get(collision_result);
    bool saw_mark_section = false;

    for (uint64_t i = 0; i < collision.interval_count; i++) {
        n00b_elf_layout_interval_t *interval = &collision.intervals[i];
        bool same_section = interval->index == shape->target_index
                         && interval->start == old_mark->offset
                         && interval->end == old_payload_end;

        if (same_section
            && interval->kind == N00B_ELF_LAYOUT_INTERVAL_SECTION_FILE) {
            saw_mark_section = true;
            continue;
        }

        if (same_section
            && old_mark->type == SHT_NOTE
            && interval->kind == N00B_ELF_LAYOUT_INTERVAL_NOTE_FILE) {
            continue;
        }

        return n00b_result_ok(bool, false);
    }

    return n00b_result_ok(bool, saw_mark_section);
}

static uint32_t
adjusted_name_index(uint32_t index, chalk_mark_shape_t *shape)
{
    if ((uint64_t)index < shape->removed_name_end) {
        return index;
    }

    return (uint32_t)((uint64_t)index - shape->removed_name_size);
}

static bool
compute_chalk_mark_shape(n00b_elf_binary_t                 *bin,
                         n00b_elf_rewrite_target_profile_t *profile,
                         bool                               replacement,
                         chalk_mark_shape_t                *shape)
{
    uint64_t target_index;
    uint64_t name_end;
    uint64_t name_bytes;

    if (bin == nullptr || profile == nullptr || shape == nullptr
        || bin->stream == nullptr || bin->stream->buf == nullptr
        || !find_single_chalk_mark(bin, &target_index)
        || target_index == bin->header.shstrndx
        || target_index >= profile->section_count
        || !section_is_metadata_chalk_mark(&bin->sections[target_index])) {
        return false;
    }

    raw_shdr_t target_shdr;
    if (!read_raw_shdr(bin, target_index, &target_shdr)
        || !find_nul_in_strtab(bin, profile, target_shdr.name, &name_end)
        || !checked_add_u64(name_end, 1, &name_end)
        || name_end <= target_shdr.name) {
        return false;
    }

    if (!section_name_write_size(r".chalk.mark", &name_bytes)) {
        return false;
    }

    *shape = (chalk_mark_shape_t){
        .target_index       = target_index,
        .target_name_index  = target_shdr.name,
        .removed_name_start = target_shdr.name,
        .removed_name_end   = name_end,
        .removed_name_size  = name_end - target_shdr.name,
    };

    if (profile->shstrtab_complete_size < shape->removed_name_size) {
        return false;
    }

    shape->compact_strtab_size =
        profile->shstrtab_complete_size - shape->removed_name_size;
    shape->new_strtab_size = shape->compact_strtab_size;
    if (replacement) {
        if (!checked_add_u64(shape->compact_strtab_size,
                             name_bytes,
                             &shape->new_strtab_size)
            || shape->compact_strtab_size > UINT32_MAX) {
            return false;
        }
        shape->replacement_name_index = (uint32_t)shape->compact_strtab_size;
    }

    uint64_t new_section_count =
        profile->section_count - 1 + (replacement ? 1u : 0u);
    if (new_section_count > UINT16_MAX
        || !checked_mul_u64(new_section_count,
                            N00B_ELF64_SHDR_SIZE,
                            &shape->new_shtab_size)) {
        return false;
    }

    uint64_t new_shstrndx = bin->header.shstrndx;
    if (target_index < bin->header.shstrndx) {
        new_shstrndx--;
    }
    if (new_shstrndx > UINT16_MAX) {
        return false;
    }
    shape->new_shstrndx = (uint16_t)new_shstrndx;

    for (uint64_t i = 0; i < profile->section_count; i++) {
        raw_shdr_t shdr;
        if (!read_raw_shdr(bin, i, &shdr)) {
            return false;
        }

        if (i == target_index) {
            continue;
        }

        if (!name_index_survives_removed_span(shdr.name, shape)) {
            return false;
        }
    }

    return true;
}

n00b_result_t(n00b_elf_rewrite_target_profile_t)
n00b_elf_rewrite_target_profile(n00b_elf_binary_t *bin)
{
    if (bin == nullptr || bin->stream == nullptr || bin->stream->buf == nullptr) {
        return n00b_result_err(n00b_elf_rewrite_target_profile_t,
                               N00B_ELF_REWRITE_ERR_NULL_BINARY);
    }

    n00b_buffer_t *buf       = bin->stream->buf;
    uint64_t       file_size = buf->byte_len;
    uint64_t       table_end;

    if (bin->header.ehsize != N00B_ELF64_EHDR_SIZE) {
        return n00b_result_ok(
            n00b_elf_rewrite_target_profile_t,
            profile_with_reason(bin,
                                N00B_ELF_REWRITE_PROFILE_INVALID_EHSIZE));
    }

    if (bin->header.shentsize != N00B_ELF64_SHDR_SIZE) {
        return n00b_result_ok(
            n00b_elf_rewrite_target_profile_t,
            profile_with_reason(bin,
                                N00B_ELF_REWRITE_PROFILE_INVALID_SHENTSIZE));
    }

    if (bin->header.phentsize != N00B_ELF64_PHDR_SIZE) {
        return n00b_result_ok(
            n00b_elf_rewrite_target_profile_t,
            profile_with_reason(bin,
                                N00B_ELF_REWRITE_PROFILE_INVALID_PHENTSIZE));
    }

    if (bin->header.shnum >= N00B_ELF_SHN_LORESERVE) {
        return n00b_result_ok(
            n00b_elf_rewrite_target_profile_t,
            profile_with_reason(bin,
                                N00B_ELF_REWRITE_PROFILE_UNSUPPORTED_SHNLO));
    }

    if (bin->header.shoff == 0 || bin->header.shnum < 2) {
        return n00b_result_ok(
            n00b_elf_rewrite_target_profile_t,
            profile_with_reason(bin, N00B_ELF_REWRITE_PROFILE_SHTAB_SIZE));
    }

    uint64_t shtab_size;
    if (!checked_mul_u64(bin->header.shnum,
                         N00B_ELF64_SHDR_SIZE,
                         &shtab_size)
        || !validate_packager_bounds(shtab_size,
                                     bin->header.shoff,
                                     file_size,
                                     &table_end)) {
        return n00b_result_ok(
            n00b_elf_rewrite_target_profile_t,
            profile_with_reason(bin,
                                N00B_ELF_REWRITE_PROFILE_INVALID_SHTAB_BOUNDS));
    }

    if (bin->header.phnum == 0) {
        return n00b_result_ok(
            n00b_elf_rewrite_target_profile_t,
            profile_with_reason(bin, N00B_ELF_REWRITE_PROFILE_ZERO_PHNUM));
    }

    if (bin->header.phnum == N00B_ELF_PN_XNUM) {
        return n00b_result_ok(
            n00b_elf_rewrite_target_profile_t,
            profile_with_reason(bin,
                                N00B_ELF_REWRITE_PROFILE_UNSUPPORTED_PXNUM));
    }

    if (bin->header.phoff == 0) {
        return n00b_result_ok(
            n00b_elf_rewrite_target_profile_t,
            profile_with_reason(bin, N00B_ELF_REWRITE_PROFILE_INVALID_PHTAB));
    }

    uint64_t phtab_size;
    if (!checked_mul_u64(bin->header.phnum,
                         N00B_ELF64_PHDR_SIZE,
                         &phtab_size)
        || !validate_packager_bounds(phtab_size,
                                     bin->header.phoff,
                                     file_size,
                                     nullptr)) {
        return n00b_result_ok(
            n00b_elf_rewrite_target_profile_t,
            profile_with_reason(bin,
                                N00B_ELF_REWRITE_PROFILE_INVALID_PHTAB_BOUNDS));
    }

    if (bin->header.shstrndx == 0) {
        return n00b_result_ok(
            n00b_elf_rewrite_target_profile_t,
            profile_with_reason(bin, N00B_ELF_REWRITE_PROFILE_NO_STRTAB));
    }

    if (bin->header.shstrndx >= N00B_ELF_SHN_LORESERVE) {
        return n00b_result_ok(
            n00b_elf_rewrite_target_profile_t,
            profile_with_reason(
                bin,
                N00B_ELF_REWRITE_PROFILE_UNSUPPORTED_STRTAB_SHNLO));
    }

    if (bin->header.shstrndx >= bin->header.shnum) {
        return n00b_result_ok(
            n00b_elf_rewrite_target_profile_t,
            profile_with_reason(
                bin,
                N00B_ELF_REWRITE_PROFILE_INVALID_STRTAB_INDEX));
    }

    raw_shdr_t shstrtab;
    if (!read_raw_shdr(bin, bin->header.shstrndx, &shstrtab)) {
        return n00b_result_ok(
            n00b_elf_rewrite_target_profile_t,
            profile_with_reason(bin,
                                N00B_ELF_REWRITE_PROFILE_INVALID_SHTAB_BOUNDS));
    }

    if (shstrtab.type != SHT_STRTAB) {
        return n00b_result_ok(
            n00b_elf_rewrite_target_profile_t,
            profile_with_reason(bin,
                                N00B_ELF_REWRITE_PROFILE_INVALID_STRTAB_TYPE));
    }

    uint64_t shstrtab_end;
    if (!validate_packager_bounds(shstrtab.size,
                                  shstrtab.offset,
                                  file_size,
                                  &shstrtab_end)) {
        return n00b_result_ok(
            n00b_elf_rewrite_target_profile_t,
            profile_with_reason(bin, N00B_ELF_REWRITE_PROFILE_STRTAB_SIZE));
    }

    n00b_elf_rewrite_target_profile_t profile =
        profile_with_reason(bin, N00B_ELF_REWRITE_PROFILE_OK);
    profile.shstrtab_offset        = shstrtab.offset;
    profile.shstrtab_reported_size = shstrtab.size;
    profile.shstrtab_complete_size = shstrtab.size;

    bool needs_terminator = shstrtab.size == 0;
    if (shstrtab.size != 0) {
        const uint8_t *p = (const uint8_t *)buf->data + shstrtab_end - 1;
        needs_terminator = *p != 0;
    }

    if (needs_terminator) {
        if (profile.shstrtab_complete_size == UINT64_MAX) {
            return n00b_result_ok(
                n00b_elf_rewrite_target_profile_t,
                profile_with_reason(
                    bin,
                    N00B_ELF_REWRITE_PROFILE_STRTAB_SIZE_WRAP));
        }
        profile.shstrtab_complete_size++;
        profile.shstrtab_requires_terminator = true;
    }

    for (uint64_t i = 0; i < bin->header.shnum; i++) {
        raw_shdr_t section;
        if (!read_raw_shdr(bin, i, &section)) {
            return n00b_result_ok(
                n00b_elf_rewrite_target_profile_t,
                profile_with_reason(
                    bin,
                    N00B_ELF_REWRITE_PROFILE_INVALID_SHTAB_BOUNDS));
        }

        if (section.name >= profile.shstrtab_reported_size) {
            return n00b_result_ok(
                n00b_elf_rewrite_target_profile_t,
                profile_with_reason(
                    bin,
                    N00B_ELF_REWRITE_PROFILE_SECTION_NAME_INDEX));
        }
    }

    return n00b_result_ok(n00b_elf_rewrite_target_profile_t, profile);
}

static n00b_elf_rewrite_plan_t *
new_plan(n00b_allocator_t *allocator)
{
    return n00b_alloc_with_opts(
        n00b_elf_rewrite_plan_t,
        &(n00b_alloc_opts_t){.allocator = allocator});
}

static n00b_result_t(n00b_elf_rewrite_plan_t *)
rejected_plan(n00b_allocator_t                         *allocator,
              n00b_elf_rewrite_rejection_reason_t      reason,
              n00b_elf_rewrite_target_profile_t        profile,
              n00b_elf_rewrite_admit_result_t         *admission)
{
    n00b_elf_rewrite_plan_t *plan = new_plan(allocator);

    plan->operation              = N00B_ELF_REWRITE_OPERATION_METADATA_INSERT;
    plan->outcome                = N00B_ELF_REWRITE_PLAN_REJECTED;
    plan->rejection_reason       = reason;
    plan->target_profile         = profile;
    plan->table_strategy         = N00B_ELF_REWRITE_TABLE_STRATEGY_NONE;
    plan->file_size              = profile.file_size;
    plan->original_section_count = profile.section_count;
    plan->new_section_count      = profile.section_count;
    plan->new_shstrndx           = 0;
    if (admission != nullptr) {
        plan->admission = *admission;
        plan->file_size = admission->file_size;
    }

    return n00b_result_ok(n00b_elf_rewrite_plan_t *, plan);
}

static bool
section_name_write_size(n00b_string_t *section_name, uint64_t *out)
{
    if (section_name->u8_bytes > UINT64_MAX - 1) {
        return false;
    }

    *out = (uint64_t)section_name->u8_bytes + 1;
    return true;
}

static bool
can_grow_tables_in_place(n00b_elf_binary_t                 *bin,
                         n00b_elf_layout_t                 *layout,
                         n00b_elf_rewrite_target_profile_t *profile,
                         n00b_string_t                     *section_name,
                         n00b_elf_rewrite_admit_placement_t placement)
{
    n00b_buffer_t *buf = bin->stream->buf;
    uint64_t       old_shtab_size;
    uint64_t       old_shtab_end;
    uint64_t       new_shtab_size;
    uint64_t       new_shtab_end;
    uint64_t       name_bytes;
    uint64_t       new_shstrtab_end;

    if (!section_name_write_size(section_name, &name_bytes)
        || !checked_mul_u64(profile->section_count,
                            N00B_ELF64_SHDR_SIZE,
                            &old_shtab_size)
        || !checked_mul_u64(profile->section_count + 1,
                            N00B_ELF64_SHDR_SIZE,
                            &new_shtab_size)
        || !checked_add_u64(bin->header.shoff,
                            old_shtab_size,
                            &old_shtab_end)
        || !checked_add_u64(bin->header.shoff,
                            new_shtab_size,
                            &new_shtab_end)
        || !checked_add_u64(profile->shstrtab_offset,
                            profile->shstrtab_complete_size,
                            &new_shstrtab_end)
        || !checked_add_u64(new_shstrtab_end,
                            name_bytes,
                            &new_shstrtab_end)) {
        return false;
    }

    if (new_shtab_end > profile->shstrtab_offset
        || new_shstrtab_end > buf->byte_len) {
        return false;
    }

    if (ranges_overlap(placement.file_offset,
                       placement.file_end,
                       bin->header.shoff,
                       new_shtab_end)
        || ranges_overlap(placement.file_offset,
                          placement.file_end,
                          profile->shstrtab_offset,
                          new_shstrtab_end)) {
        return false;
    }

    if (!zero_padding_gap_covers(layout, old_shtab_end, new_shtab_end)) {
        return false;
    }

    uint64_t old_shstrtab_file_end;
    if (!checked_add_u64(profile->shstrtab_offset,
                         profile->shstrtab_reported_size,
                         &old_shstrtab_file_end)
        || old_shstrtab_file_end > new_shstrtab_end) {
        return false;
    }

    return zero_padding_gap_covers(layout,
                                   old_shstrtab_file_end,
                                   new_shstrtab_end);
}

static bool
terminal_interval_allowed(n00b_elf_binary_t                 *bin,
                          n00b_elf_rewrite_target_profile_t *profile,
                          n00b_elf_layout_interval_t        *interval,
                          uint64_t                           shtab_end,
                          uint64_t                           shstrtab_end)
{
    if (interval->kind == N00B_ELF_LAYOUT_INTERVAL_SHTAB
        && interval->start == bin->header.shoff
        && interval->end == shtab_end) {
        return true;
    }

    if (interval->index != bin->header.shstrndx
        || interval->start != profile->shstrtab_offset
        || interval->end != shstrtab_end) {
        return false;
    }

    return interval->kind == N00B_ELF_LAYOUT_INTERVAL_SECTION_FILE
        || interval->kind == N00B_ELF_LAYOUT_INTERVAL_SECTION_STRING_TABLE
        || interval->kind == N00B_ELF_LAYOUT_INTERVAL_SECTION_NAME_STRING_TABLE;
}

static bool
terminal_tail_candidate(n00b_elf_binary_t                 *bin,
                        n00b_elf_layout_t                 *layout,
                        n00b_elf_rewrite_target_profile_t *profile,
                        uint64_t                           candidate,
                        uint64_t                           shtab_end,
                        uint64_t                           shstrtab_end,
                        n00b_allocator_t                  *allocator)
{
    auto overlaps = n00b_elf_layout_file_overlaps(layout,
                                                  candidate,
                                                  profile->file_size,
                                                  .allocator = allocator);
    if (n00b_result_is_err(overlaps)) {
        return false;
    }

    n00b_elf_layout_interval_list_t list = n00b_result_get(overlaps);
    bool saw_terminal_table = false;
    for (uint64_t i = 0; i < list.count; i++) {
        if (!terminal_interval_allowed(bin,
                                       profile,
                                       &list.items[i],
                                       shtab_end,
                                       shstrtab_end)) {
            return false;
        }

        saw_terminal_table = true;
    }

    if (!saw_terminal_table) {
        return false;
    }

    const uint8_t *bytes = (const uint8_t *)bin->stream->buf->data;
    for (uint64_t i = candidate; i < profile->file_size; i++) {
        if (range_contains(bin->header.shoff, shtab_end, i)
            || range_contains(profile->shstrtab_offset, shstrtab_end, i)
            || bytes[i] == 0) {
            continue;
        }

        return false;
    }

    return true;
}

static bool
terminal_tail_incision(n00b_elf_binary_t                 *bin,
                       n00b_elf_layout_t                 *layout,
                       n00b_elf_rewrite_target_profile_t *profile,
                       uint64_t                          *incision,
                       n00b_allocator_t                  *allocator)
{
    if (bin->overlay != nullptr) {
        return false;
    }

    n00b_buffer_t *buf = bin->stream->buf;
    uint64_t       shtab_size;
    uint64_t       shtab_end;
    uint64_t       shstrtab_end;

    if (!checked_mul_u64(profile->section_count,
                         N00B_ELF64_SHDR_SIZE,
                         &shtab_size)
        || !checked_add_u64(bin->header.shoff, shtab_size, &shtab_end)
        || !checked_add_u64(profile->shstrtab_offset,
                            profile->shstrtab_reported_size,
                            &shstrtab_end)
        || shtab_end > buf->byte_len
        || shstrtab_end > buf->byte_len) {
        return false;
    }

    uint64_t start = bin->header.shoff < profile->shstrtab_offset
                   ? bin->header.shoff
                   : profile->shstrtab_offset;

    if (terminal_tail_candidate(bin,
                                layout,
                                profile,
                                start,
                                shtab_end,
                                shstrtab_end,
                                allocator)) {
        *incision = start;
        return true;
    }

    uint64_t higher = bin->header.shoff > profile->shstrtab_offset
                    ? bin->header.shoff
                    : profile->shstrtab_offset;
    if (higher != start
        && terminal_tail_candidate(bin,
                                   layout,
                                   profile,
                                   higher,
                                   shtab_end,
                                   shstrtab_end,
                                   allocator)) {
        *incision = higher;
        return true;
    }

    return false;
}

static n00b_elf_rewrite_table_strategy_t
choose_table_strategy(n00b_elf_binary_t                         *bin,
                      n00b_elf_layout_t                         *layout,
                      n00b_elf_rewrite_metadata_request_t       *request,
                      n00b_elf_rewrite_target_profile_t         *profile,
                      n00b_elf_rewrite_admit_placement_t         placement,
                      uint64_t                                  *incision,
                      n00b_allocator_t                          *allocator)
{
    n00b_elf_rewrite_admit_placement_kind_t placement_kind = placement.kind;

    if (placement_kind == N00B_ELF_REWRITE_ADMIT_PLACEMENT_AFTER_OVERLAY) {
        return N00B_ELF_REWRITE_TABLE_STRATEGY_AFTER_OVERLAY_REPLACEMENT;
    }

    if (can_grow_tables_in_place(bin,
                                 layout,
                                 profile,
                                 request->section_name,
                                 placement)) {
        return N00B_ELF_REWRITE_TABLE_STRATEGY_IN_PLACE_SHSTRTAB_GROWTH;
    }

    if (bin->overlay != nullptr
        && policy_allows_append_after_overlay(request->policy)) {
        return N00B_ELF_REWRITE_TABLE_STRATEGY_AFTER_OVERLAY_REPLACEMENT;
    }

    if (placement_kind == N00B_ELF_REWRITE_ADMIT_PLACEMENT_EOF_TAIL
        && terminal_tail_incision(bin, layout, profile, incision, allocator)) {
        return N00B_ELF_REWRITE_TABLE_STRATEGY_TERMINAL_TAIL_INCISION;
    }

    return N00B_ELF_REWRITE_TABLE_STRATEGY_EOF_REPLACEMENT;
}

static bool
append_patch(n00b_elf_rewrite_patch_t *patches,
             uint64_t                  capacity,
             uint64_t                 *count,
             n00b_elf_rewrite_patch_t  patch)
{
    if (patch.file_offset > patch.file_end) {
        return false;
    }

    if (*count == capacity) {
        return false;
    }

    uint64_t insert_at = 0;
    while (insert_at < *count
           && patches[insert_at].file_offset <= patch.file_offset) {
        insert_at++;
    }

    if (insert_at != 0 && patches[insert_at - 1].file_end > patch.file_offset) {
        return false;
    }

    if (insert_at != *count && patch.file_end > patches[insert_at].file_offset) {
        return false;
    }

    for (uint64_t i = *count; i > insert_at; i--) {
        patches[i] = patches[i - 1];
    }

    patches[insert_at] = patch;
    *count += 1;
    return true;
}

static bool
table_replacement_start(n00b_elf_binary_t                         *bin,
                        n00b_elf_rewrite_metadata_request_t       *request,
                        n00b_elf_rewrite_target_profile_t         *profile,
                        n00b_elf_rewrite_table_strategy_t          strategy,
                        n00b_elf_rewrite_admit_placement_t         placement,
                        uint64_t                                  *start)
{
    switch (strategy) {
    case N00B_ELF_REWRITE_TABLE_STRATEGY_TERMINAL_TAIL_INCISION:
        *start = placement.file_end;
        return placement.kind == N00B_ELF_REWRITE_ADMIT_PLACEMENT_EOF_TAIL;
    case N00B_ELF_REWRITE_TABLE_STRATEGY_EOF_REPLACEMENT:
        if (placement.kind == N00B_ELF_REWRITE_ADMIT_PLACEMENT_EOF_TAIL) {
            *start = placement.file_end;
            return true;
        }

        if (bin->overlay != nullptr
            && !policy_allows_append_after_overlay(request->policy)) {
            return false;
        }

        *start = profile->file_size;
        return true;
    case N00B_ELF_REWRITE_TABLE_STRATEGY_AFTER_OVERLAY_REPLACEMENT:
        if (placement.kind == N00B_ELF_REWRITE_ADMIT_PLACEMENT_AFTER_OVERLAY) {
            *start = placement.file_end;
            return true;
        }

        if (bin->overlay != nullptr
            && policy_allows_append_after_overlay(request->policy)) {
            *start = profile->file_size;
            return true;
        }

        return false;
    case N00B_ELF_REWRITE_TABLE_STRATEGY_IN_PLACE_SHSTRTAB_GROWTH:
    case N00B_ELF_REWRITE_TABLE_STRATEGY_NONE:
        return false;
    }

    return false;
}

static bool
append_table_replacement_patch(n00b_elf_rewrite_patch_t       *patches,
                               uint64_t                       *count,
                               uint64_t                        start,
                               n00b_elf_rewrite_target_profile_t *profile,
                               n00b_string_t                   *section_name)
{
    uint64_t name_bytes;
    uint64_t new_strtab_size;
    uint64_t new_shtab_size;
    uint64_t table_bytes;
    uint64_t end;

    if (!section_name_write_size(section_name, &name_bytes)
        || !checked_add_u64(profile->shstrtab_complete_size,
                            name_bytes,
                            &new_strtab_size)
        || !checked_mul_u64(profile->section_count + 1,
                            N00B_ELF64_SHDR_SIZE,
                            &new_shtab_size)
        || !checked_add_u64(new_strtab_size, new_shtab_size, &table_bytes)
        || !checked_add_u64(start, table_bytes, &end)) {
        return false;
    }

    return append_patch(
        patches,
        N00B_ELF_REWRITE_MAX_PATCHES,
        count,
        (n00b_elf_rewrite_patch_t){
            .kind                 = N00B_ELF_REWRITE_PATCH_APPENDED_TABLES,
            .file_offset          = start,
            .file_end             = end,
            .original_file_offset = start,
            .original_file_end    = start,
        });
}

static n00b_result_t(n00b_elf_rewrite_plan_t *)
accepted_plan(n00b_allocator_t                         *allocator,
              n00b_elf_binary_t                        *bin,
              n00b_elf_rewrite_metadata_request_t      *request,
              n00b_elf_rewrite_target_profile_t         profile,
              n00b_elf_rewrite_admit_result_t           admission,
              n00b_elf_rewrite_admit_placement_t        placement)
{
    auto layout_result = n00b_elf_layout_build(bin, .allocator = allocator);
    if (n00b_result_is_err(layout_result)) {
        n00b_err_t err = n00b_result_get_err(layout_result);
        return n00b_result_err(
            n00b_elf_rewrite_plan_t *,
            err == N00B_ELF_LAYOUT_ERR_OVERFLOW
                ? N00B_ELF_REWRITE_ERR_OVERFLOW
                : N00B_ELF_REWRITE_ERR_ADMISSION);
    }

    n00b_elf_layout_t *layout = n00b_result_get(layout_result);
    uint64_t incision = 0;
    n00b_elf_rewrite_table_strategy_t strategy =
        choose_table_strategy(bin,
                              layout,
                              request,
                              &profile,
                              placement,
                              &incision,
                              allocator);

    n00b_elf_rewrite_patch_t local[N00B_ELF_REWRITE_MAX_PATCHES] = {};
    uint64_t                 count    = 0;

    if (!append_patch(local,
                      N00B_ELF_REWRITE_MAX_PATCHES,
                      &count,
                      (n00b_elf_rewrite_patch_t){
                          .kind                 = N00B_ELF_REWRITE_PATCH_ELF_HEADER,
                          .file_offset          = 0,
                          .file_end             = N00B_ELF64_EHDR_SIZE,
                          .original_file_offset = 0,
                          .original_file_end    = N00B_ELF64_EHDR_SIZE,
                      })) {
        return n00b_result_err(n00b_elf_rewrite_plan_t *,
                               N00B_ELF_REWRITE_ERR_OVERFLOW);
    }

    switch (strategy) {
    case N00B_ELF_REWRITE_TABLE_STRATEGY_IN_PLACE_SHSTRTAB_GROWTH: {
        uint64_t old_shtab_size;
        uint64_t new_shtab_size;
        uint64_t new_shtab_end;
        uint64_t name_bytes;
        uint64_t new_shstrtab_size;
        uint64_t new_shstrtab_end;
        if (!checked_mul_u64(profile.section_count,
                             N00B_ELF64_SHDR_SIZE,
                             &old_shtab_size)
            || !checked_mul_u64(profile.section_count + 1,
                                N00B_ELF64_SHDR_SIZE,
                                &new_shtab_size)
            || !section_name_write_size(request->section_name, &name_bytes)
            || !checked_add_u64(profile.shstrtab_complete_size,
                                name_bytes,
                                &new_shstrtab_size)
            || !checked_add_u64(bin->header.shoff,
                                new_shtab_size,
                                &new_shtab_end)
            || !checked_add_u64(profile.shstrtab_offset,
                                new_shstrtab_size,
                                &new_shstrtab_end)
            || !append_patch(
                local,
                N00B_ELF_REWRITE_MAX_PATCHES,
                &count,
                (n00b_elf_rewrite_patch_t){
                    .kind                 = N00B_ELF_REWRITE_PATCH_SECTION_HEADER_TABLE,
                    .file_offset          = bin->header.shoff,
                    .file_end             = new_shtab_end,
                    .original_file_offset = bin->header.shoff,
                    .original_file_end    = bin->header.shoff
                                       + old_shtab_size,
                })
            || !append_patch(
                local,
                N00B_ELF_REWRITE_MAX_PATCHES,
                &count,
                (n00b_elf_rewrite_patch_t){
                    .kind                 = N00B_ELF_REWRITE_PATCH_SECTION_NAME_STRTAB,
                    .file_offset          = profile.shstrtab_offset,
                    .file_end             = new_shstrtab_end,
                    .original_file_offset = profile.shstrtab_offset,
                    .original_file_end    = profile.shstrtab_offset
                                       + profile.shstrtab_reported_size,
                })) {
            return n00b_result_err(n00b_elf_rewrite_plan_t *,
                                   N00B_ELF_REWRITE_ERR_OVERFLOW);
        }
        break;
    }
    case N00B_ELF_REWRITE_TABLE_STRATEGY_TERMINAL_TAIL_INCISION:
        if (!append_patch(
            local,
            N00B_ELF_REWRITE_MAX_PATCHES,
            &count,
            (n00b_elf_rewrite_patch_t){
                .kind                 = N00B_ELF_REWRITE_PATCH_TABLE_TAIL,
                .file_offset          = incision,
                .file_end             = profile.file_size,
                .original_file_offset = incision,
                .original_file_end    = profile.file_size,
            })) {
            return n00b_result_err(n00b_elf_rewrite_plan_t *,
                                   N00B_ELF_REWRITE_ERR_OVERFLOW);
        }
        break;
    case N00B_ELF_REWRITE_TABLE_STRATEGY_EOF_REPLACEMENT:
    case N00B_ELF_REWRITE_TABLE_STRATEGY_AFTER_OVERLAY_REPLACEMENT:
    case N00B_ELF_REWRITE_TABLE_STRATEGY_NONE:
        break;
    }

    uint64_t payload_original_end = placement.kind
                                  == N00B_ELF_REWRITE_ADMIT_PLACEMENT_FILE_GAP
                                    ? placement.file_end
                                    : placement.file_offset;
    if (!append_patch(local,
                      N00B_ELF_REWRITE_MAX_PATCHES,
                      &count,
                      (n00b_elf_rewrite_patch_t){
                          .kind                 = N00B_ELF_REWRITE_PATCH_PAYLOAD,
                          .file_offset          = placement.file_offset,
                          .file_end             = placement.file_end,
                          .original_file_offset = placement.file_offset,
                          .original_file_end    = payload_original_end,
                      })) {
        return n00b_result_err(n00b_elf_rewrite_plan_t *,
                               N00B_ELF_REWRITE_ERR_OVERFLOW);
    }

    if (strategy == N00B_ELF_REWRITE_TABLE_STRATEGY_EOF_REPLACEMENT
        || strategy == N00B_ELF_REWRITE_TABLE_STRATEGY_AFTER_OVERLAY_REPLACEMENT
        || strategy == N00B_ELF_REWRITE_TABLE_STRATEGY_TERMINAL_TAIL_INCISION) {
        uint64_t tables_start;
        if (!table_replacement_start(bin,
                                     request,
                                     &profile,
                                     strategy,
                                     placement,
                                     &tables_start)) {
            return rejected_plan(allocator,
                                 N00B_ELF_REWRITE_REJECT_TABLE_PLACEMENT,
                                 profile,
                                 &admission);
        }

        if (!append_table_replacement_patch(local,
                                            &count,
                                            tables_start,
                                            &profile,
                                            request->section_name)) {
            return n00b_result_err(n00b_elf_rewrite_plan_t *,
                                   N00B_ELF_REWRITE_ERR_OVERFLOW);
        }
    }

    n00b_array_t(n00b_elf_rewrite_patch_t) patches =
        n00b_array_new(n00b_elf_rewrite_patch_t,
                       count,
                       .allocator = allocator);
    memcpy(patches.data, local, count * sizeof(n00b_elf_rewrite_patch_t));
    patches.len = count;

    n00b_elf_rewrite_plan_t *plan = new_plan(allocator);
    plan->operation              = N00B_ELF_REWRITE_OPERATION_METADATA_INSERT;
    plan->outcome                = N00B_ELF_REWRITE_PLAN_ACCEPTED;
    plan->rejection_reason       = N00B_ELF_REWRITE_REJECT_NONE;
    plan->target_profile         = profile;
    plan->admission              = admission;
    plan->table_strategy         = strategy;
    plan->patches                = patches;
    plan->section_name           = request->section_name;
    plan->payload                = request->payload;
    plan->section_alignment      = effective_section_alignment(
        request->file_alignment);
    plan->section_type           = request->section_type;
    plan->section_flags          = request->section_flags;
    plan->file_size              = admission.file_size;
    plan->original_section_count = profile.section_count;
    plan->new_section_count      = profile.section_count + 1;
    plan->new_shstrndx           = bin->header.shstrndx;
    plan->payload_offset         = placement.file_offset;
    plan->payload_end            = placement.file_end;

    return n00b_result_ok(n00b_elf_rewrite_plan_t *, plan);
}

static n00b_result_t(n00b_elf_rewrite_plan_t *)
plan_metadata_insert_impl(
    n00b_elf_binary_t                   *bin,
    n00b_elf_rewrite_metadata_request_t *request,
    bool                                 trusted_chalk_mark,
    n00b_allocator_t                    *allocator)
{
    if (bin == nullptr) {
        return n00b_result_err(n00b_elf_rewrite_plan_t *,
                               N00B_ELF_REWRITE_ERR_NULL_BINARY);
    }

    if (request == nullptr) {
        return n00b_result_err(n00b_elf_rewrite_plan_t *,
                               N00B_ELF_REWRITE_ERR_NULL_REQUEST);
    }

    if (request->section_name == nullptr) {
        return n00b_result_err(n00b_elf_rewrite_plan_t *,
                               N00B_ELF_REWRITE_ERR_NULL_SECTION_NAME);
    }

    if (request->payload == nullptr) {
        return n00b_result_err(n00b_elf_rewrite_plan_t *,
                               N00B_ELF_REWRITE_ERR_NULL_PAYLOAD);
    }

    if (request->payload->byte_len == 0) {
        return n00b_result_err(n00b_elf_rewrite_plan_t *,
                               N00B_ELF_REWRITE_ERR_ZERO_PAYLOAD_SIZE);
    }

    auto profile_result = n00b_elf_rewrite_target_profile(bin);
    if (n00b_result_is_err(profile_result)) {
        return n00b_result_err(n00b_elf_rewrite_plan_t *,
                               n00b_result_get_err(profile_result));
    }

    n00b_elf_rewrite_target_profile_t profile =
        n00b_result_get(profile_result);
    if (profile.reason != N00B_ELF_REWRITE_PROFILE_OK) {
        return rejected_plan(allocator,
                             N00B_ELF_REWRITE_REJECT_TARGET_PROFILE,
                             profile,
                             nullptr);
    }

    if (profile.section_count + 1 >= N00B_ELF_SHN_LORESERVE) {
        return rejected_plan(allocator,
                             N00B_ELF_REWRITE_REJECT_SECTION_COUNT_PROMOTION,
                             profile,
                             nullptr);
    }

    n00b_elf_rewrite_admit_metadata_request_t admission_request = {
        .section_name          = request->section_name,
        .payload_size          = request->payload->byte_len,
        .file_alignment        = request->file_alignment,
        .section_type          = request->section_type,
        .section_flags         = request->section_flags,
        .preferred_file_offset = request->preferred_file_offset,
        .policy                = request->policy,
    };

    auto admission_result = trusted_chalk_mark
        ? n00b_elf_rewrite_admit_chalk_mark_insert(bin,
                                                   &admission_request,
                                                   .allocator = allocator)
        : n00b_elf_rewrite_admit_metadata_insert(bin,
                                                 &admission_request,
                                                 .allocator = allocator);
    if (n00b_result_is_err(admission_result)) {
        return n00b_result_err(n00b_elf_rewrite_plan_t *,
                               N00B_ELF_REWRITE_ERR_ADMISSION);
    }

    n00b_elf_rewrite_admit_result_t admission =
        n00b_result_get(admission_result);
    if (admission.outcome == N00B_ELF_REWRITE_ADMIT_OUTCOME_REJECTED) {
        return rejected_plan(allocator,
                             N00B_ELF_REWRITE_REJECT_ADMISSION,
                             profile,
                             &admission);
    }

    if (!n00b_option_is_set(admission.placement)) {
        return n00b_result_err(n00b_elf_rewrite_plan_t *,
                               N00B_ELF_REWRITE_ERR_ADMISSION);
    }

    return accepted_plan(allocator,
                         bin,
                         request,
                         profile,
                         admission,
                         n00b_option_get(admission.placement));
}

n00b_result_t(n00b_elf_rewrite_plan_t *)
n00b_elf_rewrite_plan_metadata_insert(
    n00b_elf_binary_t                   *bin,
    n00b_elf_rewrite_metadata_request_t *request) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    return plan_metadata_insert_impl(bin, request, false, allocator);
}

n00b_result_t(n00b_elf_rewrite_plan_t *)
n00b_elf_rewrite_plan_chalk_mark_insert(
    n00b_elf_binary_t                   *bin,
    n00b_elf_rewrite_metadata_request_t *request) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    return plan_metadata_insert_impl(bin, request, true, allocator);
}

static bool
request_is_nonloadable_metadata(n00b_elf_rewrite_metadata_request_t *request)
{
    if (request->section_flags != 0) {
        return false;
    }

    return request->section_type == SHT_PROGBITS
        || request->section_type == SHT_NOTE;
}

static n00b_result_t(n00b_elf_rewrite_plan_t *)
plan_chalk_mark_delete_or_replace(
    n00b_elf_binary_t                   *bin,
    n00b_elf_rewrite_metadata_request_t *request,
    bool                                 replacement,
    n00b_allocator_t                    *allocator)
{
    if (bin == nullptr || bin->stream == nullptr || bin->stream->buf == nullptr) {
        return n00b_result_err(n00b_elf_rewrite_plan_t *,
                               N00B_ELF_REWRITE_ERR_NULL_BINARY);
    }

    if (replacement) {
        if (request == nullptr) {
            return n00b_result_err(n00b_elf_rewrite_plan_t *,
                                   N00B_ELF_REWRITE_ERR_NULL_REQUEST);
        }

        if (request->section_name == nullptr) {
            return n00b_result_err(n00b_elf_rewrite_plan_t *,
                                   N00B_ELF_REWRITE_ERR_NULL_SECTION_NAME);
        }

        if (request->payload == nullptr) {
            return n00b_result_err(n00b_elf_rewrite_plan_t *,
                                   N00B_ELF_REWRITE_ERR_NULL_PAYLOAD);
        }

        if (request->payload->byte_len == 0) {
            return n00b_result_err(n00b_elf_rewrite_plan_t *,
                                   N00B_ELF_REWRITE_ERR_ZERO_PAYLOAD_SIZE);
        }
    }

    auto profile_result = n00b_elf_rewrite_target_profile(bin);
    if (n00b_result_is_err(profile_result)) {
        return n00b_result_err(n00b_elf_rewrite_plan_t *,
                               n00b_result_get_err(profile_result));
    }

    n00b_elf_rewrite_target_profile_t profile =
        n00b_result_get(profile_result);
    if (profile.reason != N00B_ELF_REWRITE_PROFILE_OK) {
        return rejected_plan(allocator,
                             N00B_ELF_REWRITE_REJECT_TARGET_PROFILE,
                             profile,
                             nullptr);
    }

    if (replacement
        && (!section_name_is_chalk_mark(request->section_name)
            || !request_is_nonloadable_metadata(request))) {
        return rejected_plan(allocator,
                             N00B_ELF_REWRITE_REJECT_TRUSTED_NAME,
                             profile,
                             nullptr);
    }

    chalk_mark_shape_t shape = {};
    if (!compute_chalk_mark_shape(bin, &profile, replacement, &shape)) {
        return rejected_plan(allocator,
                             N00B_ELF_REWRITE_REJECT_CHALK_MARK_NOT_FOUND,
                             profile,
                             nullptr);
    }

    n00b_elf_section_t *old_mark = &bin->sections[shape.target_index];
    uint64_t old_payload_end;
    if (!checked_add_u64(old_mark->offset, old_mark->size, &old_payload_end)
        || old_payload_end > profile.file_size
        || old_mark->size > (uint64_t)SIZE_MAX) {
        return rejected_plan(allocator,
                             N00B_ELF_REWRITE_REJECT_CHALK_MARK_UNSUPPORTED,
                             profile,
                             nullptr);
    }

    auto exclusive_result =
        chalk_mark_payload_range_is_exclusive(bin,
                                              old_mark,
                                              &shape,
                                              old_payload_end,
                                              allocator);
    if (n00b_result_is_err(exclusive_result)) {
        return n00b_result_err(n00b_elf_rewrite_plan_t *,
                               n00b_result_get_err(exclusive_result));
    }
    if (!n00b_result_get(exclusive_result)) {
        return rejected_plan(allocator,
                             N00B_ELF_REWRITE_REJECT_CHALK_MARK_UNSUPPORTED,
                             profile,
                             nullptr);
    }

    uint64_t new_payload_offset = 0;
    uint64_t new_payload_end    = 0;
    uint64_t tables_start       = profile.file_size;
    if (replacement) {
        new_payload_offset = profile.file_size;
        if (!checked_add_u64(new_payload_offset,
                             request->payload->byte_len,
                             &new_payload_end)) {
            return n00b_result_err(n00b_elf_rewrite_plan_t *,
                                   N00B_ELF_REWRITE_ERR_OVERFLOW);
        }
        tables_start = new_payload_end;
    }

    uint64_t tables_size;
    uint64_t tables_end;
    if (!checked_add_u64(shape.new_strtab_size,
                         shape.new_shtab_size,
                         &tables_size)
        || !checked_add_u64(tables_start, tables_size, &tables_end)) {
        return n00b_result_err(n00b_elf_rewrite_plan_t *,
                               N00B_ELF_REWRITE_ERR_OVERFLOW);
    }

    n00b_elf_rewrite_patch_t local[N00B_ELF_REWRITE_MAX_PATCHES] = {};
    uint64_t count = 0;
    bool patch_ok =
        append_patch(local,
                     N00B_ELF_REWRITE_MAX_PATCHES,
                     &count,
                     (n00b_elf_rewrite_patch_t){
                         .kind                 = N00B_ELF_REWRITE_PATCH_ELF_HEADER,
                         .file_offset          = 0,
                         .file_end             = N00B_ELF64_EHDR_SIZE,
                         .original_file_offset = 0,
                         .original_file_end    = N00B_ELF64_EHDR_SIZE,
                     })
        && append_patch(local,
                        N00B_ELF_REWRITE_MAX_PATCHES,
                        &count,
                        (n00b_elf_rewrite_patch_t){
                            .kind                 = N00B_ELF_REWRITE_PATCH_STALE_PAYLOAD,
                            .file_offset          = old_mark->offset,
                            .file_end             = old_payload_end,
                            .original_file_offset = old_mark->offset,
                            .original_file_end    = old_payload_end,
                        });

    if (replacement) {
        patch_ok =
            patch_ok
            && append_patch(local,
                            N00B_ELF_REWRITE_MAX_PATCHES,
                            &count,
                            (n00b_elf_rewrite_patch_t){
                                .kind                 = N00B_ELF_REWRITE_PATCH_PAYLOAD,
                                .file_offset          = new_payload_offset,
                                .file_end             = new_payload_end,
                                .original_file_offset = new_payload_offset,
                                .original_file_end    = new_payload_offset,
                            });
    }

    patch_ok =
        patch_ok
        && append_patch(local,
                        N00B_ELF_REWRITE_MAX_PATCHES,
                        &count,
                        (n00b_elf_rewrite_patch_t){
                            .kind                 = N00B_ELF_REWRITE_PATCH_APPENDED_TABLES,
                            .file_offset          = tables_start,
                            .file_end             = tables_end,
                            .original_file_offset = tables_start,
                            .original_file_end    = tables_start,
                        });

    if (!patch_ok) {
        return rejected_plan(allocator,
                             N00B_ELF_REWRITE_REJECT_CHALK_MARK_UNSUPPORTED,
                             profile,
                             nullptr);
    }

    n00b_array_t(n00b_elf_rewrite_patch_t) patches =
        n00b_array_new(n00b_elf_rewrite_patch_t,
                       count,
                       .allocator = allocator);
    memcpy(patches.data, local, count * sizeof(n00b_elf_rewrite_patch_t));
    patches.len = count;

    n00b_elf_rewrite_plan_t *plan = new_plan(allocator);
    plan->operation = replacement
        ? N00B_ELF_REWRITE_OPERATION_CHALK_MARK_REPLACE
        : N00B_ELF_REWRITE_OPERATION_CHALK_MARK_DELETE;
    plan->outcome                = N00B_ELF_REWRITE_PLAN_ACCEPTED;
    plan->rejection_reason       = N00B_ELF_REWRITE_REJECT_NONE;
    plan->target_profile         = profile;
    plan->table_strategy         = N00B_ELF_REWRITE_TABLE_STRATEGY_EOF_REPLACEMENT;
    plan->patches                = patches;
    plan->section_name           = r".chalk.mark";
    plan->payload                = replacement ? request->payload : nullptr;
    plan->section_alignment      =
        replacement ? effective_section_alignment(request->file_alignment) : 1;
    plan->section_type           = replacement ? request->section_type : 0;
    plan->section_flags          = replacement ? request->section_flags : 0;
    plan->file_size              = profile.file_size;
    plan->original_section_count = profile.section_count;
    plan->new_section_count      = profile.section_count - 1
                                 + (replacement ? 1u : 0u);
    plan->removed_section_index  = shape.target_index;
    plan->removed_payload_offset = old_mark->offset;
    plan->removed_payload_end    = old_payload_end;
    plan->new_shstrndx           = shape.new_shstrndx;
    plan->payload_offset         = new_payload_offset;
    plan->payload_end            = new_payload_end;

    return n00b_result_ok(n00b_elf_rewrite_plan_t *, plan);
}

n00b_result_t(n00b_elf_rewrite_plan_t *)
n00b_elf_rewrite_plan_chalk_mark_delete(
    n00b_elf_binary_t *bin) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    return plan_chalk_mark_delete_or_replace(bin, nullptr, false, allocator);
}

n00b_result_t(n00b_elf_rewrite_plan_t *)
n00b_elf_rewrite_plan_chalk_mark_replace(
    n00b_elf_binary_t                   *bin,
    n00b_elf_rewrite_metadata_request_t *request) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    return plan_chalk_mark_delete_or_replace(bin, request, true, allocator);
}

static n00b_elf_rewrite_patch_t *
find_plan_patch(n00b_elf_rewrite_plan_t *plan,
                n00b_elf_rewrite_patch_kind_t kind)
{
    for (uint64_t i = 0; i < plan->patches.len; i++) {
        if (plan->patches.data[i].kind == kind) {
            return &plan->patches.data[i];
        }
    }

    return nullptr;
}

static bool
new_strtab_size_and_name_index(n00b_elf_rewrite_plan_t *plan,
                               uint64_t                *size_out,
                               uint32_t                *name_index_out)
{
    uint64_t name_bytes;
    uint64_t new_size;

    if (plan->target_profile.shstrtab_complete_size > UINT32_MAX
        || !section_name_write_size(plan->section_name, &name_bytes)
        || !checked_add_u64(plan->target_profile.shstrtab_complete_size,
                            name_bytes,
                            &new_size)) {
        return false;
    }

    *size_out       = new_size;
    *name_index_out =
        (uint32_t)plan->target_profile.shstrtab_complete_size;
    return true;
}

static n00b_buffer_t *
build_rewrite_shstrtab(n00b_elf_binary_t       *bin,
                       n00b_elf_rewrite_plan_t *plan,
                       n00b_allocator_t        *allocator)
{
    uint64_t new_size;
    uint32_t name_index;

    if (!new_strtab_size_and_name_index(plan, &new_size, &name_index)) {
        return nullptr;
    }

    uint64_t original_end;
    if (!checked_add_u64(plan->target_profile.shstrtab_offset,
                         plan->target_profile.shstrtab_reported_size,
                         &original_end)
        || original_end > bin->stream->buf->byte_len) {
        return nullptr;
    }

    n00b_buffer_t *strtab = new_zero_buffer(new_size, allocator);
    if (strtab == nullptr) {
        return nullptr;
    }

    memcpy(strtab->data,
           bin->stream->buf->data + plan->target_profile.shstrtab_offset,
           (size_t)plan->target_profile.shstrtab_reported_size);

    if (plan->target_profile.shstrtab_requires_terminator) {
        strtab->data[plan->target_profile.shstrtab_reported_size] = '\0';
    }

    memcpy(strtab->data + name_index,
           plan->section_name->data,
           plan->section_name->u8_bytes);
    strtab->data[name_index + plan->section_name->u8_bytes] = '\0';
    return strtab;
}

static n00b_buffer_t *
build_rewrite_shtab(n00b_elf_binary_t       *bin,
                    n00b_elf_rewrite_plan_t *plan,
                    uint64_t                 shstrtab_offset,
                    uint64_t                 shstrtab_size,
                    uint32_t                 name_index,
                    n00b_allocator_t        *allocator)
{
    uint64_t old_shtab_size;
    uint64_t new_shtab_size;
    uint64_t old_shtab_end;
    bool     big = is_big_endian(bin);

    if (plan->payload_end < plan->payload_offset
        || (uint64_t)plan->payload->byte_len
               != plan->payload_end - plan->payload_offset
        || plan->new_section_count != plan->original_section_count + 1
        || !checked_mul_u64(plan->original_section_count,
                            N00B_ELF64_SHDR_SIZE,
                            &old_shtab_size)
        || !checked_mul_u64(plan->new_section_count,
                            N00B_ELF64_SHDR_SIZE,
                            &new_shtab_size)
        || !checked_add_u64(bin->header.shoff,
                            old_shtab_size,
                            &old_shtab_end)
        || old_shtab_end > bin->stream->buf->byte_len) {
        return nullptr;
    }

    n00b_buffer_t *shtab = new_zero_buffer(new_shtab_size, allocator);
    if (shtab == nullptr) {
        return nullptr;
    }

    memcpy(shtab->data,
           bin->stream->buf->data + bin->header.shoff,
           (size_t)old_shtab_size);

    uint64_t shstrtab_header;
    if (!checked_mul_u64(bin->header.shstrndx,
                         N00B_ELF64_SHDR_SIZE,
                         &shstrtab_header)
        || shstrtab_header > old_shtab_size
        || old_shtab_size - shstrtab_header < N00B_ELF64_SHDR_SIZE) {
        return nullptr;
    }

    uint8_t *shstrtab_shdr = (uint8_t *)shtab->data + shstrtab_header;
    write_u64(shstrtab_shdr + 24, shstrtab_offset, big);
    write_u64(shstrtab_shdr + 32, shstrtab_size, big);

    uint8_t *new_shdr = (uint8_t *)shtab->data + old_shtab_size;
    write_u32(new_shdr + 0, name_index, big);
    write_u32(new_shdr + 4, plan->section_type, big);
    write_u64(new_shdr + 8, plan->section_flags, big);
    write_u64(new_shdr + 16, 0, big);
    write_u64(new_shdr + 24, plan->payload_offset, big);
    write_u64(new_shdr + 32, plan->payload->byte_len, big);
    write_u32(new_shdr + 40, 0, big);
    write_u32(new_shdr + 44, 0, big);
    write_u64(new_shdr + 48, effective_section_alignment(
                                  plan->section_alignment),
              big);
    write_u64(new_shdr + 56, 0, big);

    return shtab;
}

static void
write_raw_shdr(uint8_t *p, raw_shdr_t *shdr, bool big)
{
    write_u32(p + 0, shdr->name, big);
    write_u32(p + 4, shdr->type, big);
    write_u64(p + 8, shdr->flags, big);
    write_u64(p + 16, shdr->addr, big);
    write_u64(p + 24, shdr->offset, big);
    write_u64(p + 32, shdr->size, big);
    write_u32(p + 40, shdr->link, big);
    write_u32(p + 44, shdr->info, big);
    write_u64(p + 48, shdr->addralign, big);
    write_u64(p + 56, shdr->entsize, big);
}

static n00b_buffer_t *
build_chalk_mark_shstrtab(n00b_elf_binary_t       *bin,
                          n00b_elf_rewrite_plan_t *plan,
                          chalk_mark_shape_t      *shape,
                          n00b_allocator_t        *allocator)
{
    n00b_buffer_t *old_strtab =
        new_zero_buffer(plan->target_profile.shstrtab_complete_size,
                        allocator);
    n00b_buffer_t *new_strtab =
        new_zero_buffer(shape->new_strtab_size, allocator);

    if (old_strtab == nullptr || new_strtab == nullptr) {
        return nullptr;
    }

    uint64_t raw_end;
    if (!checked_add_u64(plan->target_profile.shstrtab_offset,
                         plan->target_profile.shstrtab_reported_size,
                         &raw_end)
        || raw_end > bin->stream->buf->byte_len) {
        return nullptr;
    }

    memcpy(old_strtab->data,
           bin->stream->buf->data + plan->target_profile.shstrtab_offset,
           (size_t)plan->target_profile.shstrtab_reported_size);

    if (plan->target_profile.shstrtab_requires_terminator) {
        old_strtab->data[plan->target_profile.shstrtab_reported_size] = '\0';
    }

    memcpy(new_strtab->data,
           old_strtab->data,
           (size_t)shape->removed_name_start);
    memcpy(new_strtab->data + shape->removed_name_start,
           old_strtab->data + shape->removed_name_end,
           (size_t)(plan->target_profile.shstrtab_complete_size
                    - shape->removed_name_end));

    if (plan->operation == N00B_ELF_REWRITE_OPERATION_CHALK_MARK_REPLACE) {
        memcpy(new_strtab->data + shape->replacement_name_index,
               plan->section_name->data,
               plan->section_name->u8_bytes);
        new_strtab->data[shape->replacement_name_index
                         + plan->section_name->u8_bytes] = '\0';
    }

    return new_strtab;
}

static n00b_buffer_t *
build_chalk_mark_shtab(n00b_elf_binary_t       *bin,
                       n00b_elf_rewrite_plan_t *plan,
                       chalk_mark_shape_t      *shape,
                       uint64_t                 shstrtab_offset,
                       uint64_t                 shstrtab_size,
                       n00b_allocator_t        *allocator)
{
    n00b_buffer_t *shtab = new_zero_buffer(shape->new_shtab_size, allocator);
    if (shtab == nullptr) {
        return nullptr;
    }

    bool     big = is_big_endian(bin);
    uint64_t out_index = 0;
    for (uint64_t i = 0; i < plan->original_section_count; i++) {
        if (i == shape->target_index) {
            continue;
        }

        raw_shdr_t shdr;
        if (!read_raw_shdr(bin, i, &shdr)
            || !name_index_survives_removed_span(shdr.name, shape)) {
            return nullptr;
        }

        shdr.name = adjusted_name_index(shdr.name, shape);
        if (i == bin->header.shstrndx) {
            shdr.offset = shstrtab_offset;
            shdr.size   = shstrtab_size;
        }

        write_raw_shdr((uint8_t *)shtab->data + out_index * N00B_ELF64_SHDR_SIZE,
                       &shdr,
                       big);
        out_index++;
    }

    if (plan->operation == N00B_ELF_REWRITE_OPERATION_CHALK_MARK_REPLACE) {
        raw_shdr_t shdr = {
            .name      = shape->replacement_name_index,
            .type      = plan->section_type,
            .flags     = plan->section_flags,
            .addr      = 0,
            .offset    = plan->payload_offset,
            .size      = plan->payload->byte_len,
            .link      = 0,
            .info      = 0,
            .addralign = effective_section_alignment(plan->section_alignment),
            .entsize   = 0,
        };
        write_raw_shdr((uint8_t *)shtab->data + out_index * N00B_ELF64_SHDR_SIZE,
                       &shdr,
                       big);
        out_index++;
    }

    if (out_index != plan->new_section_count) {
        return nullptr;
    }

    return shtab;
}

static bool
plan_output_size(n00b_elf_binary_t       *bin,
                 n00b_elf_rewrite_plan_t *plan,
                 uint64_t                *size_out)
{
    uint64_t output_size = bin->stream->buf->byte_len;

    for (uint64_t i = 0; i < plan->patches.len; i++) {
        n00b_elf_rewrite_patch_t *patch = &plan->patches.data[i];
        if (patch->file_offset > patch->file_end) {
            return false;
        }

        if (patch->file_end > output_size) {
            output_size = patch->file_end;
        }
    }

    *size_out = output_size;
    return true;
}

static bool
write_output_bytes(n00b_buffer_t *out,
                   uint64_t       offset,
                   const void    *bytes,
                   uint64_t       len)
{
    uint64_t end;

    if (len == 0) {
        return true;
    }

    if (!checked_add_u64(offset, len, &end)
        || end > out->byte_len
        || offset > (uint64_t)SIZE_MAX
        || len > (uint64_t)SIZE_MAX) {
        return false;
    }

    memcpy(out->data + offset, bytes, (size_t)len);
    return true;
}

static bool
zero_output_range(n00b_buffer_t *out, uint64_t offset, uint64_t end)
{
    if (offset > end || end > out->byte_len
        || offset > (uint64_t)SIZE_MAX
        || end - offset > (uint64_t)SIZE_MAX) {
        return false;
    }

    memset(out->data + offset, 0, (size_t)(end - offset));
    return true;
}

static bool
patch_output_header(n00b_buffer_t           *out,
                    n00b_elf_binary_t       *bin,
                    n00b_elf_rewrite_plan_t *plan,
                    uint64_t                 shtab_offset)
{
    if (out->byte_len < N00B_ELF64_EHDR_SIZE
        || plan->new_section_count > UINT16_MAX
        || plan->new_shstrndx > UINT16_MAX) {
        return false;
    }

    bool     big = is_big_endian(bin);
    uint8_t *p   = (uint8_t *)out->data;

    write_u64(p + 40, shtab_offset, big);
    write_u16(p + 60, (uint16_t)plan->new_section_count, big);
    write_u16(p + 62, plan->new_shstrndx, big);
    return true;
}

static bool
compute_live_table_offsets(n00b_elf_rewrite_plan_t *plan,
                           uint64_t                 strtab_size,
                           uint64_t                *shstrtab_offset,
                           uint64_t                *shtab_offset)
{
    switch (plan->table_strategy) {
    case N00B_ELF_REWRITE_TABLE_STRATEGY_IN_PLACE_SHSTRTAB_GROWTH: {
        n00b_elf_rewrite_patch_t *strtab_patch =
            find_plan_patch(plan, N00B_ELF_REWRITE_PATCH_SECTION_NAME_STRTAB);
        n00b_elf_rewrite_patch_t *shtab_patch =
            find_plan_patch(plan, N00B_ELF_REWRITE_PATCH_SECTION_HEADER_TABLE);

        if (strtab_patch == nullptr || shtab_patch == nullptr) {
            return false;
        }

        *shstrtab_offset = strtab_patch->file_offset;
        *shtab_offset    = shtab_patch->file_offset;
        return true;
    }
    case N00B_ELF_REWRITE_TABLE_STRATEGY_TERMINAL_TAIL_INCISION:
    case N00B_ELF_REWRITE_TABLE_STRATEGY_EOF_REPLACEMENT:
    case N00B_ELF_REWRITE_TABLE_STRATEGY_AFTER_OVERLAY_REPLACEMENT: {
        n00b_elf_rewrite_patch_t *tables =
            find_plan_patch(plan, N00B_ELF_REWRITE_PATCH_APPENDED_TABLES);

        if (tables == nullptr
            || !checked_add_u64(tables->file_offset,
                                strtab_size,
                                shtab_offset)) {
            return false;
        }

        *shstrtab_offset = tables->file_offset;
        return true;
    }
    case N00B_ELF_REWRITE_TABLE_STRATEGY_NONE:
        return false;
    }

    return false;
}

static n00b_buffer_t *
combine_replacement_tables(n00b_buffer_t    *strtab,
                           n00b_buffer_t    *shtab,
                           n00b_allocator_t *allocator)
{
    uint64_t size;

    if (!checked_add_u64(strtab->byte_len, shtab->byte_len, &size)) {
        return nullptr;
    }

    n00b_buffer_t *tables = new_zero_buffer(size, allocator);
    if (tables == nullptr) {
        return nullptr;
    }

    memcpy(tables->data, strtab->data, strtab->byte_len);
    memcpy(tables->data + strtab->byte_len, shtab->data, shtab->byte_len);
    return tables;
}

static bool
apply_supports_placement_kind(n00b_elf_rewrite_admit_placement_kind_t kind)
{
    switch (kind) {
    case N00B_ELF_REWRITE_ADMIT_PLACEMENT_EOF_TAIL:
    case N00B_ELF_REWRITE_ADMIT_PLACEMENT_FILE_GAP:
    case N00B_ELF_REWRITE_ADMIT_PLACEMENT_AFTER_OVERLAY:
        return true;
    case N00B_ELF_REWRITE_ADMIT_PLACEMENT_NONE:
        return false;
    }

    return false;
}

static bool
apply_supports_table_strategy(n00b_elf_rewrite_table_strategy_t strategy)
{
    switch (strategy) {
    case N00B_ELF_REWRITE_TABLE_STRATEGY_IN_PLACE_SHSTRTAB_GROWTH:
    case N00B_ELF_REWRITE_TABLE_STRATEGY_TERMINAL_TAIL_INCISION:
    case N00B_ELF_REWRITE_TABLE_STRATEGY_EOF_REPLACEMENT:
    case N00B_ELF_REWRITE_TABLE_STRATEGY_AFTER_OVERLAY_REPLACEMENT:
        return true;
    case N00B_ELF_REWRITE_TABLE_STRATEGY_NONE:
        return false;
    }

    return false;
}

static bool
apply_supports_overlay_shape(n00b_elf_binary_t                  *bin,
                             n00b_elf_rewrite_plan_t            *plan,
                             n00b_elf_rewrite_admit_placement_t  placement)
{
    bool append_after_overlay =
        policy_allows_append_after_overlay(plan->admission.policy);

    if (placement.kind == N00B_ELF_REWRITE_ADMIT_PLACEMENT_AFTER_OVERLAY) {
        return bin->overlay != nullptr
            && append_after_overlay
            && plan->table_strategy
                   == N00B_ELF_REWRITE_TABLE_STRATEGY_AFTER_OVERLAY_REPLACEMENT;
    }

    if (bin->overlay == nullptr) {
        return plan->table_strategy
            != N00B_ELF_REWRITE_TABLE_STRATEGY_AFTER_OVERLAY_REPLACEMENT;
    }

    return append_after_overlay
        && plan->table_strategy
               == N00B_ELF_REWRITE_TABLE_STRATEGY_AFTER_OVERLAY_REPLACEMENT;
}

n00b_result_t(n00b_buffer_t *)
n00b_elf_rewrite_apply_metadata_insert_plan(
    n00b_elf_binary_t       *bin,
    n00b_elf_rewrite_plan_t *plan) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (bin == nullptr || bin->stream == nullptr || bin->stream->buf == nullptr) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ELF_REWRITE_ERR_NULL_BINARY);
    }

    if (plan == nullptr) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ELF_REWRITE_ERR_NULL_PLAN);
    }

    if (plan->outcome != N00B_ELF_REWRITE_PLAN_ACCEPTED) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ELF_REWRITE_ERR_PLAN_REJECTED);
    }

    if (plan->section_name == nullptr
        || plan->payload == nullptr
        || plan->patches.data == nullptr
        || plan->patches.len == 0
        || plan->operation != N00B_ELF_REWRITE_OPERATION_METADATA_INSERT
        || plan->admission.outcome != N00B_ELF_REWRITE_ADMIT_OUTCOME_ACCEPTED
        || !n00b_option_is_set(plan->admission.placement)) {
        return n00b_result_err(n00b_buffer_t *, N00B_ELF_REWRITE_ERR_APPLY);
    }

    n00b_elf_rewrite_admit_placement_t placement =
        n00b_option_get(plan->admission.placement);
    if (placement.file_offset != plan->payload_offset
        || placement.file_end != plan->payload_end) {
        return n00b_result_err(n00b_buffer_t *, N00B_ELF_REWRITE_ERR_APPLY);
    }

    if (!apply_supports_placement_kind(placement.kind)
        || !apply_supports_table_strategy(plan->table_strategy)
        || !apply_supports_overlay_shape(bin, plan, placement)) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ELF_REWRITE_ERR_UNSUPPORTED_PLAN);
    }

    uint64_t strtab_size;
    uint32_t name_index;
    if (!new_strtab_size_and_name_index(plan, &strtab_size, &name_index)) {
        return n00b_result_err(n00b_buffer_t *, N00B_ELF_REWRITE_ERR_OVERFLOW);
    }

    uint64_t shstrtab_offset;
    uint64_t shtab_offset;
    if (!compute_live_table_offsets(plan,
                                    strtab_size,
                                    &shstrtab_offset,
                                    &shtab_offset)) {
        return n00b_result_err(n00b_buffer_t *, N00B_ELF_REWRITE_ERR_APPLY);
    }

    n00b_buffer_t *strtab = build_rewrite_shstrtab(bin, plan, allocator);
    if (strtab == nullptr) {
        return n00b_result_err(n00b_buffer_t *, N00B_ELF_REWRITE_ERR_APPLY);
    }

    n00b_buffer_t *shtab = build_rewrite_shtab(bin,
                                               plan,
                                               shstrtab_offset,
                                               strtab_size,
                                               name_index,
                                               allocator);
    if (shtab == nullptr) {
        return n00b_result_err(n00b_buffer_t *, N00B_ELF_REWRITE_ERR_APPLY);
    }

    uint64_t output_size;
    if (!plan_output_size(bin, plan, &output_size)) {
        return n00b_result_err(n00b_buffer_t *, N00B_ELF_REWRITE_ERR_APPLY);
    }

    n00b_buffer_t *out = new_zero_buffer(output_size, allocator);
    if (out == nullptr) {
        return n00b_result_err(n00b_buffer_t *, N00B_ELF_REWRITE_ERR_OVERFLOW);
    }

    memcpy(out->data, bin->stream->buf->data, bin->stream->buf->byte_len);

    n00b_elf_rewrite_patch_t *tail =
        find_plan_patch(plan, N00B_ELF_REWRITE_PATCH_TABLE_TAIL);
    if (tail != nullptr && !zero_output_range(out, tail->file_offset, tail->file_end)) {
        return n00b_result_err(n00b_buffer_t *, N00B_ELF_REWRITE_ERR_APPLY);
    }

    n00b_elf_rewrite_patch_t *payload =
        find_plan_patch(plan, N00B_ELF_REWRITE_PATCH_PAYLOAD);
    n00b_elf_rewrite_patch_t *header =
        find_plan_patch(plan, N00B_ELF_REWRITE_PATCH_ELF_HEADER);
    if (payload == nullptr || header == nullptr) {
        return n00b_result_err(n00b_buffer_t *, N00B_ELF_REWRITE_ERR_APPLY);
    }

    uint64_t expected_payload_original_end = placement.kind
        == N00B_ELF_REWRITE_ADMIT_PLACEMENT_FILE_GAP
            ? payload->file_end
            : payload->file_offset;
    if (header->file_offset != 0
        || header->file_end != N00B_ELF64_EHDR_SIZE
        || payload->file_offset != plan->payload_offset
        || payload->file_end != plan->payload_end
        || payload->original_file_offset != payload->file_offset
        || payload->original_file_end != expected_payload_original_end
        || !write_output_bytes(out,
                               payload->file_offset,
                               plan->payload->data,
                               plan->payload->byte_len)
        || !patch_output_header(out, bin, plan, shtab_offset)) {
        return n00b_result_err(n00b_buffer_t *, N00B_ELF_REWRITE_ERR_APPLY);
    }

    if (plan->table_strategy
        == N00B_ELF_REWRITE_TABLE_STRATEGY_IN_PLACE_SHSTRTAB_GROWTH) {
        n00b_elf_rewrite_patch_t *strtab_patch =
            find_plan_patch(plan, N00B_ELF_REWRITE_PATCH_SECTION_NAME_STRTAB);
        n00b_elf_rewrite_patch_t *shtab_patch =
            find_plan_patch(plan, N00B_ELF_REWRITE_PATCH_SECTION_HEADER_TABLE);

        if (strtab_patch == nullptr
            || shtab_patch == nullptr
            || strtab_patch->file_end != strtab_patch->file_offset
                                      + strtab->byte_len
            || shtab_patch->file_end != shtab_patch->file_offset
                                    + shtab->byte_len
            || !write_output_bytes(out,
                                   shtab_patch->file_offset,
                                   shtab->data,
                                   shtab->byte_len)
            || !write_output_bytes(out,
                                   strtab_patch->file_offset,
                                   strtab->data,
                                   strtab->byte_len)) {
            return n00b_result_err(n00b_buffer_t *,
                                   N00B_ELF_REWRITE_ERR_APPLY);
        }
    }
    else {
        n00b_elf_rewrite_patch_t *tables_patch =
            find_plan_patch(plan, N00B_ELF_REWRITE_PATCH_APPENDED_TABLES);
        n00b_buffer_t *tables =
            combine_replacement_tables(strtab, shtab, allocator);

        if (tables_patch == nullptr
            || tables == nullptr
            || tables_patch->file_end != tables_patch->file_offset
                                       + tables->byte_len
            || !write_output_bytes(out,
                                   tables_patch->file_offset,
                                   tables->data,
                                   tables->byte_len)) {
            return n00b_result_err(n00b_buffer_t *,
                                   N00B_ELF_REWRITE_ERR_APPLY);
        }
    }

    n00b_bstream_t *stream = n00b_bstream_new(out);
    auto            parsed = n00b_elf_parse(stream);
    if (n00b_result_is_err(parsed)) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ELF_REWRITE_ERR_PARSE_AFTER_APPLY);
    }

    return n00b_result_ok(n00b_buffer_t *, out);
}

n00b_result_t(n00b_buffer_t *)
n00b_elf_rewrite_apply_chalk_mark_plan(
    n00b_elf_binary_t       *bin,
    n00b_elf_rewrite_plan_t *plan) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (bin == nullptr || bin->stream == nullptr || bin->stream->buf == nullptr) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ELF_REWRITE_ERR_NULL_BINARY);
    }

    if (plan == nullptr) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ELF_REWRITE_ERR_NULL_PLAN);
    }

    if (plan->outcome != N00B_ELF_REWRITE_PLAN_ACCEPTED) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ELF_REWRITE_ERR_PLAN_REJECTED);
    }

    bool replacement =
        plan->operation == N00B_ELF_REWRITE_OPERATION_CHALK_MARK_REPLACE;
    if (plan->operation != N00B_ELF_REWRITE_OPERATION_CHALK_MARK_DELETE
        && !replacement) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ELF_REWRITE_ERR_UNSUPPORTED_PLAN);
    }

    if (plan->patches.data == nullptr || plan->patches.len == 0
        || plan->section_name == nullptr
        || !section_name_is_chalk_mark(plan->section_name)
        || (replacement && plan->payload == nullptr)) {
        return n00b_result_err(n00b_buffer_t *, N00B_ELF_REWRITE_ERR_APPLY);
    }

    chalk_mark_shape_t shape = {};
    if (!compute_chalk_mark_shape(bin,
                                  &plan->target_profile,
                                  replacement,
                                  &shape)
        || shape.target_index != plan->removed_section_index
        || shape.new_shstrndx != plan->new_shstrndx
        || plan->new_section_count
               != plan->original_section_count - 1 + (replacement ? 1u : 0u)) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ELF_REWRITE_ERR_UNSUPPORTED_PLAN);
    }

    n00b_elf_rewrite_patch_t *header =
        find_plan_patch(plan, N00B_ELF_REWRITE_PATCH_ELF_HEADER);
    n00b_elf_rewrite_patch_t *stale =
        find_plan_patch(plan, N00B_ELF_REWRITE_PATCH_STALE_PAYLOAD);
    n00b_elf_rewrite_patch_t *tables_patch =
        find_plan_patch(plan, N00B_ELF_REWRITE_PATCH_APPENDED_TABLES);

    if (header == nullptr || stale == nullptr || tables_patch == nullptr
        || header->file_offset != 0
        || header->file_end != N00B_ELF64_EHDR_SIZE
        || stale->file_offset != plan->removed_payload_offset
        || stale->file_end != plan->removed_payload_end) {
        return n00b_result_err(n00b_buffer_t *, N00B_ELF_REWRITE_ERR_APPLY);
    }

    n00b_elf_rewrite_patch_t *payload =
        find_plan_patch(plan, N00B_ELF_REWRITE_PATCH_PAYLOAD);
    if (replacement
        && (payload == nullptr
            || payload->file_offset != plan->payload_offset
            || payload->file_end != plan->payload_end
            || payload->original_file_offset != payload->file_offset
            || payload->original_file_end != payload->file_offset)) {
        return n00b_result_err(n00b_buffer_t *, N00B_ELF_REWRITE_ERR_APPLY);
    }

    if (!replacement && payload != nullptr) {
        return n00b_result_err(n00b_buffer_t *, N00B_ELF_REWRITE_ERR_APPLY);
    }

    uint64_t output_size;
    if (!plan_output_size(bin, plan, &output_size)) {
        return n00b_result_err(n00b_buffer_t *, N00B_ELF_REWRITE_ERR_APPLY);
    }

    n00b_buffer_t *strtab =
        build_chalk_mark_shstrtab(bin, plan, &shape, allocator);
    if (strtab == nullptr) {
        return n00b_result_err(n00b_buffer_t *, N00B_ELF_REWRITE_ERR_APPLY);
    }

    uint64_t shstrtab_offset = tables_patch->file_offset;
    uint64_t shtab_offset;
    if (!checked_add_u64(shstrtab_offset, strtab->byte_len, &shtab_offset)) {
        return n00b_result_err(n00b_buffer_t *, N00B_ELF_REWRITE_ERR_OVERFLOW);
    }

    n00b_buffer_t *shtab = build_chalk_mark_shtab(bin,
                                                  plan,
                                                  &shape,
                                                  shstrtab_offset,
                                                  strtab->byte_len,
                                                  allocator);
    n00b_buffer_t *tables = nullptr;
    if (shtab != nullptr) {
        tables = combine_replacement_tables(strtab, shtab, allocator);
    }
    uint64_t tables_end;
    if (shtab == nullptr || tables == nullptr
        || !checked_add_u64(tables_patch->file_offset,
                            tables->byte_len,
                            &tables_end)
        || tables_patch->file_end != tables_end) {
        return n00b_result_err(n00b_buffer_t *, N00B_ELF_REWRITE_ERR_APPLY);
    }

    n00b_buffer_t *out = new_zero_buffer(output_size, allocator);
    if (out == nullptr) {
        return n00b_result_err(n00b_buffer_t *, N00B_ELF_REWRITE_ERR_OVERFLOW);
    }

    memcpy(out->data, bin->stream->buf->data, bin->stream->buf->byte_len);

    if (!zero_output_range(out, stale->file_offset, stale->file_end)
        || (replacement
            && !write_output_bytes(out,
                                   payload->file_offset,
                                   plan->payload->data,
                                   plan->payload->byte_len))
        || !patch_output_header(out, bin, plan, shtab_offset)
        || !write_output_bytes(out,
                               tables_patch->file_offset,
                               tables->data,
                               tables->byte_len)) {
        return n00b_result_err(n00b_buffer_t *, N00B_ELF_REWRITE_ERR_APPLY);
    }

    n00b_bstream_t *stream = n00b_bstream_new(out);
    auto            parsed = n00b_elf_parse(stream);
    if (n00b_result_is_err(parsed)) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ELF_REWRITE_ERR_PARSE_AFTER_APPLY);
    }

    return n00b_result_ok(n00b_buffer_t *, out);
}

n00b_result_t(n00b_buffer_t *)
n00b_elf_rewrite_apply_metadata_insert(
    n00b_elf_binary_t                   *bin,
    n00b_elf_rewrite_metadata_request_t *request) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    auto plan_result = n00b_elf_rewrite_plan_metadata_insert(bin,
                                                             request,
                                                             .allocator = allocator);
    if (n00b_result_is_err(plan_result)) {
        return n00b_result_err(n00b_buffer_t *,
                               n00b_result_get_err(plan_result));
    }

    n00b_elf_rewrite_plan_t *plan = n00b_result_get(plan_result);
    if (plan->outcome != N00B_ELF_REWRITE_PLAN_ACCEPTED) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ELF_REWRITE_ERR_PLAN_REJECTED);
    }

    return n00b_elf_rewrite_apply_metadata_insert_plan(bin,
                                                       plan,
                                                       .allocator = allocator);
}

n00b_result_t(n00b_buffer_t *)
n00b_elf_rewrite_apply_chalk_mark_delete(
    n00b_elf_binary_t *bin) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    auto plan_result = n00b_elf_rewrite_plan_chalk_mark_delete(bin,
                                                               .allocator = allocator);
    if (n00b_result_is_err(plan_result)) {
        return n00b_result_err(n00b_buffer_t *,
                               n00b_result_get_err(plan_result));
    }

    n00b_elf_rewrite_plan_t *plan = n00b_result_get(plan_result);
    if (plan->outcome != N00B_ELF_REWRITE_PLAN_ACCEPTED) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ELF_REWRITE_ERR_PLAN_REJECTED);
    }

    return n00b_elf_rewrite_apply_chalk_mark_plan(bin,
                                                  plan,
                                                  .allocator = allocator);
}

n00b_result_t(n00b_buffer_t *)
n00b_elf_rewrite_apply_chalk_mark_replace(
    n00b_elf_binary_t                   *bin,
    n00b_elf_rewrite_metadata_request_t *request) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    auto plan_result = n00b_elf_rewrite_plan_chalk_mark_replace(
        bin,
        request,
        .allocator = allocator);
    if (n00b_result_is_err(plan_result)) {
        return n00b_result_err(n00b_buffer_t *,
                               n00b_result_get_err(plan_result));
    }

    n00b_elf_rewrite_plan_t *plan = n00b_result_get(plan_result);
    if (plan->outcome != N00B_ELF_REWRITE_PLAN_ACCEPTED) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ELF_REWRITE_ERR_PLAN_REJECTED);
    }

    return n00b_elf_rewrite_apply_chalk_mark_plan(bin,
                                                  plan,
                                                  .allocator = allocator);
}

n00b_string_t *
n00b_elf_rewrite_err_str(n00b_err_t err)
{
    switch (err) {
    case N00B_ELF_REWRITE_OK:                         return r"ok";
    case N00B_ELF_REWRITE_ERR_NULL_BINARY:            return r"ELF rewrite: null binary";
    case N00B_ELF_REWRITE_ERR_NULL_REQUEST:           return r"ELF rewrite: null request";
    case N00B_ELF_REWRITE_ERR_NULL_SECTION_NAME:      return r"ELF rewrite: null section name";
    case N00B_ELF_REWRITE_ERR_NULL_PAYLOAD:           return r"ELF rewrite: null payload";
    case N00B_ELF_REWRITE_ERR_ZERO_PAYLOAD_SIZE:      return r"ELF rewrite: zero payload size";
    case N00B_ELF_REWRITE_ERR_TARGET_PROFILE:         return r"ELF rewrite: target profile error";
    case N00B_ELF_REWRITE_ERR_ADMISSION:              return r"ELF rewrite: admission error";
    case N00B_ELF_REWRITE_ERR_OVERFLOW:               return r"ELF rewrite: integer overflow";
    case N00B_ELF_REWRITE_ERR_NULL_PLAN:              return r"ELF rewrite: null plan";
    case N00B_ELF_REWRITE_ERR_PLAN_REJECTED:          return r"ELF rewrite: rejected plan";
    case N00B_ELF_REWRITE_ERR_UNSUPPORTED_PLAN:       return r"ELF rewrite: unsupported plan";
    case N00B_ELF_REWRITE_ERR_APPLY:                  return r"ELF rewrite: apply error";
    case N00B_ELF_REWRITE_ERR_PARSE_AFTER_APPLY:      return r"ELF rewrite: output parse failed";
    case N00B_ELF_REWRITE_ERR_MARK_NOT_FOUND:         return r"ELF rewrite: mark not found";
    case N00B_ELF_REWRITE_ERR_TRUSTED_NAME:           return r"ELF rewrite: trusted name mismatch";
    default:                                          return r"ELF rewrite: unknown error code";
    }
}

n00b_string_t *
n00b_elf_rewrite_plan_outcome_str(n00b_elf_rewrite_plan_outcome_t outcome)
{
    switch (outcome) {
    case N00B_ELF_REWRITE_PLAN_ACCEPTED: return r"accepted";
    case N00B_ELF_REWRITE_PLAN_REJECTED: return r"rejected";
    }

    return r"unknown-elf-rewrite-plan-outcome";
}

n00b_string_t *
n00b_elf_rewrite_rejection_reason_str(
    n00b_elf_rewrite_rejection_reason_t reason)
{
    switch (reason) {
    case N00B_ELF_REWRITE_REJECT_NONE:                    return r"none";
    case N00B_ELF_REWRITE_REJECT_TARGET_PROFILE:          return r"target-profile";
    case N00B_ELF_REWRITE_REJECT_ADMISSION:               return r"admission";
    case N00B_ELF_REWRITE_REJECT_TABLE_PLACEMENT:         return r"table-placement";
    case N00B_ELF_REWRITE_REJECT_SECTION_COUNT_PROMOTION: return r"section-count-promotion";
    case N00B_ELF_REWRITE_REJECT_OVERFLOW:                return r"overflow";
    case N00B_ELF_REWRITE_REJECT_CHALK_MARK_NOT_FOUND:    return r"chalk-mark-not-found";
    case N00B_ELF_REWRITE_REJECT_CHALK_MARK_UNSUPPORTED:  return r"chalk-mark-unsupported";
    case N00B_ELF_REWRITE_REJECT_TRUSTED_NAME:            return r"trusted-name";
    }

    return r"unknown-elf-rewrite-rejection-reason";
}

n00b_string_t *
n00b_elf_rewrite_target_profile_reason_str(
    n00b_elf_rewrite_target_profile_reason_t reason)
{
    switch (reason) {
    case N00B_ELF_REWRITE_PROFILE_OK:                       return r"ok";
    case N00B_ELF_REWRITE_PROFILE_INVALID_EHSIZE:           return r"invalid-ehsize";
    case N00B_ELF_REWRITE_PROFILE_INVALID_SHENTSIZE:        return r"invalid-shentsize";
    case N00B_ELF_REWRITE_PROFILE_INVALID_PHENTSIZE:        return r"invalid-phentsize";
    case N00B_ELF_REWRITE_PROFILE_UNSUPPORTED_SHNLO:        return r"unsupported-shnlo";
    case N00B_ELF_REWRITE_PROFILE_SHTAB_SIZE:               return r"shtab-size";
    case N00B_ELF_REWRITE_PROFILE_INVALID_SHTAB_BOUNDS:     return r"invalid-shtab-bounds";
    case N00B_ELF_REWRITE_PROFILE_ZERO_PHNUM:               return r"zero-phnum";
    case N00B_ELF_REWRITE_PROFILE_UNSUPPORTED_PXNUM:        return r"unsupported-pxnum";
    case N00B_ELF_REWRITE_PROFILE_INVALID_PHTAB:            return r"invalid-phtab";
    case N00B_ELF_REWRITE_PROFILE_INVALID_PHTAB_BOUNDS:     return r"invalid-phtab-bounds";
    case N00B_ELF_REWRITE_PROFILE_NO_STRTAB:                return r"no-strtab";
    case N00B_ELF_REWRITE_PROFILE_STRTAB_SIZE_WRAP:         return r"strtab-size-wrap";
    case N00B_ELF_REWRITE_PROFILE_UNSUPPORTED_STRTAB_SHNLO: return r"unsupported-strtab-shnlo";
    case N00B_ELF_REWRITE_PROFILE_INVALID_STRTAB_INDEX:     return r"invalid-strtab-index";
    case N00B_ELF_REWRITE_PROFILE_INVALID_STRTAB_TYPE:      return r"invalid-strtab-type";
    case N00B_ELF_REWRITE_PROFILE_STRTAB_SIZE:              return r"strtab-size";
    case N00B_ELF_REWRITE_PROFILE_SECTION_NAME_INDEX:       return r"section-name-index";
    }

    return r"unknown-elf-rewrite-target-profile-reason";
}

n00b_string_t *
n00b_elf_rewrite_table_strategy_str(
    n00b_elf_rewrite_table_strategy_t strategy)
{
    switch (strategy) {
    case N00B_ELF_REWRITE_TABLE_STRATEGY_NONE:                    return r"none";
    case N00B_ELF_REWRITE_TABLE_STRATEGY_IN_PLACE_SHSTRTAB_GROWTH:
        return r"in-place-shstrtab-growth";
    case N00B_ELF_REWRITE_TABLE_STRATEGY_TERMINAL_TAIL_INCISION:  return r"terminal-tail-incision";
    case N00B_ELF_REWRITE_TABLE_STRATEGY_EOF_REPLACEMENT:         return r"eof-replacement";
    case N00B_ELF_REWRITE_TABLE_STRATEGY_AFTER_OVERLAY_REPLACEMENT:
        return r"after-overlay-replacement";
    }

    return r"unknown-elf-rewrite-table-strategy";
}

n00b_string_t *
n00b_elf_rewrite_patch_kind_str(n00b_elf_rewrite_patch_kind_t kind)
{
    switch (kind) {
    case N00B_ELF_REWRITE_PATCH_ELF_HEADER:           return r"elf-header";
    case N00B_ELF_REWRITE_PATCH_PAYLOAD:              return r"payload";
    case N00B_ELF_REWRITE_PATCH_SECTION_NAME_STRTAB:  return r"section-name-strtab";
    case N00B_ELF_REWRITE_PATCH_SECTION_HEADER_TABLE: return r"section-header-table";
    case N00B_ELF_REWRITE_PATCH_TABLE_TAIL:           return r"table-tail";
    case N00B_ELF_REWRITE_PATCH_APPENDED_TABLES:      return r"appended-tables";
    case N00B_ELF_REWRITE_PATCH_STALE_PAYLOAD:        return r"stale-payload";
    }

    return r"unknown-elf-rewrite-patch-kind";
}
