#define N00B_MEM_INTERNAL_API
#define N00B_USE_INTERNAL_API

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>

#include "n00b.h"
#include "core/mmaps.h"
#include "core/alloc_mdata.h"
#include "core/memory_info.h"
#include "core/align.h"
#include "core/atomic.h"
#include "core/thread.h"
#include "core/arena.h"

// TODO
// #include "core/stw.h"

// TODO
#define n00b_fprintf fprintf
#include <stdio.h>

_Atomic uint64_t static_order_id = 1;

static n00b_memperm_pipe_t *
n00b_memperm_pipe_get(void)
{
    return &n00b_thread_self()->memperm_pipe;
}

extern void n00b_debug_memory_info(bool);

// For right now, this is dumb. But it should ultimately keep sorted
// while inserting, and binary search. Will do that when we move it to
// a proper list.
//
// Note that we never record data ranges; anything not registered as a
// segment by the time a call returns here is considered data.

#define MINCORE_TEST_BIT 1

#if defined(__linux__)
#include <link.h>

static int
n00b_extract_lib_info(struct dl_phdr_info *info, size_t size, void *unused)
{
    int64_t intaddr;
    void   *startp;
    void   *endp;

    for (uint32_t i = 0; i < info->dlpi_phnum; i++) {
        size    = info->dlpi_phdr[i].p_vaddr;
        intaddr = (int64_t)(info->dlpi_addr + size);
        startp  = (void *)intaddr;
        endp    = (void *)(intaddr + info->dlpi_phdr[i].p_memsz);

        if (startp == endp) {
            continue;
        }
        n00b_register_mmap(startp,
                           endp,
                           (char *)info->dlpi_name,
                           nullptr,
                           info->dlpi_phdr[i].p_offset,
                           (intptr_t)info->dlpi_addr,
                           startp ? n00b_mmap_static : n00b_mmap_zero_page,
                           n00b_atomic_add(&static_order_id, 1),
                           false);
    }
    return 0;
}

#if defined(N00B_ALWAYS_RECHECK_STATIC_POINTERS)
static inline n00b_mmap_info_t *
n00b_check_static_maps(void *addr)
{
    dl_iterate_phdr(n00b_extract_lib_info, nullptr);
    return n00b_mmap_by_address(addr);
}
#endif

void
n00b_load_static_ranges(void)
{
    dl_iterate_phdr(n00b_extract_lib_info, nullptr);
}

#elif defined(__APPLE__)
#include <dlfcn.h>         // for dladdr, Dl_info
#include <mach-o/dyld.h>   // for _dyld_register_func_for_add_image
#include <mach-o/loader.h> // for segment_command_64, mach_header_64, LC_...

void
n00b_on_lib_load(const struct mach_header *hdr, intptr_t slide)
{
    if (hdr->magic != MH_MAGIC_64) {
        n00b_fprintf(stderr, "Only 64-bit supported.\n");
        abort();
    }

    struct mach_header_64     *header  = (struct mach_header_64 *)hdr;
    char                      *start   = (char *)&header[1];
    struct segment_command_64 *command = (struct segment_command_64 *)start;

    Dl_info info;
    dladdr(header, &info);

    for (uint32_t i = 0; i < header->ncmds; i++) {
        uint64_t seg_start = command->vmaddr + slide;
        uint64_t seg_end   = seg_start + command->vmsize;

        if (command->cmd == LC_SEGMENT_64) {
            n00b_register_mmap((void *)seg_start,
                               (void *)seg_end,
                               (char *)info.dli_fname,
                               nullptr,
                               command->fileoff,
                               -slide,
                               seg_start ? n00b_mmap_static : n00b_mmap_zero_page,
                               n00b_atomic_add(&static_order_id, 1),
                               true);
            assert(static_order_id != 1);
        }

        start += command->cmdsize;
        command = (void *)start;
    }
}

void
n00b_load_static_ranges(void)
{
    _dyld_register_func_for_add_image(n00b_on_lib_load);
}
#else
#error "Unsupported OS"
#endif

static n00b_mmap_info_t *
n00b_check_kernel_page_map(const void *addr)
{
    char *start = n00b_align_to_page_start((void *)addr);
    char  status[1];

    if (!start) {
        return nullptr;
    }

    // This cast is crucial due to different signatures across
    // mac + linux (signed vs. unsigned.
    if (mincore(start, n00b_page_size, (void *)status)) {
        return nullptr;
    }
    if (!(status[0] & MINCORE_TEST_BIT)) {
        return nullptr;
    }

    // Register just this one page.
    return n00b_mmaps_register_mem_map(&n00b_global_mem_maps,
                                       start,
                                       n00b_page_size,
                                       0,
                                       start ? n00b_mmap_unmanaged : n00b_mmap_zero_page,

                                       false);
}

