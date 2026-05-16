/** @file src/chalk/safetensors.c — SafeTensors codec wrapper.
 *
 *  Thin adapter on top of the lifted C core in safetensors_core.c.
 *  Buffer mode end-to-end.
 */

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/alloc.h"
#include "chalk/n00b_chalk.h"
#include "internal/chalk/safetensors_core.h"
#include "internal/chalk/mark_internal.h"
#include "internal/chalk/sidecar_internal.h"

#include <string.h>

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
n00b_chalk_safetensors_hash_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_buffer_t *, 1);
    chalk_st_t *st = chalk_st_parse((const uint8_t *)bytes->data,
                                     bytes->byte_len);
    if (!st) return n00b_result_err(n00b_buffer_t *, 2);
    char hex[65];
    if (chalk_st_unchalked_hash(st, hex) != CHALK_ST_OK) {
        chalk_st_free(st);
        return n00b_result_err(n00b_buffer_t *, 3);
    }
    chalk_st_free(st);
    uint8_t raw[32];
    if (!hex_to_bytes32(hex, raw)) return n00b_result_err(n00b_buffer_t *, 4);
    return n00b_result_ok(n00b_buffer_t *,
                          n00b_buffer_from_bytes((char *)raw, 32));
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_safetensors_insert_buffer(n00b_buffer_t *bytes,
                                     n00b_chalk_mark_t *mark)
{
    if (!bytes || !mark) return n00b_result_err(n00b_chalk_io_result_t *, 1);
    chalk_st_t *st = chalk_st_parse((const uint8_t *)bytes->data,
                                     bytes->byte_len);
    if (!st) return n00b_result_err(n00b_chalk_io_result_t *, 2);

    char hex[65];
    if (chalk_st_unchalked_hash(st, hex) != CHALK_ST_OK) {
        chalk_st_free(st);
        return n00b_result_err(n00b_chalk_io_result_t *, 3);
    }
    uint8_t raw[32];
    if (!hex_to_bytes32(hex, raw)) {
        chalk_st_free(st);
        return n00b_result_err(n00b_chalk_io_result_t *, 4);
    }
    n00b_buffer_t *hash_buf = n00b_buffer_from_bytes((char *)raw, 32);

    auto fin = n00b_chalk_mark_finalize(mark, hash_buf);
    if (n00b_result_is_err(fin)) {
        chalk_st_free(st);
        return n00b_result_err(n00b_chalk_io_result_t *, 5);
    }
    n00b_buffer_t *encoded = n00b_result_get(fin);

    if (chalk_st_set_chalk(st, encoded->data,
                            (size_t)encoded->byte_len) != CHALK_ST_OK) {
        chalk_st_free(st);
        return n00b_result_err(n00b_chalk_io_result_t *, 6);
    }
    size_t         out_len;
    const uint8_t *out_ptr = chalk_st_get_buffer(st, &out_len);
    n00b_buffer_t *out     = n00b_buffer_from_bytes((char *)out_ptr,
                                                     (int64_t)out_len);
    chalk_st_free(st);

    auto r = (n00b_chalk_io_result_t *)n00b_alloc(n00b_chalk_io_result_t);
    r->kind           = N00B_CHALK_OUT_IN_BAND;
    r->bytes          = out;
    r->sidecar_suffix = nullptr;
    return n00b_result_ok(n00b_chalk_io_result_t *, r);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_safetensors_delete_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_chalk_io_result_t *, 1);
    chalk_st_t *st = chalk_st_parse((const uint8_t *)bytes->data,
                                     bytes->byte_len);
    if (!st) return n00b_result_err(n00b_chalk_io_result_t *, 2);
    chalk_st_status_t s = chalk_st_remove_chalk(st);
    if (s != CHALK_ST_OK && s != CHALK_ST_ERR_NO_CHALK) {
        chalk_st_free(st);
        return n00b_result_err(n00b_chalk_io_result_t *, 3);
    }
    size_t         out_len;
    const uint8_t *out_ptr = chalk_st_get_buffer(st, &out_len);
    n00b_buffer_t *out     = n00b_buffer_from_bytes((char *)out_ptr,
                                                     (int64_t)out_len);
    chalk_st_free(st);

    auto r = (n00b_chalk_io_result_t *)n00b_alloc(n00b_chalk_io_result_t);
    r->kind           = N00B_CHALK_OUT_IN_BAND;
    r->bytes          = out;
    r->sidecar_suffix = nullptr;
    return n00b_result_ok(n00b_chalk_io_result_t *, r);
}

n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_safetensors_extract_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_chalk_extract_result_t *, 1);
    chalk_st_t *st = chalk_st_parse((const uint8_t *)bytes->data,
                                     bytes->byte_len);
    if (!st) return n00b_result_err(n00b_chalk_extract_result_t *, 2);
    size_t payload_len;
    char  *payload = chalk_st_get_payload(st, &payload_len);
    if (!payload) {
        chalk_st_free(st);
        return n00b_result_err(n00b_chalk_extract_result_t *, 3);
    }
    n00b_buffer_t *payload_buf = n00b_buffer_from_bytes(payload,
                                                         (int64_t)payload_len);
    chalk_st_free(st);
    return n00b_chalk_sidecar_parse_bytes(payload_buf,
                                          N00B_CHALK_CODEC_SAFETENSORS);
}

#include "internal/chalk/file_io.h"

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_safetensors_insert_file(n00b_string_t *path,
                                   n00b_chalk_mark_t *mark)
{
    return n00b_chalk_file_insert_via(path, mark,
                                      n00b_chalk_safetensors_insert_buffer);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_safetensors_delete_file(n00b_string_t *path)
{
    return n00b_chalk_file_delete_via(path,
                                      n00b_chalk_safetensors_delete_buffer);
}

n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_safetensors_extract_file(n00b_string_t *path)
{
    return n00b_chalk_file_extract_via(path,
                                       n00b_chalk_safetensors_extract_buffer);
}

n00b_result_t(n00b_buffer_t *)
n00b_chalk_safetensors_hash_file(n00b_string_t *path)
{
    return n00b_chalk_file_hash_via(path,
                                    n00b_chalk_safetensors_hash_buffer);
}
