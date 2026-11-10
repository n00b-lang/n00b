#include "compiler_system.h"
#include "language_hooks.h"
#include "lower/lower_internal.h"

typedef enum : uint8_t {
    UNUSED_FUNCTION,
    UNUSED_TYPE,
    UNUSED_GLOBAL,
    UNUSED_FIELD,
} unused_kind_t;

enum : uint8_t {
    UNUSED_PRIVATE = 1u << 0,
    UNUSED_ROOT = 1u << 1,
    UNUSED_ENTRY = 1u << 2,
    UNUSED_SHARED_OUTSIDE = 1u << 3,
    UNUSED_HIDDEN = 1u << 4,
};

enum : uint8_t {
    UNUSED_READ = 1u << 0,
    UNUSED_WRITE = 1u << 1,
};

typedef struct {
    core_translation_unit_t *unit;
    unused_kind_t kind;
    uint8_t flags;
    core_string_t key;
    core_string_t name;
    core_string_t owner;
    core_string_t owner_name;
    core_string_t link;
    core_diag_location_t at;
    bool live;
    uint8_t access;
    size_t first_use;
} unused_item_t;

typedef struct {
    core_string_t item;
    core_string_t context;
    uint8_t access;
    size_t next;
} unused_use_t;

core_list_decl(unused_item_t);
core_list_decl(unused_use_t);

typedef struct {
    core_buffer_t items;
    core_buffer_t uses;
    uint32_t item_count;
    uint32_t use_count;
} unit_facts_t;

typedef struct {
    n00bb_link_analysis_t *analysis;
    parse_grammar_t *grammar;
    core_translation_unit_t *root;
    core_dict_t units;
    unit_facts_t *facts;
    core_dict_t contexts;
    core_dict_t keys;
    core_dict_t implementations;
    core_buffer_t scratch;
} recorder_t;

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t at;
    bool failed;
} facts_reader_t;

static void
put_text(core_buffer_t *out, core_string_t text)
{
    append_u32_le(out, (uint32_t)text.u8_bytes);
    if (text.u8_bytes) core_buffer_append(out, text.data, text.u8_bytes);
}

static void
put_location(core_buffer_t *out, core_diag_location_t at)
{
    append_u32_le(out, at.has_location ? at.line : 0);
    append_u32_le(out, at.column);
    append_u32_le(out, at.end_line);
    append_u32_le(out, at.end_column);
}

static uint32_t
read_word(facts_reader_t *reader)
{
    if (reader->failed || reader->length - reader->at < 4) {
        reader->failed = true;
        return 0;
    }
    uint32_t value = read_u32_le(reader->data + reader->at);
    reader->at += 4;
    return value;
}

static uint8_t
read_byte(facts_reader_t *reader)
{
    if (reader->failed || reader->length == reader->at) {
        reader->failed = true;
        return 0;
    }
    return reader->data[reader->at++];
}

static core_string_t
read_text(facts_reader_t *reader)
{
    uint32_t length = read_word(reader);
    if (reader->failed || reader->length - reader->at < length) {
        reader->failed = true;
        return (core_string_t){0};
    }
    core_string_t text = core_string_borrow(
        (const char *)reader->data + reader->at, length);
    reader->at += length;
    return text;
}

static core_diag_location_t
read_location(facts_reader_t *reader)
{
    core_diag_location_t at = {0};
    at.line = read_word(reader);
    at.column = read_word(reader);
    at.end_line = read_word(reader);
    at.end_column = read_word(reader);
    at.has_location = at.line != 0;
    return at;
}

static void
put_link(core_buffer_t *out, const ir_module_t *module, core_string_t name)
{
    out->byte_len = 0;
    core_buffer_append_string(out, ir_module_name(module));
    core_buffer_putc(out, '|');
    core_buffer_append_string(out, name);
}

static bool
module_level(parse_scope_t *scope)
{
    return scope && scope->scope_tag.data
        && core_string_eq(scope->scope_tag, CORE_STRING_STATIC("module"));
}

static bool
record_like(parse_sym_entry_t *type)
{
    return type && (core_string_eq(type->adt_kind,
                                   CORE_STRING_STATIC("record"))
                    || core_string_eq(type->adt_kind,
                                      CORE_STRING_STATIC("object")));
}

static bool
trait_member(parse_sym_entry_t *symbol)
{
    core_string_t trait = CORE_STRING_STATIC("trait");
    return (symbol->owner_type
            && core_string_eq(symbol->owner_type->adt_kind, trait))
        || (symbol->scope && core_string_eq(symbol->scope->adt_kind, trait));
}

static bool
item_kind(parse_grammar_t *grammar, parse_sym_entry_t *symbol,
          unused_kind_t *kind)
{
    if (!symbol->decl_node) return false;
    if (symbol->kind == PARSE_SYM_FUNCTION) {
        *kind = UNUSED_FUNCTION;
        return nt_is(grammar, symbol->decl_node,
                     CORE_STRING_STATIC("func-def"));
    }
    if (symbol->kind == PARSE_SYM_TYPEDEF) {
        *kind = UNUSED_TYPE;
        return !symbol->is_synthetic && module_level(symbol->scope);
    }
    if (symbol->is_field) {
        *kind = UNUSED_FIELD;
        return symbol->kind == PARSE_SYM_VARIABLE
            && record_like(symbol->owner_type)
            && nt_is(grammar, symbol->decl_node,
                     CORE_STRING_STATIC("type-decl-one"));
    }
    *kind = UNUSED_GLOBAL;
    return !symbol->is_synthetic && module_level(symbol->scope)
        && is_module_global_symbol(grammar, symbol);
}

static bool
use_target(parse_sym_entry_t *symbol)
{
    return symbol->kind == PARSE_SYM_FUNCTION
        || symbol->kind == PARSE_SYM_TYPEDEF || symbol->is_field
        || module_level(symbol->scope);
}

