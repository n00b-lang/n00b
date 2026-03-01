#include <string.h>
#include "compiler/objfile/pe_build.h"

// ============================================================================
// Alignment helpers
// ============================================================================

static uint32_t
align_up(uint32_t value, uint32_t alignment)
{
    if (alignment == 0) {
        return value;
    }

    return (value + alignment - 1) & ~(alignment - 1);
}

// ============================================================================
// Simple qsort comparator for import name sorting (export names)
// ============================================================================

static int
cmp_export_names(const void *a, const void *b)
{
    const n00b_pe_exported_func_t *fa = (const n00b_pe_exported_func_t *)a;
    const n00b_pe_exported_func_t *fb = (const n00b_pe_exported_func_t *)b;

    if (!fa->name && !fb->name) {
        return 0;
    }

    if (!fa->name) {
        return 1;
    }

    if (!fb->name) {
        return -1;
    }

    return strcmp(fa->name->data, fb->name->data);
}

// ============================================================================
// Build .idata section content (import tables)
// ============================================================================

/// Compute size needed for .idata section.
static uint32_t
compute_idata_size(n00b_pe_binary_t *bin)
{
    if (bin->num_imports == 0) {
        return 0;
    }

    // Import descriptor table: (num_imports + 1) * 20  (+1 for null term)
    uint32_t size = (bin->num_imports + 1) * N00B_PE_IMPORT_DESCRIPTOR_SIZE;

    for (uint32_t i = 0; i < bin->num_imports; i++) {
        n00b_pe_import_t *imp = &bin->imports[i];

        // ILT: (num_functions + 1) * 8
        size += (imp->num_functions + 1) * 8;

        // IAT: same as ILT
        size += (imp->num_functions + 1) * 8;

        // Hint/Name entries
        for (uint32_t j = 0; j < imp->num_functions; j++) {
            if (!imp->functions[j].is_ordinal && imp->functions[j].name) {
                // 2 (hint) + strlen + 1 (NUL), aligned to 2
                uint32_t entry_sz = 2 + (uint32_t)strlen(
                                        imp->functions[j].name->data) + 1;
                size += align_up(entry_sz, 2);
            }
        }

        // DLL name: strlen + 1
        if (imp->name) {
            size += (uint32_t)strlen(imp->name->data) + 1;
        }
    }

    return align_up(size, 4);
}

