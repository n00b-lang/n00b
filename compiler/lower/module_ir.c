#include "lower_internal.h"

static core_string_t
context_hook_suffix(parse_grammar_t *grammar, parse_tree_t *hook);
static bool
context_hook_ir_name(parse_grammar_t *grammar, parse_sym_entry_t *symbol,
                     parse_tree_t *hook, core_buffer_t *out);
static bool
add_context_hook_functions(parse_grammar_t *grammar,
                           core_translation_unit_t *unit,
                           n00bb_link_analysis_t *analysis,
                           core_link_unit_t *link,
                           uint32_t producer_stage);
static bool
register_nominal_types(parse_grammar_t *grammar,
                       core_translation_unit_t *unit, ir_module_t *module,
                       bool imported);
static bool
register_variant_types(core_translation_unit_t *unit,
                       ir_module_t *destination);
static bool
register_record_types(parse_grammar_t *grammar,
                      core_translation_unit_t *unit,
                      ir_module_t *destination);
static bool
register_imported_record_layouts(parse_grammar_t *grammar,
                                 core_translation_unit_t *unit,
                                 ir_module_t *destination);
static th_type *
static_global_type(parse_grammar_t *grammar, core_translation_unit_t *unit,
                   parse_sym_entry_t *symbol);
static void
zero_fixed_shape_release_items(ir_global_init_value_t *value);
static void
zero_fixed_shape_release(ir_global_init_value_t *value);
static ir_global_init_value_t *
zero_fixed_shape_initializer(ir_module_t *module, th_type *type,
                             const ir_shape_t *shape, uint32_t dimension);
static ir_global_init_value_t *
zero_record_initializer(core_translation_unit_t *unit, ir_module_t *module,
                        th_type *type, size_t depth, bool *shaped);
static bool
zero_member_initializer(core_translation_unit_t *unit, ir_module_t *module,
                        th_type *type, const ir_shape_t *shape,
                        size_t depth, bool *shaped,
                        ir_global_init_value_t *out);
static bool
initialize_static_global(parse_grammar_t *grammar,
                         core_translation_unit_t *unit,
                         ir_module_t *module,
                         parse_sym_entry_t *symbol, ir_global_t *global,
                         ir_diags_t *diagnostics, int32_t file_id);
static bool
n00bb_unit_defines_global(n00bb_link_analysis_t *analysis,
                          core_translation_unit_t *unit,
                          parse_sym_entry_t *symbol);
static core_hash_value_t
reference_pair_hash(void *key);
static bool
reference_pair_eq(const void *left, const void *right);
static reference_pair_t *
index_link_references(core_link_unit_t *link, core_dict_t *references);
static bool
register_module_globals(parse_grammar_t *grammar,
                        core_translation_unit_t *source,
                        core_translation_unit_t *destination_unit,
                        ir_module_t *destination,
                        ir_ctx_t *ir_context,
                        n00bb_link_analysis_t *analysis,
                        core_link_unit_t *link, core_dict_t *references,
                        ir_diags_t **source_diagnostics,
                        uint32_t producer_stage);
static bool
bind_module_global_identities(ir_ctx_t *ir_context,
                              n00bb_link_analysis_t *analysis);
static ir_global_t *
canonical_unit_global(n00bb_link_analysis_t *analysis, core_string_t name);
static bool
bind_cached_global_identities(ir_ctx_t *ir_context,
                              n00bb_link_analysis_t *analysis);
static th_type *
function_semantic_shape(parse_grammar_t *grammar, core_translation_unit_t *unit,
                        parse_sym_entry_t *symbol, th_type *source_type);
static th_type *
function_semantic_type(parse_grammar_t *grammar, core_translation_unit_t *unit,
                       parse_sym_entry_t *symbol);
static th_type *
function_semantic_contract(parse_grammar_t *grammar,
                           core_translation_unit_t *unit,
                           parse_sym_entry_t *symbol);
static th_type *
trait_method_semantic_type(parse_grammar_t *grammar,
                      core_translation_unit_t *unit,
                      parse_sym_entry_t *symbol,
                      th_type **callable_contract);
static th_type *
interface_function_semantic_type(th_ctx *types, parse_sym_entry_t *symbol);
static bool
ir_initialization_error(core_translation_unit_t *unit, const char *what);
static bool
register_optional_parameter_metadata(parse_grammar_t *grammar,
                                     parse_sym_entry_t *symbol,
                                     ir_func_t *function, bool named);
static ir_func_t *
cached_function_body(n00bb_link_analysis_t *analysis,
                     core_translation_unit_t *unit,
                     core_string_t semantic_name,
                     bool overloaded, core_string_t declaration_key);
static bool
program_interface_module(void *context, core_string_t name);
static bool
n00bb_emit_default_param(ir_lowerer_t *lowerer,
                        ir_lower_context_t *context,
                        n00bb_default_param_t *parameter);

const parse_sym_entry_t *
function_symbol_at(parse_grammar_t *grammar, parse_tree_t *node)
{
    return node && nt_is(grammar, node, CORE_STRING_STATIC("func-def"))
        ? parse_tree_sym(node) : nullptr;
}

static core_string_t
context_hook_suffix(parse_grammar_t *grammar, parse_tree_t *hook)
{
    if (nt_is(grammar, hook, CORE_STRING_STATIC("context-enter")))
        return CORE_STRING_STATIC("enter");
    if (nt_is(grammar, hook, CORE_STRING_STATIC("context-exit")))
        return CORE_STRING_STATIC("exit");
    if (nt_is(grammar, hook, CORE_STRING_STATIC("context-fail")))
        return CORE_STRING_STATIC("fail");
    if (nt_is(grammar, hook, CORE_STRING_STATIC("context-ensure")))
        return CORE_STRING_STATIC("ensure");
    return (core_string_t){};
}

core_string_t
context_hook_binding_role(parse_grammar_t *grammar, parse_tree_t *hook)
{
    core_string_t suffix = context_hook_suffix(grammar, hook);
    if (!suffix.data) return (core_string_t){};
    if (core_string_eq(suffix, CORE_STRING_STATIC("enter")))
        return CORE_STRING_STATIC("n00bb.ir.context.enter");
    if (core_string_eq(suffix, CORE_STRING_STATIC("exit")))
        return CORE_STRING_STATIC("n00bb.ir.context.exit");
    if (core_string_eq(suffix, CORE_STRING_STATIC("fail")))
        return CORE_STRING_STATIC("n00bb.ir.context.fail");
    if (core_string_eq(suffix, CORE_STRING_STATIC("ensure")))
        return CORE_STRING_STATIC("n00bb.ir.context.ensure");
    return (core_string_t){};
}

static bool
context_hook_ir_name(parse_grammar_t *grammar, parse_sym_entry_t *symbol,
                     parse_tree_t *hook, core_buffer_t *out)
{
    core_string_t suffix = context_hook_suffix(grammar, hook);
    return suffix.data && context_hook_name(symbol, suffix, out);
}

bool
context_default_role(size_t index, char *buffer, size_t size,
                     core_string_t *out)
{
    if (!buffer || !size || !out) return false;
    int written = snprintf(buffer, size, "n00bb.ir.context.default.%zu",
                           index);
    if (written <= 0 || (size_t)written >= size) return false;
    *out = core_string_borrow(buffer, (size_t)written);
    return true;
}

bool
context_interface_dependencies(parse_sym_entry_t *symbol, size_t target,
                                size_t *indices, size_t capacity,
                                size_t *count)
{
    const uint8_t *data = symbol ? symbol->user_data : nullptr;
    if (count) *count = 0;
    if (!data || !(data[0] & 0x80u) || (data[0] & 0x70u))
        return false;
    uint32_t parameter_count = read_u32_le(data + 1);
    if (target >= parameter_count) return false;
    size_t offset = 5;
    for (size_t i = 0; i < parameter_count; i++) {
        if (offset > SIZE_MAX - 4) return false;
        uint16_t name_size = (uint16_t)data[offset]
            | (uint16_t)data[offset + 1] << 8;
        uint8_t flags = data[offset + 2];
        offset += 4;
        if (!name_size || name_size > SIZE_MAX - offset) return false;
        offset += name_size;
        size_t dependency_count = 0;
        if (flags & 4u) {
            if (offset > SIZE_MAX - 2) return false;
            dependency_count = (uint16_t)data[offset]
                | (uint16_t)data[offset + 1] << 8;
            offset += 2;
            if (dependency_count > (SIZE_MAX - offset) / 2) return false;
            if (i == target && dependency_count > capacity) return false;
            for (size_t d = 0; d < dependency_count; d++) {
                size_t dependency = (uint16_t)data[offset]
                    | (uint16_t)data[offset + 1] << 8;
                if (i == target && indices) indices[d] = dependency;
                offset += 2;
            }
        }
        if (i == target) {
            if (count) *count = dependency_count;
            return true;
        }
    }
    return false;
}

