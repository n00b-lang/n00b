/** @file src/chalk/elf.c — ELF codec (primary + fallback).
 *
 *  Thin wrapper on top of n00b's ELF parse + build
 *  (compiler/objfile/elf.h, elf_build.h). chalk embeds the mark as a
 *  `.chalk.mark` section in the ELF image. Same insert/delete/extract
 *  semantics as the Mach-O and PE codecs (parse → strip prior chalk
 *  section → build canonical unchalked bytes → hash → finalize mark →
 *  add `.chalk.mark` section → rebuild).
 *
 *  Hash invariant: sha256(n00b_elf_build(bin without .chalk.mark)).
 *
 *  The fallback codec (codecFallbackElf in chalk) is preserved as a
 *  hex-offset scan that doesn't depend on the full parser; it stays
 *  in elf_fallback.c. */

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/sha256.h"
#include "core/alloc.h"
#include "compiler/objfile/elf.h"
#include "compiler/objfile/elf_build.h"
#include "compiler/objfile/elf_types.h"
#include "compiler/objfile/bstream.h"
#include "chalk/n00b_chalk.h"
#include "internal/chalk/mark_internal.h"
#include "internal/chalk/sidecar_internal.h"
#include "internal/chalk/file_io.h"

#include <string.h>

#define CHALK_SECTION_NAME ".chalk.mark"

static n00b_elf_binary_t *
parse_elf(n00b_buffer_t *bytes)
{
    if (!bytes) return nullptr;
    n00b_bstream_t *bs = n00b_bstream_new(bytes);
    if (!bs) return nullptr;
    auto pr = n00b_elf_parse(bs);
    if (n00b_result_is_err(pr)) return nullptr;
    return n00b_result_get(pr);
}

static n00b_buffer_t *
sha256_buffer(n00b_buffer_t *in)
{
    n00b_sha256_digest_t w;
    n00b_sha256_hash(in->data, in->byte_len, w);
    uint8_t b[32];
    for (int i = 0; i < 8; i++) {
        uint32_t x   = w[i];
        b[i * 4]     = (uint8_t)((x >> 24) & 0xff);
        b[i * 4 + 1] = (uint8_t)((x >> 16) & 0xff);
        b[i * 4 + 2] = (uint8_t)((x >> 8) & 0xff);
        b[i * 4 + 3] = (uint8_t)(x & 0xff);
    }
    return n00b_buffer_from_bytes((char *)b, 32);
}

n00b_result_t(n00b_buffer_t *)
n00b_chalk_elf_hash_buffer(n00b_buffer_t *bytes)
{
    n00b_elf_binary_t *bin = parse_elf(bytes);
    if (!bin) {
        // Fall back to raw sha256 if the parser refuses the input.
        return n00b_result_ok(n00b_buffer_t *,
                              n00b_chalk_sha256_buffer(bytes));
    }
    n00b_elf_remove_section(bin, CHALK_SECTION_NAME);
    auto br = n00b_elf_build(bin);
    if (n00b_result_is_err(br)) {
        return n00b_result_ok(n00b_buffer_t *,
                              n00b_chalk_sha256_buffer(bytes));
    }
    return n00b_result_ok(n00b_buffer_t *,
                          sha256_buffer(n00b_result_get(br)));
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_elf_insert_buffer(n00b_buffer_t *bytes, n00b_chalk_mark_t *mark)
{
    if (!bytes || !mark) return n00b_result_err(n00b_chalk_io_result_t *, 1);
    n00b_elf_binary_t *bin = parse_elf(bytes);
    if (!bin) return n00b_result_err(n00b_chalk_io_result_t *, 2);

    n00b_elf_remove_section(bin, CHALK_SECTION_NAME);

    auto br = n00b_elf_build(bin);
    if (n00b_result_is_err(br)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 3);
    }
    n00b_buffer_t *hash_buf = sha256_buffer(n00b_result_get(br));

    auto fin = n00b_chalk_mark_finalize(mark, hash_buf);
    if (n00b_result_is_err(fin)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 4);
    }
    n00b_buffer_t *encoded = n00b_result_get(fin);

    n00b_elf_section_t *sec = n00b_elf_add_section(bin, CHALK_SECTION_NAME,
                                                    SHT_PROGBITS, 0);
    if (!sec) {
        return n00b_result_err(n00b_chalk_io_result_t *, 5);
    }
    sec->content = encoded;
    sec->size    = encoded->byte_len;

    auto br2 = n00b_elf_build(bin);
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
n00b_chalk_elf_delete_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_chalk_io_result_t *, 1);
    n00b_elf_binary_t *bin = parse_elf(bytes);
    if (!bin) return n00b_result_err(n00b_chalk_io_result_t *, 2);
    n00b_elf_remove_section(bin, CHALK_SECTION_NAME);
    auto br = n00b_elf_build(bin);
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
n00b_chalk_elf_extract_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_chalk_extract_result_t *, 1);
    n00b_elf_binary_t *bin = parse_elf(bytes);
    if (!bin) return n00b_result_err(n00b_chalk_extract_result_t *, 2);
    n00b_elf_section_t *sec = n00b_elf_section_by_name(bin, CHALK_SECTION_NAME);
    if (!sec || !sec->content) {
        return n00b_result_err(n00b_chalk_extract_result_t *, 3);
    }
    // ELF section content is exact-size (no file-alignment padding
    // like PE), so we can hand the buffer straight to the parser.
    size_t mark_len = (size_t)sec->size;
    if (mark_len == 0 || mark_len > sec->content->byte_len) {
        mark_len = sec->content->byte_len;
    }
    n00b_buffer_t *trimmed = n00b_buffer_from_bytes(sec->content->data,
                                                     (int64_t)mark_len);
    return n00b_chalk_sidecar_parse_bytes(trimmed, N00B_CHALK_CODEC_ELF);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_elf_insert_file(n00b_string_t *path, n00b_chalk_mark_t *mark)
{
    return n00b_chalk_file_insert_via(path, mark, n00b_chalk_elf_insert_buffer);
}
n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_elf_delete_file(n00b_string_t *path)
{
    return n00b_chalk_file_delete_via(path, n00b_chalk_elf_delete_buffer);
}
n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_elf_extract_file(n00b_string_t *path)
{
    return n00b_chalk_file_extract_via(path, n00b_chalk_elf_extract_buffer);
}
n00b_result_t(n00b_buffer_t *)
n00b_chalk_elf_hash_file(n00b_string_t *path)
{
    return n00b_chalk_file_hash_via(path, n00b_chalk_elf_hash_buffer);
}
