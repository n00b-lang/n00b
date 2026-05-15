/** @file src/chalk/pe.c — PE codec wrapper.
 *
 *  Thin adapter on top of n00b's PE parse + build
 *  (compiler/objfile/pe.h, pe_build.h). Embeds the chalk mark as a
 *  `.chalk` section in the PE image.
 *
 *  Hash convention: we compute the unchalked SHA-256 as
 *  sha256(n00b_pe_build(bin with .chalk removed)). This is stable
 *  across remarks because build is deterministic on the parse tree:
 *  parse(build(bin')) -> bin' for any bin' that came out of parse,
 *  so the second extract recomputes the same hash that the first
 *  insert wrote into CHALK_ID. */

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/sha256.h"
#include "core/alloc.h"
#include "compiler/objfile/pe.h"
#include "compiler/objfile/pe_build.h"
#include "compiler/objfile/pe_types.h"
#include "compiler/objfile/bstream.h"
#include "chalk/n00b_chalk.h"
#include "chalk/n00b_chalk_pe.h"
#include "internal/chalk/mark_internal.h"
#include "internal/chalk/sidecar_internal.h"
#include "internal/chalk/file_io.h"

#include <string.h>

#define CHALK_SECTION_NAME ".chalk"
#define CHALK_SECTION_CHARACTERISTICS                                          \
    (N00B_PE_SCN_CNT_INITIALIZED | N00B_PE_SCN_MEM_READ                        \
     | N00B_PE_SCN_MEM_DISCARDABLE)

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

static n00b_pe_binary_t *
parse_pe(n00b_buffer_t *bytes)
{
    if (!bytes) return nullptr;
    // Defensive copy: n00b_pe_parse stores the stream/buffer
    // reference in the parsed binary; if the caller passed a buffer
    // that some other parser handle already owns, parse-state
    // collisions are observed. Copying here costs an allocation but
    // keeps the parsed binary self-contained.
    n00b_buffer_t *copy = n00b_buffer_from_bytes(bytes->data,
                                                  (int64_t)bytes->byte_len);
    n00b_bstream_t *bs = n00b_bstream_new(copy);
    if (!bs) return nullptr;
    auto pr = n00b_pe_parse(bs);
    if (n00b_result_is_err(pr)) return nullptr;
    return n00b_result_get(pr);
}

// Build the canonical "unchalked" bytes — no .chalk section.
static n00b_buffer_t *
build_unchalked(n00b_pe_binary_t *bin)
{
    n00b_pe_remove_section(bin, CHALK_SECTION_NAME);
    auto br = n00b_pe_build(bin);
    if (n00b_result_is_err(br)) return nullptr;
    return n00b_result_get(br);
}

static n00b_buffer_t *
sha256_buffer(n00b_buffer_t *in)
{
    n00b_sha256_digest_t w;
    n00b_sha256_hash(in->data, in->byte_len, w);
    uint8_t b[32];
    for (int i = 0; i < 8; i++) {
        uint32_t x      = w[i];
        b[i * 4]        = (uint8_t)((x >> 24) & 0xff);
        b[i * 4 + 1]    = (uint8_t)((x >> 16) & 0xff);
        b[i * 4 + 2]    = (uint8_t)((x >> 8) & 0xff);
        b[i * 4 + 3]    = (uint8_t)(x & 0xff);
    }
    return n00b_buffer_from_bytes((char *)b, 32);
}

// -----------------------------------------------------------------------
// Codec entry points
// -----------------------------------------------------------------------