static bool
add_context_hook_functions(parse_grammar_t *grammar,
                           core_translation_unit_t *unit,
                           n00bb_link_analysis_t *analysis,
                           core_link_unit_t *link,
                           uint32_t producer_stage)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->ir_module) return false;
    core_list_t(parse_scope_ptr_t) scopes = link_declaration_scopes(
        state, CORE_STRING_STATIC(""));
    bool ok = true;
    for (size_t s = 0; ok && s < scopes.len; s++) {
        for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
             ok && symbol; symbol = symbol->next_in_scope) {
            if (!context_symbol(grammar, symbol)) continue;
            if (state->interface_module) {
                core_link_declaration_t *context_declaration =
                    link_declaration_for_symbol(analysis, symbol);
                static const struct {
                    core_string_t suffix;
                    core_string_t role;
                } interface_hooks[] = {
                    {CORE_STRING_STATIC("enter"),
                     CORE_STRING_STATIC("n00bb.ir.context.enter")},
                    {CORE_STRING_STATIC("exit"),
                     CORE_STRING_STATIC("n00bb.ir.context.exit")},
                    {CORE_STRING_STATIC("fail"),
                     CORE_STRING_STATIC("n00bb.ir.context.fail")},
                    {CORE_STRING_STATIC("ensure"),
                     CORE_STRING_STATIC("n00bb.ir.context.ensure")},
                };
                for (size_t h = 0; ok && h <
                     sizeof(interface_hooks) / sizeof(interface_hooks[0]); h++) {
                    core_buffer_t name = {0};
                    if (!context_hook_name(symbol, interface_hooks[h].suffix,
                                           &name)) {
                        core_buffer_release(&name);
                        ok = false;
                        break;
                    }
                    parse_sym_entry_t *hook_symbol = parse_symtab_lookup_scoped(
                        state->annotations->symtab, symbol->scope,
                        CORE_STRING_STATIC(""), core_buffer_string(&name));
                    core_buffer_release(&name);
                    if (!hook_symbol) continue;
                    core_link_declaration_t *hook_declaration =
                        link_declaration_for_symbol(analysis, hook_symbol);
                    ir_func_t *function = hook_declaration
                        ? core_link_declaration_binding(hook_declaration,
                            CORE_STRING_STATIC("n00bb.ir.function")) : nullptr;
                    ok = context_declaration && function
                        && core_link_declaration_set_binding(link,
                            context_declaration, interface_hooks[h].role,
                            producer_stage, function);
                }
                for (size_t p = 0; ok && p <
                     context_interface_parameter_count(symbol); p++) {
                    uint8_t flags = 0;
                    if (!context_interface_parameter(symbol, p, nullptr,
                                                     &flags, nullptr)) {
                        ok = false;
                        break;
                    }
                    if (!(flags & 4u)) continue;
                    core_buffer_t name = {0};
                    char role_buffer[64];
                    core_string_t role = {};
                    if (!context_default_name(symbol, p, &name)
                        || !context_default_role(p, role_buffer,
                                                 sizeof(role_buffer), &role)) {
                        core_buffer_release(&name);
                        ok = false;
                        break;
                    }
                    parse_sym_entry_t *helper_symbol =
                        parse_symtab_lookup_scoped(state->annotations->symtab,
                            symbol->scope, CORE_STRING_STATIC(""),
                            core_buffer_string(&name));
                    core_buffer_release(&name);
                    core_link_declaration_t *helper_declaration =
                        link_declaration_for_symbol(analysis, helper_symbol);
                    ir_func_t *function = helper_declaration
                        ? core_link_declaration_binding(helper_declaration,
                            CORE_STRING_STATIC("n00bb.ir.function")) : nullptr;
                    ok = helper_symbol && context_declaration && function
                        && core_link_declaration_set_binding(link,
                            context_declaration, role, producer_stage,
                            function);
                }
                continue;
            }
            static const core_string_t hooks[] = {
                CORE_STRING_STATIC("context-enter"),
                CORE_STRING_STATIC("context-exit"),
                CORE_STRING_STATIC("context-fail"),
                CORE_STRING_STATIC("context-ensure"),
            };
            for (size_t i = 0; ok && i < sizeof(hooks) / sizeof(hooks[0]);
                 i++) {
                core_string_t hook_role = n00bb_context_hook_role(hooks[i]);
                parse_tree_t *hook = hook_role.data
                    ? parse_sym_binding(symbol, hook_role) : nullptr;
                if (!hook) continue;
                core_buffer_t name = {0};
                th_type *type = th_func_collect_rest(state->types,
                    parse_tree_type(hook));
                if (type && th_kind_of(type) != TH_FUNCTION) type = nullptr;
                ir_func_t *function = context_hook_ir_name(
                        grammar, symbol, hook, &name)
                    && type && !ir_func_find(state->ir_module,
                                             core_buffer_string(&name))
                    ? ir_func_add(state->ir_module, core_buffer_string(&name),
                                  type, IR_LINK_INTERNAL) : nullptr;
                core_buffer_release(&name);
                core_link_declaration_t *declaration =
                    link_declaration_for_symbol(analysis, symbol);
                core_string_t role = context_hook_binding_role(grammar, hook);
                ok = function && declaration && role.data
                    && core_link_declaration_set_binding(link, declaration,
                        role, producer_stage, function);
            }
            core_list_t(parse_sym_entry_ptr_t) parameters =
                core_list_new(parse_sym_entry_ptr_t);
            parse_sym_entry_t *rest = nullptr;
            collect_ir_parameters(grammar,
                child_nt(grammar, symbol->decl_node, CORE_STRING_STATIC("param-decl")),
                &parameters, &rest);
            if (rest) core_list_push(parameters, rest);
            for (size_t i = 0; ok && i < parameters.len; i++) {
                parse_sym_entry_t *parameter = parameters.data[i];
                if (!parameter || !parameter->is_optional) continue;
                parse_tree_t *default_expr = child_nt(grammar,
                    parameter->decl_node, CORE_STRING_STATIC("expression"));
                core_list_t(parse_sym_entry_ptr_t) dependencies =
                    core_list_new(parse_sym_entry_ptr_t);
                context_default_dependencies(default_expr,
                    &parameters, &dependencies);
                th_type *type = context_default_ir_type(state->types,
                    symbol->type, &dependencies, parameter->type);
                core_buffer_t name = {0};
                char role_buffer[64];
                core_string_t role = {};
                bool named = context_default_name(symbol, i, &name)
                    && context_default_role(i, role_buffer,
                                            sizeof(role_buffer), &role);
                ir_func_t *function = named && type
                    && !ir_func_find(state->ir_module,
                                     core_buffer_string(&name))
                    ? ir_func_add(state->ir_module, core_buffer_string(&name),
                                  type, IR_LINK_INTERNAL) : nullptr;
                core_buffer_release(&name);
                core_link_declaration_t *declaration =
                    link_declaration_for_symbol(analysis, symbol);
                ok = function && declaration
                    && core_link_declaration_set_binding(link, declaration,
                        role, producer_stage, function);
                core_list_free(dependencies);
            }
            core_list_free(parameters);
            (void)rest;
        }
    }
    core_list_free(scopes);
    return ok;
}

bool
n00bb_failure_channel_accepts(ir_lower_context_t *context,
                              th_type *failure)
{
    th_type *caller_error = context && context->function
        ? th_func_error(context->function->type) : nullptr;
    th_ctx *types = context ? ir_module_types(context->module) : nullptr;
    if (!types || !caller_error || !failure) return false;
    if (th_kind_of(th_resolve(failure)) == TH_VAR) return true;
    if (th_is_nil(caller_error)) return false;
    if (th_kind_of(th_resolve(caller_error)) == TH_VAR) return true;
    return ir_type_eq(context->module, caller_error, failure);
}

static bool
register_nominal_types(parse_grammar_t *grammar,
                       core_translation_unit_t *unit, ir_module_t *module,
                       bool imported)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->annotations || !state->annotations->symtab)
        return false;
    for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
         scope; scope = scope->all_next) {
        for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
             symbol = symbol->next_in_scope) {
            if (symbol->kind != PARSE_SYM_TYPEDEF || !symbol->type
                || !!parse_sym_binding_origin(symbol) != imported) continue;
            bool nominal = symbol->decl_node
                ? nt_is(grammar, symbol->decl_node, CORE_STRING_STATIC("alias-stmt"))
                    && !alias_declaration_is_transparent(grammar,
                                                         symbol->decl_node)
                : core_string_eq(symbol->adt_kind, CORE_STRING_STATIC("nominal"));
            bool scalar = core_string_eq(symbol->adt_kind,
                                         CORE_STRING_STATIC("callback"))
                || core_string_eq(symbol->adt_kind,
                                  CORE_STRING_STATIC("enum"))
                || (symbol->member_scope
                    && core_string_eq(symbol->member_scope->scope_tag,
                                      CORE_STRING_STATIC("flagset")));
            if (!nominal && !scalar) continue;
            th_type *representation = th_type_definition(state->types, symbol->type);
            if (!representation || !ir_type_define(module, symbol->type,
                                                    representation)) return false;
        }
    }
    return true;
}

static bool
register_variant_types(core_translation_unit_t *unit,
                       ir_module_t *destination)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !destination || !state->annotations
        || !state->annotations->symtab) return false;
    parse_symtab_t *symtab = state->annotations->symtab;
    bool ok = true;
    for (parse_scope_t *scope = symtab->all_scopes; ok && scope;
         scope = scope->all_next) {
        bool expected_namespace = scope->ns &&
            core_string_eq(scope->ns->ns_name,
                state->interface_module
                    ? CORE_STRING_STATIC("type")
                    : CORE_STRING_STATIC(""));
        if (!expected_namespace
            || !core_string_eq(scope->scope_tag,
                            CORE_STRING_STATIC("variant"))) continue;
        core_buffer_t qualified = {0};
        append_module_type_name(unit, scope->name, &qualified);
        parse_sym_entry_t *symbol = scope->node
            ? parse_tree_sym(scope->node) : nullptr;
        th_type *variant = variant_template_type(state->types,
            core_buffer_string(&qualified), symbol);
        th_type *definition = variant
            ? th_type_definition(state->types, variant) : nullptr;
        ok = definition && ir_type_define(destination, variant, definition);
        core_buffer_release(&qualified);
    }
    return ok;
}

static bool
pin_bit_fields(ir_module_t *destination, th_type *record,
               parse_sym_entry_t *first, bool fields_only)
{
    bool valid = true;
    for (parse_sym_entry_t *field = first; valid && field;
         field = field->next_in_scope) {
        if ((fields_only && !field->is_field)
            || !(field->has_bit_offset || field->has_bit_width)) continue;
        valid = ir_layout_pin_member(destination, record, field->name,
                                     field->bit_offset, field->bit_width);
    }
    return valid;
}

static bool
register_record_types(parse_grammar_t *grammar,
                      core_translation_unit_t *unit,
                      ir_module_t *destination)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !destination || !state->types || !state->annotations
        || !state->annotations->symtab) return false;
    if (state->interface_module) {
        core_list_t(parse_scope_ptr_t) types = link_declaration_scopes(
            state, CORE_STRING_STATIC("type"));
        bool valid = true;
        for (size_t s = 0; valid && s < types.len; s++) {
            for (parse_sym_entry_t *symbol = types.data[s]->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                parse_scope_t *members = symbol->member_scope;
                if (symbol->kind != PARSE_SYM_TYPEDEF || !members ||
                    !core_string_eq(members->scope_tag,
                                    CORE_STRING_STATIC("type"))) continue;
                n00bb_export_origin_t *origin = parse_sym_binding(symbol,
                    CORE_STRING_STATIC("n00bb.export-origin"));
                th_type *template = origin
                    ? record_template_type(state->types,
                        origin->declaration, symbol) : nullptr;
                th_type *record = th_type_definition(state->types,
                                                     template);
                valid = record && th_kind_of(record) == TH_RECORD &&
                        ir_type_define(destination, template, record)
                        && pin_bit_fields(destination, template,
                                          members->first_in_scope, true);
                if (!valid) break;
            }
        }
        core_list_free(types);
        if (!valid) return false;
        return true;
    }
    for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
         scope; scope = scope->all_next) {
        if (!scope_in_value_namespace(scope)
            || !n00bb_field_scope_tag(scope->scope_tag)
            || !scope->node
            || !field_record_declaration(grammar, scope->node))
            continue;
        const n00bb_field_declaration_types_t *types =
            n00bb_record_types(parse_tree_sym(scope->node));
        th_type *record = types
            ? th_type_definition(state->types, types->shape) : nullptr;
        th_type *representation = types && types->named != types->shape
            ? th_type_definition(state->types, types->named) : nullptr;
        bool valid = record
            && ir_type_define(destination, types->shape, record)
            && (types->named == types->shape
                || (representation
                    && ir_type_define(destination, types->named,
                                      representation)))
            && pin_bit_fields(destination, types->shape,
                              scope->first_in_scope, true);
        if (!valid && n00bb_unit_failed(unit)) {
            parse_sym_entry_t *record_symbol = parse_tree_sym(scope->node);
            if (record_symbol)
                (void)parse_sym_set_binding(record_symbol,
                    N00BB_POISONED_KEY, record_symbol);
            continue;
        }
        if (!valid) {
            n00bb_internal_error(core_translation_unit_diagnostics(unit),
                unit, link_source_location(scope->node),
                "registering record types",
                "could not register the representation of this record");
            return false;
        }
    }
    return true;
}

