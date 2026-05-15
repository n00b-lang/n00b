/** @file src/chalk/codec_table.c — codec dispatcher.
 *
 *  Resolves a codec from raw bytes (magic-byte detection) and/or a
 *  file-path hint (extension matching), then forwards to the matched
 *  codec's four entry points. Codecs that aren't yet implemented have
 *  null callback slots — the public dispatcher returns
 *  `n00b_result_err(EUNSUPPORTED)` for missing slots. */

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "chalk/n00b_chalk.h"
#include "internal/chalk/codec_table.h"

#include <string.h>

// -----------------------------------------------------------------------
// Magic-byte + extension helpers
// -----------------------------------------------------------------------

static bool
has_extension(n00b_string_t *path, const char *ext)
{
    if (!path || !path->data) return false;
    size_t plen = path->u8_bytes;
    size_t elen = strlen(ext);
    if (plen < elen) return false;
    return memcmp(path->data + plen - elen, ext, elen) == 0;
}

static bool
starts_with(const uint8_t *bytes, size_t blen,
            const uint8_t *prefix, size_t plen)
{
    if (blen < plen) return false;
    return memcmp(bytes, prefix, plen) == 0;
}

// -----------------------------------------------------------------------
// Codec dispatch table.
//
// One row per codec. Unimplemented codecs have nullptr slots; the
// public dispatcher converts those to CHALK_EUNSUPPORTED at call time.
//
// As each codec lands, its row gets filled in.
// -----------------------------------------------------------------------

const n00b_chalk_codec_entry_t n00b_chalk_codec_table[] = {
    {
        .codec          = N00B_CHALK_CODEC_PYC,
        .insert_buffer  = n00b_chalk_pyc_insert_buffer,
        .delete_buffer  = n00b_chalk_pyc_delete_buffer,
        .extract_buffer = n00b_chalk_pyc_extract_buffer,
        .hash_buffer    = n00b_chalk_pyc_hash_buffer,
    },
    { .codec = N00B_CHALK_CODEC_NONE },
};

const size_t n00b_chalk_codec_table_len
    = sizeof(n00b_chalk_codec_table) / sizeof(n00b_chalk_codec_table[0]) - 1;

const n00b_chalk_codec_entry_t *
n00b_chalk_codec_entry(n00b_chalk_codec_id_t codec)
{
    for (size_t i = 0; i < n00b_chalk_codec_table_len; i++) {
        if (n00b_chalk_codec_table[i].codec == codec) {
            return &n00b_chalk_codec_table[i];
        }
    }
    return nullptr;
}

// -----------------------------------------------------------------------
// Detection
// -----------------------------------------------------------------------

n00b_chalk_codec_id_t
n00b_chalk_codec_detect(n00b_buffer_t *bytes, n00b_string_t *hint_path)
{
    if (hint_path) {
        if (has_extension(hint_path, ".pyc")
            || has_extension(hint_path, ".pyo")
            || has_extension(hint_path, ".pyd")) {
            return N00B_CHALK_CODEC_PYC;
        }
        if (has_extension(hint_path, ".onnx")
            || has_extension(hint_path, ".bin")
            || has_extension(hint_path, ".pt")
            || has_extension(hint_path, ".pth")
            || has_extension(hint_path, ".keras")) {
            return N00B_CHALK_CODEC_SIDECAR_MODEL;
        }
        if (has_extension(hint_path, ".pem")
            || has_extension(hint_path, ".crt")
            || has_extension(hint_path, ".cer")
            || has_extension(hint_path, ".der")) {
            return N00B_CHALK_CODEC_SIDECAR_CERT;
        }
        if (has_extension(hint_path, ".zip")
            || has_extension(hint_path, ".jar")
            || has_extension(hint_path, ".war")
            || has_extension(hint_path, ".ear")) {
            return N00B_CHALK_CODEC_ZIP;
        }
        if (has_extension(hint_path, ".gguf")) {
            return N00B_CHALK_CODEC_GGUF;
        }
        if (has_extension(hint_path, ".safetensors")) {
            return N00B_CHALK_CODEC_SAFETENSORS;
        }
    }

    if (!bytes || bytes->byte_len < 4) {
        return N00B_CHALK_CODEC_NONE;
    }
    const uint8_t *b = (const uint8_t *)bytes->data;
    size_t         n = bytes->byte_len;

    if (starts_with(b, n, (const uint8_t *)"\x7f""ELF", 4)) {
        return N00B_CHALK_CODEC_ELF;
    }
    if (starts_with(b, n, (const uint8_t *)"GGUF", 4)) {
        return N00B_CHALK_CODEC_GGUF;
    }
    if (starts_with(b, n, (const uint8_t *)"PK\x03\x04", 4)) {
        return N00B_CHALK_CODEC_ZIP;
    }
    if (n >= 4) {
        uint32_t m = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16)
                   | ((uint32_t)b[2] << 8)  | (uint32_t)b[3];
        if (m == 0xfeedface || m == 0xfeedfacf || m == 0xcefaedfe
            || m == 0xcffaedfe || m == 0xcafebabe || m == 0xbebafeca) {
            return N00B_CHALK_CODEC_MACHO;
        }
    }
    if (starts_with(b, n,
                    (const uint8_t *)"-----BEGIN CERTIFICATE-----", 27)) {
        return N00B_CHALK_CODEC_SIDECAR_CERT;
    }
    if (starts_with(b, n, (const uint8_t *)"#!", 2)) {
        return N00B_CHALK_CODEC_SOURCE;
    }
    return N00B_CHALK_CODEC_NONE;
}

