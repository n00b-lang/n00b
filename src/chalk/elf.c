/** @file src/chalk/elf.c — ELF codec (primary + fallback).
 *
 *  Ports chalk/plugins/elf.nim and codecElf.nim. ELF64 little-endian
 *  only — chalk's primary codec refuses other variants and the
 *  fallback codec covers the long tail with a header-hex-offset scan
 *  for the chalk magic.
 *
 *  Layout that the codec maintains for chalked ELFs:
 *
 *      [ELF header (64 bytes)]
 *      [program-header table  ...]
 *      [...other sections...]
 *      [.chalk.mark section bytes] (or .chalk.free)
 *      [section-name string table]
 *      [section header table]   <-- header.e_shoff
 *
 *  insertChalkSection grows the file at EOF, moving the section
 *  string table and the section header table along the way. The
 *  same shape is preserved on remark / unchalk so the unchalked
 *  hash remains stable.
 *
 *  Hash convention: replace chalk section contents with 32 zero
 *  bytes, sha256 the resulting file. That hash is what
 *  getUnchalkedHash returns AND what .chalk.free stores so chalk's
 *  re-validation works.
 */

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/alloc.h"
#include "core/sha256.h"
#include "chalk/n00b_chalk.h"
#include "internal/chalk/mark_internal.h"
#include "internal/chalk/sidecar_internal.h"
#include "internal/chalk/file_io.h"

#include <string.h>

// -----------------------------------------------------------------------
// ELF64 constants (mirrors chalk/src/plugins/elf.nim verbatim)
// -----------------------------------------------------------------------

#define ELF_MAGIC                  "\x7f""ELF"
#define ELF_MAGIC_LEN              4
#define ELF_CLASS_OFFSET           0x04
#define ELF_ENDIAN_OFFSET          0x05
#define ELF_VERSION_OFFSET         0x14
#define ELF_CLASS_ELF64            0x02
#define ELF_LITTLE_ENDIAN          0x01
#define ELF_VERSION1               0x00000001
#define ELF64_HEADER_SIZE          0x40

#define ELF64_TYPE_OFFSET          0x10
#define ELF64_PH_TABLE_OFFSET      0x20
#define ELF64_SH_TABLE_OFFSET      0x28
#define ELF64_PH_SIZE_OFFSET       0x36
#define ELF64_PH_COUNT_OFFSET      0x38
#define ELF64_SH_SIZE_OFFSET       0x3A
#define ELF64_SH_COUNT_OFFSET      0x3C
#define ELF64_SH_STRIDX_OFFSET     0x3E

#define ELF64_SECTION_HEADER_SIZE  0x40
#define ELF64_SECTION_NAME_32      0x00
#define ELF64_SECTION_TYPE_32      0x04
#define ELF64_SECTION_FLAGS_64     0x08
#define ELF64_SECTION_ADDR_64      0x10
#define ELF64_SECTION_OFFSET_64    0x18
#define ELF64_SECTION_SIZE_64      0x20

#define SHT_PROGBITS               1
#define SHT_NOBITS                 8
#define SHN_LORESERVE              0xff00u

#define SH_NAME_CHALKMARK          ".chalk.mark"
#define SH_NAME_CHALKFREE          ".chalk.free"
#define SHA256_BYTE_LENGTH         32

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

static uint8_t  rd8 (const uint8_t *p)  { return p[0]; }
static uint16_t rd16(const uint8_t *p)  { return (uint16_t)p[0]
                                              | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;       p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr64(uint8_t *p, uint64_t v)
{
    wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32));
}
static size_t pad8(size_t off)
{
    return (8 - (off & 7)) & 7;
}

// -----------------------------------------------------------------------
// Parsed ELF state
// -----------------------------------------------------------------------

typedef struct {
    uint64_t name_index;   // offset into strtab
    uint32_t type;
    uint64_t offset;
    uint64_t size;
    const char *name;       // resolved (points into strtab inside buffer)
} elf_section_t;

typedef struct {
    uint8_t  *data;
    size_t    len;
    size_t    cap;
    uint64_t  ph_off, ph_size, ph_count;
    uint64_t  sh_off;
    uint16_t  sh_size, sh_count;
    uint16_t  sh_stridx;
    elf_section_t *sections;
    size_t    section_count;
    int       chalk_idx;     // index into sections[], -1 if absent
    bool      has_been_unchalked;
} elf_t;