static unit_facts_t *
unit_facts(recorder_t *recorder, core_translation_unit_t *unit)
{
    bool found = false;
    size_t index = (size_t)core_dict_get(&recorder->units, unit, &found);
    return found ? &recorder->facts[index] : nullptr;
}

static core_string_t
symbol_key(recorder_t *recorder, parse_sym_entry_t *symbol)
{
    bool found = false;
    core_string_t *key = core_dict_get(&recorder->keys, symbol, &found);
    if (found) return *key;
    core_buffer_t *text = &recorder->scratch;
    text->byte_len = 0;
    parse_sym_entry_t *owner = symbol->kind == PARSE_SYM_TYPEDEF
        ? nullptr : symbol->owner_type;
    if (!n00bb_declaration_site_key(recorder->grammar, symbol, owner, text)) {
        core_diag_location_t at = n00bb_symbol_location(symbol);
        n00bb_symbol_use_key(text, symbol->owner_unit, symbol->scope, symbol);
        core_buffer_printf(text, "@%u:%u", at.line, at.column);
    }
    key = core_dict_string_key_copy(core_buffer_string(text));
    core_dict_put(&recorder->keys, symbol, key);
    return *key;
}

static core_translation_unit_t *
node_unit(parse_tree_t *node)
{
    for (parse_tree_t *at = node; at; at = parse_tree_parent(at)) {
        parse_scope_t *scope = parse_tree_scope(at);
        if (scope && scope->owner_unit) return scope->owner_unit;
    }
    return nullptr;
}

static parse_sym_entry_t *
context_of(recorder_t *recorder, parse_tree_t *node)
{
    for (parse_tree_t *at = node; at; at = parse_tree_parent(at)) {
        bool found = false;
        parse_sym_entry_t *owner = core_dict_get(&recorder->contexts, at,
                                                 &found);
        if (found) return owner;
    }
    return nullptr;
}

static bool
native_root(n00bb_link_analysis_t *analysis, core_string_t module,
            core_string_t name)
{
    for (size_t r = 0; r < analysis->native_root_count; r++)
        if (native_root_matches(analysis->native_roots[r], module, name))
            return true;
    return n00bb_native_root_named(module, name) != nullptr;
}

static bool
literal_handler(n00bb_tu_state_t *state, parse_sym_entry_t *symbol)
{
    for (size_t r = 0; r < state->literal_modifiers.len; r++)
        if (symbol == state->literal_modifiers.data[r].function) return true;
    return false;
}

static bool
entry_name(core_string_t name)
{
    return core_string_eq(name, CORE_STRING_STATIC("start"))
        || core_string_eq(name, CORE_STRING_STATIC("main"))
        || core_string_eq(name, CORE_STRING_STATIC("comptime_start"));
}

static core_string_t
field_record(n00bb_tu_state_t *state, parse_sym_entry_t *owner)
{
    th_type *object = n00bb_object_type(state->types, owner->type)
        ? n00bb_reference_definition(state->types, owner->type) : nullptr;
    return object ? named_type_name(th_ind_referent(object))
                  : named_type_name(owner->type);
}

static bool
location_within(core_diag_location_t outer, core_diag_location_t at)
{
    if (!outer.has_location || !at.has_location) return false;
    bool after = at.line > outer.line
        || (at.line == outer.line && at.column >= outer.column);
    bool before = at.line < outer.end_line
        || (at.line == outer.end_line && at.column <= outer.end_column);
    return after && before;
}

static bool
declaration_failed(core_translation_unit_t *unit, parse_sym_entry_t *symbol)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_diag_location_t span = n00bb_node_span(symbol->decl_node);
    if (symbol->kind == PARSE_SYM_FUNCTION
        && core_tree_num_children(symbol->decl_node) > 2) {
        core_diag_location_t signature = n00bb_node_span(
            core_tree_child(symbol->decl_node, 2));
        span.end_line = signature.end_line;
        span.end_column = signature.end_column;
    }
    for (size_t i = 0; i < core_diagnostics_count(diagnostics); i++) {
        const core_diagnostic_t *item = core_diagnostics_at(diagnostics, i);
        if ((item->severity == CORE_DIAG_ERROR
             || item->severity == CORE_DIAG_FATAL)
            && location_within(span, item->location)) return true;
    }
    return false;
}

static void
record_item(recorder_t *recorder, unit_facts_t *facts,
            core_translation_unit_t *unit, parse_sym_entry_t *symbol,
            unused_kind_t kind)
{
    n00bb_link_analysis_t *analysis = recorder->analysis;
    n00bb_tu_state_t *state = tu_state(unit);
    core_string_t module = unit->module.identifier;
    uint8_t flags = symbol_is_private(symbol) ? UNUSED_PRIVATE : 0;
    if (n00bb_generated_source(core_source_file_path(&unit->source))
        || declaration_failed(unit, symbol))
        flags |= UNUSED_HIDDEN;
    core_buffer_t link = {0};
    core_string_t owner = {0}, owner_name = {0};
    switch (kind) {
    case UNUSED_FUNCTION: {
        if (core_dict_contains(&recorder->implementations, symbol)
            || trait_member(symbol)
            || core_string_eq(symbol->name, CORE_STRING_STATIC("__finalize"))
            || (n00bb_reserved_name(symbol->name)
                && !core_string_eq(symbol->name, CORE_STRING_STATIC("__init"))
                && n00bb_language_hook(analysis->environment, symbol->name))
            || native_root(analysis, module, symbol->name)
            || literal_handler(state, symbol))
            flags |= UNUSED_ROOT;
        if (entry_name(symbol->name)) flags |= UNUSED_ENTRY;
        ir_func_t *function = link_ir_function_for_symbol(analysis, symbol);
        if (function && function->owner)
            put_link(&link, function->owner, function->name);
        break;
    }
    case UNUSED_GLOBAL: {
        if (n00bb_native_root_named(module, symbol->name))
            flags |= UNUSED_ROOT;
        ir_global_t *global = link_ir_global_for_symbol(analysis, symbol);
        global = global ? ir_global_identity(global) : nullptr;
        if (global && global->owner)
            put_link(&link, global->owner, global->name);
        break;
    }
    case UNUSED_FIELD: {
        if (n00bb_field_shared_outside(symbol))
            flags |= UNUSED_SHARED_OUTSIDE;
        owner = symbol_key(recorder, symbol->owner_type);
        owner_name = symbol->owner_type->name;
        core_string_t record = field_record(state, symbol->owner_type);
        if (record.data) core_buffer_append_string(&link, record);
        break;
    }
    case UNUSED_TYPE:
        break;
    }
    core_buffer_t *out = &facts->items;
    core_buffer_putc(out, (char)kind);
    core_buffer_putc(out, (char)flags);
    put_text(out, symbol_key(recorder, symbol));
    put_text(out, symbol->name);
    put_text(out, owner);
    put_text(out, owner_name);
    put_text(out, core_buffer_string(&link));
    put_location(out, n00bb_symbol_location(symbol));
    facts->item_count++;
    core_buffer_release(&link);
}

