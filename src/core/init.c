#include <stdio.h> // perror
#include <locale.h>
#include <unistd.h>
#include <sys/resource.h>

#define __N00B_THREAD_INTERNAL

#include "n00b.h"
#include "core/array.h"
#include "core/option.h"
#include "core/runtime.h"
#include "core/thread.h"
#include "core/mmaps.h"
#include "core/alloc.h"
#include "core/arena.h"
#include "core/random.h"
#include "core/stw.h"
#include "core/gc.h"
#include "strings/style_registry.h"
#include "strings/theme.h"

size_t   n00b_page_size = 0;
uint64_t n00b_gc_guard  = 0;

extern void n00b_mmaps_initialize(n00b_mmap_ctx_t *ctx);

typedef n00b_option_t(n00b_runtime_t *) opt_rt_t;
opt_rt_t n00b_default_runtime = n00b_option_none(n00b_runtime_t *);

static inline void
setup_envp(n00b_runtime_t *rt, char *envp[])
{
    if (envp == nullptr) {
        rt->envp = n00b_array_checked_ptr(char *, 0, nullptr);
        return;
    }

    int   i = 0;
    char *p = (char *)envp;
    while (*p) {
        i++;
        p++;
    }
    rt->envp = n00b_array_checked_ptr(char *, i, envp);
}

static inline void
setup_fd_limit(n00b_runtime_t *rt, rlim_t fd_limit)
{
    struct rlimit limits;

    if (fd_limit < 0) {
        return;
    }

    if (getrlimit(RLIMIT_NOFILE, &limits)) {
        perror(__func__);
        exit(1);
    }

    if (!fd_limit || ((size_t)fd_limit) > limits.rlim_max) {
        fd_limit = limits.rlim_max;
    }
    limits.rlim_cur = fd_limit;

    if (setrlimit(RLIMIT_NOFILE, &limits)) {
        perror(__func__);
        exit(1);
    }
}

static inline void
setup_threads(n00b_runtime_t *rt, unsigned int max_threads)
{
    // TODO: Dynamic value for max_threads

    rt->next_thread_slot = 0;
    n00b_thread_init(.runtime = rt);
}

static void *
n00b_slab_alloc(n00b_allocator_t *self, uint32_t sz, void *ignore)
{
    auto r = n00b_mmap(sz, .allocator = self);
    assert(n00b_result_is_ok(r));
    return n00b_result_get(r);
}

void
n00b_slab_free(n00b_allocator_t *self, void *ptr)
{
    (void)n00b_munmap(ptr);
}

static inline void
setup_slab_allocator(n00b_runtime_t *rt)
{
    n00b_allocator_setup((n00b_allocator_t *)&rt->slab_allocator,
                         (n00b_calloc_fn)n00b_slab_alloc,
                         .free              = (n00b_free_fn)n00b_slab_free,
                         .name              = "slab_allocator",
                         .inline_headers    = false,
                         .external_metadata = false,
                         .hidden            = true,
                         .__system          = true);
}

void
n00b_init(n00b_runtime_t *rt, int argc, char *argv[]) _kargs
{
    n00b_allocator_t *allocator       = nullptr;
    char             **envp           = nullptr;
    char              *numeric_locale = "";
    int                fd_limit       = 0;
    unsigned int       max_threads    = N00B_THREADS_MAX;
}
{
    n00b_page_size = sysconf(_SC_PAGESIZE);

    if (!n00b_gc_guard) {
        n00b_gc_guard = n00b_rand64();
    }

    assert(rt);
    *rt = (n00b_runtime_t){
        .stw         = N00B_NO_OWNER,
        .stw_nesting = 0,
    };

    if (!n00b_option_is_set(n00b_default_runtime)) {
        n00b_default_runtime = n00b_option_set(n00b_runtime_t *, rt);
    }

    if (numeric_locale) {
        setlocale(LC_NUMERIC, numeric_locale);
    }
    rt->argv = n00b_array_checked_ptr(char *, argc, argv);
    setup_envp(rt, envp);
    setup_fd_limit(rt, fd_limit);

    setup_slab_allocator(rt);
    n00b_mmaps_initialize(&rt->mmaps);

    // Initialize the GC root pool and root list.  The pool is a
    // system-level allocator (hidden from GC, no STW checks) used to
    // back the root list so that the collector can read it safely.
    n00b_pool_init(&rt->gc_root_pool,
                   .__system = true,
                   .hidden   = true,
                   .name     = "gc_roots");

    n00b_allocator_t *rpool = (n00b_allocator_t *)&rt->gc_root_pool;
    rt->gc_roots            = n00b_list_new(n00b_gc_root_t, rpool);

    setup_threads(rt, max_threads);

    if (allocator) {
        rt->default_allocator = allocator;
        rt->default_arena     = nullptr;
    }
    else {
        rt->default_arena     = n00b_new_arena(.use_gc = true,
                                                .name   = "default");
        rt->default_allocator = (n00b_allocator_t *)rt->default_arena;
    }

    n00b_str_registry_init();
    n00b_theme_init();

    rt->startup_complete = true;
}
