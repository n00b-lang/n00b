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
    } while (!n00b_cas(&rt->thread_list[candidate], &expected, ptr));

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

    __n00b_thread_self = (n00b_thread_t){
	.pthread_id    = pthread_self(),
	.pthread_attrs = attrs,
	.id_info.parts = {
	    .id         = acquired_slot,
	    .generation = runtime->thread_generations[acquired_slot]++,
	},
    };

    n00b_capture_stack_base(&__n00b_thread_self, runtime);
    n00b_capture_stack_top(&__n00b_thread_self);

    n00b_atomic_add(&runtime->live_threads, 1);
    n00b_futex_wake((n00b_futex_t *)&runtime->thread_list[acquired_slot], true);
}

void
n00b_thread_destroy(void)
{
    if (__n00b_thread_self.memperm_pipe.ready) {
        close(__n00b_thread_self.memperm_pipe.fds[0]);
        close(__n00b_thread_self.memperm_pipe.fds[1]);
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
#elif defined(__APPLE__)
        pthread_t ptid = pthread_self();
        highest        = pthread_get_stackaddr_np(ptid);
        size           = pthread_get_stacksize_np(ptid);
        lowest         = highest - size;
#elif defined(__FreeBSD__)
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