static bool
register_imported_record_layouts(parse_grammar_t *grammar,
                                 core_translation_unit_t *unit,
                                 ir_module_t *destination)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !destination || !state->types || !state->annotations
        || !state->annotations->symtab) return false;
    core_list_t(parse_scope_ptr_t) scopes = link_declaration_scopes(
        state, CORE_STRING_STATIC("type"));
    for (size_t s = 0; s < scopes.len; s++) {
        for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
             symbol; symbol = symbol->next_in_scope) {
            parse_scope_t *members = symbol->member_scope;
            if (symbol->kind != PARSE_SYM_TYPEDEF || !symbol->type ||
                !members || !core_string_eq(members->scope_tag,
                    CORE_STRING_STATIC("type"))) continue;
            core_buffer_t qualified = {0};
            n00bb_export_origin_t *origin = state->interface_module
                ? parse_sym_binding(symbol,
                    CORE_STRING_STATIC("n00bb.export-origin")) : nullptr;
            if (!state->interface_module)
                append_module_type_name(unit, symbol->name, &qualified);
            th_type *template = (origin || !state->interface_module)
                ? record_template_type(state->types,
                    origin ? origin->declaration
                           : core_buffer_string(&qualified),
                    symbol) : nullptr;
            th_type *shape = template
                ? th_type_definition(state->types, template) : nullptr;
            core_buffer_release(&qualified);
            if (!shape) continue;
            th_type *existing = ir_type_definition(destination, template);
            bool valid = existing
                ? th_hash(state->types, existing) ==
                    th_hash(state->types, shape)
                : ir_type_define(destination, template, shape);
            valid = valid && pin_bit_fields(destination, template,
                                            members->first_in_scope, false);
            if (valid)
                valid = ir_layout_ensure(destination, template,
                    ir_module_target(destination)) != nullptr;
            if (!valid) {
                core_list_free(scopes);
                return false;
            }
        }
    }
    core_list_free(scopes);
    (void)grammar;
    return true;
}

static th_type *
static_global_type(parse_grammar_t *grammar, core_translation_unit_t *unit,
                   parse_sym_entry_t *symbol)
{
    th_type *inferred = th_resolve(symbol ? symbol->type : nullptr);
    th_type *declared = symbol
        ? declared_type_from_node(grammar, unit, symbol->decl_node) : nullptr;
    if (!declared) return inferred;
    declared = th_resolve(declared);
    if (inferred && th_kind_of(inferred) == TH_NAMED
        && th_kind_of(declared) == TH_NAMED
        && th_named_nargs(inferred) > 0 && th_named_nargs(declared) == 0)
        return inferred;
    return declared;
}

static void
zero_fixed_shape_release_items(ir_global_init_value_t *value)
{
    if (value->kind != IR_GLOBAL_INIT_SEQUENCE
        && value->kind != IR_GLOBAL_INIT_RECORD) return;
    ir_global_init_value_t *items =
        (ir_global_init_value_t *)value->aggregate.items;
    for (size_t i = 0; i < value->aggregate.count; i++)
        zero_fixed_shape_release_items(&items[i]);
    core_free(items);
}

static void
zero_fixed_shape_release(ir_global_init_value_t *value)
{
    if (!value) return;
    zero_fixed_shape_release_items(value);
    core_free(value);
}

static ir_global_init_value_t *
zero_fixed_shape_initializer(ir_module_t *module, th_type *type,
                             const ir_shape_t *shape, uint32_t dimension)
{
    type = type ? th_resolve(type) : nullptr;
    th_type *element = type && th_kind_of(type) == TH_SEQUENCE
        ? th_resolve(th_seq_elem(type)) : nullptr;
    ir_shape_extent_t extent;
    if (!shape || !element || !ir_shape_extent(shape, dimension, &extent)
        || extent.kind != IR_SHAPE_EXTENT_STATIC
        || extent.static_value > SIZE_MAX / sizeof(ir_global_init_value_t))
        return nullptr;
    size_t count = (size_t)extent.static_value;
    ir_global_init_value_t *value = core_alloc_array(ir_global_init_value_t, 1);
    ir_global_init_value_t *items = core_alloc_array(ir_global_init_value_t,
                                                     count ? count : 1);
    memset(items, 0, (count ? count : 1) * sizeof(*items));
    *value = (ir_global_init_value_t){
        .kind = IR_GLOBAL_INIT_SEQUENCE,
        .aggregate = {.items = items, .count = count},
    };
    bool nested = th_kind_of(element) == TH_SEQUENCE;
    ir_class_t category = ir_machine_of(module, element).class;
    bool valid = true;
    for (size_t i = 0; valid && i < count; i++) {
        if (nested) {
            ir_global_init_value_t *inner = zero_fixed_shape_initializer(
                module, element, shape, dimension + 1);
            valid = inner != nullptr;
            if (valid) {
                items[i] = *inner;
                core_free(inner);
            }
        }
        else if (category == IR_CLASS_SINT || category == IR_CLASS_UINT)
            items[i].kind = IR_GLOBAL_INIT_INT;
        else if (category == IR_CLASS_BOOL)
            items[i].kind = IR_GLOBAL_INIT_BOOL;
        else if (category == IR_CLASS_FLOAT)
            items[i].kind = IR_GLOBAL_INIT_FLOAT;
        else if (category == IR_CLASS_VOID)
            items[i].kind = IR_GLOBAL_INIT_NIL;
        else valid = false;
    }
    if (!valid) {
        zero_fixed_shape_release(value);
        return nullptr;
    }
    return value;
}

static ir_global_init_value_t *
zero_record_initializer(core_translation_unit_t *unit, ir_module_t *module,
                        th_type *type, size_t depth, bool *shaped)
{
    n00bb_tu_state_t *state = tu_state(unit);
    th_type *record = depth < 16 ? static_record_shape(state, type) : nullptr;
    parse_sym_entry_t *owner = record ? type_symbol_in_loaded_imports(unit,
        state->types, type, nullptr) : nullptr;
    if (!record || th_kind_of(record) != TH_RECORD || !owner
        || !owner->member_scope) return nullptr;
    size_t count = th_record_len(record);
    ir_global_init_value_t *value = core_alloc_array(ir_global_init_value_t, 1);
    ir_global_init_value_t *items = core_alloc_array(ir_global_init_value_t,
                                                     count ? count : 1);
    memset(items, 0, (count ? count : 1) * sizeof(*items));
    *value = (ir_global_init_value_t){
        .kind = IR_GLOBAL_INIT_RECORD,
        .aggregate = {.items = items, .count = count},
    };
    bool valid = true;
    for (size_t i = 0; valid && i < count; i++) {
        const th_member *member = th_record_member(record, i);
        parse_sym_entry_t *field = nullptr;
        for (parse_sym_entry_t *candidate = owner->member_scope->first_in_scope;
             candidate && member && !field; candidate = candidate->next_in_scope)
            if (candidate->is_field
                && core_string_eq(candidate->name, member->name))
                field = candidate;
        valid = member && zero_member_initializer(unit, module, member->type,
            field ? parse_sym_binding(field,
                CORE_STRING_STATIC("ir.array-shape")) : nullptr,
            depth + 1, shaped, &items[i]);
        if (valid) items[i].member = member->name;
    }
    if (!valid) {
        zero_fixed_shape_release(value);
        return nullptr;
    }
    return value;
}

static bool
zero_member_initializer(core_translation_unit_t *unit, ir_module_t *module,
                        th_type *type, const ir_shape_t *shape,
                        size_t depth, bool *shaped,
                        ir_global_init_value_t *out)
{
    th_type *resolved = type ? th_resolve(type) : nullptr;
    if (!resolved) return false;
    if (th_kind_of(resolved) == TH_SEQUENCE) {
        ir_global_init_value_t *array = zero_fixed_shape_initializer(module,
            resolved, shape, 0);
        if (!array) return false;
        *out = *array;
        core_free(array);
        *shaped = true;
        return true;
    }
    if (th_option_item(resolved)) {
        *out = (ir_global_init_value_t){.kind = IR_GLOBAL_INIT_NIL};
        return true;
    }
    ir_class_t category = ir_machine_of(module, resolved).class;
    if (category == IR_CLASS_SINT || category == IR_CLASS_UINT) {
        *out = (ir_global_init_value_t){.kind = IR_GLOBAL_INIT_INT};
        return true;
    }
    if (category == IR_CLASS_BOOL) {
        *out = (ir_global_init_value_t){.kind = IR_GLOBAL_INIT_BOOL};
        return true;
    }
    if (category == IR_CLASS_FLOAT) {
        *out = (ir_global_init_value_t){.kind = IR_GLOBAL_INIT_FLOAT};
        return true;
    }
    if (th_builtin_kind(ir_module_types(module), resolved)
            == TH_BUILTIN_STRING) {
        *out = (ir_global_init_value_t){.kind = IR_GLOBAL_INIT_STRING};
        return true;
    }
    ir_global_init_value_t *record = zero_record_initializer(unit, module,
        resolved, depth, shaped);
    if (!record) return false;
    *out = *record;
    core_free(record);
    return true;
}

static bool
initialize_static_global(parse_grammar_t *grammar,
                         core_translation_unit_t *unit,
                         ir_module_t *module,
                         parse_sym_entry_t *symbol, ir_global_t *global,
                         ir_diags_t *diagnostics, int32_t file_id)
{
    parse_tree_t *declaration = symbol->decl_node;
    parse_tree_t *initializer = nt_is(grammar, declaration, CORE_STRING_STATIC("sym-decl"))
        ? child_nt(grammar, declaration, CORE_STRING_STATIC("expression")) : nullptr;
    if (!initializer && (!symbol->has_comptime_value
                         || !symbol->comptime_value)) {
        if (symbol->mutability == PARSE_SYM_CONST) {
            ir_lower_context_t span_context = {
                .file_id = file_id, .diagnostics = diagnostics};
            for (parse_tree_t *at = declaration; at;
                 at = parse_tree_parent(at))
                if (nt_is(grammar, at, CORE_STRING_STATIC("binding-pattern")))
                    return true;
            lower_report(&span_context, N00B_DIAG_CONST_WITHOUT_VALUE,
                lower_symbol_name_span(&span_context, symbol),
                N00B_TEXT_IS_A_CONST_SO_IT,
                N00BB_ARGS(ARG_TEXT(symbol->name)));
            lower_help(&span_context, N00B_TEXT_GIVE_IT_AN_INITIALIZER_AS,
                N00BB_ARGS(ARG_TEXT(symbol->name)));
            return false;
        }
        ir_global_init_value_t *zero = zero_fixed_shape_initializer(module,
            global->type, global->shape, 0);
        bool shaped = false;
        if (!zero) zero = zero_record_initializer(unit, module, global->type,
                                                  0, &shaped);
        if (zero && zero->kind == IR_GLOBAL_INIT_RECORD && !shaped) {
            zero_fixed_shape_release(zero);
            zero = nullptr;
        }
        bool zeroed = !zero
            || ir_global_set_static_initializer(module, global, zero);
        zero_fixed_shape_release(zero);
        return zeroed;
    }

    n00bb_tu_state_t *state = tu_state(unit);
    th_type *static_type = static_global_type(grammar, unit, symbol);
    th_type *open = static_type ? th_resolve(static_type) : nullptr;
    __int128 folded = 0;
    if (open && th_kind_of(open) == TH_VAR
        && n00bb_untyped_integer_const(grammar, symbol, &folded)) {
        th_type *fit = n00bb_integer_fit(state->types, folded);
        if (fit && th_var_bind(open, fit)) static_type = fit;
    }
    const n00bb_static_value_t *static_value = symbol->has_comptime_value
        ? symbol->comptime_value : nullptr;
    n00bb_static_value_t trivial = {0};
    if (!static_value && initializer
        && n00bb_trivial_static_value(grammar, state, initializer, &trivial))
        static_value = &trivial;

    ir_span_t span = initializer
        ? lower_span_for_tree(
            &(ir_lower_context_t){.file_id = file_id}, initializer)
        : (ir_span_t){0};
    ir_global_init_value_t value = {0};
    bool initialized = static_value && n00bb_static_value_to_ir_initializer(
        static_value, static_type, state, &value, 0)
        && ir_global_set_static_initializer(module, global, &value);

    if (!initialized) {
        if (symbol->mutability != PARSE_SYM_CONST) return true;
        if (!th_is_nil(static_type)) return true;
        ir_lower_context_t span_context = {
            .file_id = file_id, .diagnostics = diagnostics};
        lower_report(&span_context, N00B_DIAG_NIL_CONSTANT, span,
            N00B_TEXT_WOULD_HOLD_NIL_WHICH_IS,
            N00BB_ARGS(ARG_TEXT(symbol->name)));
        return false;
    }
    return true;
}