static bool
in_dead_code(recorder_t *recorder, core_translation_unit_t *unit,
             parse_tree_t *node)
{
    core_list_t(n00bb_failed_region_t) *regions =
        &recorder->analysis->dead_regions;
    if (!regions->len) return false;
    core_diag_location_t at = n00bb_node_span(node);
    for (size_t r = 0; r < regions->len; r++)
        if (core_string_eq(regions->data[r].module, unit->module.identifier)
            && span_contains(regions->data[r].span, at)) return true;
    return false;
}

static void
record_use(recorder_t *recorder, parse_sym_entry_t *target,
           parse_tree_t *node, uint8_t access)
{
    core_translation_unit_t *unit = node_unit(node);
    unit_facts_t *facts = unit_facts(recorder, unit);
    if (!facts) return;
    parse_sym_entry_t *context = context_of(recorder, node);
    if (context == target || (context && context->kind == PARSE_SYM_FUNCTION
                              && in_dead_code(recorder, unit, node)))
        return;
    core_string_t context_key = context ? symbol_key(recorder, context)
                                        : (core_string_t){0};
    put_text(&facts->uses, symbol_key(recorder, target));
    put_text(&facts->uses, context_key);
    core_buffer_putc(&facts->uses, (char)access);
    facts->use_count++;
    parse_sym_entry_t *owner = target->owner_type;
    if (owner && owner->kind == PARSE_SYM_TYPEDEF && owner != context) {
        put_text(&facts->uses, symbol_key(recorder, owner));
        put_text(&facts->uses, context_key);
        core_buffer_putc(&facts->uses, (char)UNUSED_READ);
        facts->use_count++;
    }
}

static void
record_symbol_uses(recorder_t *recorder, parse_sym_entry_t *symbol)
{
    parse_sym_entry_t *target = canonical_symbol(symbol);
    if (!target) target = symbol;
    if (!target->owner_unit || !use_target(target)
        || !n00bb_same_package(target->owner_unit, recorder->root))
        return;
    for (size_t u = 0; u < symbol->use_count; u++) {
        parse_tree_t *node = symbol->use_nodes[u];
        uint8_t access = UNUSED_READ;
        bool also_reads = false;
        if (target->is_field
            && n00bb_field_use_is_write(recorder->grammar, target, node,
                                        &also_reads))
            access = UNUSED_WRITE | (also_reads ? UNUSED_READ : 0);
        record_use(recorder, target, node, access);
    }
    for (size_t d = 0; d < symbol->definition_count; d++) {
        parse_tree_t *node = symbol->definition_nodes[d];
        if (!target->is_field && target->decl_node
            && node_is_within(node, target->decl_node)) continue;
        record_use(recorder, target, node, UNUSED_WRITE);
    }
    unit_facts_t *facts = symbol == target
        ? unit_facts(recorder, target->owner_unit) : nullptr;
    if (facts && target->is_field
        && parse_sym_binding(target, N00BB_FIELD_LITERAL_SET_KEY)) {
        put_text(&facts->uses, symbol_key(recorder, target));
        put_text(&facts->uses, (core_string_t){0});
        core_buffer_putc(&facts->uses, (char)UNUSED_WRITE);
        facts->use_count++;
    }
}

static void
record_unresolved_members(recorder_t *recorder)
{
    n00bb_link_analysis_t *analysis = recorder->analysis;
    core_list_t(parse_sym_entry_ptr_t) members =
        core_list_new(parse_sym_entry_ptr_t);
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->annotations
            || !n00bb_same_package(unit, recorder->root)) continue;
        for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
             scope; scope = scope->all_next)
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope)
                if (symbol->is_field || symbol->kind == PARSE_SYM_FUNCTION)
                    core_list_push(members, symbol);
    }
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        if (!unit_facts(recorder, unit)) continue;
        n00bb_tu_state_t *state = tu_state(unit);
        for (size_t s = 0; s < state->link_use_sites.len; s++) {
            n00bb_link_use_site_t *site = &state->link_use_sites.data[s];
            if (!site->dotted_path || site->symbol
                || !site->member_name.u8_bytes
                || parse_tree_resolution(site->node).kind
                    != PARSE_NODE_RESOLUTION_NONE) continue;
            for (size_t m = 0; m < members.len; m++)
                if (core_string_eq(members.data[m]->name, site->member_name))
                    record_use(recorder, members.data[m], site->node,
                               UNUSED_READ | UNUSED_WRITE);
        }
    }
    core_list_free(members);
}

