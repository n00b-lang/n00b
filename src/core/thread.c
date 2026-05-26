#include <pthread.h>
#ifndef _WIN32
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#else
#include "core/platform.h"
#include <io.h>
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#define __N00B_THREAD_INTERNAL

#include "n00b.h"
#include "core/runtime.h"
#include "core/thread.h"
#include "adt/option.h"
#include "core/atomic.h"
#include "core/futex.h"
#include "core/mmaps.h"
#include "core/memory_info.h"
#include "core/lock_common.h"
#include "core/mutex.h"
#include "core/rwlock.h"
#include "core/condition.h"
#include "core/alloc.h"
#include "core/stw.h"

thread_local n00b_thread_t __n00b_thread_self;

static uint32_t
n00b_thread_slot_acquire(n00b_runtime_t *rt, n00b_thread_t *ptr)
{
    uint32_t       candidate;
    n00b_thread_t *expected;

    do {
        candidate = n00b_atomic_add(&rt->next_thread_slot, 1);
        candidate %= rt->max_threads;
        expected = nullptr;
    } while (!n00b_cas(&rt->threads[candidate].thread, &expected, ptr));

    return candidate;
}

void
n00b_thread_init() _kargs
{
    n00b_runtime_t *runtime             = n00b_get_runtime();
    n00b_option_t(pthread_attr_t) attrs = n00b_option_none(pthread_attr_t);
    uint32_t acquired_slot              = 0;
}
{
    if (!acquired_slot) {
        acquired_slot = n00b_thread_slot_acquire(runtime, n00b_thread_self());
    }

    n00b_thread_record_t *rec = &runtime->threads[acquired_slot];
    uint32_t gen = rec->generation++;

    __n00b_thread_self = (n00b_thread_t){
	.pthread_id    = pthread_self(),
	.pthread_attrs = attrs,
	.record        = rec,
	.id_info.parts = {
	    .id         = acquired_slot,
	    .generation = gen,
	},
    };

    // Store the thread pointer into the slot record so that the spawner
    // (or anyone else) can find us via rt->threads[slot].thread.
    n00b_atomic_store(&rec->thread, &__n00b_thread_self);

    n00b_capture_stack_base(&__n00b_thread_self, runtime);
    n00b_capture_stack_top(&__n00b_thread_self);

    n00b_atomic_add(&runtime->live_threads, 1);
    n00b_futex_wake((n00b_futex_t *)&rec->thread, true);
}

static void
n00b_release_locks_on_thread_exit(n00b_thread_record_t *rec)
{
    // Walk exclusive locks and force-release each one.
    n00b_lock_base_t *lock = n00b_atomic_load(&rec->exclusive_locks);

    while (lock) {
        n00b_lock_base_t      *next = n00b_atomic_load(&lock->next_thread_lock);
        n00b_core_lock_info_t  info = n00b_atomic_load(&lock->data);

        info.owner   = N00B_NO_OWNER;
        info.nesting = 0;
        atomic_store(&lock->data, info);
        atomic_store(&lock->prev_thread_lock, nullptr);
        atomic_store(&lock->next_thread_lock, nullptr);

        // If this is a mutex or rwlock, release the futex.
        if (info.type == N00B_NLT_MUTEX) {
            n00b_mutex_t *m = (n00b_mutex_t *)lock;
            atomic_store(&m->futex, 0);
            if (n00b_atomic_load(&m->should_wake)) {
                n00b_futex_wake(&m->futex, true);
            }
        }
        else if (info.type == N00B_NLT_RW) {
            n00b_rwlock_t *rw = (n00b_rwlock_t *)lock;
            n00b_atomic_and(&rw->futex, ~N00B_RW_W_LOCK);
            n00b_futex_wake(&rw->futex, true);
        }

        lock = next;
    }
    n00b_atomic_store(&rec->exclusive_locks, nullptr);

    // Walk read locks and release each one.
    n00b_thread_read_log_t *rlog = n00b_atomic_load(&rec->read_locks);

    while (rlog) {
        n00b_thread_read_log_t *next = rlog->next_entry;
        n00b_rwlock_t          *rw   = rlog->obj;

        if (rw && rlog->level > 0) {
            // Decrement the reader count.
            uint32_t value, desired;
            do {
                value   = n00b_atomic_load(&rw->futex);
                desired = value - 1;
            } while (!n00b_cas(&rw->futex, &value, desired));
        }

        rlog = next;
    }
    n00b_atomic_store(&rec->read_locks, nullptr);
    n00b_atomic_store(&rec->log_alloc_cache, nullptr);
}

