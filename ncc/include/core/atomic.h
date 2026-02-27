#pragma once

// Single-threaded — atomics are plain loads/stores.
#define n00b_atomic_load(p)     (*(p))
#define n00b_atomic_store(p, v) (*(p) = (v))
