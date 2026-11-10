#include "lower/lower_internal.h"
#include "ir/pdg_memory.h"

typedef struct {
    n00bb_query_t  query;
    core_string_t  name;
    parse_tree_t  *function;
    int64_t        callback;
    core_string_t  effect;
} effect_query_parts_t;

static const core_string_t effects_query_key =
    CORE_STRING_STATIC("n00bb.effects.query");
static const core_string_t trait_check_key =
    CORE_STRING_STATIC("n00bb.trait.check");

static bool
effect_query_parts(parse_grammar_t *grammar,
                   const core_translation_unit_t *unit, parse_tree_t *node,
                   effect_query_parts_t *out)
{
    parse_tree_t *word = child_nt(grammar, node,
                                  CORE_STRING_STATIC("effect-query-word"));
    parse_tree_t *name = child_nt(grammar, node,
                                  CORE_STRING_STATIC("effect-query-name"));
    *out = (effect_query_parts_t){
        .query = word ? n00bb_query_named(source_text_of(unit, word))
                      : N00BB_QUERY_NONE,
        .name = name ? source_text_of(unit, name) : (core_string_t){0},
        .function = child_nt(grammar, node, CORE_STRING_STATIC("expression")),
        .callback = -1,
    };
    return out->query != N00BB_QUERY_NONE && out->name.u8_bytes;
}

static core_string_t
effect_declaration_name(parse_grammar_t *grammar,
                        const core_translation_unit_t *unit,
                        parse_tree_t *declaration)
{
    parse_tree_t *name = child_nt(grammar, declaration,
                                  CORE_STRING_STATIC("effect-query-name"));
    return name ? source_text_of(unit, name) : (core_string_t){0};
}

static bool
declares_named(parse_grammar_t *grammar, core_translation_unit_t *unit,
               core_string_t nonterminal, core_string_t name)
{
    n00bb_tu_state_t *state = tu_state(unit);
    for (size_t i = 0; state && i < state->module_top_level_statements.len;
         i++) {
        parse_tree_t *statement = state->module_top_level_statements.data[i];
        if (nt_is(grammar, statement, nonterminal)
            && core_string_eq(effect_declaration_name(grammar, unit,
                                                      statement), name))
            return true;
    }
    return false;
}

static const core_string_t effect_declaration_key =
    CORE_STRING_STATIC("n00bb.effect.declaration");
static const core_string_t mark_declaration_key =
    CORE_STRING_STATIC("n00bb.mark.declaration");

static ir_func_t *
metadata_stub(ir_module_t *module, core_string_t name)
{
    ir_func_t *stub = ir_func_find(module, name);
    if (stub) return stub;
    th_type *nil = th_name(ir_module_types(module), CORE_STRING_STATIC("nil"));
    th_type *signature = th_func_with_error(ir_module_types(module), nil, nil,
                                            nullptr, 0, nullptr, nullptr, 0);
    return signature ? ir_func_add(module, name, signature, IR_LINK_EXTERN)
                     : nullptr;
}

static ir_module_t *
cached_ir_module(n00bb_link_analysis_t *analysis,
                 core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state) return nullptr;
    ir_module_t *cached = state->interface_module && analysis
            && analysis->cache
        ? n00bb_cache_module(analysis->cache, unit->module.identifier)
        : nullptr;
    return cached ? cached : state->ir_module;
}

static void
append_qualified(core_buffer_t *out, const core_translation_unit_t *unit,
                 core_string_t leaf)
{
    core_buffer_append_string(out, unit->module.identifier);
    core_buffer_putc(out, '.');
    core_buffer_append_string(out, leaf);
}

static bool
records_named(n00bb_link_analysis_t *analysis, core_translation_unit_t *unit,
              core_string_t prefix, core_string_t leaf)
{
    ir_module_t *module = cached_ir_module(analysis, unit);
    if (!module) return false;
    core_buffer_t name = {0};
    core_buffer_append_string(&name, prefix);
    append_qualified(&name, unit, leaf);
    bool found = ir_func_find(module, core_buffer_string(&name)) != nullptr;
    core_buffer_release(&name);
    return found;
}

static bool
module_matches(core_string_t module, core_string_t prefix)
{
    if (!prefix.u8_bytes || core_string_eq(module, prefix)) return true;
    return module.u8_bytes > prefix.u8_bytes
        && core_string_ends_with(module, prefix)
        && module.data[module.u8_bytes - prefix.u8_bytes - 1] == '.';
}

typedef bool (*declares_fn)(n00bb_link_analysis_t *analysis,
                            parse_grammar_t *grammar,
                            core_translation_unit_t *unit, core_string_t leaf);

static bool
effect_declared(n00bb_link_analysis_t *analysis, parse_grammar_t *grammar,
                core_translation_unit_t *unit, core_string_t leaf)
{
    return declares_named(grammar, unit, CORE_STRING_STATIC("effect-decl"),
                          leaf)
        || records_named(analysis, unit, CORE_STRING_STATIC("__n00bb_effect#"),
                         leaf);
}

static bool
mark_declared(n00bb_link_analysis_t *analysis, parse_grammar_t *grammar,
              core_translation_unit_t *unit, core_string_t leaf)
{
    return declares_named(grammar, unit, CORE_STRING_STATIC("mark-decl"), leaf)
        || records_named(analysis, unit, CORE_STRING_STATIC("__n00bb_mark#"),
                         leaf);
}

static bool
function_declared(n00bb_link_analysis_t *analysis, parse_grammar_t *grammar,
                  core_translation_unit_t *unit, core_string_t leaf)
{
    (void)grammar;
    ir_module_t *module = cached_ir_module(analysis, unit);
    return module && ir_func_find(module, leaf);
}

typedef struct {
    core_translation_unit_t *unit;
    core_translation_unit_t *other;
    core_string_t            leaf;
} declaration_t;

static void
note_declaration(declaration_t *found, core_translation_unit_t *unit)
{
    if (!found->unit) found->unit = unit;
    else if (found->unit != unit && !found->other) found->other = unit;
}

static void
note_exported(n00bb_link_analysis_t *analysis, parse_grammar_t *grammar,
              core_translation_unit_t *unit, core_string_t package,
              core_string_t leaf, declares_fn declares, declaration_t *found)
{
    n00bb_tu_state_t *state = tu_state(unit);
    for (size_t p = 0; state && p < state->package_declarations.len; p++) {
        const n00bb_package_declaration_t *declaration =
            &state->package_declarations.data[p];
        if (package.u8_bytes && !core_string_eq(declaration->name, package))
            continue;
        for (size_t e = 0; e < declaration->exports.len; e++) {
            core_translation_unit_t *source =
                declaration->exports.data[e].source_unit;
            if (source && declares(analysis, grammar, source, leaf))
                note_declaration(found, source);
        }
    }
}

static declaration_t
declaring_unit(parse_grammar_t *grammar, core_translation_unit_t *root,
               core_string_t name, declares_fn declares)
{
    declaration_t found = {.leaf = n00bb_name_leaf(name)};
    core_string_t prefix = found.leaf.u8_bytes < name.u8_bytes
        ? core_string_slice(name, 0, name.u8_bytes - found.leaf.u8_bytes - 1)
        : (core_string_t){0};
    n00bb_link_analysis_t *analysis = tu_state(root)
        ? tu_state(root)->link_analysis : nullptr;
    if (!prefix.u8_bytes) {
        if (declares(analysis, grammar, root, found.leaf)) {
            found.unit = root;
            return found;
        }
        for (size_t i = 0; i < root->imports.len; i++) {
            core_translation_unit_t *unit = root->imports.data[i];
            if (declares(analysis, grammar, unit, found.leaf))
                note_declaration(&found, unit);
            note_exported(analysis, grammar, unit, (core_string_t){0},
                          found.leaf, declares, &found);
        }
        return found;
    }
    core_list_t(core_translation_unit_ptr_t) pending =
        core_list_new(core_translation_unit_ptr_t);
    core_dict_t visited;
    core_dict_init(&visited, nullptr, nullptr);
    core_list_push(pending, root);
    for (size_t at = 0; !found.other && at < pending.len; at++) {
        core_translation_unit_t *unit = pending.data[at];
        if (!core_dict_add(&visited, unit, unit)) continue;
        if (module_matches(unit->module.identifier, prefix)
            && declares(analysis, grammar, unit, found.leaf))
            note_declaration(&found, unit);
        note_exported(analysis, grammar, unit, prefix, found.leaf, declares,
                      &found);
        for (size_t i = 0; i < unit->imports.len; i++)
            core_list_push(pending, unit->imports.data[i]);
    }
    core_dict_free(&visited);
    core_list_free(pending);
    return found;
}

static bool report_ambiguous(core_diagnostics_t *diagnostics,
                             const core_translation_unit_t *unit,
                             n00bb_diag_t id, core_diag_location_t location,
                             core_string_t name, const declaration_t *found);

static bool
resolve_reported(core_diagnostics_t *diagnostics,
                 const core_translation_unit_t *reporter, n00bb_diag_t id,
                 core_diag_location_t location, parse_grammar_t *grammar,
                 core_translation_unit_t *unit, core_string_t name,
                 declares_fn declares, n00bb_text_t missing,
                 declaration_t *found)
{
    *found = declaring_unit(grammar, unit, name, declares);
    if (report_ambiguous(diagnostics, reporter, id, location, name, found))
        return false;
    if (found->unit) return true;
    n00bb_report(diagnostics, reporter, id, location, missing,
                 N00BB_ARGS(ARG_TEXT(name)));
    return false;
}

static bool
report_ambiguous(core_diagnostics_t *diagnostics,
                 const core_translation_unit_t *unit, n00bb_diag_t id,
                 core_diag_location_t location, core_string_t name,
                 const declaration_t *found)
{
    if (!found->other) return false;
    core_buffer_t first = {0};
    core_buffer_t second = {0};
    append_qualified(&first, found->unit, found->leaf);
    append_qualified(&second, found->other, found->leaf);
    n00bb_report(diagnostics, unit, id, location, N00B_TEXT_NAMED_IN_TWO_MODULES,
        N00BB_ARGS(ARG_TEXT(name), ARG_TEXT(core_buffer_string(&first)),
                   ARG_TEXT(core_buffer_string(&second))));
    core_buffer_release(&first);
    core_buffer_release(&second);
    return true;
}

static bool
resolve_effect(parse_grammar_t *grammar, core_translation_unit_t *unit,
               core_string_t name, core_buffer_t *out)
{
    if (n00bb_effect_named(name)) {
        core_buffer_append_string(out, name);
        return true;
    }
    declaration_t found = unit
        ? declaring_unit(grammar, unit, name, effect_declared)
        : (declaration_t){0};
    if (!found.unit || found.other) return false;
    append_qualified(out, found.unit, found.leaf);
    return true;
}

static parse_sym_entry_t *
named_function(parse_grammar_t *grammar, core_translation_unit_t *unit,
               parse_tree_t *expression)
{
    parse_sym_entry_t *symbol = expression
        ? attached_symbol(grammar, unit, expression) : nullptr;
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        ? canonical_symbol(symbol) : nullptr;
}

static bool
clause_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    return nt_is(grammar, node, CORE_STRING_STATIC("effect-source"))
        || nt_is(grammar, node, CORE_STRING_STATIC("effect-handler"))
        || nt_is(grammar, node, CORE_STRING_STATIC("mark-clearer"))
        || nt_is(grammar, node, CORE_STRING_STATIC("mark-rejecter"));
}

static core_list_t(parse_tree_ptr_t)
effect_clauses(parse_grammar_t *grammar, parse_tree_t *declaration)
{
    core_list_t(parse_tree_ptr_t) clauses = core_list_new(parse_tree_ptr_t);
    core_list_t(parse_tree_ptr_t) stack = core_list_new(parse_tree_ptr_t);
    core_list_push(stack, declaration);
    while (stack.len) {
        parse_tree_t *node = stack.data[--stack.len];
        if (!node || core_tree_is_leaf(node)) continue;
        if (clause_node(grammar, node)) {
            core_list_push(clauses, node);
            continue;
        }
        for (size_t i = core_tree_num_children(node); i > 0; i--)
            core_list_push(stack, core_tree_child(node, i - 1));
    }
    core_list_free(stack);
    return clauses;
}

static ir_func_t *
module_function(n00bb_link_analysis_t *analysis, core_string_t module,
                core_string_t name)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        ir_module_t *owner = cached_ir_module(analysis, analysis->units.data[u]);
        if (!owner || !core_string_eq(ir_module_name(owner), module)) continue;
        ir_func_t *function = ir_func_find(owner, name);
        if (!function) {
            core_buffer_t qualified = {0};
            core_buffer_append_string(&qualified, module);
            core_buffer_putc(&qualified, '.');
            core_buffer_append_string(&qualified, name);
            function = ir_func_find(owner, core_buffer_string(&qualified));
            core_buffer_release(&qualified);
        }
        return n00bb_cached_body(function);
    }
    return nullptr;
}

static const core_string_t mark_rules_key =
    CORE_STRING_STATIC("n00bb.mark.rules");

static parse_tree_t *
annotated_owner(parse_grammar_t *grammar, parse_tree_t *annotation)
{
    for (parse_tree_t *node = parse_tree_parent(annotation); node;
         node = parse_tree_parent(node))
        if (nt_is(grammar, node, CORE_STRING_STATIC("pos-param"))
            || nt_is(grammar, node, CORE_STRING_STATIC("type-decl-one"))
            || nt_is(grammar, node, CORE_STRING_STATIC("record-decl")))
            return node;
    return nullptr;
}

static core_string_t
declared_type_name(parse_grammar_t *grammar, parse_tree_t *node)
{
    while (node && !nt_is(grammar, node, CORE_STRING_STATIC("record-decl")))
        node = parse_tree_parent(node);
    parse_sym_entry_t *symbol = node ? parse_tree_sym(node) : nullptr;
    core_string_t name = {0};
    if (!symbol || !symbol->type
        || !th_named_info(th_resolve(symbol->type), &name, nullptr, nullptr))
        return (core_string_t){0};
    return name;
}

static int64_t
formal_index(parse_grammar_t *grammar, parse_tree_t *parameter)
{
    parse_tree_t *formal = parameter;
    while (formal && !nt_is(grammar, formal, CORE_STRING_STATIC("formal")))
        formal = parse_tree_parent(formal);
    parse_tree_t *formals = formal;
    while (formals && !nt_is(grammar, formals, CORE_STRING_STATIC("formals")))
        formals = parse_tree_parent(formals);
    core_list_t(parse_tree_ptr_t) stack = core_list_new(parse_tree_ptr_t);
    if (formals) core_list_push(stack, formals);
    int64_t index = -1;
    for (int64_t seen = 0; index < 0 && stack.len;) {
        parse_tree_t *node = stack.data[--stack.len];
        if (!node || core_tree_is_leaf(node)) continue;
        if (nt_is(grammar, node, CORE_STRING_STATIC("formal"))) {
            if (node == formal) index = seen;
            seen++;
            continue;
        }
        for (size_t i = core_tree_num_children(node); i > 0; i--)
            core_list_push(stack, core_tree_child(node, i - 1));
    }
    core_list_free(stack);
    return index;
}

