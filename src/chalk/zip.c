/** @file src/chalk/zip.c — ZIP archive codec.
 *
 *  Ports chalk/plugins/codecZip.nim. The chalk mark lives as an entry
 *  named "chalk.json" at the archive root. Unchalked hash is computed
 *  over a canonical concatenation of remaining entries:
 *
 *      H( decimal_path_count
 *         + for each path (sorted by name, after stripping chalk.json):
 *           decimal_name_len + name + decimal_content_len + content )
 *
 *  In-process sub-scan: each entry whose bytes a libchalk codec
 *  claims gets its embedded chalk mark stripped via that codec's
 *  hash_buffer (which already produces the "unchalked" view) before
 *  being fed into the hash chain.
 *
 *  Without libzip the codec returns EUNSUPPORTED on every operation.
 */

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/sha256.h"
#include "core/alloc.h"
#include "chalk/n00b_chalk.h"
#include "internal/chalk/mark_internal.h"
#include "internal/chalk/sidecar_internal.h"
#include "internal/chalk/codec_table.h"
#include "internal/chalk/file_io.h"

#include <string.h>

#define CHALK_JSON_NAME "chalk.json"

#ifndef N00B_HAVE_LIBZIP

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_zip_insert_buffer(n00b_buffer_t *bytes, n00b_chalk_mark_t *mark)
{
    (void)bytes;
    (void)mark;
    return n00b_result_err(n00b_chalk_io_result_t *, 1);
}
n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_zip_delete_buffer(n00b_buffer_t *bytes)
{
    (void)bytes;
    return n00b_result_err(n00b_chalk_io_result_t *, 1);
}
n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_zip_extract_buffer(n00b_buffer_t *bytes)
{
    (void)bytes;
    return n00b_result_err(n00b_chalk_extract_result_t *, 1);
}
n00b_result_t(n00b_buffer_t *)
n00b_chalk_zip_hash_buffer(n00b_buffer_t *bytes)
{
    (void)bytes;
    return n00b_result_err(n00b_buffer_t *, 1);
}

#else /* N00B_HAVE_LIBZIP */

#include <zip.h>

// -----------------------------------------------------------------------
// Decimal-string helper (matches Nim's `$len(x)` formatting verbatim).
// -----------------------------------------------------------------------

