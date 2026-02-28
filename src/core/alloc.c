#include "n00b.h"
#include "core/alloc.h"
#include "core/arena.h"
#include "core/alloc_mdata.h"
#include "core/dict_untyped.h"
#include "core/mmaps.h"
#include "core/memory_info.h"
#include "core/stw.h"
#include "core/pool.h"
#include "core/runtime.h"
#include "core/type_info.h"
#include "core/data_lock.h"

#ifndef N00B_METADATA_START_ENTRIES
#define N00B_METADATA_START_ENTRIES 1 << 12
#endif

extern uint64_t         n00b_gc_guard;
const n00b_alloc_opts_t _n00b_default_alloc_opts = {};
static void             n00b_run_and_remove_finalizers(void *ptr);

static inline void
n00b_alloc_add_inline_header(n00b_inline_hdr_t **hdrp,
                             size_t              alloc_len,
                             uint64_t            type_hash,
                             bool                is_array,
                             bool                no_scan,
                             bool                mem_debug,
                             bool                mem_debug_taint)
{
    n00b_inline_hdr_t *hdr = *hdrp;
    assert(alloc_len >= sizeof(n00b_inline_hdr_t));

    *hdr = (n00b_inline_hdr_t){
        .guard           = n00b_gc_guard,
        .tinfo           = type_hash,
        .alloc_len       = alloc_len,
        .is_array        = is_array,
        .no_scan         = no_scan,
        .mem_debug       = mem_debug,
        .mem_debug_taint = mem_debug_taint,
    };

    *hdrp = ++hdr;
}

void *
_n00b_alloc_raw(size_t             n,
                size_t             sz,
                uint64_t           type_hash,
                const char        *location,
                n00b_alloc_opts_t *opts,
                +) _kargs: opaque
{
    n00b_inline_hdr_t *hdr      = nullptr;
    n00b_oob_hdr_t    *map_item = nullptr;
    n00b_alloc_opts_t  local_opts;

    if (!opts) {
        local_opts = _n00b_default_alloc_opts;
        opts       = &local_opts;
    }

    n00b_ensure_allocator(opts->allocator);

    if (!opts->allocator->__system) {
        n00b_thread_checkin();
    }

    uint64_t request  = n * sz;
    bool     is_array = n > 1;

    if (opts->allocator->add_inline_header) {
        request += N00B_ALLOC_HDR_SZ;
    }
    void *r;

    if (!request) {
        request = opts->allocator->add_inline_header ? sizeof(n00b_inline_hdr_t) : 1;
    }

    request = n00b_align(request);

    // Currently never pass parameters to the allocator. For future use.
    r = (*opts->allocator->zero_alloc)(opts->allocator, request, nullptr);

    if (opts->allocator->add_inline_header) {
        hdr = r;

        n00b_alloc_add_inline_header((n00b_inline_hdr_t **)&r,
                                     request,
                                     type_hash,
                                     n > 1,
                                     opts->no_scan,
                                     opts->mem_debug,
                                     opts->debug_taint);
    }

    if (opts->allocator->metadata_pool != nullptr) {
        n00b_alloc_opts_t md_opts = {.allocator = opts->allocator->metadata_pool};
        map_item                  = n00b_alloc_with_opts(n00b_oob_hdr_t, &md_opts);

        *map_item = (n00b_oob_hdr_t){
            .user_ptr        = r,
            .tinfo           = type_hash,
            .alloc_len       = request,
            .is_array        = n > 1,
            .no_scan         = opts->no_scan,
            .mem_debug       = opts->mem_debug,
            .mem_debug_taint = opts->debug_taint,
            .hcur            = hdr,
            .file_name       = location,
        };

        n00b_dict_untyped_put(opts->allocator->metadata, r, map_item);
        assert(n00b_dict_untyped_get(opts->allocator->metadata, r, nullptr) == map_item);
    }

    // If the allocator has no headers and no metadata but is visible to
    // the GC (non-hidden), register the allocation in the range tree so
    // the collector's fallback path can discover and scan it.
    if (!opts->allocator->hidden && !opts->allocator->add_inline_header
        && opts->allocator->metadata_pool == nullptr
        && n00b_option_is_set(n00b_default_runtime)) {
        n00b_mmap_register_range(r,
                                 (char *)r + request,
                                 n00b_mmap_pool,
                                 .allocator = opts->allocator);
    }

    assert(!(((uint64_t)r) & (N00B_ALIGN - 1)));

    // Dispatch the vtable constructor if the type is registered.
    // Guard on startup_complete to avoid early-init lookups.
    //
    // Constructor dispatch depends on type_info flags:
    //   !ctor_takes_kargs && !ctor_takes_vargs: ctor(self)
    //   ctor_takes_kargs  && !ctor_takes_vargs: ctor(self, kargs_ptr)
    //   ctor_takes_vargs:                       ctor(self, vargs, kargs_ptr)
    //
    // For kargs/vargs constructors, the data comes from vargs packed
    // by n00b_new_kargs / n00b_new_both.  If no vargs were provided,
    // kargs/vargs constructors are skipped (use n00b_new_kargs to
    // trigger construction, not bare n00b_alloc).
    if (!is_array && type_hash && n00b_option_is_set(n00b_default_runtime)
        && n00b_get_runtime()->startup_complete) {
        auto tinfo_opt = n00b_type_lookup(type_hash);

        if (n00b_option_is_set(tinfo_opt)) {
            n00b_type_info_t *tinfo = n00b_option_get(tinfo_opt);
            n00b_vtable_entry ctor  = tinfo->core_vtable[N00B_BI_CONSTRUCTOR];
            if (ctor) {
                bool have_vargs = vargs && vargs->nargs > 0;

                if (tinfo->ctor_takes_vargs && have_vargs) {
                    // ctor(self, vargs, kargs).
                    // n00b_new_both packs: real_varg0, ..., kargs_ptr
                    // kargs is always the last varg.
                    void *ctor_kargs = vargs->args[vargs->nargs - 1];
                    vargs->nargs--;
                    ((void (*)(void *, n00b_vargs_t *, void *))ctor)(
                        r, vargs, ctor_kargs);
                }
                else if (tinfo->ctor_takes_kargs && !tinfo->ctor_takes_vargs) {
                    // ctor(self, kargs).
                    // kargs comes from the opaque _kargs parameter.
                    if (kargs) {
                        ((void (*)(void *, void *))ctor)(r, kargs);
                    }
                }
                else if (!tinfo->ctor_takes_kargs && !tinfo->ctor_takes_vargs) {
                    // ctor(self) — always dispatched (no data needed).
                    ((void (*)(void *))ctor)(r);
                }
            }
        }
    }

    if (opts->finalizer) {
        n00b_runtime_t *rt = n00b_get_runtime();
        if (rt) {
            // The raw header key must match what the GC uses in ctx->memos:
            // - OOB arenas: (n00b_inline_hdr_t *)map_item (the OOB record)
            // - Inline-only: hdr (the actual inline header)
            n00b_inline_hdr_t *alloc_key = (opts->allocator->metadata_pool != nullptr)
                                             ? (n00b_inline_hdr_t *)map_item
                                             : hdr;

            n00b_alloc_opts_t     md_opts = {.allocator = (n00b_allocator_t *)&rt->system_pool};
            n00b_finalizer_info_t *info  = n00b_alloc_with_opts(n00b_finalizer_info_t, &md_opts);

            *info = (n00b_finalizer_info_t){
                .funcptr    = opts->finalizer,
                .alloc_info = alloc_key,
                .user_ptr   = opts->finalizer_data,
            };
            n00b_list_push(rt->finalizers, info);
        }
    }

    return r;
}