static bool
annotation_target(parse_grammar_t *grammar, n00bb_link_analysis_t *analysis,
                  parse_tree_t *owner, core_buffer_t *entry)
{
    if (nt_is(grammar, owner, CORE_STRING_STATIC("pos-param"))) {
        parse_sym_entry_t *parameter = parse_tree_sym(owner);
        parse_tree_t *definition = parse_tree_parent(owner);
        while (definition && !function_symbol_at(grammar, definition))
            definition = parse_tree_parent(definition);
        parse_sym_entry_t *function = (parse_sym_entry_t *)
            function_symbol_at(grammar, definition);
        ir_func_t *ir = function && analysis
            ? link_ir_function_for_symbol(analysis, canonical_symbol(function))
            : nullptr;
        int64_t index = ir && parameter ? formal_index(grammar, owner) : -1;
        if (index < 0) return false;
        core_buffer_puts(entry, "param\t");
        core_buffer_append_string(entry, ir_module_name(ir->owner));
        core_buffer_putc(entry, '\t');
        core_buffer_append_string(entry, ir->name);
        core_buffer_printf(entry, "\t%lld", (long long)index);
        return true;
    }
    core_string_t type = declared_type_name(grammar, owner);
    if (!type.u8_bytes) return false;
    bool field = nt_is(grammar, owner, CORE_STRING_STATIC("type-decl-one"));
    if (field) core_buffer_puts(entry, "field\t");
    else core_buffer_puts(entry, "type\t");
    core_buffer_append_string(entry, type);
    if (field) {
        parse_tree_t *name = nullptr;
        for (size_t c = 0; !name && c < core_tree_num_children(owner); c++)
            if (core_tree_is_leaf(core_tree_child(owner, c)))
                name = core_tree_child(owner, c);
        core_buffer_putc(entry, '\t');
        core_buffer_append_string(entry, first_token_text(name));
    }
    return true;
}

static bool
record_mark_annotation(parse_grammar_t *grammar, core_translation_unit_t *unit,
                       n00bb_link_analysis_t *analysis, ir_ctx_t *context,
                       parse_tree_t *annotation)
{
    n00bb_tu_state_t *state = tu_state(unit);
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    parse_tree_t *form = core_tree_num_children(annotation)
        ? core_tree_child(annotation, 0) : nullptr;
    bool rejects = form && nt_is(grammar, form,
                                 CORE_STRING_STATIC("mark-rejects"));
    parse_tree_t *mark = form ? child_nt(grammar, form,
        CORE_STRING_STATIC("effect-query-name")) : nullptr;
    core_string_t name = source_text_of(unit, mark);
    parse_tree_t *owner = annotated_owner(grammar, annotation);
    declaration_t found = {0};
    if (!resolve_reported(diagnostics, unit, N00B_DIAG_MARK_DECLARATION,
            link_source_location(mark ? mark : annotation), grammar, unit,
            name, mark_declared, N00B_TEXT_NOT_A_MARK, &found))
        return true;
    core_translation_unit_t *declaring = found.unit;
    core_string_t leaf = found.leaf;
    if (rejects && owner
        && !nt_is(grammar, owner, CORE_STRING_STATIC("pos-param"))) {
        n00bb_report(diagnostics, unit, N00B_DIAG_MARK_DECLARATION,
            link_source_location(annotation), N00B_TEXT_REJECTS_ONLY_PARAMETERS,
            N00BB_ARGS(ARG_TEXT(name)));
        return true;
    }
    core_buffer_t entry = {0};
    if (rejects) core_buffer_puts(&entry, "rejects\t");
    else core_buffer_puts(&entry, "carries\t");
    append_qualified(&entry, declaring, leaf);
    core_buffer_putc(&entry, '\t');
    bool ok = true;
    if (!owner || !annotation_target(grammar, analysis, owner, &entry))
        n00bb_report(diagnostics, unit, N00B_DIAG_MARK_DECLARATION,
            link_source_location(annotation), N00B_TEXT_MARK_ANNOTATION_TARGET,
            N00BB_ARGS(ARG_TEXT(name)));
    else {
        core_buffer_t stub_name = {0};
        core_buffer_puts(&stub_name, "__n00bb_mark_rule#");
        core_buffer_append_string(&stub_name, unit->module.identifier);
        ir_func_t *stub = metadata_stub(state->ir_module,
                                        core_buffer_string(&stub_name));
        core_buffer_release(&stub_name);
        ok = stub
            && ir_annot_set(context, &stub->annots, (ir_annot_t){
                   .key = mark_rules_key, .kind = IR_ANNOT_STR,
                   .s = ir_strdup(context, unit->module.identifier)})
            && ir_func_add_effect(stub, core_buffer_string(&entry));
    }
    core_buffer_release(&entry);
    return ok;
}

bool
record_mark_annotations(parse_grammar_t *grammar,
                        core_translation_unit_t *unit,
                        n00bb_link_analysis_t *analysis, ir_ctx_t *context)
{
    n00bb_tu_state_t *state = tu_state(unit);
    bool ok = true;
    for (size_t a = 0; ok && state && a < state->mark_annotations.len; a++)
        ok = record_mark_annotation(grammar, unit, analysis, context,
                                    state->mark_annotations.data[a]);
    return ok;
}

static bool
tab_fields(core_string_t text, core_string_t *fields, size_t count)
{
    for (size_t f = 0; f < count; f++) {
        int64_t tab = f + 1 < count ? core_string_find_byte(text, '\t')
                                    : (int64_t)text.u8_bytes;
        if (tab <= 0) return false;
        fields[f] = core_string_slice(text, 0, (size_t)tab);
        text = f + 1 < count
            ? core_string_slice(text, (size_t)tab + 1,
                                text.u8_bytes - (size_t)tab - 1)
            : text;
    }
    return true;
}

#define DECLARATIONS_KEY   "n00b.module-declarations"
#define DECLARATIONS_OWNER "declarations:"

enum { DECLARATIONS_VERSION = 1 };

static core_string_t
declaration_key(core_string_t stub)
{
    if (core_string_starts_with(stub, CORE_STRING_STATIC("__n00bb_effect#")))
        return effect_declaration_key;
    if (core_string_starts_with(stub, CORE_STRING_STATIC("__n00bb_mark#")))
        return mark_declaration_key;
    if (core_string_starts_with(stub, CORE_STRING_STATIC("__n00bb_mark_rule#")))
        return mark_rules_key;
    if (core_string_starts_with(stub,
                                CORE_STRING_STATIC("__n00bb_trait_check#")))
        return trait_check_key;
    return (core_string_t){0};
}

static bool
encode_declarations(void *context, ir_module_t *module, core_buffer_t *out)
{
    (void)context;
    for (size_t f = 0; f < ir_func_count(module); f++) {
        ir_func_t *stub = ir_func_at(module, f);
        core_string_t key = declaration_key(stub->name);
        ir_annot_t *declared = key.u8_bytes
            ? ir_annot_get(&stub->annots, key) : nullptr;
        if (!declared || declared->kind != IR_ANNOT_STR) continue;
        core_buffer_puts(out, "S\t");
        core_buffer_append_string(out, stub->name);
        core_buffer_putc(out, '\t');
        core_buffer_append_string(out, declared->s);
        core_buffer_putc(out, '\n');
        for (uint32_t e = 0;; e++) {
            core_string_t entry = ir_func_effect_at(stub, e);
            if (!entry.u8_bytes) break;
            core_buffer_puts(out, "E\t");
            core_buffer_append_string(out, entry);
            core_buffer_putc(out, '\n');
        }
    }
    return true;
}

bool
publish_declarations(n00bb_link_analysis_t *analysis)
{
    return n00bb_publish_module_metadata(analysis,
        CORE_STRING_STATIC(DECLARATIONS_OWNER),
        CORE_STRING_STATIC(DECLARATIONS_KEY), DECLARATIONS_VERSION,
        encode_declarations, nullptr);
}

static bool
decode_declarations(ir_ctx_t *context, ir_module_t *module,
                    core_string_t text)
{
    ir_func_t *stub = nullptr;
    while (text.u8_bytes) {
        int64_t end = core_string_find_byte(text, '\n');
        size_t length = end < 0 ? text.u8_bytes : (size_t)end;
        core_string_t line = core_string_slice(text, 0, length);
        text = end < 0 ? (core_string_t){0}
            : core_string_slice(text, length + 1, text.u8_bytes - length - 1);
        core_string_t fields[3];
        if (core_string_starts_with(line, CORE_STRING_STATIC("E\t"))) {
            if (stub && !ir_func_add_effect(stub, core_string_slice(line, 2,
                                                    line.u8_bytes - 2)))
                return false;
            continue;
        }
        stub = nullptr;
        if (!tab_fields(line, fields, 3)) continue;
        core_string_t key = declaration_key(fields[1]);
        stub = key.u8_bytes ? metadata_stub(module, fields[1]) : nullptr;
        if (stub) stub->span = IR_SPAN_UNKNOWN;
        if (stub && !ir_annot_set(context, &stub->annots, (ir_annot_t){
                .key = key, .kind = IR_ANNOT_STR,
                .s = ir_strdup(context, fields[2])}))
            return false;
    }
    return true;
}

static bool
restore_module(void *context, core_string_t name, const uint8_t *data,
               size_t size)
{
    n00bb_link_analysis_t *analysis = context;
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        ir_module_t *module = state && state->interface_module
            ? cached_ir_module(analysis, analysis->units.data[u]) : nullptr;
        if (module && core_string_eq(ir_module_name(module), name)
            && !decode_declarations(analysis->ir_context, module,
                   (core_string_t){.data = (char *)data, .u8_bytes = size}))
            return false;
    }
    return true;
}

bool
restore_declarations(n00bb_link_analysis_t *analysis)
{
    bool ok = true;
    for (size_t e = 0; ok && e < analysis->linked_interface_count; e++)
        ok = n00bb_each_module_metadata(analysis->linked_interfaces[e],
            CORE_STRING_STATIC(DECLARATIONS_OWNER),
            CORE_STRING_STATIC(DECLARATIONS_KEY), DECLARATIONS_VERSION,
            restore_module, analysis);
    return ok;
}

n00bb_user_effect_t *
user_effect(n00bb_link_analysis_t *analysis, core_string_t qualified)
{
    for (size_t e = 0; e < analysis->user_effects.len; e++)
        if (core_string_eq(analysis->user_effects.data[e].qualified,
                           qualified))
            return &analysis->user_effects.data[e];
    return nullptr;
}

static void
collect_effect_entries(n00bb_link_analysis_t *analysis,
                       core_translation_unit_t *unit, ir_func_t *stub,
                       n00bb_user_effect_t *effect)
{
    for (uint32_t e = 0;; e++) {
        core_string_t entry = ir_func_effect_at(stub, e);
        core_string_t fields[3];
        if (!entry.u8_bytes) break;
        if (!tab_fields(entry, fields, 3)) continue;
        bool native = core_string_eq(fields[1], CORE_STRING_STATIC("*"));
        ir_func_t *function = native ? nullptr
            : module_function(analysis, fields[1], fields[2]);
        if (!function && !native) continue;
        n00bb_effect_clause_t clause = {.function = function,
            .native = native ? fields[2] : n00bb_native_symbol(function),
            .unit = unit};
        if (clause.native.u8_bytes)
            core_list_push(analysis->effect_natives, clause.native);
        if (core_string_eq(fields[0], CORE_STRING_STATIC("handled")))
            core_list_push(effect->handlers, clause);
        else core_list_push(effect->sources, clause);
        if (function)
            core_dict_put(&analysis->effect_functions, function, function);
    }
}

static void
release_user_effects(n00bb_link_analysis_t *analysis)
{
    for (size_t e = 0; e < analysis->user_effects.len; e++) {
        core_free(analysis->user_effects.data[e].qualified.data);
        core_list_free(analysis->user_effects.data[e].sources);
        core_list_free(analysis->user_effects.data[e].handlers);
    }
    analysis->user_effects.len = 0;
    analysis->effect_natives.len = 0;
    if (analysis->effect_functions_ready)
        core_dict_free(&analysis->effect_functions);
    analysis->effect_functions_ready = false;
}

typedef void (*stub_fn)(void *context, core_translation_unit_t *unit,
                        ir_func_t *stub, core_string_t declared);

static void
for_each_stub(n00bb_link_analysis_t *analysis, core_string_t prefix,
              core_string_t key, stub_fn visit, void *context)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        ir_module_t *module = cached_ir_module(analysis, unit);
        for (size_t f = 0; module && f < ir_func_count(module); f++) {
            ir_func_t *stub = ir_func_at(module, f);
            ir_annot_t *declared = core_string_starts_with(stub->name, prefix)
                ? ir_annot_get(&stub->annots, key) : nullptr;
            if (declared && declared->kind == IR_ANNOT_STR)
                visit(context, unit, stub, declared->s);
        }
    }
}

static void
collect_user_effect(void *context, core_translation_unit_t *unit,
                    ir_func_t *stub, core_string_t declared)
{
    n00bb_link_analysis_t *analysis = context;
    n00bb_user_effect_t *effect = user_effect(analysis, declared);
    if (!effect) {
        const n00bb_effect_name_t *builtin = n00bb_effect_named(declared);
        core_list_push(analysis->user_effects, ((n00bb_user_effect_t){
            .qualified = core_string_dup(declared),
            .bits = builtin ? builtin->bits : 0,
            .sources = core_list_new(n00bb_effect_clause_t),
            .handlers = core_list_new(n00bb_effect_clause_t),
        }));
        effect = &analysis->user_effects.data[analysis->user_effects.len - 1];
    }
    collect_effect_entries(analysis, unit, stub, effect);
}

void
collect_user_effects(n00bb_link_analysis_t *analysis)
{
    release_user_effects(analysis);
    core_dict_init(&analysis->effect_functions, nullptr, nullptr);
    analysis->effect_functions_ready = true;
    for_each_stub(analysis, CORE_STRING_STATIC("__n00bb_effect#"),
                  effect_declaration_key, collect_user_effect, analysis);
}

bool
answer_query(n00bb_link_analysis_t *analysis, ir_func_t *subject,
             n00bb_query_t query, core_string_t effect)
{
    return n00bb_query_answer(analysis->summaries, subject, query, effect,
        n00bb_query_about_effect(query) ? user_effect(analysis, effect)
                                        : nullptr);
}

static parse_sym_entry_t *
effect_query_symbol(parse_grammar_t *grammar, core_translation_unit_t *unit,
                    parse_tree_t *node, const effect_query_parts_t *parts)
{
    parse_sym_entry_t *symbol = parts->function
        ? attached_symbol(grammar, unit, parts->function)
        : enclosing_function_symbol(grammar, node);
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        ? canonical_symbol(symbol) : nullptr;
}

static ir_func_t *
effect_query_subject(n00bb_link_analysis_t *analysis, parse_grammar_t *grammar,
                     core_translation_unit_t *unit, parse_tree_t *node,
                     const effect_query_parts_t *parts)
{
    parse_sym_entry_t *symbol = effect_query_symbol(grammar, unit, node,
                                                    parts);
    ir_func_t *function = symbol && analysis
        ? link_ir_function_for_symbol(analysis, symbol) : nullptr;
    return function ? n00bb_cached_body(function) : nullptr;
}

