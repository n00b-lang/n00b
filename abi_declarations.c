#include "abi_declarations.h"

#include <limits.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/hash.h"
#include "lib/indexed_table.h"
#include "lib/list.h"

core_list_decl(n00b_abi_source_location_v1);
core_list_decl(n00b_abi_source_file_v1);
core_list_decl(uint32_t);
typedef const core_translation_unit_t *abi_source_unit_t;
core_list_decl(abi_source_unit_t);

typedef struct {
    const core_translation_unit_t *unit;
    parse_token_stream_t *tokens;
    n00bb_abi_name_pool_t *names;
    n00bb_abi_node_source_fn node_source;
    void *source_context;
    core_list_t(n00b_abi_source_file_v1) files;
    core_list_t(abi_source_unit_t) owners;
} location_context_t;

static core_hash_value_t
pointer_hash(void *value)
{
    return (core_hash_value_t)(uintptr_t)value;
}

static bool
pointer_equal(const void *left, const void *right)
{
    return left == right;
}

static bool
source_index(location_context_t *context,
             const core_translation_unit_t *unit, uint32_t *out)
{
    if (!unit || !out) return false;
    for (size_t i = 1; i < context->owners.len; i++)
        if (context->owners.data[i] == unit) {
            *out = (uint32_t)i;
            return true;
        }
    if (context->files.len >= UINT32_MAX) return false;
    uint32_t index = (uint32_t)context->files.len;
    n00b_abi_source_file_v1 row = {0};
    const char *bytes = core_source_file_contents(&unit->source);
    size_t size = core_source_file_size(&unit->source);
    row.source_identity = core_xxh3_64bits_raw(bytes ? bytes : "", size);
    if (!n00bb_abi_name_pool_encode(context->names,
            core_source_file_path(&unit->source), N00B_ABI_TABLE_SOURCE_FILE,
            index, offsetof(n00b_abi_source_file_v1, path), &row.path))
        return false;
    core_list_push(context->files, row);
    core_list_push(context->owners, unit);
    *out = index;
    return true;
}

static bool
node_location(location_context_t *context, parse_tree_t *node,
              n00b_abi_source_location_v1 *out)
{
    const core_translation_unit_t *unit = context->unit;
    parse_token_stream_t *tokens = context->tokens;
    if (context->node_source
        && !context->node_source(context->source_context, node,
                                 &unit, &tokens)) return false;
    uint32_t file_index = 0;
    if (!tokens || !source_index(context, unit, &file_index)) return false;
    parse_token_info_t *first = nullptr, *last = nullptr;
    if (parse_node_is_token(node)) first = last = parse_node_token(node);
    else {
        parse_nt_node_t value = core_tree_node_value(node);
        if (value.start < 0 || value.end <= value.start) return false;
        first = scan_stream_get(tokens, value.start);
        last = scan_stream_get(tokens, value.end - 1);
    }
    if (!first || !last || first->origin != PARSE_TOKEN_SOURCE
        || last->origin != PARSE_TOKEN_SOURCE
        || !first->line || !first->column || !last->endline
        || !last->endcol || first->start_byte > last->end_byte
        || last->end_byte > core_source_file_size(&unit->source)) return false;
    *out = (n00b_abi_source_location_v1){
        .file_index = file_index,
        .start_line = first->line, .start_column = first->column,
        .end_line = last->endline, .end_column = last->endcol,
        .start_byte_offset = first->start_byte,
        .end_byte_offset = last->end_byte,
    };
    return true;
}

static bool
add_location(core_indexed_table_t *lookup,
             core_list_t(n00b_abi_source_location_v1) *locations,
             location_context_t *context, parse_tree_t *node,
             uint32_t *out_index)
{
    if (!node) { *out_index = 0; return true; }
    const core_indexed_entry_t *existing = core_indexed_table_find(
        lookup, node);
    if (existing) { *out_index = existing->index; return true; }
    if (locations->len >= UINT32_MAX) return false;
    n00b_abi_source_location_v1 row = {0};
    if (!node_location(context, node, &row)) return false;
    existing = core_indexed_table_intern(lookup, node, node, nullptr);
    if (!existing || existing->index != locations->len) return false;
    *out_index = existing->index;
    core_list_push(*locations, row);
    return true;
}

