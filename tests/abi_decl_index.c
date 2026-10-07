#include <assert.h>

#include "abi_decl_index.h"

int
main(void)
{
    parse_symtab_t *symbols = parse_symtab_new();
    parse_tree_t type_declaration = {0};
    parse_tree_t field_declaration = {0};
    assert(symbols);
    parse_sym_entry_t *builtin = parse_symtab_add(symbols,
        CORE_STRING_STATIC("builtin"), CORE_STRING_STATIC("helper"),
        PARSE_SYM_FUNCTION, nullptr);
    assert(builtin);
    parse_symtab_push_scope(symbols, CORE_STRING_STATIC("type"),
                            CORE_STRING_STATIC("module"));
    parse_scope_t *module = parse_symtab_current_scope(symbols,
        CORE_STRING_STATIC("type"));
    assert(module);
    module->scope_tag = CORE_STRING_STATIC("module");
    parse_sym_entry_t *type = parse_symtab_add(symbols,
        CORE_STRING_STATIC("type"), CORE_STRING_STATIC("Item"),
        PARSE_SYM_TYPEDEF, &type_declaration);
    assert(type);
    parse_symtab_push_scope(symbols, CORE_STRING_STATIC("type"),
                            CORE_STRING_STATIC("record"));
    parse_scope_t *record = parse_symtab_current_scope(symbols,
        CORE_STRING_STATIC("type"));
    parse_sym_entry_t *field = parse_symtab_add(symbols,
        CORE_STRING_STATIC("type"), CORE_STRING_STATIC("value"),
        PARSE_SYM_VARIABLE, &field_declaration);
    assert(record && field);
    field->owner_type = type;
    n00bb_abi_decl_index_t *index = n00bb_abi_decl_index_new(symbols);
    assert(index && n00bb_abi_decl_scope_count(index) == 3
        && n00bb_abi_decl_symbol_count(index) == 3);
    uint32_t module_row = 0, record_row = 0;
    uint32_t type_row = 0, field_row = 0, builtin_row = 0;
    assert(n00bb_abi_decl_find_scope(index, module, &module_row)
        && n00bb_abi_decl_find_scope(index, record, &record_row)
        && n00bb_abi_decl_find_symbol(index, type, &type_row)
        && n00bb_abi_decl_find_symbol(index, field, &field_row)
        && !n00bb_abi_decl_find_symbol(index, builtin, &builtin_row));
    assert(module_row == 1 && record_row == 2
        && type_row && field_row && type_row != field_row
        && n00bb_abi_decl_scope_at(index, module_row) == module
        && n00bb_abi_decl_scope_at(index, record_row) == record
        && n00bb_abi_decl_symbol_at(index, type_row) == type
        && n00bb_abi_decl_symbol_at(index, field_row) == field
        && !n00bb_abi_decl_symbol_at(index, 0));
    n00bb_abi_decl_index_free(index);
    parse_symtab_free(symbols);
    return 0;
}
