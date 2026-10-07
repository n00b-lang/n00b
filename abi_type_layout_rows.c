#include "abi_type_layout_rows.h"

#include <limits.h>
#include <string.h>

#include "ir/layout.h"
#include "ir/typelang.h"
#include "lib/alloc.h"
#include "lib/list.h"

core_list_decl(n00b_abi_layout_v1);
core_list_decl(n00b_abi_physical_run_v1);
core_list_decl(uint32_t);

static bool
same_name(const char *left, const char *right)
{
    return left == right || (left && right && !strcmp(left, right));
}

static bool
same_runs(const ir_layout_bit_run_t *left, const ir_layout_bit_run_t *right,
          size_t count)
{
    if (count && (!left || !right)) return false;
    for (size_t i = 0; i < count; i++)
        if (left[i].offset != right[i].offset
            || left[i].width != right[i].width) return false;
    return true;
}

static bool
same_accesses(const ir_layout_access_t *left,
              const ir_layout_access_t *right, size_t count)
{
    if (count && (!left || !right)) return false;
    for (size_t i = 0; i < count; i++)
        if (left[i].unit_bit_offset != right[i].unit_bit_offset
            || left[i].unit_width_bits != right[i].unit_width_bits
            || left[i].lsb != right[i].lsb
            || left[i].value_width_bits != right[i].value_width_bits)
            return false;
    return true;
}

static bool
same_layout(ir_module_t *module, const ir_final_layout_t *left,
            const ir_final_layout_t *right)
{
    if (!left || !right) return left == right;
    if (left->intrinsic_bits != right->intrinsic_bits
        || left->alignment_bytes != right->alignment_bytes
        || left->n_occupied != right->n_occupied
        || left->n_reserved != right->n_reserved
        || left->n_members != right->n_members
        || left->has_discriminant != right->has_discriminant
        || left->discriminant_is_niche != right->discriminant_is_niche
        || !same_runs(left->occupied, right->occupied, left->n_occupied)
        || !same_runs(left->reserved, right->reserved, left->n_reserved)
        || left->member_overlap_region.offset
            != right->member_overlap_region.offset
        || left->member_overlap_region.width
            != right->member_overlap_region.width
        || !same_accesses(&left->discriminant, &right->discriminant, 1))
        return false;
    if (left->n_members && (!left->members || !right->members)) return false;
    for (size_t i = 0; i < left->n_members; i++) {
        const ir_final_member_layout_t *a = &left->members[i];
        const ir_final_member_layout_t *b = &right->members[i];
        if (!same_name(a->name, b->name) || a->ordinal != b->ordinal
            || a->bit_offset != b->bit_offset
            || a->n_occupied != b->n_occupied
            || a->n_accesses != b->n_accesses
            || !same_runs(a->occupied, b->occupied, a->n_occupied)
            || !same_accesses(a->accesses, b->accesses, a->n_accesses)
            || (a->type != b->type
                && (!a->type || !b->type
                    || ir_type_id(module, a->type)
                        != ir_type_id(module, b->type)))) return false;
    }
    return true;
}

static bool
append_runs(const ir_layout_bit_run_t *source, size_t count,
            uint64_t extent_bits, uint32_t stream_base,
            core_list_t(n00b_abi_physical_run_v1) *runs,
            core_list_t(uint32_t) *indexes, n00b_abi_range_v1 *range)
{
    *range = (n00b_abi_range_v1){0};
    if (!count) return true;
    if (!source || indexes->len > UINT32_MAX - stream_base) return false;
    range->first = stream_base + (uint32_t)indexes->len;
    for (size_t i = 0; i < count; i++) {
        uint64_t bit = source[i].offset;
        uint64_t remaining = source[i].width;
        if (!remaining || bit > extent_bits
            || remaining > extent_bits - bit) return false;
        while (remaining) {
            if (runs->len >= UINT32_MAX
                || indexes->len >= UINT32_MAX - stream_base) return false;
            uint32_t width = remaining > UINT32_MAX
                ? UINT32_MAX : (uint32_t)remaining;
            uint32_t run_index = (uint32_t)runs->len;
            core_list_push(*runs, ((n00b_abi_physical_run_v1){
                .byte_offset = bit / 8,
                .bit_offset = (uint32_t)(bit % 8),
                .width_bits = width,
            }));
            core_list_push(*indexes, run_index);
            bit += width;
            remaining -= width;
        }
    }
    range->count = (uint32_t)(stream_base + indexes->len - range->first);
    return true;
}