static bool
append_index(core_list_t(uint32_t) *stream, uint32_t base, uint32_t index)
{
    if (stream->len >= UINT32_MAX - base) return false;
    core_list_push(*stream, index);
    return true;
}

static bool
append_node_range(core_indexed_table_t *lookup,
                  core_list_t(n00b_abi_source_location_v1) *locations,
                  location_context_t *context,
                  parse_tree_t *const *nodes,
                  size_t count, uint32_t stream_base,
                  core_list_t(uint32_t) *stream, n00b_abi_range_v1 *range)
{
    if (!count) return true;
    if (!nodes || stream->len > UINT32_MAX - stream_base) return false;
    size_t first = stream->len;
    for (size_t i = 0; i < count; i++) {
        uint32_t index = 0;
        if (!nodes[i] || !add_location(lookup, locations, context,
                                       nodes[i], &index)
            || !index || !append_index(stream, stream_base, index))
            return false;
    }
    *range = (n00b_abi_range_v1){
        .first = stream_base + (uint32_t)first,
        .count = (uint32_t)(stream->len - first),
    };
    return true;
}

static bool
append_group(core_list_t(uint32_t) *stream, uint32_t base,
             core_list_t(uint32_t) *group, n00b_abi_range_v1 *range)
{
    if (!group->len) return true;
    if (stream->len > UINT32_MAX - base
        || group->len > UINT32_MAX - base - stream->len) return false;
    *range = (n00b_abi_range_v1){
        .first = base + (uint32_t)stream->len,
        .count = (uint32_t)group->len,
    };
    for (size_t i = 0; i < group->len; i++)
        core_list_push(*stream, group->data[i]);
    return true;
}

static bool
symbol_kind(const parse_sym_entry_t *symbol, uint32_t *out)
{
    switch (symbol->kind) {
    case PARSE_SYM_VARIABLE:
        *out = symbol->owner_type
            && ((symbol->owner_type->member_scope
                 && core_string_eq(symbol->owner_type->member_scope->scope_tag,
                                   CORE_STRING_STATIC("variant")))
                || core_string_eq(symbol->owner_type->adt_kind,
                                  CORE_STRING_STATIC("variant")))
            ? N00B_ABI_SYMBOL_VARIANT_ARM
            : symbol->is_field ? N00B_ABI_SYMBOL_FIELD
                               : N00B_ABI_SYMBOL_VARIABLE;
        return true;
    case PARSE_SYM_FUNCTION: *out = N00B_ABI_SYMBOL_FUNCTION; return true;
    case PARSE_SYM_TYPEDEF: *out = N00B_ABI_SYMBOL_TYPE; return true;
    case PARSE_SYM_TAG: *out = N00B_ABI_SYMBOL_VARIANT_ARM; return true;
    case PARSE_SYM_ENUM_CONST: *out = N00B_ABI_SYMBOL_ENUM_ITEM; return true;
    case PARSE_SYM_LABEL: *out = N00B_ABI_SYMBOL_LABEL; return true;
    case PARSE_SYM_PARAM: *out = N00B_ABI_SYMBOL_PARAMETER; return true;
    case PARSE_SYM_MODULE: *out = N00B_ABI_SYMBOL_MODULE; return true;
    }
    return false;
}

