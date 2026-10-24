#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

extern _Atomic(void *) worker_heap
    __asm("_$bitcode_gc_worker_live$worker_heap");
extern void gc_worker(void *) __asm("_sys.internal.gc.collect.worker");
extern void n00b_abi_tls_relocate(void);

static pthread_t thread;
static _Atomic bool stop_wait;

static void *run_worker(void *unused)
{
    (void)unused;
    void *heap = NULL;
    while (!(heap = atomic_load_explicit(&worker_heap, memory_order_acquire))) {
        if (atomic_load_explicit(&stop_wait, memory_order_acquire)) return NULL;
        sched_yield();
    }
    n00b_abi_tls_relocate();
    gc_worker(heap);
    return NULL;
}

__attribute__((constructor)) static void start_worker(void)
{
    if (pthread_create(&thread, NULL, run_worker, NULL) != 0) abort();
}

__attribute__((destructor)) static void join_worker(void)
{
    atomic_store_explicit(&stop_wait, true, memory_order_release);
    if (pthread_join(thread, NULL) != 0) abort();
}