static int64_t
parameter_index(parse_sym_entry_t *symbol, core_string_t name)
{
    n00bb_function_parameter_facts_t *facts = symbol
        ? parse_sym_binding(symbol, N00BB_PARAMETER_FACTS_KEY) : nullptr;
    for (size_t p = 0; facts && p < facts->param_count; p++) {
        parse_sym_entry_t *parameter = facts->params[p].node
            ? parse_tree_sym(facts->params[p].node) : nullptr;
        if (parameter && core_string_eq(parameter->name, name))
            return (int64_t)p;
    }
    return -1;
}

static int64_t
callback_parameter(parse_grammar_t *grammar, core_translation_unit_t *unit,
                   parse_tree_t *node, const effect_query_parts_t *parts)
{
    parse_sym_entry_t *symbol = parts->function
        ? attached_symbol(grammar, unit, parts->function) : nullptr;
    parse_sym_entry_t *owner = symbol && symbol->kind == PARSE_SYM_PARAM
        ? enclosing_function_symbol(grammar, node) : nullptr;
    int64_t index = owner ? parameter_index(owner, symbol->name) : -1;
    th_type *type = index >= 0 && symbol->type ? th_resolve(symbol->type)
                                               : nullptr;
    n00bb_tu_state_t *state = tu_state(unit);
    return type && (th_kind_of(type) == TH_FUNCTION
                    || (state && n00bb_callback_contract_for_type(state,
                                                                  type)))
        ? index : -1;
}

static ir_instr_t *
emit_extern_call(ir_lower_context_t *context, core_string_t name,
                 core_string_t native, th_type *signature,
                 th_type *result_type, core_string_t prefix,
                 ir_value_t *argument, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    ir_func_t *function = ir_func_find(context->module, name);
    if (!function && signature)
        function = ir_func_add(context->module, name, signature,
                               IR_LINK_EXTERN);
    bool annotated = function && (!native.u8_bytes
        || ir_annot_set(lower->ir_context, &function->annots, (ir_annot_t){
               .key = CORE_STRING_STATIC("n00bb.abi.native"),
               .kind = IR_ANNOT_STR, .s = native}));
    ir_value_t *callee = annotated
        ? ir_func_address(context->module, function) : nullptr;
    ir_instr_t *call = callee
        ? ir_lower_emit_instruction(context, IR_OP_CALL, span) : nullptr;
    bool emitted = call
        && ir_lower_set_temp_dest(context, call, prefix, result_type)
        && ir_instr_operand_value(call, callee)
        && (!argument || ir_instr_operand_value(call, argument))
        && ir_instr_set_call_signature(call, signature);
    return emitted ? call : nullptr;
}

static bool
check_mark_question(ir_lower_context_t *context, parse_grammar_t *grammar,
                    parse_tree_t *node, effect_query_parts_t *parts)
{
    n00bb_lower_context_t *lower = context->user;
    ir_span_t span = lower_span_for_tree(context, node);
    parse_sym_entry_t *symbol = parts->function
        ? attached_symbol(grammar, lower->unit, parts->function) : nullptr;
    if (!symbol || !enclosing_function_symbol(grammar, node)
        || (symbol->kind != PARSE_SYM_VARIABLE
            && symbol->kind != PARSE_SYM_PARAM)) {
        lower_report(context, N00B_DIAG_EFFECT_QUESTION, span,
            N00B_TEXT_MAY_CARRY_NEEDS_LOCAL, N00BB_NO_ARGS);
        return false;
    }
    declaration_t found = {0};
    if (!resolve_reported(lower_diagnostics(context), lower_unit(context),
            N00B_DIAG_EFFECT_QUESTION, n00bb_span_location(span), grammar,
            lower->unit, parts->name, mark_declared, N00B_TEXT_NOT_A_MARK,
            &found))
        return false;
    core_buffer_t qualified = {0};
    append_qualified(&qualified, found.unit, found.leaf);
    parts->effect = ir_strdup(lower->ir_context,
                              core_buffer_string(&qualified));
    core_buffer_release(&qualified);
    return true;
}

static ir_instr_t *
emit_mark_question(ir_lower_context_t *context, parse_grammar_t *grammar,
                   parse_tree_t *node, const effect_query_parts_t *parts,
                   ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *state = tu_state(lower->unit);
    parse_sym_entry_t *symbol = attached_symbol(grammar, lower->unit,
                                                parts->function);
    ir_value_t *value = symbol ? ir_lower_bound_value(context, symbol)
                               : nullptr;
    th_ctx *types = ir_module_types(context->module);
    th_type *boolean = th_name(types, CORE_STRING_STATIC("bool"));
    if (!value || !boolean || !state || !state->link_analysis) return nullptr;
    th_param param = {.mode = TH_IN, .type = value->type};
    th_type *signature = th_func_with_error(types, boolean,
        th_name(types, CORE_STRING_STATIC("nil")), &param, 1, nullptr,
        nullptr, 0);
    core_buffer_t name = {0};
    core_buffer_printf(&name, N00BB_MARK_QUERY "%016llx",
        signature ? (unsigned long long)th_hash(types, signature) : 0ull);
    ir_instr_t *call = emit_extern_call(context, core_buffer_string(&name),
        (core_string_t){0}, signature, boolean,
        CORE_STRING_STATIC("__n00bb_mark_query_"), value, span);
    core_buffer_release(&name);
    if (!call) return nullptr;
    core_list_push(state->link_analysis->mark_questions,
        ((n00bb_mark_question_t){.node = node, .call = call,
                                 .mark = parts->effect, .answer = true}));
    return call;
}

static bool
check_effect_query(ir_lower_context_t *context, parse_grammar_t *grammar,
                   parse_tree_t *node, effect_query_parts_t *parts,
                   ir_func_t **subject_out)
{
    if (parts->query == N00BB_QUERY_MAY_CARRY) {
        *subject_out = nullptr;
        return check_mark_question(context, grammar, node, parts);
    }
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *state = tu_state(lower->unit);
    ir_span_t span = lower_span_for_tree(context, node);
    core_string_t word = n00bb_query_word(parts->query);
    if (!parts->function && !enclosing_function_symbol(grammar, node)) {
        lower_report(context, N00B_DIAG_EFFECT_QUESTION, span,
            N00B_TEXT_SHORT_QUERY_OUTSIDE,
            N00BB_ARGS(ARG_TEXT(word), ARG_TEXT(parts->name)));
        lower_help(context, N00B_TEXT_NAME_THE_FUNCTION_FIRST,
            N00BB_ARGS(ARG_TEXT(word), ARG_TEXT(parts->name)));
        return false;
    }
    parts->callback = callback_parameter(grammar, lower->unit, node, parts);
    bool callback = parts->callback >= 0;
    ir_func_t *subject = callback ? nullptr : effect_query_subject(
        state ? state->link_analysis : nullptr, grammar, lower->unit, node,
        parts);
    if (!subject && !callback) {
        lower_report(context, N00B_DIAG_EFFECT_QUESTION,
            lower_span_for_tree(context, parts->function
                                         ? parts->function : node),
            N00B_TEXT_QUERY_NEEDS_FUNCTION, N00BB_ARGS(ARG_TEXT(word)));
        return false;
    }
    if (callback && parts->query != N00BB_QUERY_FORBIDS_EFFECT
        && parts->query != N00BB_QUERY_HAS_EFFECT) {
        lower_report(context, N00B_DIAG_EFFECT_QUESTION, span,
            N00B_TEXT_CALLBACK_QUERY_FORMS,
            N00BB_ARGS(ARG_TEXT(source_text_of(lower->unit,
                                               parts->function))));
        return false;
    }
    if (n00bb_query_about_effect(parts->query)) {
        const n00bb_effect_name_t *effect = n00bb_effect_named(parts->name);
        declaration_t found = {0};
        if (!effect && !resolve_reported(lower_diagnostics(context),
                lower_unit(context), N00B_DIAG_EFFECT_QUESTION,
                n00bb_span_location(span), grammar, lower->unit, parts->name,
                effect_declared, N00B_TEXT_NOT_AN_EFFECT, &found))
            return false;
        core_buffer_t qualified = {0};
        if (effect) core_buffer_append_string(&qualified, parts->name);
        else append_qualified(&qualified, found.unit, found.leaf);
        parts->effect = ir_strdup(lower->ir_context,
                                  core_buffer_string(&qualified));
        core_buffer_release(&qualified);
        if (parts->query == N00BB_QUERY_HAS_EFFECT && effect
            && effect->group) {
            lower_report(context, N00B_DIAG_EFFECT_QUESTION, span,
                N00B_TEXT_HAS_EFFECT_TAKES_ONE,
                N00BB_ARGS(ARG_TEXT(parts->name)));
            return false;
        }
    }
    else if (parameter_index(effect_query_symbol(grammar, lower->unit, node,
                                                 parts),
                             parts->name) < 0) {
        lower_report(context, N00B_DIAG_EFFECT_QUESTION, span,
            N00B_TEXT_NO_PARAMETER_NAMED,
            N00BB_ARGS(ARG_TEXT(n00bb_function_root(subject->name)),
                       ARG_TEXT(parts->name)));
        return false;
    }
    *subject_out = subject;
    return true;
}

ir_lower_result_t
n00bb_lower_effect_query(ir_lower_context_t *context, parse_visit_t *visit,
                         ir_value_t *const *operands, size_t operand_count,
                         th_type *result_type, ir_span_t span, void *state)
{
    (void)operands;
    (void)operand_count;
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    result_type = th_name(ir_module_types(context->module),
                          CORE_STRING_STATIC("bool"));
    effect_query_parts_t parts;
    if (!lower || !result_type
        || !effect_query_parts(visit->grammar, lower->unit, visit->node,
                               &parts))
        return lower_internal(context, span, "malformed effect question");
    ir_func_t *subject = nullptr;
    if (!check_effect_query(context, visit->grammar, visit->node, &parts,
                            &subject))
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    if (parts.query == N00BB_QUERY_MAY_CARRY) {
        ir_instr_t *question = emit_mark_question(context, visit->grammar,
                                                  visit->node, &parts, span);
        return question ? (ir_lower_result_t){IR_LOWER_VALUE, question->dest}
                        : lower_internal(context, span,
                                         "could not emit a mark question");
    }
    if (!subject)
        return lower_report(context, N00B_DIAG_EFFECT_QUESTION, span,
            N00B_TEXT_CALLBACK_QUERY_VALUE,
            N00BB_ARGS(ARG_TEXT(source_text_of(lower->unit,
                                               parts.function))));
    core_string_t native = CORE_STRING_STATIC("n00bb_effect_query");
    th_type *signature = th_func_with_error(ir_module_types(context->module),
        result_type, th_name(ir_module_types(context->module),
                             CORE_STRING_STATIC("nil")),
        nullptr, 0, nullptr, nullptr, 0);
    core_string_t name = n00bb_query_about_effect(parts.query) ? parts.effect
                                                               : parts.name;
    ir_instr_t *call = emit_extern_call(context, native, native, signature,
        result_type, CORE_STRING_STATIC("__n00bb_query_"), nullptr, span);
    bool emitted = call
        && annotate_effects_query(lower->ir_context, call,
               ir_func_address(subject->owner, subject), name)
        && ir_annot_set(lower->ir_context, &call->annots, (ir_annot_t){
               .key = effects_query_key, .kind = IR_ANNOT_STR,
               .s = n00bb_query_word(parts.query)});
    return emitted ? (ir_lower_result_t){IR_LOWER_VALUE, call->dest}
                   : lower_internal(context, span,
                                    "could not emit an effect question");
}

enum { CHECK_QUERY = 1u << 0, CHECK_OTHER = 1u << 1 };

static uint8_t
check_shape(parse_grammar_t *grammar, const core_translation_unit_t *unit,
            parse_tree_t *node)
{
    if (!node) return 0;
    if (core_tree_is_leaf(node)) {
        static const core_string_t allowed[] = {
            CORE_STRING_STATIC("not"), CORE_STRING_STATIC("!"),
            CORE_STRING_STATIC("and"), CORE_STRING_STATIC("&&"),
            CORE_STRING_STATIC("or"), CORE_STRING_STATIC("||"),
            CORE_STRING_STATIC("("), CORE_STRING_STATIC(")"),
            CORE_STRING_STATIC("true"), CORE_STRING_STATIC("false"),
        };
        core_string_t text = source_text_of(unit, node);
        for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
            if (core_string_eq(text, allowed[i])) return 0;
        return CHECK_OTHER;
    }
    if (nt_is(grammar, node, CORE_STRING_STATIC("effect-query")))
        return CHECK_QUERY;
    uint8_t shape = 0;
    for (size_t i = 0; shape != (CHECK_QUERY | CHECK_OTHER)
         && i < core_tree_num_children(node); i++)
        shape |= check_shape(grammar, unit, core_tree_child(node, i));
    return shape;
}

typedef bool (*effect_query_fn)(void *context, parse_tree_t *query,
                                effect_query_parts_t *parts);

static bool
for_each_effect_query(parse_grammar_t *grammar,
                      const core_translation_unit_t *unit, parse_tree_t *node,
                      effect_query_fn visit, void *context)
{
    if (!node || core_tree_is_leaf(node)) return true;
    if (nt_is(grammar, node, CORE_STRING_STATIC("effect-query"))) {
        effect_query_parts_t parts;
        return effect_query_parts(grammar, unit, node, &parts)
            && visit(context, node, &parts);
    }
    bool ok = true;
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        ok = for_each_effect_query(grammar, unit, core_tree_child(node, i),
                                   visit, context) && ok;
    return ok;
}

static size_t check_operands(parse_grammar_t *grammar, parse_tree_t *node,
                             parse_tree_t **operands, bool *negated);

static void
collect_conjuncts(parse_grammar_t *grammar, parse_tree_t *node,
                  core_list_t(parse_tree_ptr_t) *out)
{
    while (node && !core_tree_is_leaf(node)) {
        if (nt_is(grammar, node, CORE_STRING_STATIC("effect-query"))) {
            core_list_push(*out, node);
            return;
        }
        parse_tree_t *operands[2] = {0};
        bool negated = false;
        size_t count = check_operands(grammar, node, operands, &negated);
        if (negated) return;
        if (count == 2
            && nt_is(grammar, node, CORE_STRING_STATIC("and-expr"))) {
            collect_conjuncts(grammar, operands[0], out);
            node = operands[1];
            continue;
        }
        node = count == 1 ? operands[0] : nullptr;
    }
}

typedef struct {
    ir_lower_context_t *context;
    core_list_t(parse_tree_ptr_t) conjuncts;
} check_validation_t;

