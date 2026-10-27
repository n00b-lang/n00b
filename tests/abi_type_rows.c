#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "abi_string.h"
#include "abi_type_rows.h"
#include "abi_value_schema.h"
#include "builtin_types.h"
#include "ir/layout.h"
#include "ir/typelang.h"
#include "lib/alloc.h"

#define S(x) CORE_STRING_STATIC(x)

int
main(void)
{
    n00bb_register_builtin_types();
    n00b_abi_type_v1 reference = {
        .kind = N00B_ABI_TYPE_REFERENCE,
        .flags = N00B_ABI_TYPE_FLAG_PLAIN_REF,
    };
    size_t invalid_offset = SIZE_MAX;
    assert(n00bb_abi_row_values_valid(N00B_ABI_TABLE_TYPE,
        &reference, sizeof(reference), &invalid_offset));
    reference.kind = N00B_ABI_TYPE_PRIMITIVE;
    assert(!n00bb_abi_row_values_valid(N00B_ABI_TABLE_TYPE,
        &reference, sizeof(reference), &invalid_offset)
        && invalid_offset == offsetof(n00b_abi_type_v1, flags));

    ir_ctx_t *context = ir_ctx_new();
    const ir_profile_t profile = {.name = S("n00b")};
    ir_module_t *runtime = ir_module_new(context, S("example"), &profile,
                                          S("x86_64-linux"));
    assert(runtime);
    th_ctx *types = ir_ctx_types(context);
    th_type *string = th_name(types, S("string"));
    th_type *record = th_name(types,
        S("example.package.AVeryLongRecordTypeName"));
    ir_final_layout_t string_layout = {
        .intrinsic_bits = 128,
        .alignment_bytes = 8,
    };
    assert(ir_layout_publish_final(runtime, string, S("x86_64-linux"),
                                   &string_layout));
    n00bb_abi_type_index_t *index = n00bb_abi_type_index_new(runtime);
    uint32_t string_index = 0, record_index = 0;
    assert(index && n00bb_abi_type_index_add(index, string, &string_index)
        && n00bb_abi_type_index_add(index, record, &record_index));

    parse_symtab_t *symbols = parse_symtab_new();
    parse_tree_t *declaration = parse_tree_new_node((parse_nt_node_t){0});
    parse_sym_entry_t *owner = parse_symtab_add(symbols,
        CORE_STRING_STATIC("type"), CORE_STRING_STATIC("AVeryLongRecordTypeName"),
        PARSE_SYM_TYPEDEF, declaration);
    assert(owner);
    owner->type = record;
    owner->adt_kind = CORE_STRING_STATIC("record");
    n00bb_abi_name_pool_t *names = n00bb_abi_name_pool_new();
    n00bb_abi_type_identity_rows_t rows = {0};
    assert(n00bb_abi_type_identity_rows_build(index, runtime,
                                               false, symbols, nullptr, nullptr,
                                               names, &rows));
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
    core_string_t spelling = th_print(types, record);
    assert(decoded.length == spelling.u8_bytes
        && n00bb_abi_name_pool_size(names) == spelling.u8_bytes
        && !memcmp(n00bb_abi_name_pool_bytes(names), spelling.data,
                   spelling.u8_bytes));
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
    assert(relocation_count == 2
        && relocations[0].section_offset == 64
            + (uint64_t)record_index * sizeof(n00b_abi_type_v1)
            + offsetof(n00b_abi_type_v1, canonical_name)
        && relocations[1].section_offset == 64
            + (uint64_t)record_index * sizeof(n00b_abi_type_v1)
            + offsetof(n00b_abi_type_v1, named_constructor));
    core_free(relocations);

    n00bb_abi_type_index_t *handles = n00bb_abi_type_index_new(runtime);
    n00bb_abi_name_pool_t *handle_names = n00bb_abi_name_pool_new();
    th_arg item = {.kind = TH_ARG_TYPE, .type = string};
    const core_string_t handle_names_interned[] = {
        S(TH_OPTION), S("Atomic"), S("Array"),
    };
    const uint32_t handle_kinds[] = {
        N00B_ABI_TYPE_OPTION, N00B_ABI_TYPE_ATOMIC, N00B_ABI_TYPE_ARRAY,
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
    th_arg named_args[] = {
        {.kind = TH_ARG_TYPE, .type = string},
        {.kind = TH_ARG_UINT, .u = 7},
        {.kind = TH_ARG_SINT, .s = -3},
        {.kind = TH_ARG_STR, .str = S("fmt")},
    };
    th_type *named = th_apply(types, S("example.Dict"), named_args, 4);
    uint32_t named_index = 0;
    assert(named && n00bb_abi_type_index_add(handles, named,
                                              &named_index));
    th_type *generic_record = th_apply(types, S("example.Box"), &item, 1);
    th_member field = {.name = S("value"), .type = string};
    th_type *record_definition = th_record(types, &field, 1);
    uint32_t generic_record_index = 0;
    assert(generic_record && record_definition
        && ir_type_define(runtime, generic_record, record_definition)
        && n00bb_abi_type_index_add(handles, generic_record,
                                    &generic_record_index));
    assert(ir_layout_publish_final(runtime, bits3, S("x86_64-linux"),
                                   &bits3_layout));
    n00bb_abi_type_identity_rows_t handle_rows = {0};
    assert(n00bb_abi_type_identity_rows_build(handles, runtime,
                                               false, nullptr, nullptr, nullptr,
                                               handle_names,
                                               &handle_rows));
    for (size_t i = 0; i < sizeof(handle_kinds) / sizeof(*handle_kinds); i++)
        assert(handle_rows.rows[handle_indexes[i]].kind == handle_kinds[i]);
    assert(handle_rows.rows[bits3_index].kind == N00B_ABI_TYPE_PRIMITIVE
        && handle_rows.rows[bits3_index].semantic_width_bits == 3
        && handle_rows.rows[bits3_index].size_bits == 8
        && handle_rows.rows[bits3_index].unpacked_storage_bits == 8
        && (handle_rows.rows[bits3_index].flags
            & N00B_ABI_TYPE_FLAG_SIGNED));
    assert(handle_rows.rows[generic_record_index].kind
        == N00B_ABI_TYPE_RECORD);
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
    const n00b_abi_type_v1 *named_row = &handle_rows.rows[named_index];
    assert(named_row->kind == N00B_ABI_TYPE_NAMED
        && !named_row->layout_index && named_row->named_arguments.count == 4
        && named_row->components.count == 1
        && components.indexes[named_row->components.first - 3]
            == string_component_index);
    const n00b_abi_type_argument_v1 *args = handle_rows.arguments
        + named_row->named_arguments.first;
    assert(args[0].owner_type_index == named_index
        && args[0].kind == N00B_ABI_TYPE_ARG_TYPE
        && args[0].type_index == string_component_index
        && args[1].ordinal == 1
        && args[1].kind == N00B_ABI_TYPE_ARG_UINT
        && args[1].scalar_bits == 7
        && args[2].ordinal == 2
        && args[2].kind == N00B_ABI_TYPE_ARG_SINT
        && (int64_t)args[2].scalar_bits == -3
        && args[3].ordinal == 3
        && args[3].kind == N00B_ABI_TYPE_ARG_STRING);
    const n00b_abi_type_v1 *record_row =
        &handle_rows.rows[generic_record_index];
    assert(record_row->named_arguments.count == 1
        && handle_rows.arguments[record_row->named_arguments.first]
            .type_index == string_component_index
        && record_row->components.count == 1
        && components.indexes[record_row->components.first - 3]
            == string_component_index);
    n00bb_abi_type_components_release(&components);
    n00bb_abi_type_identity_rows_release(&handle_rows);
    n00bb_abi_name_pool_free(handle_names);
    n00bb_abi_type_index_free(handles);

    n00bb_abi_type_identity_rows_release(&rows);
    n00bb_abi_name_pool_free(names);
    parse_symtab_free(symbols);
    n00bb_abi_type_index_free(index);
    ir_ctx_free(context);
    return 0;
}