void
n00b_allocator_setup(n00b_allocator_t *allocator, n00b_calloc_fn alloc) _kargs
{
    n00b_free_fn              free              = nullptr;
    n00b_allocator_destroy_fn destroy           = nullptr;
    const char               *name              = nullptr;
    bool                      inline_headers    = true;
    bool                      external_metadata = true;
    // RISKY for custom allocators. Hides from GC.
    bool                      hidden            = false;
    // DO NOT USE for custom allocators. Skips mmaps.
    bool                      __nomap           = false;
    // DO NOT USE for custom allocators. Skips STW check.
    bool                      __system          = false;
    bool                      __is_md_pool      = false;
}
{
    (void)__nomap;
    n00b_allocator_t *md_pool = nullptr;

    if (external_metadata) {
        md_pool = (n00b_allocator_t *)n00b_new_arena(.no_map         = true,
                                                     .__system       = true,
                                                     .hidden         = true,
                                                     .use_gc         = false,
                                                     .inline_headers = false,
                                                     .name           = "md_pool");
    }

    n00b_dict_untyped_t *md = nullptr;

    if (external_metadata) {
        md = n00b_alloc_with_opts(n00b_dict_untyped_t, &(n00b_alloc_opts_t){.allocator = md_pool});
    }

    *allocator = (n00b_allocator_t){
        .zero_alloc        = alloc,
        .free              = free,
        .destroy           = destroy,
        .debug_name        = name,
        .add_inline_header = inline_headers,
        .__system          = __system,
        .__md_pool         = __is_md_pool,
        .hidden            = hidden,
        .metadata_pool     = md_pool,
        .metadata          = md,
    };

    if (external_metadata) {
        n00b_dict_untyped_init(allocator->metadata,
                               .start_capacity = N00B_METADATA_START_ENTRIES,
                               .allocator      = allocator->metadata_pool,
                               .hash           = n00b_hash_word,
                               .skip_obj_hash  = true);
    }
}

