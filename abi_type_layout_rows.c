#include "abi_type_layout_rows.h"

#include <limits.h>
#include <string.h>

#include "ir/layout.h"
#include "ir/typelang.h"
#include "lib/alloc.h"
#include "lib/list.h"
#include "record_layout_request.h"

core_list_decl(n00b_abi_layout_v1);
core_list_decl(n00b_abi_physical_run_v1);
core_list_decl(n00b_abi_packed_run_v1);
core_list_decl(n00b_abi_field_v1);
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

static bool
append_accesses(const ir_layout_access_t *source, size_t count,
                uint64_t extent_bits, uint32_t stream_base,
                core_list_t(n00b_abi_packed_run_v1) *runs,
                core_list_t(uint32_t) *indexes, n00b_abi_range_v1 *range)
{
    *range = (n00b_abi_range_v1){0};
    if (!count) return true;
    if (!source || indexes->len > UINT32_MAX - stream_base) return false;
    range->first = stream_base + (uint32_t)indexes->len;
    for (size_t i = 0; i < count; i++) {
        const ir_layout_access_t *access = &source[i];
        if (!access->unit_width_bits || !access->value_width_bits
            || access->unit_bit_offset % 8
            || access->lsb >= access->unit_width_bits
            || access->value_width_bits
                > access->unit_width_bits - access->lsb
            || access->unit_bit_offset > extent_bits
            || access->lsb > extent_bits - access->unit_bit_offset
            || access->value_width_bits
                > extent_bits - access->unit_bit_offset - access->lsb
            || runs->len >= UINT32_MAX
            || indexes->len >= UINT32_MAX - stream_base) return false;
        uint32_t run_index = (uint32_t)runs->len;
        core_list_push(*runs, ((n00b_abi_packed_run_v1){
            .unit_byte_offset = access->unit_bit_offset / 8,
            .unit_width_bits = access->unit_width_bits,
            .lsb_position = access->lsb,
            .width_bits = access->value_width_bits,
        }));
        core_list_push(*indexes, run_index);
    }
    range->count = (uint32_t)(stream_base + indexes->len - range->first);
    return true;
}

static bool
record_policy(const parse_sym_entry_t *owner, uint32_t *out)
{
    *out = N00B_ABI_LAYOUT_DEFAULT;
    if (!owner) return true;
    n00bb_record_layout_request_t *request = parse_sym_binding(owner,
        CORE_STRING_STATIC("n00bb.record-layout-request"));
    if (!request || !request->policy_node) return true;
    switch (request->policy) {
    case N00BB_RECORD_DEFAULT: *out = N00B_ABI_LAYOUT_DEFAULT; return true;
    case N00BB_RECORD_ORDERED: *out = N00B_ABI_LAYOUT_ORDERED; return true;
    case N00BB_RECORD_C: *out = N00B_ABI_LAYOUT_C; return true;
    }
    return false;
}

static bool
append_fields(n00bb_abi_type_index_t *index,
              const n00bb_abi_decl_index_t *declarations,
              n00bb_abi_name_pool_t *names,
              const parse_sym_entry_t *owner,
              const ir_final_layout_t *final, uint32_t parent_type_index,
              uint32_t stream_base,
              core_list_t(n00b_abi_field_v1) *fields,
              core_list_t(n00b_abi_packed_run_v1) *packed,
              core_list_t(n00b_abi_physical_run_v1) *physical,
              core_list_t(uint32_t) *indexes,
              n00b_abi_range_v1 *field_range)
{
    *field_range = (n00b_abi_range_v1){0};
    if (!final->n_members) return true;
    if (!final->members || !names || final->n_members > UINT32_MAX
        || fields->len > UINT32_MAX - final->n_members)
        return false;
    uint32_t *member_indexes = core_alloc_array(uint32_t, final->n_members);
    bool valid = true;
    for (size_t i = 0; valid && i < final->n_members; i++) {
        const ir_final_member_layout_t *member = &final->members[i];
        n00b_abi_field_v1 row = {
            .parent_type_index = parent_type_index,
            .declaration_ordinal = member->ordinal,
            .logical_bit_offset = member->bit_offset,
            .visibility = N00B_ABI_VISIBILITY_PRIVATE,
        };
        parse_sym_entry_t *field = owner && owner->member_scope
            && member->name ? parse_symtab_lookup_in(owner->member_scope,
                core_string_borrow(member->name, strlen(member->name)))
            : nullptr;
        if (owner && member->name
            && (!field || !field->is_field || field->owner_type != owner
                || !declarations
                || !n00bb_abi_decl_find_symbol(declarations, field,
                                                &row.symbol_index))) {
            valid = false;
            break;
        }
        core_string_t visibility = field && field->visibility.u8_bytes
            ? field->visibility : owner ? owner->visibility
                                      : (core_string_t){0};
        if (core_string_eq(visibility, CORE_STRING_STATIC("public")))
            row.visibility = N00B_ABI_VISIBILITY_PUBLIC;
        if (!member->type || !n00bb_abi_type_index_find(index, member->type,
                                                         &row.child_type_index)
            || (member->name && !n00bb_abi_name_pool_encode(names,
                core_string_borrow(member->name, strlen(member->name)),
                N00B_ABI_TABLE_FIELD, (uint32_t)fields->len,
                offsetof(n00b_abi_field_v1, name), &row.name))
            || !append_accesses(member->accesses, member->n_accesses,
                final->intrinsic_bits, stream_base, packed, indexes,
                &row.packed_runs)
            || !append_runs(member->occupied, member->n_occupied,
                final->intrinsic_bits, stream_base, physical, indexes,
                &row.physical_runs)) {
            valid = false;
            break;
        }
        member_indexes[i] = (uint32_t)fields->len;
        core_list_push(*fields, row);
    }
    if (valid) {
        if (indexes->len > UINT32_MAX - stream_base
            || final->n_members > UINT32_MAX - stream_base - indexes->len)
            valid = false;
        else {
            *field_range = (n00b_abi_range_v1){
                .first = stream_base + (uint32_t)indexes->len,
                .count = (uint32_t)final->n_members,
            };
            for (size_t i = 0; i < final->n_members; i++)
                core_list_push(*indexes, member_indexes[i]);
        }
    }
    core_free(member_indexes);
    return valid;
}

