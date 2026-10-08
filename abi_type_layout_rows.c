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
core_list_decl(n00b_abi_arm_v1);
core_list_decl(n00b_abi_tag_extractor_v1);
core_list_decl(n00b_abi_trace_v1);
core_list_decl(n00b_abi_trace_child_v1);
core_list_decl(n00b_abi_trace_arm_v1);
core_list_decl(uint32_t);

typedef enum {
    TRACE_UNSEEN,
    TRACE_NONE,
    TRACE_PRESENT,
    TRACE_UNKNOWN,
    TRACE_INVALID,
    TRACE_ACTIVE,
} trace_need_t;

typedef struct {
    n00bb_abi_type_index_t *index;
    ir_module_t *module;
    const n00bb_abi_type_identity_rows_t *types;
    const n00b_abi_range_v1 *members;
    const core_list_t(n00b_abi_field_v1) *fields;
    const core_list_t(n00b_abi_arm_v1) *arms;
    const core_list_t(uint32_t) *indexes;
    uint32_t stream_base;
    uint8_t *need;
} trace_source_t;

static bool
trace_unary_component(const trace_source_t *source, uint32_t type_index,
                      uint32_t *component_index)
{
    th_type *type = n00bb_abi_type_index_at(source->index, type_index);
    if (!type) return false;
    th_type *component = nullptr;
    if (source->types->rows[type_index].kind == N00B_ABI_TYPE_WRAPPER)
        component = ir_type_definition(source->module, type);
    else {
        const th_arg *argument = th_named_nargs(type) == 1
            ? th_named_arg(type, 0) : nullptr;
        if (argument && argument->kind == TH_ARG_TYPE)
            component = argument->type;
    }
    return component && n00bb_abi_type_index_find(source->index, component,
                                                  component_index);
}

static const n00b_abi_field_v1 *
trace_member(const trace_source_t *source, uint32_t type_index,
             uint32_t ordinal)
{
    n00b_abi_range_v1 range = source->members[type_index];
    if (ordinal >= range.count || range.first < source->stream_base)
        return nullptr;
    uint32_t first = range.first - source->stream_base;
    if (first > source->indexes->len
        || range.count > source->indexes->len - first) return nullptr;
    uint32_t index = source->indexes->data[first + ordinal];
    if (!index) return nullptr;
    if (source->types->rows[type_index].kind == N00B_ABI_TYPE_VARIANT)
        return index < source->arms->len
            ? &source->arms->data[index].field : nullptr;
    return index < source->fields->len
        ? &source->fields->data[index] : nullptr;
}

static trace_need_t
trace_need(trace_source_t *source, uint32_t type_index)
{
    if (type_index >= source->types->count) return TRACE_INVALID;
    trace_need_t cached = source->need[type_index];
    if (cached) return cached == TRACE_ACTIVE ? TRACE_INVALID : cached;
    const n00b_abi_type_v1 *type = &source->types->rows[type_index];
    if (type->flags & N00B_ABI_TYPE_FLAG_NON_CONCRETE) {
        source->need[type_index] = TRACE_UNKNOWN;
        return TRACE_UNKNOWN;
    }
    trace_need_t result = TRACE_NONE;
    switch (type->kind) {
    case N00B_ABI_TYPE_REFERENCE:
    case N00B_ABI_TYPE_ARRAY:
    case N00B_ABI_TYPE_VIEW:
    case N00B_ABI_TYPE_STRING:
    case N00B_ABI_TYPE_BOX:
    case N00B_ABI_TYPE_CALLBACK:
    case N00B_ABI_TYPE_TRAIT:
    case N00B_ABI_TYPE_UNION:
        result = type->size_bits && type->alignment_bytes
            ? TRACE_PRESENT : TRACE_UNKNOWN;
        break;
    case N00B_ABI_TYPE_RECORD:
    case N00B_ABI_TYPE_TUPLE:
    case N00B_ABI_TYPE_VARIANT: {
        if (!type->layout_index) {
            result = TRACE_UNKNOWN;
            break;
        }
        source->need[type_index] = TRACE_ACTIVE;
        n00b_abi_range_v1 members = source->members[type_index];
        for (uint32_t i = 0; i < members.count; i++) {
            const n00b_abi_field_v1 *field = trace_member(source,
                type_index, i);
            trace_need_t child = field
                ? trace_need(source, field->child_type_index)
                : TRACE_INVALID;
            if (child == TRACE_INVALID || child == TRACE_UNKNOWN) {
                result = child;
                break;
            }
            if (child == TRACE_PRESENT) result = TRACE_PRESENT;
        }
        break;
    }
    case N00B_ABI_TYPE_OPTION:
    case N00B_ABI_TYPE_WRAPPER:
    case N00B_ABI_TYPE_ATOMIC:
        if (!type->layout_index) result = TRACE_UNKNOWN;
        else {
            uint32_t child_index = 0;
            result = trace_unary_component(source, type_index, &child_index)
                ? trace_need(source, child_index) : TRACE_INVALID;
        }
        break;
    case N00B_ABI_TYPE_VARIABLE:
        /* These need representation-specific tracing before a parent map
         * can safely include them. */
        result = TRACE_UNKNOWN;
        break;
    default:
        break;
    }
    source->need[type_index] = result;
    return result;
}

