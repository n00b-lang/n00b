#include <string.h>

#include "compiler/objfile/writer.h"
#include "compiler/objfile/endian.h"

// ============================================================================
// Growth
// ============================================================================

static void
writer_ensure(n00b_writer_t *w, size_t need)
{
    if (w->error) {
        return;
    }

    size_t required = w->pos + need;
    size_t cap      = n00b_buffer_len(w->buf);

    if (required <= cap) {
        return;
    }

    size_t new_cap = cap * 2;

    if (new_cap < required) {
        new_cap = required;
    }

    n00b_buffer_resize(w->buf, new_cap);

    if ((size_t)n00b_buffer_len(w->buf) < required) {
        w->error = true;
    }
}

// ============================================================================
// Construction
// ============================================================================

n00b_writer_t *
n00b_writer_new(size_t initial_capacity)
{
    if (initial_capacity == 0) {
        initial_capacity = 4096;
    }

    n00b_writer_t *w = n00b_alloc(n00b_writer_t);
    w->buf           = n00b_buffer_new(initial_capacity);
    w->pos           = 0;
    w->swap_endian   = false;
    w->error         = false;

    // Zero-fill the buffer and set byte_len to capacity so we can
    // write anywhere within it.
    memset(w->buf->data, 0, initial_capacity);
    w->buf->byte_len = initial_capacity;

    return w;
}

// ============================================================================
// Position
// ============================================================================

size_t
n00b_writer_pos(n00b_writer_t *w)
{
    return w->pos;
}

void
n00b_writer_setpos(n00b_writer_t *w, size_t pos)
{
    if (pos > (size_t)n00b_buffer_len(w->buf)) {
        w->error = true;
        return;
    }

    w->pos = pos;
}

void
n00b_writer_align(n00b_writer_t *w, size_t alignment)
{
    if (alignment <= 1) {
        return;
    }

    size_t remainder = w->pos % alignment;

    if (remainder == 0) {
        return;
    }

    size_t pad = alignment - remainder;

    n00b_writer_write_zeros(w, pad);
}

// ============================================================================
// Sequential writes
// ============================================================================

void
n00b_writer_write_u8(n00b_writer_t *w, uint8_t v)
{
    writer_ensure(w, 1);
    w->buf->data[w->pos] = (char)v;
    w->pos += 1;
}

void
n00b_writer_write_u16(n00b_writer_t *w, uint16_t v)
{
    if (w->swap_endian) {
        v = n00b_swap16(v);
    }

    writer_ensure(w, 2);
    memcpy(w->buf->data + w->pos, &v, 2);
    w->pos += 2;
}

void
n00b_writer_write_u32(n00b_writer_t *w, uint32_t v)
{
    if (w->swap_endian) {
        v = n00b_swap32(v);
    }

    writer_ensure(w, 4);
    memcpy(w->buf->data + w->pos, &v, 4);
    w->pos += 4;
}

void
n00b_writer_write_u64(n00b_writer_t *w, uint64_t v)
{
    if (w->swap_endian) {
        v = n00b_swap64(v);
    }

    writer_ensure(w, 8);
    memcpy(w->buf->data + w->pos, &v, 8);
    w->pos += 8;
}

void
n00b_writer_write_i8(n00b_writer_t *w, int8_t v)
{
    n00b_writer_write_u8(w, (uint8_t)v);
}

void
n00b_writer_write_i16(n00b_writer_t *w, int16_t v)
{
    n00b_writer_write_u16(w, (uint16_t)v);
}

void
n00b_writer_write_i32(n00b_writer_t *w, int32_t v)
{
    n00b_writer_write_u32(w, (uint32_t)v);
}

void
n00b_writer_write_i64(n00b_writer_t *w, int64_t v)
{
    n00b_writer_write_u64(w, (uint64_t)v);
}

void
n00b_writer_write_bytes(n00b_writer_t *w, const void *data, size_t n)
{
    if (n == 0) {
        return;
    }

    writer_ensure(w, n);
    memcpy(w->buf->data + w->pos, data, n);
    w->pos += n;
}

void
n00b_writer_write_cstring(n00b_writer_t *w, const char *s)
{
    size_t len = strlen(s);

    n00b_writer_write_bytes(w, s, len);
    n00b_writer_write_u8(w, 0);
}

void
n00b_writer_write_buffer(n00b_writer_t *w, n00b_buffer_t *buf)
{
    if (!buf) {
        return;
    }

    size_t len = n00b_buffer_len(buf);

    if (len > 0) {
        n00b_writer_write_bytes(w, buf->data, len);
    }
}

void
n00b_writer_write_zeros(n00b_writer_t *w, size_t n)
{
    if (n == 0) {
        return;
    }

    writer_ensure(w, n);
    memset(w->buf->data + w->pos, 0, n);
    w->pos += n;
}

void
n00b_writer_write_uleb128(n00b_writer_t *w, uint64_t v)
{
    for (;;) {
        uint8_t byte = v & 0x7F;
        v >>= 7;

        if (v != 0) {
            byte |= 0x80;
        }

        n00b_writer_write_u8(w, byte);

        if (v == 0) {
            break;
        }
    }
}

