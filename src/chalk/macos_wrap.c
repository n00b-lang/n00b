/** @file src/chalk/macos_wrap.c — bash-script Mach-O wrapper codec.
 *
 *  Ports chalk/plugins/codecMacOs.nim. Wraps a native Mach-O binary
 *  as a self-extracting bash script that decodes the embedded
 *  base64 blob to /tmp and execs it. The chalk mark lives as the
 *  last line of the script, with the SHA-256 of the unchalked
 *  Mach-O the line above.
 *
 *  Wrapped layout:
 *     <prefix>                   <- exact match
 *     <base64 blob>              <- single line, the Mach-O
 *     CHALK_DADFEDABBADABBEDBAD_END
 *     chmod +x ${CMDLOC}
 *     exec ${CMDLOC} ${@}
 *     <sha256 hex>               <- unchalked Mach-O sha256
 *     <chalk mark JSON>          <- single line
 */

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/sha256.h"
#include "core/alloc.h"
#include "chalk/n00b_chalk.h"
#include "internal/chalk/mark_internal.h"
#include "internal/chalk/sidecar_internal.h" // n00b_chalk_sha256_buffer

#include <string.h>

// -----------------------------------------------------------------------
// Wrapper template (verbatim from codecMacOs.nim:21-45)
// -----------------------------------------------------------------------

static const char k_prefix[] =
    "#!/bin/bash\n"
    "\n"
    "BASE_NAME=$(basename -- \"${BASH_SOURCE[0]}\")\n"
    "SCRIPT_DIR=$( cd -- \"$( dirname -- \"${BASH_SOURCE[0]}\" )\" && pwd )\n"
    "SCRIPT_PATH=$(echo ${SCRIPT_DIR})\n"
    "CMDDIR=$(echo ${SCRIPT_PATH} | sed s/-/_CHALKDA_/g)\n"
    "CMDDIR=$(echo ${CMDDIR} | sed s/\" \"/_CHALKSP_/g)\n"
    "CMDDIR=$(echo ${CMDDIR} | sed s#/#_CHALKSL_#g)\n"
    "CMDDIR=/tmp/${CMDDIR}\n"
    "\n"
    "if [[ ! -d ${CMDDIR} ]] ; then\n"
    "  mkdir ${CMDDIR}\n"
    "fi\n"
    "\n"
    "CMDLOC=${CMDDIR}/${BASE_NAME}\n"
    "\n"
    "if [[ -x ${CMDLOC} ]] ; then\n"
    "  HASH=$(/usr/bin/shasum --tag -a 256 ${CMDLOC} | cut -f 4 -d ' ')\n"
    "  if [[ $(grep ${HASH} ${SCRIPT_PATH}/${BASE_NAME}) ]]; then\n"
    "    exec ${CMDLOC} ${@}\n"
    "  fi\n"
    "fi\n"
    "(base64 -d)  < /bin/cat << CHALK_DADFEDABBADABBEDBAD_END > ${CMDLOC}\n";

static const char *k_postfix_lines[] = {
    "CHALK_DADFEDABBADABBEDBAD_END",
    "chmod +x ${CMDLOC}",
    "exec ${CMDLOC} ${@}",
};
#define K_POSTFIX_LEN (sizeof(k_postfix_lines) / sizeof(k_postfix_lines[0]))

// -----------------------------------------------------------------------
// Mach-O magic
// -----------------------------------------------------------------------

static bool
is_macho_magic(n00b_buffer_t *b)
{
    if (!b || b->byte_len < 4) return false;
    const uint8_t *d = (const uint8_t *)b->data;
    uint32_t m = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16)
               | ((uint32_t)d[2] << 8)  | (uint32_t)d[3];
    return m == 0xfeedface || m == 0xfeedfacf
        || m == 0xcefaedfe || m == 0xcffaedfe
        || m == 0xcafebabe || m == 0xbebafeca;
}

static bool
starts_with_prefix(n00b_buffer_t *b)
{
    size_t plen = sizeof(k_prefix) - 1;
    if (!b || b->byte_len < plen) return false;
    return memcmp(b->data, k_prefix, plen) == 0;
}