static bool
n00bb_unit_defines_global(n00bb_link_analysis_t *analysis,
                          core_translation_unit_t *unit,
                          parse_sym_entry_t *symbol)
{
    core_link_declaration_t *declaration =
        link_declaration_for_symbol(analysis, symbol);
    return !declaration || core_string_eq(
        core_link_declaration_module(declaration), unit->module.identifier);
}

static core_hash_value_t
reference_pair_hash(void *key)
{
    return core_hash_raw(key, sizeof(reference_pair_t));
}

static bool
reference_pair_eq(const void *left, const void *right)
{
    const reference_pair_t *a = left;
    const reference_pair_t *b = right;
    return a->referrer == b->referrer && a->target == b->target;
}

static reference_pair_t *
index_link_references(core_link_unit_t *link, core_dict_t *references)
{
    size_t count = core_link_unit_reference_count(link);
    reference_pair_t *pairs = core_alloc_array(reference_pair_t,
                                               count ? count : 1);
    core_dict_init(references, reference_pair_hash, reference_pair_eq);
    for (size_t r = 0; r < count; r++) {
        core_link_reference_t *reference =
            core_link_unit_reference_at_index(link, r);
        pairs[r] = (reference_pair_t){
            .referrer = core_link_reference_referrer(reference),
            .target = core_link_reference_target(reference),
        };
        core_dict_add(references, &pairs[r], &pairs[r]);
    }
    return pairs;
}

static bool
register_module_globals(parse_grammar_t *grammar,
                        core_translation_unit_t *source,
                        core_translation_unit_t *destination_unit,
                        ir_module_t *destination,
                        ir_ctx_t *ir_context,
                        n00bb_link_analysis_t *analysis,
                        core_link_unit_t *link, core_dict_t *references,
                        ir_diags_t **source_diagnostics,
                        uint32_t producer_stage)
{
    n00bb_tu_state_t *source_state = tu_state(source);
    if (!source_state || !source_state->annotations || !destination)
        return false;
    if (!*source_diagnostics)
        *source_diagnostics = attach_ir_diags(ir_context, nullptr, source,
                                              nullptr);
    ir_diags_t *diagnostics = *source_diagnostics;
    if (!diagnostics) return false;
    int32_t source_file = source_state->ir_module
        ? ir_file_count(source_state->ir_module) - 1 : -1;
    core_list_t(parse_scope_ptr_t) scopes = link_declaration_scopes(
        source_state, CORE_STRING_STATIC(""));
    bool ok = true;
    for (size_t s = 0; ok && s < scopes.len; s++) {
        for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
             ok && symbol; symbol = symbol->next_in_scope) {
            if (symbol->kind != PARSE_SYM_VARIABLE
                || context_symbol(grammar, symbol)
                || (!symbol_binding_node(symbol)
                    && !source_state->interface_module)
                || !symbol->type
                || !n00bb_unit_defines_global(analysis, source, symbol))
                continue;
            if (source != destination_unit) {
                reference_pair_t wanted = {
                    .referrer = destination_unit,
                    .target = link_declaration_for_symbol(analysis, symbol),
                };
                if (!wanted.target
                    || !core_dict_contains(references, &wanted)) continue;
            }
            core_buffer_t name = {0};
            append_module_symbol_name(source, symbol, &name);
            th_type *global_type = static_global_type(grammar, source,
                                                       symbol);
            ir_global_t *global = global_type
                ? ir_global_add(destination, core_buffer_string(&name),
                                global_type) : nullptr;
            ir_shape_t *array_shape = parse_sym_binding(symbol,
                CORE_STRING_STATIC("ir.array-shape"));
            bool local_owner = destination == source_state->ir_module;
            bool canonical_owner = local_owner;
            bool local_definition = local_owner
                && !source_state->interface_module;
            if (global) {
                bool shape_bound = !array_shape
                    || ir_global_set_shape(global, array_shape);
                global->is_extern = !local_definition;
                global->is_static = local_definition
                    && symbol_is_private(symbol);
                global->mutability = symbol->mutability == PARSE_SYM_MUTABLE
                    ? IR_MUT_MUTABLE
                    : symbol->mutability == PARSE_SYM_CONST ? IR_MUT_CONST
                    : IR_MUT_IMMUTABLE;
                th_type *referent = th_resolve(global->type);
                ok = shape_bound
                    && (!referent || (th_kind_of(referent) != TH_INDIRECTION
                            && !n00bb_reference_definition(
                                ir_module_types(destination), referent))
                        || ir_value_set_indirect_access(ir_context,
                            ir_global_address(destination, global),
                            IR_INDIRECT_ACCESS_READ_WRITE));
                ir_storage_duration_t duration =
                    IR_STORAGE_DURATION_UNSPECIFIED;
                switch (symbol->storage_duration) {
                case PARSE_SYM_STORAGE_COMPTIME:
                    duration = IR_STORAGE_DURATION_COMPTIME;
                    break;
                case PARSE_SYM_STORAGE_AUTOMATIC:
                    duration = IR_STORAGE_DURATION_AUTOMATIC;
                    break;
                case PARSE_SYM_STORAGE_MODULE:
                    duration = IR_STORAGE_DURATION_MODULE;
                    break;
                case PARSE_SYM_STORAGE_THREAD:
                    duration = IR_STORAGE_DURATION_THREAD;
                    break;
                case PARSE_SYM_STORAGE_DYNAMIC:
                    duration = IR_STORAGE_DURATION_DYNAMIC;
                    break;
                case PARSE_SYM_STORAGE_UNSPECIFIED:
                    break;
                }
                if (ok)
                    ok = ir_global_set_storage_duration(
                        ir_context, global, duration)
                        && ir_global_set_storage_class(ir_context, global,
                            symbol->requested_storage_class,
                            symbol->storage_class_required,
                            symbol->selected_storage_class);
                if (symbol->is_thread_local)
                    ir_annot_set(ir_context, &global->annots,
                        (ir_annot_t){.key = CORE_STRING_STATIC("n00b.thread_local"),
                                     .kind = IR_ANNOT_BOOL, .b = true});
                if (local_owner) {
                    if (symbol->decl_node)
                        global->span = lower_span_for_tree(
                            &(ir_lower_context_t){.file_id = source_file},
                            symbol->decl_node);
                    if (ok && local_definition)
                        ok = initialize_static_global(grammar, source,
                            destination, symbol, global, diagnostics,
                            source_file);
                    core_link_declaration_t *declaration = canonical_owner
                        ? link_declaration_for_symbol(analysis, symbol)
                        : nullptr;
                    bool already_bound = declaration
                        && core_link_declaration_binding(declaration,
                            CORE_STRING_STATIC("n00bb.ir.global"));
                    if (ok && canonical_owner && (!declaration
                        || ((!source_state->interface_module
                                || !already_bound)
                            && !core_link_declaration_set_binding(link,
                                declaration,
                                CORE_STRING_STATIC("n00bb.ir.global"),
                                producer_stage, global)))) {
                        if (!n00bb_unit_failed(source))
                            n00bb_internal_error(
                                core_translation_unit_diagnostics(source),
                                source,
                                link_source_location(symbol->decl_node),
                                "registering module globals",
                                "could not bind global `%.*s` to its "
                                "canonical declaration",
                                (int)symbol->name.u8_bytes,
                                symbol->name.data);
                        ok = false;
                    }
                }
            }
            else {
                if (!n00bb_unit_failed(source))
                    n00bb_internal_error(
                        core_translation_unit_diagnostics(source), source,
                        link_source_location(symbol->decl_node),
                        "registering module globals",
                        "could not create storage for global `%.*s`",
                        (int)symbol->name.u8_bytes, symbol->name.data);
                ok = false;
            }
            core_buffer_release(&name);
            if (!ok && n00bb_unit_failed(source)) ok = true;
        }
    }
    core_list_free(scopes);
    return ok && (!ir_diags_failed(diagnostics)
                  || n00bb_unit_failed(source));
}

static bool
bind_module_global_identities(ir_ctx_t *ir_context,
                              n00bb_link_analysis_t *analysis)
{
    if (!ir_context || !analysis) return false;
    for (size_t i = 0; i < analysis->units.len; i++) {
        core_translation_unit_t *source = analysis->units.data[i];
        n00bb_tu_state_t *source_state = tu_state(source);
        if (!source_state || !source_state->annotations) return false;
        core_list_t(parse_scope_ptr_t) scopes = link_declaration_scopes(
            source_state, CORE_STRING_STATIC(""));
        bool ok = true;
        for (size_t s = 0; ok && s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 ok && symbol; symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_VARIABLE
                    || !n00bb_unit_defines_global(analysis, source, symbol)
                    || !is_module_global_symbol(analysis->grammar, symbol))
                    continue;
                ir_global_t *canonical = link_ir_global_for_symbol(analysis,
                                                                    symbol);
                if (!canonical) continue;
                core_buffer_t name = {0};
                append_module_symbol_name(source, symbol, &name);
                for (size_t j = 0; ok && j < analysis->units.len; j++) {
                    ir_module_t *module = unit_module(
                        tu_state(analysis->units.data[j]));
                    ir_global_t *view = module
                        ? ir_global_find(module, core_buffer_string(&name))
                        : nullptr;
                    if (view)
                        ok = ir_global_set_identity(ir_context, view,
                                                    canonical);
                }
                core_buffer_release(&name);
            }
        }
        core_list_free(scopes);
        if (!ok) return false;
    }
    return true;
}

