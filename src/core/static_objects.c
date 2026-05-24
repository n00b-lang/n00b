#define N00B_MEM_INTERNAL_API

#include "n00b.h"
#include "core/static_objects.h"
#include "core/mmaps.h"

#if defined(_WIN32)
#include "core/platform.h"
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#endif

static size_t
n00b_static_objects_enumerate_entries(n00b_static_object_entry_t const *start,
                                      n00b_static_object_entry_t const *stop,
                                      n00b_static_object_iter_cb cb,
                                      void *user)
{
    if (!start || !stop || stop < start) {
        return 0;
    }

    size_t count = 0;

    for (n00b_static_object_entry_t const *entry = start; entry < stop; entry++) {
        const n00b_static_object_desc_t *desc = *entry;
        if (!desc || !desc->start || desc->len == 0) {
            continue;
        }
        if (cb) {
            cb(desc, user);
        }
        count++;
    }

    return count;
}

#if defined(__APPLE__)
static size_t
n00b_static_objects_enumerate_macho_image(const struct mach_header *hdr,
                                          intptr_t slide,
                                          n00b_static_object_iter_cb cb,
                                          void *user)
{
    if (!hdr || hdr->magic != MH_MAGIC_64) {
        return 0;
    }

    const struct mach_header_64 *header = (const struct mach_header_64 *)hdr;
    const uint8_t *cursor = (const uint8_t *)&header[1];
    size_t count = 0;

    for (uint32_t i = 0; i < header->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)cursor;

        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg =
                (const struct segment_command_64 *)cursor;
            const struct section_64 *section =
                (const struct section_64 *)(seg + 1);

            for (uint32_t j = 0; j < seg->nsects; j++) {
                if (strncmp(section[j].segname, "__DATA", 16) != 0
                    || strncmp(section[j].sectname, "n00b_stobj", 16) != 0) {
                    continue;
                }

                uintptr_t start_addr = (uintptr_t)section[j].addr + (uintptr_t)slide;
                uintptr_t stop_addr  = start_addr + (uintptr_t)section[j].size;
                count += n00b_static_objects_enumerate_entries(
                    (n00b_static_object_entry_t const *)start_addr,
                    (n00b_static_object_entry_t const *)stop_addr,
                    cb,
                    user);
            }
        }

        cursor += lc->cmdsize;
    }

    return count;
}
#elif defined(_WIN32)
N00B_STATIC_OBJECT_SECTION_PRE(".n00bs$a")
n00b_static_object_entry_t const __n00b_static_object_section_start
    N00B_STATIC_OBJECT_SECTION_POST(".n00bs$a") = nullptr;

N00B_STATIC_OBJECT_SECTION_PRE(".n00bs$z")
n00b_static_object_entry_t const __n00b_static_object_section_end
    N00B_STATIC_OBJECT_SECTION_POST(".n00bs$z") = nullptr;
#else
extern n00b_static_object_entry_t const __start_n00b_stobj[] __attribute__((weak));
extern n00b_static_object_entry_t const __stop_n00b_stobj[] __attribute__((weak));
#endif

#if defined(_WIN32)
static void
n00b_static_object_register_pe_mapping(const n00b_static_object_desc_t *desc)
{
    const char *cursor = (const char *)desc->start;
    const char *limit  = cursor + desc->len;

    while (cursor < limit) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(cursor, &mbi, sizeof(mbi))) {
            return;
        }
        if (mbi.State != MEM_COMMIT || mbi.Protect == PAGE_NOACCESS) {
            return;
        }

        char *start = (char *)mbi.BaseAddress;
        char *end   = start + mbi.RegionSize;
        if (end <= cursor) {
            return;
        }

        (void)n00b_mmap_register(start,
                                 end,
                                 n00b_mmap_static,
                                 .file              = desc->file,
                                 .definitely_unique = false);
        cursor = end;
    }
}
#endif

n00b_alloc_range_t *
n00b_static_object_register_desc(const n00b_static_object_desc_t *desc)
{
    if (!desc || !desc->start || desc->len == 0) {
        return nullptr;
    }

    auto existing = n00b_mmap_range_by_address((void *)desc->start);
    if (n00b_option_is_set(existing)) {
        n00b_alloc_range_t *range = n00b_option_get(existing);
        if (range->start == desc->start && range->len == desc->len
            && range->object_id == desc->object_id) {
            if (range->identity == nullptr && desc->identity != nullptr) {
                (void)n00b_static_identity_register(desc->identity, range);
            }
            return range;
        }
    }

#if defined(_WIN32)
    n00b_static_object_register_pe_mapping(desc);
#endif

    return _n00b_static_object_register((void *)desc->start,
                                        (size_t)desc->len,
                                        desc->tinfo,
                                        desc->file,
                                        .scan_kind = desc->scan_kind,
                                        .scan_cb   = desc->scan_cb,
                                        .scan_user = desc->scan_user,
                                        .object_id = desc->object_id,
                                        .identity  = desc->identity,
                                        .flags     = desc->flags);
}

static void
n00b_static_object_register_cb(const n00b_static_object_desc_t *desc, void *user)
{
    size_t *count = (size_t *)user;
    if (n00b_static_object_register_desc(desc)) {
        (*count)++;
    }
}

size_t
n00b_static_objects_enumerate(n00b_static_object_iter_cb cb, void *user)
{
#if defined(__APPLE__)
    uint32_t image_count = _dyld_image_count();
    size_t count = 0;

    for (uint32_t i = 0; i < image_count; i++) {
        count += n00b_static_objects_enumerate_macho_image(
            _dyld_get_image_header(i),
            _dyld_get_image_vmaddr_slide(i),
            cb,
            user);
    }

    return count;
#elif defined(_WIN32)
    return n00b_static_objects_enumerate_entries(
        &__n00b_static_object_section_start + 1,
        &__n00b_static_object_section_end,
        cb,
        user);
#else
    if (!__start_n00b_stobj || !__stop_n00b_stobj) {
        return 0;
    }
    return n00b_static_objects_enumerate_entries(__start_n00b_stobj,
                                                __stop_n00b_stobj,
                                                cb,
                                                user);
#endif
}

size_t
n00b_static_objects_register_all(void)
{
    size_t count = 0;
    (void)n00b_static_objects_enumerate(n00b_static_object_register_cb, &count);
    return count;
}

#if defined(__APPLE__)
size_t
n00b_static_objects_register_macho_image(const struct mach_header *hdr,
                                         intptr_t slide)
{
    size_t count = 0;
    (void)n00b_static_objects_enumerate_macho_image(
        hdr,
        slide,
        n00b_static_object_register_cb,
        &count);
    return count;
}
#endif