n00b_result_t(n00b_buffer_t *)
n00b_chalk_pe_hash_buffer(n00b_buffer_t *bytes)
{
    n00b_pe_binary_t *bin = parse_pe(bytes);
    if (!bin) return n00b_result_err(n00b_buffer_t *, 1);
    n00b_buffer_t *unchalked = build_unchalked(bin);
    if (!unchalked) return n00b_result_err(n00b_buffer_t *, 2);
    return n00b_result_ok(n00b_buffer_t *, sha256_buffer(unchalked));
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_pe_insert_buffer(n00b_buffer_t *bytes, n00b_chalk_mark_t *mark)
{
    if (!bytes || !mark) return n00b_result_err(n00b_chalk_io_result_t *, 1);
    n00b_pe_binary_t *bin = parse_pe(bytes);
    if (!bin) return n00b_result_err(n00b_chalk_io_result_t *, 2);

    // Strip any existing chalk section so the unchalked hash is
    // computed against a clean baseline.
    n00b_pe_remove_section(bin, CHALK_SECTION_NAME);

    auto br = n00b_pe_build(bin);
    if (n00b_result_is_err(br)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 3);
    }
    n00b_buffer_t *unchalked = n00b_result_get(br);
    n00b_buffer_t *hash_buf  = sha256_buffer(unchalked);

    auto fin = n00b_chalk_mark_finalize(mark, hash_buf);
    if (n00b_result_is_err(fin)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 4);
    }
    n00b_buffer_t *encoded = n00b_result_get(fin);

    // Append the chalk section and rebuild.
    n00b_pe_section_t *sec = n00b_pe_add_section(bin, CHALK_SECTION_NAME,
                                                  CHALK_SECTION_CHARACTERISTICS);
    if (!sec) {
        return n00b_result_err(n00b_chalk_io_result_t *, 5);
    }
    sec->content      = encoded;
    sec->raw_size     = (uint32_t)encoded->byte_len;
    sec->virtual_size = (uint32_t)encoded->byte_len;

    auto br2 = n00b_pe_build(bin);
    if (n00b_result_is_err(br2)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 6);
    }
    n00b_chalk_io_result_t *r = n00b_alloc(n00b_chalk_io_result_t);
    r->kind           = N00B_CHALK_OUT_IN_BAND;
    r->bytes          = n00b_result_get(br2);
    r->sidecar_suffix = nullptr;
    return n00b_result_ok(n00b_chalk_io_result_t *, r);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_pe_delete_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_chalk_io_result_t *, 1);
    n00b_pe_binary_t *bin = parse_pe(bytes);
    if (!bin) return n00b_result_err(n00b_chalk_io_result_t *, 2);
    n00b_pe_remove_section(bin, CHALK_SECTION_NAME);
    auto br = n00b_pe_build(bin);
    if (n00b_result_is_err(br)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 3);
    }
    n00b_chalk_io_result_t *r = n00b_alloc(n00b_chalk_io_result_t);
    r->kind           = N00B_CHALK_OUT_IN_BAND;
    r->bytes          = n00b_result_get(br);
    r->sidecar_suffix = nullptr;
    return n00b_result_ok(n00b_chalk_io_result_t *, r);
}

n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_pe_extract_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_chalk_extract_result_t *, 1);
    n00b_bstream_t *bs = n00b_bstream_new(bytes);
    if (!bs) return n00b_result_err(n00b_chalk_extract_result_t *, 2);
    auto pr = n00b_pe_parse(bs);
    if (n00b_result_is_err(pr)) {
        return n00b_result_err(n00b_chalk_extract_result_t *, 2);
    }
    n00b_pe_binary_t *bin = n00b_result_get(pr);
    n00b_pe_section_t *sec = n00b_pe_section_by_name(bin, CHALK_SECTION_NAME);
    if (!sec || !sec->content) {
        return n00b_result_err(n00b_chalk_extract_result_t *, 3);
    }
    // sec->content holds raw_size bytes (file-alignment padded); the
    // mark JSON length is virtual_size. Trim to drop the trailing
    // alignment zeros before parsing.
    size_t mark_len = (size_t)sec->virtual_size;
    if (mark_len == 0 || mark_len > sec->content->byte_len) {
        mark_len = sec->content->byte_len;
    }
    n00b_buffer_t *trimmed = n00b_buffer_from_bytes(sec->content->data,
                                                     (int64_t)mark_len);
    return n00b_chalk_sidecar_parse_bytes(trimmed, N00B_CHALK_CODEC_PE);
}

n00b_chalk_pe_sig_kind_t
n00b_chalk_pe_signature_kind(n00b_buffer_t *bytes)
{
    n00b_pe_binary_t *bin = parse_pe(bytes);
    if (!bin) return N00B_CHALK_PE_SIG_NONE;
    return bin->num_certificates > 0 ? N00B_CHALK_PE_SIG_AUTHENTICODE
                                      : N00B_CHALK_PE_SIG_NONE;
}

// File-mode entry points via the shared helper.
n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_pe_insert_file(n00b_string_t *path, n00b_chalk_mark_t *mark)
{
    return n00b_chalk_file_insert_via(path, mark, n00b_chalk_pe_insert_buffer);
}
n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_pe_delete_file(n00b_string_t *path)
{
    return n00b_chalk_file_delete_via(path, n00b_chalk_pe_delete_buffer);
}
n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_pe_extract_file(n00b_string_t *path)
{
    return n00b_chalk_file_extract_via(path, n00b_chalk_pe_extract_buffer);
}
n00b_result_t(n00b_buffer_t *)
n00b_chalk_pe_hash_file(n00b_string_t *path)
{
    return n00b_chalk_file_hash_via(path, n00b_chalk_pe_hash_buffer);
}