static bool
trace_index_append(uint32_t stream_base, core_list_t(uint32_t) *indexes,
                   uint32_t index, n00b_abi_range_v1 *range)
{
    if (indexes->len >= UINT32_MAX - stream_base
        || range->count == UINT32_MAX) return false;
    if (!range->count) range->first = stream_base + (uint32_t)indexes->len;
    core_list_push(*indexes, index);
    range->count++;
    return true;
}

static bool
trace_child_append(const trace_source_t *source,
                   uint32_t parent_type_index,
                   const n00b_abi_field_v1 *field, uint32_t stream_base,
                   core_list_t(n00b_abi_trace_child_v1) *children,
                   core_list_t(uint32_t) *indexes,
                   n00b_abi_range_v1 *range)
{
    const n00b_abi_type_v1 *parent = &source->types->rows[parent_type_index];
    const n00b_abi_type_v1 *child = &source->types->rows[
        field->child_type_index];
    uint64_t offset = field->logical_bit_offset;
    if (offset % 8 || !child->alignment_bytes
        || (offset / 8) % child->alignment_bytes
        || offset > parent->size_bits
        || child->size_bits > parent->size_bits - offset
        || children->len >= UINT32_MAX) return false;
    uint32_t index = (uint32_t)children->len;
    core_list_push(*children, ((n00b_abi_trace_child_v1){
        .byte_offset = offset / 8,
        .child_type_index = field->child_type_index,
    }));
    return trace_index_append(stream_base, indexes, index, range);
}