// -----------------------------------------------------------------------
// Parsing
// -----------------------------------------------------------------------

static bool
elf_parse(elf_t *e)
{
    if (e->len < ELF64_HEADER_SIZE) return false;
    if (memcmp(e->data, ELF_MAGIC, ELF_MAGIC_LEN) != 0) return false;
    if (rd8(e->data + ELF_CLASS_OFFSET)  != ELF_CLASS_ELF64)   return false;
    if (rd8(e->data + ELF_ENDIAN_OFFSET) != ELF_LITTLE_ENDIAN) return false;
    if (rd32(e->data + ELF_VERSION_OFFSET) != ELF_VERSION1)    return false;

    e->ph_off    = rd64(e->data + ELF64_PH_TABLE_OFFSET);
    e->ph_size   = rd16(e->data + ELF64_PH_SIZE_OFFSET);
    e->ph_count  = rd16(e->data + ELF64_PH_COUNT_OFFSET);
    e->sh_off    = rd64(e->data + ELF64_SH_TABLE_OFFSET);
    e->sh_size   = rd16(e->data + ELF64_SH_SIZE_OFFSET);
    e->sh_count  = rd16(e->data + ELF64_SH_COUNT_OFFSET);
    e->sh_stridx = rd16(e->data + ELF64_SH_STRIDX_OFFSET);

    if (e->sh_size != ELF64_SECTION_HEADER_SIZE) return false;
    if (e->sh_count == 0) return false;
    if (e->sh_stridx >= e->sh_count) return false;
    if (e->sh_stridx >= SHN_LORESERVE) return false;

    size_t table_bytes = (size_t)e->sh_size * (size_t)e->sh_count;
    if (e->sh_off + table_bytes > e->len) return false;

    e->sections      = n00b_alloc_array(elf_section_t, e->sh_count);
    e->section_count = e->sh_count;
    for (size_t i = 0; i < e->section_count; i++) {
        const uint8_t *sh = e->data + e->sh_off + i * e->sh_size;
        e->sections[i].name_index = rd32(sh + ELF64_SECTION_NAME_32);
        e->sections[i].type       = rd32(sh + ELF64_SECTION_TYPE_32);
        e->sections[i].offset     = rd64(sh + ELF64_SECTION_OFFSET_64);
        e->sections[i].size       = rd64(sh + ELF64_SECTION_SIZE_64);
        e->sections[i].name       = nullptr;
    }

    elf_section_t *str = &e->sections[e->sh_stridx];
    if (str->type == SHT_NOBITS) return false;
    if (str->offset + str->size > e->len) return false;
    const char *strtab = (const char *)e->data + str->offset;
    size_t      strlen_max = str->size;

    e->chalk_idx          = -1;
    e->has_been_unchalked = false;
    for (size_t i = 0; i < e->section_count; i++) {
        uint64_t ni = e->sections[i].name_index;
        if (ni >= strlen_max) return false;
        e->sections[i].name = strtab + ni;
        if (e->sections[i].type != SHT_PROGBITS) continue;
        if (strcmp(e->sections[i].name, SH_NAME_CHALKMARK) == 0) {
            e->chalk_idx = (int)i;
        }
        else if (strcmp(e->sections[i].name, SH_NAME_CHALKFREE) == 0) {
            e->chalk_idx          = (int)i;
            e->has_been_unchalked = true;
        }
    }
    return true;
}

static elf_t *
elf_new(n00b_buffer_t *bytes)
{
    elf_t *e = n00b_alloc(elf_t);
    // Take an owning copy so mutation is safe.
    e->cap  = bytes->byte_len > 64 ? bytes->byte_len * 2 : 128;
    e->data = (uint8_t *)n00b_alloc_array(char, e->cap);
    memcpy(e->data, bytes->data, bytes->byte_len);
    e->len = bytes->byte_len;
    if (!elf_parse(e)) return nullptr;
    return e;
}