static bool
recorded_unit(core_translation_unit_t *unit, core_translation_unit_t *root)
{
    n00bb_tu_state_t *state = tu_state(unit);
    return state && state->annotations && !state->interface_module
        && n00bb_same_package(unit, root);
}

static void
record_unused_facts(n00bb_link_analysis_t *analysis,
                    core_translation_unit_t *root)
{
    recorder_t recorder = {
        .analysis = analysis,
        .grammar = analysis->grammar,
        .root = root,
        .facts = core_alloc_array(unit_facts_t, analysis->units.len + 1),
    };
    core_dict_init(&recorder.units, nullptr, nullptr);
    core_dict_init(&recorder.contexts, nullptr, nullptr);
    core_dict_init(&recorder.keys, nullptr, nullptr);
    core_dict_init(&recorder.implementations, nullptr, nullptr);
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        for (parse_scope_t *scope = state && state->annotations
                 ? state->annotations->symtab->all_scopes : nullptr;
             scope; scope = scope->all_next)
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                parse_sym_entry_t *implementation = parse_sym_binding(symbol,
                    CORE_STRING_STATIC("n00bb.dispatch-adapter"));
                if (implementation)
                    core_dict_put(&recorder.implementations, implementation,
                                  1);
            }
        if (!recorded_unit(unit, root)) continue;
        core_dict_put(&recorder.units, unit, u);
        for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
             scope; scope = scope->all_next)
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                unused_kind_t kind;
                if (item_kind(recorder.grammar, symbol, &kind)
                    && (kind == UNUSED_FUNCTION || kind == UNUSED_TYPE))
                    core_dict_put(&recorder.contexts, symbol->decl_node,
                                  symbol);
            }
    }
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        unit_facts_t *facts = unit_facts(&recorder, unit);
        if (!facts) continue;
        n00bb_tu_state_t *state = tu_state(unit);
        for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
             scope; scope = scope->all_next)
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                unused_kind_t kind;
                if (item_kind(recorder.grammar, symbol, &kind))
                    record_item(&recorder, facts, unit, symbol, kind);
            }
    }
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->annotations
            || !n00bb_same_package(unit, root)) continue;
        for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
             scope; scope = scope->all_next)
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope)
                record_symbol_uses(&recorder, symbol);
    }
    record_unresolved_members(&recorder);
    for (size_t u = 0; u < analysis->units.len; u++) {
        unit_facts_t *facts = unit_facts(&recorder, analysis->units.data[u]);
        if (!facts) continue;
        core_buffer_t *out = &tu_state(analysis->units.data[u])->unused_facts;
        out->byte_len = 0;
        append_u32_le(out, facts->item_count);
        if (facts->items.byte_len)
            core_buffer_append(out, facts->items.data, facts->items.byte_len);
        append_u32_le(out, facts->use_count);
        if (facts->uses.byte_len)
            core_buffer_append(out, facts->uses.data, facts->uses.byte_len);
    }
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_buffer_release(&recorder.facts[u].items);
        core_buffer_release(&recorder.facts[u].uses);
    }
    core_free(recorder.facts);
    core_dict_foreach(&recorder.keys, item)
        core_dict_string_key_free(item->value);
    core_dict_free(&recorder.keys);
    core_dict_free(&recorder.implementations);
    core_dict_free(&recorder.contexts);
    core_dict_free(&recorder.units);
    core_buffer_release(&recorder.scratch);
}

static bool
read_unit_facts(core_translation_unit_t *unit,
                core_list_t(unused_item_t) *items,
                core_list_t(unused_use_t) *uses)
{
    const core_buffer_t *buffer = &tu_state(unit)->unused_facts;
    facts_reader_t reader = {
        .data = (const uint8_t *)buffer->data, .length = buffer->byte_len,
    };
    uint32_t item_count = read_word(&reader);
    for (uint32_t i = 0; !reader.failed && i < item_count; i++) {
        unused_item_t item = {.unit = unit, .first_use = SIZE_MAX};
        item.kind = (unused_kind_t)read_byte(&reader);
        item.flags = read_byte(&reader);
        item.key = read_text(&reader);
        item.name = read_text(&reader);
        item.owner = read_text(&reader);
        item.owner_name = read_text(&reader);
        item.link = read_text(&reader);
        item.at = read_location(&reader);
        if (!reader.failed) core_list_push(*items, item);
    }
    uint32_t use_count = read_word(&reader);
    for (uint32_t i = 0; !reader.failed && i < use_count; i++) {
        unused_use_t use = {.next = SIZE_MAX};
        use.item = read_text(&reader);
        use.context = read_text(&reader);
        use.access = read_byte(&reader);
        if (!reader.failed) core_list_push(*uses, use);
    }
    return !reader.failed && reader.at == reader.length;
}

static void
note_link(core_dict_t *names, core_buffer_t *text, const ir_module_t *module,
          core_string_t name)
{
    if (!module) return;
    put_link(text, module, name);
    core_string_t key = core_buffer_string(text);
    if (!core_dict_contains(names, &key))
        core_dict_put(names, core_dict_string_key_copy(key), 1);
}

static void
free_names(core_dict_t *names)
{
    core_dict_foreach(names, item) core_dict_string_key_free(item->key);
    core_dict_free(names);
}