// -----------------------------------------------------------------------
// Base64 (RFC 4648, with '=' padding)
// -----------------------------------------------------------------------

static const char k_b64_alphabet[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

static n00b_string_t *
base64_encode_bytes(const uint8_t *data, size_t len)
{
    size_t triples = len / 3;
    size_t rem     = len - triples * 3;
    size_t outlen  = triples * 4 + (rem ? 4 : 0);
    char  *out     = n00b_alloc_array(char, outlen);
    size_t op      = 0;
    size_t ip      = 0;
    for (size_t i = 0; i < triples; i++) {
        uint32_t v = ((uint32_t)data[ip] << 16)
                   | ((uint32_t)data[ip + 1] << 8)
                   | (uint32_t)data[ip + 2];
        ip += 3;
        out[op++] = k_b64_alphabet[(v >> 18) & 0x3f];
        out[op++] = k_b64_alphabet[(v >> 12) & 0x3f];
        out[op++] = k_b64_alphabet[(v >> 6)  & 0x3f];
        out[op++] = k_b64_alphabet[v & 0x3f];
    }
    if (rem == 1) {
        uint32_t v = (uint32_t)data[ip] << 16;
        out[op++] = k_b64_alphabet[(v >> 18) & 0x3f];
        out[op++] = k_b64_alphabet[(v >> 12) & 0x3f];
        out[op++] = '=';
        out[op++] = '=';
    }
    else if (rem == 2) {
        uint32_t v = ((uint32_t)data[ip] << 16) | ((uint32_t)data[ip + 1] << 8);
        out[op++] = k_b64_alphabet[(v >> 18) & 0x3f];
        out[op++] = k_b64_alphabet[(v >> 12) & 0x3f];
        out[op++] = k_b64_alphabet[(v >> 6)  & 0x3f];
        out[op++] = '=';
    }
    return n00b_string_from_raw(out, (int64_t)outlen);
}

static int
b64_dec_char(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static n00b_buffer_t *
base64_decode(const char *src, size_t len)
{
    // Trim trailing whitespace.
    while (len > 0 && (src[len - 1] == '\n' || src[len - 1] == '\r'
                       || src[len - 1] == ' '  || src[len - 1] == '\t')) {
        len--;
    }
    size_t pad = 0;
    if (len > 0 && src[len - 1] == '=') pad++;
    if (len > 1 && src[len - 2] == '=') pad++;
    size_t out_len = (len / 4) * 3 - pad;
    uint8_t *out = (uint8_t *)n00b_alloc_array(char, out_len);
    size_t op = 0;
    for (size_t i = 0; i < len; i += 4) {
        int a = b64_dec_char(src[i]);
        int b = b64_dec_char(src[i + 1]);
        int c = src[i + 2] == '=' ? 0 : b64_dec_char(src[i + 2]);
        int d = src[i + 3] == '=' ? 0 : b64_dec_char(src[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return nullptr;
        uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12)
                   | ((uint32_t)c << 6)  | (uint32_t)d;
        if (op < out_len) out[op++] = (uint8_t)((v >> 16) & 0xff);
        if (op < out_len) out[op++] = (uint8_t)((v >> 8) & 0xff);
        if (op < out_len) out[op++] = (uint8_t)(v & 0xff);
    }
    return n00b_buffer_from_bytes((char *)out, (int64_t)out_len);
}

// -----------------------------------------------------------------------
// Wrapper parsing
// -----------------------------------------------------------------------

typedef struct {
    n00b_buffer_t *macho;          // decoded mach-o bytes
    const char    *mark;           // pointer into wrapper bytes
    size_t         mark_len;
    bool           valid;
} parsed_wrap_t;

static parsed_wrap_t
parse_wrapped(n00b_buffer_t *b)
{
    parsed_wrap_t r = { .valid = false };
    size_t plen = sizeof(k_prefix) - 1;
    if (!starts_with_prefix(b)) return r;

    // Body is everything after the prefix, with trailing whitespace
    // stripped.
    const char *body  = b->data + plen;
    size_t      blen  = b->byte_len - plen;
    while (blen > 0 && (body[blen - 1] == '\n' || body[blen - 1] == '\r'
                        || body[blen - 1] == ' '  || body[blen - 1] == '\t')) {
        blen--;
    }

    // Split into newline-terminated lines.
    const char *lines[16];
    size_t      lens[16];
    size_t      n = 0;
    size_t      start = 0;
    for (size_t i = 0; i <= blen; i++) {
        if (i == blen || body[i] == '\n') {
            if (n == 16) return r;
            lines[n] = body + start;
            lens[n]  = i - start;
            n++;
            start = i + 1;
        }
    }
    // Chalk expects 3 + len(postfix_lines) lines exactly.
    if (n != 3 + K_POSTFIX_LEN) return r;

    // Postfix lines 1..K_POSTFIX_LEN
    for (size_t i = 0; i < K_POSTFIX_LEN; i++) {
        size_t pll = strlen(k_postfix_lines[i]);
        if (lens[1 + i] != pll
            || memcmp(lines[1 + i], k_postfix_lines[i], pll) != 0) {
            return r;
        }
    }

    // Line 0 = base64 blob.
    auto macho = base64_decode(lines[0], lens[0]);
    if (!macho) return r;
    r.macho = macho;

    // Last line = chalk mark.
    r.mark     = lines[n - 1];
    r.mark_len = lens[n - 1];
    r.valid    = true;
    return r;
}

// -----------------------------------------------------------------------
// Build a wrapped script: prefix + base64(macho) + postfix lines +
// hash hex + mark JSON.
// -----------------------------------------------------------------------

static n00b_buffer_t *
build_wrapper(n00b_buffer_t *macho_bytes,
              n00b_string_t *hash_hex,
              n00b_buffer_t *encoded_mark)
{
    auto    b64    = base64_encode_bytes((const uint8_t *)macho_bytes->data,
                                         macho_bytes->byte_len);
    size_t  prefix_len = sizeof(k_prefix) - 1;
    size_t  postfix_total = 0;
    for (size_t i = 0; i < K_POSTFIX_LEN; i++) {
        postfix_total += strlen(k_postfix_lines[i]) + 1; // + '\n'
    }
    size_t total = prefix_len
                 + b64->u8_bytes + 1
                 + postfix_total
                 + hash_hex->u8_bytes + 1
                 + encoded_mark->byte_len + 1;
    char *out = n00b_alloc_array(char, total);
    size_t op = 0;
    memcpy(out + op, k_prefix, prefix_len);
    op += prefix_len;
    memcpy(out + op, b64->data, b64->u8_bytes);
    op += b64->u8_bytes;
    out[op++] = '\n';
    for (size_t i = 0; i < K_POSTFIX_LEN; i++) {
        size_t pll = strlen(k_postfix_lines[i]);
        memcpy(out + op, k_postfix_lines[i], pll);
        op += pll;
        out[op++] = '\n';
    }
    memcpy(out + op, hash_hex->data, hash_hex->u8_bytes);
    op += hash_hex->u8_bytes;
    out[op++] = '\n';
    memcpy(out + op, encoded_mark->data, encoded_mark->byte_len);
    op += encoded_mark->byte_len;
    out[op++] = '\n';
    return n00b_buffer_from_bytes(out, (int64_t)op);
}

static const char k_hex[16] = "0123456789abcdef";

static n00b_string_t *
sha256_hex_of(n00b_buffer_t *in)
{
    auto digest = n00b_chalk_sha256_buffer(in);
    char hex[64];
    for (int i = 0; i < 32; i++) {
        uint8_t v = (uint8_t)digest->data[i];
        hex[i * 2]     = k_hex[(v >> 4) & 0xf];
        hex[i * 2 + 1] = k_hex[v & 0xf];
    }
    return n00b_string_from_raw(hex, 64);
}

// -----------------------------------------------------------------------
// Codec entry points
// -----------------------------------------------------------------------

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_macos_wrap_insert_buffer(n00b_buffer_t *bytes,
                                    n00b_chalk_mark_t *mark)
{
    if (!bytes || !mark) {
        return n00b_result_err(n00b_chalk_io_result_t *, 1);
    }

    n00b_buffer_t *macho;
    if (starts_with_prefix(bytes)) {
        // Already wrapped — re-mark the existing binary.
        auto parsed = parse_wrapped(bytes);
        if (!parsed.valid) {
            return n00b_result_err(n00b_chalk_io_result_t *, 2);
        }
        macho = parsed.macho;
    }
    else if (is_macho_magic(bytes)) {
        macho = bytes;
    }
    else {
        return n00b_result_err(n00b_chalk_io_result_t *, 3);
    }

    auto hash_buf = n00b_chalk_sha256_buffer(macho);
    auto fin      = n00b_chalk_mark_finalize(mark, hash_buf);
    if (n00b_result_is_err(fin)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 4);
    }
    n00b_buffer_t *encoded_mark = n00b_result_get(fin);
    n00b_string_t *hash_hex     = sha256_hex_of(macho);
    n00b_buffer_t *wrapped      = build_wrapper(macho, hash_hex, encoded_mark);

    auto r = (n00b_chalk_io_result_t *)n00b_alloc(n00b_chalk_io_result_t);
    r->kind           = N00B_CHALK_OUT_IN_BAND;
    r->bytes          = wrapped;
    r->sidecar_suffix = nullptr;
    return n00b_result_ok(n00b_chalk_io_result_t *, r);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_macos_wrap_delete_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_chalk_io_result_t *, 1);
    if (!starts_with_prefix(bytes)) {
        // Already un-wrapped — nothing to do.
        auto r = (n00b_chalk_io_result_t *)
                     n00b_alloc(n00b_chalk_io_result_t);
        r->kind           = N00B_CHALK_OUT_IN_BAND;
        r->bytes          = bytes;
        r->sidecar_suffix = nullptr;
        return n00b_result_ok(n00b_chalk_io_result_t *, r);
    }
    auto parsed = parse_wrapped(bytes);
    if (!parsed.valid) {
        return n00b_result_err(n00b_chalk_io_result_t *, 2);
    }
    auto r = (n00b_chalk_io_result_t *)n00b_alloc(n00b_chalk_io_result_t);
    r->kind           = N00B_CHALK_OUT_IN_BAND;
    r->bytes          = parsed.macho;
    r->sidecar_suffix = nullptr;
    return n00b_result_ok(n00b_chalk_io_result_t *, r);
}

n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_macos_wrap_extract_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_chalk_extract_result_t *, 1);
    auto parsed = parse_wrapped(bytes);
    if (!parsed.valid) {
        return n00b_result_err(n00b_chalk_extract_result_t *, 2);
    }
    auto mark_buf = n00b_buffer_from_bytes((char *)parsed.mark,
                                            (int64_t)parsed.mark_len);
    return n00b_chalk_sidecar_parse_bytes(mark_buf,
                                          N00B_CHALK_CODEC_MACOS_WRAP);
}