static void
elf_ensure_cap(elf_t *e, size_t need)
{
    if (need <= e->cap) return;
    size_t new_cap = e->cap;
    while (new_cap < need) new_cap *= 2;
    uint8_t *grown = (uint8_t *)n00b_alloc_array(char, new_cap);
    memcpy(grown, e->data, e->len);
    e->data = grown;
    e->cap  = new_cap;
}

// -----------------------------------------------------------------------
// Insert or replace the chalk section.
//
// The chalk codec writes everything past the original chalk-section
// offset (or past the original strtab/sh-table when inserting fresh).
// We follow the same layout: strtab and sh-table sit at the end, the
// chalk section sits just before them.
// -----------------------------------------------------------------------

static bool
elf_insert_or_set(elf_t *e, const char *name, const uint8_t *data,
                  size_t data_len)
{
    size_t name_len = strlen(name);

    if (e->chalk_idx >= 0) {
        // Replace path: assume the chalk section is sandwiched between
        // file body and the strtab, and the section header table is at
        // EOF. Rewrite: chalk-section, padding, strtab, padding, sh-table.
        elf_section_t *ch  = &e->sections[e->chalk_idx];
        elf_section_t *str = &e->sections[e->sh_stridx];

        // Update name bytes in strtab (in place — names have equal length
        // because both ".chalk.mark" and ".chalk.free" are 11 chars).
        if (strlen(ch->name) != name_len) return false;
        memcpy(e->data + str->offset + ch->name_index, name, name_len);

        size_t chalk_off = ch->offset;
        size_t out_off   = chalk_off + data_len;
        size_t pad1      = pad8(out_off);
        size_t str_off   = out_off + pad1;
        size_t str_end   = str_off + str->size;
        size_t pad2      = pad8(str_end);
        size_t sh_off    = str_end + pad2;
        size_t sh_end    = sh_off + (size_t)e->sh_size * e->sh_count;

        elf_ensure_cap(e, sh_end);

        // Read the strtab + sh-table into a temp before writing.
        uint8_t *tmp_str  = n00b_alloc_array(uint8_t, str->size);
        memcpy(tmp_str, e->data + str->offset, str->size);
        size_t   sht_size = (size_t)e->sh_size * e->sh_count;
        uint8_t *tmp_sht  = n00b_alloc_array(uint8_t, sht_size);
        memcpy(tmp_sht, e->data + e->sh_off, sht_size);

        // Update chalk section size + data.
        wr64(e->data + e->sh_off + (size_t)e->chalk_idx * e->sh_size
             + ELF64_SECTION_SIZE_64, data_len);
        memcpy(e->data + chalk_off, data, data_len);
        memset(e->data + chalk_off + data_len, 0, pad1);

        // Update strtab offset in its section header.
        wr64(e->data + e->sh_off + (size_t)e->sh_stridx * e->sh_size
             + ELF64_SECTION_OFFSET_64, str_off);
        memcpy(e->data + str_off, tmp_str, str->size);
        memset(e->data + str_off + str->size, 0, pad2);

        // Move sh-table.
        wr64(e->data + ELF64_SH_TABLE_OFFSET, sh_off);
        memcpy(e->data + sh_off, tmp_sht, sht_size);
        // Fix the chalk section's section-header offset.
        wr64(e->data + sh_off + (size_t)e->chalk_idx * e->sh_size
             + ELF64_SECTION_OFFSET_64, chalk_off);
        wr64(e->data + sh_off + (size_t)e->sh_stridx * e->sh_size
             + ELF64_SECTION_OFFSET_64, str_off);

        e->len = sh_end;
        return elf_parse(e);
    }

    // Insert path: append at EOF.
    if (e->sh_count + 1u >= SHN_LORESERVE) return false;
    elf_section_t *str = &e->sections[e->sh_stridx];

    size_t  truncate_off = e->len;
    size_t  pad0         = pad8(truncate_off);
    size_t  chalk_off    = truncate_off + pad0;
    size_t  out_off      = chalk_off + data_len;
    size_t  pad1         = pad8(out_off);
    size_t  new_str_off  = out_off + pad1;
    size_t  new_str_size = str->size + name_len + 1;  // +1 for NUL
    size_t  str_end      = new_str_off + new_str_size;
    size_t  pad2         = pad8(str_end);
    size_t  new_sh_off   = str_end + pad2;
    size_t  new_sh_count = (size_t)e->sh_count + 1;
    size_t  new_sht_size = new_sh_count * e->sh_size;
    size_t  new_total    = new_sh_off + new_sht_size;

    elf_ensure_cap(e, new_total);

    // Build new strtab (old + name + NUL).
    uint8_t *new_strtab = n00b_alloc_array(uint8_t, new_str_size);
    memcpy(new_strtab, e->data + str->offset, str->size);
    memcpy(new_strtab + str->size, name, name_len);
    new_strtab[str->size + name_len] = '\0';
    uint32_t new_name_index = (uint32_t)str->size;

    // Snapshot original section header table.
    size_t   old_sht_size = (size_t)e->sh_size * e->sh_count;
    uint8_t *old_sht      = n00b_alloc_array(uint8_t, old_sht_size);
    memcpy(old_sht, e->data + e->sh_off, old_sht_size);

    // Zero pad0 in place (no-op visually; harmless if cap was bigger).
    memset(e->data + truncate_off, 0, pad0);

    // Write chalk section bytes.
    memcpy(e->data + chalk_off, data, data_len);
    memset(e->data + chalk_off + data_len, 0, pad1);

    // Write new strtab and its trailing pad.
    memcpy(e->data + new_str_off, new_strtab, new_str_size);
    memset(e->data + new_str_off + new_str_size, 0, pad2);

    // Build new section header table: old entries + a new one for chalk.
    // Update strtab's section header (offset + size) before copying.
    wr64(old_sht + (size_t)e->sh_stridx * e->sh_size + ELF64_SECTION_OFFSET_64,
         new_str_off);
    wr64(old_sht + (size_t)e->sh_stridx * e->sh_size + ELF64_SECTION_SIZE_64,
         new_str_size);

    memcpy(e->data + new_sh_off, old_sht, old_sht_size);
    uint8_t *new_entry = e->data + new_sh_off + old_sht_size;
    memset(new_entry, 0, e->sh_size);
    wr32(new_entry + ELF64_SECTION_NAME_32, new_name_index);
    wr32(new_entry + ELF64_SECTION_TYPE_32, SHT_PROGBITS);
    wr64(new_entry + ELF64_SECTION_OFFSET_64, chalk_off);
    wr64(new_entry + ELF64_SECTION_SIZE_64,   data_len);

    // Update ELF header: section count, sh-table offset.
    wr16(e->data + ELF64_SH_COUNT_OFFSET, (uint16_t)new_sh_count);
    wr64(e->data + ELF64_SH_TABLE_OFFSET, new_sh_off);

    e->len = new_total;
    return elf_parse(e);
}

