#define N00B_USE_INTERNAL_API
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/data_lock.h"
#include "core/arena.h"
#include "util/defer.h"

// ============================================================================
// Hex map for encoding
// ============================================================================

static const char n00b_hex_map_lower[16] = {
    '0',
    '1',
    '2',
    '3',
    '4',
    '5',
    '6',
    '7',
    '8',
    '9',
    'a',
    'b',
    'c',
    'd',
    'e',
    'f',
};

// ============================================================================
// Internal helpers
// ============================================================================

/**
 * Create a new buffer with the given byte length, using the specified
 * allocator (or runtime default if nullptr).
 */
static n00b_buffer_t *
_buffer_create(int64_t length, n00b_allocator_t *allocator)
{
    n00b_buffer_t *buf = n00b_alloc(n00b_buffer_t, .allocator = allocator);

    n00b_buffer_init(buf, .length = length, .allocator = allocator);
    return buf;
}

// ============================================================================
// Construction
// ============================================================================

void
n00b_buffer_init(n00b_buffer_t *obj) _kargs
{
    int64_t           length    = -1;
    char             *raw       = nullptr;
    n00b_string_t    *hex       = nullptr;
    char             *ptr       = nullptr;
    n00b_allocator_t *allocator = nullptr;
    bool              no_lock   = false;
}
{
    obj->lock      = no_lock ? nullptr : n00b_data_lock_new();
    obj->allocator = allocator;

    if (obj->lock) {
        n00b_add_finalizer(obj, n00b_finalize_data_lock, obj->lock);
    }

    if (raw == nullptr && hex == nullptr && ptr == nullptr) {
        if (length < 0) {
            return;
        }
    }

    if (length < 0) {
        if (hex == nullptr) {
            return;
        }
        else {
            length = hex->u8_bytes >> 1;
        }
    }

    if (length == 0) {
        obj->alloc_len = N00B_EMPTY_BUFFER_ALLOC;
        obj->data      = n00b_alloc_array(char, obj->alloc_len, .allocator = obj->allocator);
        obj->byte_len  = 0;
        return;
    }

    if (length > 0 && ptr == nullptr) {
        int64_t alloc_len = n00b_align_closest_pow2_ceil(length);

        obj->data      = n00b_alloc_array(char, alloc_len, .allocator = obj->allocator);
        obj->alloc_len = alloc_len;
    }

    if (raw != nullptr) {
        if (hex != nullptr || ptr != nullptr) {
            return;
        }
        memcpy(obj->data, raw, length);
    }

    if (ptr != nullptr) {
        if (hex != nullptr) {
            return;
        }
        obj->data = ptr;
    }

    if (hex != nullptr) {
        uint8_t cur         = 0;
        int     valid_count = 0;

        for (size_t i = 0; i < hex->u8_bytes; i++) {
            uint8_t byte = (uint8_t)hex->data[i];

            if (byte >= '0' && byte <= '9') {
                if ((++valid_count) % 2 == 1) {
                    cur = (byte - '0') << 4;
                }
                else {
                    cur |= (byte - '0');
                    obj->data[obj->byte_len++] = cur;
                }
                continue;
            }
            if (byte >= 'a' && byte <= 'f') {
                if ((++valid_count) % 2 == 1) {
                    cur = ((byte - 'a') + 10) << 4;
                }
                else {
                    cur |= (byte - 'a') + 10;
                    obj->data[obj->byte_len++] = cur;
                }
                continue;
            }
            if (byte >= 'A' && byte <= 'F') {
                if ((++valid_count) % 2 == 1) {
                    cur = ((byte - 'A') + 10) << 4;
                }
                else {
                    cur |= (byte - 'A') + 10;
                    obj->data[obj->byte_len++] = cur;
                }
                continue;
            }
        }
    }
    else {
        obj->byte_len = length;
    }
}

// ============================================================================
// Len
// ============================================================================

