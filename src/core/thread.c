#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#define __N00B_THREAD_INTERNAL

#include "n00b.h"
#include "core/runtime.h"
#include "core/thread.h"
#include "core/option.h"
#include "core/atomic.h"
#include "core/futex.h"
#include "core/mmaps.h"
#include "core/memory_info.h"
#include "core/lock_common.h"
#include "core/mutex.h"
#include "core/rwlock.h"

thread_local n00b_thread_t __n00b_thread_self;

static uint32_t
n00b_thread_slot_acquire(n00b_runtime_t *rt, n00b_thread_t *ptr)
{
    uint32_t       candidate;
    n00b_thread_t *expected;

    do {
        candidate = n00b_atomic_add(&rt->next_thread_slot, 1);
        candidate %= N00B_THREADS_MAX;
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
        n00b_release_locks_on_thread_exit(rec);
        n00b_atomic_store(&rec->thread, nullptr);
    }

    if (__n00b_thread_self.memperm_pipe.ready) {
        close(__n00b_thread_self.memperm_pipe.fds[0]);
        close(__n00b_thread_self.memperm_pipe.fds[1]);
    }

    n00b_runtime_t *rt = n00b_get_runtime();
    if (rt) {
        n00b_atomic_add(&rt->live_threads, -1);
    }
}

void
n00b_capture_stack_base(n00b_thread_t *thread, n00b_runtime_t *runtime)
{
    size_t size;
    char  *highest;
    char  *lowest;

    if (!n00b_atomic_load(&runtime->live_threads)) {
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
    }
    else {
#if defined(__linux__)
        pthread_getattr_np(thread->pthread_id, &thread->pthread_attrs);
        // Pthreads reports the lowest address, not the highest.
        pthread_attr_getstack(&thread->pthread_attrs, (void **)&lowest, &size);
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

    thread->stack_base = highest;
    thread->stack_map  = n00b_option_get(
        n00b_mmap_register(lowest, highest, n00b_mmap_stack));
}
