/** @file src/chalk/pe.c — PE codec.
 *
 *  In-place section-table splice + header patches on the input bytes:
 *  no full parse-then-rebuild on the write paths. Mirrors the
 *  strategy of `src/chalk/macho_core.c` for Mach-O — parse only to
 *  locate offsets, then operate on raw bytes.
 *
 *  Hash convention: sha256 of the input file with the .chalk
 *  section's header entry and section data virtually removed. For
 *  first-time chalking this collapses to sha256(input). For
 *  re-marking, we copy the input bytes, splice out the .chalk header
 *  entry, splice out the .chalk section data, decrement
 *  NumberOfSections, and hash that synthetic buffer — same value
 *  the next extract → hash recomputes.
 */

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
#include <stdint.h>

#define CHALK_SECTION_NAME ".chalk"
#define CHALK_SECTION_CHARACTERISTICS                                          \
    (N00B_PE_SCN_CNT_INITIALIZED | N00B_PE_SCN_MEM_READ                        \
     | N00B_PE_SCN_MEM_DISCARDABLE)

// -----------------------------------------------------------------------
// Little-endian byte access
// -----------------------------------------------------------------------

static inline uint32_t
rd_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint16_t
rd_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline void
wr_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static inline void
wr_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static inline uint32_t
align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

// -----------------------------------------------------------------------
// PE layout (cached from parsed bin)
// -----------------------------------------------------------------------

typedef struct {
    uint32_t pe_offset;             // e_lfanew (offset to "PE\0\0")
    uint32_t coff_offset;            // pe_offset + 4
    uint32_t opt_offset;             // coff_offset + 20
    uint32_t opt_size;               // SizeOfOptionalHeader
    uint32_t section_table_offset;
    uint32_t num_sections;
    uint32_t section_alignment;
    uint32_t file_alignment;
    uint32_t size_of_image;
    uint32_t size_of_headers;
    uint32_t cert_va;                // file offset to cert table (or 0)
    uint32_t cert_size;
    uint32_t data_dir_count;
    uint32_t data_dir_offset;        // file offset to first data directory
} pe_layout_t;

static bool
pe_get_layout(n00b_buffer_t *input, n00b_pe_binary_t *bin, pe_layout_t *out)
{
    if (!input || !bin || !out) return false;
    if (input->byte_len < 0x40) return false;
    const uint8_t *p = (const uint8_t *)input->data;

    out->pe_offset    = bin->pe_offset;
    out->coff_offset  = out->pe_offset + 4;
    out->opt_offset   = out->coff_offset + 20;
    if (out->opt_offset + 2 > (uint32_t)input->byte_len) return false;
    out->opt_size     = rd_u16_le(p + out->coff_offset + 16);
    out->section_table_offset = out->opt_offset + out->opt_size;
    out->num_sections = bin->num_sections;
    out->section_alignment = bin->section_alignment;
    out->file_alignment    = bin->file_alignment;
    out->size_of_image     = bin->size_of_image;
    out->size_of_headers   = bin->size_of_headers;
    out->cert_va           = bin->data_dirs[N00B_PE_DD_CERTIFICATE].VirtualAddress;
    out->cert_size         = bin->data_dirs[N00B_PE_DD_CERTIFICATE].Size;
    out->data_dir_count    = bin->num_data_dirs;

    // DataDirectory starts after the windows-specific fields in OPT.
    // For PE32+ those end at OPT+112; for PE32 at OPT+96. The PE magic
    // distinguishes them.
    uint16_t magic = bin->magic;
    if (magic == 0x20b) {
        out->data_dir_offset = out->opt_offset + 112;
    }
    else if (magic == 0x10b) {
        out->data_dir_offset = out->opt_offset + 96;
    }
    else {
        return false;  // ROM image or unknown — bail.
    }
    return true;
}

// -----------------------------------------------------------------------
// In-place section-table splice
// -----------------------------------------------------------------------

// Where would the next-after-existing section's raw data go?
static uint32_t
max_raw_end(n00b_pe_binary_t *bin)
{
    uint32_t end = 0;
    for (uint32_t i = 0; i < bin->num_sections; i++) {
        n00b_pe_section_t *s = &bin->sections[i];
        if (s->raw_size == 0) continue;
        uint32_t e = s->raw_offset + s->raw_size;
        if (e > end) end = e;
    }
    return end;
}

static uint32_t
max_va_end(n00b_pe_binary_t *bin)
{
    uint32_t end = 0;
    for (uint32_t i = 0; i < bin->num_sections; i++) {
        n00b_pe_section_t *s = &bin->sections[i];
        uint32_t vs = s->virtual_size > 0 ? s->virtual_size : s->raw_size;
        uint32_t e  = s->virtual_address + vs;
        if (e > end) end = e;
    }
    return end;
}