static bool
valid_check_query(void *context, parse_tree_t *query,
                  effect_query_parts_t *parts)
{
    check_validation_t *validation = context;
    ir_lower_context_t *lowering = validation->context;
    n00bb_lower_context_t *lower = lowering->user;
    ir_func_t *subject = nullptr;
    if (!check_effect_query(lowering, lower->grammar, query, parts, &subject))
        return false;
    if (parts->query == N00BB_QUERY_MAY_CARRY)
        return emit_mark_question(lowering, lower->grammar, query, parts,
                                  lower_span_for_tree(lowering, query))
            != nullptr;
    if (subject || parts->callback < 0) return true;
    if (!core_option_is_set(core_list_find(validation->conjuncts, query))) {
        lower_report(lowering, N00B_DIAG_EFFECT_QUESTION,
            lower_span_for_tree(lowering, query),
            N00B_TEXT_CALLBACK_QUERY_CONJUNCT,
            N00BB_ARGS(ARG_TEXT(source_text_of(lower->unit,
                                               parts->function))));
        return false;
    }
    core_buffer_t tag = {0};
    n00bb_bound_tag(&tag, parts->query, (uint32_t)parts->callback,
                    parts->effect);
    bool added = ir_func_add_effect(lowering->function,
                                    core_buffer_string(&tag));
    core_buffer_release(&tag);
    return added;
}

static bool
add_clause_entry(ir_func_t *stub, core_string_t kind, const ir_func_t *function,
                 int64_t param)
{
    core_buffer_t entry = {0};
    core_buffer_append_string(&entry, kind);
    core_buffer_putc(&entry, '\t');
    core_buffer_append_string(&entry, ir_module_name(function->owner));
    core_buffer_putc(&entry, '\t');
    core_buffer_append_string(&entry, function->name);
    if (param >= 0) core_buffer_printf(&entry, "\t%lld", (long long)param);
    bool added = ir_func_add_effect(stub, core_buffer_string(&entry));
    core_buffer_release(&entry);
    return added;
}

static bool
record_effect_clause(ir_lower_context_t *context, ir_func_t *stub,
                     parse_sym_entry_t *symbol, bool handled)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *state = tu_state(lower->unit);
    ir_func_t *function = state && state->link_analysis
        ? link_ir_function_for_symbol(state->link_analysis, symbol) : nullptr;
    const n00bb_axiom_t *axiom = function ? nullptr
        : n00bb_axiom_of_native_name(symbol->name);
    if (!function && axiom && axiom->native.u8_bytes) {
        core_buffer_t entry = {0};
        if (handled) core_buffer_puts(&entry, "handled\t*\t");
        else core_buffer_puts(&entry, "from\t*\t");
        core_buffer_append_string(&entry, axiom->native);
        bool added = ir_func_add_effect(stub, core_buffer_string(&entry));
        core_buffer_release(&entry);
        return added;
    }
    return !function || add_clause_entry(stub, handled
        ? CORE_STRING_STATIC("handled") : CORE_STRING_STATIC("from"),
        function, -1);
}

static ir_func_t *
declaration_stub(ir_lower_context_t *context, core_string_t prefix,
                 core_string_t key, core_string_t qualified,
                 parse_tree_t *declaration)
{
    n00bb_lower_context_t *lower = context->user;
    core_buffer_t name = {0};
    core_buffer_append_string(&name, prefix);
    core_buffer_append_string(&name, qualified);
    ir_func_t *stub = metadata_stub(context->module,
                                    core_buffer_string(&name));
    core_buffer_release(&name);
    if (stub) stub->span = lower_span_for_tree(context, declaration);
    return stub && ir_annot_set(lower->ir_context, &stub->annots, (ir_annot_t){
               .key = key, .kind = IR_ANNOT_STR,
               .s = ir_strdup(lower->ir_context, qualified)})
        ? stub : nullptr;
}

static bool
defines_body(parse_grammar_t *grammar, const parse_sym_entry_t *symbol)
{
    parse_tree_t *definition = (parse_tree_t *)function_symbol_at(grammar,
        symbol->decl_node) ? symbol->decl_node : nullptr;
    return definition
        && child_nt(grammar, definition, CORE_STRING_STATIC("body"));
}

bool
n00bb_lower_effect_declaration(ir_lower_context_t *context,
                               parse_visit_t *visit, bool entering,
                               void *opaque)
{
    (void)opaque;
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    if (!entering || !lower) return true;
    parse_grammar_t *grammar = visit->grammar;
    core_string_t name = effect_declaration_name(grammar, lower->unit,
                                                 visit->node);
    ir_span_t span = lower_span_for_tree(context, visit->node);
    const n00bb_effect_name_t *builtin = n00bb_effect_named(name);
    if (builtin && builtin->group) {
        lower_report(context, N00B_DIAG_EFFECT_DECLARATION, span,
            N00B_TEXT_EFFECT_IS_BUILT_IN, N00BB_ARGS(ARG_TEXT(name)));
        return ir_lower_prune_node(context, visit->node);
    }
    declaration_t found = {.unit = lower->unit};
    bool resolved = builtin || resolve_reported(lower_diagnostics(context),
        lower_unit(context), N00B_DIAG_EFFECT_DECLARATION,
        n00bb_span_location(span), grammar, lower->unit, name,
        effect_declared, N00B_TEXT_NOT_AN_EFFECT, &found);
    if (!resolved && found.other)
        return ir_lower_prune_node(context, visit->node);
    core_translation_unit_t *owner = resolved ? found.unit : nullptr;
    core_string_t leaf = found.leaf;
    core_buffer_t qualified = {0};
    if (builtin) core_buffer_append_string(&qualified, name);
    else if (owner) append_qualified(&qualified, owner, leaf);
    ir_func_t *stub = owner
        ? declaration_stub(context, CORE_STRING_STATIC("__n00bb_effect#"),
                           effect_declaration_key,
                           core_buffer_string(&qualified), visit->node)
        : nullptr;
    core_buffer_release(&qualified);
    core_string_t declaring = owner ? n00bb_package_id(owner)
                                    : (core_string_t){0};
    core_string_t writing = n00bb_package_id(lower->unit);
    core_list_t(parse_tree_ptr_t) clauses = effect_clauses(grammar,
                                                           visit->node);
    for (size_t c = 0; c < clauses.len; c++) {
        parse_tree_t *clause = clauses.data[c];
        parse_tree_t *expression = child_nt(grammar, clause,
                                            CORE_STRING_STATIC("expression"));
        parse_tree_t *site = expression ? expression : clause;
        bool handled = nt_is(grammar, clause,
                             CORE_STRING_STATIC("effect-handler"));
        parse_sym_entry_t *symbol = named_function(grammar, lower->unit,
                                                   expression);
        bool rejected = builtin && symbol
            && (handled || (builtin->bits == IR_SUMMARY_MAY_DIVERGE
                            && defines_body(grammar, symbol)));
        if (rejected)
            lower_report(context, N00B_DIAG_EFFECT_DECLARATION,
                lower_span_for_tree(context, handled ? clause : site),
                handled ? N00B_TEXT_BUILT_IN_NOT_HANDLED
                        : N00B_TEXT_NORETURN_NEEDS_FOREIGN,
                N00BB_ARGS(ARG_TEXT(name),
                           ARG_TEXT(source_text_of(lower->unit, site))));
        else if (symbol && stub
                 && !record_effect_clause(context, stub, symbol, handled)) {
            core_list_free(clauses);
            return false;
        }
        if (!symbol)
            lower_report(context, N00B_DIAG_EFFECT_DECLARATION,
                lower_span_for_tree(context, site),
                N00B_TEXT_EFFECT_NEEDS_FUNCTION,
                N00BB_ARGS(ARG_TEXT(source_text_of(lower->unit, site))));
        else if (handled && owner && owner != lower->unit
                 && !core_string_eq(declaring, writing))
            lower_report(context, N00B_DIAG_HANDLED_ELSEWHERE,
                lower_span_for_tree(context, clause),
                N00B_TEXT_HANDLED_OUTSIDE_PACKAGE,
                N00BB_ARGS(ARG_TEXT(name),
                           ARG_TEXT(n00bb_package_namespace(owner)),
                           ARG_TEXT(n00bb_package_namespace(lower->unit))));
    }
    core_list_free(clauses);
    return ir_lower_prune_node(context, visit->node);
}

static bool
record_mark_rule(ir_lower_context_t *context, ir_func_t *stub,
                 parse_tree_t *clause, bool clears)
{
    n00bb_lower_context_t *lower = context->user;
    parse_grammar_t *grammar = lower->grammar;
    parse_tree_t *target = child_nt(grammar, clause,
                                    CORE_STRING_STATIC("effect-query-name"));
    parse_tree_t *parameter = core_tree_num_children(clause) > 4
        ? core_tree_child(clause, 4) : nullptr;
    core_string_t name = source_text_of(lower->unit, target);
    declaration_t found = target
        ? declaring_unit(grammar, lower->unit, name, function_declared)
        : (declaration_t){0};
    if (report_ambiguous(lower_diagnostics(context), lower_unit(context),
                         N00B_DIAG_MARK_DECLARATION,
                         n00bb_span_location(lower_span_for_tree(context,
                                                                 target)),
                         name, &found))
        return true;
    core_translation_unit_t *owner = found.unit;
    core_string_t leaf = found.leaf;
    n00bb_tu_state_t *state = tu_state(lower->unit);
    ir_module_t *module = owner && state
        ? cached_ir_module(state->link_analysis, owner) : nullptr;
    ir_func_t *function = module ? ir_func_find(module, leaf) : nullptr;
    ir_span_t span = lower_span_for_tree(context, target ? target : clause);
    if (!function) {
        lower_report(context, N00B_DIAG_MARK_DECLARATION, span,
            N00B_TEXT_EFFECT_NEEDS_FUNCTION, N00BB_ARGS(ARG_TEXT(name)));
        return true;
    }
    core_string_t wanted = parameter ? first_token_text(parameter)
                                     : (core_string_t){0};
    int64_t index = n00bb_parameter_named(function, wanted);
    if (index < 0) {
        lower_report(context, N00B_DIAG_MARK_DECLARATION, span,
            N00B_TEXT_NO_PARAMETER_NAMED,
            N00BB_ARGS(ARG_TEXT(name), ARG_TEXT(wanted)));
        return true;
    }
    th_type *signature = th_resolve(function->type);
    th_type *failure = signature && th_kind_of(signature) == TH_FUNCTION
        ? th_func_error(signature) : nullptr;
    if (clears && (!failure || th_is_nil(th_resolve(failure)))) {
        lower_report(context, N00B_DIAG_MARK_DECLARATION, span,
            N00B_TEXT_VALIDATOR_NEEDS_FAIL, N00BB_ARGS(ARG_TEXT(name)));
        return true;
    }
    return add_clause_entry(stub, clears ? CORE_STRING_STATIC("cleared")
                                         : CORE_STRING_STATIC("rejected"),
                            function, index);
}

bool
n00bb_lower_mark_declaration(ir_lower_context_t *context,
                             parse_visit_t *visit, bool entering,
                             void *opaque)
{
    (void)opaque;
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    if (!entering || !lower) return true;
    parse_grammar_t *grammar = visit->grammar;
    core_string_t name = effect_declaration_name(grammar, lower->unit,
                                                 visit->node);
    if (core_string_find_byte(name, '.') >= 0) {
        lower_report(context, N00B_DIAG_MARK_DECLARATION,
            lower_span_for_tree(context, visit->node),
            N00B_TEXT_MARK_DECLARED_ELSEWHERE, N00BB_ARGS(ARG_TEXT(name)));
        return ir_lower_prune_node(context, visit->node);
    }
    core_buffer_t qualified = {0};
    append_qualified(&qualified, lower->unit, name);
    ir_func_t *stub = declaration_stub(context,
        CORE_STRING_STATIC("__n00bb_mark#"), mark_declaration_key,
        core_buffer_string(&qualified), visit->node);
    core_buffer_release(&qualified);
    bool ok = stub != nullptr;
    core_list_t(parse_tree_ptr_t) clauses = effect_clauses(grammar,
                                                           visit->node);
    for (size_t c = 0; ok && c < clauses.len; c++) {
        parse_tree_t *clause = clauses.data[c];
        if (nt_is(grammar, clause, CORE_STRING_STATIC("effect-source"))) {
            parse_tree_t *expression = child_nt(grammar, clause,
                CORE_STRING_STATIC("expression"));
            parse_sym_entry_t *symbol = named_function(grammar, lower->unit,
                                                       expression);
            if (!symbol)
                lower_report(context, N00B_DIAG_MARK_DECLARATION,
                    lower_span_for_tree(context, expression ? expression
                                                            : clause),
                    N00B_TEXT_EFFECT_NEEDS_FUNCTION,
                    N00BB_ARGS(ARG_TEXT(source_text_of(lower->unit,
                        expression ? expression : clause))));
            else ok = record_effect_clause(context, stub, symbol, false);
        }
        else ok = record_mark_rule(context, stub, clause,
            nt_is(grammar, clause, CORE_STRING_STATIC("mark-clearer")));
    }
    core_list_free(clauses);
    return ok && ir_lower_prune_node(context, visit->node);
}

bool
n00bb_lower_effect_check(ir_lower_context_t *context, parse_visit_t *visit,
                         bool entering, void *opaque)
{
    (void)opaque;
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    if (!entering || !lower) return true;
    parse_tree_t *expression = child_nt(visit->grammar, visit->node,
                                        CORE_STRING_STATIC("expression"));
    uint8_t shape = check_shape(visit->grammar, lower->unit, expression);
    if (!(shape & CHECK_QUERY)) return true;
    if (shape & CHECK_OTHER) {
        lower_report(context, N00B_DIAG_EFFECT_CHECK_MIXED,
            lower_span_for_tree(context, visit->node),
            N00B_TEXT_CHECK_MIXES_VALUES, N00BB_NO_ARGS);
        lower_help(context, N00B_TEXT_PUT_RUN_TIME_PART_APART, N00BB_NO_ARGS);
        return ir_lower_prune_node(context, visit->node);
    }
    n00bb_tu_state_t *state = tu_state(lower->unit);
    check_validation_t validation = {
        .context = context, .conjuncts = core_list_new(parse_tree_ptr_t),
    };
    collect_conjuncts(visit->grammar, expression, &validation.conjuncts);
    bool valid = for_each_effect_query(visit->grammar, lower->unit,
                                       expression, valid_check_query,
                                       &validation);
    core_list_free(validation.conjuncts);
    if (valid && state && state->link_analysis)
        core_list_push(state->link_analysis->effect_checks,
            ((n00bb_effect_check_t){
                .unit = lower->unit, .statement = visit->node,
                .expression = expression}));
    return ir_lower_prune_node(context, visit->node);
}

typedef struct {
    const ir_value_t *value;
    bool              unique;
} slot_store_t;

static bool
slot_store(void *context, const ir_memdef_t *def)
{
    slot_store_t *store = context;
    const ir_value_t *value = def->kind == IR_MEMDEF_STORE && def->instr
            && def->instr->n_operands == 2
        ? ir_operand_value(&def->instr->operands[1]) : nullptr;
    store->unique = store->unique && value
        && (!store->value || store->value == value);
    store->value = value;
    return store->unique;
}

static const ir_value_t *
stored_value(ir_func_t *function, const ir_instr_t *load)
{
    ir_pdg_t *pdg = function ? ir_func_pdg(function) : nullptr;
    ir_pdg_memory_t *memssa = pdg ? ir_pdg_memory(pdg) : nullptr;
    slot_store_t store = {.unique = true};
    bool visited = memssa && ir_pdg_memory_visit_clobbers(memssa,
        ir_pdg_memory_access_of(memssa, load, IR_ACCESS_LOAD), slot_store,
        &store);
    return visited && store.unique ? store.value : nullptr;
}