bool
n00bb_abi_type_layout_rows_build(
    n00bb_abi_type_index_t *index, ir_module_t *comptime,
    ir_module_t *runtime, uint32_t stream_base,
    n00bb_abi_type_identity_rows_t *types,
    n00bb_abi_type_layout_rows_t *out)
{
    if (!index || !runtime || !ir_module_target(runtime) || !types
        || !types->rows || !out
        || types->count != n00bb_abi_type_index_count(index)
        || (comptime && (ir_module_types(comptime)
                != ir_module_types(runtime)
            || !ir_module_target(comptime)
            || strcmp(ir_module_target(comptime), ir_module_target(runtime)))))
        return false;
    core_list_t(n00b_abi_layout_v1) layouts = {0};
    core_list_t(n00b_abi_physical_run_v1) runs = {0};
    core_list_t(uint32_t) indexes = {0};
    uint32_t *type_layout_indexes = core_alloc_array(uint32_t, types->count);
    core_list_push(layouts, ((n00b_abi_layout_v1){0}));
    core_list_push(runs, ((n00b_abi_physical_run_v1){0}));
    bool valid = true;
    for (size_t i = 0; valid && i < types->count; i++) {
        th_type *type = n00bb_abi_type_index_at(index, (uint32_t)i);
        const ir_final_layout_t *final = ir_layout_final(
            runtime, type, ir_module_target(runtime));
        const ir_final_layout_t *compile_layout = comptime
            ? ir_layout_final(comptime, type, ir_module_target(runtime))
            : nullptr;
        if (final && compile_layout
            && !same_layout(runtime, final, compile_layout)) {
            valid = false;
            break;
        }
        if (!final) final = compile_layout;
        if (!final) continue;
        if (!final->alignment_bytes || final->has_discriminant
            || final->n_members || layouts.len >= UINT32_MAX
            || types->rows[i].size_bits != final->intrinsic_bits
            || types->rows[i].alignment_bytes != final->alignment_bytes) {
            valid = false;
            break;
        }
        n00b_abi_layout_v1 row = {
            .type_index = (uint32_t)i,
            .policy = N00B_ABI_LAYOUT_DEFAULT,
        };
        valid = append_runs(final->occupied, final->n_occupied,
            final->intrinsic_bits, stream_base, &runs, &indexes,
            &row.occupied_runs)
            && append_runs(final->reserved, final->n_reserved,
                final->intrinsic_bits, stream_base, &runs, &indexes,
                &row.reserved_runs);
        if (!valid) break;
        type_layout_indexes[i] = (uint32_t)layouts.len;
        core_list_push(layouts, row);
    }
    if (valid) {
        for (size_t i = 0; i < types->count; i++)
            types->rows[i].layout_index = type_layout_indexes[i];
        *out = (n00bb_abi_type_layout_rows_t){
            .layouts = layouts.data, .layout_count = layouts.len,
            .runs = runs.data, .run_count = runs.len,
            .run_indexes = indexes.data, .run_index_count = indexes.len,
        };
    }
    else {
        core_list_free(layouts);
        core_list_free(runs);
        core_list_free(indexes);
    }
    core_free(type_layout_indexes);
    return valid;
}

void
n00bb_abi_type_layout_rows_release(n00bb_abi_type_layout_rows_t *rows)
{
    if (!rows) return;
    core_free(rows->layouts);
    core_free(rows->runs);
    core_free(rows->run_indexes);
    *rows = (n00bb_abi_type_layout_rows_t){0};
}