/// Write .idata content into writer, return the RVA-relative offsets.
static void
write_idata(n00b_writer_t *w, n00b_pe_binary_t *bin,
            uint32_t idata_rva, uint32_t idata_file_off)
{
    uint32_t desc_off = idata_file_off;
    uint32_t data_off = desc_off
                        + (bin->num_imports + 1) * N00B_PE_IMPORT_DESCRIPTOR_SIZE;

    // First pass: write ILT/IAT/hint-name/DLL-name, record RVAs
    // We need to compute the offsets for each import's ILT and IAT.

    // Allocate temp arrays for ILT and IAT RVAs
    uint32_t *ilt_rvas = n00b_alloc_array(uint32_t, bin->num_imports);
    uint32_t *iat_rvas = n00b_alloc_array(uint32_t, bin->num_imports);
    uint32_t *name_rvas = n00b_alloc_array(uint32_t, bin->num_imports);

    // Layout: after descriptors, ILT arrays, IAT arrays, hint/name, DLL names
    uint32_t cur = data_off;

    for (uint32_t i = 0; i < bin->num_imports; i++) {
        n00b_pe_import_t *imp = &bin->imports[i];

        // ILT
        ilt_rvas[i] = idata_rva + (cur - idata_file_off);
        cur += (imp->num_functions + 1) * 8;

        // IAT
        iat_rvas[i] = idata_rva + (cur - idata_file_off);
        cur += (imp->num_functions + 1) * 8;
    }

    // Hint/Name entries and DLL names follow
    uint32_t hn_start = cur;
    (void)hn_start;

    // Write ILT and IAT entries, and hint/name entries
    for (uint32_t i = 0; i < bin->num_imports; i++) {
        n00b_pe_import_t *imp = &bin->imports[i];
        uint32_t ilt_off = idata_file_off
                           + (ilt_rvas[i] - idata_rva);
        uint32_t iat_off = idata_file_off
                           + (iat_rvas[i] - idata_rva);

        for (uint32_t j = 0; j < imp->num_functions; j++) {
            uint64_t thunk_val;

            if (imp->functions[j].is_ordinal) {
                thunk_val = N00B_PE_IMPORT_ORDINAL_FLAG64
                            | imp->functions[j].ordinal;
            }
            else {
                // Hint/Name entry at cur
                uint32_t hn_rva = idata_rva + (cur - idata_file_off);

                n00b_writer_setpos(w, cur);
                n00b_writer_write_u16(w, imp->functions[j].hint);

                if (imp->functions[j].name) {
                    n00b_writer_write_cstring(w,
                                              imp->functions[j].name->data);
                }
                else {
                    n00b_writer_write_u8(w, 0);
                }

                // Align to 2
                if (n00b_writer_pos(w) & 1) {
                    n00b_writer_write_u8(w, 0);
                }

                cur = (uint32_t)n00b_writer_pos(w);
                thunk_val = hn_rva;
            }

            // Write ILT entry
            n00b_writer_setpos(w, ilt_off + (size_t)j * 8);
            n00b_writer_write_u64(w, thunk_val);

            // Write IAT entry (copy of ILT)
            n00b_writer_setpos(w, iat_off + (size_t)j * 8);
            n00b_writer_write_u64(w, thunk_val);
        }

        // Null terminator for ILT and IAT
        n00b_writer_setpos(w, ilt_off + (size_t)imp->num_functions * 8);
        n00b_writer_write_u64(w, 0);
        n00b_writer_setpos(w, iat_off + (size_t)imp->num_functions * 8);
        n00b_writer_write_u64(w, 0);

        // DLL name
        name_rvas[i] = idata_rva + (cur - idata_file_off);
        n00b_writer_setpos(w, cur);

        if (imp->name) {
            n00b_writer_write_cstring(w, imp->name->data);
        }
        else {
            n00b_writer_write_u8(w, 0);
        }

        cur = (uint32_t)n00b_writer_pos(w);
    }

    // Write import descriptors
    n00b_writer_setpos(w, desc_off);

    for (uint32_t i = 0; i < bin->num_imports; i++) {
        n00b_writer_write_u32(w, ilt_rvas[i]);   // OriginalFirstThunk
        n00b_writer_write_u32(w, 0);             // TimeDateStamp
        n00b_writer_write_u32(w, 0);             // ForwarderChain
        n00b_writer_write_u32(w, name_rvas[i]);  // Name
        n00b_writer_write_u32(w, iat_rvas[i]);   // FirstThunk
    }

    // Null terminator descriptor
    n00b_writer_write_zeros(w, N00B_PE_IMPORT_DESCRIPTOR_SIZE);
}

// ============================================================================
// Build .edata section content (export table)
// ============================================================================

static uint32_t
compute_edata_size(n00b_pe_binary_t *bin)
{
    if (!bin->export_info || bin->export_info->num_functions == 0) {
        return 0;
    }

    n00b_pe_export_info_t *ei = bin->export_info;

    // Export directory: 40 bytes
    uint32_t size = 40;

    // Address table: num_functions * 4
    size += ei->num_functions * 4;

    // Count named functions
    uint32_t named = 0;

    for (uint32_t i = 0; i < ei->num_functions; i++) {
        if (ei->functions[i].name) {
            named++;
        }
    }

    // Name pointer table: named * 4
    size += named * 4;

    // Ordinal table: named * 2
    size += named * 2;

    // Name strings
    for (uint32_t i = 0; i < ei->num_functions; i++) {
        if (ei->functions[i].name) {
            size += (uint32_t)strlen(ei->functions[i].name->data) + 1;
        }
    }

    // Module name
    if (ei->name) {
        size += (uint32_t)strlen(ei->name->data) + 1;
    }

    return align_up(size, 4);
}

