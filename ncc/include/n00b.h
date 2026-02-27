#pragma once

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>

typedef uint32_t n00b_codepoint_t;
typedef int32_t  n00b_color_t;
typedef unsigned __int128 n00b_uint128_t;
typedef n00b_uint128_t n00b_hash_value_t;
typedef uint64_t n00b_size_t;

// Stub types — threading/GC infrastructure not needed.
typedef void n00b_rwlock_t;
typedef void n00b_allocator_t;
typedef void n00b_runtime_t;
typedef void n00b_mmap_info_t;
typedef void n00b_inline_hdr_t;
typedef void n00b_oob_hdr_t;

// Forward declarations for types used across headers.
typedef struct n00b_string_t     n00b_string_t;
typedef struct n00b_buffer_t     n00b_buffer_t;
typedef struct n00b_dict_untyped_t n00b_dict_untyped_t;

typedef n00b_hash_value_t (*n00b_hash_fn)(void *);

// Codepoint predicate for scanner match_if.
typedef bool (*n00b_cp_predicate_fn)(n00b_codepoint_t, void *);

#define n00b_min(a, b) \
    ({ __typeof__(a) _a = (a), _b = (b); _a < _b ? _a : _b; })
#define n00b_max(a, b) \
    ({ __typeof__(a) _a = (a), _b = (b); _a > _b ? _a : _b; })

#define n00b_likely(x)   __builtin_expect(!!(x), 1)
#define n00b_unlikely(x) __builtin_expect(!!(x), 0)