// This only gets called when lookup fails.
static inline n00b_mmap_info_t *
n00b_check_for_unmanaged_map(const void *addr)
{
    n00b_mmap_info_t *result;
    // On MacOS, we register for dynamic events, so don't need to make a
    // dynamic call; it would have been in the static list (minux some
    // race condition, where we'll just accept returning that an address
    // is data).
    //
    // On Linux, I recommend not turning on this re-checking; we
    // expose the ability to manually re-run (call
    // n00b_reload_static_ranges() yourself).

#if defined(__linux__) && defined(N00B_ALWAYS_RECHECK_STATIC_POINTERS)
    result = n00b_check_static_maps(addr);
    if (result) {
        return result;
    }
#endif

    result = n00b_check_kernel_page_map(addr);

    return result;
}

n00b_mmap_info_t *
n00b_mmap_info_lookup(const void *addr)
{
    n00b_mmap_info_t *result = n00b_mmap_by_address((void *)addr);

    if (result) {
        if (result->kind == n00b_mmap_zero_page) {
            return nullptr;
        }
        return result;
    }

    return n00b_check_for_unmanaged_map(addr);
}

// clang-format off
n00b_option_t(n00b_allocator_t *)
n00b_find_allocator(void *val)
{
    n00b_mmap_info_t *mmap = n00b_mmap_info_lookup(val);

    if (!mmap) {
        return n00b_option_none(n00b_allocator_t *);
    }

    return n00b_option_from_nullable(n00b_allocator_t *, atomic_load(&mmap->allocator));
}
// clang-format on

n00b_mmap_perms_t
n00b_check_memory_perms(void *ptr)
{
    bool cannot_write = false;
    bool cannot_read  = false;

    n00b_memperm_pipe_t *pipe_state = n00b_memperm_pipe_get();
    int                  local_pipe_fds[2];
    int                 *pipe_fds  = local_pipe_fds;
    bool                 use_cache = false;

    signal(SIGPIPE, SIG_IGN);

    if (pipe_state) {
        if (!pipe_state->ready) {
            if (pipe(pipe_state->fds)) {
                return n00b_mmap_perms_no_access;
            }
            int flags = fcntl(pipe_state->fds[0], F_GETFL, 0);
            if (flags >= 0) {
                fcntl(pipe_state->fds[0], F_SETFL, flags | O_NONBLOCK);
            }
            pipe_state->ready = 1;
        }
        pipe_fds  = pipe_state->fds;
        use_cache = true;
    }
    else {
        if (pipe(local_pipe_fds)) {
            return n00b_mmap_perms_no_access;
        }
        int flags = fcntl(local_pipe_fds[0], F_GETFL, 0);
        if (flags >= 0) {
            fcntl(local_pipe_fds[0], F_SETFL, flags | O_NONBLOCK);
        }
    }

    ssize_t wrc = write(pipe_fds[1], ptr, 1);
    if (wrc <= 0) {
        cannot_write = true;
    }

    struct pollfd pollset = {
        .fd     = pipe_fds[0],
        .events = POLL_IN,
    };

    int prc = poll(&pollset, 1, 0);
    if (prc <= 0 || !(pollset.revents & POLL_IN)) {
        cannot_read = true;
        char drain;
        (void)read(pipe_fds[0], &drain, 1);
    }
    else {
        ssize_t rrc = read(pipe_fds[0], ptr, 1);
        if (rrc <= 0) {
            cannot_read = true;
            char drain;
            (void)read(pipe_fds[0], &drain, 1);
        }
    }

    if (!use_cache) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
    }

    if (cannot_write) {
        if (cannot_read) {
            return n00b_mmap_perms_no_access;
        }
        else {
            return n00b_mmap_perms_ro;
        }
    }

    return n00b_mmap_perms_rw;
}

size_t
n00b_address_is_probable_cstring(void *addr, size_t *bytelen, size_t min_len)
{
// Looks for UTF-8 AND null termination.
// TODO: hook up unicode.
#if 0
    uint32_t blen = 0;
    uint32_t clen = 0;
    int32_t  cp;
    char    *page_end;
    char    *p          = (char *)addr;
    char    *page_start = (char *)addr;

    if (n00b_value_is_data(p)) {
        return 0;
    }

    page_end = n00b_align_to_page_start(p) + n00b_page_size;

    while (true) {
        while (p < page_end) {
            cp = n00b_u8_decode_character(&p, page_end);
            if (cp < 0) {
                return 0;
            }
            else {
                if (!cp) {
                    if (clen < min_len) {
                        return 0;
                    }
                    blen += p - page_start - 1;
                    *bytelen = blen;
                    return clen;
                }
                if (!n00b_codepoint_is_printable(cp)) {
                    return 0;
                }
                clen++;
            }
        }
        blen += p - page_start;
        page_start = p;
        page_end += n00b_page_size;

        // If this returns true, there's nothing in static or heap memory.
        if (n00b_value_is_data(p)) {
            return 0;
        }
    }
#else
    return 0;
#endif
}

bool
n00b_memory_scan_init(n00b_memory_scan_t *ctx, void *s, size_t len, uint8_t cat_flags)
{
    uint64_t start = (uint64_t)s;
    uint64_t end   = start + len;

    ctx->cur = (uint64_t *)n00b_align_ceil(start, sizeof(void *));
    ctx->end = (uint64_t *)n00b_align_floor(end, sizeof(void *));

    if (n00b_value_is_data(ctx->cur) || n00b_value_is_data(ctx->end)) {
        return false;
    }

    if (!cat_flags) {
        ctx->flags = n00b_mmap_type_mask;
    }
    else {
        ctx->flags = cat_flags;
    }

    return true;
}