static void
write_edata(n00b_writer_t *w, n00b_pe_binary_t *bin,
            uint32_t edata_rva, uint32_t edata_file_off)
{
    n00b_pe_export_info_t *ei = bin->export_info;

    // Count named exports
    uint32_t named = 0;

    for (uint32_t i = 0; i < ei->num_functions; i++) {
        if (ei->functions[i].name) {
            named++;
        }
    }

    // Layout within .edata
    uint32_t dir_off    = edata_file_off;
    uint32_t addr_off   = dir_off + 40;
    uint32_t nptr_off   = addr_off + ei->num_functions * 4;
    uint32_t ord_off    = nptr_off + named * 4;
    uint32_t str_off    = ord_off + named * 2;

    // Compute RVAs
    uint32_t addr_rva   = edata_rva + (addr_off - edata_file_off);
    uint32_t nptr_rva   = edata_rva + (nptr_off - edata_file_off);
    uint32_t ord_rva    = edata_rva + (ord_off - edata_file_off);

    // Write module name first in string area
    uint32_t modname_rva = 0;

    n00b_writer_setpos(w, str_off);

    if (ei->name) {
        modname_rva = edata_rva + (str_off - edata_file_off);
        n00b_writer_write_cstring(w, ei->name->data);
    }

    // Write function name strings, record their RVAs
    // Build index of named functions sorted alphabetically
    uint32_t *name_indices = n00b_alloc_array(uint32_t, named);
    uint32_t ni = 0;

    for (uint32_t i = 0; i < ei->num_functions; i++) {
        if (ei->functions[i].name) {
            name_indices[ni++] = i;
        }
    }

    // Simple insertion sort by name (for binary search requirement)
    for (uint32_t i = 1; i < named; i++) {
        uint32_t key = name_indices[i];

        int j = (int)i - 1;

        while (j >= 0
               && strcmp(ei->functions[name_indices[j]].name->data,
                         ei->functions[key].name->data) > 0) {
            name_indices[j + 1] = name_indices[j];
            j--;
        }

        name_indices[j + 1] = key;
    }

    uint32_t *name_rvas = n00b_alloc_array(uint32_t, named);

    for (uint32_t i = 0; i < named; i++) {
        uint32_t fi = name_indices[i];

        name_rvas[i] = edata_rva
                        + ((uint32_t)n00b_writer_pos(w) - edata_file_off);
        n00b_writer_write_cstring(w, ei->functions[fi].name->data);
    }

    // Write export directory (40 bytes)
    n00b_writer_setpos(w, dir_off);
    n00b_writer_write_u32(w, 0);                 // Characteristics
    n00b_writer_write_u32(w, 0);                 // TimeDateStamp
    n00b_writer_write_u16(w, 0);                 // MajorVersion
    n00b_writer_write_u16(w, 0);                 // MinorVersion
    n00b_writer_write_u32(w, modname_rva);       // Name
    n00b_writer_write_u32(w, ei->ordinal_base);  // Base
    n00b_writer_write_u32(w, ei->num_functions); // NumberOfFunctions
    n00b_writer_write_u32(w, named);             // NumberOfNames
    n00b_writer_write_u32(w, addr_rva);          // AddressOfFunctions
    n00b_writer_write_u32(w, nptr_rva);          // AddressOfNames
    n00b_writer_write_u32(w, ord_rva);           // AddressOfNameOrdinals

    // Write address table
    n00b_writer_setpos(w, addr_off);

    for (uint32_t i = 0; i < ei->num_functions; i++) {
        n00b_writer_write_u32(w, ei->functions[i].rva);
    }

    // Write name pointer table (sorted)
    n00b_writer_setpos(w, nptr_off);

    for (uint32_t i = 0; i < named; i++) {
        n00b_writer_write_u32(w, name_rvas[i]);
    }

    // Write ordinal table
    n00b_writer_setpos(w, ord_off);

    for (uint32_t i = 0; i < named; i++) {
        uint32_t fi = name_indices[i];

        // Ordinal index = ordinal - ordinal_base
        uint16_t ord_idx = (uint16_t)(ei->functions[fi].ordinal
                                       - ei->ordinal_base);
        n00b_writer_write_u16(w, ord_idx);
    }
}

// ============================================================================
// Build .reloc section content
// ============================================================================