// Append a section to a copy of `input`. Returns a new buffer with
// the section header spliced into the section table, the section
// data appended (before any trailing Authenticode cert table), and
// the COFF/Optional header fields patched. NULL on out-of-room
// errors (SizeOfHeaders too small to fit another section header).
static n00b_buffer_t *
pe_inplace_add_chalk(n00b_buffer_t *input, n00b_pe_binary_t *bin,
                     n00b_buffer_t *payload, uint32_t characteristics)
{
    pe_layout_t L;
    if (!pe_get_layout(input, bin, &L)) return nullptr;

    uint32_t fa = L.file_alignment;
    uint32_t sa = L.section_alignment;
    if (fa == 0 || sa == 0) return nullptr;

    // Section table must have room for one more 40-byte entry. The
    // section table sits in the headers region [opt_offset+opt_size,
    // size_of_headers). If that gap isn't 40+ bytes we'd have to grow
    // size_of_headers, which shifts every section's raw_offset — out
    // of scope here. Abort.
    uint32_t new_entry_offset = L.section_table_offset + 40 * L.num_sections;
    if (new_entry_offset + 40 > L.size_of_headers) return nullptr;

    // Compute new section placement.
    uint32_t pl = (uint32_t)payload->byte_len;
    uint32_t new_raw_size = align_up(pl == 0 ? 1 : pl, fa);
    uint32_t raw_end      = max_raw_end(bin);
    if (raw_end < L.size_of_headers) raw_end = L.size_of_headers;
    uint32_t new_raw_offset = align_up(raw_end, fa);
    uint32_t new_va         = align_up(max_va_end(bin), sa);
    uint32_t new_va_size    = pl;
    if (new_va_size == 0) new_va_size = 1;

    // Cert table handling — if present and would be overlapped, push
    // it back. Cert table's "RVA" is actually a file offset.
    uint32_t cert_off = L.cert_va;
    uint32_t cert_sz  = L.cert_size;
    uint32_t cert_new_off = cert_off;
    bool     cert_present = (cert_sz > 0 && cert_off > 0);

    if (cert_present && cert_off >= new_raw_offset) {
        cert_new_off = new_raw_offset + new_raw_size;
        // Cert tables are 8-byte aligned per spec.
        cert_new_off = (cert_new_off + 7) & ~7u;
    }

    // Compute output size.
    uint32_t out_size;
    if (cert_present) {
        out_size = cert_new_off + cert_sz;
    }
    else {
        uint32_t end_of_new_section = new_raw_offset + new_raw_size;
        out_size = (uint32_t)input->byte_len > end_of_new_section
                       ? (uint32_t)input->byte_len
                       : end_of_new_section;
    }

    // Allocate output and copy input.
    uint8_t *out = n00b_alloc_array(uint8_t, out_size);
    memset(out, 0, out_size);
    // Copy everything up to where new section data starts. If cert
    // shifted, we'll also need to copy original bytes up to cert,
    // not including cert itself (cert goes to its new offset).
    uint32_t copy_until = cert_present ? cert_off : (uint32_t)input->byte_len;
    if (copy_until > new_raw_offset) {
        // No, we never want to overwrite input bytes that live at
        // [new_raw_offset, copy_until) — but those bytes are
        // typically zero padding past raw_end. Just copy everything
        // up to new_raw_offset.
        copy_until = new_raw_offset;
    }
    memcpy(out, input->data, copy_until);

    // Patch: COFF.NumberOfSections (offset coff_offset + 2)
    uint32_t new_num = L.num_sections + 1;
    wr_u16_le(out + L.coff_offset + 2, (uint16_t)new_num);

    // Patch: OPT.SizeOfImage = align_up(new_va + new_va_size, sa)
    uint32_t new_size_of_image = align_up(new_va + new_va_size, sa);
    wr_u32_le(out + L.opt_offset + 56, new_size_of_image);

    // Patch: DataDirectory[CERTIFICATE].VirtualAddress if cert shifted.
    if (cert_present && cert_new_off != cert_off
        && L.data_dir_count > N00B_PE_DD_CERTIFICATE) {
        uint8_t *dd = out + L.data_dir_offset + 8 * N00B_PE_DD_CERTIFICATE;
        wr_u32_le(dd + 0, cert_new_off);
        // size stays the same
    }

    // Write the new section header entry.
    uint8_t *sh = out + new_entry_offset;
    memset(sh, 0, 40);
    size_t namelen = strlen(CHALK_SECTION_NAME);
    if (namelen > 8) namelen = 8;
    memcpy(sh, CHALK_SECTION_NAME, namelen);
    wr_u32_le(sh + 8,  pl);              // VirtualSize
    wr_u32_le(sh + 12, new_va);          // VirtualAddress
    wr_u32_le(sh + 16, new_raw_size);    // SizeOfRawData
    wr_u32_le(sh + 20, new_raw_offset);  // PointerToRawData
    wr_u32_le(sh + 24, 0);               // PointerToRelocations
    wr_u32_le(sh + 28, 0);               // PointerToLinenumbers
    wr_u16_le(sh + 32, 0);               // NumberOfRelocations
    wr_u16_le(sh + 34, 0);               // NumberOfLinenumbers
    wr_u32_le(sh + 36, characteristics); // Characteristics

    // Write the payload.
    if (pl > 0) {
        if (new_raw_offset + pl > out_size) {
            // Shouldn't happen given our size calc; defensive abort.
            return nullptr;
        }
        memcpy(out + new_raw_offset, payload->data, pl);
        // Trailing bytes in the aligned slot are zero (memset above).
    }

    // Relocate cert table if it shifted.
    if (cert_present && cert_new_off != cert_off) {
        if (cert_new_off + cert_sz > out_size) return nullptr;
        memcpy(out + cert_new_off,
               (const uint8_t *)input->data + cert_off,
               cert_sz);
    }
    else if (cert_present) {
        // Cert wasn't shifted, but we may not have copied that range
        // (we capped copy_until at new_raw_offset).
        if (cert_off + cert_sz > out_size) return nullptr;
        memcpy(out + cert_off,
               (const uint8_t *)input->data + cert_off,
               cert_sz);
    }

    return n00b_buffer_from_bytes((char *)out, (int64_t)out_size);
}