static void
note_global_accesses(n00bb_link_analysis_t *analysis, ir_func_t *function,
                     core_buffer_t *text, core_dict_t *reads,
                     core_dict_t *writes)
{
    bool initializer = n00bb_const_initializer(function)
        || annotated_true(function, CORE_STRING_STATIC("ir.initializer"));
    const ir_summary_t *summary = ir_summary_of(analysis->summaries,
                                                function);
    for (uint32_t i = 0; summary && i < ir_summary_path_count(summary); i++) {
        const ir_spath_t *path = ir_summary_path_at(summary, i);
        if ((path->root != IR_SPLACE_GLOBAL && path->root != IR_SPLACE_THREAD)
            || !path->global) continue;
        ir_global_t *global = ir_global_identity((ir_global_t *)path->global);
        if (path->flags & (IR_SPATH_READ | IR_SPATH_READS_ENTRY))
            note_link(reads, text, global->owner, global->name);
        if ((path->flags & IR_SPATH_WRITE) && !initializer)
            note_link(writes, text, global->owner, global->name);
    }
}

static core_list_t(n00bb_ir_func_ptr_t)
initializer_bodies(n00bb_link_analysis_t *analysis)
{
    core_list_t(n00bb_ir_func_ptr_t) bodies =
        core_list_new(n00bb_ir_func_ptr_t);
    for (size_t u = 0; u < analysis->units.len; u++) {
        ir_module_t *module = unit_module(tu_state(analysis->units.data[u]));
        for (size_t f = 0; module && f < ir_func_count(module); f++) {
            ir_func_t *function = ir_func_at(module, f);
            if (!annotated_true(function,
                                CORE_STRING_STATIC("ir.initializer")))
                continue;
            ir_func_t *body = n00bb_cached_body(function);
            core_list_push(bodies, body ? body : function);
        }
    }
    return bodies;
}

static bool
narrowed_argument(const ir_instr_t *call)
{
    for (uint32_t o = 1; o < call->n_operands; o++) {
        const ir_value_t *value = ir_operand_value(&call->operands[o]);
        for (uint32_t depth = 0; value && value->kind == IR_VAL_INSTR
             && value->def && depth < 8; depth++) {
            if (value->def->op == IR_OP_TYPE_NARROW) return true;
            if (value->def->op != IR_OP_COPY || value->def->n_operands != 1)
                break;
            value = ir_operand_value(&value->def->operands[0]);
        }
    }
    return false;
}

static core_list_t(n00bb_ir_func_ptr_t)
used_field_readers(n00bb_link_analysis_t *analysis)
{
    core_string_t helper = CORE_STRING_STATIC("n00bb.generated-value-method");
    core_dict_t listed;
    core_dict_init(&listed, nullptr, nullptr);
    core_list_t(n00bb_ir_func_ptr_t) used = core_list_new(n00bb_ir_func_ptr_t);
    core_list_t(n00bb_ir_func_ptr_t) initializers =
        initializer_bodies(analysis);
    for (size_t f = 0; f < analysis->emitted_functions.len
         + initializers.len; f++) {
        ir_func_t *function = f < analysis->emitted_functions.len
            ? analysis->emitted_functions.data[f]
            : initializers.data[f - analysis->emitted_functions.len];
        if (!annotated_true(function, helper)
            && core_dict_add(&listed, function, 1))
            core_list_push(used, function);
    }
    core_list_free(initializers);
    for (size_t at = 0; at < used.len; at++) {
        ir_func_t *function = used.data[at];
        for (uint32_t b = 0; b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                ir_instr_t *in = function->blocks[b]->instrs[i];
                ir_func_t *target = ir_call_function(in);
                if (target && annotated_true(target, helper)
                    && !narrowed_argument(in)
                    && core_dict_add(&listed, target, 1))
                    core_list_push(used, target);
            }
    }
    core_dict_free(&listed);
    return used;
}

static void
gather_field_uses(n00bb_link_analysis_t *analysis,
                  core_list_t(unused_item_t) *items, const size_t *fields,
                  size_t count, uint8_t *flags)
{
    core_list_t(n00bb_ir_func_ptr_t) used = used_field_readers(analysis);
    size_t groups = analysis->rule_facts->count;
    size_t *starts = core_alloc_array(size_t, groups + 1);
    size_t *fill = core_alloc_array(size_t, groups + 1);
    ir_func_t **ordered = core_alloc_array(ir_func_t *, used.len + 1);
    for (size_t f = 0; f < used.len; f++)
        starts[rule_group(analysis->rule_facts, used.data[f]) + 1]++;
    for (size_t g = 0; g < groups; g++) starts[g + 1] += starts[g];
    for (size_t f = 0; f < used.len; f++) {
        size_t group = rule_group(analysis->rule_facts, used.data[f]);
        ordered[starts[group] + fill[group]++] = used.data[f];
    }
    for (size_t g = 0; g < groups; g++) {
        if (starts[g] == starts[g + 1]) continue;
        ir_summary_facts_t *facts = ir_summary_facts_build(
            analysis->summaries, ordered + starts[g],
            starts[g + 1] - starts[g], false);
        for (size_t c = 0; c < count; c++) {
            unused_item_t *field = &items->data[fields[c]];
            flags[c] |= ir_summary_facts_field_use(facts, field->link,
                                                   field->name);
        }
        ir_summary_facts_free(facts);
    }
    core_free(ordered);
    core_free(fill);
    core_free(starts);
    core_list_free(used);
}

static void
expose_type(th_ctx *types, th_type *type, core_dict_t *exposed,
            uint32_t depth)
{
    type = type ? th_resolve(type) : nullptr;
    if (!type || depth > 32) return;
    th_type *item = th_option_item(type);
    if (item) {
        expose_type(types, item, exposed, depth + 1);
        return;
    }
    switch (th_kind_of(type)) {
    case TH_INDIRECTION:
        expose_type(types, th_ind_referent(type), exposed, depth + 1);
        return;
    case TH_SEQUENCE:
        expose_type(types, th_seq_elem(type), exposed, depth + 1);
        return;
    case TH_NAMED: {
        core_string_t name = named_type_name(type);
        if (!name.data || core_dict_contains(exposed, &name)) return;
        core_dict_put(exposed, core_dict_string_key_copy(name), 1);
        th_type *definition = th_type_definition(types, type);
        definition = definition ? th_resolve(definition) : nullptr;
        for (size_t m = 0; definition && th_kind_of(definition) == TH_RECORD
             && m < th_record_len(definition); m++)
            expose_type(types, th_record_member(definition, m)->type,
                        exposed, depth + 1);
        return;
    }
    default:
        return;
    }
}