static ir_global_t *
canonical_unit_global(n00bb_link_analysis_t *analysis, core_string_t name)
{
    for (size_t u = 0; name.data && u < analysis->units.len; u++) {
        ir_module_t *module = unit_module(tu_state(analysis->units.data[u]));
        core_string_t module_name = module ? ir_module_name(module)
                                           : (core_string_t){};
        size_t prefix = module_name.u8_bytes;
        if (!prefix || !core_string_starts_with(name, module_name)
            || name.u8_bytes <= prefix || name.data[prefix] != '.') continue;
        ir_global_t *global = ir_global_find(module, name);
        if (global) return ir_global_identity(global);
    }
    return nullptr;
}

static bool
bind_cached_global_identities(ir_ctx_t *ir_context,
                              n00bb_link_analysis_t *analysis)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        ir_module_t *cached = state && state->interface_module
            ? interface_body_module(analysis, unit->module.identifier)
            : nullptr;
        for (size_t g = 0; cached && g < ir_global_count(cached); g++) {
            ir_global_t *global = ir_global_at(cached, g);
            ir_global_t *canonical = canonical_unit_global(analysis,
                                                           global->name);
            th_type *open = canonical ? th_resolve(canonical->type) : nullptr;
            if (open && canonical != global && th_kind_of(open) == TH_VAR
                && !th_var_rebind(open, global->type))
                return false;
            if (canonical && canonical != global
                && !ir_global_set_identity(ir_context, global, canonical))
                return false;
            if (canonical && canonical != global && !global->is_extern) {
                if (!analysis->cached_globals_ready) {
                    core_dict_init(&analysis->cached_globals, nullptr,
                                   nullptr);
                    analysis->cached_globals_ready = true;
                }
                core_dict_put(&analysis->cached_globals,
                              ir_global_identity(canonical), global);
            }
        }
    }
    return true;
}

th_type *
lowered_rest_type(th_ctx *types, const th_param *source, th_mode mode,
                  th_param *lowered)
{
    if (!types || !source || !lowered || !th_mode_is_valid(mode))
        return nullptr;
    th_type *type = mode == TH_INOUT
        ? th_ind(types, 0, source->type) : source->type;
    if (!type) return nullptr;
    *lowered = (th_param){
        .mode = mode == TH_CONSUME || mode == TH_VIEW ? mode : TH_IN,
        .type = type,
    };
    return type;
}

static th_type *
function_semantic_shape(parse_grammar_t *grammar, core_translation_unit_t *unit,
                        parse_sym_entry_t *symbol, th_type *source_type)
{
    n00bb_tu_state_t *state = tu_state(unit);
    source_type = source_type ? th_resolve(source_type) : nullptr;
    if (!state || !symbol || !source_type
        || th_kind_of(source_type) != TH_FUNCTION) return nullptr;
    if (parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.generated-value-method")))
        return source_type;
    parse_tree_t *signature = child_nt(grammar, symbol->decl_node,
                                       CORE_STRING_STATIC("func-type-signature"));
    core_list_t(parse_sym_entry_ptr_t) formals =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *rest_symbol = nullptr;
    collect_ir_parameters(grammar, signature, &formals, &rest_symbol);
    if (formals.len != th_func_nparams(source_type)
        || ((rest_symbol != nullptr) != (th_func_rest(source_type) != nullptr))) {
        core_list_free(formals);
        return nullptr;
    }

    core_list_t(th_param) parameters = core_list_new(th_param);
    core_list_t(th_kwparam) keywords = core_list_new(th_kwparam);
    bool ok = true;
    for (size_t i = 0; ok && i < formals.len; i++) {
        const th_param *source = th_func_param(source_type, i);
        parse_sym_entry_t *formal = formals.data[i];
        th_mode mode = formal && formal->has_mode ? formal->mode : TH_IN;
        if (!source || !formal || !th_mode_is_valid(mode)) {
            ok = false;
            break;
        }
        core_list_push(parameters, ((th_param){
            .mode = mode, .type = source->type,
        }));
    }
    const th_param *source_rest = th_func_rest(source_type);
    th_param lowered_rest = {0};
    const th_param *rest = nullptr;
    if (source_rest) {
        th_mode mode = rest_symbol && rest_symbol->has_mode
            ? rest_symbol->mode : TH_IN;
        rest = lowered_rest_type(state->types, source_rest, mode,
                                 &lowered_rest) ? &lowered_rest : nullptr;
        if (!rest) ok = false;
    }
    for (size_t i = 0; ok && i < th_func_nkw(source_type); i++) {
        const th_kwparam *keyword = th_func_kw(source_type, i);
        if (!keyword) ok = false;
        else core_list_push(keywords, *keyword);
    }
    th_type *semantic_type = ok
        ? th_func_with_error(state->types, th_func_ret(source_type),
            th_func_error(source_type), parameters.data, parameters.len,
            rest, keywords.data, keywords.len) : nullptr;
    core_list_free(formals);
    core_list_free(parameters);
    core_list_free(keywords);
    return semantic_type;
}

static th_type *
function_semantic_type(parse_grammar_t *grammar, core_translation_unit_t *unit,
                       parse_sym_entry_t *symbol)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !symbol || !symbol->type) return nullptr;
    th_type *contract = n00bb_symbol_signature(symbol);
    th_type *source_type = th_type_clone(state->types, contract);
    return function_semantic_shape(grammar, unit, symbol, source_type);
}

static th_type *
function_semantic_contract(parse_grammar_t *grammar,
                           core_translation_unit_t *unit,
                           parse_sym_entry_t *symbol)
{
    n00bb_tu_state_t *state = tu_state(unit);
    th_type *contract = symbol && symbol->callable_contract
        ? symbol->callable_contract : nullptr;
    if (!state || !contract) return nullptr;
    return function_semantic_shape(grammar, unit, symbol, contract);
}

static th_type *
trait_method_semantic_shape(parse_grammar_t *grammar,
                       core_translation_unit_t *unit,
                       parse_sym_entry_t *symbol, th_type *source)
{
    n00bb_tu_state_t *state = tu_state(unit);
    source = th_resolve(source);
    parse_tree_t *signature = symbol && symbol->decl_node
        ? child_nt(grammar, symbol->decl_node, CORE_STRING_STATIC("trait-func-type-signature"))
        : nullptr;
    parse_tree_t *parameters_node = signature
        ? child_nt(grammar, signature, CORE_STRING_STATIC("trait-param-decl")) : nullptr;
    if (!state || !source || th_kind_of(source) != TH_FUNCTION
        || !parameters_node) return nullptr;
    core_list_t(parse_sym_entry_ptr_t) formals =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *rest_symbol = nullptr;
    collect_ir_parameters(grammar, parameters_node, &formals, &rest_symbol);
    if (formals.len != th_func_nparams(source)
        || ((rest_symbol != nullptr) != (th_func_rest(source) != nullptr))) {
        core_list_free(formals);
        return nullptr;
    }
    core_list_t(th_param) parameters = core_list_new(th_param);
    core_list_t(th_kwparam) keywords = core_list_new(th_kwparam);
    bool ok = true;
    for (size_t i = 0; ok && i < formals.len; i++) {
        const th_param *input = th_func_param(source, i);
        parse_sym_entry_t *formal = formals.data[i];
        th_mode mode = formal && formal->has_mode ? formal->mode : TH_IN;
        if (!input || !formal || !th_mode_is_valid(mode)) ok = false;
        else core_list_push(parameters, ((th_param){
            .mode = mode, .type = input->type,
        }));
    }
    const th_param *source_rest = th_func_rest(source);
    th_param lowered_rest = {0};
    const th_param *rest = nullptr;
    if (source_rest) {
        th_mode mode = rest_symbol && rest_symbol->has_mode
            ? rest_symbol->mode : TH_IN;
        rest = lowered_rest_type(state->types, source_rest, mode,
                                 &lowered_rest) ? &lowered_rest : nullptr;
        if (!rest) ok = false;
    }
    for (size_t i = 0; ok && i < th_func_nkw(source); i++) {
        const th_kwparam *keyword = th_func_kw(source, i);
        if (!keyword) ok = false;
        else core_list_push(keywords, *keyword);
    }
    th_type *semantic = ok
        ? th_func_with_error(state->types, th_func_ret(source),
            th_func_error(source), parameters.data, parameters.len, rest,
            keywords.data, keywords.len) : nullptr;
    core_list_free(formals);
    core_list_free(parameters);
    core_list_free(keywords);
    return semantic;
}

static th_type *
trait_method_semantic_type(parse_grammar_t *grammar,
                      core_translation_unit_t *unit,
                      parse_sym_entry_t *symbol,
                      th_type **callable_contract)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (callable_contract) *callable_contract = nullptr;
    if (!state || !symbol) return nullptr;
    th_type *contract = n00bb_symbol_signature(symbol);
    th_type *source = contract ? th_resolve(contract) : nullptr;
    th_type *semantic = source
        ? trait_method_semantic_shape(grammar, unit, symbol, source) : nullptr;
    if (semantic && callable_contract) *callable_contract = semantic;
    return semantic;
}

static th_type *
interface_function_semantic_type(th_ctx *types, parse_sym_entry_t *symbol)
{
    th_type *contract = symbol && symbol->callable_contract
        ? symbol->callable_contract : symbol ? symbol->type : nullptr;
    th_type *signature = contract ? th_resolve(contract) : nullptr;
    if (!types || !signature || th_kind_of(signature) != TH_FUNCTION)
        return nullptr;
    size_t count = function_interface_parameter_count(symbol);
    size_t expected = th_func_nparams(signature)
        + (th_func_rest(signature) != nullptr);
    if (count != expected) return nullptr;

    core_list_t(th_param) parameters = core_list_new(th_param);
    bool ok = true;
    for (size_t i = 0; ok && i < th_func_nparams(signature); i++) {
        const th_param *source = th_func_param(signature, i);
        uint8_t flags = 0, mode = 0;
        if (!source || !function_interface_parameter(symbol, i, nullptr,
                &flags, &mode) || (flags & ~1u)
            || !th_mode_is_valid((th_mode)mode)
            || ((flags & 1u) && !th_mode_is_input_only((th_mode)mode))) {
            ok = false;
            break;
        }
        core_list_push(parameters, ((th_param){
            .mode = mode, .type = source->type,
        }));
    }
    const th_param *source_rest = th_func_rest(signature);
    th_param rest_value = {0};
    const th_param *rest = nullptr;
    if (ok && source_rest) {
        uint8_t flags = 0, mode = 0;
        if (!function_interface_parameter(symbol,
                th_func_nparams(signature), nullptr, &flags, &mode)
            || flags != 2u || !th_mode_is_valid((th_mode)mode)) ok = false;
        else {
            rest = lowered_rest_type(types, source_rest, (th_mode)mode,
                                     &rest_value) ? &rest_value : nullptr;
            if (!rest) ok = false;
        }
    }
    th_type *semantic = ok
        ? th_func_with_error(types, th_func_ret(signature),
            th_func_error(signature), parameters.data, parameters.len,
            rest, nullptr, 0) : nullptr;
    core_list_free(parameters);
    return semantic;
}

static bool
ir_initialization_error(core_translation_unit_t *unit, const char *what)
{
    if (unit
        && !core_diagnostics_failed(core_translation_unit_diagnostics(unit)))
        n00bb_internal_error(core_translation_unit_diagnostics(unit), unit,
            (core_diag_location_t){0}, "setting up code generation", "%s",
            what);
    return false;
}

