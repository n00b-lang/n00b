#pragma once

// Single-threaded — atomics are plain loads/stores.
#define ncc_atomic_load(p)     (*(p))
#define ncc_atomic_store(p, v) (*(p) = (v))