static bool
foreign_call(const ir_instr_t *in)
{
    ir_func_t *target = n00bb_direct_call_target(in);
    return target && (target->linkage == IR_LINK_EXTERN || !target->n_blocks);
}

static void
expose_callback(th_ctx *types, ir_func_t *callback, core_dict_t *exposed)
{
    for (uint32_t p = 0; p < callback->n_params; p++)
        expose_type(types, callback->params[p]->type, exposed, 0);
    for (uint32_t b = 0; b < callback->n_blocks; b++) {
        ir_instr_t *terminator = ir_block_terminator(callback->blocks[b]);
        if (terminator && terminator->op == IR_OP_RETURN
            && terminator->n_operands)
            expose_type(types,
                ir_operand_value(&terminator->operands[0])->type, exposed,
                0);
    }
}

static void
exposed_records(n00bb_link_analysis_t *analysis, core_dict_t *exposed)
{
    for (size_t f = 0; f < analysis->emitted_functions.len; f++) {
        ir_func_t *function = analysis->emitted_functions.data[f];
        th_ctx *types = ir_module_types(function->owner);
        for (uint32_t b = 0; b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                ir_instr_t *in = function->blocks[b]->instrs[i];
                if (!foreign_call(in)) continue;
                if (in->dest) expose_type(types, in->dest->type, exposed, 0);
                for (uint32_t o = 1; o < in->n_operands; o++) {
                    ir_value_t *argument = ir_operand_value(&in->operands[o]);
                    if (!argument) continue;
                    if (argument->kind == IR_VAL_FUNC && argument->owner)
                        expose_callback(types,
                            n00bb_cached_body(argument->owner), exposed);
                    else expose_type(types, argument->type, exposed, 0);
                }
            }
    }
    for (size_t u = 0; u < analysis->units.len; u++) {
        ir_module_t *module = unit_module(tu_state(analysis->units.data[u]));
        for (size_t g = 0; module && g < ir_global_count(module); g++) {
            ir_global_t *global = ir_global_at(module, g);
            if (global->mutability == IR_MUT_CONST)
                expose_type(ir_module_types(module), global->type, exposed,
                            0);
        }
    }
}

typedef struct {
    core_list_t(unused_item_t) *items;
    core_list_t(unused_use_t) *uses;
    core_dict_t index;
    core_dict_t by_link;
    core_dict_t bodies;
    core_dict_t scanned;
    core_list_t(size_t) work;
    core_list_t(n00bb_ir_func_ptr_t) scan;
    core_buffer_t text;
} liveness_t;

static size_t
find_item(core_dict_t *index, core_string_t key)
{
    bool found = false;
    size_t at = (size_t)core_dict_get(index, &key, &found);
    return found ? at : SIZE_MAX;
}

static void
scan_function(liveness_t *liveness, ir_func_t *function)
{
    ir_func_t *body = function ? n00bb_cached_body(function) : nullptr;
    body = body ? body : function;
    if (body && core_dict_add(&liveness->scanned, body, 1))
        core_list_push(liveness->scan, body);
}

static void
mark_live(liveness_t *liveness, size_t at)
{
    if (at == SIZE_MAX || liveness->items->data[at].live) return;
    unused_item_t *item = &liveness->items->data[at];
    item->live = true;
    if (item->kind == UNUSED_TYPE) core_list_push(liveness->work, at);
    if (item->kind != UNUSED_FUNCTION) return;
    core_list_push(liveness->work, at);
    bool found = false;
    ir_func_t *function = item->link.u8_bytes
        ? core_dict_get(&liveness->bodies, &item->link, &found) : nullptr;
    if (found) scan_function(liveness, function);
}

static void
mark_linked(liveness_t *liveness, const ir_func_t *function)
{
    if (!function || !function->owner) return;
    put_link(&liveness->text, function->owner, function->name);
    core_string_t link = core_buffer_string(&liveness->text);
    mark_live(liveness, find_item(&liveness->by_link, link));
}

static void
note_callee(liveness_t *liveness, ir_func_t *target)
{
    ir_func_t *body = n00bb_cached_body(target);
    mark_linked(liveness, target);
    if (body && body != target) mark_linked(liveness, body);
    scan_function(liveness, target);
}

static void
scan_callees(liveness_t *liveness, ir_func_t *function)
{
    if (!function->n_blocks) return;
    bool *reached = core_alloc_array(bool, function->n_blocks);
    ir_block_t **blocks = core_alloc_array(ir_block_t *, function->n_blocks);
    size_t count = 0;
    ir_block_t *entry = ir_block_entry(function);
    if (entry) {
        reached[entry->index] = true;
        blocks[count++] = entry;
    }
    for (size_t b = 0; b < count; b++) {
        ir_block_t *block = blocks[b];
        ir_instr_t *terminator = ir_block_terminator(block);
        for (uint32_t o = 0; terminator && o < terminator->n_operands; o++) {
            ir_block_t *to = ir_succ_of(&terminator->operands[o]);
            if (to && !reached[to->index]) {
                reached[to->index] = true;
                blocks[count++] = to;
            }
        }
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];
            for (uint32_t o = 0; in && o < in->n_operands; o++) {
                ir_value_t *value = ir_operand_value(&in->operands[o]);
                if (value && value->kind == IR_VAL_FUNC && value->owner)
                    note_callee(liveness, value->owner);
            }
        }
    }
    core_free(blocks);
    core_free(reached);
}

