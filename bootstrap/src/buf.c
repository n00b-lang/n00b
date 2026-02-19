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
    return base_calloc(1, sizeof(ncc_buf_t) + len);
}

ncc_buf_t *
ncc_buf_concat(ncc_buf_t *b, char *start, int64_t len)
{
    int64_t total = len;

    if (b) {
        total += b->len;
    }
    ncc_buf_t *result = base_calloc(1, total + sizeof(ncc_buf_t));
    if (!result) {
        return b;
    }
    char *p                = result->data;
    result->len            = total;

    if (b) {
        memcpy(p, b->data, b->len);
        p += b->len;
        base_dealloc(b);
    }

    memcpy(p, start, len);

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

    ncc_buf_t *result = base_calloc(1, sz + sizeof(ncc_buf_t));
    result->len            = fread(result->data, 1, sz, f);

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

    size_t len             = strlen(str);
    ncc_buf_t *result = base_calloc(1, sizeof(ncc_buf_t) + len + 1);

    result->len = len;
    memcpy(result->data, str, len);

    return result;
}