void
n00b_writer_write_sleb128(n00b_writer_t *w, int64_t v)
{
    bool more = true;

    while (more) {
        uint8_t byte = v & 0x7F;
        v >>= 7;

        // If the sign bit of the byte is set and v is all sign-extended,
        // or if the sign bit is clear and v is zero, we're done.
        if ((v == 0 && (byte & 0x40) == 0)
            || (v == -1 && (byte & 0x40) != 0)) {
            more = false;
        }
        else {
            byte |= 0x80;
        }

        n00b_writer_write_u8(w, byte);
    }
}

// ============================================================================
// Random-access patches
// ============================================================================

void
n00b_writer_patch_u16(n00b_writer_t *w, size_t off, uint16_t v)
{
    if (w->swap_endian) {
        v = n00b_swap16(v);
    }

    // Ensure the buffer is large enough for the patch location.
    if (off + 2 > n00b_buffer_len(w->buf)) {
        size_t need = off + 2 - n00b_buffer_len(w->buf);
        size_t saved = w->pos;
        w->pos = n00b_buffer_len(w->buf);
        writer_ensure(w, need);
        w->pos = saved;
    }

    memcpy(w->buf->data + off, &v, 2);
}

void
n00b_writer_patch_u32(n00b_writer_t *w, size_t off, uint32_t v)
{
    if (w->swap_endian) {
        v = n00b_swap32(v);
    }

    if (off + 4 > n00b_buffer_len(w->buf)) {
        size_t need = off + 4 - n00b_buffer_len(w->buf);
        size_t saved = w->pos;
        w->pos = n00b_buffer_len(w->buf);
        writer_ensure(w, need);
        w->pos = saved;
    }

    memcpy(w->buf->data + off, &v, 4);
}

void
n00b_writer_patch_u64(n00b_writer_t *w, size_t off, uint64_t v)
{
    if (w->swap_endian) {
        v = n00b_swap64(v);
    }

    if (off + 8 > n00b_buffer_len(w->buf)) {
        size_t need = off + 8 - n00b_buffer_len(w->buf);
        size_t saved = w->pos;
        w->pos = n00b_buffer_len(w->buf);
        writer_ensure(w, need);
        w->pos = saved;
    }

    memcpy(w->buf->data + off, &v, 8);
}

void
n00b_writer_patch_i64(n00b_writer_t *w, size_t off, int64_t v)
{
    n00b_writer_patch_u64(w, off, (uint64_t)v);
}

// ============================================================================
// Error checking
// ============================================================================

bool
n00b_writer_has_error(n00b_writer_t *w)
{
    return w->error;
}

// ============================================================================
// Endianness
// ============================================================================

void
n00b_writer_set_endian(n00b_writer_t *w, n00b_endian_t endian)
{
    union {
        uint16_t u;
        uint8_t  b[2];
    } probe = {.u = 1};

    bool host_is_little = (probe.b[0] == 1);

    if (endian == N00B_ENDIAN_LITTLE) {
        w->swap_endian = !host_is_little;
    }
    else {
        w->swap_endian = host_is_little;
    }
}

// ============================================================================
// Finalize
// ============================================================================

n00b_buffer_t *
n00b_writer_finalize(n00b_writer_t *w)
{
    n00b_buffer_t *buf = w->buf;

    // Truncate to actual written extent.
    if (w->pos < (size_t)n00b_buffer_len(buf)) {
        buf->byte_len = w->pos;
    }

    w->buf = nullptr;

    return buf;
}

// ============================================================================
// String table builder
// ============================================================================

#define STRTAB_INITIAL_CAP 256

n00b_strtab_builder_t *
n00b_strtab_builder_new(void)
{
    n00b_strtab_builder_t *sb = n00b_alloc(n00b_strtab_builder_t);

    sb->cap  = STRTAB_INITIAL_CAP;
    sb->data = n00b_alloc_array(char, sb->cap);
    sb->len  = 1; // First byte is always NUL.
    sb->data[0] = '\0';

    return sb;
}

uint32_t
n00b_strtab_builder_add(n00b_strtab_builder_t *sb, const char *str)
{
    if (!str || str[0] == '\0') {
        return 0; // Empty string always at offset 0.
    }

    size_t slen = strlen(str);

    // Deduplicate: linear scan for exact match.
    for (size_t i = 1; i < sb->len; ) {
        const char *existing = sb->data + i;
        size_t      elen     = strlen(existing);

        if (elen == slen && memcmp(existing, str, slen) == 0) {
            return (uint32_t)i;
        }

        i += elen + 1;
    }

    // Need to append: slen + 1 (for NUL terminator).
    size_t need = sb->len + slen + 1;

    if (need > sb->cap) {
        size_t new_cap = sb->cap * 2;

        if (new_cap < need) {
            new_cap = need;
        }

        char *new_data = n00b_alloc_array(char, new_cap);
        memcpy(new_data, sb->data, sb->len);
        sb->data = new_data;
        sb->cap  = new_cap;
    }

    uint32_t offset = (uint32_t)sb->len;
    memcpy(sb->data + sb->len, str, slen);
    sb->len += slen;
    sb->data[sb->len] = '\0';
    sb->len += 1;

    return offset;
}

void
n00b_strtab_builder_write(n00b_strtab_builder_t *sb, n00b_writer_t *w)
{
    n00b_writer_write_bytes(w, sb->data, sb->len);
}

size_t
n00b_strtab_builder_size(n00b_strtab_builder_t *sb)
{
    return sb->len;
}
