/**
 * Futex-based mutex implementation.
 *
 * Spins briefly (N00B_SPIN_LIMIT iterations) before falling back to
 * an OS-level futex wait.  Supports recursive locking via the lock
 * accounting subsystem.
 */

#define N00B_USE_INTERNAL_API

#include <setjmp.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/thread.h"
#include "core/mutex.h"
#include "core/stw.h"
#include "core/atomic.h"

#define MUTEX_UNLOCKED 0
#define MUTEX_LOCKED   1

void
_n00b_mutex_init(n00b_mutex_t *lock, char *loc)
{
    n00b_lock_init_accounting((void *)lock, N00B_NLT_MUTEX, loc);
    n00b_futex_init(&lock->futex);
}

void
n00b_sys_mutex_init(n00b_mutex_t *lock, char *loc)
{
    memset(lock, 0, sizeof(n00b_mutex_t));
    n00b_futex_init(&lock->futex);
    lock->no_log = true;
    lock->inited = true;
}

static inline bool
spin_phase(n00b_mutex_t *mutex, char *loc)
{
    n00b_thread_t *thread = n00b_thread_self();

    int32_t               tid  = thread->id_info.parts.id;
    n00b_core_lock_info_t info = n00b_atomic_load(&mutex->data);

    n00b_mac_barrier();
    if (info.owner == tid) {
        n00b_lock_acquire_accounting((void *)mutex, thread, loc);
        return true;
    }

    for (int i = 0; i < N00B_SPIN_LIMIT; i++) {
        if (!n00b_atomic_or(&mutex->futex, 1)) {
            n00b_lock_acquire_accounting((void *)mutex, thread, loc);
            return true;
        }
    }

    return false;
}

#if defined(N00B_DEBUG)
static inline void
ensure_ownership(n00b_mutex_t *mutex)
{
    n00b_mac_barrier();

    n00b_thread_t        *thread = n00b_thread_self();
    n00b_core_lock_info_t info   = n00b_atomic_load(&mutex->data);

    assert(info.owner == thread->id_info.parts.id);
}
#else
#define ensure_ownership(x)
#endif

int
_n00b_mutex_lock(n00b_mutex_t *mutex, char *loc)
{
    const void *jumps[2] = {&&lock_off, &&lock_on};
    goto *(jumps[n00b_atomic_load(&n00b_get_runtime()->startup_complete)]);
lock_on:
    if (spin_phase(mutex, loc)) {
        ensure_ownership(mutex);
        return 0;
    }

    n00b_thread_t       *thread = n00b_thread_self();
    n00b_stw_suspend_ctx stw_ctx;

    n00b_atomic_add(&mutex->should_wake, 1);
    n00b_thread_suspend(stw_ctx);
    n00b_register_lock_wait(thread, mutex, loc);

    do {
        n00b_futex_wait_for_value(&mutex->futex, 0);
    } while (n00b_atomic_or(&mutex->futex, 1));

    n00b_atomic_add(&mutex->should_wake, -1);
    n00b_wait_done(thread);
    n00b_mac_barrier();

    n00b_lock_acquire_accounting((void *)mutex, thread, loc);
    n00b_thread_resume(stw_ctx);

    ensure_ownership(mutex);

lock_off:
    return 0;
}

bool
_n00b_mutex_try_lock(n00b_mutex_t *mutex, int usec, char *loc)
{
    const void *jumps[2] = {&&lock_off, &&lock_on};
    goto *(jumps[n00b_atomic_load(&n00b_get_runtime()->startup_complete)]);
lock_on:
    if (spin_phase(mutex, loc)) {
        ensure_ownership(mutex);
        return true;
    }

    n00b_thread_t       *thread = n00b_thread_self();
    n00b_stw_suspend_ctx stw_ctx;

    n00b_atomic_add(&mutex->should_wake, 1);
    n00b_thread_suspend(stw_ctx);
    n00b_register_lock_wait(thread, mutex, loc);

    do {
        n00b_futex_timed_wait_for_value(&mutex->futex, 0, usec);
    } while (n00b_atomic_or(&mutex->futex, 1));

    n00b_atomic_add(&mutex->should_wake, -1);
    n00b_wait_done(thread);
    n00b_thread_resume(stw_ctx);

    n00b_mac_barrier();

    n00b_lock_acquire_accounting((void *)mutex, thread, loc);
    ensure_ownership(mutex);
lock_off:
    return true;
}

bool
_n00b_mutex_unlock(n00b_mutex_t *mutex, char *loc)
{
    const void *jumps[2] = {&&lock_off, &&lock_on};
    goto *(jumps[n00b_atomic_load(&n00b_get_runtime()->startup_complete)]);
lock_on:
    if (!n00b_lock_release_accounting((void *)mutex, loc)) {
        return false;
    }

    atomic_store(&mutex->futex, 0);

    if (n00b_atomic_load(&mutex->should_wake)) {
        n00b_futex_wake(&mutex->futex, false);
    }
lock_off:
    return true;
}