static const ir_value_t *
passed_value(ir_func_t *function, const ir_value_t *value)
{
    for (uint32_t depth = 0; value && depth < 16; depth++) {
        const ir_instr_t *def = value->kind == IR_VAL_INSTR ? value->def
                                                            : nullptr;
        const ir_value_t *source = def && def->n_operands == 1
            ? ir_operand_value(&def->operands[0]) : nullptr;
        if (def && def->op == IR_OP_COPY) value = source;
        else if (def && def->op == IR_OP_LOAD && source
                 && source->kind == IR_VAL_INSTR && source->def
                 && source->def->op == IR_OP_ALLOC)
            value = stored_value(function, def);
        else return value;
    }
    return value;
}

static ir_func_t *
bounded_target(n00bb_link_analysis_t *analysis, const ir_instr_t *call,
               const core_buffer_t *tags, core_string_t label)
{
    ir_func_t *owner = call->block ? call->block->owner : nullptr;
    th_type *signature = ir_instr_call_signature(call);
    if (!tags->byte_len || !owner || !signature
        || th_kind_of(th_resolve(signature)) != TH_FUNCTION) return nullptr;
    ir_module_t *module = owner->owner;
    signature = th_resolve(signature);
    core_buffer_t name = {0};
    core_buffer_printf(&name, "__n00bb_callback_bound#%016llx",
        (unsigned long long)(core_xxh3_64bits_raw(tags->data, tags->byte_len)
            ^ core_xxh3_64bits_raw(label.data, label.u8_bytes)
            ^ th_hash(ir_module_types(module), signature)));
    ir_func_t *target = ir_func_find(module, core_buffer_string(&name));
    if (!target) {
        target = ir_func_add(module, core_buffer_string(&name), signature,
                             IR_LINK_EXTERN);
        for (size_t p = 0; target && p < th_func_nparams(signature); p++) {
            core_buffer_t parameter = {0};
            core_buffer_printf(&parameter, "arg%zu", p);
            (void)ir_param_add(target, core_buffer_string(&parameter),
                               th_func_param(signature, p)->type);
            core_buffer_release(&parameter);
        }
        for (core_string_t rest = core_buffer_string(tags);
             target && rest.u8_bytes;) {
            int64_t end = core_string_find_byte(rest, '\n');
            size_t length = end < 0 ? rest.u8_bytes : (size_t)end;
            (void)ir_func_add_effect(target, core_string_slice(rest, 0,
                                                               length));
            rest = end < 0 ? (core_string_t){0}
                : core_string_slice(rest, length + 1,
                                    rest.u8_bytes - length - 1);
        }
        if (target)
            (void)ir_annot_set(analysis->ir_context, &target->annots,
                (ir_annot_t){.key = n00bb_callback_bound_key,
                             .kind = IR_ANNOT_STR,
                             .s = ir_strdup(analysis->ir_context, label)});
    }
    core_buffer_release(&name);
    return target;
}

static void
push_builtin_bound(n00bb_link_analysis_t *analysis, core_string_t trait,
                   core_string_t method, core_string_t effect)
{
    core_list_push(analysis->trait_bounds, ((n00bb_trait_bound_t){
        .span = IR_SPAN_UNKNOWN, .trait = trait, .qualified = trait,
        .method = method,
        .rule = {.query = N00BB_QUERY_FORBIDS_EFFECT, .effect = effect},
    }));
}

static void
push_operator_bound(n00bb_link_analysis_t *analysis, core_string_t trait,
                    core_string_t method)
{
    push_builtin_bound(analysis, trait, method,
                       CORE_STRING_STATIC("impure"));
}

static void
collect_operator_bounds(n00bb_link_analysis_t *analysis)
{
    push_operator_bound(analysis, CORE_STRING_STATIC("Comparable"),
                        CORE_STRING_STATIC("__cmp"));
    push_operator_bound(analysis, CORE_STRING_STATIC("Equal"),
                        CORE_STRING_STATIC("__equal"));
    push_operator_bound(analysis, CORE_STRING_STATIC("Negate"),
                        CORE_STRING_STATIC("__negate"));
    push_operator_bound(analysis, CORE_STRING_STATIC("Not"),
                        CORE_STRING_STATIC("__not"));
    for (size_t o = 0; o < n00bb_binary_operator_count; o++) {
        const n00bb_binary_operator_t *op = &n00bb_binary_operators[o];
        push_operator_bound(analysis, op->trait, op->method);
        push_operator_bound(analysis, op->in_place_trait, op->in_place);
    }
    push_builtin_bound(analysis, CORE_STRING_STATIC("Finalize"),
                       CORE_STRING_STATIC("__finalize"),
                       CORE_STRING_STATIC("allocates"));
}

static parse_tree_t *
trait_declaration(parse_grammar_t *grammar, parse_sym_entry_t *symbol)
{
    parse_tree_t *node = symbol->kind == PARSE_SYM_TYPEDEF ? symbol->decl_node
                                                          : nullptr;
    for (int depth = 0; node && depth < 2; depth++) {
        if (nt_is(grammar, node, CORE_STRING_STATIC("trait-decl")))
            return node;
        node = parse_tree_parent(node);
    }
    return nullptr;
}

static bool
trait_check_stub(ir_module_t *module, ir_ctx_t *context, core_string_t trait,
                 size_t index, ir_span_t span, core_string_t entry)
{
    core_buffer_t name = {0};
    core_buffer_puts(&name, "__n00bb_trait_check#");
    core_buffer_append_string(&name, trait);
    core_buffer_printf(&name, "#%zu", index);
    ir_func_t *stub = metadata_stub(module, core_buffer_string(&name));
    core_buffer_release(&name);
    if (!stub) return false;
    stub->span = span;
    return ir_annot_set(context, &stub->annots, (ir_annot_t){
            .key = trait_check_key, .kind = IR_ANNOT_STR,
            .s = ir_strdup(context, trait)})
        && ir_func_add_effect(stub, entry);
}

static void
collect_nodes(parse_grammar_t *grammar, parse_tree_t *node,
              core_string_t nonterminal, core_list_t(parse_tree_ptr_t) *out)
{
    if (!node || core_tree_is_leaf(node)) return;
    if (nt_is(grammar, node, nonterminal)) {
        core_list_push(*out, node);
        return;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        collect_nodes(grammar, core_tree_child(node, i), nonterminal, out);
}

static void
collect_operands(parse_grammar_t *grammar, parse_tree_t *node,
                 core_string_t nonterminal,
                 core_list_t(parse_tree_ptr_t) *out)
{
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        collect_nodes(grammar, core_tree_child(node, i), nonterminal, out);
}

static core_string_t
term_method(const core_translation_unit_t *unit, parse_tree_t *term)
{
    return core_tree_num_children(term) > 2
        ? source_text_of(unit, core_tree_child(term, 2)) : (core_string_t){0};
}

static core_string_t
term_word(parse_grammar_t *grammar, const core_translation_unit_t *unit,
          parse_tree_t *term, bool negated)
{
    core_string_t word = source_text_of(unit, child_nt(grammar, term,
        CORE_STRING_STATIC("effect-query-word")));
    if (!negated) return word;
    if (core_string_eq(word, CORE_STRING_STATIC("may_effect")))
        return CORE_STRING_STATIC("forbids_effect");
    if (core_string_eq(word, CORE_STRING_STATIC("forbids_effect")))
        return CORE_STRING_STATIC("may_effect");
    return (core_string_t){0};
}

static parse_tree_t *
strip_negations(parse_grammar_t *grammar, parse_tree_t *unary, bool *negated)
{
    *negated = false;
    for (parse_tree_t *inner = child_nt(grammar, unary,
             CORE_STRING_STATIC("trait-check-unary"));
         inner; inner = child_nt(grammar, unary,
             CORE_STRING_STATIC("trait-check-unary"))) {
        *negated = !*negated;
        unary = inner;
    }
    return child_nt(grammar, unary, CORE_STRING_STATIC("trait-check-term"));
}

static bool
record_conjunction(ir_module_t *module, ir_ctx_t *context,
                   core_translation_unit_t *unit, parse_grammar_t *grammar,
                   int32_t file_id, core_string_t trait, parse_tree_t *check,
                   size_t *index, bool *recorded)
{
    core_list_t(parse_tree_ptr_t) ands = core_list_new(parse_tree_ptr_t);
    core_list_t(parse_tree_ptr_t) unaries = core_list_new(parse_tree_ptr_t);
    collect_operands(grammar, check, CORE_STRING_STATIC("trait-check-and"),
                     &ands);
    if (ands.len == 1)
        collect_operands(grammar, ands.data[0],
                         CORE_STRING_STATIC("trait-check-unary"), &unaries);
    bool simple = ands.len == 1;
    for (size_t u = 0; simple && u < unaries.len; u++) {
        bool negated = false;
        parse_tree_t *term = strip_negations(grammar, unaries.data[u],
                                             &negated);
        simple = term && term_word(grammar, unit, term, negated).u8_bytes;
    }
    bool ok = true;
    for (size_t u = 0; simple && ok && u < unaries.len; u++) {
        bool negated = false;
        parse_tree_t *term = strip_negations(grammar, unaries.data[u],
                                             &negated);
        core_buffer_t entry = {0};
        core_buffer_append_string(&entry, term_word(grammar, unit, term,
                                                    negated));
        core_buffer_putc(&entry, '\t');
        core_buffer_append_string(&entry, term_method(unit, term));
        core_buffer_putc(&entry, '\t');
        core_buffer_append_string(&entry, source_text_of(unit, child_nt(
            grammar, term, CORE_STRING_STATIC("effect-query-name"))));
        ok = trait_check_stub(module, context, trait, (*index)++,
                              tree_span(file_id, term),
                              core_buffer_string(&entry));
        core_buffer_release(&entry);
    }
    core_list_free(ands);
    core_list_free(unaries);
    *recorded = simple;
    return ok;
}

static void
encode_trait_check(parse_grammar_t *grammar, core_translation_unit_t *unit,
                   parse_tree_t *node, core_buffer_t *out,
                   core_string_t *method, bool *mixed)
{
    if (nt_is(grammar, node, CORE_STRING_STATIC("trait-check-unary"))) {
        parse_tree_t *term = child_nt(grammar, node,
                                      CORE_STRING_STATIC("trait-check-term"));
        parse_tree_t *negated = child_nt(grammar, node,
            CORE_STRING_STATIC("trait-check-unary"));
        if (term) {
            core_string_t name = term_method(unit, term);
            if (method->u8_bytes && !core_string_eq(*method, name))
                *mixed = true;
            *method = name;
            core_buffer_append_string(out, term_word(grammar, unit, term,
                                                     false));
            core_buffer_putc(out, ' ');
            core_buffer_append_string(out, source_text_of(unit, child_nt(
                grammar, term, CORE_STRING_STATIC("effect-query-name"))));
            core_buffer_putc(out, ' ');
            return;
        }
        if (negated) core_buffer_puts(out, "N ");
        encode_trait_check(grammar, unit, negated ? negated
            : child_nt(grammar, node, CORE_STRING_STATIC("trait-check")),
            out, method, mixed);
        return;
    }
    bool disjunction = nt_is(grammar, node, CORE_STRING_STATIC("trait-check"));
    core_list_t(parse_tree_ptr_t) operands = core_list_new(parse_tree_ptr_t);
    collect_operands(grammar, node, disjunction
        ? CORE_STRING_STATIC("trait-check-and")
        : CORE_STRING_STATIC("trait-check-unary"), &operands);
    for (size_t o = 1; o < operands.len; o++) {
        if (disjunction) core_buffer_puts(out, "O ");
        else core_buffer_puts(out, "A ");
    }
    for (size_t o = 0; o < operands.len; o++)
        encode_trait_check(grammar, unit, operands.data[o], out, method,
                           mixed);
    core_list_free(operands);
}

static bool
record_trait_expression(ir_module_t *module, ir_ctx_t *context,
                        core_translation_unit_t *unit,
                        parse_grammar_t *grammar, int32_t file_id,
                        core_string_t trait, parse_tree_t *check,
                        size_t index)
{
    core_buffer_t encoded = {0};
    core_string_t method = {0};
    bool mixed = false;
    encode_trait_check(grammar, unit, check, &encoded, &method, &mixed);
    if (mixed) {
        n00bb_report(core_translation_unit_diagnostics(unit), unit,
            N00B_DIAG_TRAIT_CHECK, link_source_location(check),
            N00B_TEXT_TRAIT_CHECK_ONE_METHOD, N00BB_ARGS(ARG_TEXT(trait)));
        core_buffer_release(&encoded);
        return true;
    }
    core_buffer_t entry = {0};
    core_buffer_puts(&entry, "expr\t");
    core_buffer_append_string(&entry, method);
    core_buffer_putc(&entry, '\t');
    core_buffer_append(&entry, encoded.data, encoded.byte_len);
    core_buffer_release(&encoded);
    bool recorded = trait_check_stub(module, context, trait, index,
                                     tree_span(file_id, check),
                                     core_buffer_string(&entry));
    core_buffer_release(&entry);
    return recorded;
}

bool
record_trait_checks(parse_grammar_t *grammar, core_translation_unit_t *unit,
                    ir_module_t *module, ir_ctx_t *context, int32_t file_id)
{
    n00bb_tu_state_t *state = tu_state(unit);
    core_list_t(parse_scope_ptr_t) scopes = module_scopes_in_namespace(
        state, CORE_STRING_STATIC("type"));
    bool ok = true;
    for (size_t s = 0; ok && s < scopes.len; s++)
        for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
             ok && symbol; symbol = symbol->next_in_scope) {
            parse_tree_t *declaration = trait_declaration(grammar, symbol);
            if (!declaration) continue;
            size_t index = 0;
            core_list_t(parse_tree_ptr_t) checks =
                core_list_new(parse_tree_ptr_t);
            collect_nodes(grammar, declaration,
                          CORE_STRING_STATIC("trait-check"), &checks);
            for (size_t c = 0; ok && c < checks.len; c++) {
                bool recorded = false;
                ok = record_conjunction(module, context, unit, grammar,
                                        file_id, symbol->name, checks.data[c],
                                        &index, &recorded);
                if (ok && !recorded)
                    ok = record_trait_expression(module, context, unit,
                        grammar, file_id, symbol->name, checks.data[c],
                        index++);
            }
            core_list_free(checks);
        }
    core_list_free(scopes);
    return ok;
}

static core_string_t
trait_effect_name(n00bb_link_analysis_t *analysis,
                  core_translation_unit_t *unit, core_string_t name)
{
    core_buffer_t qualified = {0};
    core_string_t resolved = !n00bb_effect_named(name)
            && resolve_effect(analysis->grammar, unit, name, &qualified)
        ? core_string_dup(core_buffer_string(&qualified)) : name;
    core_buffer_release(&qualified);
    return resolved;
}

