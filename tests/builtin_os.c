#include "builtin_os.h"

#include <assert.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

// Thread control and the stack queries are n00b code now; their tests are
// tests/programs/os/os_threads.n. This checks what the runtime keeps in C.

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
    return 0;
}