static size_t
fmt_dec(uint64_t v, char out[24])
{
    char tmp[24];
    size_t n = 0;
    if (v == 0) { out[0] = '0'; return 1; }
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (size_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

// -----------------------------------------------------------------------
// Entry collection
// -----------------------------------------------------------------------

typedef struct {
    char    *name;
    size_t   name_len;
    uint8_t *bytes;
    size_t   bytes_len;
} zip_entry_t;

static int
entry_cmp(const void *a, const void *b)
{
    const zip_entry_t *pa = (const zip_entry_t *)a;
    const zip_entry_t *pb = (const zip_entry_t *)b;
    size_t n = pa->name_len < pb->name_len ? pa->name_len : pb->name_len;
    int c = memcmp(pa->name, pb->name, n);
    if (c != 0) return c;
    if (pa->name_len < pb->name_len) return -1;
    if (pa->name_len > pb->name_len) return 1;
    return 0;
}

// Read all archive entries (skipping `chalk.json`) into a fresh array.
// Each entry's bytes pass through the chalk codec dispatcher's
// hash_buffer when a codec claims them — that's the in-process
// sub-scan: nested artifacts contribute their unchalked-form bytes,
// not their raw chalked bytes.
static bool
read_entries(struct zip_t *zip, zip_entry_t **out_entries,
             size_t *out_n, bool *out_had_chalk_json)
{
    zip_int64_t count = zip_get_num_entries(zip, 0);
    if (count < 0) return false;
    *out_had_chalk_json = false;

    // Worst case: all entries kept.
    zip_entry_t *entries = n00b_alloc_array(zip_entry_t, (size_t)count);
    size_t       n       = 0;

    for (zip_int64_t i = 0; i < count; i++) {
        zip_stat_t st;
        if (zip_stat_index(zip, (zip_uint64_t)i, 0, &st) != 0) continue;
        if (!(st.valid & ZIP_STAT_NAME) || !(st.valid & ZIP_STAT_SIZE)) continue;
        if (strcmp(st.name, CHALK_JSON_NAME) == 0) {
            *out_had_chalk_json = true;
            continue;
        }
        // Skip directory entries (zero-length, name ends in '/').
        size_t nl = strlen(st.name);
        if (nl > 0 && st.name[nl - 1] == '/') continue;

        zip_file_t *zf = zip_fopen_index(zip, (zip_uint64_t)i, 0);
        if (!zf) continue;

        uint8_t *buf = (uint8_t *)n00b_alloc_array(char, (size_t)st.size + 1);
        zip_int64_t got = zip_fread(zf, buf, st.size);
        zip_fclose(zf);
        if (got < 0 || (zip_uint64_t)got != st.size) continue;

        // Recursive sub-scan: if a libchalk codec claims these bytes,
        // hash through its hash_buffer (which returns the SHA-256 of
        // the unchalked form). Otherwise feed the bytes verbatim.
        n00b_buffer_t *as_buf
            = n00b_buffer_from_bytes((char *)buf, (int64_t)st.size);
        n00b_chalk_codec_id_t cid = n00b_chalk_codec_detect(as_buf, nullptr);
        const n00b_chalk_codec_entry_t *ent = n00b_chalk_codec_entry(cid);
        if (ent && ent->hash_buffer) {
            auto hr = ent->hash_buffer(as_buf);
            if (n00b_result_is_ok(hr)) {
                n00b_buffer_t *h = n00b_result_get(hr);
                buf      = (uint8_t *)h->data;
                got      = (zip_int64_t)h->byte_len;
            }
        }

        char *name_copy = n00b_alloc_array(char, nl + 1);
        memcpy(name_copy, st.name, nl + 1);

        entries[n].name      = name_copy;
        entries[n].name_len  = nl;
        entries[n].bytes     = buf;
        entries[n].bytes_len = (size_t)got;
        n++;
    }

    qsort(entries, n, sizeof(zip_entry_t), entry_cmp);
    *out_entries = entries;
    *out_n       = n;
    return true;
}

static n00b_buffer_t *
hash_entries(zip_entry_t *entries, size_t n)
{
    n00b_sha256_ctx_t ctx;
    n00b_sha256_init(&ctx);

    char dec[24];
    size_t dl = fmt_dec((uint64_t)n, dec);
    n00b_sha256_update(&ctx, dec, dl);

    for (size_t i = 0; i < n; i++) {
        dl = fmt_dec((uint64_t)entries[i].name_len, dec);
        n00b_sha256_update(&ctx, dec, dl);
        n00b_sha256_update(&ctx, entries[i].name, entries[i].name_len);

        dl = fmt_dec((uint64_t)entries[i].bytes_len, dec);
        n00b_sha256_update(&ctx, dec, dl);
        n00b_sha256_update(&ctx, entries[i].bytes, entries[i].bytes_len);
    }
    n00b_sha256_digest_t words;
    n00b_sha256_finalize(&ctx, words);
    uint8_t bytes[32];
    for (int i = 0; i < 8; i++) {
        uint32_t w = words[i];
        bytes[i * 4]     = (uint8_t)((w >> 24) & 0xff);
        bytes[i * 4 + 1] = (uint8_t)((w >> 16) & 0xff);
        bytes[i * 4 + 2] = (uint8_t)((w >> 8) & 0xff);
        bytes[i * 4 + 3] = (uint8_t)(w & 0xff);
    }
    return n00b_buffer_from_bytes((char *)bytes, 32);
}

// -----------------------------------------------------------------------
// libzip glue: open an in-memory buffer for read.
// -----------------------------------------------------------------------

static struct zip_t *
open_zip_buffer(n00b_buffer_t *bytes, bool writable, zip_source_t **out_src)
{
    zip_error_t err = {0};
    zip_source_t *src = zip_source_buffer_create(
        bytes->data, (zip_uint64_t)bytes->byte_len,
        0 /* don't free the buffer; n00b GC owns it */,
        &err);
    if (!src) return nullptr;
    int flags = writable ? 0 : ZIP_RDONLY;
    struct zip_t *z = zip_open_from_source(src, flags, &err);
    if (!z) { zip_source_free(src); return nullptr; }
    if (out_src) *out_src = src;
    return z;
}

// Build a buffer of the zip's bytes by closing into a writable zip
// over a buffer source whose data we can fetch back.
static n00b_buffer_t *
zip_to_buffer(struct zip_t *z, zip_source_t *src)
{
    // Keep the source alive across close.
    zip_source_keep(src);
    if (zip_close(z) != 0) return nullptr;

    zip_source_open(src);
    zip_source_seek(src, 0, SEEK_END);
    zip_int64_t total = zip_source_tell(src);
    if (total < 0) return nullptr;
    zip_source_seek(src, 0, SEEK_SET);
    uint8_t *out = (uint8_t *)n00b_alloc_array(char, (size_t)total);
    zip_int64_t got = zip_source_read(src, out, (zip_uint64_t)total);
    zip_source_close(src);
    zip_source_free(src);
    if (got != total) return nullptr;
    return n00b_buffer_from_bytes((char *)out, (int64_t)total);
}

// -----------------------------------------------------------------------
// Codec entry points
// -----------------------------------------------------------------------

n00b_result_t(n00b_buffer_t *)
n00b_chalk_zip_hash_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_buffer_t *, 1);
    zip_source_t *src = nullptr;
    struct zip_t *z   = open_zip_buffer(bytes, false, &src);
    if (!z) return n00b_result_err(n00b_buffer_t *, 2);

    zip_entry_t *entries = nullptr;
    size_t       n       = 0;
    bool         dummy   = false;
    if (!read_entries(z, &entries, &n, &dummy)) {
        zip_close(z);
        return n00b_result_err(n00b_buffer_t *, 3);
    }
    n00b_buffer_t *h = hash_entries(entries, n);
    zip_close(z);
    zip_source_free(src);
    return n00b_result_ok(n00b_buffer_t *, h);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_zip_insert_buffer(n00b_buffer_t *bytes, n00b_chalk_mark_t *mark)
{
    if (!bytes || !mark) return n00b_result_err(n00b_chalk_io_result_t *, 1);

    // First pass: compute the unchalked hash from the input bytes.
    auto hr = n00b_chalk_zip_hash_buffer(bytes);
    if (n00b_result_is_err(hr)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 2);
    }
    n00b_buffer_t *hash_buf = n00b_result_get(hr);
    auto fin = n00b_chalk_mark_finalize(mark, hash_buf);
    if (n00b_result_is_err(fin)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 3);
    }
    n00b_buffer_t *encoded = n00b_result_get(fin);

    // Second pass: open the buffer writable, replace/add chalk.json.
    zip_source_t *src = nullptr;
    struct zip_t *z   = open_zip_buffer(bytes, true, &src);
    if (!z) return n00b_result_err(n00b_chalk_io_result_t *, 4);

    zip_int64_t idx = zip_name_locate(z, CHALK_JSON_NAME, 0);
    if (idx >= 0) zip_delete(z, (zip_uint64_t)idx);

    zip_source_t *mark_src = zip_source_buffer(z, encoded->data,
                                                (zip_uint64_t)encoded->byte_len,
                                                0);
    if (!mark_src) {
        zip_close(z);
        zip_source_free(src);
        return n00b_result_err(n00b_chalk_io_result_t *, 5);
    }
    if (zip_file_add(z, CHALK_JSON_NAME, mark_src,
                      ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8) < 0) {
        zip_source_free(mark_src);
        zip_close(z);
        zip_source_free(src);
        return n00b_result_err(n00b_chalk_io_result_t *, 6);
    }
    n00b_buffer_t *out = zip_to_buffer(z, src);
    if (!out) return n00b_result_err(n00b_chalk_io_result_t *, 7);

    auto r = (n00b_chalk_io_result_t *)n00b_alloc(n00b_chalk_io_result_t);
    r->kind           = N00B_CHALK_OUT_IN_BAND;
    r->bytes          = out;
    r->sidecar_suffix = nullptr;
    return n00b_result_ok(n00b_chalk_io_result_t *, r);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_zip_delete_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_chalk_io_result_t *, 1);
    zip_source_t *src = nullptr;
    struct zip_t *z   = open_zip_buffer(bytes, true, &src);
    if (!z) return n00b_result_err(n00b_chalk_io_result_t *, 2);
    zip_int64_t idx = zip_name_locate(z, CHALK_JSON_NAME, 0);
    if (idx >= 0) zip_delete(z, (zip_uint64_t)idx);
    n00b_buffer_t *out = zip_to_buffer(z, src);
    if (!out) return n00b_result_err(n00b_chalk_io_result_t *, 3);

    auto r = (n00b_chalk_io_result_t *)n00b_alloc(n00b_chalk_io_result_t);
    r->kind           = N00B_CHALK_OUT_IN_BAND;
    r->bytes          = out;
    r->sidecar_suffix = nullptr;
    return n00b_result_ok(n00b_chalk_io_result_t *, r);
}

