#include <assert.h>
#include <stdint.h>

#include "abi_type_index.h"
#include "ir/typelang.h"

int
main(void)
{
    ir_ctx_t *context = ir_ctx_new();
    assert(context);
    const ir_profile_t profile = {.name = "n00b"};
    ir_module_t *runtime = ir_module_new(context, "example", &profile,
                                          "x86_64-linux");
    ir_module_t *comptime = ir_module_new(context, "example$comptime",
                                           &profile, "x86_64-linux");
    assert(runtime && comptime);
    th_ctx *types = ir_ctx_types(context);
    th_type *nil = th_name(types, "nil");
    th_type *integer = th_name(types, "int");
    th_type *boolean = th_name(types, "bool");
    th_type *node = th_name(types, "example.Node");
    th_type *node_ref = th_ind(types, 0, node);
    th_member member = {.name = "next", .type = node_ref};
    th_type *record = th_record(types, &member, 1);
    assert(nil && integer && boolean && node_ref && record);
    assert(ir_type_define(runtime, node, record));

    n00bb_abi_type_index_t *index = n00bb_abi_type_index_new(runtime);
    assert(index && n00bb_abi_type_index_count(index) == 1);
    assert(ir_type_eq(runtime, n00bb_abi_type_index_at(index, 0), nil));
    uint32_t nil_index = UINT32_MAX;
    assert(n00bb_abi_type_index_find(index, nil, &nil_index));
    assert(nil_index == 0);

    uint32_t node_index = UINT32_MAX;
    assert(n00bb_abi_type_index_add(index, node, &node_index));
    assert(node_index != 0);
    uint32_t record_index = UINT32_MAX;
    uint32_t ref_index = UINT32_MAX;
    assert(!n00bb_abi_type_index_find(index, record, &record_index));
    assert(n00bb_abi_type_index_find(index, node_ref, &ref_index));
    assert(n00bb_abi_type_index_add(index, record, &record_index));
    assert(record_index != node_index && ref_index != node_index);
    size_t recursive_count = n00bb_abi_type_index_count(index);
    assert(n00bb_abi_type_index_add(index, node, &node_index));
    assert(n00bb_abi_type_index_count(index) == recursive_count);

    th_type *first_var = th_var_named(types, "value");
    th_type *second_var = th_var_named(types, "value");
    assert(first_var && second_var);
    th_type *choices[] = {integer, boolean};
    th_constraint one_of = {
        .kind = TH_CON_ONE_OF,
        .one_of = {.types = choices, .n = 2},
    };
    assert(th_var_constrain(types, first_var, one_of));
    th_param parameters[] = {{.mode = TH_IN, .type = first_var}};
    th_type *function = th_func_with_error(types, nil, boolean,
        parameters, 1, nullptr, nullptr, 0);
    assert(function);
    uint32_t function_index = UINT32_MAX;
    assert(n00bb_abi_type_index_add(index, function, &function_index));
    uint32_t first_index = UINT32_MAX, second_index = UINT32_MAX;
    uint32_t integer_index = UINT32_MAX, boolean_index = UINT32_MAX;
    assert(n00bb_abi_type_index_find(index, first_var, &first_index));
    assert(n00bb_abi_type_index_find(index, integer, &integer_index));
    assert(n00bb_abi_type_index_find(index, boolean, &boolean_index));
    assert(first_index != function_index && integer_index != boolean_index);
    assert(n00bb_abi_type_index_add(index, second_var, &second_index));
    assert(first_index != second_index);

    assert(ir_type_intern(comptime, th_array(types, integer)));
    assert(n00bb_abi_type_index_collect_ir(index, comptime));
    uint32_t array_index = UINT32_MAX;
    assert(n00bb_abi_type_index_find(index, th_array(types, integer),
                                     &array_index));
    assert(array_index != integer_index);
    th_type *contract_var = th_var_named(types, "contract_value");
    th_param concrete_param = {.mode = TH_IN, .type = integer};
    th_param generic_param = {.mode = TH_IN, .type = contract_var};
    th_type *concrete_call = th_func(types, nil, &concrete_param, 1,
                                     nullptr, nullptr, 0);
    th_type *generic_call = th_func(types, nil, &generic_param, 1,
                                    nullptr, nullptr, 0);
    ir_func_t *function_ir = ir_func_add(runtime, "identity",
                                         concrete_call, IR_LINK_INTERNAL);
    assert(function_ir && ir_func_set_callable_contract(function_ir,
                                                         generic_call));
    th_type *annotation_only = th_name(types, "example.AnnotationOnly");
    assert(ir_annot_set(context, ir_module_annots(runtime), (ir_annot_t){
        .key = "n00b.type_marker",
        .kind = IR_ANNOT_TYPE,
        .t = annotation_only,
    }));
    th_type *descriptor_only = th_name(types, "example.DescriptorOnly");
    ir_global_t *selected_type = ir_global_add(runtime, "selected_type",
                                                th_name(types, "typespec"));
    ir_global_init_value_t descriptor_value = {
        .kind = IR_GLOBAL_INIT_TYPE,
        .descriptor_type = descriptor_only,
    };
    assert(selected_type && ir_global_set_static_initializer(runtime,
        selected_type, &descriptor_value));
    assert(n00bb_abi_type_index_collect_ir(index, runtime));
    uint32_t contract_var_index = UINT32_MAX;
    uint32_t annotation_index = UINT32_MAX;
    uint32_t descriptor_index = UINT32_MAX;
    assert(n00bb_abi_type_index_find(index, contract_var,
                                     &contract_var_index));
    assert(n00bb_abi_type_index_find(index, annotation_only,
                                     &annotation_index));
    assert(n00bb_abi_type_index_find(index, descriptor_only,
                                     &descriptor_index));
    assert(contract_var_index != integer_index);
    size_t count = n00bb_abi_type_index_count(index);
    assert(n00bb_abi_type_index_collect_ir(index, comptime));
    assert(n00bb_abi_type_index_count(index) == count);

    parse_symtab_t *symbols = parse_symtab_new();
    assert(symbols);
    parse_tree_t declaration = {0};
    parse_sym_entry_t *declared = parse_symtab_add(symbols,
        CORE_STRING_STATIC(""), CORE_STRING_STATIC("from_symbol"),
        PARSE_SYM_VARIABLE, &declaration);
    parse_sym_entry_t *unused_builtin = parse_symtab_add(symbols,
        CORE_STRING_STATIC("builtin"), CORE_STRING_STATIC("unused"),
        PARSE_SYM_FUNCTION, nullptr);
    parse_sym_entry_t *module_alias = parse_symtab_add(symbols,
        CORE_STRING_STATIC(""), CORE_STRING_STATIC("dependency"),
        PARSE_SYM_MODULE, &declaration);
    parse_sym_entry_t *synthetic = parse_symtab_add(symbols,
        CORE_STRING_STATIC(""), CORE_STRING_STATIC("generated"),
        PARSE_SYM_VARIABLE, &declaration);
    assert(declared && unused_builtin && module_alias && synthetic);
    th_type *string = th_name(types, "string");
    th_type *unused = th_name(types, "only_unused_builtin");
    th_type *module_placeholder = th_var_named(types, "module_placeholder");
    th_type *synthetic_type = th_name(types, "only_synthetic");
    declared->type = string;
    unused_builtin->type = unused;
    module_alias->type = module_placeholder;
    synthetic->type = synthetic_type;
    synthetic->is_synthetic = true;
    assert(n00bb_abi_type_index_collect_symbols(index, symbols));
    uint32_t string_index = UINT32_MAX;
    uint32_t missing_index = UINT32_MAX;
    assert(n00bb_abi_type_index_find(index, string, &string_index));
    assert(!n00bb_abi_type_index_find(index, unused, &missing_index));
    assert(!n00bb_abi_type_index_find(index, module_placeholder,
                                      &missing_index));
    assert(!n00bb_abi_type_index_find(index, synthetic_type,
                                      &missing_index));
    parse_symtab_free(symbols);

    n00bb_abi_type_index_free(index);
    ir_ctx_free(context);
    return 0;
}