n00b_size_t
n00b_buffer_len(n00b_buffer_t *buffer)
{
    return (n00b_size_t)buffer->byte_len;
}

// ============================================================================
// Resize
// ============================================================================

void
n00b_buffer_resize(n00b_buffer_t *buffer, uint64_t new_sz)
{
    defer_on();
    n00b_buffer_acquire_w(buffer);

    if ((int64_t)new_sz <= (int64_t)buffer->alloc_len) {
        buffer->byte_len = new_sz;
        Return;
    }

    uint64_t new_alloc_sz = n00b_align_closest_pow2_ceil(new_sz);
    char    *new_data = n00b_alloc_array(char, new_alloc_sz, .allocator = buffer->allocator);

    memcpy(new_data, buffer->data, buffer->byte_len);

    if (buffer->data) {
        n00b_free(buffer->data);
    }

    buffer->data      = new_data;
    buffer->byte_len  = new_sz;
    buffer->alloc_len = new_alloc_sz;

    Return;
    defer_func_end();
}

// ============================================================================
// Add (concatenate)
// ============================================================================

n00b_buffer_t *
n00b_buffer_add(n00b_buffer_t *b1, n00b_buffer_t *b2) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (!b1) {
        return b2;
    }
    if (!b2) {
        return b1;
    }

    defer_on();
    n00b_buffer_acquire_r(b1);
    n00b_buffer_acquire_r(b2);

    int64_t        l1     = n00b_max((int64_t)b1->byte_len, (int64_t)0);
    int64_t        l2     = n00b_max((int64_t)b2->byte_len, (int64_t)0);
    int64_t        lnew   = l1 + l2;
    n00b_buffer_t *result = _buffer_create(lnew, allocator);

    if (l1 > 0) {
        memcpy(result->data, b1->data, l1);
    }
    if (l2 > 0) {
        memcpy(result->data + l1, b2->data, l2);
    }

    Return result;
    defer_func_end();
}

// ============================================================================
// Find
// ============================================================================

n00b_option_t(int64_t) n00b_buffer_find(n00b_buffer_t *b, n00b_buffer_t *sub) _kargs
{
    n00b_option_t(size_t) start = n00b_option_set(size_t, 0);
    n00b_option_t(size_t) end   = n00b_option_none(size_t);
}
{
    size_t blen   = b->byte_len;
    size_t sublen = sub->byte_len;
    size_t s      = n00b_option_get_or_else(start, 0);
    size_t e      = n00b_option_get_or_else(end, blen);

    if (s > blen) {
        return n00b_option_none(int64_t);
    }
    if (e > blen) {
        e = blen;
    }
    if (e <= s) {
        return n00b_option_none(int64_t);
    }
    if (sublen == 0) {
        return n00b_option_set(int64_t, (int64_t)s);
    }

    char *bp   = b->data + s;
    char *endp = b->data + e - sublen;

    while (bp <= endp) {
        char *p    = bp;
        char *subp = sub->data;
        bool  hit  = true;

        for (size_t i = 0; i < sublen; i++) {
            if (*p++ != *subp++) {
                hit = false;
                break;
            }
        }
        if (hit) {
            return n00b_option_set(int64_t, (int64_t)(bp - b->data));
        }
        bp++;
    }

    return n00b_option_none(int64_t);
}

// ============================================================================
// Index get / set
// ============================================================================

n00b_result_t(uint8_t) n00b_buffer_get_index(n00b_buffer_t *b, int64_t n)
{
    defer_on();
    n00b_buffer_acquire_r(b);

    if (n < 0) {
        n += (int64_t)b->byte_len;
        if (n < 0) {
            Return n00b_result_err(uint8_t, N00B_ERR_BUFFER_INDEX_OOB);
        }
    }
    if ((uint64_t)n >= b->byte_len) {
        Return n00b_result_err(uint8_t, N00B_ERR_BUFFER_INDEX_OOB);
    }

    uint8_t result = (uint8_t)b->data[n];
    Return  n00b_result_ok(uint8_t, result);
    defer_func_end();
}