static uint32_t
compute_reloc_size(n00b_pe_binary_t *bin)
{
    if (bin->num_relocations == 0) {
        return 0;
    }

    // Group by page (rva & ~0xFFF)
    // Worst case: each reloc on a different page → 8 + 2 per reloc + padding
    // We'll compute exactly.

    uint32_t size = 0;
    uint32_t i    = 0;

    while (i < bin->num_relocations) {
        uint32_t page = bin->relocations[i].rva & ~0xFFFu;

        // Count entries in this page
        uint32_t count = 0;

        while (i + count < bin->num_relocations
               && (bin->relocations[i + count].rva & ~0xFFFu) == page) {
            count++;
        }

        // Pad to even number of entries for 4-byte alignment
        uint32_t padded = count;

        if (padded & 1) {
            padded++;
        }

        size += 8 + padded * 2;
        i += count;
    }

    return size;
}

static void
write_reloc(n00b_writer_t *w, n00b_pe_binary_t *bin, uint32_t reloc_file_off)
{
    n00b_writer_setpos(w, reloc_file_off);

    uint32_t i = 0;

    while (i < bin->num_relocations) {
        uint32_t page = bin->relocations[i].rva & ~0xFFFu;

        // Count entries in this page
        uint32_t count = 0;

        while (i + count < bin->num_relocations
               && (bin->relocations[i + count].rva & ~0xFFFu) == page) {
            count++;
        }

        // Pad to even for 4-byte alignment
        uint32_t padded = count;

        if (padded & 1) {
            padded++;
        }

        uint32_t block_size = 8 + padded * 2;

        n00b_writer_write_u32(w, page);
        n00b_writer_write_u32(w, block_size);

        for (uint32_t j = 0; j < count; j++) {
            uint16_t offset = (uint16_t)(bin->relocations[i + j].rva & 0xFFF);
            uint16_t type   = (uint16_t)bin->relocations[i + j].type;
            uint16_t entry  = (type << 12) | offset;

            n00b_writer_write_u16(w, entry);
        }

        // Padding entries (type ABSOLUTE = 0)
        for (uint32_t j = count; j < padded; j++) {
            n00b_writer_write_u16(w, 0);
        }

        i += count;
    }
}

// ============================================================================
// Top-level builder
// ============================================================================