// -----------------------------------------------------------------------
// Public dispatcher entry points
// -----------------------------------------------------------------------

n00b_chalk_codec_id_t
n00b_chalk_detect_buffer(n00b_buffer_t *bytes, n00b_string_t *hint_path)
{
    return n00b_chalk_codec_detect(bytes, hint_path);
}

n00b_chalk_codec_id_t
n00b_chalk_detect_file(n00b_string_t *path)
{
    return n00b_chalk_codec_detect(nullptr, path);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_insert_buffer(n00b_buffer_t *bytes, n00b_chalk_mark_t *mark)
{
    n00b_chalk_codec_id_t           id = n00b_chalk_codec_detect(bytes, nullptr);
    const n00b_chalk_codec_entry_t *e  = n00b_chalk_codec_entry(id);
    if (!e || !e->insert_buffer) {
        return n00b_result_err(n00b_chalk_io_result_t *, 1);
    }
    return e->insert_buffer(bytes, mark);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_delete_buffer(n00b_buffer_t *bytes)
{
    n00b_chalk_codec_id_t           id = n00b_chalk_codec_detect(bytes, nullptr);
    const n00b_chalk_codec_entry_t *e  = n00b_chalk_codec_entry(id);
    if (!e || !e->delete_buffer) {
        return n00b_result_err(n00b_chalk_io_result_t *, 1);
    }
    return e->delete_buffer(bytes);
}

n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_extract_buffer(n00b_buffer_t *bytes)
{
    n00b_chalk_codec_id_t           id = n00b_chalk_codec_detect(bytes, nullptr);
    const n00b_chalk_codec_entry_t *e  = n00b_chalk_codec_entry(id);
    if (!e || !e->extract_buffer) {
        return n00b_result_err(n00b_chalk_extract_result_t *, 1);
    }
    return e->extract_buffer(bytes);
}

n00b_result_t(n00b_buffer_t *)
n00b_chalk_hash_buffer(n00b_buffer_t *bytes)
{
    n00b_chalk_codec_id_t           id = n00b_chalk_codec_detect(bytes, nullptr);
    const n00b_chalk_codec_entry_t *e  = n00b_chalk_codec_entry(id);
    if (!e || !e->hash_buffer) {
        return n00b_result_err(n00b_buffer_t *, 1);
    }
    return e->hash_buffer(bytes);
}

// File-mode entry points: stubbed until per-codec file adapters land.
// They need n00b's file API plumbing which the upcoming codec commits
// will integrate.
n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_insert_file(n00b_string_t *path, n00b_chalk_mark_t *mark)
{
    (void)path;
    (void)mark;
    return n00b_result_err(n00b_chalk_io_result_t *, 1);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_delete_file(n00b_string_t *path)
{
    (void)path;
    return n00b_result_err(n00b_chalk_io_result_t *, 1);
}

n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_extract_file(n00b_string_t *path)
{
    (void)path;
    return n00b_result_err(n00b_chalk_extract_result_t *, 1);
}

n00b_result_t(n00b_buffer_t *)
n00b_chalk_hash_file(n00b_string_t *path)
{
    (void)path;
    return n00b_result_err(n00b_buffer_t *, 1);
}

n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_extract_sidecar_buffer(n00b_buffer_t *sidecar_bytes)
{
    (void)sidecar_bytes;
    return n00b_result_err(n00b_chalk_extract_result_t *, 1);
}