n00b_result_t(bool) n00b_buffer_set_index(n00b_buffer_t *b, int64_t n, uint8_t c)
{
    defer_on();
    n00b_buffer_acquire_w(b);

    if (n < 0) {
        n += (int64_t)b->byte_len;
        if (n < 0) {
            Return n00b_result_err(bool, N00B_ERR_BUFFER_INDEX_OOB);
        }
    }
    if ((uint64_t)n >= b->byte_len) {
        Return n00b_result_err(bool, N00B_ERR_BUFFER_INDEX_OOB);
    }

    b->data[n] = (char)c;
    Return n00b_result_ok(bool, true);
    defer_func_end();
}

// ============================================================================
// Slice get / set
// ============================================================================

n00b_buffer_t *
n00b_buffer_get_slice(n00b_buffer_t *b, int64_t start, int64_t end) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    defer_on();
    n00b_buffer_acquire_r(b);

    int64_t len = (int64_t)b->byte_len;

    if (start < 0) {
        start += len;
    }
    else if (start >= len) {
        Return _buffer_create(0, allocator);
    }
    if (end < 0) {
        end += len + 1;
    }
    else if (end > len) {
        end = len;
    }
    if ((start | end) < 0 || start >= end) {
        Return _buffer_create(0, allocator);
    }

    int64_t        slice_len = end - start;
    n00b_buffer_t *result    = _buffer_create(slice_len, allocator);

    memcpy(result->data, b->data + start, slice_len);

    Return result;
    defer_func_end();
}

n00b_result_t(bool)
    n00b_buffer_set_slice(n00b_buffer_t *b, int64_t start, int64_t end, n00b_buffer_t *val)
{
    defer_on();
    n00b_buffer_acquire_w(b);

    int64_t len = (int64_t)b->byte_len;

    if (start < 0) {
        start += len;
    }
    else if (start >= len) {
        Return n00b_result_err(bool, N00B_ERR_BUFFER_SLICE_OOB);
    }
    if (end < 0) {
        end += len + 1;
    }
    else if (end > len) {
        end = len;
    }
    if ((start | end) < 0 || start >= end) {
        Return n00b_result_err(bool, N00B_ERR_BUFFER_SLICE_OOB);
    }

    int64_t slice_len   = end - start;
    int64_t new_len     = (int64_t)b->byte_len - slice_len;
    int64_t replace_len = 0;

    if (val != nullptr) {
        replace_len = (int64_t)val->byte_len;
        new_len += replace_len;
    }

    if (new_len <= (int64_t)b->byte_len) {
        if (val != nullptr && val->byte_len > 0) {
            memcpy(b->data + start, val->data, replace_len);
        }
        if (end < (int64_t)b->byte_len) {
            memmove(b->data + start + replace_len, b->data + end, b->byte_len - end);
        }
    }
    else {
        char *new_buf = n00b_alloc_array(char, new_len, .allocator = b->allocator);
        if (start > 0) {
            memcpy(new_buf, b->data, start);
        }
        if (replace_len != 0) {
            memcpy(new_buf + start, val->data, replace_len);
        }
        if (end < (int64_t)b->byte_len) {
            memcpy(new_buf + start + replace_len, b->data + end, b->byte_len - end);
        }
        if (b->data) {
            n00b_free(b->data);
        }
        b->data      = new_buf;
        b->alloc_len = new_len;
    }

    b->byte_len = new_len;

    Return n00b_result_ok(bool, true);
    defer_func_end();
}

// ============================================================================
// Copy
// ============================================================================

n00b_buffer_t *
n00b_buffer_copy(n00b_buffer_t *inbuf) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    defer_on();
    n00b_buffer_acquire_r(inbuf);

    n00b_buffer_t *outbuf = _buffer_create((int64_t)inbuf->byte_len, allocator);

    memcpy(outbuf->data, inbuf->data, inbuf->byte_len);

    Return outbuf;
    defer_func_end();
}

