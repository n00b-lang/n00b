/** @file src/chalk/macho.c — Mach-O codec wrapper.
 *
 *  Adapter from the lifted Mach-O core (macho_core.c / macho_parse.c
 *  / macho_stream.c) to the libchalk codec entry points. Buffer mode
 *  end-to-end. The lifted core expects a parsed macho_fat_t handle;
 *  every entry point here builds one from a fresh buffer copy. */

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/alloc.h"
#include "chalk/n00b_chalk.h"
#include "internal/chalk/macho_core.h"
#include "internal/chalk/macho_parse.h"
#include "internal/chalk/macho_stream.h"
#include "internal/chalk/macho_types.h"
#include "internal/chalk/mark_internal.h"
#include "internal/chalk/sidecar_internal.h"

#include <string.h>

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

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

// Parse the input buffer into a fresh macho_fat_t. Returns NULL on
// parse failure.
static macho_fat_t *
parse_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return nullptr;
    n00b_buffer_t  *copy = n00b_buffer_from_bytes(bytes->data,
                                                   (int64_t)bytes->byte_len);
    macho_stream_t *s    = macho_stream_new(copy);
    if (!s) return nullptr;
    auto pr = macho_parse(s);
    if (n00b_result_is_err(pr)) return nullptr;
    return (macho_fat_t *)n00b_result_get(pr);
}

// Extract the first binary slice from a parsed fat. The codec
// supports single-binary inputs only; multi-arch fat handling is a
// later refinement.
static macho_binary_t *
first_binary(macho_fat_t *fat)
{
    if (!fat || fat->count == 0) return nullptr;
    return fat->binaries[0];
}

// -----------------------------------------------------------------------
// Codec entry points
// -----------------------------------------------------------------------

n00b_result_t(n00b_buffer_t *)
n00b_chalk_macho_hash_buffer(n00b_buffer_t *bytes)
{
    macho_fat_t *fat = parse_buffer(bytes);
    if (!fat) return n00b_result_err(n00b_buffer_t *, 1);
    macho_binary_t *bin = first_binary(fat);
    if (!bin) return n00b_result_err(n00b_buffer_t *, 2);
    char hex[65];
    if (chalk_macho_unchalked_hash(bin, hex) != CHALK_MACHO_OK) {
        return n00b_result_err(n00b_buffer_t *, 3);
    }
    uint8_t raw[32];
    if (!hex_to_bytes32(hex, raw)) {
        return n00b_result_err(n00b_buffer_t *, 4);
    }
    return n00b_result_ok(n00b_buffer_t *,
                          n00b_buffer_from_bytes((char *)raw, 32));
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_macho_insert_buffer(n00b_buffer_t *bytes, n00b_chalk_mark_t *mark)
{
    if (!bytes || !mark) return n00b_result_err(n00b_chalk_io_result_t *, 1);
    macho_fat_t *fat = parse_buffer(bytes);
    if (!fat) return n00b_result_err(n00b_chalk_io_result_t *, 2);
    macho_binary_t *bin = first_binary(fat);
    if (!bin) return n00b_result_err(n00b_chalk_io_result_t *, 3);

    // Remove any pre-existing chalk note so the unchalked-hash + new
    // mark are computed against the clean binary.
    (void)chalk_macho_remove_note(bin);

    char hex[65];
    if (chalk_macho_unchalked_hash(bin, hex) != CHALK_MACHO_OK) {
        return n00b_result_err(n00b_chalk_io_result_t *, 4);
    }
    uint8_t raw[32];
    if (!hex_to_bytes32(hex, raw)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 5);
    }
    n00b_buffer_t *hash_buf = n00b_buffer_from_bytes((char *)raw, 32);

    auto fin = n00b_chalk_mark_finalize(mark, hash_buf);
    if (n00b_result_is_err(fin)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 6);
    }
    n00b_buffer_t *encoded = n00b_result_get(fin);

    if (chalk_macho_add_note(bin,
                              (const uint8_t *)encoded->data,
                              (size_t)encoded->byte_len) != CHALK_MACHO_OK) {
        return n00b_result_err(n00b_chalk_io_result_t *, 7);
    }
    size_t         out_len;
    const uint8_t *out_ptr = chalk_macho_get_buffer(bin, &out_len);
    n00b_buffer_t *out     = n00b_buffer_from_bytes((char *)out_ptr,
                                                     (int64_t)out_len);

    auto r = (n00b_chalk_io_result_t *)n00b_alloc(n00b_chalk_io_result_t);
    r->kind           = N00B_CHALK_OUT_IN_BAND;
    r->bytes          = out;
    r->sidecar_suffix = nullptr;
    return n00b_result_ok(n00b_chalk_io_result_t *, r);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_macho_delete_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_chalk_io_result_t *, 1);
    macho_fat_t *fat = parse_buffer(bytes);
    if (!fat) return n00b_result_err(n00b_chalk_io_result_t *, 2);
    macho_binary_t *bin = first_binary(fat);
    if (!bin) return n00b_result_err(n00b_chalk_io_result_t *, 3);

    chalk_macho_status_t s = chalk_macho_remove_note(bin);
    if (s != CHALK_MACHO_OK && s != CHALK_MACHO_ERR_NO_CHALK_NOTE) {
        return n00b_result_err(n00b_chalk_io_result_t *, 4);
    }
    size_t         out_len;
    const uint8_t *out_ptr = chalk_macho_get_buffer(bin, &out_len);
    n00b_buffer_t *out     = n00b_buffer_from_bytes((char *)out_ptr,
                                                     (int64_t)out_len);

    auto r = (n00b_chalk_io_result_t *)n00b_alloc(n00b_chalk_io_result_t);
    r->kind           = N00B_CHALK_OUT_IN_BAND;
    r->bytes          = out;
    r->sidecar_suffix = nullptr;
    return n00b_result_ok(n00b_chalk_io_result_t *, r);
}

