/**
 * @file n00b_md5.h
 * @brief Standalone MD5 hash (RFC 1321).
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t state[4];
    uint64_t count;
    uint8_t  buffer[64];
} n00b_md5_ctx_t;

extern void n00b_md5_init(n00b_md5_ctx_t *ctx);
extern void n00b_md5_update(n00b_md5_ctx_t *ctx, const void *data, size_t len);
extern void n00b_md5_finalize(n00b_md5_ctx_t *ctx, uint8_t digest[16]);

/// One-shot MD5 hash.
extern void n00b_md5(const uint8_t *data, size_t len, uint8_t out[16]);