static core_string_t
resolved_expression(n00bb_link_analysis_t *analysis,
                    core_translation_unit_t *unit, core_string_t text)
{
    core_buffer_t out = {0};
    while (text.u8_bytes) {
        int64_t space = core_string_find_byte(text, ' ');
        size_t length = space < 0 ? text.u8_bytes : (size_t)space;
        core_string_t token = core_string_slice(text, 0, length);
        text = space < 0 ? (core_string_t){0}
            : core_string_slice(text, length + 1, text.u8_bytes - length - 1);
        if (!token.u8_bytes) continue;
        core_buffer_append_string(&out, token);
        core_buffer_putc(&out, ' ');
        n00bb_query_t query = n00bb_query_named(token);
        if (query == N00BB_QUERY_NONE) continue;
        space = core_string_find_byte(text, ' ');
        length = space < 0 ? text.u8_bytes : (size_t)space;
        core_string_t name = core_string_slice(text, 0, length);
        text = space < 0 ? (core_string_t){0}
            : core_string_slice(text, length + 1, text.u8_bytes - length - 1);
        if (n00bb_query_about_effect(query)
            && resolve_effect(analysis->grammar, unit, name, &out))
            ;
        else core_buffer_append_string(&out, name);
        core_buffer_putc(&out, ' ');
    }
    core_string_t resolved = core_string_dup(core_buffer_string(&out));
    core_buffer_release(&out);
    return resolved;
}

static void
collect_trait_stub(n00bb_link_analysis_t *analysis,
                   core_translation_unit_t *unit, ir_func_t *stub)
{
    ir_annot_t *trait = ir_annot_get(&stub->annots, trait_check_key);
    core_string_t fields[3];
    if (!trait || trait->kind != IR_ANNOT_STR
        || !tab_fields(ir_func_effect_at(stub, 0), fields, 3)) return;
    core_buffer_t qualified = {0};
    append_qualified(&qualified, unit, trait->s);
    core_list_push(analysis->trait_bounds, ((n00bb_trait_bound_t){
        .unit = unit, .span = stub->span, .trait = trait->s,
        .qualified = core_string_dup(core_buffer_string(&qualified)),
        .method = fields[1],
        .rule = {.query = n00bb_query_named(fields[0]),
                 .effect = core_string_eq(fields[0], CORE_STRING_STATIC("expr"))
                     ? (core_string_t){0}
                     : trait_effect_name(analysis, unit, fields[2])},
        .expression = core_string_eq(fields[0], CORE_STRING_STATIC("expr"))
            ? resolved_expression(analysis, unit, fields[2])
            : (core_string_t){0},
    }));
    core_buffer_release(&qualified);
}

static void
collect_trait_bounds(n00bb_link_analysis_t *analysis)
{
    if (analysis->trait_bounds.len) return;
    collect_operator_bounds(analysis);
    for (size_t u = 0; u < analysis->units.len; u++) {
        ir_module_t *module = cached_ir_module(analysis,
                                               analysis->units.data[u]);
        for (size_t f = 0; module && f < ir_func_count(module); f++)
            collect_trait_stub(analysis, analysis->units.data[u],
                               ir_func_at(module, f));
    }
}

static void
trait_bound_tags(n00bb_link_analysis_t *analysis, core_string_t trait,
                 core_string_t method, core_buffer_t *tags)
{
    collect_trait_bounds(analysis);
    for (size_t b = 0; b < analysis->trait_bounds.len; b++) {
        const n00bb_trait_bound_t *bound = &analysis->trait_bounds.data[b];
        if ((!core_string_eq(bound->trait, trait)
             && !core_string_eq(bound->qualified, trait))
            || !core_string_eq(bound->method, method)
            || (bound->rule.query != N00BB_QUERY_FORBIDS_EFFECT
                && bound->rule.query != N00BB_QUERY_HAS_EFFECT)) continue;
        n00bb_bound_tag(tags, bound->rule.query, 0, bound->rule.effect);
        core_buffer_putc(tags, '\n');
    }
}

static ir_func_t *
find_bounded_target(n00bb_link_analysis_t *analysis, const ir_instr_t *call)
{
    ir_func_t *owner = call->block ? call->block->owner : nullptr;
    const ir_value_t *callee = call->n_operands
        ? passed_value(owner, ir_operand_value(&call->operands[0])) : nullptr;
    const ir_instr_t *method = callee && callee->kind == IR_VAL_INSTR
        ? callee->def : nullptr;
    core_buffer_t tags = {0};
    core_buffer_t label = {0};
    if (method && method->op == IR_OP_TRAIT_METHOD && method->n_operands >= 2
        && method->operands[0].kind == IR_OPD_NAME
        && method->operands[1].kind == IR_OPD_NAME) {
        trait_bound_tags(analysis, method->operands[0].name,
                         method->operands[1].name, &tags);
        core_buffer_append_string(&label, method->operands[0].name);
        core_buffer_putc(&label, '.');
        core_buffer_append_string(&label, method->operands[1].name);
    }
    core_string_t hook_trait = {0}, hook_method = {0};
    if (!method && n00bb_open_hook(call, &hook_trait, &hook_method)) {
        trait_bound_tags(analysis, hook_trait, hook_method, &tags);
        core_buffer_append_string(&label, hook_trait);
        core_buffer_putc(&label, '.');
        core_buffer_append_string(&label, hook_method);
    }
    for (uint32_t e = 0; callee && callee->kind == IR_VAL_PARAM; e++) {
        core_string_t tag = ir_func_effect_at(owner, e);
        if (!tag.u8_bytes) break;
        n00bb_bound_t bound;
        if (!n00bb_bound_parse(tag, &bound) || bound.param != callee->index)
            continue;
        core_buffer_append_string(&tags, tag);
        core_buffer_putc(&tags, '\n');
    }
    ir_func_t *target = bounded_target(analysis, call, &tags,
                                       core_buffer_string(&label));
    core_buffer_release(&tags);
    core_buffer_release(&label);
    return target;
}

bool
bounded_call_target(n00bb_link_analysis_t *analysis, const ir_instr_t *call,
                    ir_summary_target_fn add, void *sink)
{
    bool found = false;
    ir_func_t *target = core_dict_get(&analysis->bounded_targets,
                                      (void *)call, &found);
    if (!found) {
        target = find_bounded_target(analysis, call);
        core_dict_put(&analysis->bounded_targets, (void *)call, target);
    }
    if (target) add(sink, target);
    return target != nullptr;
}

typedef struct {
    n00bb_link_analysis_t *analysis;
    const n00bb_effect_check_t *check;
    core_diagnostics_t *diagnostics;
    size_t explained;
} check_eval_t;

static bool
check_operator(parse_grammar_t *grammar, parse_tree_t *node)
{
    return nt_is(grammar, node, CORE_STRING_STATIC("and-op"))
        || nt_is(grammar, node, CORE_STRING_STATIC("or-op"))
        || nt_is(grammar, node, CORE_STRING_STATIC("not-op"));
}

static size_t
check_operands(parse_grammar_t *grammar, parse_tree_t *node,
               parse_tree_t **operands, bool *negated)
{
    size_t count = 0;
    *negated = false;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (core_tree_is_leaf(child)) continue;
        if (nt_is(grammar, child, CORE_STRING_STATIC("not-op")))
            *negated = true;
        if (check_operator(grammar, child)) continue;
        if (count < 2) operands[count] = child;
        count++;
    }
    return count;
}

static bool
check_query_value(check_eval_t *eval, parse_tree_t *node, ir_func_t **subject,
                  effect_query_parts_t *parts)
{
    parse_grammar_t *grammar = eval->analysis->grammar;
    *subject = nullptr;
    if (!effect_query_parts(grammar, eval->check->unit, node, parts))
        return false;
    if (callback_parameter(grammar, eval->check->unit, node, parts) >= 0)
        return true;
    if (parts->query == N00BB_QUERY_MAY_CARRY) {
        bool found = false;
        bool answer = false;
        for (size_t q = 0; q < eval->analysis->mark_questions.len; q++)
            if (eval->analysis->mark_questions.data[q].node == node) {
                found = true;
                answer = answer || eval->analysis->mark_questions.data[q].answer;
            }
        return answer || !found;
    }
    *subject = effect_query_subject(eval->analysis, grammar,
                                    eval->check->unit, node, parts);
    core_buffer_t effect = {0};
    core_string_t name = n00bb_query_about_effect(parts->query)
            && resolve_effect(grammar, eval->check->unit, parts->name, &effect)
        ? core_buffer_string(&effect) : parts->name;
    bool answer = answer_query(eval->analysis, *subject, parts->query, name);
    core_buffer_release(&effect);
    return answer;
}

static bool
check_value(check_eval_t *eval, parse_tree_t *node)
{
    parse_grammar_t *grammar = eval->analysis->grammar;
    if (core_tree_is_leaf(node))
        return core_string_eq(source_text_of(eval->check->unit, node),
                              CORE_STRING_STATIC("true"));
    if (nt_is(grammar, node, CORE_STRING_STATIC("effect-query"))) {
        ir_func_t *subject = nullptr;
        effect_query_parts_t parts;
        return check_query_value(eval, node, &subject, &parts);
    }
    parse_tree_t *operands[2] = {0};
    bool negated = false;
    size_t count = check_operands(grammar, node, operands, &negated);
    if (negated) return !check_value(eval, operands[0]);
    if (count == 2 && nt_is(grammar, node, CORE_STRING_STATIC("and-expr")))
        return check_value(eval, operands[0])
            && check_value(eval, operands[1]);
    if (count == 2 && nt_is(grammar, node, CORE_STRING_STATIC("or-expr")))
        return check_value(eval, operands[0])
            || check_value(eval, operands[1]);
    if (count == 1) return check_value(eval, operands[0]);
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (core_tree_is_leaf(core_tree_child(node, i)))
            return check_value(eval, core_tree_child(node, i));
    return false;
}

static void
report_false_query(check_eval_t *eval, parse_tree_t *node, core_string_t piece)
{
    if (!eval->explained++)
        n00bb_report(eval->diagnostics, eval->check->unit,
            N00B_DIAG_EFFECT_CHECK_FALSE,
            n00bb_node_span(eval->check->statement),
            N00B_TEXT_CHECK_IS_FALSE, N00BB_ARGS(ARG_TEXT(piece)));
    else
        n00bb_report_secondary(eval->diagnostics, eval->check->unit,
            n00bb_node_span(node), N00B_TEXT_CHECK_ALSO_FALSE,
            N00BB_ARGS(ARG_TEXT(piece)));
    core_free(piece.data);
}

static void
explain_query(check_eval_t *eval, parse_tree_t *node, bool value)
{
    ir_func_t *subject = nullptr;
    effect_query_parts_t parts;
    (void)check_query_value(eval, node, &subject, &parts);
    if (parts.query == N00BB_QUERY_MAY_CARRY) {
        report_false_query(eval, node, n00bb_text(
            value ? N00B_TEXT_MAY_CARRY_HERE : N00B_TEXT_DOES_NOT_CARRY_HERE,
            N00BB_ARGS(ARG_TEXT(source_text_of(eval->check->unit,
                                               parts.function)),
                       ARG_TEXT(parts.name))));
        return;
    }
    if (!subject) return;
    core_string_t function = n00bb_function_root(subject->name);
    bool present = parts.query == N00BB_QUERY_FORBIDS_EFFECT ? !value : value;
    const n00bb_effect_name_t *effect = n00bb_effect_named(parts.name);
    ir_summary_t *summary = ir_summary_of(eval->analysis->summaries, subject);
    uint64_t bits = summary ? ir_summary_effect_bits(summary) : 0;
    for (size_t i = 0; present && effect && effect->group
         && i < n00bb_effect_name_count(); i++) {
        const n00bb_effect_name_t *member = n00bb_effect_name_at(i);
        if (!member->group && (member->bits & effect->bits & bits))
            effect = member;
    }
    if (effect) parts.name = effect->name;
    core_string_t word = n00bb_query_word(parts.query);
    core_string_t verb = core_string_slice(word, 4, word.u8_bytes - 4);
    n00bb_text_t text;
    n00bb_args_t args = N00BB_ARGS(ARG_TEXT(function), ARG_TEXT(parts.name));
    switch (parts.query) {
    case N00BB_QUERY_MAY_EFFECT:
    case N00BB_QUERY_FORBIDS_EFFECT:
        text = present ? N00B_TEXT_QUERY_MAY_DO : N00B_TEXT_QUERY_NEVER_DOES;
        break;
    case N00BB_QUERY_HAS_EFFECT:
        if (effect && effect->bits == IR_SUMMARY_MAY_DIVERGE)
            text = value ? N00B_TEXT_QUERY_NEVER_COMES_BACK
                         : N00B_TEXT_QUERY_COMES_BACK;
        else text = value ? N00B_TEXT_QUERY_ALWAYS_DOES
                          : N00B_TEXT_QUERY_NOT_ALWAYS;
        break;
    default:
        text = value ? N00B_TEXT_QUERY_MAY_ROLE : N00B_TEXT_QUERY_NEVER_ROLE;
        args = N00BB_ARGS(ARG_TEXT(function), ARG_TEXT(verb),
                          ARG_TEXT(parts.name));
        break;
    }
    report_false_query(eval, node, n00bb_text(text, args));
    if (parts.query == N00BB_QUERY_HAS_EFFECT && !value
        && !(effect && effect->bits == IR_SUMMARY_MAY_DIVERGE)) {
        core_buffer_t qualified = {0};
        bool named = resolve_effect(eval->analysis->grammar,
                                    eval->check->unit, parts.name, &qualified);
        const ir_instr_t *exit = named ? n00bb_must_skip(
            eval->analysis->summaries, subject, effect ? effect->bits : 0,
            user_effect(eval->analysis, core_buffer_string(&qualified)))
            : nullptr;
        core_buffer_release(&qualified);
        core_string_t exit_word = exit && exit->op == IR_OP_FAIL
            ? CORE_STRING_STATIC("fail") : CORE_STRING_STATIC("return");
        if (exit && user_span(exit))
            n00bb_report_secondary(eval->diagnostics, eval->check->unit,
                n00bb_span_location(exit->span), N00B_TEXT_EXIT_WITHOUT_EFFECT,
                N00BB_ARGS(ARG_TEXT(exit_word)));
        else if (exit) {
            ir_span_t end = subject->span;
            end.start_line = end.end_line;
            end.start_col = end.end_col;
            end.start_byte = end.end_byte;
            n00bb_report_secondary(eval->diagnostics, eval->check->unit,
                n00bb_span_location(end), N00B_TEXT_END_WITHOUT_EFFECT,
                N00BB_ARGS(ARG_TEXT(function), ARG_TEXT(exit_word)));
        }
    }
    if (!present || !summary
        || (parts.query != N00BB_QUERY_MAY_EFFECT
            && parts.query != N00BB_QUERY_FORBIDS_EFFECT))
        return;
    if (!effect) {
        core_buffer_t qualified = {0};
        if (resolve_effect(eval->analysis->grammar, eval->check->unit,
                           parts.name, &qualified))
            named_chain_notes(eval->analysis, eval->diagnostics, subject,
                              core_buffer_string(&qualified));
        core_buffer_release(&qualified);
        return;
    }
    explain_chain_notes(eval->analysis, eval->diagnostics, subject,
                        bound_witness_flag(summary, parts.name));
}