// Remove the .chalk section in place. Returns a new buffer with the
// .chalk section header entry removed, NumberOfSections decremented,
// and the .chalk data bytes excised (everything past the .chalk data
// gets shifted up). For sections following .chalk in file order,
// their PointerToRawData fields stay valid because we *don't* shift
// other sections — we just zero out the .chalk region and rely on
// downstream loaders ignoring the trailing zeros. This is a
// conservative approximation; a full implementation would compact.
//
// The minimum invariant we need: extract on the result returns no
// .chalk section. Hash equals sha256 of this returned buffer.
static n00b_buffer_t *
pe_inplace_remove_chalk(n00b_buffer_t *input, n00b_pe_binary_t *bin)
{
    pe_layout_t L;
    if (!pe_get_layout(input, bin, &L)) return nullptr;

    // Find .chalk section index.
    int      chalk_idx = -1;
    for (uint32_t i = 0; i < bin->num_sections; i++) {
        n00b_string_t *n = bin->sections[i].name;
        if (n && n->u8_bytes == strlen(CHALK_SECTION_NAME)
            && memcmp(n->data, CHALK_SECTION_NAME,
                      strlen(CHALK_SECTION_NAME)) == 0) {
            chalk_idx = (int)i;
            break;
        }
    }
    if (chalk_idx < 0) {
        // Nothing to remove — return a copy of input.
        return n00b_buffer_from_bytes(input->data, (int64_t)input->byte_len);
    }

    n00b_pe_section_t *cs = &bin->sections[chalk_idx];

    uint32_t out_size = (uint32_t)input->byte_len;
    uint8_t *out = n00b_alloc_array(uint8_t, out_size);
    memcpy(out, input->data, out_size);

    // Decrement NumberOfSections.
    wr_u16_le(out + L.coff_offset + 2, (uint16_t)(L.num_sections - 1));

    // Remove the .chalk entry from the section table by shifting all
    // subsequent entries down by 40 bytes.
    uint32_t entry_off  = L.section_table_offset + 40 * (uint32_t)chalk_idx;
    uint32_t tail_count = L.num_sections - 1 - (uint32_t)chalk_idx;
    if (tail_count > 0) {
        memmove(out + entry_off,
                out + entry_off + 40,
                tail_count * 40);
    }
    // Zero the freed entry slot (now past the new last entry).
    memset(out + L.section_table_offset + 40 * (L.num_sections - 1),
           0, 40);

    // Zero the .chalk section's raw data.
    if (cs->raw_size > 0
        && cs->raw_offset + cs->raw_size <= out_size) {
        memset(out + cs->raw_offset, 0, cs->raw_size);
    }

    // SizeOfImage update: if the chalk section was the topmost VA,
    // shrink. Compute new max VA over remaining sections.
    uint32_t new_va_max = 0;
    for (uint32_t i = 0; i < bin->num_sections; i++) {
        if ((int)i == chalk_idx) continue;
        n00b_pe_section_t *s = &bin->sections[i];
        uint32_t vs = s->virtual_size > 0 ? s->virtual_size : s->raw_size;
        uint32_t e  = s->virtual_address + vs;
        if (e > new_va_max) new_va_max = e;
    }
    uint32_t new_soi = align_up(new_va_max, L.section_alignment);
    wr_u32_le(out + L.opt_offset + 56, new_soi);

    return n00b_buffer_from_bytes((char *)out, (int64_t)out_size);
}

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

