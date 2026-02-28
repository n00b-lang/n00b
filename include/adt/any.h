#pragma once

// The 'any' data type, indicating that we need to check run-time type
// information.  It always shallow-copies the entire datum; if it's
// larger than uint64_t, we instead store a pointer to the copy.
//
// The type id is the typehash() of the type.

typedef struct {
    uint64_t type_hash;
    union {
        int64_t int_contents;
        void   *ref_contents;
    } contents;
} n00b_any_t;
