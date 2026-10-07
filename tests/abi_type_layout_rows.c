#include <assert.h>
#include <stdint.h>

#include "abi_type_layout_rows.h"
#include "ir/layout.h"
#include "ir/typelang.h"

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
                                               nullptr, names, &identities));
    n00bb_abi_type_layout_rows_t layouts = {0};
    assert(n00bb_abi_type_layout_rows_build(index, comptime, runtime, 1,
                                             &identities, &layouts));
    assert(layouts.layout_count == 2 && layouts.run_count == 3
        && layouts.run_index_count == 2);
    assert(identities.rows[i3_index].layout_index == 1);
    assert(layouts.layouts[1].type_index == i3_index
        && layouts.layouts[1].occupied_runs.first == 1
        && layouts.layouts[1].occupied_runs.count == 1
        && layouts.layouts[1].reserved_runs.first == 2
        && layouts.layouts[1].reserved_runs.count == 1);
    assert(layouts.run_indexes[0] == 1 && layouts.run_indexes[1] == 2);
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
                                               nullptr, names, &identities));
    layouts = (n00bb_abi_type_layout_rows_t){0};
    assert(!n00bb_abi_type_layout_rows_build(index, comptime, runtime, 1,
                                              &identities, &layouts));
    assert(!layouts.layouts && !layouts.runs && !layouts.run_indexes);
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
                                               nullptr, names, &identities));
    layouts = (n00bb_abi_type_layout_rows_t){0};
    assert(n00bb_abi_type_layout_rows_build(index, comptime, runtime, 1,
                                             &identities, &layouts));
    assert(layouts.run_count == 3 && layouts.run_index_count == 2
        && layouts.runs[1].width_bits == UINT32_MAX
        && layouts.runs[2].bit_offset == 7
        && layouts.runs[2].width_bits == 1);
    n00bb_abi_type_layout_rows_release(&layouts);
    n00bb_abi_type_identity_rows_release(&identities);
    n00bb_abi_name_pool_free(names);
    n00bb_abi_type_index_free(index);
    ir_ctx_free(context);
    return 0;
}
