#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "abi_directory.h"
#include "n00b-abi-v1.h"
#include "lib/object_emit.h"
#include "lib/string.h"

typedef struct n00bb_abi_name_pool_t n00bb_abi_name_pool_t;

n00bb_abi_name_pool_t *n00bb_abi_name_pool_new(void);
void n00bb_abi_name_pool_free(n00bb_abi_name_pool_t *pool);

/* Encodes an ordinary n00b string value. Long values borrow immutable bytes
 * from the pool section; the record location is needed for its relocation. */
bool n00bb_abi_name_pool_encode(n00bb_abi_name_pool_t *pool,
    core_string_t text, uint32_t table_kind, uint32_t record_index,
    size_t field_offset, n00b_abi_string_v1 *out);
const uint8_t *n00bb_abi_name_pool_bytes(const n00bb_abi_name_pool_t *pool);
size_t n00bb_abi_name_pool_size(const n00bb_abi_name_pool_t *pool);

/* Returns an owned relocation array targeting metadata section 1 and the
 * caller's pool symbol 0. No relocation is needed for short values. */
bool n00bb_abi_name_pool_relocations(
    const n00bb_abi_name_pool_t *pool,
    const n00bb_abi_directory_entry_t *entries, size_t entry_count,
    core_object_emit_absolute64_t **out, size_t *out_count);
