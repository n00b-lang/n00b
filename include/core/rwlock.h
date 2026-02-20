/**
 * @file rwlock.h
 * @brief Futex-based reader-writer lock.
 *
 * Supports multiple concurrent readers, exclusive writers, reader-to-writer
 * upgrade, and nesting.  Integrates with the per-thread lock chain for
 * crash debugging.
 */
#pragma once

#include "n00b.h"
#include "core/lock_common.h"
#include "core/futex.h"

/**
 * @brief Debug log entry for rwlock operations (used only in debug builds).
 */
typedef struct n00b_rwdebug_t {
    _Atomic(struct n00b_rwdebug_t *) prev;
    _Atomic(struct n00b_rwdebug_t *) next;
    char                            *loc;
    bool                             lock_op;
    int32_t                          thread_id;
    int32_t                          nest;
    char                            *trace;
} n00b_rwdebug_t;

struct n00b_rwlock_t {
    N00B_COMMON_LOCK_BASE;
    n00b_futex_t              futex;
    _Atomic(n00b_rwdebug_t *) first_entry;
    _Atomic(n00b_rwdebug_t *) last_entry;
};

/**
 * @brief Initialize a reader-writer lock.
 * @param lock RWLock to initialize.
 * @param loc  Source location (auto-filled by macro).
 */
extern void _n00b_rw_init(n00b_rwlock_t *, char *);

/**
 * @brief Acquire the write lock (exclusive).
 * @param lock RWLock to acquire.
 * @param loc  Source location (auto-filled by macro).
 * @return     0 on success.
 */
extern int  _n00b_rw_write_lock(n00b_rwlock_t *, char *);

/**
 * @brief Acquire the read lock (shared).
 * @param lock RWLock to acquire.
 * @param loc  Source location (auto-filled by macro).
 */
extern void _n00b_rw_read_lock(n00b_rwlock_t *, char *);

/**
 * @brief Release a read or write lock.
 * @param lock RWLock to release.
 * @param loc  Source location (auto-filled by macro).
 * @return     true if a lock was fully released.
 */
extern bool _n00b_rw_unlock(n00b_rwlock_t *, char *);

#define n00b_rw_write_lock(l) _n00b_rw_write_lock((l), N00B_LOC_STRING())
#define n00b_rw_read_lock(l)  _n00b_rw_read_lock((l), N00B_LOC_STRING())
#define n00b_rw_unlock(l)     _n00b_rw_unlock((l), N00B_LOC_STRING())
#define n00b_rw_lock_init(l)  _n00b_rw_init((l), N00B_LOC_STRING())

#define N00B_RW_UNLOCKED 0x00000000
#define N00B_RW_W_LOCK   0x40000000

// Read-lock accounting (implemented in lock_accounting.c).
extern void n00b_rlock_accounting(n00b_rwlock_t          *lock,
                                  n00b_thread_read_log_t *record,
                                  n00b_thread_t          *thread,
                                  int                     value,
                                  char                   *loc);
extern void n00b_runlock_accounting(n00b_rwlock_t          *lock,
                                    n00b_thread_read_log_t *record,
                                    n00b_thread_t          *thread,
                                    int                     value,
                                    char                   *loc);