static bool
append_traces(trace_source_t *source,
              const core_list_t(n00b_abi_layout_v1) *layouts,
              const core_list_t(n00b_abi_tag_extractor_v1) *tags,
              uint32_t stream_base,
              core_list_t(n00b_abi_trace_v1) *traces,
              core_list_t(n00b_abi_trace_child_v1) *children,
              core_list_t(n00b_abi_trace_arm_v1) *arms,
              core_list_t(uint32_t) *indexes)
{
    core_list_push(*traces, ((n00b_abi_trace_v1){0}));
    core_list_push(*children, ((n00b_abi_trace_child_v1){0}));
    core_list_push(*arms, ((n00b_abi_trace_arm_v1){0}));
    for (uint32_t type_index = 0; type_index < source->types->count;
         type_index++) {
        n00b_abi_type_v1 *type = &source->types->rows[type_index];
        trace_need_t need = trace_need(source, type_index);
        if (need == TRACE_INVALID) return false;
        if (need != TRACE_PRESENT) continue;
        if (!type->layout_index || type->layout_index >= layouts->len
            || traces->len >= UINT32_MAX) return false;
        type->tracing_index = (uint32_t)traces->len;
        core_list_push(*traces, ((n00b_abi_trace_v1){
            .type_index = type_index,
        }));
    }
    for (uint32_t type_index = 0; type_index < source->types->count;
         type_index++) {
        n00b_abi_type_v1 *type = &source->types->rows[type_index];
        if (!type->tracing_index) continue;
        n00b_abi_trace_v1 *trace = &traces->data[type->tracing_index];
        if (type->kind == N00B_ABI_TYPE_OPTION
            || type->kind == N00B_ABI_TYPE_WRAPPER
            || type->kind == N00B_ABI_TYPE_ATOMIC) {
            uint32_t child_index = 0;
            if (!trace_unary_component(source, type_index, &child_index)
                || !source->types->rows[child_index].tracing_index)
                return false;
            trace->child_trace_index = source->types->rows[
                child_index].tracing_index;
            if (type->kind == N00B_ABI_TYPE_OPTION) {
                const n00b_abi_layout_v1 *layout = &layouts->data[
                    type->layout_index];
                uint32_t tag_index = layout->tag_extractor_index;
                if (!tag_index || tag_index >= tags->len) return false;
                const n00b_abi_tag_extractor_v1 *tag = &tags->data[tag_index];
                trace->tag_extractor_index = tag_index;
                if (tag->kind == N00B_ABI_TAG_DIRECT_BITS)
                    trace->option_bit_offset = tag->unit_byte_offset * 8
                        + tag->lsb_position;
            }
            continue;
        }
        if (type->kind != N00B_ABI_TYPE_RECORD
            && type->kind != N00B_ABI_TYPE_TUPLE
            && type->kind != N00B_ABI_TYPE_VARIANT) continue;
        bool variant = type->kind == N00B_ABI_TYPE_VARIANT;
        if (variant) {
            trace->tag_extractor_index = layouts->data[
                type->layout_index].tag_extractor_index;
            if (!trace->tag_extractor_index) return false;
        }
        for (uint32_t i = 0; i < source->members[type_index].count; i++) {
            const n00b_abi_field_v1 *field = trace_member(source,
                type_index, i);
            if (!field) return false;
            n00b_abi_range_v1 *range = &trace->fixed_children;
            n00b_abi_trace_arm_v1 arm = {0};
            if (variant) range = &arm.children;
            if (trace_need(source, field->child_type_index)
                == TRACE_PRESENT
                && !trace_child_append(source, type_index, field,
                    stream_base, children, indexes, range)) return false;
            if (variant) {
                uint32_t arm_index = source->indexes->data[
                    source->members[type_index].first
                    - source->stream_base + i];
                if (arm_index >= source->arms->len
                    || arms->len >= UINT32_MAX) return false;
                arm.tag_value = source->arms->data[arm_index].tag_value;
                core_list_push(*arms, arm);
            }
        }
        if (variant) {
            uint32_t first = (uint32_t)(arms->len
                - source->members[type_index].count);
            for (uint32_t i = first; i < arms->len; i++)
                if (!trace_index_append(stream_base, indexes, i,
                        &trace->arms)) return false;
        }
    }
    return true;
}

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
build_member_row(n00bb_abi_type_index_t *index,
                 const n00bb_abi_decl_index_t *declarations,
                 n00bb_abi_name_pool_t *names,
                 const parse_sym_entry_t *owner,
                 const ir_final_layout_t *final,
                 const ir_final_member_layout_t *member,
                 uint32_t parent_type_index, uint32_t stream_base,
                 uint32_t table_kind, uint32_t row_index,
                 core_list_t(n00b_abi_packed_run_v1) *packed,
                 core_list_t(n00b_abi_physical_run_v1) *physical,
                 core_list_t(uint32_t) *indexes,
                 n00b_abi_field_v1 *row)
{
    *row = (n00b_abi_field_v1){
            .parent_type_index = parent_type_index,
            .declaration_ordinal = member->ordinal,
            .logical_bit_offset = member->bit_offset,
            .visibility = N00B_ABI_VISIBILITY_PRIVATE,
        };
        parse_sym_entry_t *field = owner && owner->member_scope
            && member->name ? parse_symtab_lookup_in(owner->member_scope,
                core_string_borrow(member->name, strlen(member->name)))
            : nullptr;
        bool arm = table_kind == N00B_ABI_TABLE_ARM;
        if (owner && member->name
            && (!field || (arm ? (field->kind != PARSE_SYM_TAG
                                   && field->kind != PARSE_SYM_VARIABLE)
                                  : !field->is_field)
                || field->owner_type != owner
                || !declarations
                || !n00bb_abi_decl_find_symbol(declarations, field,
                                                &row->symbol_index)))
            return false;
        core_string_t visibility = field && field->visibility.u8_bytes
            ? field->visibility : owner ? owner->visibility
                                      : (core_string_t){0};
        if (core_string_eq(visibility, CORE_STRING_STATIC("public")))
            row->visibility = N00B_ABI_VISIBILITY_PUBLIC;
        else if (owner && (!visibility.u8_bytes
                 || core_string_eq(visibility, CORE_STRING_STATIC("protected"))
                 || core_string_eq(visibility, CORE_STRING_STATIC("package"))))
            row->visibility = N00B_ABI_VISIBILITY_PACKAGE;
        if (!member->type || !n00bb_abi_type_index_find(index, member->type,
                                                         &row->child_type_index)
            || (member->name && !n00bb_abi_name_pool_encode(names,
                core_string_borrow(member->name, strlen(member->name)),
                table_kind, row_index,
                offsetof(n00b_abi_field_v1, name), &row->name))
            || !append_accesses(member->accesses, member->n_accesses,
                final->intrinsic_bits, stream_base, packed, indexes,
                &row->packed_runs)
            || !append_runs(member->occupied, member->n_occupied,
                final->intrinsic_bits, stream_base, physical, indexes,
                &row->physical_runs)) return false;
    return true;
}

