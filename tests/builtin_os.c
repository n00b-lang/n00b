#include "builtin_os.h"

#include <assert.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

enum { TEST_TIMEOUT_NS = 2000000000 };

static atomic_uint_fast64_t worker_control;
static atomic_uint_fast64_t worker_stack_high;
static atomic_uint_fast64_t worker_ticks;
static atomic_bool worker_done;

#if defined(_WIN32)
static DWORD WINAPI stw_worker(void *unused)
#else
static void *stw_worker(void *unused)
#endif
{
    (void)unused;
    char stack_anchor;
    atomic_store(&worker_stack_high,
                 (uintptr_t)n00b_os_thread_stack_high_self());
    assert(atomic_load(&worker_stack_high) > (uintptr_t)&stack_anchor);
    atomic_store(&worker_control, n00b_os_thread_control_self());
    while (!atomic_load(&worker_done))
        atomic_fetch_add(&worker_ticks, 1);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void test_thread_suspend(void)
{
#if defined(_WIN32)
    HANDLE worker = CreateThread(NULL, 0, stw_worker, NULL, 0, NULL);
    assert(worker != NULL);
#else
    pthread_t worker;
    assert(pthread_create(&worker, NULL, stw_worker, NULL) == 0);
#endif
    uint64_t started = n00b_os_now_ns();
    while (!atomic_load(&worker_control) || atomic_load(&worker_ticks) < 1000) {
        assert(n00b_os_now_ns() - started < TEST_TIMEOUT_NS);
        n00b_os_thread_yield();
    }
    uint64_t control = atomic_load(&worker_control);
    n00b_os_thread_context context = {0};
    assert(n00b_os_thread_suspend(control, &context, TEST_TIMEOUT_NS) == 0);
    assert(context.sp != 0 && context.pc != 0 && context.nregs != 0);
    uint64_t stopped_ticks = atomic_load(&worker_ticks);
    uint64_t pause_start = n00b_os_now_ns();
    while (n00b_os_now_ns() - pause_start < 1000000)
        n00b_os_cpu_relax();
    assert(atomic_load(&worker_ticks) == stopped_ticks);
    assert(n00b_os_thread_resume(control, &context) == 0);
    started = n00b_os_now_ns();
    while (atomic_load(&worker_ticks) == stopped_ticks) {
        assert(n00b_os_now_ns() - started < TEST_TIMEOUT_NS);
        n00b_os_thread_yield();
    }
    atomic_store(&worker_done, true);
#if defined(_WIN32)
    assert(WaitForSingleObject(worker, INFINITE) == WAIT_OBJECT_0);
    CloseHandle(worker);
#else
    assert(pthread_join(worker, NULL) == 0);
#endif
}

int main(void)
{
    size_t page = n00b_os_page_size();
    char stack_anchor;
    assert((uintptr_t)n00b_os_thread_stack_high_self()
           > (uintptr_t)&stack_anchor);
#if defined(__x86_64__) || (defined(__APPLE__) && defined(__aarch64__))
    assert(n00b_os_stack_red_zone_bytes() == 128);
#else
    assert(n00b_os_stack_red_zone_bytes() == 0);
#endif
    assert(page && !(page & (page - 1)));
    assert(n00b_os_thread_id() != 0);
    assert(n00b_os_thread_id() == n00b_os_thread_id());
    n00b_os_thread_context self = {0};
    n00b_os_thread_capture_self(&self);
    assert(self.sp && self.fp && self.pc && self.nregs != 0);
    assert(self.nregs <= N00B_OS_CONTEXT_REGS);
    uint64_t before = n00b_os_now_ns();
    uint64_t after = n00b_os_now_ns();
    assert(before && after >= before);

    void *ordinary = n00b_os_map_zeroed(page, 0);
    assert(ordinary && !((uintptr_t)ordinary & (page - 1)));
    for (size_t i = 0; i < page; i++)
        assert(((unsigned char *)ordinary)[i] == 0);
    ((unsigned char *)ordinary)[0] = 42;
    assert(n00b_os_protect(ordinary, page, N00B_OS_PROT_READ) == 0);
    assert(((unsigned char *)ordinary)[0] == 42);
    assert(n00b_os_protect(ordinary, page,
                           N00B_OS_PROT_READ | N00B_OS_PROT_WRITE) == 0);
    assert(n00b_os_unmap(ordinary, page) == 0);

    size_t alignment = page > 32768 ? page : 32768;
    void *aligned = n00b_os_map_zeroed(alignment * 2, alignment);
    assert(aligned && !((uintptr_t)aligned & (alignment - 1)));
    assert(n00b_os_unmap(aligned, alignment * 2) == 0);

    assert(n00b_os_map_zeroed(0, 0) == NULL);
    assert(n00b_os_map_zeroed(page, 3) == NULL);
    test_thread_suspend();
    return 0;
}