void
n00b_free(void *ptr)
{
    n00b_run_and_remove_finalizers(ptr);

    n00b_allocator_opt_t alloc_opt = n00b_mem_get_allocator(ptr);

    if (!n00b_option_is_set(alloc_opt)) {
        return;
    }

    n00b_allocator_t *allocator = n00b_option_get(alloc_opt);

    if (!allocator->free) {
        return;
    }

    // Remove the range-tree entry for headerless non-hidden allocations
    // (the mirror of the registration in _n00b_alloc_raw).
    if (!allocator->hidden && !allocator->add_inline_header
        && allocator->metadata_pool == nullptr) {
        n00b_runtime_t  *rt   = n00b_get_runtime();
        n00b_mmap_ctx_t *mctx = n00b_global_mem_map(rt);
        n00b_mmap_delete_ranges(mctx, (uint64_t)ptr, (uint64_t)ptr + 1);
    }

    if (allocator->add_inline_header) {
        ptr = (char *)ptr - N00B_ALLOC_HDR_SZ;
    }

    (*allocator->free)(allocator, ptr);
}

void
n00b_add_finalizer(void *obj, n00b_finalizer_t fn, void *user_data)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    assert(rt);

    // Use the same raw header key that the GC stores in its memos dict.
    // For OOB arenas, this is the n00b_oob_hdr_t* cast to n00b_inline_hdr_t*.
    // For inline-only arenas, it's the actual inline header.
    n00b_alloc_info_t ainfo = n00b_find_alloc_info(obj);
    assert(ainfo.kind == n00b_alloc_oob || ainfo.kind == n00b_alloc_inline);

    n00b_inline_hdr_t *hdr = (ainfo.kind == n00b_alloc_oob) ? (n00b_inline_hdr_t *)ainfo.hdr.oob
                                                            : ainfo.hdr.in_line;

    n00b_allocator_t      *sp   = (n00b_allocator_t *)&rt->system_pool;
    n00b_finalizer_info_t *info = n00b_alloc_with_opts(n00b_finalizer_info_t, &(n00b_alloc_opts_t){.allocator = sp});

    *info = (n00b_finalizer_info_t){
        .funcptr    = fn,
        .alloc_info = hdr,
        .user_ptr   = user_data,
    };

    n00b_list_push(rt->finalizers, info);
}

static void
n00b_run_and_remove_finalizers(void *ptr)
{
    if (!n00b_option_is_set(n00b_default_runtime)) {
        return;
    }

    n00b_runtime_t *rt = n00b_get_runtime();
    if (!rt || !rt->finalizers.data) {
        goto type_cleanup;
    }

    // Use the same raw header lookup as n00b_add_finalizer.
    n00b_alloc_info_t ainfo = n00b_find_alloc_info(ptr);
    if (ainfo.kind != n00b_alloc_oob && ainfo.kind != n00b_alloc_inline) {
        goto type_cleanup;
    }

    {
        n00b_inline_hdr_t *hdr = (ainfo.kind == n00b_alloc_oob)
                                   ? (n00b_inline_hdr_t *)ainfo.hdr.oob
                                   : ainfo.hdr.in_line;

        // Walk backwards for safe removal via n00b_list_delete.
        size_t len = n00b_list_len(rt->finalizers);

        for (size_t i = len; i > 0; i--) {
            n00b_finalizer_info_t *entry = n00b_list_get(rt->finalizers, i - 1);

            if (entry->alloc_info == hdr) {
                entry->funcptr(entry->user_ptr);
                (void)n00b_list_delete(rt->finalizers, i - 1);
                n00b_free(entry);
            }
        }
    }

type_cleanup:;
    // Lock cleanup and vtable destructor via the type registry.
    auto tinfo_opt = n00b_type_info_for(ptr);
    if (!n00b_option_is_set(tinfo_opt)) {
        return;
    }
    n00b_type_info_t *tinfo = n00b_option_get(tinfo_opt);

    // Free the lock if the type has a registered lock_offset.
    if (n00b_option_is_set(tinfo->lock_offset)) {
        uint32_t        offset   = n00b_option_get(tinfo->lock_offset);
        n00b_rwlock_t **lock_ptr = (n00b_rwlock_t **)((char *)ptr + offset);
        if (*lock_ptr) {
            n00b_free(*lock_ptr);
            *lock_ptr = nullptr;
        }
    }

    // Run the vtable destructor.
    n00b_vtable_entry dtor = tinfo->core_vtable[N00B_BI_FINALIZER];
    if (dtor) {
        ((void (*)(void *))dtor)(ptr);
    }
}

