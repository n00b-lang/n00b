#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "abi_string.h"
#include "abi_type_rows.h"
#include "ir/layout.h"
#include "ir/typelang.h"
#include "lib/alloc.h"

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
    th_type *string = th_name(types, "string");
    th_type *record = th_name(types,
        "example.package.AVeryLongRecordTypeName");
    ir_final_layout_t string_layout = {
        .intrinsic_bits = 128,
        .alignment_bytes = 8,
    };
    assert(ir_layout_publish_final(runtime, string, "x86_64-linux",
                                   &string_layout));
    n00bb_abi_type_index_t *index = n00bb_abi_type_index_new(runtime);
    uint32_t string_index = 0, record_index = 0;
    assert(index && n00bb_abi_type_index_add(index, string, &string_index)
        && n00bb_abi_type_index_add(index, record, &record_index));

    parse_symtab_t *symbols = parse_symtab_new();
    parse_tree_t declaration = {0};
    parse_sym_entry_t *owner = parse_symtab_add(symbols,
        CORE_STRING_STATIC("type"), CORE_STRING_STATIC("AVeryLongRecordTypeName"),
        PARSE_SYM_TYPEDEF, &declaration);
    assert(owner);
    owner->type = record;
    owner->adt_kind = CORE_STRING_STATIC("record");
    n00bb_abi_name_pool_t *names = n00bb_abi_name_pool_new();
    n00bb_abi_type_identity_rows_t rows = {0};
    assert(n00bb_abi_type_identity_rows_build(index, comptime, runtime,
                                               symbols, names, &rows));
    assert(rows.count == 3 && string_index != record_index);
    assert(rows.rows[0].kind == N00B_ABI_TYPE_NIL
        && rows.rows[0].alignment_bytes == 1);
    assert(rows.rows[string_index].kind == N00B_ABI_TYPE_STRING
        && rows.rows[string_index].size_bits == 128
        && rows.rows[string_index].alignment_bytes == 8
        && rows.rows[string_index].typehash == th_hash(types, string));
    assert(rows.rows[record_index].kind == N00B_ABI_TYPE_RECORD
        && rows.rows[record_index].typehash == th_hash(types, record));
    n00bb_abi_string_header_t decoded;
    assert(n00bb_abi_string_decode(
        (const uint8_t *)&rows.rows[record_index].canonical_name,
        sizeof(n00b_abi_string_v1), &decoded));
    assert(decoded.tag == N00BB_ABI_STRING_LONG);
    const char *spelling = th_print(types, record);
    assert(decoded.length == strlen(spelling)
        && n00bb_abi_name_pool_size(names) == strlen(spelling)
        && !memcmp(n00bb_abi_name_pool_bytes(names), spelling,
                   strlen(spelling)));
    n00bb_abi_directory_entry_t directory = {
        .kind = N00B_ABI_TABLE_TYPE,
        .record_bytes = sizeof(n00b_abi_type_v1),
        .record_count = (uint32_t)rows.count,
        .file_offset = 64,
    };
    core_object_emit_absolute64_t *relocations = nullptr;
    size_t relocation_count = 0;
    assert(n00bb_abi_name_pool_relocations(names, &directory, 1,
        &relocations, &relocation_count));
    assert(relocation_count == 1
        && relocations[0].section_offset == 64
            + (uint64_t)record_index * sizeof(n00b_abi_type_v1)
            + offsetof(n00b_abi_type_v1, canonical_name));
    core_free(relocations);

    n00bb_abi_type_index_t *handles = n00bb_abi_type_index_new(runtime);
    n00bb_abi_name_pool_t *handle_names = n00bb_abi_name_pool_new();
    th_arg item = {.kind = TH_ARG_TYPE, .type = string};
    const char *handle_names_interned[] = {
        "option", "ref", "atomic", "array", "view",
    };
    const uint32_t handle_kinds[] = {
        N00B_ABI_TYPE_OPTION, N00B_ABI_TYPE_REFERENCE,
        N00B_ABI_TYPE_ATOMIC, N00B_ABI_TYPE_ARRAY, N00B_ABI_TYPE_VIEW,
    };
    uint32_t handle_indexes[sizeof(handle_kinds) / sizeof(*handle_kinds)];
    for (size_t i = 0; i < sizeof(handle_kinds) / sizeof(*handle_kinds); i++) {
        th_type *handle = th_apply(types, handle_names_interned[i], &item, 1);
        assert(handle && n00bb_abi_type_index_add(handles, handle,
                                                  &handle_indexes[i]));
    }
    th_type *bits3 = th_bitint(types, 3, true);
    uint32_t bits3_index = 0;
    ir_final_layout_t bits3_layout = {
        .intrinsic_bits = 8,
        .alignment_bytes = 1,
    };
    assert(bits3 && n00bb_abi_type_index_add(handles, bits3,
                                              &bits3_index));
    th_type *structural_ref = th_ind(types, 0, string);
    th_type *fixed_array = th_array_n(types, string, 4);
    th_type *arms[] = {string, bits3};
    th_type *alternatives = th_sum(types, TH_UNION, arms, 2);
    uint32_t ref_index = 0, array_index = 0, union_index = 0;
    assert(structural_ref && fixed_array && alternatives
        && n00bb_abi_type_index_add(handles, structural_ref, &ref_index)
        && n00bb_abi_type_index_add(handles, fixed_array, &array_index)
        && n00bb_abi_type_index_add(handles, alternatives, &union_index));
    assert(ir_layout_publish_final(runtime, bits3, "x86_64-linux",
                                   &bits3_layout));
    n00bb_abi_type_identity_rows_t handle_rows = {0};
    assert(n00bb_abi_type_identity_rows_build(handles, comptime, runtime,
                                               nullptr, handle_names,
                                               &handle_rows));
    for (size_t i = 0; i < sizeof(handle_kinds) / sizeof(*handle_kinds); i++)
        assert(handle_rows.rows[handle_indexes[i]].kind == handle_kinds[i]);
    assert(handle_rows.rows[bits3_index].kind == N00B_ABI_TYPE_PRIMITIVE
        && handle_rows.rows[bits3_index].semantic_width_bits == 3
        && handle_rows.rows[bits3_index].size_bits == 8
        && handle_rows.rows[bits3_index].unpacked_storage_bits == 8
        && (handle_rows.rows[bits3_index].flags
            & N00B_ABI_TYPE_FLAG_SIGNED));
    n00bb_abi_type_components_t components = {0};
    assert(n00bb_abi_type_components_build(handles, runtime, 3,
                                            &handle_rows, &components));
    uint32_t string_component_index = 0;
    assert(n00bb_abi_type_index_find(handles, string,
                                      &string_component_index));
    for (size_t i = 0; i < sizeof(handle_kinds) / sizeof(*handle_kinds); i++) {
        n00b_abi_range_v1 range = handle_rows.rows[handle_indexes[i]].components;
        assert(range.count == 1 && range.first >= 3
            && components.indexes[range.first - 3]
                == string_component_index);
    }
    uint32_t unary_indexes[] = {ref_index, array_index};
    for (size_t i = 0; i < sizeof(unary_indexes) / sizeof(*unary_indexes); i++) {
        n00b_abi_range_v1 range = handle_rows.rows[unary_indexes[i]].components;
        assert(range.count == 1 && range.first >= 3
            && components.indexes[range.first - 3]
                == string_component_index);
    }
    n00b_abi_range_v1 union_range = handle_rows.rows[union_index].components;
    assert(union_range.count == th_sum_len(alternatives));
    for (size_t i = 0; i < union_range.count; i++) {
        uint32_t arm_index = 0;
        assert(n00bb_abi_type_index_find(handles,
            th_sum_arm(alternatives, i), &arm_index));
        assert(components.indexes[union_range.first - 3 + i] == arm_index);
    }
    assert(handle_rows.rows[0].components.count == 0
        && handle_rows.rows[bits3_index].components.count == 0);
    n00bb_abi_type_components_release(&components);
    n00bb_abi_type_identity_rows_release(&handle_rows);
    n00bb_abi_name_pool_free(handle_names);
    n00bb_abi_type_index_free(handles);

    ir_final_layout_t conflicting = {
        .intrinsic_bits = 64,
        .alignment_bytes = 8,
    };
    assert(ir_layout_publish_final(comptime, string, "x86_64-linux",
                                   &conflicting));
    n00bb_abi_type_identity_rows_t rejected = {0};
    assert(!n00bb_abi_type_identity_rows_build(index, comptime, runtime,
                                                symbols, names, &rejected));
    assert(!rejected.rows && !rejected.count);

    n00bb_abi_type_identity_rows_release(&rows);
    n00bb_abi_name_pool_free(names);
    parse_symtab_free(symbols);
    n00bb_abi_type_index_free(index);
    ir_ctx_free(context);
    return 0;
}