// ============================================================================
// Raw C access
// ============================================================================

char *
n00b_buffer_to_c(n00b_buffer_t *b, int64_t *len_ptr)
{
    if (len_ptr) {
        *len_ptr = (int64_t)b->byte_len;
    }
    return b->data;
}

// ============================================================================
// To string
// ============================================================================

n00b_string_t
n00b_buffer_to_string(n00b_buffer_t *buffer)
{
    defer_on();
    n00b_buffer_acquire_r(buffer);

    n00b_string_t result = {};
    int64_t       nbytes = (int64_t)buffer->byte_len;

    result.data = n00b_alloc_array(char, nbytes + 1);
    memcpy(result.data, buffer->data, nbytes);
    result.data[nbytes] = '\0';
    result.u8_bytes     = nbytes;
    result.codepoints   = nbytes; // Placeholder: assume ASCII / 1:1.
    result.styling      = nullptr;

    Return result;
    defer_func_end();
}

// ============================================================================
// To hex string
// ============================================================================

n00b_string_t
n00b_buffer_to_hex_str(n00b_buffer_t *buf)
{
    defer_on();
    n00b_buffer_acquire_r(buf);

    int64_t       hex_len = (int64_t)buf->byte_len * 2;
    n00b_string_t result  = {};

    result.data = n00b_alloc_array(char, hex_len + 1);
    char *p     = result.data;

    for (size_t i = 0; i < buf->byte_len; i++) {
        uint8_t c = ((uint8_t *)buf->data)[i];
        *p++      = n00b_hex_map_lower[(c >> 4)];
        *p++      = n00b_hex_map_lower[c & 0x0f];
    }

    *p                = '\0';
    result.codepoints = hex_len;
    result.u8_bytes   = hex_len;
    result.styling    = nullptr;

    Return result;
    defer_func_end();
}

// ============================================================================
// Join
// ============================================================================

n00b_buffer_t *
n00b_buffer_join(n00b_buffer_t **items, size_t count, n00b_buffer_t *joiner) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (count == 0) {
        return _buffer_create(0, allocator);
    }

    defer_on();

    int64_t new_len = 0;
    int64_t jlen    = 0;

    for (size_t i = 0; i < count; i++) {
        new_len += (int64_t)items[i]->byte_len;
    }

    if (joiner != nullptr) {
        jlen = (int64_t)joiner->byte_len;
        new_len += jlen * ((int64_t)count - 1);
    }

    n00b_buffer_t *result = _buffer_create(new_len, allocator);
    char          *p      = result->data;

    int64_t clen = (int64_t)items[0]->byte_len;
    memcpy(p, items[0]->data, clen);

    for (size_t i = 1; i < count; i++) {
        p += clen;
        if (jlen > 0) {
            memcpy(p, joiner->data, jlen);
            p += jlen;
        }
        clen = (int64_t)items[i]->byte_len;
        memcpy(p, items[i]->data, clen);
    }

    Return result;
    defer_func_end();
}

// ============================================================================
// From codepoint
// ============================================================================

n00b_buffer_t *
n00b_buffer_from_codepoint(n00b_codepoint_t cp) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    uint8_t tmp[4];
    int     nbytes = n00b_utf8_encode_codepoint(cp, tmp);

    if (nbytes <= 0) {
        return _buffer_create(0, allocator);
    }

    n00b_buffer_t *result = _buffer_create(nbytes, allocator);
    memcpy(result->data, tmp, nbytes);

    return result;
}

// ============================================================================
// Free
// ============================================================================

void
n00b_buffer_free(n00b_buffer_t *buf)
{
    if (!buf) {
        return;
    }

    if (buf->data) {
        n00b_free(buf->data);
    }

    buf->data      = nullptr;
    buf->byte_len  = 0;
    buf->alloc_len = 0;
    buf->flags     = 0;
    buf->lock      = nullptr;
}