static void
apply_use(liveness_t *liveness, const unused_use_t *use)
{
    size_t at = find_item(&liveness->index, use->item);
    if (at == SIZE_MAX) return;
    unused_item_t *item = &liveness->items->data[at];
    item->access |= use->access;
    if (item->kind == UNUSED_FUNCTION || item->kind == UNUSED_TYPE)
        mark_live(liveness, at);
}

static void
index_bodies(liveness_t *liveness, ir_module_t *module)
{
    for (size_t f = 0; module && f < ir_func_count(module); f++) {
        ir_func_t *function = ir_func_at(module, f);
        ir_func_t *body = n00bb_cached_body(function);
        put_link(&liveness->text, function->owner, function->name);
        core_string_t link = core_buffer_string(&liveness->text);
        if (!core_dict_contains(&liveness->bodies, &link))
            core_dict_put(&liveness->bodies,
                          core_dict_string_key_copy(link),
                          body ? body : function);
    }
}

static void
solve_liveness(n00bb_link_analysis_t *analysis, liveness_t *liveness,
               core_translation_unit_t *entry_unit, bool whole_program)
{
    core_list_t(unused_item_t) *items = liveness->items;
    core_list_t(unused_use_t) *uses = liveness->uses;
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        index_bodies(liveness, unit_module(state));
        if (state && state->interface_module)
            index_bodies(liveness, interface_body_module(analysis,
                                                         unit->module.identifier));
    }
    for (size_t i = 0; i < items->len; i++) {
        unused_item_t *item = &items->data[i];
        if (!core_dict_contains(&liveness->index, &item->key))
            core_dict_put(&liveness->index, &item->key, i);
        if (item->kind == UNUSED_FUNCTION && item->link.u8_bytes
            && !core_dict_contains(&liveness->by_link, &item->link))
            core_dict_put(&liveness->by_link, &item->link, i);
    }
    core_list_t(size_t) always = core_list_new(size_t);
    for (size_t i = 0; i < uses->len; i++) {
        if (!uses->data[i].context.u8_bytes) {
            core_list_push(always, i);
            continue;
        }
        size_t context = find_item(&liveness->index, uses->data[i].context);
        if (context == SIZE_MAX) continue;
        uses->data[i].next = items->data[context].first_use;
        items->data[context].first_use = i;
    }
    for (size_t f = 0; f < analysis->reachable_functions.len; f++) {
        ir_func_t *function = analysis->reachable_functions.data[f];
        ir_func_t *body = n00bb_cached_body(function);
        core_dict_add(&liveness->scanned, body ? body : function, 1);
    }
    for (size_t f = 0; f < analysis->reachable_functions.len; f++) {
        ir_func_t *function = analysis->reachable_functions.data[f];
        ir_func_t *body = n00bb_cached_body(function);
        mark_linked(liveness, function);
        if (body && body != function) mark_linked(liveness, body);
    }
    for (size_t i = 0; i < items->len; i++) {
        unused_item_t *item = &items->data[i];
        bool exported = !whole_program && !(item->flags & UNUSED_PRIVATE);
        if ((item->kind == UNUSED_FUNCTION || item->kind == UNUSED_TYPE)
            && ((item->flags & UNUSED_ROOT) || exported
                || ((item->flags & UNUSED_ENTRY) && item->unit == entry_unit)))
            mark_live(liveness, i);
    }
    for (size_t a = 0; a < always.len; a++)
        apply_use(liveness, &uses->data[always.data[a]]);
    core_list_free(always);
    while (liveness->work.len || liveness->scan.len) {
        if (liveness->scan.len) {
            scan_callees(liveness,
                         liveness->scan.data[--liveness->scan.len]);
            continue;
        }
        size_t at = liveness->work.data[--liveness->work.len];
        for (size_t u = items->data[at].first_use; u != SIZE_MAX;
             u = uses->data[u].next)
            apply_use(liveness, &uses->data[u]);
    }
}

static void
report_item(const unused_item_t *item, n00bb_diag_t id, n00bb_text_t text,
            n00bb_text_t help)
{
    core_diagnostics_t *diagnostics =
        core_translation_unit_diagnostics(item->unit);
    n00bb_args_t args = item->kind == UNUSED_FIELD
        ? N00BB_ARGS(ARG_TEXT(item->name), ARG_TEXT(item->owner_name))
        : N00BB_ARGS(ARG_TEXT(item->name));
    if (n00bb_report(diagnostics, item->unit, id, item->at, text, args)
        && help != N00B_TEXT_COUNT)
        n00bb_report_help(diagnostics, help, N00BB_NO_ARGS);
}

static void
report_field(const unused_item_t *item, bool read, bool written)
{
    if (read && written) return;
    report_item(item,
        written ? N00B_DIAG_FIELD_NEVER_READ
            : read ? N00B_DIAG_FIELD_NEVER_SET : N00B_DIAG_FIELD_NEVER_USED,
        written ? N00B_TEXT_FIELD_NEVER_READ
            : read ? N00B_TEXT_FIELD_OF_NEVER_SET : N00B_TEXT_FIELD_NEVER_USED,
        N00B_TEXT_COUNT);
}

static bool
internal_failure(n00bb_link_analysis_t *analysis)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(analysis->units.data[u]);
        for (size_t i = 0; i < core_diagnostics_count(diagnostics); i++)
            if (core_diagnostics_at(diagnostics, i)->severity
                == CORE_DIAG_INTERNAL) return true;
    }
    return false;
}

