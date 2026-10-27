#include <assert.h>
#include <string.h>

#include "abi_declarations.h"
#include "ir/typelang.h"
#include "lib/alloc.h"

typedef struct {
    parse_tree_t *foreign_node;
    const core_translation_unit_t *foreign_unit;
    parse_token_stream_t *foreign_tokens;
    const core_translation_unit_t *local_unit;
    parse_token_stream_t *local_tokens;
} test_sources_t;

static bool
source_of_node(void *context, parse_tree_t *node,
               const core_translation_unit_t **unit,
               parse_token_stream_t **tokens)
{
    test_sources_t *sources = context;
    bool foreign = node == sources->foreign_node;
    *unit = foreign ? sources->foreign_unit : sources->local_unit;
    *tokens = foreign ? sources->foreign_tokens : sources->local_tokens;
    return true;
}

static parse_token_info_t *
source_token(size_t start, size_t end, uint32_t line, uint32_t column,
             uint32_t endcol)
{
    parse_token_info_t *token = parse_token_new();
    parse_token_set_origin(token, PARSE_TOKEN_SOURCE);
    parse_token_set_span(token, start, end);
    parse_token_set_position(token, line, column, line, endcol);
    return token;
}

int
main(void)
{
    const char source[] = "record Item { value: i32 }";
    core_module_spec_t spec = {0};
    core_source_file_t file = {0};
    core_translation_unit_t unit = {0};
    assert(core_module_spec_init(&spec, CORE_STRING_STATIC("example"))
        && core_source_file_init(&file, CORE_STRING_STATIC("example.n00b"),
                                 core_string_borrow(source,
                                                    sizeof(source) - 1))
        && core_translation_unit_init(&unit, &spec, &file));
    core_module_spec_release(&spec);
    parse_token_info_t *first = source_token(0, 6, 1, 1, 7);
    parse_token_info_t *second = source_token(14, 19, 1, 15, 20);
    parse_token_info_t *tokens_array[] = {first, second};
    parse_token_stream_t *tokens = scan_token_stream_from_array(
        tokens_array, 2);
    assert(tokens);
    parse_tree_t *type_declaration = parse_tree_new_node((parse_nt_node_t){0});
    parse_tree_t *field_declaration = parse_tree_new_node((parse_nt_node_t){0});
    parse_tree_t *foreign_use = parse_tree_new_node((parse_nt_node_t){0});
    core_tree_node_value(type_declaration).start = 0;
    core_tree_node_value(type_declaration).end = 2;
    core_tree_node_value(field_declaration).start = 1;
    core_tree_node_value(field_declaration).end = 2;
    core_tree_node_value(foreign_use).start = 0;
    core_tree_node_value(foreign_use).end = 1;

    const char foreign_text[] = "value";
    core_module_spec_t foreign_spec = {0};
    core_source_file_t foreign_file = {0};
    core_translation_unit_t foreign_unit = {0};
    assert(core_module_spec_init(&foreign_spec,
        CORE_STRING_STATIC("consumer"))
        && core_source_file_init(&foreign_file,
            CORE_STRING_STATIC("consumer.n00b"),
            core_string_borrow(foreign_text, sizeof(foreign_text) - 1))
        && core_translation_unit_init(&foreign_unit, &foreign_spec,
            &foreign_file));
    core_module_spec_release(&foreign_spec);
    parse_token_info_t *foreign_token = source_token(0, 5, 1, 1, 6);
    parse_token_info_t *foreign_tokens_array[] = {foreign_token};
    parse_token_stream_t *foreign_tokens = scan_token_stream_from_array(
        foreign_tokens_array, 1);
    assert(foreign_tokens);
    test_sources_t sources = {
        .foreign_node = foreign_use, .foreign_unit = &foreign_unit,
        .foreign_tokens = foreign_tokens, .local_unit = &unit,
        .local_tokens = tokens,
    };

    parse_symtab_t *symtab = parse_symtab_new();
    parse_symtab_push_scope(symtab, CORE_STRING_STATIC("type"),
                            CORE_STRING_STATIC("module"));
    parse_scope_t *module_scope = parse_symtab_current_scope(symtab,
        CORE_STRING_STATIC("type"));
    module_scope->scope_tag = CORE_STRING_STATIC("module");
    parse_sym_entry_t *type_symbol = parse_symtab_add(symtab,
        CORE_STRING_STATIC("type"), CORE_STRING_STATIC("Item"),
        PARSE_SYM_TYPEDEF, type_declaration);
    type_symbol->adt_kind = CORE_STRING_STATIC("record");
    type_symbol->visibility = CORE_STRING_STATIC("public");
    parse_symtab_push_scope(symtab, CORE_STRING_STATIC("type"),
                            CORE_STRING_STATIC("record"));
    parse_scope_t *record_scope = parse_symtab_current_scope(symtab,
        CORE_STRING_STATIC("type"));
    record_scope->scope_tag = CORE_STRING_STATIC("type");
    parse_sym_entry_t *field_symbol = parse_symtab_add(symtab,
        CORE_STRING_STATIC("type"), CORE_STRING_STATIC("value"),
        PARSE_SYM_VARIABLE, field_declaration);
    field_symbol->is_field = true;
    field_symbol->owner_type = type_symbol;
    assert(parse_sym_add_use(field_symbol, foreign_use));
    ir_ctx_t *context = ir_ctx_new();
    const ir_profile_t profile = {.name = CORE_STRING_STATIC("n00b")};
    ir_module_t *module = ir_module_new(context,
                                        CORE_STRING_STATIC("example"),
                                        &profile,
                                        CORE_STRING_STATIC("x86_64-linux"));
    assert(module);
    th_ctx *types = ir_ctx_types(context);
    type_symbol->type = th_name(types, CORE_STRING_STATIC("example.Item"));
    field_symbol->type = th_bitint(types, 32, true);
    n00bb_abi_type_index_t *type_index = n00bb_abi_type_index_new(module);
    assert(type_index && n00bb_abi_type_index_collect_symbols(type_index,
                                             symtab, nullptr, nullptr));
    n00bb_abi_name_pool_t *names = n00bb_abi_name_pool_new();
    n00bb_abi_type_identity_rows_t type_rows = {0};
    assert(n00bb_abi_type_identity_rows_build(type_index,
        module, false, symtab, nullptr, nullptr, names, &type_rows));
    n00bb_abi_decl_index_t *declarations = n00bb_abi_decl_index_new(symtab);
    n00bb_abi_declarations_t rows = {0};
    assert(declarations && n00bb_abi_declarations_build(declarations,
        type_index, &type_rows, &unit, tokens, names,
        source_of_node, &sources, 5, &rows));
    assert(rows.scope_count == 3 && rows.symbol_count == 3
        && rows.location_count == 3 && rows.source_file_count == 2);
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
        && (rows.symbols[type_symbol_index].flags
            & N00B_ABI_SYMBOL_FLAG_EXPORTED)
        && rows.symbols[field_symbol_index].kind == N00B_ABI_SYMBOL_FIELD
        && rows.symbols[field_symbol_index].type_index == field_type_index
        && rows.symbols[field_symbol_index].visibility
            == N00B_ABI_VISIBILITY_PUBLIC
        && !(rows.symbols[field_symbol_index].flags
            & N00B_ABI_SYMBOL_FLAG_EXPORTED)
        && type_rows.rows[type_index_value].owner_module_index == 1);
    const n00b_abi_source_location_v1 *type_location =
        &rows.locations[rows.symbols[type_symbol_index]
            .declaration_location_index];
    const n00b_abi_source_location_v1 *field_location =
        &rows.locations[rows.symbols[field_symbol_index]
            .declaration_location_index];
    assert(type_location->file_index == 1
        && type_location->start_byte_offset == 0
        && type_location->end_byte_offset == 19
        && field_location->start_byte_offset == 14
        && field_location->end_byte_offset == 19
        && field_location->start_column == 15);
    assert(!rows.symbols[field_symbol_index].uses.count);
    n00bb_abi_declarations_release(&rows);
    type_symbol->visibility = (core_string_t){0};
    assert(n00bb_abi_declarations_build(declarations, type_index,
        &type_rows, &unit, tokens, names, source_of_node, &sources, 5, &rows)
        && rows.symbols[type_symbol_index].visibility
            == N00B_ABI_VISIBILITY_PUBLIC
        && rows.symbols[field_symbol_index].visibility
            == N00B_ABI_VISIBILITY_PUBLIC
        && (rows.symbols[type_symbol_index].flags
            & N00B_ABI_SYMBOL_FLAG_EXPORTED));
    n00bb_abi_declarations_release(&rows);
    type_symbol->visibility = CORE_STRING_STATIC("private");
    assert(n00bb_abi_declarations_build(declarations, type_index,
        &type_rows, &unit, tokens, names, source_of_node, &sources, 5, &rows)
        && rows.symbols[type_symbol_index].visibility
            == N00B_ABI_VISIBILITY_PRIVATE
        && rows.symbols[field_symbol_index].visibility
            == N00B_ABI_VISIBILITY_PRIVATE
        && !(rows.symbols[type_symbol_index].flags
            & N00B_ABI_SYMBOL_FLAG_EXPORTED));
    n00bb_abi_declarations_release(&rows);
    type_symbol->visibility = (core_string_t){0};
    field_symbol->visibility = CORE_STRING_STATIC("private");
    assert(n00bb_abi_declarations_build(declarations, type_index,
        &type_rows, &unit, tokens, names, source_of_node, &sources, 5, &rows)
        && rows.symbols[type_symbol_index].visibility
            == N00B_ABI_VISIBILITY_PUBLIC
        && rows.symbols[field_symbol_index].visibility
            == N00B_ABI_VISIBILITY_PRIVATE);
    n00bb_abi_declarations_release(&rows);
    n00bb_abi_decl_index_free(declarations);
    n00bb_abi_type_identity_rows_release(&type_rows);
    n00bb_abi_name_pool_free(names);
    n00bb_abi_type_index_free(type_index);
    ir_ctx_free(context);
    parse_symtab_free(symtab);
    scan_token_stream_free(tokens);
    scan_token_stream_free(foreign_tokens);
    core_translation_unit_release(&unit);
    core_translation_unit_release(&foreign_unit);
    return 0;
}
