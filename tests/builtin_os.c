#include "builtin_os.h"

#include <assert.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

// Suspending and resuming threads is n00b code now; its test is
// tests/programs/os/os_threads.n. This checks the stack bound on a thread
// other than the main one.

static atomic_uint_fast64_t worker_stack_high;

#if defined(_WIN32)
static DWORD WINAPI stack_worker(void *unused)
#else
static void *stack_worker(void *unused)
#endif
{
    (void)unused;
    char stack_anchor;
    atomic_store(&worker_stack_high,
                 (uintptr_t)n00b_os_thread_stack_high_self());
    assert(atomic_load(&worker_stack_high) > (uintptr_t)&stack_anchor);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void test_thread_stack(void)
{
#if defined(_WIN32)
    HANDLE worker = CreateThread(NULL, 0, stack_worker, NULL, 0, NULL);
    assert(worker != NULL);
    assert(WaitForSingleObject(worker, INFINITE) == WAIT_OBJECT_0);
    CloseHandle(worker);
#else
    pthread_t worker;
    assert(pthread_create(&worker, NULL, stack_worker, NULL) == 0);
    assert(pthread_join(worker, NULL) == 0);
#endif
    assert(atomic_load(&worker_stack_high) != 0);
}

// The page size, from the platform: the runtime's own is n00b code now.
static size_t page_size(void)
{
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (size_t)info.dwPageSize;
#else
    long size = sysconf(_SC_PAGESIZE);
    return size > 0 ? (size_t)size : 0;
#endif
}

int main(void)
{
    size_t page = page_size();
    char stack_anchor;
    assert((uintptr_t)n00b_os_thread_stack_high_self()
           > (uintptr_t)&stack_anchor);
#if (defined(__x86_64__) && !defined(_WIN32)) \
    || (defined(__APPLE__) && defined(__aarch64__))
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
    assert(((unsigned char *)ordinary)[0] == 42);
    assert(n00b_os_unmap(ordinary, page) == 0);

    size_t alignment = page > 32768 ? page : 32768;
    void *aligned = n00b_os_map_zeroed(alignment * 2, alignment);
    assert(aligned && !((uintptr_t)aligned & (alignment - 1)));
    assert(n00b_os_unmap(aligned, alignment * 2) == 0);

    assert(n00b_os_map_zeroed(0, 0) == NULL);
    assert(n00b_os_map_zeroed(page, 3) == NULL);
    test_thread_stack();
    return 0;
}
