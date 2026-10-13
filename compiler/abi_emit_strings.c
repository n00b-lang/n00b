#include "abi_emit_strings.h"

#include <string.h>

#include "abi_string.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/dict.h"
#include "lib/list.h"

typedef struct {
    core_string_t text;
    size_t offset;
} pooled_name_t;

typedef pooled_name_t *pooled_name_ptr_t;
core_list_decl(pooled_name_ptr_t);

typedef struct {
    uint32_t table_kind;
    uint32_t record_index;
    size_t field_offset;
    size_t pool_offset;
} pending_name_relocation_t;
core_list_decl(pending_name_relocation_t);

struct n00bb_abi_name_pool_t {
    core_buffer_t bytes;
    core_dict_t names;
    core_list_t(pooled_name_ptr_t) owned_names;
    core_list_t(pending_name_relocation_t) pending;
};

static core_hash_value_t
name_hash(void *key)
{
    return core_string_hash(((const pooled_name_t *)key)->text);
}

static bool
name_equal(const void *left, const void *right)
{
    return core_string_eq(((const pooled_name_t *)left)->text,
                          ((const pooled_name_t *)right)->text);
}

n00bb_abi_name_pool_t *
n00bb_abi_name_pool_new(void)
{
    n00bb_abi_name_pool_t *pool = core_alloc(n00bb_abi_name_pool_t);
    core_dict_init(&pool->names, name_hash, name_equal);
    return pool;
}

void
n00bb_abi_name_pool_free(n00bb_abi_name_pool_t *pool)
{
    if (!pool) return;
    core_buffer_release(&pool->bytes);
    core_dict_free(&pool->names);
    for (size_t i = 0; i < pool->owned_names.len; i++) {
        pooled_name_t *name = pool->owned_names.data[i];
        core_free(name->text.data);
        core_free(name);
    }
    core_list_free(pool->owned_names);
    core_list_free(pool->pending);
    core_free(pool);
}

bool
n00bb_abi_name_pool_value(n00bb_abi_name_pool_t *pool,
    core_string_t text, n00b_abi_string_encoding_v1 encoding,
    n00b_abi_string_v1 *out, size_t *pool_offset)
{
    return n00bb_abi_name_pool_value_tagged(pool, text, encoding,
        text.u8_bytes <= N00BB_ABI_STRING_SHORT_CAPACITY
            ? N00B_ABI_STRING_SHORT : N00B_ABI_STRING_LONG,
        out, pool_offset);
}

bool
n00bb_abi_name_pool_value_tagged(n00bb_abi_name_pool_t *pool,
    core_string_t text, n00b_abi_string_encoding_v1 encoding,
    n00b_abi_string_tag_v1 tag, n00b_abi_string_v1 *out,
    size_t *pool_offset)
{
    if (!pool || !out || !pool_offset
        || (text.u8_bytes && !text.data)
        || encoding > N00B_ABI_STRING_UTF8
        || (tag != N00B_ABI_STRING_SHORT
            && tag != N00B_ABI_STRING_LONG
            && tag != N00B_ABI_STRING_SPAN)) return false;
    *out = (n00b_abi_string_v1){0};
    *pool_offset = 0;
    if (tag == N00B_ABI_STRING_SHORT) {
        if (text.u8_bytes > N00BB_ABI_STRING_SHORT_CAPACITY) return false;
        if (text.u8_bytes)
            memcpy(out->short_value.bytes, text.data, text.u8_bytes);
        out->short_value.selector = (uint8_t)text.u8_bytes
            | (encoding << 4);
        return true;
    }
    if (text.u8_bytes > N00B_ABI_STRING_LONG_LENGTH_MASK
        || pool->owned_names.len == SIZE_MAX) return false;
    pooled_name_t probe = {.text = text};
    bool found = false;
    pooled_name_t *name = core_dict_get(&pool->names, &probe, &found);
    size_t offset;
    if (found) offset = name->offset;
    else {
        bool empty = !text.u8_bytes;
        size_t backing_bytes = text.u8_bytes + (size_t)empty;
        if (pool->bytes.byte_len > SIZE_MAX - 2
            || backing_bytes > SIZE_MAX - pool->bytes.byte_len - 2)
            return false;
        offset = pool->bytes.byte_len;
        if (empty) {
            const char zero = 0;
            core_buffer_append(&pool->bytes, &zero, 1);
        }
        else core_buffer_append(&pool->bytes, text.data, text.u8_bytes);
        name = core_alloc(pooled_name_t);
        *name = (pooled_name_t){.text = core_string_dup(text),
                                .offset = offset};
        core_dict_put(&pool->names, name, name);
        core_list_push(pool->owned_names, name);
    }
    out->long_value.selector = (uint64_t)text.u8_bytes
        | ((uint64_t)encoding << N00B_ABI_STRING_ENCODING_SHIFT)
        | ((uint64_t)tag << N00B_ABI_STRING_TAG_SHIFT);
    *pool_offset = offset;
    return true;
}

