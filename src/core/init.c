#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#ifndef _WIN32
#include <unistd.h>
#include <termios.h>
#include <sys/resource.h>
#else
#include "core/platform.h"
#endif

#define __N00B_THREAD_INTERNAL

#include "n00b.h"
#include "adt/array.h"
#include "adt/option.h"
#include "core/runtime.h"
#include "core/thread.h"
#include "core/mmaps.h"
#include "core/alloc.h"
#include "core/arena.h"
#include "core/random.h"
#include "core/stw.h"
#include "core/gc.h"
#include "core/type_info.h"
#include "text/strings/style_registry.h"
#include "text/strings/theme.h"
#include "core/string.h"
#include "core/buffer.h"
#include "conduit/conduit.h"
#include "conduit/service.h"
#include "conduit/fd_managed.h"
#include "conduit/fd_writer.h"
#include "conduit/io.h"

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

#ifdef _WIN32
static inline void
setup_fd_limit(n00b_runtime_t *rt, int fd_limit)
{
    (void)rt;
    (void)fd_limit;
}
#else
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
#endif

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
n00b_shutdown(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    if (!rt) return;

    n00b_conduit_t *c = rt->default_conduit;
    if (c) {
        n00b_atomic_store(&c->shutdown, true);

        n00b_list_foreach(c->io_backends, p) {
            n00b_conduit_io_backend_t *io = *p;
            if (io) {
                n00b_conduit_io_shutdown(io);
            }
        }
    }

    n00b_stop_the_world();
    n00b_restart_the_world();

    while (n00b_atomic_load(&rt->live_threads) > 1) {
        n00b_futex_wait((n00b_futex_t *)&rt->live_threads,
                         n00b_atomic_load(&rt->live_threads),
                         50000000);
    }

    // Drain any remaining output on tty file descriptors.
#ifndef _WIN32
    if (isatty(STDOUT_FILENO)) {
        tcdrain(STDOUT_FILENO);
    }
    if (isatty(STDERR_FILENO)) {
        tcdrain(STDERR_FILENO);
    }
#endif
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
    static _Atomic bool n00b_init_called = false;
    if (atomic_exchange(&n00b_init_called, true)) {
        return;
    }

#ifdef _WIN32
    n00b_page_size = base_page_size();
#else
    n00b_page_size = sysconf(_SC_PAGESIZE);
#endif

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

    // Initialize the system pool.  This is a system-level allocator
    // (hidden from GC, no STW checks) used to back the root list and
    // lock accounting records so that the collector and other threads
    // can read them safely.
    n00b_pool_init(&rt->system_pool,
                   .__system = true,
                   .hidden   = true,
                   .name     = "system_pool");

    n00b_allocator_t *rpool = (n00b_allocator_t *)&rt->system_pool;
    rt->gc_roots            = n00b_list_new(n00b_gc_root_t, rpool);
    rt->finalizers          = n00b_list_new_private(n00b_finalizer_info_t *, rpool);

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

    n00b_type_registry_init();

    // Conduit infrastructure pool: non-hidden so the GC scans its
    // pages for heap pointers, keeping subscription objects alive.
    // Not __system so STW checks apply normally.
    n00b_allocator_t *cpool = n00b_pool_init(&rt->conduit_pool,
                                              .name = "conduit_pool");

    rt->sub_map = n00b_alloc_with_opts(n00b_dict_untyped_t, &(n00b_alloc_opts_t){.allocator = cpool});
    n00b_dict_untyped_init(rt->sub_map,
                           .allocator     = cpool,
                           .skip_obj_hash = true);

    n00b_gc_register_root(rt->sub_map);

    n00b_str_registry_init();
    n00b_theme_init();

    // Create default conduit + service for IO (stdout/stderr, signals).
    n00b_result_t(n00b_conduit_t *) cond_r = n00b_conduit_new();
    if (n00b_result_is_ok(cond_r)) {
        rt->default_conduit = n00b_result_get(cond_r);

        n00b_result_t(n00b_conduit_service_t *) svc_r =
            n00b_conduit_service_new(rt->default_conduit);

        if (n00b_result_is_ok(svc_r)) {
            rt->default_service = n00b_result_get(svc_r);
            n00b_conduit_service_start(rt->default_service);

            // Get the default IO thread's backend for fd management.
            auto io_opt = n00b_conduit_service_default_io(rt->default_service);

            if (n00b_option_is_set(io_opt)) {
                n00b_conduit_svc_thread_t *svc_thread = n00b_option_get(io_opt);

                auto out_r = n00b_conduit_fd_manage(
                    rt->default_conduit, svc_thread->io, 1, false);
                if (n00b_result_is_ok(out_r)) {
                    rt->stdout_owner = n00b_result_get(out_r);
                }

                auto err_r = n00b_conduit_fd_manage(
                    rt->default_conduit, svc_thread->io, 2, false);
                if (n00b_result_is_ok(err_r)) {
                    rt->stderr_owner = n00b_result_get(err_r);
                }
            }

            // Create typed stdout/stderr buffer topics and wire
            // fd-writer sinks that do the actual kernel write().
            auto *out_typed = n00b_conduit_topic_init(
                n00b_buffer_t *,
                rt->default_conduit,
                n00b_conduit_str_uri(r"stdout"));

            if (out_typed) {
                rt->stdout_topic = (n00b_conduit_topic_base_t *)out_typed;
                n00b_conduit_fd_writer_new(rt->default_conduit,
                                            out_typed, 1);
            }

            auto *err_typed = n00b_conduit_topic_init(
                n00b_buffer_t *,
                rt->default_conduit,
                n00b_conduit_str_uri(r"stderr"));

            if (err_typed) {
                rt->stderr_topic = (n00b_conduit_topic_base_t *)err_typed;
                n00b_conduit_fd_writer_new(rt->default_conduit,
                                            err_typed, 2);
            }
        }
    }

    rt->startup_complete = true;
}
