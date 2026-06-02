/**
 * @file objfile_elf_casegen.h
 * @brief Test-local ELF64 fixture generator for object-file known answers.
 *
 * Generates small synthetic ELF byte buffers for parser and future strict
 * rewrite-admission tests. The helpers are intentionally local to unit tests;
 * production layout analysis should live under `compiler/objfile/`.
 *
 * Related modules:
 * - `compiler/objfile/elf.h` for the parser under test.
 * - `adt/interval_tree.h` for the existing interval tree that later layout
 *   admission work should reuse.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "n00b.h"
#include "core/buffer.h"
#include "compiler/objfile/elf_types.h"

typedef enum {
    N00B_TEST_ELF_CASE_KNOWN,
    N00B_TEST_ELF_CASE_EXPLORE,
    N00B_TEST_ELF_CASE_PENDING,
    N00B_TEST_ELF_CASE_DIVERGE,
    N00B_TEST_ELF_CASE_RETIRED,
} n00b_test_elf_case_state_t;

typedef enum {
    N00B_TEST_ELF_PARSE_OK,
    N00B_TEST_ELF_PARSE_REJECT,
} n00b_test_elf_parse_expect_t;

typedef enum {
    N00B_TEST_ELF_ORACLE_NONE,
    N00B_TEST_ELF_ORACLE_READ_TARGET,
} n00b_test_elf_oracle_mode_t;

typedef enum {
    N00B_TEST_ELF_ORACLE_VALID_TARGET,
    N00B_TEST_ELF_ORACLE_INVALID_TARGET,
} n00b_test_elf_oracle_expect_t;

typedef enum {
    N00B_TEST_ELF_DIVERGENCE_SHARED_SCOPE,
    N00B_TEST_ELF_DIVERGENCE_N00B_BROADER,
    N00B_TEST_ELF_DIVERGENCE_BRANDON_NARROWER,
    N00B_TEST_ELF_DIVERGENCE_DIAGNOSTIC_ONLY,
} n00b_test_elf_divergence_t;

typedef enum {
    N00B_TEST_ELF_GEN_VALID_MINIMAL_EXEC,
    N00B_TEST_ELF_GEN_BAD_MAGIC,
    N00B_TEST_ELF_GEN_ELF32_INPUT,
    N00B_TEST_ELF_GEN_TRUNCATED_HEADER,
    N00B_TEST_ELF_GEN_PHTAB_NOT_IN_LOAD,
    N00B_TEST_ELF_GEN_ENTRY_OUTSIDE_LOAD,
    N00B_TEST_ELF_GEN_ENTRY_IN_MEM_NOT_FILE,
    N00B_TEST_ELF_GEN_PT_PHDR_BAD_SIZE,
    N00B_TEST_ELF_GEN_SHSTRTAB_NOT_TERMINATED,
    N00B_TEST_ELF_GEN_OVERLAY_AFTER_SEGMENTS,
    N00B_TEST_ELF_GEN_LAYOUT_CLASSIFICATION,
    N00B_TEST_ELF_GEN_LAYOUT_NONZERO_UNKNOWN,
} n00b_test_elf_generator_t;

typedef struct {
    const char                     *name;
    n00b_test_elf_case_state_t      state;
    n00b_test_elf_generator_t       generator;
    n00b_test_elf_parse_expect_t    expect_parse;
    const char                     *expect_reason;
    n00b_test_elf_oracle_mode_t     oracle_mode;
    n00b_test_elf_oracle_expect_t   oracle_expect;
    n00b_test_elf_divergence_t      divergence;
    const char                     *description;
} n00b_test_elf_case_t;

static const n00b_test_elf_case_t n00b_test_elf_cases[] = {
    {
        .name          = "valid_minimal_exec",
        .state         = N00B_TEST_ELF_CASE_KNOWN,
        .generator     = N00B_TEST_ELF_GEN_VALID_MINIMAL_EXEC,
        .expect_parse  = N00B_TEST_ELF_PARSE_OK,
        .expect_reason = "ok",
        .oracle_mode   = N00B_TEST_ELF_ORACLE_READ_TARGET,
        .oracle_expect = N00B_TEST_ELF_ORACLE_VALID_TARGET,
        .divergence    = N00B_TEST_ELF_DIVERGENCE_SHARED_SCOPE,
        .description   = "Minimal ELF64 executable satisfying Brandon's reader.",
    },
    {
        .name          = "bad_magic",
        .state         = N00B_TEST_ELF_CASE_KNOWN,
        .generator     = N00B_TEST_ELF_GEN_BAD_MAGIC,
        .expect_parse  = N00B_TEST_ELF_PARSE_REJECT,
        .expect_reason = "bad_magic",
        .oracle_mode   = N00B_TEST_ELF_ORACLE_READ_TARGET,
        .oracle_expect = N00B_TEST_ELF_ORACLE_INVALID_TARGET,
        .divergence    = N00B_TEST_ELF_DIVERGENCE_SHARED_SCOPE,
        .description   = "Header-sized input without ELF magic.",
    },
    {
        .name          = "elf32_input",
        .state         = N00B_TEST_ELF_CASE_KNOWN,
        .generator     = N00B_TEST_ELF_GEN_ELF32_INPUT,
        .expect_parse  = N00B_TEST_ELF_PARSE_REJECT,
        .expect_reason = "unsupported_elf_class",
        .oracle_mode   = N00B_TEST_ELF_ORACLE_READ_TARGET,
        .oracle_expect = N00B_TEST_ELF_ORACLE_INVALID_TARGET,
        .divergence    = N00B_TEST_ELF_DIVERGENCE_SHARED_SCOPE,
        .description   = "ELF magic with EI_CLASS set to ELFCLASS32.",
    },
    {
        .name          = "truncated_header",
        .state         = N00B_TEST_ELF_CASE_KNOWN,
        .generator     = N00B_TEST_ELF_GEN_TRUNCATED_HEADER,
        .expect_parse  = N00B_TEST_ELF_PARSE_REJECT,
        .expect_reason = "truncated_header",
        .oracle_mode   = N00B_TEST_ELF_ORACLE_READ_TARGET,
        .oracle_expect = N00B_TEST_ELF_ORACLE_INVALID_TARGET,
        .divergence    = N00B_TEST_ELF_DIVERGENCE_SHARED_SCOPE,
        .description   = "ELF magic but shorter than an ELF64 header.",
    },
    {
        .name          = "phtab_not_in_load",
        .state         = N00B_TEST_ELF_CASE_EXPLORE,
        .generator     = N00B_TEST_ELF_GEN_PHTAB_NOT_IN_LOAD,
        .expect_parse  = N00B_TEST_ELF_PARSE_OK,
        .expect_reason = "no_admission_api",
        .oracle_mode   = N00B_TEST_ELF_ORACLE_READ_TARGET,
        .oracle_expect = N00B_TEST_ELF_ORACLE_INVALID_TARGET,
        .divergence    = N00B_TEST_ELF_DIVERGENCE_N00B_BROADER,
        .description   = "PHTAB is present but outside all PT_LOAD ranges.",
    },
    {
        .name          = "entry_outside_load",
        .state         = N00B_TEST_ELF_CASE_EXPLORE,
        .generator     = N00B_TEST_ELF_GEN_ENTRY_OUTSIDE_LOAD,
        .expect_parse  = N00B_TEST_ELF_PARSE_OK,
        .expect_reason = "no_admission_api",
        .oracle_mode   = N00B_TEST_ELF_ORACLE_READ_TARGET,
        .oracle_expect = N00B_TEST_ELF_ORACLE_INVALID_TARGET,
        .divergence    = N00B_TEST_ELF_DIVERGENCE_N00B_BROADER,
        .description   = "Entrypoint address is outside all PT_LOAD ranges.",
    },
    {
        .name          = "entry_in_mem_not_file",
        .state         = N00B_TEST_ELF_CASE_EXPLORE,
        .generator     = N00B_TEST_ELF_GEN_ENTRY_IN_MEM_NOT_FILE,
        .expect_parse  = N00B_TEST_ELF_PARSE_OK,
        .expect_reason = "no_admission_api",
        .oracle_mode   = N00B_TEST_ELF_ORACLE_READ_TARGET,
        .oracle_expect = N00B_TEST_ELF_ORACLE_INVALID_TARGET,
        .divergence    = N00B_TEST_ELF_DIVERGENCE_N00B_BROADER,
        .description   = "Entrypoint is inside p_memsz but outside file bytes.",
    },
    {
        .name          = "pt_phdr_bad_size",
        .state         = N00B_TEST_ELF_CASE_EXPLORE,
        .generator     = N00B_TEST_ELF_GEN_PT_PHDR_BAD_SIZE,
        .expect_parse  = N00B_TEST_ELF_PARSE_OK,
        .expect_reason = "no_admission_api",
        .oracle_mode   = N00B_TEST_ELF_ORACLE_READ_TARGET,
        .oracle_expect = N00B_TEST_ELF_ORACLE_INVALID_TARGET,
        .divergence    = N00B_TEST_ELF_DIVERGENCE_N00B_BROADER,
        .description   = "PT_PHDR exists but does not match PHTAB size.",
    },
    {
        .name          = "shstrtab_not_terminated",
        .state         = N00B_TEST_ELF_CASE_EXPLORE,
        .generator     = N00B_TEST_ELF_GEN_SHSTRTAB_NOT_TERMINATED,
        .expect_parse  = N00B_TEST_ELF_PARSE_OK,
        .expect_reason = "ok",
        .oracle_mode   = N00B_TEST_ELF_ORACLE_READ_TARGET,
        .oracle_expect = N00B_TEST_ELF_ORACLE_VALID_TARGET,
        .divergence    = N00B_TEST_ELF_DIVERGENCE_DIAGNOSTIC_ONLY,
        .description   = "Section string table lacks an in-file trailing NUL.",
    },
    {
        .name          = "overlay_after_segments",
        .state         = N00B_TEST_ELF_CASE_PENDING,
        .generator     = N00B_TEST_ELF_GEN_OVERLAY_AFTER_SEGMENTS,
        .expect_parse  = N00B_TEST_ELF_PARSE_OK,
        .expect_reason = "ok",
        .oracle_mode   = N00B_TEST_ELF_ORACLE_NONE,
        .oracle_expect = N00B_TEST_ELF_ORACLE_VALID_TARGET,
        .divergence    = N00B_TEST_ELF_DIVERGENCE_DIAGNOSTIC_ONLY,
        .description   = "Extra bytes appear after all modeled ELF ranges.",
    },
    {
        .name          = "layout_classification",
        .state         = N00B_TEST_ELF_CASE_PENDING,
        .generator     = N00B_TEST_ELF_GEN_LAYOUT_CLASSIFICATION,
        .expect_parse  = N00B_TEST_ELF_PARSE_OK,
        .expect_reason = "layout_phase_2_only",
        .oracle_mode   = N00B_TEST_ELF_ORACLE_NONE,
        .oracle_expect = N00B_TEST_ELF_ORACLE_VALID_TARGET,
        .divergence    = N00B_TEST_ELF_DIVERGENCE_DIAGNOSTIC_ONLY,
        .description   = "ELF with string tables, interpreter, note, and NOBITS.",
    },
    {
        .name          = "layout_nonzero_unknown",
        .state         = N00B_TEST_ELF_CASE_PENDING,
        .generator     = N00B_TEST_ELF_GEN_LAYOUT_NONZERO_UNKNOWN,
        .expect_parse  = N00B_TEST_ELF_PARSE_OK,
        .expect_reason = "layout_phase_2_only",
        .oracle_mode   = N00B_TEST_ELF_ORACLE_NONE,
        .oracle_expect = N00B_TEST_ELF_ORACLE_VALID_TARGET,
        .divergence    = N00B_TEST_ELF_DIVERGENCE_DIAGNOSTIC_ONLY,
        .description   = "Layout fixture with a nonzero unmodeled byte gap.",
    },
};

static const size_t n00b_test_elf_case_count =
    sizeof(n00b_test_elf_cases) / sizeof(n00b_test_elf_cases[0]);

static inline const char *
n00b_test_elf_case_state_name(n00b_test_elf_case_state_t state)
{
    switch (state) {
    case N00B_TEST_ELF_CASE_KNOWN:
        return "known";
    case N00B_TEST_ELF_CASE_EXPLORE:
        return "explore";
    case N00B_TEST_ELF_CASE_PENDING:
        return "pending";
    case N00B_TEST_ELF_CASE_DIVERGE:
        return "diverge";
    case N00B_TEST_ELF_CASE_RETIRED:
        return "retired";
    }

    return "unknown";
}

static inline const char *
n00b_test_elf_oracle_mode_arg(n00b_test_elf_oracle_mode_t mode)
{
    switch (mode) {
    case N00B_TEST_ELF_ORACLE_NONE:
        return "none";
    case N00B_TEST_ELF_ORACLE_READ_TARGET:
        return "read-target";
    }

    return "none";
}

static inline const char *
n00b_test_elf_oracle_expect_name(n00b_test_elf_oracle_expect_t expect)
{
    switch (expect) {
    case N00B_TEST_ELF_ORACLE_VALID_TARGET:
        return "valid-target";
    case N00B_TEST_ELF_ORACLE_INVALID_TARGET:
        return "invalid-target";
    }

    return "oracle-error";
}

static inline const char *
n00b_test_elf_divergence_name(n00b_test_elf_divergence_t divergence)
{
    switch (divergence) {
    case N00B_TEST_ELF_DIVERGENCE_SHARED_SCOPE:
        return "shared-scope";
    case N00B_TEST_ELF_DIVERGENCE_N00B_BROADER:
        return "n00b-broader";
    case N00B_TEST_ELF_DIVERGENCE_BRANDON_NARROWER:
        return "brandon-narrower";
    case N00B_TEST_ELF_DIVERGENCE_DIAGNOSTIC_ONLY:
        return "diagnostic-only";
    }

    return "diagnostic-only";
}

static inline const n00b_test_elf_case_t *
n00b_test_elf_case_by_name(const char *name)
{
    for (size_t i = 0; i < n00b_test_elf_case_count; i++) {
        if (strcmp(n00b_test_elf_cases[i].name, name) == 0) {
            return &n00b_test_elf_cases[i];
        }
    }

    return nullptr;
}

static void
n00b_test_elf_put16(uint8_t *p, uint16_t v)
{
    memcpy(p, &v, sizeof(v));
}

static void
n00b_test_elf_put32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, sizeof(v));
}

static void
n00b_test_elf_put64(uint8_t *p, uint64_t v)
{
    memcpy(p, &v, sizeof(v));
}

static n00b_buffer_t *
n00b_test_elf_new_zeroed(size_t size)
{
    n00b_buffer_t *buf = n00b_buffer_new((int64_t)size);
    memset(buf->data, 0, size);
    buf->byte_len = size;
    return buf;
}

static void
n00b_test_elf_write_header(uint8_t *p,
                           uint16_t type,
                           uint64_t entry,
                           uint64_t phoff,
                           uint16_t phnum,
                           uint64_t shoff,
                           uint16_t shnum,
                           uint16_t shstrndx)
{
    p[0] = 0x7f;
    p[1] = 'E';
    p[2] = 'L';
    p[3] = 'F';
    p[EI_CLASS]   = ELFCLASS64;
    p[EI_DATA]    = ELFDATA2LSB;
    p[EI_VERSION] = EV_CURRENT;

    n00b_test_elf_put16(p + 16, type);
    n00b_test_elf_put16(p + 18, EM_X86_64);
    n00b_test_elf_put32(p + 20, EV_CURRENT);
    n00b_test_elf_put64(p + 24, entry);
    n00b_test_elf_put64(p + 32, phoff);
    n00b_test_elf_put64(p + 40, shoff);
    n00b_test_elf_put16(p + 52, 64);
    n00b_test_elf_put16(p + 54, 56);
    n00b_test_elf_put16(p + 56, phnum);
    n00b_test_elf_put16(p + 58, 64);
    n00b_test_elf_put16(p + 60, shnum);
    n00b_test_elf_put16(p + 62, shstrndx);
}

static void
n00b_test_elf_write_phdr(uint8_t *p,
                         uint32_t type,
                         uint32_t flags,
                         uint64_t offset,
                         uint64_t vaddr,
                         uint64_t filesz,
                         uint64_t memsz,
                         uint64_t align)
{
    n00b_test_elf_put32(p + 0, type);
    n00b_test_elf_put32(p + 4, flags);
    n00b_test_elf_put64(p + 8, offset);
    n00b_test_elf_put64(p + 16, vaddr);
    n00b_test_elf_put64(p + 24, vaddr);
    n00b_test_elf_put64(p + 32, filesz);
    n00b_test_elf_put64(p + 40, memsz);
    n00b_test_elf_put64(p + 48, align);
}

static void
n00b_test_elf_write_shdr(uint8_t *p,
                         uint32_t name,
                         uint32_t type,
                         uint64_t flags,
                         uint64_t addr,
                         uint64_t offset,
                         uint64_t size,
                         uint32_t link,
                         uint32_t info,
                         uint64_t addralign,
                         uint64_t entsize)
{
    n00b_test_elf_put32(p + 0, name);
    n00b_test_elf_put32(p + 4, type);
    n00b_test_elf_put64(p + 8, flags);
    n00b_test_elf_put64(p + 16, addr);
    n00b_test_elf_put64(p + 24, offset);
    n00b_test_elf_put64(p + 32, size);
    n00b_test_elf_put32(p + 40, link);
    n00b_test_elf_put32(p + 44, info);
    n00b_test_elf_put64(p + 48, addralign);
    n00b_test_elf_put64(p + 56, entsize);
}

static void
n00b_test_elf_write_shstrtab(uint8_t *p, size_t offset, bool terminated)
{
    uint8_t *strtab = p + offset;
    strtab[0] = '\0';
    memcpy(strtab + 1, ".shstrtab", 9);

    if (terminated) {
        strtab[10] = '\0';
    }
}

static n00b_buffer_t *
n00b_test_elf_minimal_exec(uint64_t entry,
                           uint64_t load_offset,
                           uint64_t load_vaddr,
                           uint64_t load_filesz,
                           uint64_t load_memsz,
                           bool include_pt_phdr,
                           bool bad_pt_phdr,
                           bool shstrtab_terminated,
                           bool include_overlay)
{
    const size_t base_size     = 512;
    const size_t overlay_size  = include_overlay ? 16 : 0;
    const size_t total_size    = base_size + overlay_size;
    const size_t phoff         = 64;
    const size_t phnum         = include_pt_phdr ? 2 : 1;
    const size_t shoff         = 256;
    const size_t shnum         = 2;
    const size_t shstrtab_off  = 384;
    const size_t shstrtab_size = shstrtab_terminated ? 11 : 10;

    n00b_buffer_t *buf = n00b_test_elf_new_zeroed(total_size);
    uint8_t       *p   = (uint8_t *)buf->data;

    n00b_test_elf_write_header(p, ET_EXEC, entry, phoff, (uint16_t)phnum,
                               shoff, (uint16_t)shnum, 1);

    n00b_test_elf_write_phdr(p + phoff,
                             PT_LOAD,
                             PF_R | PF_X,
                             load_offset,
                             load_vaddr,
                             load_filesz,
                             load_memsz,
                             0x1000);

    if (include_pt_phdr) {
        uint64_t phtab_size = phnum * 56;
        uint64_t phdr_size  = bad_pt_phdr ? 56 : phtab_size;

        n00b_test_elf_write_phdr(p + phoff + 56,
                                 PT_PHDR,
                                 PF_R,
                                 phoff,
                                 load_vaddr + phoff - load_offset,
                                 phdr_size,
                                 phdr_size,
                                 8);
    }

    n00b_test_elf_write_shdr(p + shoff + 64,
                             1,
                             SHT_STRTAB,
                             0,
                             0,
                             shstrtab_off,
                             shstrtab_size,
                             0,
                             0,
                             1,
                             0);
    n00b_test_elf_write_shstrtab(p, shstrtab_off, shstrtab_terminated);

    if (include_overlay) {
        for (size_t i = 0; i < overlay_size; i++) {
            p[base_size + i] = (uint8_t)(0xa0 + i);
        }
    }

    return buf;
}

static n00b_buffer_t *
n00b_test_elf_layout_classification(bool nonzero_unknown)
{
    const size_t total_size = 1152;
    const size_t phoff      = 64;
    const size_t phnum      = 3;
    const size_t shoff      = 640;
    const size_t shnum      = 8;

    const size_t interp_off = 240;
    const size_t interp_sz  = 16;
    const size_t note_off   = 256;
    const size_t note_sz    = 32;
    const size_t shstr_off  = 320;
    const size_t shstr_sz   = 54;
    const size_t str_off    = 384;
    const size_t str_sz     = 16;
    const size_t dynstr_off = 416;
    const size_t dynstr_sz  = 16;
    const size_t sym_off    = 448;
    const size_t sym_sz     = 2 * 24;
    const size_t dynsym_off = 512;
    const size_t dynsym_sz  = 2 * 24;

    n00b_buffer_t *buf = n00b_test_elf_new_zeroed(total_size);
    uint8_t       *p   = (uint8_t *)buf->data;

    n00b_test_elf_write_header(p, ET_EXEC, 0x400080, phoff, (uint16_t)phnum,
                               shoff, (uint16_t)shnum, 1);

    n00b_test_elf_write_phdr(p + phoff,
                             PT_LOAD,
                             PF_R | PF_X,
                             0,
                             0x400000,
                             232,
                             232,
                             0x1000);
    n00b_test_elf_write_phdr(p + phoff + 56,
                             PT_INTERP,
                             PF_R,
                             interp_off,
                             0x400000 + interp_off,
                             interp_sz,
                             interp_sz,
                             1);
    n00b_test_elf_write_phdr(p + phoff + 112,
                             PT_NOTE,
                             PF_R,
                             note_off,
                             0x400000 + note_off,
                             note_sz,
                             note_sz,
                             4);

    memcpy(p + interp_off, "/lib/ld.so", 10);
    p[interp_off + 10] = '\0';

    n00b_test_elf_put32(p + note_off + 0, 4);
    n00b_test_elf_put32(p + note_off + 4, 16);
    n00b_test_elf_put32(p + note_off + 8, NT_GNU_ABI_TAG);
    memcpy(p + note_off + 12, "GNU", 3);
    p[note_off + 15] = '\0';

    uint8_t *shstr = p + shstr_off;
    shstr[0] = '\0';
    memcpy(shstr + 1, ".shstrtab", 9);
    shstr[10] = '\0';
    memcpy(shstr + 11, ".strtab", 7);
    shstr[18] = '\0';
    memcpy(shstr + 19, ".symtab", 7);
    shstr[26] = '\0';
    memcpy(shstr + 27, ".dynstr", 7);
    shstr[34] = '\0';
    memcpy(shstr + 35, ".dynsym", 7);
    shstr[42] = '\0';
    memcpy(shstr + 43, ".note", 5);
    shstr[48] = '\0';
    memcpy(shstr + 49, ".bss", 4);
    shstr[53] = '\0';

    uint8_t *str = p + str_off;
    str[0] = '\0';
    memcpy(str + 1, "main", 4);
    str[5] = '\0';

    uint8_t *dynstr = p + dynstr_off;
    dynstr[0] = '\0';
    memcpy(dynstr + 1, "puts", 4);
    dynstr[5] = '\0';

    uint8_t *sym = p + sym_off + 24;
    n00b_test_elf_put32(sym + 0, 1);
    sym[4] = N00B_ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
    n00b_test_elf_put16(sym + 6, 1);
    n00b_test_elf_put64(sym + 8, 0x400080);

    uint8_t *dynsym = p + dynsym_off + 24;
    n00b_test_elf_put32(dynsym + 0, 1);
    dynsym[4] = N00B_ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
    n00b_test_elf_put16(dynsym + 6, 1);
    n00b_test_elf_put64(dynsym + 8, 0x400090);

    uint8_t *sh = p + shoff;
    n00b_test_elf_write_shdr(sh + 64,
                             1,
                             SHT_STRTAB,
                             0,
                             0,
                             shstr_off,
                             shstr_sz,
                             0,
                             0,
                             1,
                             0);
    n00b_test_elf_write_shdr(sh + 128,
                             11,
                             SHT_STRTAB,
                             0,
                             0,
                             str_off,
                             str_sz,
                             0,
                             0,
                             1,
                             0);
    n00b_test_elf_write_shdr(sh + 192,
                             19,
                             SHT_SYMTAB,
                             0,
                             0,
                             sym_off,
                             sym_sz,
                             2,
                             1,
                             8,
                             24);
    n00b_test_elf_write_shdr(sh + 256,
                             27,
                             SHT_STRTAB,
                             SHF_ALLOC,
                             0x400000 + dynstr_off,
                             dynstr_off,
                             dynstr_sz,
                             0,
                             0,
                             1,
                             0);
    n00b_test_elf_write_shdr(sh + 320,
                             35,
                             SHT_DYNSYM,
                             SHF_ALLOC,
                             0x400000 + dynsym_off,
                             dynsym_off,
                             dynsym_sz,
                             4,
                             1,
                             8,
                             24);
    n00b_test_elf_write_shdr(sh + 384,
                             43,
                             SHT_NOTE,
                             SHF_ALLOC,
                             0x400000 + note_off,
                             note_off,
                             note_sz,
                             0,
                             0,
                             4,
                             0);
    n00b_test_elf_write_shdr(sh + 448,
                             49,
                             SHT_NOBITS,
                             SHF_ALLOC | SHF_WRITE,
                             0x400600,
                             576,
                             32,
                             0,
                             0,
                             16,
                             0);

    if (nonzero_unknown) {
        p[300] = 0xcc;
    }

    return buf;
}

static n00b_buffer_t *
n00b_test_elf_case_generate(const n00b_test_elf_case_t *test_case)
{
    switch (test_case->generator) {
    case N00B_TEST_ELF_GEN_VALID_MINIMAL_EXEC:
        return n00b_test_elf_minimal_exec(0x400080, 0, 0x400000, 512, 512,
                                          false, false, true, false);
    case N00B_TEST_ELF_GEN_BAD_MAGIC: {
        n00b_buffer_t *buf = n00b_test_elf_new_zeroed(64);
        memcpy(buf->data, "NOPE", 4);
        return buf;
    }
    case N00B_TEST_ELF_GEN_ELF32_INPUT: {
        n00b_buffer_t *buf = n00b_test_elf_new_zeroed(64);
        uint8_t       *p   = (uint8_t *)buf->data;
        p[0] = 0x7f;
        p[1] = 'E';
        p[2] = 'L';
        p[3] = 'F';
        p[EI_CLASS]   = ELFCLASS32;
        p[EI_DATA]    = ELFDATA2LSB;
        p[EI_VERSION] = EV_CURRENT;
        return buf;
    }
    case N00B_TEST_ELF_GEN_TRUNCATED_HEADER: {
        n00b_buffer_t *buf = n00b_test_elf_new_zeroed(16);
        uint8_t       *p   = (uint8_t *)buf->data;
        p[0] = 0x7f;
        p[1] = 'E';
        p[2] = 'L';
        p[3] = 'F';
        p[EI_CLASS]   = ELFCLASS64;
        p[EI_DATA]    = ELFDATA2LSB;
        p[EI_VERSION] = EV_CURRENT;
        return buf;
    }
    case N00B_TEST_ELF_GEN_PHTAB_NOT_IN_LOAD:
        return n00b_test_elf_minimal_exec(0x400000, 128, 0x400000, 384, 384,
                                          false, false, true, false);
    case N00B_TEST_ELF_GEN_ENTRY_OUTSIDE_LOAD:
        return n00b_test_elf_minimal_exec(0x500000, 0, 0x400000, 512, 512,
                                          false, false, true, false);
    case N00B_TEST_ELF_GEN_ENTRY_IN_MEM_NOT_FILE:
        return n00b_test_elf_minimal_exec(0x400101, 0, 0x400000, 0x100, 0x2000,
                                          false, false, true, false);
    case N00B_TEST_ELF_GEN_PT_PHDR_BAD_SIZE:
        return n00b_test_elf_minimal_exec(0x400080, 0, 0x400000, 512, 512,
                                          true, true, true, false);
    case N00B_TEST_ELF_GEN_SHSTRTAB_NOT_TERMINATED:
        return n00b_test_elf_minimal_exec(0x400080, 0, 0x400000, 512, 512,
                                          false, false, false, false);
    case N00B_TEST_ELF_GEN_OVERLAY_AFTER_SEGMENTS:
        return n00b_test_elf_minimal_exec(0x400080, 0, 0x400000, 512, 512,
                                          false, false, true, true);
    case N00B_TEST_ELF_GEN_LAYOUT_CLASSIFICATION:
        return n00b_test_elf_layout_classification(false);
    case N00B_TEST_ELF_GEN_LAYOUT_NONZERO_UNKNOWN:
        return n00b_test_elf_layout_classification(true);
    }

    return nullptr;
}