static void
explain_check(check_eval_t *eval, parse_tree_t *node, bool want)
{
    parse_grammar_t *grammar = eval->analysis->grammar;
    if (core_tree_is_leaf(node)) return;
    if (nt_is(grammar, node, CORE_STRING_STATIC("effect-query"))) {
        bool value = check_value(eval, node);
        if (value != want) explain_query(eval, node, value);
        return;
    }
    parse_tree_t *operands[2] = {0};
    bool negated = false;
    size_t count = check_operands(grammar, node, operands, &negated);
    if (negated) {
        explain_check(eval, operands[0], !want);
        return;
    }
    bool conjunction = nt_is(grammar, node, CORE_STRING_STATIC("and-expr"));
    bool disjunction = nt_is(grammar, node, CORE_STRING_STATIC("or-expr"));
    if (count == 2 && (conjunction || disjunction)) {
        for (size_t i = 0; i < 2; i++)
            if (conjunction != want
                || check_value(eval, operands[i]) != want)
                explain_check(eval, operands[i], want);
        return;
    }
    if (count == 1) explain_check(eval, operands[0], want);
}

typedef struct {
    n00bb_link_analysis_t *analysis;
    const n00bb_effect_check_t *check;
    core_list_t(n00bb_ir_func_ptr_t) *roots;
} check_subjects_t;

static bool
push_check_subject(void *context, parse_tree_t *query,
                   effect_query_parts_t *parts)
{
    check_subjects_t *subjects = context;
    ir_func_t *subject = effect_query_subject(subjects->analysis,
        subjects->analysis->grammar, subjects->check->unit, query, parts);
    if (subject && subject->n_blocks) core_list_push(*subjects->roots, subject);
    return true;
}

void
push_effect_check_subjects(n00bb_link_analysis_t *analysis,
                           core_list_t(n00bb_ir_func_ptr_t) *roots)
{
    for (size_t c = 0; c < analysis->effect_checks.len; c++) {
        check_subjects_t subjects = {
            .analysis = analysis, .check = &analysis->effect_checks.data[c],
            .roots = roots,
        };
        (void)for_each_effect_query(analysis->grammar, subjects.check->unit,
                                    subjects.check->expression,
                                    push_check_subject, &subjects);
    }
}

bool
evaluate_effect_checks(n00bb_link_analysis_t *analysis)
{
    bool holds = true;
    for (size_t c = 0; c < analysis->effect_checks.len; c++) {
        const n00bb_effect_check_t *check = &analysis->effect_checks.data[c];
        check_eval_t eval = {
            .analysis = analysis, .check = check,
            .diagnostics = core_translation_unit_diagnostics(check->unit),
        };
        if (check_value(&eval, check->expression)) continue;
        explain_check(&eval, check->expression, true);
        n00bb_report_help(eval.diagnostics, N00B_TEXT_CHANGE_THE_CODE_OR_CHECK,
                          N00BB_NO_ARGS);
        holds = false;
    }
    analysis->effect_checks.len = 0;
    return holds;
}

static bool
unresolved_only(n00bb_link_analysis_t *analysis, ir_func_t *function,
                core_string_t effect_name)
{
    if (analysis->closed_world && !analysis->may_link_further) return false;
    ir_summary_t *summary = ir_summary_of(analysis->summaries, function);
    const n00bb_effect_name_t *effect = n00bb_effect_named(effect_name);
    if (!summary || !effect) return false;
    uint32_t flags = ir_summary_flags(summary);
    uint32_t violating = (uint32_t)(effect->bits & flags
                                    & IR_SEFFECT_FLAG_MASK);
    if (!violating && (flags & IR_SUMMARY_CALLS_UNKNOWN))
        violating = IR_SWITNESS_UNKNOWN;
    for (uint32_t rest = violating; rest; rest &= rest - 1) {
        const ir_switness_t *chain[WITNESS_DEPTH];
        uint32_t depth = witness_chain(analysis, summary, rest & -rest,
                                       chain);
        if (!depth || chain[depth - 1]->callee
            || !core_string_eq(chain[depth - 1]->name,
                               CORE_STRING_STATIC("?")))
            return false;
    }
    return violating != 0;
}

static bool
bound_holds(n00bb_link_analysis_t *analysis, ir_func_t *function,
            const n00bb_bound_t *bound)
{
    return answer_query(analysis, function, bound->query, bound->effect)
        || (bound->query == N00BB_QUERY_FORBIDS_EFFECT
            && unresolved_only(analysis, function, bound->effect));
}

static void
explain_bound(n00bb_link_analysis_t *analysis, core_diagnostics_t *diagnostics,
              ir_func_t *function, const n00bb_bound_t *bound)
{
    if (bound->query != N00BB_QUERY_FORBIDS_EFFECT) return;
    if (!n00bb_effect_named(bound->effect)) {
        named_chain_notes(analysis, diagnostics, function, bound->effect);
        return;
    }
    explain_chain_notes(analysis, diagnostics, function,
        bound_witness_flag(ir_summary_of(analysis->summaries, function),
                           bound->effect));
}

static bool
forwards_bound(ir_func_t *caller, uint32_t param, const n00bb_bound_t *bound)
{
    for (uint32_t e = 0;; e++) {
        core_string_t tag = ir_func_effect_at(caller, e);
        if (!tag.u8_bytes) return false;
        n00bb_bound_t own;
        if (n00bb_bound_parse(tag, &own) && own.param == param
            && own.query == bound->query
            && core_string_eq(own.effect, bound->effect)) return true;
    }
}

static bool
check_callback_argument(n00bb_link_analysis_t *analysis, ir_func_t *caller,
                        const ir_instr_t *call, ir_func_t *callee,
                        const n00bb_bound_t *bound)
{
    const ir_value_t *argument = bound->param + 1 < call->n_operands
        ? passed_value(caller,
                       ir_operand_value(&call->operands[bound->param + 1]))
        : nullptr;
    if (!argument) return true;
    ir_func_t *handler = argument->kind == IR_VAL_INSTR && argument->def
        ? callback_handler(argument->def) : nullptr;
    if (!handler && argument->kind == IR_VAL_FUNC && argument->owner)
        handler = n00bb_cached_body(argument->owner);
    bool meets = handler ? bound_holds(analysis, handler, bound)
        : argument->kind == IR_VAL_PARAM
            && forwards_bound(caller, argument->index, bound);
    if (meets) return true;
    core_translation_unit_t *unit = unit_for_module(analysis, caller->owner);
    core_diagnostics_t *diagnostics = unit
        ? core_translation_unit_diagnostics(unit) : nullptr;
    if (!diagnostics) return false;
    core_buffer_t parameter = {0};
    param_text(&parameter, callee, bound->param);
    core_string_t name = n00bb_function_root(callee->name);
    n00bb_text_t text = !handler ? N00B_TEXT_CALLBACK_UNKNOWN_TARGET
        : bound->query == N00BB_QUERY_HAS_EFFECT
        ? N00B_TEXT_CALLBACK_NOT_ALWAYS : N00B_TEXT_CALLBACK_MAY_DO;
    n00bb_report(diagnostics, unit, N00B_DIAG_CALLBACK_BOUND,
        n00bb_span_location(call->span), text,
        N00BB_ARGS(ARG_TEXT(name), ARG_TEXT(core_buffer_string(&parameter)),
                   ARG_TEXT(bound->effect)));
    core_buffer_release(&parameter);
    if (handler) explain_bound(analysis, diagnostics, handler, bound);
    n00bb_report_help(diagnostics, N00B_TEXT_PASS_A_CHECKED_CALLBACK,
                      N00BB_NO_ARGS);
    return false;
}

static bool
implements_method(th_ctx *types, ir_func_t *function, core_string_t root,
                  const n00bb_trait_bound_t *bound)
{
    if (!core_string_eq(root, bound->method) || !function->n_params
        || !function->params[0]) return false;
    th_env_method_t found = {0};
    return th_env_method_resolve(types, function->params[0]->type,
                                 bound->trait, bound->method, &found)
            == TH_ENV_METHOD_FOUND
        || th_env_method_resolve(types, function->params[0]->type,
                                 bound->qualified, bound->method, &found)
            == TH_ENV_METHOD_FOUND;
}

typedef struct {
    core_translation_unit_t   *unit;
    ir_func_t                 *function;
    const n00bb_trait_bound_t *bound;
} trait_violation_t;
core_list_decl(trait_violation_t);

static bool
through_violation(n00bb_link_analysis_t *analysis, core_dict_t *violators,
                  const trait_violation_t *violation)
{
    ir_summary_t *summary = ir_summary_of(analysis->summaries,
                                          violation->function);
    const ir_switness_t *chain[WITNESS_DEPTH];
    uint32_t depth = witness_chain(analysis, summary,
        bound_witness_flag(summary, violation->bound->rule.effect), chain);
    for (uint32_t d = 0; d < depth; d++) {
        ir_func_t *callee = chain[d]->callee
            ? n00bb_cached_body(chain[d]->callee) : nullptr;
        bool found = false;
        if (callee && callee != violation->function)
            (void)core_dict_get(violators, callee, &found);
        if (found) return true;
    }
    return false;
}

static core_string_t
next_token(core_string_t *rest)
{
    while (rest->u8_bytes && rest->data[0] == ' ')
        *rest = core_string_slice(*rest, 1, rest->u8_bytes - 1);
    int64_t space = core_string_find_byte(*rest, ' ');
    size_t length = space < 0 ? rest->u8_bytes : (size_t)space;
    core_string_t token = core_string_slice(*rest, 0, length);
    *rest = core_string_slice(*rest, length, rest->u8_bytes - length);
    return token;
}

static bool
evaluate_trait_check(n00bb_link_analysis_t *analysis,
                     const n00bb_trait_bound_t *bound, ir_func_t *function,
                     core_string_t *rest)
{
    core_string_t kind = next_token(rest);
    if (core_string_eq(kind, CORE_STRING_STATIC("N")))
        return !evaluate_trait_check(analysis, bound, function, rest);
    if (core_string_eq(kind, CORE_STRING_STATIC("A"))
        || core_string_eq(kind, CORE_STRING_STATIC("O"))) {
        bool left = evaluate_trait_check(analysis, bound, function, rest);
        bool right = evaluate_trait_check(analysis, bound, function, rest);
        return kind.data[0] == 'A' ? left && right : left || right;
    }
    n00bb_bound_t leaf = {.query = n00bb_query_named(kind),
                          .effect = next_token(rest)};
    return bound_holds(analysis, function, &leaf);
}

static bool
trait_bound_holds(n00bb_link_analysis_t *analysis,
                  const n00bb_trait_bound_t *bound, ir_func_t *function)
{
    core_string_t rest = bound->expression;
    return bound->expression.u8_bytes
        ? evaluate_trait_check(analysis, bound, function, &rest)
        : bound_holds(analysis, function, &bound->rule);
}

static void
report_implementation(n00bb_link_analysis_t *analysis,
                      const trait_violation_t *violation)
{
    const n00bb_trait_bound_t *bound = violation->bound;
    core_diagnostics_t *diagnostics =
        core_translation_unit_diagnostics(violation->unit);
    n00bb_report(diagnostics, violation->unit, N00B_DIAG_TRAIT_CHECK,
        n00bb_span_location(violation->function->span),
        bound->expression.u8_bytes ? N00B_TEXT_IMPLEMENTATION_CHECK_FALSE
        : bound->rule.query == N00BB_QUERY_HAS_EFFECT
            ? N00B_TEXT_IMPLEMENTATION_NOT_ALWAYS
            : N00B_TEXT_IMPLEMENTATION_MAY_DO,
        N00BB_ARGS(ARG_TEXT(n00bb_function_root(violation->function->name)),
                   ARG_TEXT(bound->trait), ARG_TEXT(bound->method),
                   ARG_TEXT(bound->rule.effect)));
    explain_bound(analysis, diagnostics, violation->function, &bound->rule);
    if (ir_span_known(bound->span))
        n00bb_report_secondary(diagnostics, bound->unit,
            n00bb_span_location(bound->span), N00B_TEXT_TRAIT_CHECKS_HERE,
            N00BB_ARGS(ARG_TEXT(bound->trait)));
}

static void
check_trait_candidate(n00bb_link_analysis_t *analysis, ir_func_t *function,
                      core_dict_t *seen,
                      core_list_t(trait_violation_t) *violations,
                      core_dict_t *violators)
{
    if (!function || !function->n_blocks
        || !core_dict_add(seen, function, function)) return;
    core_translation_unit_t *owner = unit_for_module(analysis,
                                                     function->owner);
    n00bb_tu_state_t *types = owner ? tu_state(owner) : nullptr;
    if (!types || !types->types || types->interface_module) return;
    core_string_t root = n00bb_function_root(function->name);
    for (size_t b = 0; b < analysis->trait_bounds.len; b++) {
        const n00bb_trait_bound_t *bound = &analysis->trait_bounds.data[b];
        if (!implements_method(types->types, function, root, bound)
            || trait_bound_holds(analysis, bound, function))
            continue;
        core_list_push(*violations, ((trait_violation_t){
            .unit = owner, .function = function, .bound = bound}));
        core_dict_put(violators, function, function);
    }
}

bool
check_trait_implementations(n00bb_link_analysis_t *analysis)
{
    collect_trait_bounds(analysis);
    core_list_t(trait_violation_t) violations =
        core_list_new(trait_violation_t);
    core_dict_t violators, seen;
    core_dict_init(&violators, nullptr, nullptr);
    core_dict_init(&seen, nullptr, nullptr);
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        if (!state || !state->ir_module || state->interface_module) continue;
        th_ctx *types = ir_module_types(state->ir_module);
        for (size_t f = 0; f < ir_func_count(state->ir_module); f++) {
            ir_func_t *function = n00bb_cached_body(
                ir_func_at(state->ir_module, f));
            if (function && th_is_closed(types, function->type))
                check_trait_candidate(analysis, function, &seen, &violations,
                                      &violators);
        }
    }
    for (size_t f = 0; f < analysis->reachable_functions.len; f++)
        check_trait_candidate(analysis,
            n00bb_cached_body(analysis->reachable_functions.data[f]), &seen,
            &violations, &violators);
    core_dict_free(&seen);
    for (size_t v = 0; v < violations.len; v++) {
        const trait_violation_t *violation = &violations.data[v];
        if (violation->bound->rule.query == N00BB_QUERY_FORBIDS_EFFECT
            && through_violation(analysis, &violators, violation))
            continue;
        report_implementation(analysis, violation);
    }
    bool holds = violations.len == 0;
    core_dict_free(&violators);
    core_list_free(violations);
    return holds;
}

static bool
check_callback_caller(n00bb_link_analysis_t *analysis, ir_func_t *caller,
                      core_dict_t *seen)
{
    if (!caller || !caller->n_blocks || !core_dict_add(seen, caller, caller))
        return true;
    core_translation_unit_t *owner = unit_for_module(analysis, caller->owner);
    if (!owner || tu_state(owner)->interface_module) return true;
    bool holds = true;
    for (uint32_t b = 0; b < caller->n_blocks; b++)
        for (uint32_t i = 0; i < caller->blocks[b]->n_instrs; i++) {
            ir_instr_t *call = caller->blocks[b]->instrs[i];
            ir_func_t *callee = n00bb_direct_call_target(call);
            ir_summary_t *summary = callee
                ? ir_summary_for(analysis->summaries, callee) : nullptr;
            for (uint32_t e = 0; summary
                 && e < ir_summary_effect_count(summary); e++) {
                n00bb_bound_t bound;
                if (n00bb_bound_parse(ir_summary_effect_at(summary, e),
                                      &bound))
                    holds = check_callback_argument(analysis, caller, call,
                                                    callee, &bound) && holds;
            }
        }
    return holds;
}