static bool
symbol_attributes(const parse_sym_entry_t *symbol, n00b_abi_symbol_v1 *row)
{
    core_string_t visibility = symbol->visibility;
    if (!visibility.u8_bytes && symbol->owner_type)
        visibility = symbol->owner_type->visibility;
    if (visibility.u8_bytes
        && !core_string_eq(visibility, CORE_STRING_STATIC("public"))
        && !core_string_eq(visibility, CORE_STRING_STATIC("private"))
        && !core_string_eq(visibility, CORE_STRING_STATIC("protected")))
        return false;
    row->visibility = core_string_eq(visibility,
        CORE_STRING_STATIC("public")) ? N00B_ABI_VISIBILITY_PUBLIC
                                      : N00B_ABI_VISIBILITY_PRIVATE;
    row->phase_mask = (uint32_t)symbol->phases;
    row->mutable_phase_mask = (uint32_t)symbol->mutable_phases;
    row->initializer_phase_mask = (uint32_t)symbol->initializer_phases;
    row->storage_duration = (uint32_t)symbol->storage_duration;
    uint32_t valid_phases = N00B_ABI_PHASE_COMPTIME | N00B_ABI_PHASE_RUNTIME;
    if ((row->phase_mask | row->mutable_phase_mask
         | row->initializer_phase_mask) & ~valid_phases) return false;
    switch (symbol->mutability) {
    case PARSE_SYM_MUTABLE:
    case PARSE_SYM_GLOBAL:
        row->binding_kind = N00B_ABI_BINDING_VAR; break;
    case PARSE_SYM_IMMUTABLE:
        row->binding_kind = N00B_ABI_BINDING_LET; break;
    case PARSE_SYM_CONST:
        row->binding_kind = N00B_ABI_BINDING_CONST; break;
    default: return false;
    }
    if (symbol->kind == PARSE_SYM_TYPEDEF
        || symbol->kind == PARSE_SYM_FUNCTION
        || symbol->kind == PARSE_SYM_MODULE
        || symbol->kind == PARSE_SYM_LABEL)
        row->binding_kind = N00B_ABI_BINDING_NOT_APPLICABLE;
    return row->storage_duration <= N00B_ABI_DURATION_DYNAMIC;
}

