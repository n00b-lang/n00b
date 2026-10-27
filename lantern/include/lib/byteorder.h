#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const uint8_t *at;
    const uint8_t *end;
    bool           failed;
} core_byte_cursor_t;

static inline uint8_t core_cursor_u8(core_byte_cursor_t *c)
{
    if (c->at >= c->end) {
        c->failed = true;
        return 0;
    }
    return *c->at++;
}

static inline uint64_t core_cursor_le(core_byte_cursor_t *c, unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; i++)
        value |= (uint64_t)core_cursor_u8(c) << (8 * i);
    return value;
}

static inline uint64_t core_cursor_uleb(core_byte_cursor_t *c)
{
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        uint8_t byte = core_cursor_u8(c);
        value |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) return value;
    }
    c->failed = true;
    return value;
}

static inline int64_t core_cursor_sleb(core_byte_cursor_t *c)
{
    uint64_t value = 0;
    unsigned shift = 0;
    uint8_t byte = 0;
    do {
        byte = core_cursor_u8(c);
        value |= (uint64_t)(byte & 0x7f) << shift;
        shift += 7;
    } while ((byte & 0x80) && shift < 64);
    if (byte & 0x80) c->failed = true;
    if (shift < 64 && (byte & 0x40)) value |= ~UINT64_C(0) << shift;
    return (int64_t)value;
}

static inline void core_cursor_skip(core_byte_cursor_t *c, uint64_t count)
{
    if (count > (uint64_t)(c->end - c->at)) {
        c->failed = true;
        c->at = c->end;
    }
    else c->at += count;
}

static inline void core_le_store_u16(uint8_t out[2], uint16_t value)
{
    for (unsigned i = 0; i < 2; i++) out[i] = (uint8_t)(value >> (i * 8));
}

static inline void core_le_store_u32(uint8_t out[4], uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) out[i] = (uint8_t)(value >> (i * 8));
}

static inline void core_le_store_u64(uint8_t out[8], uint64_t value)
{
    for (unsigned i = 0; i < 8; i++) out[i] = (uint8_t)(value >> (i * 8));
}

static inline uint16_t core_le_load_u16(const uint8_t in[2])
{
    uint16_t value = 0;
    for (unsigned i = 0; i < 2; i++) value |= (uint16_t)in[i] << (i * 8);
    return value;
}

static inline uint32_t core_le_load_u32(const uint8_t in[4])
{
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; i++) value |= (uint32_t)in[i] << (i * 8);
    return value;
}

static inline uint64_t core_le_load_u64(const uint8_t in[8])
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++) value |= (uint64_t)in[i] << (i * 8);
    return value;
}
