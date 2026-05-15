/** @file src/chalk/elf_fallback.c — Fallback ELF codec.
 *
 *  Hex-offset scan only — no structural parsing. Used when the
 *  primary ELF codec can't make sense of an ELF variant. Supports
 *  extract + hash; insert/delete return EUNSUPPORTED.
 *
 *  Mirrors chalk's codecFallbackElf.nim. */

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/alloc.h"
#include "chalk/n00b_chalk.h"
#include "internal/chalk/mark_internal.h"
#include "internal/chalk/sidecar_internal.h"
#include "internal/chalk/file_io.h"

#include <string.h>

#define ELF_MAGIC     "\x7f""ELF"
#define ELF_MAGIC_LEN 4

static int64_t
find_magic(const uint8_t *data, size_t len)
{
    static const char magic[] = N00B_CHALK_MAGIC_STRING;
    size_t mlen = sizeof(magic) - 1;
    if (len < mlen) return -1;
    for (size_t i = 0; i + mlen <= len; i++) {
        if (memcmp(data + i, magic, mlen) == 0) return (int64_t)i;
    }
    return -1;
}

n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_elf_fallback_extract_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_chalk_extract_result_t *, 1);
    if (bytes->byte_len < 4
        || memcmp(bytes->data, ELF_MAGIC, ELF_MAGIC_LEN) != 0) {
        return n00b_result_err(n00b_chalk_extract_result_t *, 2);
    }
    int64_t magic = find_magic((const uint8_t *)bytes->data, bytes->byte_len);
    if (magic < 0) return n00b_result_err(n00b_chalk_extract_result_t *, 3);
    int64_t bs = -1;
    for (int64_t i = magic; i >= 0; i--) {
        if (bytes->data[i] == '{') { bs = i; break; }
    }
    if (bs < 0) return n00b_result_err(n00b_chalk_extract_result_t *, 4);
    int  depth  = 0;
    bool in_str = false;
    bool escape = false;
    int64_t end = -1;
    for (size_t i = (size_t)bs; i < bytes->byte_len; i++) {
        char c = bytes->data[i];
        if (in_str) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) { end = (int64_t)(i + 1); break; }
        }
    }
    if (end < 0) return n00b_result_err(n00b_chalk_extract_result_t *, 5);
    auto payload = n00b_buffer_from_bytes(bytes->data + bs,
                                           (int64_t)(end - bs));
    return n00b_chalk_sidecar_parse_bytes(payload,
                                          N00B_CHALK_CODEC_ELF_FALLBACK);
}

n00b_result_t(n00b_buffer_t *)
n00b_chalk_elf_fallback_hash_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_buffer_t *, 1);
    return n00b_result_ok(n00b_buffer_t *, n00b_chalk_sha256_buffer(bytes));
}

n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_elf_fallback_extract_file(n00b_string_t *path)
{
    return n00b_chalk_file_extract_via(path,
                                       n00b_chalk_elf_fallback_extract_buffer);
}
n00b_result_t(n00b_buffer_t *)
n00b_chalk_elf_fallback_hash_file(n00b_string_t *path)
{
    // Plain SHA-256 of the file — fallback has no canonical form.
    return n00b_chalk_hash_file_stream(path);
}