bool
n00bb_abi_name_pool_encode(n00bb_abi_name_pool_t *pool,
    core_string_t text, uint32_t table_kind, uint32_t record_index,
    size_t field_offset, n00b_abi_string_v1 *out)
{
    return n00bb_abi_name_pool_encode_with_encoding(pool, text,
        N00B_ABI_STRING_UTF8, table_kind, record_index, field_offset, out);
}

bool
n00bb_abi_name_pool_encode_with_encoding(
    n00bb_abi_name_pool_t *pool, core_string_t text,
    n00b_abi_string_encoding_v1 encoding, uint32_t table_kind,
    uint32_t record_index, size_t field_offset, n00b_abi_string_v1 *out)
{
    return n00bb_abi_name_pool_encode_tagged(pool, text, encoding,
        text.u8_bytes <= N00BB_ABI_STRING_SHORT_CAPACITY
            ? N00B_ABI_STRING_SHORT : N00B_ABI_STRING_LONG,
        table_kind, record_index, field_offset, out);
}

bool
n00bb_abi_name_pool_encode_tagged(
    n00bb_abi_name_pool_t *pool, core_string_t text,
    n00b_abi_string_encoding_v1 encoding, n00b_abi_string_tag_v1 tag,
    uint32_t table_kind, uint32_t record_index, size_t field_offset,
    n00b_abi_string_v1 *out)
{
    if (!pool || pool->pending.len == SIZE_MAX) return false;
    size_t offset = 0;
    if (!n00bb_abi_name_pool_value_tagged(pool, text, encoding, tag,
                                   out, &offset)) return false;
    if (tag == N00B_ABI_STRING_SHORT) return true;
    core_list_push(pool->pending, ((pending_name_relocation_t){
        .table_kind = table_kind,
        .record_index = record_index,
        .field_offset = field_offset,
        .pool_offset = offset,
    }));
    return true;
}

const uint8_t *
n00bb_abi_name_pool_bytes(const n00bb_abi_name_pool_t *pool)
{
    return pool ? (const uint8_t *)pool->bytes.data : nullptr;
}

size_t
n00bb_abi_name_pool_size(const n00bb_abi_name_pool_t *pool)
{
    return pool ? pool->bytes.byte_len : 0;
}

bool
n00bb_abi_name_pool_relocations(
    const n00bb_abi_name_pool_t *pool,
    const n00bb_abi_directory_entry_t *entries, size_t entry_count,
    core_object_emit_absolute64_t **out, size_t *out_count)
{
    if (!pool || !out || !out_count || (entry_count && !entries)) return false;
    core_object_emit_absolute64_t *relocations = pool->pending.len
        ? core_alloc_array(core_object_emit_absolute64_t, pool->pending.len)
        : nullptr;
    for (size_t i = 0; i < pool->pending.len; i++) {
        const pending_name_relocation_t *pending = &pool->pending.data[i];
        const n00bb_abi_directory_entry_t *entry = nullptr;
        for (size_t j = 0; j < entry_count; j++)
            if (entries[j].kind == pending->table_kind) {
                entry = &entries[j];
                break;
            }
        if (!entry || pending->record_index >= entry->record_count
            || entry->record_bytes < sizeof(void *)
            || pending->field_offset > entry->record_bytes - sizeof(void *)) {
            core_free(relocations);
            return false;
        }
        uint64_t relative = (uint64_t)entry->record_bytes
            * pending->record_index + pending->field_offset;
        if (entry->file_offset > UINT64_MAX - relative) {
            core_free(relocations);
            return false;
        }
        relocations[i] = (core_object_emit_absolute64_t){
            .section_index = 1,
            .section_offset = entry->file_offset + relative,
            .symbol_index = 0,
            .addend_bits = pending->pool_offset,
        };
    }
    *out = relocations;
    *out_count = pool->pending.len;
    return true;
}
