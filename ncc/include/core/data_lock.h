#pragma once

#include "n00b.h"

// Single-threaded — all lock operations are no-ops.
static inline ncc_rwlock_t *ncc_data_lock_new(void) { return nullptr; }
static inline void ncc_finalize_data_lock(void *p) { (void)p; }
static inline void ncc_data_read_lock(ncc_rwlock_t *lock) { (void)lock; }
static inline void ncc_data_write_lock(ncc_rwlock_t *lock) { (void)lock; }
static inline void ncc_data_unlock(ncc_rwlock_t *lock) { (void)lock; }