bool
n00bb_abi_type_layout_rows_build(
    n00bb_abi_type_index_t *index, ir_module_t *comptime,
    ir_module_t *runtime, uint32_t stream_base,
    const n00bb_abi_decl_index_t *declarations,
    n00bb_abi_name_pool_t *names,
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
    core_list_t(n00b_abi_packed_run_v1) packed = {0};
    core_list_t(n00b_abi_field_v1) fields = {0};
    core_list_t(uint32_t) indexes = {0};
    uint32_t *type_layout_indexes = core_alloc_array(uint32_t, types->count);
    n00b_abi_range_v1 *type_field_ranges = core_alloc_array(
        n00b_abi_range_v1, types->count);
    core_list_push(layouts, ((n00b_abi_layout_v1){0}));
    core_list_push(runs, ((n00b_abi_physical_run_v1){0}));
    core_list_push(packed, ((n00b_abi_packed_run_v1){0}));
    core_list_push(fields, ((n00b_abi_field_v1){0}));
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
        if (!final) {
            if (types->owners && types->owners[i]
                && core_string_eq(types->owners[i]->adt_kind,
                                  CORE_STRING_STATIC("record")))
                valid = false;
            continue;
        }
        if (!final->alignment_bytes || final->has_discriminant
            || (final->n_members
                && types->rows[i].kind != N00B_ABI_TYPE_RECORD)
            || layouts.len >= UINT32_MAX
            || types->rows[i].size_bits != final->intrinsic_bits
            || types->rows[i].alignment_bytes != final->alignment_bytes) {
            valid = false;
            break;
        }
        n00b_abi_layout_v1 row = {
            .type_index = (uint32_t)i,
            .policy = N00B_ABI_LAYOUT_DEFAULT,
        };
        const parse_sym_entry_t *owner = types->owners
            ? types->owners[i] : nullptr;
        valid = record_policy(owner, &row.policy)
            && append_runs(final->occupied, final->n_occupied,
            final->intrinsic_bits, stream_base, &runs, &indexes,
            &row.occupied_runs)
            && append_runs(final->reserved, final->n_reserved,
                final->intrinsic_bits, stream_base, &runs, &indexes,
                &row.reserved_runs)
            && append_fields(index, declarations, names, owner, final,
                (uint32_t)i, stream_base, &fields, &packed, &runs,
                &indexes, &type_field_ranges[i]);
        if (!valid) break;
        type_layout_indexes[i] = (uint32_t)layouts.len;
        core_list_push(layouts, row);
    }
    if (valid) {
        for (size_t i = 0; i < types->count; i++) {
            types->rows[i].layout_index = type_layout_indexes[i];
            types->rows[i].fields_or_arms = type_field_ranges[i];
        }
        *out = (n00bb_abi_type_layout_rows_t){
            .layouts = layouts.data, .layout_count = layouts.len,
            .runs = runs.data, .run_count = runs.len,
            .packed_runs = packed.data, .packed_run_count = packed.len,
            .fields = fields.data, .field_count = fields.len,
            .stream_indexes = indexes.data, .stream_index_count = indexes.len,
        };
    }
    else {
        core_list_free(layouts);
        core_list_free(runs);
        core_list_free(packed);
        core_list_free(fields);
        core_list_free(indexes);
    }
    core_free(type_layout_indexes);
    core_free(type_field_ranges);
    return valid;
}

void
n00bb_abi_type_layout_rows_release(n00bb_abi_type_layout_rows_t *rows)
{
    if (!rows) return;
    core_free(rows->layouts);
    core_free(rows->runs);
    core_free(rows->packed_runs);
    core_free(rows->fields);
    core_free(rows->stream_indexes);
    *rows = (n00bb_abi_type_layout_rows_t){0};
}