static bool
register_optional_parameter_metadata(parse_grammar_t *grammar,
                                     parse_sym_entry_t *symbol,
                                     ir_func_t *function, bool named)
{
    if (!symbol || !function) return false;
    parse_tree_t *signature = child_nt(grammar, symbol->decl_node,
                                       CORE_STRING_STATIC("func-type-signature"));
    if (!signature)
        signature = child_nt(grammar, symbol->decl_node,
                             CORE_STRING_STATIC("trait-func-type-signature"));
    parse_tree_t *formals = child_nt(grammar, signature, CORE_STRING_STATIC("param-decl"));
    if (!formals)
        formals = child_nt(grammar, signature, CORE_STRING_STATIC("trait-param-decl"));
    size_t count = th_func_nparams(function->type)
        - (th_func_rest(symbol->type) != nullptr);
    named = named && !function->n_params;
    for (size_t i = 0; i < count; i++) {
        uint8_t flags = 0;
        core_string_t name = {0};
        if (!function_parameter_info(grammar, symbol, formals, i,
                nullptr, &name, &flags, nullptr)) return !symbol->decl_node;
        if (named && (!name.data || !ir_param_add(function, name,
                          th_func_param(function->type, i)->type)))
            return false;
        if ((flags & 1u)
            && !ir_func_set_parameter_optional(function, (uint32_t)i, true))
            return false;
    }
    return true;
}

static ir_func_t *
cached_function_body(n00bb_link_analysis_t *analysis,
                     core_translation_unit_t *unit,
                     core_string_t semantic_name,
                     bool overloaded, core_string_t declaration_key)
{
    ir_module_t *cached = analysis
        ? interface_body_module(analysis, unit->module.identifier)
        : nullptr;
    if (!cached || !semantic_name.data) return nullptr;
    core_string_t local = {};
    int64_t separator = overloaded && declaration_key.data
        ? core_string_find_byte(declaration_key, ':') : -1;
    if (separator >= 0)
        local = core_string_slice(declaration_key, (size_t)separator + 1,
            declaration_key.u8_bytes - (size_t)separator - 1);
    else {
        core_string_t start = semantic_name;
        (void)n00bb_name_member(semantic_name, unit->module.identifier,
                                &start);
        local = n00bb_function_root(start);
    }
    ir_func_t *body = local.u8_bytes ? ir_func_find(cached, local) : nullptr;
    return body && body->n_blocks ? body : nullptr;
}

bool
attach_cached_body(ir_ctx_t *ir_context, ir_func_t *function, ir_func_t *body)
{
    ir_value_t *address = body ? ir_func_address(body->owner, body) : nullptr;
    if (body) function->definition = body;
    return !body || (address && ir_annot_set(ir_context, &function->annots,
        (ir_annot_t){.key = CORE_STRING_STATIC("n00bb.cached_body"), .kind = IR_ANNOT_VALUE,
                     .v = address}));
}

ir_module_t *
n00bb_source_unit_module(void *context, core_string_t name)
{
    n00bb_link_analysis_t *analysis = context;
    for (size_t i = 0; name.data && i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state->interface_module
            && core_string_eq(name, unit->module.identifier))
            return state->ir_module;
    }
    return nullptr;
}

static ir_module_t *
program_or_cached_module(void *context, core_string_t name)
{
    n00bb_link_analysis_t *analysis = context;
    ir_module_t *module = n00bb_source_unit_module(context, name);
    return module ? module : n00bb_cache_module(analysis->cache, name);
}

ir_module_t *
n00bb_linked_unit_module(n00bb_link_analysis_t *analysis, core_string_t name)
{
    core_translation_unit_t *unit = n00bb_unit_named(analysis, name);
    n00bb_tu_state_t *state = unit ? tu_state(unit) : nullptr;
    return state ? state->ir_module : nullptr;
}

static bool
program_interface_module(void *context, core_string_t name)
{
    n00bb_link_analysis_t *analysis = context;
    for (size_t i = 0; name.data && i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        if (tu_state(unit)->interface_module
            && core_string_eq(name, unit->module.identifier))
            return true;
    }
    return false;
}

bool
initialize_ir_modules(ir_ctx_t *ir_context, parse_grammar_t *grammar,
                      core_link_unit_t *link,
                      n00bb_link_analysis_t *analysis,
                      uint32_t producer_stage)
{
    core_list_t(core_translation_unit_ptr_t) *units = &analysis->units;
    core_target_desc_t target;
    if (!n00bb_output_target_profile(&target)) {
        if (units->len)
            ir_initialization_error(units->data[0],
                "the target is not an ABI v0.1 target");
        return false;
    }
    analysis->target = target;
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_tree_t *tree = n00bb_tu_parsed_tree(state);
        if (!tree || !state->semantic_annotation_complete)
            return ir_initialization_error(unit,
                "translation unit has no annotated tree for Lantern IR");
        state->ir_module = ir_module_new(ir_context,
            unit->module.identifier, n00bb_type_profile(), target.triple);
        if (!state->ir_module)
            return ir_initialization_error(unit,
                "could not allocate the Lantern IR module");
        core_string_t path = core_source_file_path(&unit->source);
        int32_t file_id = ir_file_add(state->ir_module, path);
        ir_module_set_span(state->ir_module, ir_span_make(file_id, 0,
            (uint32_t)core_source_file_size(&unit->source), 1, 1, 1, 1));
        if (!record_trait_checks(grammar, unit, state->ir_module, ir_context,
                                 file_id))
            return ir_initialization_error(unit,
                "could not record trait checks in Lantern IR");
    }

    for (size_t i = 0; i < units->len; i++)
        if (!register_nominal_types(grammar, units->data[i],
                tu_state(units->data[i])->ir_module, false))
            return ir_initialization_error(units->data[i],
                "could not retain nominal representations in Lantern IR");

    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        ir_module_t *module = tu_state(unit)->ir_module;
        if (!register_nominal_types(grammar, unit, module, true))
            return ir_initialization_error(unit,
                "could not retain nominal representations in Lantern IR");
        if (!register_variant_types(unit, module))
            return ir_initialization_error(unit,
                "could not register variant definitions in Lantern IR");
        if (!register_record_types(grammar, unit, module))
            return ir_initialization_error(unit,
                "could not register record definitions in Lantern IR");
    }

    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        for (size_t j = 0; j < units->len; j++) {
            if (units->data[i] == units->data[j]
                || !n00bb_unit_reaches_import(unit, units->data[j])) continue;
            if (!register_imported_record_layouts(grammar, units->data[j],
                                                   tu_state(unit)->ir_module))
                return ir_initialization_error(unit,
                    "could not register imported record layout in Lantern IR");
        }
    }

    core_dict_t references;
    reference_pair_t *reference_pairs = index_link_references(link,
                                                              &references);
    ir_diags_t **source_diagnostics = core_alloc_array(ir_diags_t *,
                                                       units->len);
    for (size_t i = 0; i < units->len; i++)
        for (size_t j = 0; j < units->len; j++)
            if (!register_module_globals(grammar, units->data[j],
                    units->data[i], tu_state(units->data[i])->ir_module,
                    ir_context, analysis, link, &references,
                    &source_diagnostics[j], producer_stage)) {
                core_dict_free(&references);
                core_free(reference_pairs);
                core_free(source_diagnostics);
                return ir_initialization_error(units->data[i],
                    "could not register module globals");
            }
    core_dict_free(&references);
    core_free(reference_pairs);
    core_free(source_diagnostics);
    if (!bind_module_global_identities(ir_context, analysis))
        return ir_initialization_error(units->len ? units->data[0] : nullptr,
            "could not bind module-local global views to their canonical "
            "identities");
    size_t declaration_order[units->len + 1];
    size_t source_count = 0;
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 0; i < units->len; i++)
            if (tu_state(units->data[i])->interface_module == (pass == 1))
                declaration_order[source_count++] = i;
    source_count = 0;
    for (size_t i = 0; i < units->len; i++)
        source_count += !tu_state(units->data[i])->interface_module;

    for (size_t k = 0; k <= units->len; k++) {
        if (k == source_count) {
            if (!n00bb_cache_load_ir(analysis->cache, n00bb_source_unit_module,
                    program_interface_module, analysis))
                return ir_initialization_error(
                    units->len ? units->data[0] : nullptr,
                    "could not load the IR of cached SCCs");
            if (!load_library_ir(analysis, program_or_cached_module))
                return ir_initialization_error(
                    units->len ? units->data[0] : nullptr,
                    "could not load the IR a library interface carries");
            if (!bind_cached_global_identities(ir_context, analysis))
                return ir_initialization_error(
                    units->len ? units->data[0] : nullptr,
                    "could not bind cached globals to their canonical identities");
        }
        if (k == units->len) break;
        size_t i = declaration_order[k];
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        core_list_t(parse_scope_ptr_t) scopes = link_declaration_scopes(
            state, CORE_STRING_STATIC(""));
        for (size_t s = 0; s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_FUNCTION || !symbol->type)
                    continue;
                bool definition = function_symbol_at(grammar,
                        symbol->decl_node)
                    || (!state->interface_module && parse_sym_binding(symbol,
                        CORE_STRING_STATIC("n00bb.generated-value-method")));
                parse_tree_t *prelude = definition
                    ? child_nt(grammar, symbol->decl_node, CORE_STRING_STATIC("func-prelude"))
                    : nullptr;
                bool always_inline = prelude
                    && child_nt(grammar, prelude, CORE_STRING_STATIC("func-inline"));
                ir_linkage_t linkage = !definition ? IR_LINK_EXTERN
                    : symbol_is_private(symbol)
                        && !core_string_eq(symbol->name,
                                           CORE_STRING_STATIC("__finalize"))
                        ? IR_LINK_INTERNAL : IR_LINK_EXTERNAL;
                th_type *semantic_type = state->interface_module
                    ? symbol->user_data
                        ? interface_function_semantic_type(state->types,
                                                           symbol)
                        : interface_context_hook_symbol(symbol)
                            || parse_sym_binding(symbol, CORE_STRING_STATIC(
                                "n00bb.generated-value-method"))
                            ? symbol->type
                            : nullptr
                    : function_semantic_type(grammar, unit, symbol);
                if (semantic_type && th_kind_of(semantic_type) == TH_FUNCTION)
                    semantic_type = th_func_collect_rest(state->types,
                                                         semantic_type);
                core_link_declaration_t *declaration =
                    link_declaration_for_symbol(analysis, symbol);
                core_string_t declaration_key = declaration
                    ? core_link_declaration_key(declaration)
                    : (core_string_t){0};
                core_string_t overload_group = declaration
                    ? core_link_declaration_overload_group(declaration)
                    : (core_string_t){0};
                bool overloaded = declaration && overload_group.data
                    && core_link_unit_overload_member_count(link,
                        unit->module.identifier, overload_group) > 1;
                core_string_t semantic_name = symbol->name;
                if (state->interface_module) {
                    n00bb_export_origin_t *origin = parse_sym_binding(symbol,
                        CORE_STRING_STATIC("n00bb.export-origin"));
                    if (origin) semantic_name = origin->declaration;
                    size_t module_size = origin ? origin->module.u8_bytes : 0;
                    if (origin && state->ir_module
                        && core_string_eq(origin->module,
                                          ir_module_name(state->ir_module))
                        && semantic_name.u8_bytes > module_size + 1
                        && core_string_starts_with(semantic_name,
                                                   origin->module)
                        && semantic_name.data[module_size] == '.')
                        semantic_name = core_string_slice(semantic_name,
                            module_size + 1,
                            semantic_name.u8_bytes - module_size - 1);
                }
                if (!state->interface_module && overloaded
                    && declaration_key.data) {
                    int64_t separator = core_string_find_byte(
                        declaration_key, ':');
                    if (separator >= 0)
                        semantic_name = core_string_slice(declaration_key,
                            (size_t)separator + 1,
                            declaration_key.u8_bytes - (size_t)separator - 1);
                }
                ir_func_t *function = declaration
                    ? core_link_declaration_binding(declaration,
                        CORE_STRING_STATIC("n00bb.ir.function"))
                    : nullptr;
                bool new_function = function == nullptr;
                ir_func_t *cached_body = new_function
                    && state->interface_module
                    ? cached_function_body(analysis, unit, semantic_name,
                                           overloaded, declaration_key)
                    : nullptr;
                if (new_function && semantic_type)
                    function = ir_func_add(state->ir_module, cached_body
                                               ? cached_body->name
                                               : semantic_name,
                                           semantic_type, linkage);
                if (new_function && function && !cached_body
                    && symbol->decl_node && state->ir_module)
                    function->span = lower_span_for_tree(
                        &(ir_lower_context_t){
                            .file_id = ir_file_count(state->ir_module) - 1},
                        symbol->decl_node);
                always_inline = always_inline
                    || (cached_body && ir_func_is_always_inline(cached_body));
                if (always_inline && function
                    && !ir_func_set_always_inline(ir_context, function, true)) {
                    core_list_free(scopes);
                    return ir_initialization_error(unit,
                        "could not attach inline semantics to a function");
                }
                if (new_function && function
                    && !register_optional_parameter_metadata(
                        grammar, symbol, function, false)) {
                    core_list_free(scopes);
                    return ir_initialization_error(unit,
                        "could not attach optional parameter semantics to a function");
                }
                if (function && cached_body
                    && !attach_cached_body(ir_context, function,
                                           cached_body)) {
                    core_list_free(scopes);
                    return ir_initialization_error(unit,
                        "could not attach a cached body to a function");
                }
                bool polymorphic_contract = symbol->callable_contract
                    && !th_is_closed(state->types,
                                     symbol->callable_contract);
                th_type *ir_callable_contract = th_func_collect_rest(
                    state->types,
                    polymorphic_contract && !state->interface_module
                        ? function_semantic_contract(grammar, unit, symbol)
                        : symbol->callable_contract);
                if (new_function && function && polymorphic_contract
                    && (!ir_callable_contract
                        || !ir_func_set_callable_contract(
                            function, ir_callable_contract))) {
                    core_list_free(scopes);
                    return ir_initialization_error(unit,
                        "could not attach a callable contract to a function declaration");
                }
                if ((!function || !declaration) && n00bb_unit_failed(unit))
                    continue;
                if (!function || !declaration
                    || !core_link_declaration_set_binding(link, declaration,
                        CORE_STRING_STATIC("n00bb.ir.function"),
                        producer_stage, function)) {
                    core_list_free(scopes);
                    return ir_initialization_error(unit,
                        "could not bind function declarations in Lantern IR");
                }
            }
        }
        core_list_free(scopes);
    }
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        if (!symtab) return ir_initialization_error(unit,
            "translation unit has no symbol table for trait requirements");
        for (parse_scope_t *scope = symtab->all_scopes; scope;
             scope = scope->all_next) {
            if (!scope_in_value_namespace(scope)
                || !core_string_eq(scope->scope_tag,
                                CORE_STRING_STATIC("trait"))) continue;
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_FUNCTION) continue;
                core_buffer_t name = {0};
                core_buffer_printf(&name, "__trait_req_%.*s_%.*s",
                    (int)scope->name.u8_bytes, scope->name.data,
                    (int)symbol->name.u8_bytes, symbol->name.data);
                th_type *contract = nullptr;
                th_type *semantic_type = state->interface_module
                    && !symbol->decl_node ? symbol->type
                    : trait_method_semantic_type(grammar, unit, symbol,
                                                 &contract);
                if (state->interface_module && !symbol->decl_node)
                    contract = symbol->callable_contract;
                ir_func_t *function = semantic_type
                    ? ir_func_add(state->ir_module, core_buffer_string(&name),
                                  semantic_type, IR_LINK_EXTERN) : nullptr;
                uint64_t contract_arity = 0;
                bool polymorphic_contract = contract
                    && th_binder_arity(contract, &contract_arity)
                    && contract_arity > 0;
                if (function && polymorphic_contract
                    && !ir_func_set_callable_contract(function, contract))
                    function = nullptr;
                if (function
                    && !register_optional_parameter_metadata(
                        grammar, symbol, function, true)) function = nullptr;
                core_link_declaration_t *declaration =
                    link_declaration_for_symbol(analysis, symbol);
                if (!function || !declaration
                    || !core_link_declaration_set_binding(link, declaration,
                        CORE_STRING_STATIC("n00bb.ir.function"),
                        producer_stage, function)) {
                    core_buffer_release(&name);
                    return ir_initialization_error(unit,
                        "could not register an abstract trait method in Lantern IR");
                }
                core_buffer_release(&name);
            }
        }
    }
    for (size_t i = 0; i < units->len; i++)
        if (!add_context_hook_functions(grammar, units->data[i], analysis,
                                         link, producer_stage))
            return ir_initialization_error(units->data[i],
                "could not register context hooks in Lantern IR");
    if (!restore_declarations(analysis))
        return ir_initialization_error(units->len ? units->data[0] : nullptr,
            "could not restore declarations from a linked interface");
    for (size_t i = 0; i < units->len; i++)
        if (!tu_state(units->data[i])->interface_module
            && !record_mark_annotations(grammar, units->data[i], analysis,
                                        ir_context))
            return ir_initialization_error(units->data[i],
                "could not record mark annotations in Lantern IR");
    return true;
}

