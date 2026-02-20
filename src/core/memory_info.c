#define N00B_MEM_INTERNAL_API
#define N00B_USE_INTERNAL_API

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>

#include "n00b_build_config.h"

#include "n00b.h"
#include "core/mmaps.h"
#include "core/alloc_mdata.h"
#include "core/memory_info.h"
#include "core/align.h"
#include "core/atomic.h"
#include "core/thread.h"
#include "core/arena.h"
#include "core/runtime.h"
#include "core/interval_tree.h"

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

#if N00B_HAVE_DL_ITERATE_PHDR
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
        n00b_mmap_register(startp,
                           endp,
                           startp ? n00b_mmap_static : n00b_mmap_zero_page,
                           .file              = (char *)info->dlpi_name,
                           .binary_offset     = info->dlpi_phdr[i].p_offset,
                           .slide             = (intptr_t)info->dlpi_addr,
                           .order_id          = n00b_atomic_add(&static_order_id, 1),
                           .definitely_unique = false);
    }
    return 0;
}

#if defined(N00B_ALWAYS_RECHECK_STATIC_POINTERS)
static inline n00b_mmap_opt_t
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
            (void)n00b_mmap_register((void *)seg_start,
                                     (void *)seg_end,
                                     seg_start ? n00b_mmap_static : n00b_mmap_zero_page,
                                     .file              = (char *)info.dli_fname,
                                     .binary_offset     = command->fileoff,
                                     .slide             = -slide,
                                     .order_id          = n00b_atomic_add(&static_order_id, 1),
                                     .definitely_unique = false);
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
void
n00b_load_static_ranges(void)
{
    // No loader callback capability detected; pointer classification
    // falls back to on-demand kernel page checks.
}
#endif

static n00b_mmap_opt_t
n00b_check_kernel_page_map(const void *addr)
{
    char *start = n00b_align_to_page_start((void *)addr);
    char  status[1];

    if (!start) {
        return n00b_option_none(n00b_mmap_info_t *);
    }

    // This cast is crucial due to different signatures across
    // mac + linux (signed vs. unsigned.
    if (mincore(start, n00b_page_size, (void *)status)) {
        return n00b_option_none(n00b_mmap_info_t *);
    }
    if (!(status[0] & MINCORE_TEST_BIT)) {
        return n00b_option_none(n00b_mmap_info_t *);
    }

    // Register just this one page.
    return n00b_mmap_register(start,
                              start + n00b_page_size,
                              start ? n00b_mmap_unmanaged : n00b_mmap_zero_page,
                              .definitely_unique = false);
}

// This only gets called when lookup fails.
static inline n00b_mmap_opt_t
n00b_check_for_unmanaged_map(const void *addr)
{
    n00b_mmap_opt_t result;
    // On MacOS, we register for dynamic events, so don't need to make a
    // dynamic call; it would have been in the static list (minux some
    // race condition, where we'll just accept returning that an address
    // is data).
    //
    // On Linux, I recommend not turning on this re-checking; we
    // expose the ability to manually re-run (call
    // n00b_reload_static_ranges() yourself).

#if N00B_HAVE_DL_ITERATE_PHDR && defined(N00B_ALWAYS_RECHECK_STATIC_POINTERS)
    result = n00b_check_static_maps(addr);
    if (n00b_option_is_set(result)) {
        return result;
    }
#endif

    result = n00b_check_kernel_page_map(addr);

    return result;
}

n00b_mmap_opt_t
n00b_mmap_info_lookup(const void *addr)
{
    auto result = n00b_mmap_by_address((void *)addr);

    if (n00b_option_is_set(result)) {
        if (n00b_option_get(result)->kind == n00b_mmap_zero_page) {
            return n00b_option_none(n00b_mmap_info_t *);
        }
        return result;
    }

    return n00b_check_for_unmanaged_map(addr);
}

// clang-format off
n00b_option_t(n00b_allocator_t *)
n00b_find_allocator(void *val)
{
    auto mmap_opt = n00b_mmap_info_lookup(val);

    if (!n00b_option_is_set(mmap_opt)) {
        return n00b_option_none(n00b_allocator_t *);
    }

    n00b_mmap_info_t *mmap = n00b_option_get(mmap_opt);
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
        .events = POLLIN,
    };

    int prc = poll(&pollset, 1, 0);
    if (prc <= 0 || !(pollset.revents & POLLIN)) {
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
        void *val      = (void *)*ctx->cur;
        auto  mmap_opt = n00b_mmap_info_lookup(val);

        if (n00b_option_is_set(mmap_opt)) {
            mmap = n00b_option_get(mmap_opt);
            if (mmap->kind & ctx->flags) {
                result   = (void *)ctx->cur;
                ctx->cur = ctx->cur + 1;
                break;
            }
        }
        mmap     = nullptr;
        ctx->cur = ctx->cur + 1;
    }

    if (!mmap) {
        return nullptr;
    }

    if (perms) {
        *perms = n00b_check_memory_perms(ctx->cur);
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

    switch (n->kind) {
    case n00b_mmap_static:
        if (!all) {
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
    default:
        abort();
    }

    n00b_allocator_t *a    = n->allocator;
    const char       *file = "(no file)";
    if (n->file) {
        file = n->file;
    }

    if (aseg) {
        if (!a->metadata) {
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
            n00b_allocator_t *link = a->metadata_pool;

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
                         link->debug_name ? link->debug_name : "*no name*",
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
}

void
n00b_debug_memory_info(bool all)
{
    unsigned int     len = 0;
    n00b_mmap_ctx_t *ctx = n00b_global_mem_map(n00b_get_runtime());

    n00b_allocator_t *alloc      = (n00b_allocator_t *)&ctx->pool;
    n00b_stack_t(void *) results = n00b_stack_new(void *, alloc);

    (void)n00b_interval_search_ordered(ctx->mmap_tree, 0, UINT64_MAX, &results);

    for (size_t i = 0; i < n00b_stack_len(results); i++) {
        n00b_interval_node_t *node = results.data[i];
        show_mem_info((n00b_mmap_info_t *)node->data, all, &len);
    }

    n00b_fprintf(stderr,
                 "\nTotal heap usage: %'u bytes (%'zu pages)\n",
                 len,
                 len / n00b_page_size);
}
