/*
 * Futex-based reader-writer lock implementation.
 *
 * The futex word encodes both the reader count (low 30 bits) and the
 * writer-lock bit (bit 30, N00B_RW_W_LOCK).  Writers set the W_LOCK bit
 * to block new readers, then wait for the reader count to reach zero.
 *
 * Reader-to-writer upgrade is supported: the upgrading reader first
 * decrements the reader count (to avoid self-deadlock), then competes
 * for the write bit normally.
 */

#define N00B_USE_INTERNAL_API

#include "n00b.h"
#include "core/runtime.h"
#include "core/thread.h"
#include "core/rwlock.h"
#include "core/stw.h"
#include "core/alloc.h"
#include "core/atomic.h"

static n00b_thread_read_log_t *
find_read_lock_record(n00b_rwlock_t *lock, n00b_thread_t *thread)
{
    n00b_thread_record_t   *rec = thread->record;
    n00b_thread_read_log_t *log = n00b_atomic_load(&rec->read_locks);

    while (log != nullptr) {
        if (log->obj == lock) {
            return log;
        }
        log = log->next_entry;
    }

    return nullptr;
}

static inline n00b_thread_read_log_t *
acquire_read_record(n00b_rwlock_t *lock, n00b_thread_t *thread)
{
    n00b_thread_record_t   *rec = thread->record;
    n00b_thread_read_log_t *log;
    n00b_thread_read_log_t *prev;

    if (n00b_atomic_load(&rec->log_alloc_cache)) {
        log = n00b_atomic_load(&rec->log_alloc_cache);
        n00b_atomic_store(&rec->log_alloc_cache, log->next_entry);

        if (n00b_atomic_load(&rec->log_alloc_cache)) {
            n00b_atomic_load(&rec->log_alloc_cache)->prev_entry = nullptr;
        }

        log->next_entry = nullptr;
    }
    else {
        n00b_runtime_t   *rt = n00b_get_runtime();
        n00b_allocator_t *sp = (n00b_allocator_t *)&rt->system_pool;

        log = n00b_alloc_with_opts(n00b_thread_read_log_t,
                                   &(n00b_alloc_opts_t){.allocator = sp});
    }
    log->obj   = lock;
    log->level = 0;
    prev       = n00b_atomic_load(&rec->read_locks);

    log->next_entry = prev;
    n00b_atomic_store(&rec->read_locks, log);

    if (prev) {
        prev->prev_entry = log;
    }

    return log;
}

static void
register_read(n00b_rwlock_t          *lock,
              n00b_thread_t          *thread,
              int                     value,
              n00b_thread_read_log_t *log,
              char                   *loc)
{
    if (!log) {
        log = acquire_read_record(lock, thread);
    }

    log->level++;
    _n00b_rlock_accounting(lock, log, thread, value, loc);
}

void
_n00b_rw_init(n00b_rwlock_t *lock, char *loc)
{
    n00b_lock_init_accounting((void *)lock, N00B_NLT_RW, loc);
    n00b_futex_init(&lock->futex);
}

int
_n00b_rw_write_lock(n00b_rwlock_t *lock, char *loc)
{
    n00b_thread_t          *thread    = n00b_thread_self();
    int32_t                 tid       = thread->id_info.parts.id;
    n00b_core_lock_info_t   info      = n00b_atomic_load(&lock->data);
    n00b_thread_read_log_t *record    = find_read_lock_record(lock, thread);
    bool                    upgrading = false;
    uint32_t                value;
    n00b_stw_suspend_ctx    stw_ctx;

    if (info.owner == tid) {
        goto post_resume;
    }

    if (record != nullptr) {
        upgrading = true;

        // Remove ourselves as a reader to avoid self-deadlock on upgrade.
        volatile uint32_t desired;

        do {
            value   = n00b_atomic_load(&lock->futex);
            desired = value - 1;
        } while (!n00b_cas(&lock->futex, &value, desired));
    }

    n00b_thread_suspend(stw_ctx);

    // Compete for the write bit.
    value = n00b_atomic_or(&lock->futex, N00B_RW_W_LOCK);

    while (value & N00B_RW_W_LOCK) {
        n00b_register_lock_wait(thread, lock, loc);
        n00b_futex_wait_for_value(&lock->futex, N00B_RW_UNLOCKED);
        value = n00b_atomic_or(&lock->futex, N00B_RW_W_LOCK);
        n00b_wait_done(thread);
    }

    // Wait for readers to drain.
    if (value) {
        n00b_register_lock_wait(thread, lock, loc);
        assert(lock);

        n00b_futex_wait_for_value(&lock->futex, N00B_RW_W_LOCK);
        n00b_barrier();
        n00b_wait_done(thread);
    }

    if (upgrading) {
        n00b_atomic_add(&lock->futex, 1);
    }

    n00b_thread_resume(stw_ctx);

post_resume:

{
    int result = n00b_lock_acquire_accounting((void *)lock, thread, loc);

    info = n00b_atomic_load(&lock->data);
    assert(info.owner == tid);

    return result;
}
}