bool
validate_module_declarations(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             ir_ctx_t *ir_context)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->ir_module) return false;
    ir_diags_t *diagnostics = attach_ir_diags(ir_context, nullptr, unit,
                                              nullptr);
    if (!diagnostics) return false;
    int32_t file_id = ir_file_count(state->ir_module) - 1;
    ir_lower_context_t span_context = {.file_id = file_id,
                                       .diagnostics = diagnostics};
    bool valid = true;
    core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
    for (size_t s = 0; valid && s < scopes.len; s++) {
        for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
             symbol; symbol = symbol->next_in_scope) {
            parse_tree_t *binding = symbol_binding_node(symbol);
            if (symbol->kind == PARSE_SYM_FUNCTION
                || symbol->kind == PARSE_SYM_MODULE
                || context_symbol(grammar, symbol)
                || (symbol->decl_node
                    && nt_is(grammar, symbol->decl_node, CORE_STRING_STATIC("trait-decl")))
                || (symbol->decl_node
                    && (field_record_declaration(grammar, symbol->decl_node)
                        || nt_is(grammar, symbol->decl_node, CORE_STRING_STATIC("variant-decl"))
                        || nt_is(grammar, symbol->decl_node, CORE_STRING_STATIC("enum-stmt"))))
                || (symbol->decl_node
                    && nested_module_declaration_owner(grammar,
                                                        symbol->decl_node))
                || !binding)
                continue;
            if (symbol->kind == PARSE_SYM_VARIABLE
                && symbol->type
                && (nt_is(grammar, binding, CORE_STRING_STATIC("sym-decl"))
                    || nt_is(grammar, binding,
                             CORE_STRING_STATIC("pattern-binding"))
                    || nt_is(grammar, binding, CORE_STRING_STATIC("assign-stmt")))) {
                bool registered = link_ir_global_for_symbol(
                    state->link_analysis, symbol) != nullptr;
                if (registered) continue;
            }
            if (n00bb_unit_failed(unit)) continue;
            lower_report(&span_context, N00B_DIAG_NOT_SUPPORTED_YET,
                lower_span_for_tree(&span_context, binding),
                N00B_TEXT_NOT_SUPPORTED_YET_THIS_KIND,
                N00BB_ARGS(ARG_TEXT(symbol->name)));
            valid = false;
            break;
        }
    }
    core_list_free(scopes);
    return valid && (!ir_diags_failed(diagnostics) || n00bb_unit_failed(unit));
}

static bool
n00bb_emit_default_param(ir_lowerer_t *lowerer,
                        ir_lower_context_t *context,
                        n00bb_default_param_t *parameter)
{
    if (!lowerer || !context || !parameter) return false;
    if (parameter->emitted) return true;
    parameter->lowering = true;
    n00bb_lower_context_t *lower = context->user;
    if (!lower) return false;
    for (size_t i = 0; i < lower->default_params.len; i++) {
        n00bb_default_param_t *dependency = &lower->default_params.data[i];
        if (dependency->emitted || !n00bb_default_references(
                parameter->expression, dependency->symbol)) continue;
        if (dependency == parameter) {
            lower_report(context, N00B_DIAG_DEFAULT_CYCLE,
                lower_symbol_name_span(context, parameter->symbol),
                N00B_TEXT_THE_DEFAULT_VALUE_OF_PARAMETER_2,
                N00BB_ARGS(ARG_TEXT(parameter->symbol->name)));
            lower_help(context, N00B_TEXT_DEFAULT_READS_ITSELF_HELP,
                N00BB_NO_ARGS);
        }
        else {
            lower_report(context, N00B_DIAG_DEFAULT_ORDER,
                lower_symbol_name_span(context, parameter->symbol),
                N00B_TEXT_DEFAULT_USES_LATER_PARAMETER,
                N00BB_ARGS(ARG_TEXT(parameter->symbol->name),
                           ARG_TEXT(dependency->symbol->name)));
            lower_help(context, N00B_TEXT_DEFAULT_ORDER_HELP, N00BB_NO_ARGS);
        }
        return false;
    }
    ir_span_t span = lower_span_for_tree(context, parameter->symbol->decl_node);
    th_type *bool_type = n00bb_ir_builtin_type(context->module, CORE_STRING_STATIC("bool"));
    uint64_t id = context->next_value_id++;
    char provided_buffer[48], missing_buffer[48], join_buffer[48];
    char present_buffer[48];
    core_string_t provided_name = n00bb_format_name(provided_buffer,
        sizeof(provided_buffer), "__n00bb_default_provided_%llu",
        (unsigned long long)id);
    core_string_t missing_name = n00bb_format_name(missing_buffer,
        sizeof(missing_buffer), "__n00bb_default_missing_%llu",
        (unsigned long long)id);
    core_string_t join_name = n00bb_format_name(join_buffer,
        sizeof(join_buffer), "__n00bb_default_join_%llu",
        (unsigned long long)id);
    core_string_t present_name = n00bb_format_name(present_buffer,
        sizeof(present_buffer), "__n00bb_default_present_%llu",
        (unsigned long long)id);
    ir_block_t *provided = ir_block_add(context->function, provided_name);
    ir_block_t *missing = ir_block_add(context->function, missing_name);
    ir_block_t *join = ir_block_add(context->function, join_name);
    ir_instr_t *present_test = provided && missing && join && bool_type
        ? ir_lower_emit_instruction(context, IR_OP_PARAM_PRESENT, span)
        : nullptr;
    bool present_built = present_test
        && ir_instr_set_dest(present_test, present_name, bool_type)
        && ir_instr_operand_value(present_test, parameter->argument);
    ir_instr_t *branch = present_built
        ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
    if (!present_built || !branch
        || !ir_instr_operand_value(branch, present_test->dest)
        || !ir_instr_operand_block(branch, provided)
        || !ir_instr_operand_block(branch, missing)) {
        lower_internal(context, span,
                    "could not branch on default argument presence");
        return false;
    }
    branch->span = span;

    context->block = provided;
    context->reachable = true;
    ir_value_t *given = parameter->argument;
    if (!given || !n00bb_emit_local_write(context, parameter->local,
                                           given, span)
        || !n00bb_emit_jump(context, join, span)) {
        lower_internal(context, span,
                    "could not bind a supplied default argument");
        return false;
    }
    context->block = missing;
    context->reachable = true;
    n00bb_default_param_t *active_default = lower
        ? lower->active_default_param : nullptr;
    if (lower) lower->active_default_param = parameter;
    ir_lower_result_t value = ir_lower_tree(lowerer, context,
        lower ? lower->grammar : nullptr, parameter->expression);
    if (lower) lower->active_default_param = active_default;
    if (value.status == IR_LOWER_FAILED) return false;
    if (value.status == IR_LOWER_VALUE && value.value) {
        ir_value_t *resolved = value.value;
        if (!n00bb_emit_local_write(context, parameter->local, resolved,
                                    span)) {
            lower_internal(context, span,
                        "could not store a default parameter value");
            return false;
        }
        if (!n00bb_emit_jump(context, join, span)) return false;
    }
    else if (value.status != IR_LOWER_TERMINATED) {
        lower_internal(context, span,
                       "a default parameter expression produced no value");
        return false;
    }
    context->block = join;
    context->reachable = true;
    parameter->lowering = false;
    parameter->emitted = true;
    return true;
}