void
n00b_allocator_destroy(n00b_allocator_t *allocator)
{
    if (allocator->metadata_pool) {
        n00b_allocator_destroy(allocator->metadata_pool);
        // Only free the pool struct if it was heap-allocated (i.e.,
        // it's a real pool, not an arena whose struct lives in mmap).
        if (allocator->metadata_pool->__md_pool) {
            free(allocator->metadata_pool);
        }
        allocator->metadata_pool = nullptr;
    }

    (*allocator->destroy)(allocator);
}

#define find_sentinal(p, s) _find_sentinal(((uint64_t)p), ((uint64_t *)s))

static inline char *
_find_sentinal(uint64_t p_num, uint64_t *start)
{
    uint64_t *p = (uint64_t *)n00b_align_floor(p_num, sizeof(void *));

    while (p >= start) {
        if (*p == n00b_gc_guard) {
            return (char *)p;
        }
        p--;
    }

    return nullptr;
}

// Returns the most authoritative header, unless prefer_inline is set,
// in which case it'll return the inline header.
//
// If you need to know if it's an out-of-heap pointer so you can cast it,
// then provide is_metadata_header.
//
// Returns nullptr if not found. This obviously should all change to
// return a variant, once I revisit variants.

void
_n00b_find_alloc_info(void *addr, n00b_alloc_info_t *result) _kargs
{
    n00b_allocator_t *allocator       = nullptr;
    bool              scan_for_header = false;
}
{
    auto  mmap_opt = n00b_mmap_by_address(addr);
    char *p        = (char *)addr;

    if (!n00b_option_is_set(mmap_opt)) {
        *result = (n00b_alloc_info_t){.kind = n00b_alloc_none};
        return;
    }

    n00b_mmap_info_t *mmap = n00b_option_get(mmap_opt);
    n00b_allocator_t *al   = mmap->allocator;
    switch (mmap->kind) {
    case n00b_mmap_static:
        if (n00b_check_memory_perms(p) == n00b_mmap_perms_no_access) {
            break;
        }

        p -= sizeof(n00b_static_header_t);
        if (((uint64_t)p) < mmap->start) {
            *result = (n00b_alloc_info_t){.kind = n00b_alloc_none};
            break;
        }
        n00b_static_header_t *sh = (n00b_static_header_t *)p;

        // STATIC magic in truly static segments, but the gc sentinel
        // for the stack.

        if (sh->static_magic != N00B_STATIC_MAGIC && sh->static_magic != n00b_gc_guard) {
            break;
        }

        *result = (n00b_alloc_info_t){
            .kind        = n00b_alloc_inline,
            .hdr.in_line = (n00b_inline_hdr_t *)&sh->static_magic,
        };
        return;
    case n00b_mmap_pool:
    case n00b_mmap_managed_segment:
    case n00b_mmap_sys_segment:

        if (al->add_inline_header) {
            p -= N00B_ALLOC_HDR_SZ;
        }

        if (scan_for_header && al->add_inline_header) {
            char *scan_ptr = find_sentinal(p, (char *)mmap->start);

            if (!scan_ptr) {
                break;
            }

            p    = scan_ptr;
            addr = scan_ptr + N00B_ALLOC_HDR_SZ;
        }

        if (al->metadata) {
            n00b_oob_hdr_t *oob = n00b_dict_untyped_get(al->metadata, addr, nullptr);
            if (!oob) {
                *result = (n00b_alloc_info_t){.kind = n00b_alloc_err};
                return;
            }
            *result = (n00b_alloc_info_t){
                .kind    = n00b_alloc_oob,
                .hdr.oob = oob,
            };
            return;
        }

        n00b_inline_hdr_t *hdr = (n00b_inline_hdr_t *)p;

        if (((uint64_t)p) < mmap->start || hdr->guard != n00b_gc_guard) {
            *result = (n00b_alloc_info_t){.kind = n00b_alloc_err};
            return;
        }
        *result = (n00b_alloc_info_t){
            .kind        = n00b_alloc_inline,
            .hdr.in_line = hdr,
        };
        return;

    default:
        break;
    }
    *result = (n00b_alloc_info_t){.kind = n00b_alloc_none};
    return;
}

n00b_option_t(n00b_inline_hdr_t *) n00b_object_header(void *p)
{
    n00b_alloc_info_t info = n00b_find_alloc_info(p);

    if (!n00b_alloc_info_is_oob(info)) {
        return n00b_alloc_info_inline(info);
    }

    n00b_oob_hdr_t *oob = info.hdr.oob;
    if (!oob->hcur) {
        return n00b_option_none(n00b_inline_hdr_t *);
    }
    return n00b_option_set(n00b_inline_hdr_t *, oob->hcur);
}