n00b_result_t(n00b_buffer_t *)
n00b_pe_build(n00b_pe_binary_t *bin)
{
    if (!bin) {
        return n00b_result_err(n00b_buffer_t *, N00B_ERR_BUILD);
    }

    uint32_t file_align = bin->file_alignment;
    uint32_t sec_align  = bin->section_alignment;

    if (file_align == 0) {
        file_align = 0x200;
    }

    if (sec_align == 0) {
        sec_align = 0x1000;
    }

    // ========================================================================
    // Phase 1: Compute synthetic section sizes
    // ========================================================================

    uint32_t idata_size = compute_idata_size(bin);
    uint32_t edata_size = compute_edata_size(bin);
    uint32_t reloc_size = compute_reloc_size(bin);

    // Count total sections (user + synthetic)
    uint32_t num_user_sections = bin->num_sections;
    uint32_t num_synth         = 0;

    if (idata_size > 0) {
        num_synth++;
    }

    if (edata_size > 0) {
        num_synth++;
    }

    if (reloc_size > 0) {
        num_synth++;
    }

    uint32_t total_sections = num_user_sections + num_synth;

    // ========================================================================
    // Phase 2: Layout
    // ========================================================================

    // Headers: DOS header (64) + stub → align to pe_offset
    //          PE sig (4) + file header (20) + optional header (240)
    //          Section headers (40 * total_sections)
    uint32_t pe_off     = bin->pe_offset > 0 ? bin->pe_offset : 0x80;
    uint32_t headers_sz = pe_off + 4 + N00B_PE_FILE_HEADER_SIZE
                          + N00B_PE_OPTIONAL_HEADER64_SIZE
                          + total_sections * N00B_PE_SECTION_HEADER_SIZE;
    uint32_t headers_aligned = align_up(headers_sz, file_align);

    // Assign section file offsets and RVAs
    // RVAs start at sec_align
    uint32_t cur_rva     = sec_align;
    uint32_t cur_file    = headers_aligned;

    // Track synthetic section positions
    uint32_t idata_rva = 0, idata_foff = 0;
    uint32_t edata_rva = 0, edata_foff = 0;
    uint32_t reloc_rva = 0, reloc_foff = 0;

    // User section layout
    typedef struct {
        uint32_t rva;
        uint32_t file_off;
        uint32_t raw_size;
        uint32_t virtual_size;
    } sec_layout_t;

    sec_layout_t *layouts = nullptr;

    if (num_user_sections > 0) {
        layouts = n00b_alloc_array(sec_layout_t, num_user_sections);
    }

    for (uint32_t i = 0; i < num_user_sections; i++) {
        n00b_pe_section_t *s = &bin->sections[i];

        uint32_t raw_sz = 0;

        if (s->content) {
            raw_sz = (uint32_t)n00b_buffer_len(s->content);
        }

        uint32_t virt_sz = s->virtual_size > 0 ? s->virtual_size : raw_sz;

        if (virt_sz == 0) {
            virt_sz = sec_align;
        }

        layouts[i].rva          = cur_rva;
        layouts[i].file_off     = cur_file;
        layouts[i].raw_size     = align_up(raw_sz, file_align);
        layouts[i].virtual_size = virt_sz;

        cur_rva  += align_up(virt_sz, sec_align);
        cur_file += layouts[i].raw_size > 0
                    ? layouts[i].raw_size : align_up(file_align, file_align);
    }

    // Synthetic sections
    if (idata_size > 0) {
        idata_rva  = cur_rva;
        idata_foff = cur_file;
        cur_rva   += align_up(idata_size, sec_align);
        cur_file  += align_up(idata_size, file_align);
    }

    if (edata_size > 0) {
        edata_rva  = cur_rva;
        edata_foff = cur_file;
        cur_rva   += align_up(edata_size, sec_align);
        cur_file  += align_up(edata_size, file_align);
    }

    if (reloc_size > 0) {
        reloc_rva  = cur_rva;
        reloc_foff = cur_file;
        cur_rva   += align_up(reloc_size, sec_align);
        cur_file  += align_up(reloc_size, file_align);
    }

    uint32_t size_of_image = align_up(cur_rva, sec_align);
    uint32_t total_file    = cur_file;

    // ========================================================================
    // Phase 3: Serialize
    // ========================================================================

    n00b_writer_t *w = n00b_writer_new(total_file);

    // --- DOS header (64 bytes) ---
    n00b_pe_dos_header_t dh = bin->dos_header;

    dh.e_magic  = N00B_PE_MAGIC_MZ;
    dh.e_lfanew = pe_off;

    n00b_writer_write_bytes(w, &dh, N00B_PE_DOS_HEADER_SIZE);

    // DOS stub (if present)
    if (bin->dos_stub) {
        n00b_writer_setpos(w, N00B_PE_DOS_HEADER_SIZE);
        n00b_writer_write_buffer(w, bin->dos_stub);
    }

    // --- PE signature ---
    n00b_writer_setpos(w, pe_off);
    n00b_writer_write_u32(w, N00B_PE_SIGNATURE);

    // --- File header (20 bytes) ---
    n00b_writer_write_u16(w, bin->machine);
    n00b_writer_write_u16(w, (uint16_t)total_sections);
    n00b_writer_write_u32(w, bin->time_date_stamp);
    n00b_writer_write_u32(w, bin->pointer_to_symbol_table);
    n00b_writer_write_u32(w, bin->number_of_symbols);
    n00b_writer_write_u16(w, N00B_PE_OPTIONAL_HEADER64_SIZE);
    n00b_writer_write_u16(w, bin->characteristics);

    // --- Optional header (240 bytes) ---
    size_t opt_hdr_start = n00b_writer_pos(w);

    // Use stored subsystem version, falling back to OS version for compat
    uint16_t subsys_major = bin->major_subsystem_version
                            ? bin->major_subsystem_version
                            : bin->major_os_version;
    uint16_t subsys_minor = bin->major_subsystem_version
                            ? bin->minor_subsystem_version
                            : bin->minor_os_version;

    n00b_writer_write_u16(w, N00B_PE_OPT_MAGIC_PE32P);
    n00b_writer_write_u8(w, bin->major_linker_version);
    n00b_writer_write_u8(w, bin->minor_linker_version);
    n00b_writer_write_u32(w, bin->size_of_code);
    n00b_writer_write_u32(w, bin->size_of_initialized_data);
    n00b_writer_write_u32(w, bin->size_of_uninitialized_data);
    n00b_writer_write_u32(w, bin->entry_point);
    n00b_writer_write_u32(w, bin->base_of_code);
    n00b_writer_write_u64(w, bin->imagebase);
    n00b_writer_write_u32(w, sec_align);
    n00b_writer_write_u32(w, file_align);
    n00b_writer_write_u16(w, bin->major_os_version);
    n00b_writer_write_u16(w, bin->minor_os_version);
    n00b_writer_write_u16(w, bin->major_image_version);
    n00b_writer_write_u16(w, bin->minor_image_version);
    n00b_writer_write_u16(w, subsys_major);
    n00b_writer_write_u16(w, subsys_minor);
    n00b_writer_write_u32(w, bin->win32_version_value);
    n00b_writer_write_u32(w, size_of_image);
    n00b_writer_write_u32(w, headers_aligned);
    n00b_writer_write_u32(w, bin->checksum);
    n00b_writer_write_u16(w, bin->subsystem);
    n00b_writer_write_u16(w, bin->dll_characteristics);
    n00b_writer_write_u64(w, bin->size_of_stack_reserve
                             ? bin->size_of_stack_reserve : 0x100000);
    n00b_writer_write_u64(w, bin->size_of_stack_commit
                             ? bin->size_of_stack_commit : 0x1000);
    n00b_writer_write_u64(w, bin->size_of_heap_reserve
                             ? bin->size_of_heap_reserve : 0x100000);
    n00b_writer_write_u64(w, bin->size_of_heap_commit
                             ? bin->size_of_heap_commit : 0x1000);
    n00b_writer_write_u32(w, bin->loader_flags);
    n00b_writer_write_u32(w, N00B_PE_NUM_DATA_DIRS);

    // Data directories (16 entries)
    // Start with zeros, then patch import/export/reloc/IAT
    size_t dd_start = n00b_writer_pos(w);

    n00b_writer_write_zeros(w, N00B_PE_NUM_DATA_DIRS * N00B_PE_DATA_DIRECTORY_SIZE);

    // Patch data directories
    if (edata_size > 0) {
        n00b_writer_patch_u32(w, dd_start + N00B_PE_DD_EXPORT * 8, edata_rva);
        n00b_writer_patch_u32(w, dd_start + N00B_PE_DD_EXPORT * 8 + 4, edata_size);
    }

    if (idata_size > 0) {
        n00b_writer_patch_u32(w, dd_start + N00B_PE_DD_IMPORT * 8, idata_rva);
        n00b_writer_patch_u32(w, dd_start + N00B_PE_DD_IMPORT * 8 + 4, idata_size);

        // IAT: starts after the import descriptors in .idata
        // Compute IAT RVA (it's the first IAT in the idata)
        // For simplicity, point IAT directory at the full .idata
        // (Real PE files have a separate IAT DD pointing at the IAT array)
    }

    if (reloc_size > 0) {
        n00b_writer_patch_u32(w, dd_start + N00B_PE_DD_BASERELOC * 8, reloc_rva);
        n00b_writer_patch_u32(w, dd_start + N00B_PE_DD_BASERELOC * 8 + 4,
                              reloc_size);
    }

    // Verify we wrote exactly the optional header
    assert(n00b_writer_pos(w) == opt_hdr_start + N00B_PE_OPTIONAL_HEADER64_SIZE);

    // --- Section headers ---
    // User sections
    for (uint32_t i = 0; i < num_user_sections; i++) {
        // Name: 8 bytes, zero-padded
        const char *name = bin->sections[i].name
                           ? bin->sections[i].name->data : "";
        size_t nlen = strlen(name);

        if (nlen > 8) {
            nlen = 8;
        }

        n00b_writer_write_bytes(w, name, nlen);
        n00b_writer_write_zeros(w, 8 - nlen);

        n00b_writer_write_u32(w, layouts[i].virtual_size);
        n00b_writer_write_u32(w, layouts[i].rva);
        n00b_writer_write_u32(w, layouts[i].raw_size);
        n00b_writer_write_u32(w, layouts[i].file_off);
        n00b_writer_write_u32(w, 0);  // PointerToRelocations
        n00b_writer_write_u32(w, 0);  // PointerToLinenumbers
        n00b_writer_write_u16(w, 0);  // NumberOfRelocations
        n00b_writer_write_u16(w, 0);  // NumberOfLinenumbers
        n00b_writer_write_u32(w, bin->sections[i].characteristics);
    }

    // Synthetic section headers
    if (idata_size > 0) {
        n00b_writer_write_bytes(w, ".idata\0\0", 8);
        n00b_writer_write_u32(w, idata_size);
        n00b_writer_write_u32(w, idata_rva);
        n00b_writer_write_u32(w, align_up(idata_size, file_align));
        n00b_writer_write_u32(w, idata_foff);
        n00b_writer_write_u32(w, 0);
        n00b_writer_write_u32(w, 0);
        n00b_writer_write_u16(w, 0);
        n00b_writer_write_u16(w, 0);
        n00b_writer_write_u32(w, N00B_PE_SCN_CNT_INITIALIZED | N00B_PE_SCN_MEM_READ);
    }

    if (edata_size > 0) {
        n00b_writer_write_bytes(w, ".edata\0\0", 8);
        n00b_writer_write_u32(w, edata_size);
        n00b_writer_write_u32(w, edata_rva);
        n00b_writer_write_u32(w, align_up(edata_size, file_align));
        n00b_writer_write_u32(w, edata_foff);
        n00b_writer_write_u32(w, 0);
        n00b_writer_write_u32(w, 0);
        n00b_writer_write_u16(w, 0);
        n00b_writer_write_u16(w, 0);
        n00b_writer_write_u32(w, N00B_PE_SCN_CNT_INITIALIZED | N00B_PE_SCN_MEM_READ);
    }

    if (reloc_size > 0) {
        n00b_writer_write_bytes(w, ".reloc\0\0", 8);
        n00b_writer_write_u32(w, reloc_size);
        n00b_writer_write_u32(w, reloc_rva);
        n00b_writer_write_u32(w, align_up(reloc_size, file_align));
        n00b_writer_write_u32(w, reloc_foff);
        n00b_writer_write_u32(w, 0);
        n00b_writer_write_u32(w, 0);
        n00b_writer_write_u16(w, 0);
        n00b_writer_write_u16(w, 0);
        n00b_writer_write_u32(w, N00B_PE_SCN_CNT_INITIALIZED | N00B_PE_SCN_MEM_READ
                              | N00B_PE_SCN_MEM_DISCARDABLE);
    }

    // ========================================================================
    // Phase 4: Write section data
    // ========================================================================

    // User sections
    for (uint32_t i = 0; i < num_user_sections; i++) {
        if (bin->sections[i].content) {
            n00b_writer_setpos(w, layouts[i].file_off);
            n00b_writer_write_buffer(w, bin->sections[i].content);
        }
    }

    // Synthetic sections
    if (idata_size > 0) {
        write_idata(w, bin, idata_rva, idata_foff);
    }

    if (edata_size > 0) {
        write_edata(w, bin, edata_rva, edata_foff);
    }

    if (reloc_size > 0) {
        write_reloc(w, bin, reloc_foff);
    }

    // ========================================================================
    // Phase 5: Finalize
    // ========================================================================

    // Ensure the writer position is at the end of the file so that
    // n00b_writer_finalize() does not truncate the buffer.
    n00b_writer_setpos(w, total_file);

    if (w->error) {
        return n00b_result_err(n00b_buffer_t *, N00B_ERR_BUILD);
    }

    return n00b_result_ok(n00b_buffer_t *, n00b_writer_finalize(w));
}