n00b_result_t(n00b_buffer_t *)
n00b_chalk_macos_wrap_hash_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_buffer_t *, 1);
    if (starts_with_prefix(bytes)) {
        auto parsed = parse_wrapped(bytes);
        if (!parsed.valid) {
            return n00b_result_err(n00b_buffer_t *, 2);
        }
        return n00b_result_ok(n00b_buffer_t *,
                              n00b_chalk_sha256_buffer(parsed.macho));
    }
    if (is_macho_magic(bytes)) {
        return n00b_result_ok(n00b_buffer_t *,
                              n00b_chalk_sha256_buffer(bytes));
    }
    return n00b_result_err(n00b_buffer_t *, 3);
}

// File-mode stubs (deferred with the rest of the file-API plumbing).
n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_macos_wrap_insert_file(n00b_string_t *path, n00b_chalk_mark_t *mark)
{
    (void)path;
    (void)mark;
    return n00b_result_err(n00b_chalk_io_result_t *, 1);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_macos_wrap_delete_file(n00b_string_t *path)
{
    (void)path;
    return n00b_result_err(n00b_chalk_io_result_t *, 1);
}

n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_macos_wrap_extract_file(n00b_string_t *path)
{
    (void)path;
    return n00b_result_err(n00b_chalk_extract_result_t *, 1);
}

n00b_result_t(n00b_buffer_t *)
n00b_chalk_macos_wrap_hash_file(n00b_string_t *path)
{
    (void)path;
    return n00b_result_err(n00b_buffer_t *, 1);
}