n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_zip_extract_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_chalk_extract_result_t *, 1);
    zip_source_t *src = nullptr;
    struct zip_t *z   = open_zip_buffer(bytes, false, &src);
    if (!z) return n00b_result_err(n00b_chalk_extract_result_t *, 2);

    zip_int64_t idx = zip_name_locate(z, CHALK_JSON_NAME, 0);
    if (idx < 0) {
        zip_close(z);
        zip_source_free(src);
        return n00b_result_err(n00b_chalk_extract_result_t *, 3);
    }
    zip_stat_t st;
    if (zip_stat_index(z, (zip_uint64_t)idx, 0, &st) != 0
        || !(st.valid & ZIP_STAT_SIZE)) {
        zip_close(z);
        zip_source_free(src);
        return n00b_result_err(n00b_chalk_extract_result_t *, 4);
    }
    zip_file_t *zf = zip_fopen_index(z, (zip_uint64_t)idx, 0);
    if (!zf) {
        zip_close(z);
        zip_source_free(src);
        return n00b_result_err(n00b_chalk_extract_result_t *, 5);
    }
    uint8_t *buf = (uint8_t *)n00b_alloc_array(char, (size_t)st.size);
    zip_int64_t got = zip_fread(zf, buf, st.size);
    zip_fclose(zf);
    zip_close(z);
    zip_source_free(src);
    if (got < 0 || (zip_uint64_t)got != st.size) {
        return n00b_result_err(n00b_chalk_extract_result_t *, 6);
    }
    auto payload = n00b_buffer_from_bytes((char *)buf, (int64_t)got);
    return n00b_chalk_sidecar_parse_bytes(payload, N00B_CHALK_CODEC_ZIP);
}

#endif /* N00B_HAVE_LIBZIP */

// File-mode entries are always declared (the buffer versions either
// work or return EUNSUPPORTED based on N00B_HAVE_LIBZIP).
n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_zip_insert_file(n00b_string_t *path, n00b_chalk_mark_t *mark)
{
    return n00b_chalk_file_insert_via(path, mark,
                                      n00b_chalk_zip_insert_buffer);
}
n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_zip_delete_file(n00b_string_t *path)
{
    return n00b_chalk_file_delete_via(path, n00b_chalk_zip_delete_buffer);
}
n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_zip_extract_file(n00b_string_t *path)
{
    return n00b_chalk_file_extract_via(path, n00b_chalk_zip_extract_buffer);
}
n00b_result_t(n00b_buffer_t *)
n00b_chalk_zip_hash_file(n00b_string_t *path)
{
    return n00b_chalk_file_hash_via(path, n00b_chalk_zip_hash_buffer);
}