void *
n00b_memory_scan_next(n00b_memory_scan_t   *ctx,
                      n00b_mmap_rec_kind_t *tinfo,
                      n00b_mmap_perms_t    *perms)
{
    void             *result = nullptr;
    n00b_mmap_info_t *mmap   = nullptr;

    while (!result && (ctx->cur < ctx->end)) {
        void *val = (void *)*ctx->cur;
        mmap      = n00b_mmap_info_lookup(val);

        if (mmap && (mmap->kind & ctx->flags)) {
            result   = (void *)ctx->cur;
            ctx->cur = ctx->cur + 1;
            break;
        }
        mmap     = nullptr;
        ctx->cur = ctx->cur + 1;
    }

    if (!mmap) {
        return nullptr;
    }

    if (perms) {
        *perms = mmap ? n00b_check_memory_perms(ctx->cur) : n00b_mmap_perms_data_not_addr;
    }

    if (tinfo) {
        *tinfo = mmap->kind;
    }

    return result;
}

static void
show_mem_info(n00b_mmap_info_t *n, bool all, unsigned int *lenp)
{
    char *kind_str = nullptr;
    bool  aseg     = false;

    if (!n) {
        return;
    }

    show_mem_info(n->left, all, lenp);

    switch (n->kind) {
    case n00b_mmap_static:
        if (!all) {
            show_mem_info(n->right, all, lenp);
            return;
        }
        else {
            kind_str = "Static memory ";
        }
        break;
    case n00b_mmap_arena:
        kind_str = "Arena header   "; // Currently not being registered.
        *lenp += (n->end - n->start);
        break;
    case n00b_mmap_managed_segment:
        kind_str = "Arena segment  ";
        *lenp += (n->end - n->start);
        aseg = true;
        break;
    case n00b_mmap_sys_segment:
        kind_str = "Sys segment    ";
        *lenp += (n->end - n->start);
        aseg = true;
        break;
    case n00b_mmap_stack:
        kind_str = "Thread stack   ";
        break;
    case n00b_mmap_unmanaged:
        *lenp += (n->end - n->start);
        kind_str = "Unmanaged heap ";
        break;
    case n00b_mmap_internal:
        *lenp += (n->end - n->start);
        kind_str = "N00b internal  ";
        break;
    case n00b_mmap_zero_page:
        kind_str = "Inaccessible   ";
        break;
    case n00b_mmap_pool:
        kind_str = "Pool alloc     ";
        *lenp += (n->end - n->start);
        aseg = true;
        break;
    case n00b_mmap_api_mmap:
        kind_str = "n00b_mmap()    ";
        *lenp += (n->end - n->start);
        break;
    case n00b_mmap_root: // Should have been explicitly skipped.
    default:             // Should also be unreachable.
        abort();
    }

    n00b_arena_t *a    = (n00b_arena_t *)n->allocator;
    char         *file = "(no file)";
    if (n->file) {
        file = n->file;
    }

    if (aseg) {
        if (a->alloc_metadata || !a->linked_arena) {
            n00b_fprintf(stderr,
                         "%s %s %p-%p (%'llu bytes, %'zu pages) arena @ %p (%s)\n",
                         kind_str,
                         file,
                         (void *)n->start,
                         (void *)n->end,
                         (unsigned long long)(n->end - n->start),
                         (size_t)(n->end - n->start) / n00b_page_size,
                         a,
                         n->file ? n->file : "*no name*");
        }
        else {
            n00b_arena_t     *link = a->linked_arena;
            n00b_allocator_t *al   = (n00b_allocator_t *)link;

            n00b_fprintf(stderr,
                         "%s %s %p-%p (%'llu bytes, %'zu pages) arena @ "
                         "%p (alloc mdata for arena '%s' @%p)\n",
                         kind_str,
                         file,
                         (void *)n->start,
                         (void *)n->end,
                         (unsigned long long)(n->end - n->start),
                         (unsigned int)(n->end - n->start) / n00b_page_size,
                         a,
                         al->debug_name ? al->debug_name : "*no name*",
                         link);
        }
    }
    else {
        n00b_fprintf(stderr,
                     "%s %s %p-%p (%'llu bytes, %'zu pages)\n",
                     kind_str,
                     file,
                     (void *)n->start,
                     (void *)n->end,
                     (unsigned long long)(n->end - n->start),
                     (unsigned int)(n->end - n->start) / n00b_page_size);
    }

    show_mem_info(n->right, all, lenp);
    return;
}

void
n00b_debug_memory_info(bool all)
{
    unsigned int len = 0;

    n00b_mmap_info_t *n = &n00b_global_mem_maps.root;
    show_mem_info(n->left, all, &len);
    show_mem_info(n->right, all, &len);
    n00b_fprintf(stderr,
                 "\nTotal heap usage: %'u bytes (%'zu pages)\n",
                 len,
                 len / n00b_page_size);
}