void
n00b_thread_destroy(void)
{
    n00b_thread_record_t *rec = __n00b_thread_self.record;

    if (rec) {
        // If this thread is on a CV's waiters list, remove it.
        n00b_condition_t *cv = rec->cv_info.current_cv;
        if (cv) {
            (void)n00b_list_remove_all(cv->waiters, &__n00b_thread_self);
            rec->cv_info.current_cv = nullptr;
        }

        n00b_release_locks_on_thread_exit(rec);
        n00b_atomic_or(&__n00b_thread_self.self_lock, N00B_SUSPEND);
        n00b_atomic_store(&rec->thread, nullptr);
    }

    if (__n00b_thread_self.memperm_pipe.ready) {
#ifdef _WIN32
        _close(__n00b_thread_self.memperm_pipe.fds[0]);
        _close(__n00b_thread_self.memperm_pipe.fds[1]);
#else
        close(__n00b_thread_self.memperm_pipe.fds[0]);
        close(__n00b_thread_self.memperm_pipe.fds[1]);
#endif
    }

    n00b_runtime_t *rt = n00b_get_runtime();
    if (rt) {
        if (__n00b_thread_self.stack_map) {
            n00b_mmap_unregister((void *)__n00b_thread_self.stack_map->start);
            __n00b_thread_self.stack_map = nullptr;
        }

        n00b_atomic_add(&rt->live_threads, -1);
        n00b_futex_wake((n00b_futex_t *)&rt->live_threads, true);
    }
}

bool
n00b_current_thread_stack_contains(void *ptr)
{
    if (ptr == nullptr) {
        return false;
    }

    uintptr_t        p      = (uintptr_t)ptr;
    n00b_thread_t   *thread = n00b_thread_self();
    n00b_mmap_info_t *map   = thread->stack_map;

    if (map && p >= map->start && p < map->end) {
        return true;
    }

#ifdef _WIN32
    NT_TIB   *tib     = (NT_TIB *)NtCurrentTeb();
    uintptr_t highest = (uintptr_t)tib->StackBase;
    uintptr_t lowest  = (uintptr_t)tib->StackLimit;
    return p >= lowest && p < highest;
#elif defined(__APPLE__)
    pthread_t ptid    = pthread_self();
    uintptr_t highest = (uintptr_t)pthread_get_stackaddr_np(ptid);
    size_t    size    = pthread_get_stacksize_np(ptid);
    uintptr_t lowest  = highest - size;
    return p >= lowest && p < highest;
#elif defined(__linux__)
    pthread_attr_t attrs;
    void          *lowest_ptr = nullptr;
    size_t         size       = 0;

    if (pthread_getattr_np(pthread_self(), &attrs) != 0) {
        return false;
    }

    int rc = pthread_attr_getstack(&attrs, &lowest_ptr, &size);
    pthread_attr_destroy(&attrs);

    if (rc != 0) {
        return false;
    }

    uintptr_t lowest  = (uintptr_t)lowest_ptr;
    uintptr_t highest = lowest + size;
    return p >= lowest && p < highest;
#elif defined(__FreeBSD__)
    pthread_attr_t attrs;
    void          *lowest_ptr = nullptr;
    size_t         size       = 0;

    if (pthread_attr_get_np(pthread_self(), &attrs) != 0) {
        return false;
    }

    if (pthread_attr_getstackaddr(&attrs, &lowest_ptr) != 0
        || pthread_attr_getstacksize(&attrs, &size) != 0) {
        return false;
    }

    uintptr_t lowest  = (uintptr_t)lowest_ptr;
    uintptr_t highest = lowest + size;
    return p >= lowest && p < highest;
#else
    return false;
#endif
}

