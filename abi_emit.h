#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/object_emit.h"
#include "lib/target.h"
#include "lib/module.h"
#include "ir/ir.h"
#include "parse/symtab.h"

typedef struct {
    uint32_t kind;
    const void *records;
    size_t record_count;
} n00bb_abi_table_rows_t;

typedef struct {
    uint8_t *bytes;
    size_t byte_count;
    uint32_t alignment_bytes;
} n00bb_abi_packed_tables_t;

/* Rows are already in ABI layout. Pointer-bearing fields must be zeroed
 * and supplied as object relocations by the caller. */
bool n00bb_abi_pack_tables(const core_target_desc_t *target,
                           uint64_t artifact_capabilities,
                           const n00bb_abi_table_rows_t *tables,
                           size_t table_count,
                           n00bb_abi_packed_tables_t *out);
void n00bb_abi_packed_tables_release(n00bb_abi_packed_tables_t *packed);

/* `extra_sections` follow the metadata section, which is section 1 in
 * `symbols` and `relocations`. The object and packed tables have separate
 * ownership. */
bool n00bb_abi_write_object(
    const core_target_desc_t *target,
    const n00bb_abi_packed_tables_t *packed,
    const core_object_emit_section_t *extra_sections, size_t extra_count,
    const core_object_emit_symbol_t *symbols, size_t symbol_count,
    const core_object_emit_absolute64_t *relocations,
    size_t relocation_count, uint8_t **out_image, size_t *out_size);

/* The first supported TU case: a finalized, acyclic unit with no local
 * declarations, globals, or executable initializer body. Builtin types and
 * no-op synthesized initializers are allowed. Other units fail rather than
 * emitting incomplete metadata. The caller owns both outputs. */
bool n00bb_abi_emit_empty_unit(
    core_translation_unit_t *unit, core_string_t package_id,
    const parse_symtab_t *symbols,
    const ir_module_t *comptime_ir, const ir_module_t *runtime_ir,
    const core_target_desc_t *target, uint64_t artifact_capabilities,
    n00bb_abi_packed_tables_t *out_tables,
    uint8_t **out_object, size_t *out_object_bytes);