// -----------------------------------------------------------------------
// Unchalk: replace chalk section content with 32 zeros, sha256 file,
// store hash in chalk section. The chalk section gets renamed to
// .chalk.free if it was .chalk.mark.
// -----------------------------------------------------------------------

static bool
elf_unchalk_for_hash(elf_t *e, uint8_t out_hash[32])
{
    uint8_t zeros[SHA256_BYTE_LENGTH] = {0};
    if (!elf_insert_or_set(e, SH_NAME_CHALKFREE, zeros, SHA256_BYTE_LENGTH)) {
        return false;
    }
    n00b_sha256_digest_t words;
    n00b_sha256_hash(e->data, e->len, words);
    for (int i = 0; i < 8; i++) {
        uint32_t w = words[i];
        out_hash[i * 4]     = (uint8_t)((w >> 24) & 0xff);
        out_hash[i * 4 + 1] = (uint8_t)((w >> 16) & 0xff);
        out_hash[i * 4 + 2] = (uint8_t)((w >> 8) & 0xff);
        out_hash[i * 4 + 3] = (uint8_t)(w & 0xff);
    }
    // Store hash into the chalk section bytes (which are now 32 zeros).
    elf_section_t *ch = &e->sections[e->chalk_idx];
    memcpy(e->data + ch->offset, out_hash, SHA256_BYTE_LENGTH);
    return true;
}

// -----------------------------------------------------------------------
// Codec entry points
// -----------------------------------------------------------------------

