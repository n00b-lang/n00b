#pragma once

#include "n00b.h"

// Single-threaded — all lock operations are no-ops.
static inline n00b_rwlock_t *n00b_data_lock_new(void) { return nullptr; }
static inline void n00b_finalize_data_lock(void *p) { (void)p; }
static inline void n00b_data_read_lock(n00b_rwlock_t *lock) { (void)lock; }
static inline void n00b_data_write_lock(n00b_rwlock_t *lock) { (void)lock; }
static inline void n00b_data_unlock(n00b_rwlock_t *lock) { (void)lock; }
