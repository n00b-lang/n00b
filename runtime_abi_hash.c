#include "n00b-abi-v1.h"
#include "abi_string.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#define XXH_INLINE_ALL
#include <xxhash.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

void n00bb_bootstrap_xxh3_128_array(const n00b_abi_array_value_v1 *value,
                                    uint64_t result[2])
{
    const n00b_abi_array_descriptor_header_v1 *shape = value
        ? value->descriptor : NULL;
    if (!shape || !result || shape->rank != 1 || shape->flat_rank != 1
        || shape->flags || shape->reserved_zero
        || shape->total_count > ((const uint64_t *)(shape + 1))[0]
        || shape->total_count > SIZE_MAX
        || (shape->total_count && !value->data)) __builtin_trap();
    XXH128_hash_t hash = XXH3_128bits(value->data ? value->data : "",
                                       (size_t)shape->total_count);
    result[0] = hash.low64;
    result[1] = hash.high64;
}

static void hash_scalar_bytes(const uint8_t *source, uint32_t bits,
                              bool floating, uint64_t result[2])
{
    uint64_t bytes = (bits + 7u) / 8u;
    if (!(bits & 7u) && !floating) {
        XXH128_hash_t hash = XXH3_128bits(source, (size_t)bytes);
        result[0] = hash.low64;
        result[1] = hash.high64;
        return;
    }
    uint8_t canonical[16] = {0};
    const volatile uint8_t *source_bytes = source;
    for (uint64_t i = 0; i < bytes; i++) canonical[i] = source_bytes[i];
    if (bits & 7u)
        canonical[bytes - 1] &= (uint8_t)((1u << (bits & 7u)) - 1u);
    if (floating && bits == 64) {
        uint64_t floating_bits = 0;
        __builtin_memcpy(&floating_bits, canonical, sizeof floating_bits);
        if (!(floating_bits & UINT64_C(0x7fffffffffffffff)))
            __builtin_memset(canonical, 0, sizeof floating_bits);
    }
    XXH128_hash_t hash = XXH3_128bits(canonical, (size_t)bytes);
    result[0] = hash.low64;
    result[1] = hash.high64;
}

void n00bb_bootstrap_hash_scalar_known(uint32_t type_field, uint64_t payload,
                                       uint32_t bits, uint32_t floating,
                                       uint64_t result[2])
{
    if (!result || !bits || bits > 128 || floating > 1) __builtin_trap();
    uint64_t bytes = (bits + 7u) / 8u;
    const uint8_t *source = (type_field & UINT32_C(0x80000000)) || bytes > 8
        ? (const uint8_t *)(uintptr_t)payload : (const uint8_t *)&payload;
    if (!source) __builtin_trap();
    hash_scalar_bytes(source, bits, floating != 0, result);
}

void n00bb_bootstrap_hash_integer_64(uint64_t value, uint64_t result[2])
{
    XXH128_hash_t hash = XXH3_128bits(&value, sizeof value);
    result[0] = hash.low64;
    result[1] = hash.high64;
}

void n00bb_bootstrap_hash_scalar(uint32_t type_field, uint64_t payload,
                                 uint64_t result[2])
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    const n00b_abi_type_v1 *type = snapshot
        ? n00b_abi_v1_type(snapshot,
                           type_field & UINT32_C(0x7fffffff)) : NULL;
    if (!type || !result || !type->size_bits || type->size_bits > 128)
        __builtin_trap();
    uint64_t bytes = (type->size_bits + 7) / 8;
    bool indirect = (type_field & UINT32_C(0x80000000)) || bytes > 8;
    const uint8_t *source = indirect
        ? (const uint8_t *)(uintptr_t)payload : (const uint8_t *)&payload;
    if (!source) __builtin_trap();
    if (type->kind != N00B_ABI_TYPE_PRIMITIVE
        && type->kind != N00B_ABI_TYPE_ENUM
        && type->kind != N00B_ABI_TYPE_FLAGSET
        && type->kind != N00B_ABI_TYPE_REFERENCE) __builtin_trap();

    uint32_t bits = type->kind == N00B_ABI_TYPE_REFERENCE
        ? (uint32_t)type->size_bits : type->semantic_width_bits;
    if (!bits || bits > 128 || (uint64_t)bits > bytes * 8)
        __builtin_trap();
    bool floating = false;
    if (type->kind == N00B_ABI_TYPE_PRIMITIVE && bits == 64) {
            const uint8_t *name = NULL;
            uint64_t length = 0;
            if (n00b_abi_v1_string_bytes(snapshot, &type->canonical_name,
                                         &name, &length) != 1) __builtin_trap();
            floating = (length == 3 && !__builtin_memcmp(name, "f64", 3))
                || (length == 5 && !__builtin_memcmp(name, "float", 5));
    }
    hash_scalar_bytes(source, bits, floating, result);
}

void n00bb_bootstrap_hash_string(uint64_t low, uint64_t high,
                                 uint64_t result[2])
{
    n00b_abi_string_v1 value = {.words = {low, high}};
    n00bb_abi_string_header_t header;
    if (!result || !n00bb_abi_string_decode((const uint8_t *)&value,
                                            sizeof value, &header)
        || header.tag == N00BB_ABI_STRING_RICH) __builtin_trap();
    const uint8_t *bytes = header.tag == N00BB_ABI_STRING_SHORT
        ? value.short_value.bytes : value.long_value.data;
    if (header.length > SIZE_MAX || (header.length && !bytes))
        __builtin_trap();
    XXH128_hash_t hash = XXH3_128bits(bytes ? bytes : (const uint8_t *)"",
                                      (size_t)header.length);
    result[0] = hash.low64;
    result[1] = hash.high64;
}
