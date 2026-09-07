#pragma once

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

typedef uint32_t core_codepoint_t;
typedef unsigned __int128 core_uint128_t;
typedef uint64_t core_hash_value_t;
typedef uint64_t core_size_t;

typedef struct core_string_t     core_string_t;
typedef struct core_buffer_t     core_buffer_t;
typedef struct core_dict_t core_dict_t;

typedef core_hash_value_t (*core_hash_fn)(void *);

typedef bool (*scan_cp_predicate_fn)(core_codepoint_t, void *);

#define core_min(a, b) \
    ({ __typeof__(a) _a = (a), _b = (b); _a < _b ? _a : _b; })
#define core_max(a, b) \
    ({ __typeof__(a) _a = (a), _b = (b); _a > _b ? _a : _b; })

#define core_likely(x)   __builtin_expect(!!(x), 1)
#define core_unlikely(x) __builtin_expect(!!(x), 0)