bool
check_callback_bounds(n00bb_link_analysis_t *analysis)
{
    bool holds = true;
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        if (!state || !state->ir_module || state->interface_module) continue;
        th_ctx *types = ir_module_types(state->ir_module);
        for (size_t f = 0; f < ir_func_count(state->ir_module); f++) {
            ir_func_t *caller = n00bb_cached_body(
                ir_func_at(state->ir_module, f));
            if (caller && th_is_closed(types, caller->type))
                holds = check_callback_caller(analysis, caller, &seen)
                    && holds;
        }
    }
    for (size_t f = 0; f < analysis->reachable_functions.len; f++)
        holds = check_callback_caller(analysis,
            n00bb_cached_body(analysis->reachable_functions.data[f]), &seen)
            && holds;
    core_dict_free(&seen);
    return holds;
}

typedef struct {
    ir_span_t span;
    uint32_t  param;
} mark_site_t;
core_list_decl(mark_site_t);

typedef struct {
    n00bb_link_analysis_t     *analysis;
    core_list_t(core_string_t) names;
    core_list_t(mark_site_t)   reported;
    n00bb_marks_t              marks;
    size_t                     violations;
} mark_check_t;

static bool
parse_rule(mark_check_t *check, core_string_t module, core_string_t name,
           core_string_t index, uint64_t bit, n00bb_mark_rule_t *rule)
{
    ir_func_t *function = module_function(check->analysis, module, name);
    uint64_t param = 0;
    size_t consumed = 0;
    if (!function
        || (index.u8_bytes
            && (!core_string_parse_u64(index, 10, &param, &consumed)
                || consumed != index.u8_bytes)))
        return false;
    *rule = (n00bb_mark_rule_t){.function = function,
                                .param = (uint32_t)param, .marks = bit};
    return true;
}

static void
collect_mark_rules(mark_check_t *check, ir_func_t *stub, uint64_t bit)
{
    for (uint32_t e = 0;; e++) {
        core_string_t entry = ir_func_effect_at(stub, e);
        core_string_t fields[4];
        if (!entry.u8_bytes) break;
        bool from = core_string_starts_with(entry, CORE_STRING_STATIC("from\t"));
        n00bb_mark_rule_t rule;
        if (!tab_fields(entry, fields, from ? 3 : 4)
            || !parse_rule(check, fields[1], fields[2],
                           from ? (core_string_t){0} : fields[3], bit, &rule))
            continue;
        if (from) core_list_push(check->marks.sources, rule);
        else if (core_string_eq(fields[0], CORE_STRING_STATIC("cleared")))
            core_list_push(check->marks.clears, rule);
        else core_list_push(check->marks.rejects, rule);
    }
}

static void
collect_mark(void *context, core_translation_unit_t *unit, ir_func_t *stub,
             core_string_t declared)
{
    mark_check_t *check = context;
    if (check->names.len >= 64) {
        n00bb_report(core_translation_unit_diagnostics(unit), unit,
            N00B_DIAG_MARK_DECLARATION, n00bb_span_location(stub->span),
            N00B_TEXT_TOO_MANY_MARKS, N00BB_ARGS(ARG_TEXT(declared)));
        check->violations++;
        return;
    }
    uint64_t bit = 1ull << check->names.len;
    core_list_push(check->names, declared);
    collect_mark_rules(check, stub, bit);
}

static uint64_t
mark_bit(const mark_check_t *check, core_string_t qualified)
{
    for (size_t m = 0; m < check->names.len; m++)
        if (core_string_eq(check->names.data[m], qualified))
            return 1ull << m;
    return 0;
}

static void
collect_annotation_rules(void *context, core_translation_unit_t *unit,
                         ir_func_t *stub, core_string_t declared)
{
    (void)unit;
    (void)declared;
    mark_check_t *check = context;
    for (uint32_t e = 0;; e++) {
        core_string_t entry = ir_func_effect_at(stub, e);
        core_string_t fields[6];
        if (!entry.u8_bytes) break;
        if (!tab_fields(entry, fields, 3)) continue;
        uint64_t bit = mark_bit(check, fields[1]);
        if (!bit) continue;
        bool rejects = core_string_eq(fields[0],
                                      CORE_STRING_STATIC("rejects"));
        n00bb_mark_rule_t rule;
        if (core_string_starts_with(fields[2], CORE_STRING_STATIC("param\t"))
            && tab_fields(entry, fields, 6)) {
            if (!parse_rule(check, fields[3], fields[4], fields[5], bit,
                            &rule)) continue;
            if (rejects) core_list_push(check->marks.rejects, rule);
            else core_list_push(check->marks.seeds, rule);
        }
        else if (core_string_starts_with(fields[2],
                                         CORE_STRING_STATIC("field\t"))
                 && tab_fields(entry, fields, 5))
            core_list_push(check->marks.carriers, ((n00bb_mark_carrier_t){
                .type = fields[3], .field = fields[4], .marks = bit}));
        else if (core_string_starts_with(fields[2],
                                         CORE_STRING_STATIC("type\t"))
                 && tab_fields(entry, fields, 4))
            core_list_push(check->marks.carriers, ((n00bb_mark_carrier_t){
                .type = fields[3], .marks = bit}));
    }
}

static void
collect_marks(mark_check_t *check)
{
    for_each_stub(check->analysis, CORE_STRING_STATIC("__n00bb_mark#"),
                  mark_declaration_key, collect_mark, check);
    for_each_stub(check->analysis, CORE_STRING_STATIC("__n00bb_mark_rule#"),
                  mark_rules_key, collect_annotation_rules, check);
}

static void
mark_names(const mark_check_t *check, uint64_t marks, core_buffer_t *out)
{
    for (size_t m = 0; m < check->names.len; m++)
        if (marks >> m & 1) {
            if (out->byte_len) core_buffer_puts(out, ", ");
            core_buffer_append_string(out,
                                      n00bb_name_leaf(check->names.data[m]));
        }
}

static core_string_t
local_source_name(const ir_value_t *value)
{
    ir_annots_t *annots = value ? ir_value_annots((ir_value_t *)value)
                                : nullptr;
    ir_annot_t *name = annots
        ? ir_annot_get(annots, IR_SOURCE_NAME_KEY)
        : nullptr;
    return name && name->kind == IR_ANNOT_STR ? name->s : (core_string_t){0};
}

static core_string_t
value_name(const ir_value_t *value)
{
    for (int depth = 0; value && depth < 32; depth++) {
        if (value->kind == IR_VAL_PARAM) return value->name;
        core_string_t name = local_source_name(value);
        if (name.u8_bytes) return name;
        const ir_instr_t *def = value->kind == IR_VAL_INSTR ? value->def
                                                           : nullptr;
        value = def && def->op == IR_OP_COPY && def->n_operands
            ? ir_operand_value(&def->operands[0]) : nullptr;
    }
    return CORE_STRING_STATIC("this argument");
}

static bool
call_target_text(core_buffer_t *out, const ir_instr_t *call)
{
    ir_func_t *target = call ? n00bb_direct_call_target(call) : nullptr;
    out->byte_len = 0;
    if (target) function_name_text(out, target);
    return target != nullptr;
}

static bool
already_reported(mark_check_t *check, const n00bb_mark_violation_t *violation)
{
    ir_span_t span = violation->call->span;
    for (size_t r = 0; r < check->reported.len; r++) {
        const mark_site_t *site = &check->reported.data[r];
        if (site->param == violation->param
            && site->span.file == span.file
            && site->span.start_byte == span.start_byte
            && site->span.end_byte == span.end_byte)
            return true;
    }
    core_list_push(check->reported, ((mark_site_t){
        .span = span, .param = violation->param}));
    return false;
}

static void
report_mark_violation(void *context, const n00bb_mark_violation_t *violation)
{
    mark_check_t *check = context;
    n00bb_link_analysis_t *analysis = check->analysis;
    core_translation_unit_t *unit = unit_for_module(analysis,
                                                    violation->caller->owner);
    n00bb_tu_state_t *state = unit ? tu_state(unit) : nullptr;
    if (!state || state->interface_module
        || already_reported(check, violation)) return;
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_buffer_t marks = {0};
    mark_names(check, violation->marks, &marks);
    core_buffer_t callee = {0};
    function_name_text(&callee, violation->callee);
    core_string_t value = value_name(violation->argument);
    core_string_t param = violation->param < violation->callee->n_params
            && violation->callee->params[violation->param]
        ? violation->callee->params[violation->param]->name
        : CORE_STRING_STATIC("?");
    n00bb_report(diagnostics, unit, N00B_DIAG_MARK_REJECTED,
        n00bb_span_location(violation->call->span),
        N00B_TEXT_MAY_CARRY_REJECTED,
        N00BB_ARGS(ARG_TEXT(value), ARG_TEXT(core_buffer_string(&marks)),
                   ARG_TEXT(param), ARG_TEXT(core_buffer_string(&callee))));
    check->violations++;
    core_buffer_t text = {0};
    if (call_target_text(&text, violation->origin))
        n00bb_report_secondary(diagnostics, nullptr,
            n00bb_span_location(violation->origin->span),
            N00B_TEXT_MARK_COMES_FROM,
            N00BB_ARGS(ARG_TEXT(value), ARG_TEXT(core_buffer_string(&text))));
    if (violation->carrier && !violation->by_type)
        n00bb_report_secondary(diagnostics, nullptr,
            n00bb_span_location(violation->caller->span),
            N00B_TEXT_PARAMETER_CARRIES,
            N00BB_ARGS(ARG_TEXT(violation->carrier->name),
                       ARG_TEXT(core_buffer_string(&marks))));
    else if (violation->carrier) {
        core_string_t type = {0};
        if (th_named_info(th_resolve(violation->carrier->type), &type, nullptr,
                          nullptr))
            n00bb_report_secondary(diagnostics, nullptr,
                n00bb_span_location(violation->call->span),
                N00B_TEXT_TYPE_CARRIES,
                N00BB_ARGS(ARG_TEXT(n00bb_name_leaf(type)),
                           ARG_TEXT(core_buffer_string(&marks))));
    }
    if (violation->field)
        n00bb_report_secondary(diagnostics, nullptr,
            n00bb_span_location(violation->field->span),
            N00B_TEXT_FIELD_CARRIES,
            N00BB_ARGS(ARG_TEXT(violation->field->operands[1].name),
                       ARG_TEXT(core_buffer_string(&marks))));
    if (call_target_text(&text, violation->via))
        n00bb_report_secondary(diagnostics, nullptr,
            n00bb_span_location(violation->via->span),
            N00B_TEXT_MARK_PASSES_THROUGH,
            N00BB_ARGS(ARG_TEXT(core_buffer_string(&text))));
    if (call_target_text(&text, violation->sink))
        n00bb_report_secondary(diagnostics, nullptr,
            n00bb_span_location(violation->sink->span),
            N00B_TEXT_MARK_REACHES,
            N00BB_ARGS(ARG_TEXT(core_buffer_string(&callee)), ARG_TEXT(param),
                       ARG_TEXT(core_buffer_string(&text))));
    ir_func_t *clearer = nullptr;
    for (size_t r = 0; !clearer && r < check->marks.clears.len; r++)
        if (check->marks.clears.data[r].marks & violation->marks)
            clearer = check->marks.clears.data[r].function;
    text.byte_len = 0;
    if (clearer) {
        function_name_text(&text, clearer);
        n00bb_report_help(diagnostics, N00B_TEXT_CHECK_IT_FIRST,
                          N00BB_ARGS(ARG_TEXT(core_buffer_string(&text))));
    }
    else n00bb_report_help(diagnostics, N00B_TEXT_MARK_HAS_NO_VALIDATOR,
                           N00BB_ARGS(ARG_TEXT(core_buffer_string(&marks))));
    core_buffer_release(&text);
    core_buffer_release(&marks);
    core_buffer_release(&callee);
}

static bool
fold_mark_questions(n00bb_link_analysis_t *analysis)
{
    bool folded = true;
    for (size_t q = 0; q < analysis->mark_questions.len; q++) {
        ir_instr_t *call = analysis->mark_questions.data[q].call;
        call->op = IR_OP_CONST;
        call->n_operands = 0;
        call->n_mods = 0;
        folded = ir_instr_operand_uint(call,
                     analysis->mark_questions.data[q].answer) && folded;
    }
    return folded;
}

bool
check_marks(n00bb_link_analysis_t *analysis)
{
    mark_check_t check = {
        .analysis = analysis, .names = core_list_new(core_string_t),
        .reported = core_list_new(mark_site_t),
        .marks = {
            .functions = core_list_new(n00bb_ir_func_ptr_t),
            .sources = core_list_new(n00bb_mark_rule_t),
            .clears = core_list_new(n00bb_mark_rule_t),
            .rejects = core_list_new(n00bb_mark_rule_t),
            .seeds = core_list_new(n00bb_mark_rule_t),
            .carriers = core_list_new(n00bb_mark_carrier_t),
            .summaries = analysis->summaries,
            .report = report_mark_violation,
        },
    };
    check.marks.context = &check;
    check.marks.questions = &analysis->mark_questions;
    collect_marks(&check);
    for (size_t q = 0; q < analysis->mark_questions.len; q++)
        analysis->mark_questions.data[q].marks = mark_bit(&check,
            analysis->mark_questions.data[q].mark);
    if (check.marks.rejects.len || analysis->mark_questions.len) {
        core_dict_t seen;
        core_dict_init(&seen, nullptr, nullptr);
        for (size_t f = 0; f < analysis->emitted_functions.len; f++) {
            ir_func_t *body = n00bb_cached_body(
                analysis->emitted_functions.data[f]);
            if (body && body->n_blocks && core_dict_add(&seen, body, body))
                core_list_push(check.marks.functions, body);
        }
        for (size_t u = 0; u < analysis->units.len; u++) {
            n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
            if (!state || !state->ir_module || state->interface_module)
                continue;
            for (size_t f = 0; f < ir_func_count(state->ir_module); f++) {
                ir_func_t *body = n00bb_cached_body(
                    ir_func_at(state->ir_module, f));
                if (body && body->n_blocks && core_dict_add(&seen, body, body))
                    core_list_push(check.marks.functions, body);
            }
        }
        core_dict_free(&seen);
        n00bb_check_marks(&check.marks);
    }
    if (!fold_mark_questions(analysis)) check.violations++;
    bool holds = check.violations == 0;
    core_list_free(check.names);
    core_list_free(check.reported);
    core_list_free(check.marks.functions);
    core_list_free(check.marks.sources);
    core_list_free(check.marks.clears);
    core_list_free(check.marks.rejects);
    core_list_free(check.marks.seeds);
    core_list_free(check.marks.carriers);
    return holds;
}
