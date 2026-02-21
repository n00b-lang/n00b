/**
 * @file buf.c
 * @brief Buffer implementation with module struct API.
 */

#define NCC_LIB_IMPL  // Prevent compat macros from interfering with definitions
#include "buf.h"

#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>

ncc_buf_t *
ncc_buf_alloc(int64_t len)
{
    ncc_buf_t *b = base_calloc(1, sizeof(ncc_buf_t) + len);
    b->cap = len;
    return b;
}

ncc_buf_t *
ncc_buf_concat(ncc_buf_t *b, char *start, int64_t len)
{
    if (!b) {
        // First append: allocate with doubling headroom.
        int64_t cap = len < 4096 ? 4096 : len * 2;
        ncc_buf_t *result = base_calloc(1, sizeof(ncc_buf_t) + cap);
        result->len = len;
        result->cap = cap;
        memcpy(result->data, start, len);
        return result;
    }

    int64_t needed = b->len + len;

    if (needed <= b->cap) {
        // Fits in existing allocation.
        memcpy(b->data + b->len, start, len);
        b->len = needed;
        return b;
    }

    // Need to grow: double until it fits.
    int64_t new_cap = b->cap ? b->cap : 4096;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    ncc_buf_t *result = base_calloc(1, sizeof(ncc_buf_t) + new_cap);
    result->len = needed;
    result->cap = new_cap;
    memcpy(result->data, b->data, b->len);
    memcpy(result->data + b->len, start, len);
    base_dealloc(b);

    return result;
}

ncc_buf_t *
ncc_buf_concat_str(ncc_buf_t *b, char *s)
{
    return ncc_buf_concat(b, s, strlen(s));
}

ncc_buf_t *
ncc_buf_read_file(FILE *f)
{
    if (fseek(f, 0, SEEK_END)) {
        return nullptr;
    }
    long sz = ftell(f);
    if (fseek(f, 0, SEEK_SET)) {
        return nullptr;
    }

    ncc_buf_t *result = ncc_buf_alloc(sz);
    result->len       = fread(result->data, 1, sz, f);

    (void)fclose(f);

    return result;
}

ncc_buf_t *
ncc_buf_read_file_by_name(char *fname)
{
    FILE *f = fopen(fname, "r");
    if (!f) {
        return nullptr;
    }

    return ncc_buf_read_file(f);
}

ncc_buf_t *
ncc_buf_read_stream(FILE *f)
{
    ncc_buf_t *result = nullptr;
    char chunk[NCC_BUF_CHUNK_SIZE];
    size_t n;

    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        result = ncc_buf_concat(result, chunk, n);
    }

    fclose(f);

    if (!result) {
        result = ncc_buf_alloc(0);
    }

    return result;
}

ncc_buf_t *
ncc_buf_from_str(const char *str)
{
    if (!str) {
        return ncc_buf_alloc(0);
    }

    size_t len        = strlen(str);
    ncc_buf_t *result = ncc_buf_alloc(len + 1);

    result->len = len;
    memcpy(result->data, str, len);

    return result;
}