bool
n00bb_abi_declarations_build(
    const n00bb_abi_decl_index_t *declarations,
    n00bb_abi_type_index_t *type_index,
    n00bb_abi_type_identity_rows_t *type_rows,
    const core_translation_unit_t *unit,
    parse_token_stream_t *tokens, n00bb_abi_name_pool_t *names,
    n00bb_abi_node_source_fn node_source, void *source_context,
    uint32_t stream_base, n00bb_abi_declarations_t *out)
{
    if (!declarations || !type_index || !type_rows || !type_rows->rows
        || type_rows->count != n00bb_abi_type_index_count(type_index)
        || !unit || !tokens || !names || !out) return false;
    size_t scope_count = n00bb_abi_decl_scope_count(declarations);
    size_t symbol_count = n00bb_abi_decl_symbol_count(declarations);
    n00b_abi_scope_v1 *scopes = core_alloc_array(n00b_abi_scope_v1,
                                                 scope_count);
    n00b_abi_symbol_v1 *symbols = core_alloc_array(n00b_abi_symbol_v1,
                                                    symbol_count);
    core_list_t(n00b_abi_source_location_v1) locations = {0};
    location_context_t sources = {
        .unit = unit, .tokens = tokens, .names = names,
        .node_source = node_source, .source_context = source_context,
    };
    core_list_t(uint32_t) stream = {0};
    core_list_t(uint32_t) *children = core_alloc_array(
        core_list_t(uint32_t), scope_count);
    core_list_t(uint32_t) *members = core_alloc_array(
        core_list_t(uint32_t), scope_count);
    core_indexed_table_t *location_index = core_indexed_table_new(
        1, UINT32_MAX, pointer_hash, pointer_equal);
    core_list_push(locations, ((n00b_abi_source_location_v1){0}));
    core_list_push(sources.files, ((n00b_abi_source_file_v1){0}));
    core_list_push(sources.owners, ((abi_source_unit_t)nullptr));
    uint32_t local_source_index = 0;
    bool valid = location_index != nullptr
        && source_index(&sources, unit, &local_source_index)
        && local_source_index == 1;
    for (size_t i = 1; valid && i < scope_count; i++) {
        parse_scope_t *scope = n00bb_abi_decl_scope_at(declarations,
                                                        (uint32_t)i);
        if (!scope) { valid = false; break; }
        n00b_abi_scope_v1 *row = &scopes[i];
        row->module_index = 1;
        if (scope->parent
            && !n00bb_abi_decl_find_scope(declarations, scope->parent,
                                            &row->parent_scope_index)
            && !core_string_eq(scope->scope_tag,
                               CORE_STRING_STATIC("module"))) {
            valid = false;
            break;
        }
        if (row->parent_scope_index)
            core_list_push(children[row->parent_scope_index], (uint32_t)i);
        core_string_t kind = scope->scope_tag.u8_bytes
            ? scope->scope_tag : scope->name.u8_bytes
                ? scope->name : CORE_STRING_STATIC("scope");
        valid = n00bb_abi_name_pool_encode(names, kind,
            N00B_ABI_TABLE_SCOPE, (uint32_t)i,
            offsetof(n00b_abi_scope_v1, kind_name), &row->kind_name)
            && add_location(location_index, &locations, &sources,
                            scope->node, &row->source_location_index);
    }
    for (size_t i = 1; valid && i < symbol_count; i++) {
        parse_sym_entry_t *symbol = n00bb_abi_decl_symbol_at(declarations,
                                                               (uint32_t)i);
        n00b_abi_symbol_v1 *row = &symbols[i];
        th_type *type = n00bb_abi_type_index_symbol_type(type_index,
                                                         symbol);
        if (!symbol || !type
            || !n00bb_abi_decl_find_scope(declarations, symbol->scope,
                                            &row->scope_index)
            || !n00bb_abi_type_index_find(type_index, type,
                                            &row->type_index)
            || !symbol_kind(symbol, &row->kind)
            || !symbol_attributes(symbol, row)) {
            valid = false;
            break;
        }
        row->module_index = 1;
        core_list_push(members[row->scope_index], (uint32_t)i);
        valid = n00bb_abi_name_pool_encode(names, symbol->name,
            N00B_ABI_TABLE_SYMBOL, (uint32_t)i,
            offsetof(n00b_abi_symbol_v1, name), &row->name)
            && add_location(location_index, &locations, &sources,
                symbol->decl_node, &row->declaration_location_index)
            && append_node_range(location_index, &locations, &sources,
                symbol->definition_nodes, symbol->definition_count,
                stream_base, &stream, &row->definitions)
            && append_node_range(location_index, &locations, &sources,
                symbol->use_nodes, symbol->use_count,
                stream_base, &stream, &row->uses);
        if (valid && symbol->selected_storage_class.u8_bytes)
            valid = n00bb_abi_name_pool_encode(names,
                symbol->selected_storage_class, N00B_ABI_TABLE_SYMBOL,
                (uint32_t)i,
                offsetof(n00b_abi_symbol_v1, storage_class_name),
                &row->storage_class_name);
    }
    for (size_t i = 1; valid && i < scope_count; i++)
        valid = append_group(&stream, stream_base, &children[i],
                &scopes[i].children)
            && append_group(&stream, stream_base, &members[i],
                &scopes[i].symbols);
    if (valid) {
        for (size_t i = 1; i < symbol_count; i++) {
            parse_sym_entry_t *symbol = n00bb_abi_decl_symbol_at(
                declarations, (uint32_t)i);
            if (symbol->kind == PARSE_SYM_TYPEDEF
                && (symbol->adt_kind.u8_bytes
                    || (symbol->member_scope
                        && (core_string_eq(
                                symbol->member_scope->scope_tag,
                                CORE_STRING_STATIC("variant"))
                            || core_string_eq(
                                symbol->member_scope->scope_tag,
                                CORE_STRING_STATIC("trait"))))))
                type_rows->rows[symbols[i].type_index].owner_module_index = 1;
        }
        *out = (n00bb_abi_declarations_t){
            .scopes = scopes, .scope_count = scope_count,
            .symbols = symbols, .symbol_count = symbol_count,
            .locations = locations.data, .location_count = locations.len,
            .source_files = sources.files.data,
            .source_file_count = sources.files.len,
            .stream_indexes = stream.data, .stream_index_count = stream.len,
        };
    }
    else {
        core_free(scopes);
        core_free(symbols);
        core_list_free(locations);
        core_list_free(sources.files);
        core_list_free(stream);
    }
    core_indexed_table_free(location_index);
    core_list_free(sources.owners);
    for (size_t i = 0; i < scope_count; i++) {
        core_list_free(children[i]);
        core_list_free(members[i]);
    }
    core_free(children);
    core_free(members);
    return valid;
}

void
n00bb_abi_declarations_release(n00bb_abi_declarations_t *rows)
{
    if (!rows) return;
    core_free(rows->scopes);
    core_free(rows->symbols);
    core_free(rows->locations);
    core_free(rows->source_files);
    core_free(rows->stream_indexes);
    *rows = (n00bb_abi_declarations_t){0};
}