static bool
append_members(n00bb_abi_type_index_t *index,
               const n00bb_abi_decl_index_t *declarations,
               n00bb_abi_name_pool_t *names,
               const parse_sym_entry_t *owner,
               const ir_final_layout_t *final, uint32_t parent_type_index,
               uint32_t stream_base,
               core_list_t(n00b_abi_field_v1) *fields,
               core_list_t(n00b_abi_arm_v1) *arms,
               core_list_t(n00b_abi_packed_run_v1) *packed,
               core_list_t(n00b_abi_physical_run_v1) *physical,
               core_list_t(uint32_t) *indexes,
               n00b_abi_range_v1 *member_range)
{
    *member_range = (n00b_abi_range_v1){0};
    if (!final->n_members) return true;
    bool variant = final->has_discriminant;
    if (!final->members || !names || final->n_members > UINT32_MAX
        || (variant ? arms->len : fields->len)
             > UINT32_MAX - final->n_members)
        return false;
    uint32_t *member_indexes = core_alloc_array(uint32_t, final->n_members);
    bool valid = member_indexes != nullptr;
    for (size_t i = 0; valid && i < final->n_members; i++) {
        const ir_final_member_layout_t *member = &final->members[i];
        uint32_t row_index = (uint32_t)(variant ? arms->len : fields->len);
        n00b_abi_field_v1 row;
        if (!build_member_row(index, declarations, names, owner, final,
                              member, parent_type_index, stream_base,
                              variant ? N00B_ABI_TABLE_ARM
                                      : N00B_ABI_TABLE_FIELD,
                              row_index, packed, physical, indexes, &row)) {
            valid = false;
            break;
        }
        if (variant) {
            if (member->ordinal != i || i >= UINT32_MAX) {
                valid = false;
                break;
            }
            member_indexes[i] = (uint32_t)arms->len;
            core_list_push(*arms, ((n00b_abi_arm_v1){
                .field = row, .tag_value = (uint32_t)i + 1,
            }));
            continue;
        }
        member_indexes[i] = (uint32_t)fields->len;
        core_list_push(*fields, row);
    }
    if (valid) {
        if (indexes->len > UINT32_MAX - stream_base
            || final->n_members > UINT32_MAX - stream_base - indexes->len)
            valid = false;
        else {
            *member_range = (n00b_abi_range_v1){
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
    core_list_t(n00b_abi_arm_v1) arms = {0};
    core_list_t(n00b_abi_tag_extractor_v1) tag_extractors = {0};
    core_list_t(n00b_abi_trace_v1) traces = {0};
    core_list_t(n00b_abi_trace_child_v1) trace_children = {0};
    core_list_t(n00b_abi_trace_arm_v1) trace_arms = {0};
    core_list_t(uint32_t) indexes = {0};
    uint32_t *type_layout_indexes = core_alloc_array(uint32_t, types->count);
    n00b_abi_range_v1 *type_field_ranges = core_alloc_array(
        n00b_abi_range_v1, types->count);
    core_list_push(layouts, ((n00b_abi_layout_v1){0}));
    core_list_push(runs, ((n00b_abi_physical_run_v1){0}));
    core_list_push(packed, ((n00b_abi_packed_run_v1){0}));
    core_list_push(fields, ((n00b_abi_field_v1){0}));
    core_list_push(arms, ((n00b_abi_arm_v1){0}));
    core_list_push(tag_extractors, ((n00b_abi_tag_extractor_v1){0}));
    bool valid = true;
    for (size_t i = 0; valid && i < types->count; i++) {
        th_type *type = n00bb_abi_type_index_at(index, (uint32_t)i);
        ir_module_t *definition_owner = ir_type_definition_owner(runtime,
            type);
        if ((!types->owners || !types->owners[i])
            && definition_owner && definition_owner != runtime
            && definition_owner != comptime) continue;
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
                && (core_string_eq(types->owners[i]->adt_kind,
                                   CORE_STRING_STATIC("record"))
                    || core_string_eq(types->owners[i]->adt_kind,
                                      CORE_STRING_STATIC("variant"))
                    || (types->owners[i]->member_scope
                        && core_string_eq(
                            types->owners[i]->member_scope->scope_tag,
                            CORE_STRING_STATIC("variant")))))
                valid = false;
            continue;
        }
        bool option = types->rows[i].kind == N00B_ABI_TYPE_OPTION;
        bool wrapper = types->rows[i].kind == N00B_ABI_TYPE_WRAPPER;
        bool own_tag = types->rows[i].kind == N00B_ABI_TYPE_VARIANT
            || option;
        if (!final->alignment_bytes
            || (!wrapper && final->has_discriminant != own_tag)
            || (final->n_members
                && types->rows[i].kind != N00B_ABI_TYPE_RECORD
                && types->rows[i].kind != N00B_ABI_TYPE_VARIANT
                && types->rows[i].kind != N00B_ABI_TYPE_TUPLE
                && !wrapper)
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
        if (own_tag) {
            const ir_layout_access_t *access = &final->discriminant;
            if (access->unit_bit_offset % 8 || !access->unit_width_bits
                || !access->value_width_bits
                || access->value_width_bits > (option ? 64u : 32u)
                || access->unit_width_bits > 64
                || access->lsb >= access->unit_width_bits
                || access->value_width_bits > access->unit_width_bits - access->lsb
                || (final->discriminant_is_niche && !option)
                || tag_extractors.len >= UINT32_MAX) {
                valid = false;
                break;
            }
            uint32_t tag_kind = N00B_ABI_TAG_DIRECT_BITS;
            if (option && final->discriminant_is_niche) {
                if (final->intrinsic_bits == 64
                    && access->unit_bit_offset == 0
                    && access->unit_width_bits == 64
                    && access->value_width_bits == 64 && !access->lsb)
                    tag_kind = N00B_ABI_TAG_POINTER_NULL;
                else if (final->intrinsic_bits == 128
                    && access->unit_bit_offset == 64
                    && access->unit_width_bits == 64
                    && access->value_width_bits == 64 && !access->lsb)
                    tag_kind = N00B_ABI_TAG_BITPTR_DEPTH;
                else {
                    valid = false;
                    break;
                }
            }
            else if (option && (access->value_width_bits != 1
                || access->unit_width_bits != 8)) {
                valid = false;
                break;
            }
            row.tag_extractor_index = (uint32_t)tag_extractors.len;
            core_list_push(tag_extractors, ((n00b_abi_tag_extractor_v1){
                .unit_byte_offset = access->unit_bit_offset / 8,
                .mask = access->value_width_bits == 64 ? UINT64_MAX
                    : (UINT64_C(1) << access->value_width_bits) - 1,
                .unit_width_bits = access->unit_width_bits,
                .lsb_position = access->lsb,
                .tag_width_bits = tag_kind == N00B_ABI_TAG_POINTER_NULL
                    ? 1 : access->value_width_bits,
                .kind = tag_kind,
            }));
        }
        valid = record_policy(owner, &row.policy)
            && append_runs(final->occupied, final->n_occupied,
            final->intrinsic_bits, stream_base, &runs, &indexes,
            &row.occupied_runs)
            && append_runs(final->reserved, final->n_reserved,
                final->intrinsic_bits, stream_base, &runs, &indexes,
                &row.reserved_runs)
            && (wrapper || append_members(index, declarations, names, owner,
                final, (uint32_t)i, stream_base, &fields, &arms, &packed,
                &runs, &indexes, &type_field_ranges[i]));
        if (valid && types->rows[i].kind == N00B_ABI_TYPE_TUPLE)
            row.policy = N00B_ABI_LAYOUT_ORDERED;
        if (!valid) break;
        type_layout_indexes[i] = (uint32_t)layouts.len;
        core_list_push(layouts, row);
    }
    if (valid) {
        for (size_t i = 0; i < types->count; i++) {
            types->rows[i].layout_index = type_layout_indexes[i];
            types->rows[i].fields_or_arms = type_field_ranges[i];
        }
        uint8_t *need = core_alloc_array(uint8_t, types->count);
        valid = need != nullptr;
        if (valid) {
            memset(need, 0, types->count);
            trace_source_t source = {
                .index = index,
                .module = runtime,
                .types = types, .members = type_field_ranges,
                .fields = &fields, .arms = &arms, .indexes = &indexes,
                .stream_base = stream_base, .need = need,
            };
            valid = append_traces(&source, &layouts, &tag_extractors,
                stream_base, &traces,
                &trace_children, &trace_arms, &indexes);
        }
        core_free(need);
    }
    if (valid) {
        *out = (n00bb_abi_type_layout_rows_t){
            .layouts = layouts.data, .layout_count = layouts.len,
            .runs = runs.data, .run_count = runs.len,
            .packed_runs = packed.data, .packed_run_count = packed.len,
            .fields = fields.data, .field_count = fields.len,
            .arms = arms.data, .arm_count = arms.len,
            .tag_extractors = tag_extractors.data,
            .tag_extractor_count = tag_extractors.len,
            .traces = traces.data, .trace_count = traces.len,
            .trace_children = trace_children.data,
            .trace_child_count = trace_children.len,
            .trace_arms = trace_arms.data,
            .trace_arm_count = trace_arms.len,
            .stream_indexes = indexes.data, .stream_index_count = indexes.len,
        };
    }
    else {
        core_list_free(layouts);
        core_list_free(runs);
        core_list_free(packed);
        core_list_free(fields);
        core_list_free(arms);
        core_list_free(tag_extractors);
        core_list_free(traces);
        core_list_free(trace_children);
        core_list_free(trace_arms);
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
    core_free(rows->arms);
    core_free(rows->tag_extractors);
    core_free(rows->traces);
    core_free(rows->trace_children);
    core_free(rows->trace_arms);
    core_free(rows->stream_indexes);
    *rows = (n00bb_abi_type_layout_rows_t){0};
}