void
n00b_capture_stack_base(n00b_thread_t *thread, n00b_runtime_t *runtime)
{
    size_t size;
    char  *highest;
    char  *lowest;

#ifdef _WIN32
    // On Windows, use the Thread Environment Block for all threads.
    (void)runtime;
    {
        NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
        highest = (char *)tib->StackBase;
        lowest  = (char *)tib->StackLimit;
        size    = highest - lowest;
    }
#else
    if (!n00b_atomic_load(&runtime->live_threads)) {
#ifdef __APPLE__
        // pthread_get_stackaddr_np and the env-walking heuristic both
        // miss the main thread's true stack top on macOS — pthread
        // reports the pthread-managed region (below where the kernel
        // placed argv/envp and main()'s frame), and env-walking only
        // covers the env-string area, which isn't always the
        // highest-addressed page in the stack mapping.  Either way,
        // any local in main's frame can end up *above* the registered
        // stack_map range, and the GC's stack-root scan misses it.
        // The mach_vm region containing a known stack address IS the
        // full stack, so use that.
        // Pick an anchor that's actually inside the main thread's
        // real stack region.  On macOS the kernel-set-up main stack
        // is *above* what pthread_get_stackaddr_np reports, and also
        // above where the argv/envp strings live, so a local in this
        // function (in the pthread region) and argv[0] / envp[0] (in
        // the strings region) both miss it.  The argv array itself
        // (`runtime->argv.data` — the C `argv` pointer value passed to
        // main) lives at the very top of main's actual stack frame
        // area, so that's the anchor that gets mach_vm_region_recurse
        // to return the right region.
        char anchor;
        char *anchor_p = (char *)runtime->argv.data;
        if (!anchor_p) {
            anchor_p = (char *)&anchor;
        }
        mach_vm_address_t region_addr = (mach_vm_address_t)anchor_p;
        mach_vm_size_t    region_size = 0;
        natural_t                       depth = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t          info_count = VM_REGION_SUBMAP_INFO_COUNT_64;
        kern_return_t                   kr;
        kr = mach_vm_region_recurse(mach_task_self(), &region_addr,
                                    &region_size, &depth,
                                    (vm_region_recurse_info_t)&info,
                                    &info_count);
        (void)0;
        if (kr == KERN_SUCCESS) {
            lowest  = (char *)(uintptr_t)region_addr;
            highest = lowest + region_size;
            size    = region_size;
        }
        else {
            pthread_t ptid = pthread_self();
            highest        = pthread_get_stackaddr_np(ptid);
            size           = pthread_get_stacksize_np(ptid);
            lowest         = highest - size;
        }
#else
        struct rlimit rlimit;
        getrlimit(RLIMIT_STACK, &rlimit);
        size = rlimit.rlim_cur;
        extern char **environ;
        char        **env = environ;
        // Stop at the top string.
        while (env[1]) {
            env++;
        }
        // Find the very end, then align it.
        char *p = *env + 1;
        highest = p + strlen(p) + 1 + sizeof(void *);
        highest = (char *)(((uint64_t)highest) & ~(sizeof(void *) - 1));
        lowest  = highest - size;
#endif
    }
    else {
#if defined(__linux__)
        pthread_getattr_np(thread->pthread_id, &thread->pthread_attrs.value);
        thread->pthread_attrs.has_value = true;
        // Pthreads reports the lowest address, not the highest.
        pthread_attr_getstack(&thread->pthread_attrs.value, (void **)&lowest, &size);
        highest = lowest + size;
#elifdef __APPLE__
        pthread_t ptid = pthread_self();
        highest        = pthread_get_stackaddr_np(ptid);
        size           = pthread_get_stacksize_np(ptid);
        lowest         = highest - size;
#elifdef __FreeBSD__
        pthread_attr_get_np(pthread_self(), &thread->pthread_attrs);
        pthread_attr_getstackaddr(&thread->pthread_attrs, (void **)&lowest);
        pthread_attr_getstacksize(&thread->pthread_attrs, &size);
        highest = lowest + size;
#else
#error "Don't know how to find pthread stack bounds on this platform"
#endif
    }
#endif

    thread->stack_base = highest;
    thread->stack_map  = n00b_option_get(
        n00b_mmap_register(lowest, highest, n00b_mmap_stack));
}

// ============================================================================
// Thread spawn / join
// ============================================================================

typedef struct {
    void *(*fn)(void *);
    void              *arg;
    uint32_t           tid;     // pre-reserved slot
    n00b_futex_t       ready;   // launcher signals "initialized"
    void              *result;  // return value from fn
} n00b_tbundle_t;