void
n00bb_report_unused_items(n00bb_link_analysis_t *analysis)
{
    core_translation_unit_t *root = analysis && analysis->units.len
        ? analysis->units.data[0] : nullptr;
    if (!root || internal_failure(analysis)) return;
    record_unused_facts(analysis, root);
    core_list_t(unused_item_t) items = core_list_new(unused_item_t);
    core_list_t(unused_use_t) uses = core_list_new(unused_use_t);
    bool complete = true;
    for (size_t u = 0; complete && u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->annotations
            || !n00bb_same_package(unit, root)) continue;
        complete = state->unused_facts.byte_len
            && read_unit_facts(unit, &items, &uses);
    }
    if (!complete) {
        core_list_free(uses);
        core_list_free(items);
        return;
    }
    bool whole_program = !analysis->may_link_further;
    n00bb_tu_state_t *root_state = tu_state(root);
    core_translation_unit_t *entry_unit = root_state->link
        ? core_link_unit_root(root_state->link, 0) : root;
    liveness_t liveness = {
        .items = &items,
        .uses = &uses,
        .work = core_list_new(size_t),
        .scan = core_list_new(n00bb_ir_func_ptr_t),
    };
    core_dict_init_string(&liveness.index);
    core_dict_init_string(&liveness.by_link);
    core_dict_init_string(&liveness.bodies);
    core_dict_init(&liveness.scanned, nullptr, nullptr);
    solve_liveness(analysis, &liveness, entry_unit, whole_program);
    core_list_free(liveness.scan);
    core_list_free(liveness.work);
    core_dict_free(&liveness.scanned);
    core_dict_foreach(&liveness.bodies, item)
        core_dict_string_key_free(item->key);
    core_dict_free(&liveness.bodies);
    core_dict_free(&liveness.by_link);
    core_buffer_t text = liveness.text;

    core_dict_t reads, writes, exposed;
    core_dict_init_string(&reads);
    core_dict_init_string(&writes);
    core_dict_init_string(&exposed);
    if (analysis->summaries) {
        for (size_t f = 0; f < analysis->emitted_functions.len; f++)
            note_global_accesses(analysis,
                analysis->emitted_functions.data[f], &text, &reads, &writes);
        core_list_t(n00bb_ir_func_ptr_t) initializers =
            initializer_bodies(analysis);
        for (size_t f = 0; f < initializers.len; f++)
            note_global_accesses(analysis, initializers.data[f], &text,
                                 &reads, &writes);
        core_list_free(initializers);
    }
    if (whole_program) exposed_records(analysis, &exposed);
    size_t *fields = core_alloc_array(size_t, items.len + 1);
    uint8_t *field_flags = core_alloc_array(uint8_t, items.len + 1);
    size_t field_count = 0;
    for (size_t i = 0; i < items.len; i++) {
        unused_item_t *item = &items.data[i];
        if (item->kind != UNUSED_FIELD || (item->flags & UNUSED_HIDDEN)
            || !(whole_program || (item->flags & UNUSED_PRIVATE)))
            continue;
        size_t owner = find_item(&liveness.index, item->owner);
        if (owner == SIZE_MAX || !items.data[owner].live
            || ((item->flags & UNUSED_SHARED_OUTSIDE)
                && !(whole_program && analysis->closed_world))
            || (whole_program && (!item->link.u8_bytes
                || core_dict_contains(&exposed, &item->link))))
            continue;
        fields[field_count++] = i;
    }
    if (whole_program && analysis->summaries && analysis->rule_facts
        && field_count)
        gather_field_uses(analysis, &items, fields, field_count, field_flags);
    for (size_t c = 0; c < field_count; c++) {
        unused_item_t *item = &items.data[fields[c]];
        report_field(item,
            (item->access & UNUSED_READ)
                || (field_flags[c] & (IR_SFIELD_READ | IR_SFIELD_WHOLE)),
            (item->access & UNUSED_WRITE)
                || (field_flags[c] & (IR_SFIELD_WRITE | IR_SFIELD_WHOLE)));
    }
    core_free(field_flags);
    core_free(fields);
    for (size_t i = 0; i < items.len; i++) {
        unused_item_t *item = &items.data[i];
        if ((item->flags & (UNUSED_HIDDEN | UNUSED_ROOT))
            || !(whole_program || (item->flags & UNUSED_PRIVATE)))
            continue;
        switch (item->kind) {
        case UNUSED_FUNCTION:
            if (!item->live)
                report_item(item, N00B_DIAG_UNUSED_FUNCTION,
                    (item->flags & UNUSED_PRIVATE)
                        ? N00B_TEXT_PRIVATE_FUNCTION_NEVER_CALLED
                        : N00B_TEXT_FUNCTION_NEVER_CALLED,
                    N00B_TEXT_CALL_OR_DELETE);
            break;
        case UNUSED_TYPE:
            if (!item->live)
                report_item(item, N00B_DIAG_UNUSED_TYPE,
                            N00B_TEXT_TYPE_NEVER_USED, N00B_TEXT_REMOVE_IT);
            break;
        case UNUSED_GLOBAL: {
            if ((item->access & UNUSED_READ)
                || (item->link.u8_bytes
                    && core_dict_contains(&reads, &item->link)))
                break;
            bool written = (item->access & UNUSED_WRITE)
                || (item->link.u8_bytes
                    && core_dict_contains(&writes, &item->link));
            report_item(item,
                written ? N00B_DIAG_GLOBAL_NEVER_READ
                        : N00B_DIAG_UNUSED_GLOBAL,
                written ? N00B_TEXT_GLOBAL_SET_NEVER_READ
                        : N00B_TEXT_GLOBAL_NEVER_USED,
                N00B_TEXT_REMOVE_IT);
            break;
        }
        case UNUSED_FIELD:
            break;
        }
    }
    free_names(&exposed);
    free_names(&writes);
    free_names(&reads);
    core_buffer_release(&text);
    core_dict_free(&liveness.index);
    core_list_free(uses);
    core_list_free(items);
}