bool
n00bb_initialize_default_params(ir_lowerer_t *lowerer,
                                ir_lower_context_t *context,
                                core_list_t(n00bb_default_param_t) *params)
{
    for (size_t i = 0; i < params->len; i++)
        if (!params->data[i].expression
            || !n00bb_emit_default_param(lowerer, context,
                                         &params->data[i])) return false;
    return true;
}

bool
n00bb_initialize_default_reference(ir_lower_context_t *context,
                                   parse_visit_t *visit, bool entering,
                                   void *state)
{
    (void)state;
    if (!entering || !visit->node) return true;
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !lower->active_default_param) return true;
    parse_sym_entry_t *symbol = attached_symbol(visit->grammar, lower->unit,
                                                visit->node);
    if (!symbol) symbol = parse_tree_sym(visit->node);
    for (size_t i = 0; symbol && i < lower->default_params.len; i++) {
        n00bb_default_param_t *parameter =
            &lower->default_params.data[i];
        if (parameter->symbol != symbol) continue;
        if (!parameter->emitted) {
            lower_report(context, N00B_DIAG_DEFAULT_ORDER,
                lower_span_for_tree(context, visit->node),
                N00B_TEXT_THIS_DEFAULT_VALUE_READS_PARAMETER,
                N00BB_ARGS(ARG_TEXT(symbol->name)));
            return false;
        }
        return true;
    }
    return true;
}

bool
bind_missing_external(ir_lower_context_t *context,
                      parse_sym_entry_t *symbol)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
    if (!state || !symbol || !symbol->type) return false;
    if (symbol->kind == PARSE_SYM_FUNCTION) {
        parse_sym_entry_t *function = canonical_symbol(symbol);
        ir_func_t *target = link_ir_function_for_symbol(
            state->link_analysis, function);
        core_translation_unit_t *target_unit = target
            ? unit_for_symbol(lower->grammar, lower->units, function)
            : nullptr;
        ir_module_t *target_module = unit_module(
            target_unit ? tu_state(target_unit) : nullptr);
        ir_value_t *address = target && target_module
            ? ir_func_address(target_module, target) : nullptr;
        return address && ir_lower_bind_value(context, symbol, address);
    }
    if (symbol->kind != PARSE_SYM_VARIABLE
        || context_symbol(lower->grammar, symbol)) return false;
    core_translation_unit_t *owner = unit_for_symbol(lower->grammar,
                                                    lower->units, symbol);
    n00bb_tu_state_t *owner_state = owner ? tu_state(owner) : nullptr;
    parse_tree_t *binding = symbol_binding_node(symbol);
    if (!owner_state || (!owner_state->interface_module && (!binding
        || (!nt_is(lower->grammar, binding, CORE_STRING_STATIC("sym-decl"))
            && !nt_is(lower->grammar, binding,
                      CORE_STRING_STATIC("pattern-binding"))
            && !nt_is(lower->grammar, binding, CORE_STRING_STATIC("assign-stmt"))))))
        return false;
    ir_global_t *global = link_ir_global_for_symbol(
        state->link_analysis, symbol);
    ir_global_t *phase_global = global
        ? ir_global_find(context->module, global->name) : nullptr;
    ir_value_t *value = phase_global
        ? ir_global_address(context->module, phase_global) : nullptr;
    return value && ir_lower_bind_value(context, symbol, value);
}

static bool
self_derived(core_dict_t *derived, const ir_instr_t *in)
{
    ir_value_t *value = in->n_operands
        ? ir_operand_value(&in->operands[0]) : nullptr;
    ir_local_t *local = in->n_operands
        ? ir_operand_local(&in->operands[0]) : nullptr;
    return (value && core_dict_contains(derived, value))
        || (local && core_dict_contains(derived, local));
}

static void
collect_self_values(ir_func_t *function, core_dict_t *derived)
{
    core_dict_put(derived, function->params[0], function->params[0]);
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t b = 0; b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                ir_instr_t *in = function->blocks[b]->instrs[i];
                bool carries = in->op == IR_OP_COPY || in->op == IR_OP_LOAD
                    || in->op == IR_OP_ADDRESS_OF
                    || in->op == IR_OP_NOMINAL_CAST
                    || in->op == IR_OP_TYPE_NARROW;
                if (!carries || !self_derived(derived, in)) continue;
                void *into = in->dest_kind == IR_DEST_LOCAL
                    ? (void *)in->dest_local : (void *)in->dest;
                if (into && !core_dict_contains(derived, into)) {
                    core_dict_put(derived, into, into);
                    changed = true;
                }
            }
    }
}

static bool
self_field_stored(ir_func_t *function, core_dict_t *derived,
                  core_string_t name)
{
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = function->blocks[b]->instrs[i];
            const ir_value_t *address = in->op == IR_OP_STORE
                    && in->n_operands == 2
                ? ir_operand_value(&in->operands[0]) : nullptr;
            const ir_instr_t *field = address && address->kind == IR_VAL_INSTR
                ? address->def : nullptr;
            if (field && field->op == IR_OP_FIELD && field->n_operands == 2
                && field->operands[1].kind == IR_OPD_NAME
                && core_string_eq(field->operands[1].name, name)
                && self_derived(derived, field))
                return true;
        }
    return false;
}

static bool
check_initialized_fields(core_translation_unit_t *unit, ir_func_t *function)
{
    if (!function->n_blocks || !function->n_params
        || !core_string_eq(function->params[0]->name,
                           CORE_STRING_STATIC("self"))
        || !core_string_starts_with(function->name,
                                    CORE_STRING_STATIC("__init"))
        || (function->name.u8_bytes > 6 && function->name.data[6] != '#'))
        return true;
    th_ctx *types = ir_module_types(function->owner);
    th_type *self = th_resolve(function->params[0]->type);
    if (self && th_kind_of(self) == TH_INDIRECTION)
        self = th_resolve(th_ind_referent(self));
    th_type *shape = self ? th_fields_record(types, self) : nullptr;
    core_dict_t derived;
    core_dict_init(&derived, nullptr, nullptr);
    bool collected = false;
    bool ok = true;
    for (size_t i = 0; shape && th_kind_of(shape) == TH_RECORD
         && i < th_record_len(shape); i++) {
        const th_member *member = th_record_member(shape, i);
        if (!member || !member->type
            || !n00bb_type_may_finalize(unit, member->type)) continue;
        if (!collected) collect_self_values(function, &derived);
        collected = true;
        if (self_field_stored(function, &derived, member->name)) continue;
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(unit);
        core_string_t type_text = n00bb_print_type(types, member->type);
        n00bb_report(diagnostics, unit, N00B_DIAG_FINALIZED_FIELD_UNSET,
            n00bb_span_location(function->span),
            N00B_TEXT_FINALIZED_FIELD_UNSET,
            N00BB_ARGS(ARG_TEXT(member->name), ARG_TEXT(type_text)));
        n00bb_report_help(diagnostics, N00B_TEXT_SET_EVERY_FINALIZED_FIELD,
                          N00BB_NO_ARGS);
        ok = false;
    }
    core_dict_free(&derived);
    return ok;
}

bool
n00bb_check_initialized_fields(n00bb_link_analysis_t *analysis)
{
    bool ok = true;
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        ir_module_t *module = state && !state->interface_module
            ? state->ir_module : nullptr;
        for (size_t f = 0; module && f < ir_func_count(module); f++)
            ok = check_initialized_fields(unit, ir_func_at(module, f)) && ok;
    }
    return ok;
}

bool
n00bb_open_result_variable(ir_lower_context_t *context,
                           parse_sym_entry_t *function)
{
    parse_sym_entry_t *result = function
        ? parse_sym_binding(function, N00BB_RESULT_KEY) : nullptr;
    if (!result) return true;
    th_ctx *types = ir_module_types(context->module);
    th_type *signature = th_resolve(context->function->type);
    if (signature && th_kind_of(signature) == TH_FUNCTION)
        result->type = th_func_ret(signature);
    if (!result->type) result->type = th_var(types);
    ir_span_t span = ir_module_span(context->module);
    span.implied = true;
    ir_local_t *local = ir_local_add(context->function,
                                     CORE_STRING_STATIC("result"),
                                     result->type);
    if (!local || !n00bb_bind_local(context, result, local, span))
        return false;
    ir_instr_t *nil = ir_lower_emit_instruction(context, IR_OP_NIL, span);
    if (!nil || !ir_lower_set_temp_dest(context, nil,
            CORE_STRING_STATIC("__n00bb_result_nil_"), th_nil(types)))
        return false;
    n00bb_lower_context_t *lower = context->user;
    if (!n00bb_emit_local_write(context, local, nil->dest, span))
        return false;
    core_list_push(tu_state(lower->unit)->result_defaults,
                   context->block->instrs[context->block->n_instrs - 1]);
    return true;
}
