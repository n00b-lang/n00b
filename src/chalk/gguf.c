/** @file src/chalk/gguf.c — GGUF codec wrapper.
 *
 *  Thin adapter that exposes the libchalk codec API on top of the
 *  lifted C core in gguf_core.c. Buffer mode end-to-end. */

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/alloc.h"
#include "chalk/n00b_chalk.h"
#include "internal/chalk/gguf_core.h"
#include "internal/chalk/mark_internal.h"
#include "internal/chalk/sidecar_internal.h"

#include <string.h>

// Convert a 64-char hex string to 32 raw bytes (caller-supplied dst).
static bool
hex_to_bytes32(const char *hex, uint8_t out[32])
{
    for (int i = 0; i < 32; i++) {
        char hi = hex[i * 2];
        char lo = hex[i * 2 + 1];
        int  h, l;
        if      (hi >= '0' && hi <= '9') h = hi - '0';
        else if (hi >= 'a' && hi <= 'f') h = hi - 'a' + 10;
        else if (hi >= 'A' && hi <= 'F') h = hi - 'A' + 10;
        else return false;
        if      (lo >= '0' && lo <= '9') l = lo - '0';
        else if (lo >= 'a' && lo <= 'f') l = lo - 'a' + 10;
        else if (lo >= 'A' && lo <= 'F') l = lo - 'A' + 10;
        else return false;
        out[i] = (uint8_t)((h << 4) | l);
    }
    return true;
}

n00b_result_t(n00b_buffer_t *)
n00b_chalk_gguf_hash_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_buffer_t *, 1);
    chalk_gguf_t *g = chalk_gguf_parse((const uint8_t *)bytes->data,
                                        bytes->byte_len);
    if (!g) return n00b_result_err(n00b_buffer_t *, 2);
    char hex[65];
    if (chalk_gguf_unchalked_hash(g, hex) != CHALK_GGUF_OK) {
        chalk_gguf_free(g);
        return n00b_result_err(n00b_buffer_t *, 3);
    }
    chalk_gguf_free(g);
    uint8_t raw[32];
    if (!hex_to_bytes32(hex, raw)) {
        return n00b_result_err(n00b_buffer_t *, 4);
    }
    return n00b_result_ok(n00b_buffer_t *,
                          n00b_buffer_from_bytes((char *)raw, 32));
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_gguf_insert_buffer(n00b_buffer_t *bytes, n00b_chalk_mark_t *mark)
{
    if (!bytes || !mark) return n00b_result_err(n00b_chalk_io_result_t *, 1);
    chalk_gguf_t *g = chalk_gguf_parse((const uint8_t *)bytes->data,
                                        bytes->byte_len);
    if (!g) return n00b_result_err(n00b_chalk_io_result_t *, 2);

    // Compute unchalked hash for CHALK_ID / HASH derivation.
    char hex[65];
    if (chalk_gguf_unchalked_hash(g, hex) != CHALK_GGUF_OK) {
        chalk_gguf_free(g);
        return n00b_result_err(n00b_chalk_io_result_t *, 3);
    }
    uint8_t raw[32];
    if (!hex_to_bytes32(hex, raw)) {
        chalk_gguf_free(g);
        return n00b_result_err(n00b_chalk_io_result_t *, 4);
    }
    n00b_buffer_t *hash_buf = n00b_buffer_from_bytes((char *)raw, 32);

    auto fin = n00b_chalk_mark_finalize(mark, hash_buf);
    if (n00b_result_is_err(fin)) {
        chalk_gguf_free(g);
        return n00b_result_err(n00b_chalk_io_result_t *, 5);
    }
    n00b_buffer_t *encoded = n00b_result_get(fin);

    if (chalk_gguf_set_chalk(g, encoded->data,
                              (size_t)encoded->byte_len) != CHALK_GGUF_OK) {
        chalk_gguf_free(g);
        return n00b_result_err(n00b_chalk_io_result_t *, 6);
    }
    size_t         out_len;
    const uint8_t *out_ptr = chalk_gguf_get_buffer(g, &out_len);
    n00b_buffer_t *out     = n00b_buffer_from_bytes((char *)out_ptr,
                                                     (int64_t)out_len);
    chalk_gguf_free(g);

    auto r = (n00b_chalk_io_result_t *)n00b_alloc(n00b_chalk_io_result_t);
    r->kind           = N00B_CHALK_OUT_IN_BAND;
    r->bytes          = out;
    r->sidecar_suffix = nullptr;
    return n00b_result_ok(n00b_chalk_io_result_t *, r);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_gguf_delete_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_chalk_io_result_t *, 1);
    chalk_gguf_t *g = chalk_gguf_parse((const uint8_t *)bytes->data,
                                        bytes->byte_len);
    if (!g) return n00b_result_err(n00b_chalk_io_result_t *, 2);
    chalk_gguf_status_t s = chalk_gguf_remove_chalk(g);
    if (s != CHALK_GGUF_OK && s != CHALK_GGUF_ERR_NO_CHALK) {
        chalk_gguf_free(g);
        return n00b_result_err(n00b_chalk_io_result_t *, 3);
    }
    size_t         out_len;
    const uint8_t *out_ptr = chalk_gguf_get_buffer(g, &out_len);
    n00b_buffer_t *out     = n00b_buffer_from_bytes((char *)out_ptr,
                                                     (int64_t)out_len);
    chalk_gguf_free(g);

    auto r = (n00b_chalk_io_result_t *)n00b_alloc(n00b_chalk_io_result_t);
    r->kind           = N00B_CHALK_OUT_IN_BAND;
    r->bytes          = out;
    r->sidecar_suffix = nullptr;
    return n00b_result_ok(n00b_chalk_io_result_t *, r);
}

n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_gguf_extract_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_chalk_extract_result_t *, 1);
    chalk_gguf_t *g = chalk_gguf_parse((const uint8_t *)bytes->data,
                                        bytes->byte_len);
    if (!g) return n00b_result_err(n00b_chalk_extract_result_t *, 2);
    size_t      payload_len;
    const char *payload = chalk_gguf_get_payload(g, &payload_len);
    if (!payload) {
        chalk_gguf_free(g);
        return n00b_result_err(n00b_chalk_extract_result_t *, 3);
    }
    n00b_buffer_t *payload_buf = n00b_buffer_from_bytes((char *)payload,
                                                         (int64_t)payload_len);
    chalk_gguf_free(g);
    return n00b_chalk_sidecar_parse_bytes(payload_buf,
                                          N00B_CHALK_CODEC_GGUF);
}

// File-mode stubs.
n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_gguf_insert_file(n00b_string_t *path, n00b_chalk_mark_t *mark)
{
    (void)path;
    (void)mark;
    return n00b_result_err(n00b_chalk_io_result_t *, 1);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_gguf_delete_file(n00b_string_t *path)
{
    (void)path;
    return n00b_result_err(n00b_chalk_io_result_t *, 1);
}

n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_gguf_extract_file(n00b_string_t *path)
{
    (void)path;
    return n00b_result_err(n00b_chalk_extract_result_t *, 1);
}

n00b_result_t(n00b_buffer_t *)
n00b_chalk_gguf_hash_file(n00b_string_t *path)
{
    (void)path;
    return n00b_result_err(n00b_buffer_t *, 1);
}