n00b_result_t(n00b_buffer_t *)
n00b_chalk_elf_hash_buffer(n00b_buffer_t *bytes)
{
    elf_t *e = elf_new(bytes);
    if (!e) {
        // Fallback: plain SHA-256 of the raw bytes.
        return n00b_result_ok(n00b_buffer_t *,
                              n00b_chalk_sha256_buffer(bytes));
    }
    uint8_t hash[32];
    if (!elf_unchalk_for_hash(e, hash)) {
        return n00b_result_ok(n00b_buffer_t *,
                              n00b_chalk_sha256_buffer(bytes));
    }
    return n00b_result_ok(n00b_buffer_t *,
                          n00b_buffer_from_bytes((char *)hash, 32));
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_elf_insert_buffer(n00b_buffer_t *bytes, n00b_chalk_mark_t *mark)
{
    if (!bytes || !mark) return n00b_result_err(n00b_chalk_io_result_t *, 1);
    elf_t *e = elf_new(bytes);
    if (!e) return n00b_result_err(n00b_chalk_io_result_t *, 2);

    uint8_t hash[32];
    if (!elf_unchalk_for_hash(e, hash)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 3);
    }
    n00b_buffer_t *hash_buf = n00b_buffer_from_bytes((char *)hash, 32);
    auto fin = n00b_chalk_mark_finalize(mark, hash_buf);
    if (n00b_result_is_err(fin)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 4);
    }
    n00b_buffer_t *encoded = n00b_result_get(fin);
    if (!elf_insert_or_set(e, SH_NAME_CHALKMARK,
                            (const uint8_t *)encoded->data,
                            (size_t)encoded->byte_len)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 5);
    }
    auto r = (n00b_chalk_io_result_t *)n00b_alloc(n00b_chalk_io_result_t);
    r->kind           = N00B_CHALK_OUT_IN_BAND;
    r->bytes          = n00b_buffer_from_bytes((char *)e->data, (int64_t)e->len);
    r->sidecar_suffix = nullptr;
    return n00b_result_ok(n00b_chalk_io_result_t *, r);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_elf_delete_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_chalk_io_result_t *, 1);
    elf_t *e = elf_new(bytes);
    if (!e) return n00b_result_err(n00b_chalk_io_result_t *, 2);
    uint8_t hash[32];
    if (!elf_unchalk_for_hash(e, hash)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 3);
    }
    auto r = (n00b_chalk_io_result_t *)n00b_alloc(n00b_chalk_io_result_t);
    r->kind           = N00B_CHALK_OUT_IN_BAND;
    r->bytes          = n00b_buffer_from_bytes((char *)e->data, (int64_t)e->len);
    r->sidecar_suffix = nullptr;
    return n00b_result_ok(n00b_chalk_io_result_t *, r);
}

n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_elf_extract_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_chalk_extract_result_t *, 1);
    elf_t *e = elf_new(bytes);
    if (!e) return n00b_result_err(n00b_chalk_extract_result_t *, 2);
    if (e->chalk_idx < 0 || e->has_been_unchalked) {
        return n00b_result_err(n00b_chalk_extract_result_t *, 3);
    }
    elf_section_t *ch  = &e->sections[e->chalk_idx];
    n00b_buffer_t *payload = n00b_buffer_from_bytes((char *)e->data + ch->offset,
                                                     (int64_t)ch->size);
    return n00b_chalk_sidecar_parse_bytes(payload, N00B_CHALK_CODEC_ELF);
}

// File-mode entries via the shared helpers.
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

// -----------------------------------------------------------------------
// Fallback codec: header-offset scan only. Supports extract + hash,
// returns ENOTSUP for insert/delete. Mirrors codecFallbackElf.nim.
// -----------------------------------------------------------------------

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
    // Walk back to `{`, then forward to matching `}`.
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
    return n00b_chalk_sidecar_parse_bytes(payload, N00B_CHALK_CODEC_ELF_FALLBACK);
}

n00b_result_t(n00b_buffer_t *)
n00b_chalk_elf_fallback_hash_buffer(n00b_buffer_t *bytes)
{
    // Plain SHA-256 of the file as-is; the fallback has no notion of
    // "unchalked" form beyond what the user gave us.
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
    return n00b_chalk_file_hash_via(path, n00b_chalk_elf_fallback_hash_buffer);
}
