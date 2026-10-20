#pragma once

#include <stdint.h>

/* The caller supplies the fixed-width byte extent. These operations do not
 * require aligned addresses or the host's byte order. */
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