static n00b_pe_binary_t *
parse_pe(n00b_buffer_t *bytes)
{
    if (!bytes) return nullptr;
    n00b_bstream_t *bs = n00b_bstream_new(bytes);
    if (!bs) return nullptr;
    auto pr = n00b_pe_parse(bs);
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
    n00b_pe_section_t *existing = n00b_pe_section_by_name(bin,
                                                          CHALK_SECTION_NAME);
    if (!existing) {
        return n00b_result_ok(n00b_buffer_t *, sha256_buffer(bytes));
    }
    n00b_buffer_t *unchalked = pe_inplace_remove_chalk(bytes, bin);
    if (!unchalked) return n00b_result_err(n00b_buffer_t *, 2);
    return n00b_result_ok(n00b_buffer_t *, sha256_buffer(unchalked));
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_pe_insert_buffer(n00b_buffer_t *bytes, n00b_chalk_mark_t *mark)
{
    if (!bytes || !mark) return n00b_result_err(n00b_chalk_io_result_t *, 1);
    n00b_pe_binary_t *bin = parse_pe(bytes);
    if (!bin) return n00b_result_err(n00b_chalk_io_result_t *, 2);

    // Compute unchalked bytes for hashing. First-time chalking is
    // free (just hash input). Re-mark uses in-place removal.
    n00b_pe_section_t *had_chalk = n00b_pe_section_by_name(bin,
                                                            CHALK_SECTION_NAME);
    n00b_buffer_t *unchalked_bytes;
    if (!had_chalk) {
        unchalked_bytes = bytes;
    }
    else {
        unchalked_bytes = pe_inplace_remove_chalk(bytes, bin);
        if (!unchalked_bytes) {
            return n00b_result_err(n00b_chalk_io_result_t *, 3);
        }
        // The bin's parsed structure is now stale relative to
        // unchalked_bytes (still describes the chalked file). Re-parse
        // so subsequent in-place add operates on accurate offsets.
        bin = parse_pe(unchalked_bytes);
        if (!bin) return n00b_result_err(n00b_chalk_io_result_t *, 3);
    }

    n00b_buffer_t *hash_buf = sha256_buffer(unchalked_bytes);

    auto fin = n00b_chalk_mark_finalize(mark, hash_buf);
    if (n00b_result_is_err(fin)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 4);
    }
    n00b_buffer_t *encoded = n00b_result_get(fin);

    // In-place splice: copy unchalked bytes, append .chalk section
    // header + data, patch headers. No n00b_pe_build.
    n00b_buffer_t *out = pe_inplace_add_chalk(unchalked_bytes, bin, encoded,
                                              CHALK_SECTION_CHARACTERISTICS);
    if (!out) return n00b_result_err(n00b_chalk_io_result_t *, 5);

    n00b_chalk_io_result_t *r = n00b_alloc(n00b_chalk_io_result_t);
    r->kind           = N00B_CHALK_OUT_IN_BAND;
    r->bytes          = out;
    r->sidecar_suffix = nullptr;
    return n00b_result_ok(n00b_chalk_io_result_t *, r);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_pe_delete_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_chalk_io_result_t *, 1);
    n00b_pe_binary_t *bin = parse_pe(bytes);
    if (!bin) return n00b_result_err(n00b_chalk_io_result_t *, 2);
    n00b_buffer_t *out = pe_inplace_remove_chalk(bytes, bin);
    if (!out) return n00b_result_err(n00b_chalk_io_result_t *, 3);
    n00b_chalk_io_result_t *r = n00b_alloc(n00b_chalk_io_result_t);
    r->kind           = N00B_CHALK_OUT_IN_BAND;
    r->bytes          = out;
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

n00b_result_t(n00b_buffer_t *)
n00b_chalk_pe_strip_signature(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_buffer_t *, 1);
    n00b_pe_binary_t *bin = parse_pe(bytes);
    if (!bin) return n00b_result_err(n00b_buffer_t *, 2);
    bin->certificates     = nullptr;
    bin->num_certificates = 0;
    auto br = n00b_pe_build(bin);
    if (n00b_result_is_err(br)) {
        return n00b_result_err(n00b_buffer_t *, 3);
    }
    return n00b_result_ok(n00b_buffer_t *, n00b_result_get(br));
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
