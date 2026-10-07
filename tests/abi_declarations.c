#include <assert.h>
#include <string.h>

#include "abi_declarations.h"
#include "ir/typelang.h"
#include "lib/alloc.h"

int
main(void)
{
    const char source[] = "type Item { value: i32 }";
    core_module_spec_t spec = {0};
    core_source_file_t file = {0};
    core_translation_unit_t unit = {0};
    assert(core_module_spec_init(&spec, CORE_STRING_STATIC("example"))
        && core_source_file_init(&file, CORE_STRING_STATIC("example.n00b"),
                                 source, sizeof(source) - 1)
        && core_translation_unit_init(&unit, &spec, &file));
    core_module_spec_release(&spec);
    parse_token_info_t *first = core_alloc(parse_token_info_t);
    parse_token_info_t *second = core_alloc(parse_token_info_t);
    *first = (parse_token_info_t){
        .origin = PARSE_TOKEN_SOURCE, .line = 1, .column = 1,
        .endline = 1, .endcol = 5, .start_byte = 0, .end_byte = 4,
    };
    *second = (parse_token_info_t){
        .origin = PARSE_TOKEN_SOURCE, .line = 1, .column = 13,
        .endline = 1, .endcol = 18, .start_byte = 12, .end_byte = 17,
    };
    parse_token_info_t *tokens_array[] = {first, second};
    parse_token_stream_t *tokens = scan_token_stream_from_array(
        tokens_array, 2);
    assert(tokens);
    parse_tree_t type_declaration = {0}, field_declaration = {0};
    core_tree_node_value(&type_declaration).start = 0;
    core_tree_node_value(&type_declaration).end = 2;
    core_tree_node_value(&field_declaration).start = 1;
    core_tree_node_value(&field_declaration).end = 2;

    parse_symtab_t *symtab = parse_symtab_new();
    parse_symtab_push_scope(symtab, CORE_STRING_STATIC("type"),
                            CORE_STRING_STATIC("module"));
    parse_scope_t *module_scope = parse_symtab_current_scope(symtab,
        CORE_STRING_STATIC("type"));
    module_scope->scope_tag = CORE_STRING_STATIC("module");
    parse_sym_entry_t *type_symbol = parse_symtab_add(symtab,
        CORE_STRING_STATIC("type"), CORE_STRING_STATIC("Item"),
        PARSE_SYM_TYPEDEF, &type_declaration);
    type_symbol->adt_kind = CORE_STRING_STATIC("record");
    type_symbol->visibility = CORE_STRING_STATIC("public");
    parse_symtab_push_scope(symtab, CORE_STRING_STATIC("type"),
                            CORE_STRING_STATIC("record"));
    parse_scope_t *record_scope = parse_symtab_current_scope(symtab,
        CORE_STRING_STATIC("type"));
    record_scope->scope_tag = CORE_STRING_STATIC("type");
    parse_sym_entry_t *field_symbol = parse_symtab_add(symtab,
        CORE_STRING_STATIC("type"), CORE_STRING_STATIC("value"),
        PARSE_SYM_VARIABLE, &field_declaration);
    field_symbol->is_field = true;
    field_symbol->owner_type = type_symbol;
    ir_ctx_t *context = ir_ctx_new();
    const ir_profile_t profile = {.name = "n00b"};
    ir_module_t *module = ir_module_new(context, "example", &profile,
                                        "x86_64-linux");
    assert(module);
    th_ctx *types = ir_ctx_types(context);
    type_symbol->type = th_name(types, "example.Item");
    field_symbol->type = th_bitint(types, 32, true);
    n00bb_abi_type_index_t *type_index = n00bb_abi_type_index_new(module);
    assert(type_index && n00bb_abi_type_index_collect_symbols(type_index,
                                                                symtab));
    n00bb_abi_name_pool_t *names = n00bb_abi_name_pool_new();
    n00bb_abi_type_identity_rows_t type_rows = {0};
    assert(n00bb_abi_type_identity_rows_build(type_index, nullptr,
        module, symtab, names, &type_rows));
    n00bb_abi_decl_index_t *declarations = n00bb_abi_decl_index_new(symtab);
    n00bb_abi_declarations_t rows = {0};
    assert(declarations && n00bb_abi_declarations_build(declarations,
        type_index, &type_rows, &unit, tokens, names, 5, &rows));
    assert(rows.scope_count == 3 && rows.symbol_count == 3
        && rows.location_count == 3);
    uint32_t module_scope_index = 0, record_scope_index = 0;
    uint32_t type_symbol_index = 0, field_symbol_index = 0;
    uint32_t type_index_value = 0, field_type_index = 0;
    assert(n00bb_abi_decl_find_scope(declarations, module_scope,
                                     &module_scope_index)
        && n00bb_abi_decl_find_scope(declarations, record_scope,
                                     &record_scope_index)
        && n00bb_abi_decl_find_symbol(declarations, type_symbol,
                                      &type_symbol_index)
        && n00bb_abi_decl_find_symbol(declarations, field_symbol,
                                      &field_symbol_index)
        && n00bb_abi_type_index_find(type_index, type_symbol->type,
                                      &type_index_value)
        && n00bb_abi_type_index_find(type_index, field_symbol->type,
                                      &field_type_index));
    assert(rows.scopes[record_scope_index].parent_scope_index
            == module_scope_index
        && rows.scopes[module_scope_index].children.count == 1
        && rows.stream_indexes[
            rows.scopes[module_scope_index].children.first - 5]
            == record_scope_index
        && rows.scopes[record_scope_index].symbols.count == 1
        && rows.stream_indexes[
            rows.scopes[record_scope_index].symbols.first - 5]
            == field_symbol_index);
    assert(rows.symbols[type_symbol_index].kind == N00B_ABI_SYMBOL_TYPE
        && rows.symbols[type_symbol_index].type_index == type_index_value
        && rows.symbols[type_symbol_index].scope_index
            == module_scope_index
        && rows.symbols[type_symbol_index].visibility
            == N00B_ABI_VISIBILITY_PUBLIC
        && rows.symbols[field_symbol_index].kind == N00B_ABI_SYMBOL_FIELD
        && rows.symbols[field_symbol_index].type_index == field_type_index
        && rows.symbols[field_symbol_index].visibility
            == N00B_ABI_VISIBILITY_PUBLIC
        && type_rows.rows[type_index_value].owner_module_index == 1);
    const n00b_abi_source_location_v1 *type_location =
        &rows.locations[rows.symbols[type_symbol_index]
            .declaration_location_index];
    const n00b_abi_source_location_v1 *field_location =
        &rows.locations[rows.symbols[field_symbol_index]
            .declaration_location_index];
    assert(type_location->file_index == 1
        && type_location->start_byte_offset == 0
        && type_location->end_byte_offset == 17
        && field_location->start_byte_offset == 12
        && field_location->end_byte_offset == 17
        && field_location->start_column == 13);
    n00bb_abi_declarations_release(&rows);
    n00bb_abi_decl_index_free(declarations);
    n00bb_abi_type_identity_rows_release(&type_rows);
    n00bb_abi_name_pool_free(names);
    n00bb_abi_type_index_free(type_index);
    ir_ctx_free(context);
    parse_symtab_free(symtab);
    scan_token_stream_free(tokens);
    core_translation_unit_release(&unit);
    return 0;
}