n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_macho_extract_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_chalk_extract_result_t *, 1);
    macho_fat_t *fat = parse_buffer(bytes);
    if (!fat) return n00b_result_err(n00b_chalk_extract_result_t *, 2);
    macho_binary_t *bin = first_binary(fat);
    if (!bin) return n00b_result_err(n00b_chalk_extract_result_t *, 3);
    size_t   payload_len;
    uint8_t *payload = chalk_macho_get_chalk_payload(bin, &payload_len);
    if (!payload) return n00b_result_err(n00b_chalk_extract_result_t *, 4);
    n00b_buffer_t *payload_buf = n00b_buffer_from_bytes((char *)payload,
                                                         (int64_t)payload_len);
    return n00b_chalk_sidecar_parse_bytes(payload_buf,
                                          N00B_CHALK_CODEC_MACHO);
}

n00b_chalk_macho_sig_kind_t
n00b_chalk_macho_signature_kind(n00b_buffer_t *bytes)
{
    macho_fat_t *fat = parse_buffer(bytes);
    if (!fat) return N00B_CHALK_MACHO_SIG_NONE;
    macho_binary_t *bin = first_binary(fat);
    if (!bin) return N00B_CHALK_MACHO_SIG_NONE;
    switch (chalk_macho_signature_kind(bin)) {
    case CHALK_MACHO_SIG_NONE:      return N00B_CHALK_MACHO_SIG_NONE;
    case CHALK_MACHO_SIG_ADHOC:     return N00B_CHALK_MACHO_SIG_ADHOC;
    case CHALK_MACHO_SIG_REAL_CERT: return N00B_CHALK_MACHO_SIG_CERT;
    case CHALK_MACHO_SIG_MALFORMED: return N00B_CHALK_MACHO_SIG_MALFORMED;
    default:                        return N00B_CHALK_MACHO_SIG_MALFORMED;
    }
}

n00b_result_t(n00b_buffer_t *)
n00b_chalk_macho_strip_signature(n00b_buffer_t *bytes)
{
    macho_fat_t *fat = parse_buffer(bytes);
    if (!fat) return n00b_result_err(n00b_buffer_t *, 1);
    macho_binary_t *bin = first_binary(fat);
    if (!bin) return n00b_result_err(n00b_buffer_t *, 2);
    if (chalk_macho_strip_signature(bin) != CHALK_MACHO_OK) {
        return n00b_result_err(n00b_buffer_t *, 3);
    }
    size_t         out_len;
    const uint8_t *out_ptr = chalk_macho_get_buffer(bin, &out_len);
    return n00b_result_ok(n00b_buffer_t *,
                          n00b_buffer_from_bytes((char *)out_ptr,
                                                  (int64_t)out_len));
}

#include "internal/chalk/file_io.h"

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_macho_insert_file(n00b_string_t *path, n00b_chalk_mark_t *mark)
{
    return n00b_chalk_file_insert_via(path, mark,
                                      n00b_chalk_macho_insert_buffer);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_macho_delete_file(n00b_string_t *path)
{
    return n00b_chalk_file_delete_via(path, n00b_chalk_macho_delete_buffer);
}

n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_macho_extract_file(n00b_string_t *path)
{
    return n00b_chalk_file_extract_via(path,
                                       n00b_chalk_macho_extract_buffer);
}

n00b_result_t(n00b_buffer_t *)
n00b_chalk_macho_hash_file(n00b_string_t *path)
{
    return n00b_chalk_file_hash_via(path, n00b_chalk_macho_hash_buffer);
}
