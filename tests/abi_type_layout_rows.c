#include <assert.h>
#include <stdint.h>

#include "abi_type_layout_rows.h"
#include "ir/layout.h"
#include "ir/typelang.h"
#include "record_layout_request.h"

int
main(void)
{
    ir_ctx_t *context = ir_ctx_new();
    const ir_profile_t profile = {.name = "n00b"};
    ir_module_t *runtime = ir_module_new(context, "example", &profile,
                                          "x86_64-linux");
    ir_module_t *comptime = ir_module_new(context, "example$comptime",
                                           &profile, "x86_64-linux");
    assert(runtime && comptime);
    th_ctx *types = ir_ctx_types(context);
    th_type *i3 = th_bitint(types, 3, true);
    ir_layout_bit_run_t occupied = {.offset = 0, .width = 3};
    ir_layout_bit_run_t reserved = {.offset = 3, .width = 5};
    ir_final_layout_t final = {
        .intrinsic_bits = 8, .alignment_bytes = 1,
        .occupied = &occupied, .n_occupied = 1,
        .reserved = &reserved, .n_reserved = 1,
    };
    assert(i3 && ir_layout_publish_final(runtime, i3, "x86_64-linux",
                                          &final)
        && ir_layout_publish_final(comptime, i3, "x86_64-linux", &final));
    n00bb_abi_type_index_t *index = n00bb_abi_type_index_new(runtime);
    uint32_t i3_index = 0;
    assert(index && n00bb_abi_type_index_add(index, i3, &i3_index));
    n00bb_abi_name_pool_t *names = n00bb_abi_name_pool_new();
    n00bb_abi_type_identity_rows_t identities = {0};
    assert(n00bb_abi_type_identity_rows_build(index, comptime, runtime,
                                               nullptr, nullptr, nullptr,
                                               names, &identities));
    n00bb_abi_type_layout_rows_t layouts = {0};
    assert(n00bb_abi_type_layout_rows_build(index, comptime, runtime, 1,
                                             nullptr, names, &identities,
                                             &layouts));
    assert(layouts.layout_count == 2 && layouts.run_count == 3
        && layouts.stream_index_count == 2);
    assert(identities.rows[i3_index].layout_index == 1);
    assert(layouts.layouts[1].type_index == i3_index
        && layouts.layouts[1].occupied_runs.first == 1
        && layouts.layouts[1].occupied_runs.count == 1
        && layouts.layouts[1].reserved_runs.first == 2
        && layouts.layouts[1].reserved_runs.count == 1);
    assert(layouts.stream_indexes[0] == 1
        && layouts.stream_indexes[1] == 2);
    assert(layouts.runs[1].byte_offset == 0
        && layouts.runs[1].bit_offset == 0
        && layouts.runs[1].width_bits == 3);
    assert(layouts.runs[2].byte_offset == 0
        && layouts.runs[2].bit_offset == 3
        && layouts.runs[2].width_bits == 5);
    n00bb_abi_type_layout_rows_release(&layouts);
    n00bb_abi_type_identity_rows_release(&identities);
    n00bb_abi_name_pool_free(names);
    n00bb_abi_type_index_free(index);

    th_type *u3 = th_bitint(types, 3, false);
    ir_layout_bit_run_t different_occupied = {.offset = 0, .width = 4};
    ir_layout_bit_run_t different_reserved = {.offset = 4, .width = 4};
    ir_final_layout_t different = final;
    different.occupied = &different_occupied;
    different.reserved = &different_reserved;
    assert(ir_layout_publish_final(runtime, u3, "x86_64-linux", &final)
        && ir_layout_publish_final(comptime, u3, "x86_64-linux",
                                    &different));
    index = n00bb_abi_type_index_new(runtime);
    assert(index && n00bb_abi_type_index_add(index, u3, nullptr));
    names = n00bb_abi_name_pool_new();
    identities = (n00bb_abi_type_identity_rows_t){0};
    assert(n00bb_abi_type_identity_rows_build(index, comptime, runtime,
                                               nullptr, nullptr, nullptr,
                                               names, &identities));
    layouts = (n00bb_abi_type_layout_rows_t){0};
    assert(!n00bb_abi_type_layout_rows_build(index, comptime, runtime, 1,
                                              nullptr, names, &identities,
                                              &layouts));
    assert(!layouts.layouts && !layouts.runs && !layouts.stream_indexes);
    n00bb_abi_type_identity_rows_release(&identities);
    n00bb_abi_name_pool_free(names);
    n00bb_abi_type_index_free(index);

    th_type *large = th_record(types, nullptr, 0);
    ir_layout_bit_run_t large_run = {
        .offset = 0, .width = (uint64_t)UINT32_MAX + 1,
    };
    ir_final_layout_t large_final = {
        .intrinsic_bits = large_run.width,
        .alignment_bytes = 8,
        .occupied = &large_run,
        .n_occupied = 1,
    };
    assert(large && ir_layout_publish_final(runtime, large,
                                             "x86_64-linux", &large_final));
    index = n00bb_abi_type_index_new(runtime);
    assert(index && n00bb_abi_type_index_add(index, large, nullptr));
    names = n00bb_abi_name_pool_new();
    identities = (n00bb_abi_type_identity_rows_t){0};
    assert(n00bb_abi_type_identity_rows_build(index, comptime, runtime,
                                               nullptr, nullptr, nullptr,
                                               names, &identities));
    layouts = (n00bb_abi_type_layout_rows_t){0};
    assert(n00bb_abi_type_layout_rows_build(index, comptime, runtime, 1,
                                             nullptr, names, &identities,
                                             &layouts));
    assert(layouts.run_count == 3 && layouts.stream_index_count == 2
        && layouts.runs[1].width_bits == UINT32_MAX
        && layouts.runs[2].bit_offset == 7
        && layouts.runs[2].width_bits == 1);
    n00bb_abi_type_layout_rows_release(&layouts);
    n00bb_abi_type_identity_rows_release(&identities);
    n00bb_abi_name_pool_free(names);
    n00bb_abi_type_index_free(index);

    parse_symtab_t *symbols = parse_symtab_new();
    parse_tree_t type_node = {0}, field_node = {0}, policy_node = {0};
    parse_symtab_push_scope(symbols, CORE_STRING_STATIC("type"),
                            CORE_STRING_STATIC("module"));
    parse_scope_t *module_scope = parse_symtab_current_scope(symbols,
        CORE_STRING_STATIC("type"));
    module_scope->scope_tag = CORE_STRING_STATIC("module");
    parse_sym_entry_t *owner = parse_symtab_add(symbols,
        CORE_STRING_STATIC("type"), CORE_STRING_STATIC("Item"),
        PARSE_SYM_TYPEDEF, &type_node);
    owner->type = th_name(types, "example.Item");
    owner->adt_kind = CORE_STRING_STATIC("record");
    owner->visibility = CORE_STRING_STATIC("public");
    n00bb_record_layout_request_t request = {
        .policy = N00BB_RECORD_ORDERED, .policy_node = &policy_node,
    };
    assert(parse_sym_set_binding(owner,
        CORE_STRING_STATIC("n00bb.record-layout-request"), &request));
    parse_symtab_push_scope(symbols, CORE_STRING_STATIC("type"),
                            CORE_STRING_STATIC("record"));
    owner->member_scope = parse_symtab_current_scope(symbols,
        CORE_STRING_STATIC("type"));
    parse_sym_entry_t *field = parse_symtab_add(symbols,
        CORE_STRING_STATIC("type"), CORE_STRING_STATIC("value"),
        PARSE_SYM_VARIABLE, &field_node);
    field->type = th_bitint(types, 64, true);
    field->is_field = true;
    field->owner_type = owner;
    ir_layout_bit_run_t field_bits = {.offset = 0, .width = 64};
    ir_layout_access_t access = {
        .unit_bit_offset = 0, .unit_width_bits = 64,
        .lsb = 0, .value_width_bits = 64,
    };
    ir_final_member_layout_t member = {
        .name = "value", .type = field->type, .ordinal = 0,
        .bit_offset = 0, .occupied = &field_bits, .n_occupied = 1,
        .accesses = &access, .n_accesses = 1,
    };
    ir_final_layout_t record_final = {
        .intrinsic_bits = 64, .alignment_bytes = 8,
        .occupied = &field_bits, .n_occupied = 1,
        .members = &member, .n_members = 1,
    };
    assert(ir_layout_publish_final(runtime, owner->type,
        "x86_64-linux", &record_final));
    index = n00bb_abi_type_index_new(runtime);
    uint32_t record_index = 0;
    assert(index && n00bb_abi_type_index_add(index, owner->type,
        &record_index) && n00bb_abi_type_index_add(index, field->type,
        nullptr));
    names = n00bb_abi_name_pool_new();
    identities = (n00bb_abi_type_identity_rows_t){0};
    assert(n00bb_abi_type_identity_rows_build(index, comptime, runtime,
        symbols, nullptr, nullptr, names, &identities));
    n00bb_abi_decl_index_t *declarations =
        n00bb_abi_decl_index_new(symbols);
    layouts = (n00bb_abi_type_layout_rows_t){0};
    assert(declarations && n00bb_abi_type_layout_rows_build(index,
        comptime, runtime, 1, declarations, names, &identities, &layouts));
    uint32_t field_symbol_index = 0, child_type_index = 0;
    assert(n00bb_abi_decl_find_symbol(declarations, field,
        &field_symbol_index) && n00bb_abi_type_index_find(index,
        field->type, &child_type_index));
    const n00b_abi_field_v1 *emitted = &layouts.fields[1];
    assert(layouts.field_count == 2 && layouts.packed_run_count == 2
        && layouts.run_count == 3 && layouts.stream_index_count == 4
        && identities.rows[record_index].fields_or_arms.first == 4
        && identities.rows[record_index].fields_or_arms.count == 1
        && layouts.stream_indexes[3] == 1
        && layouts.layouts[identities.rows[record_index].layout_index].policy
            == N00B_ABI_LAYOUT_ORDERED
        && emitted->parent_type_index == record_index
        && emitted->child_type_index == child_type_index
        && emitted->symbol_index == field_symbol_index
        && emitted->visibility == N00B_ABI_VISIBILITY_PUBLIC
        && emitted->logical_bit_offset == 0
        && emitted->packed_runs.first == 2
        && emitted->packed_runs.count == 1
        && emitted->physical_runs.first == 3
        && emitted->physical_runs.count == 1
        && layouts.packed_runs[1].unit_width_bits == 64
        && layouts.packed_runs[1].width_bits == 64
        && layouts.runs[2].width_bits == 64);
    n00bb_abi_type_layout_rows_release(&layouts);
    n00bb_abi_decl_index_free(declarations);
    n00bb_abi_type_identity_rows_release(&identities);
    n00bb_abi_name_pool_free(names);
    n00bb_abi_type_index_free(index);
    parse_symtab_free(symbols);
    ir_ctx_free(context);
    return 0;
}