void
_n00b_rw_read_lock(n00b_rwlock_t *lock, char *loc)
{
    n00b_thread_t          *thread  = n00b_thread_self();
    n00b_core_lock_info_t   info    = n00b_atomic_load(&lock->data);
    n00b_thread_read_log_t *record  = find_read_lock_record(lock, thread);
    uint32_t                value   = 0;
    volatile uint32_t       desired = 0;

    n00b_barrier();

    if (info.owner == (int32_t)thread->id_info.parts.id) {
        n00b_lock_acquire_accounting((void *)lock, thread, loc);
        return;
    }

    // Fast path for nested reads.
    if (record) {
        register_read(lock, thread, -1, record, loc);
        return;
    }

    // Fast path: no contention.
    if (n00b_cas(&lock->futex, &value, 1)) {
        register_read(lock, thread, desired, nullptr, loc);
        return;
    }

    n00b_barrier();

    n00b_stw_suspend_ctx stw_ctx;
    n00b_thread_suspend(stw_ctx);

    value = n00b_atomic_load(&lock->futex);
    while (true) {
        if (value & N00B_RW_W_LOCK) {
            n00b_register_lock_wait(thread, lock, loc);
            n00b_futex_wait(&lock->futex, value, 0);
            n00b_wait_done(thread);

            value = n00b_atomic_load(&lock->futex);
            continue;
        }

        /* Bugfix: the previous `do { ... } while (cas(..., desired))`
         * form skipped this assignment when the W_LOCK branch did
         * `continue` — `continue` in a do-while jumps to the loop
         * condition, so the cas would run with the *previous*
         * iteration's `desired` (or the initial 0 on first pass).
         * That caused two failure modes:
         *   1. After waking from a W_LOCK wait, the cas would attempt
         *      `value -> 0`, swallowing the reader-count increment.
         *   2. The subsequent reader-unlock would underflow the count
         *      to UINT_MAX, pinning W_LOCK + a huge reader count and
         *      deadlocking every later acquirer.
         * Recomputing `desired` immediately before each cas attempt
         * keeps the count math correct. */
        desired = value + 1;
        if (n00b_cas((volatile _Atomic(uint32_t) *)&lock->futex,
                     &value, desired)) {
            break;
        }
    }

    n00b_thread_resume(stw_ctx);
    register_read(lock, thread, desired, nullptr, loc);

    n00b_barrier();
}

bool
_n00b_rw_unlock(n00b_rwlock_t *lock, char *loc)
{
    n00b_thread_t        *thread = n00b_thread_self();
    n00b_thread_record_t *rec    = thread->record;
    n00b_core_lock_info_t info   = n00b_atomic_load(&lock->data);

    // If we're a writer, any nesting comes out of our write level.
    if (info.owner == (int32_t)thread->id_info.parts.id) {
        if (!n00b_lock_release_accounting((void *)lock, loc)) {
            return false;
        }
        n00b_atomic_and(&lock->futex, ~N00B_RW_W_LOCK);
        n00b_futex_wake(&lock->futex, true);

        return true;
    }

    n00b_thread_read_log_t *log = find_read_lock_record(lock, thread);

    if (!log) {
        abort();
    }

    if (--log->level) {
        return false;
    }

    if (n00b_atomic_load(&rec->read_locks) == log) {
        n00b_atomic_store(&rec->read_locks, log->next_entry);
        assert(!log->prev_entry);
    }
    else {
        assert(log->prev_entry != log);
        log->prev_entry->next_entry = log->next_entry;
    }

    if (log->next_entry) {
        log->next_entry->prev_entry = log->prev_entry;
    }

    if (n00b_atomic_load(&rec->log_alloc_cache)) {
        n00b_atomic_load(&rec->log_alloc_cache)->prev_entry = log;
    }

    log->prev_entry = nullptr;
    log->next_entry = n00b_atomic_load(&rec->log_alloc_cache);
    n00b_atomic_store(&rec->log_alloc_cache, log);

    n00b_stw_suspend_ctx stw_ctx;
    uint32_t             value, desired;

    n00b_thread_suspend(stw_ctx);

    do {
        value   = n00b_atomic_load(&lock->futex);
        desired = value - 1;
    } while (!n00b_cas(&lock->futex, &value, desired));

    _n00b_runlock_accounting(lock, log, thread, desired, loc);

    n00b_thread_resume(stw_ctx);

    return true;
}