static void *
n00b_thread_launcher(void *raw)
{
    n00b_tbundle_t *bundle = raw;
    n00b_runtime_t *rt     = n00b_get_runtime();

    n00b_thread_init(.runtime = rt, .acquired_slot = bundle->tid);
    n00b_capture_stack_top(&__n00b_thread_self);

    // Cache fn/arg locally before signalling the spawner.  Once we set
    // bundle->ready, the spawner may continue and the bundle (allocated in
    // the pinned system_pool — see n00b_thread_spawn) is no longer
    // strictly needed by either side; freeing it after the worker call
    // closes the leak introduced by moving allocation out of the GC arena.
    void *(*fn)(void *) = bundle->fn;
    void   *arg         = bundle->arg;

    // Signal spawner that we are initialized.
    n00b_atomic_store(&bundle->ready, 1);
    n00b_futex_wake(&bundle->ready, true);

    void *result   = fn(arg);
    bundle->result = result;

    n00b_free(bundle);
    n00b_thread_destroy();
    return result;
}

n00b_result_t(n00b_thread_t *)
n00b_thread_spawn(void *(*fn)(void *), void *arg)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    if (!rt) return n00b_result_err(n00b_thread_t *, ENXIO);

    // Pre-acquire a thread slot so the launcher can use it directly.
    // We use a temporary pointer that the slot_acquire CAS needs.
    // We'll fix up the back-pointer once the child thread initializes.
    n00b_thread_t *placeholder = (n00b_thread_t *)(uintptr_t)1;
    uint32_t       slot        = n00b_thread_slot_acquire(rt, placeholder);

    // Allocate the bundle from system_pool (pinned, non-movable) rather
    // than the GC default arena.  Between pthread_create() and the worker's
    // call to n00b_thread_init(), the new thread holds `bundle` in a
    // register that the GC's stack-scan cannot see (the thread isn't
    // registered with the runtime yet).  If GC fires on any other thread
    // during that window and moves the bundle in the default arena, the
    // worker's register copy goes stale — its first deref of `bundle->fn`
    // or `bundle->arg` then reads garbage and dispatches into nowhere.
    // The system_pool never moves, closing this race.
    n00b_tbundle_t *bundle = n00b_alloc_with_opts(
        n00b_tbundle_t,
        &(n00b_alloc_opts_t){.allocator = (n00b_allocator_t *)&rt->system_pool});
    if (!bundle) {
        n00b_atomic_store(&rt->threads[slot].thread, (n00b_thread_t *)nullptr);
        return n00b_result_err(n00b_thread_t *, ENOMEM);
    }

    bundle->fn    = fn;
    bundle->arg   = arg;
    bundle->tid   = slot;
    n00b_futex_init(&bundle->ready);

    pthread_t ptid;
    int       rc = pthread_create(&ptid, nullptr, n00b_thread_launcher, bundle);
    if (rc != 0) {
        n00b_atomic_store(&rt->threads[slot].thread, (n00b_thread_t *)nullptr);
        return n00b_result_err(n00b_thread_t *, rc);
    }

    // Wait for the child to finish n00b_thread_init.
    while (!n00b_atomic_load(&bundle->ready)) {
        n00b_stw_suspend_ctx stw_ctx = {0};

        n00b_thread_suspend(stw_ctx);
        n00b_futex_wait(&bundle->ready, 0, 100000000); // 100ms
        n00b_thread_resume(stw_ctx);
    }

    // Now rt->threads[slot].thread points to the child's TLS n00b_thread_t.
    n00b_thread_t *child = n00b_atomic_load(&rt->threads[slot].thread);

    // Store the pthread_t so join works even if the TLS pointer is
    // only valid while the child is alive.
    if (child) {
        child->pthread_id = ptid;
    }

    return n00b_result_ok(n00b_thread_t *, child);
}

void *
n00b_thread_join(n00b_thread_t *thread)
{
    if (!thread) return nullptr;

    void *retval = nullptr;

    n00b_stw_suspend_ctx stw_ctx = {0};

    n00b_thread_suspend(stw_ctx);
    pthread_join(thread->pthread_id, &retval);
    n00b_thread_resume(stw_ctx);

    return retval;
}
