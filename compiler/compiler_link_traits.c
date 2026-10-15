#include <string.h>

#include "compiler_link.h"
#include "compiler_tu.h"
#include "parse/tree_util.h"
#include "runtime_native_roots.h"

typedef th_type *th_type_ptr_t;
core_list_decl(th_type_ptr_t);
core_list_decl(size_t);

parse_sym_entry_t *
resolve_linked_method(core_link_unit_t *link, n00bb_link_use_site_t *site,
                     core_translation_unit_t *unit,
                     const th_env_method_t *method)
{
    core_link_reference_t *reference = site ? site->reference : nullptr;
    if (!reference || !method
        || !method->module || !method->root_name) return nullptr;
    parse_sym_entry_t *builtin_symbol = nullptr;
    if (!strcmp(method->module, "builtins") && method->qualified_name) {
        const char *internal_name = strrchr(method->qualified_name, '.');
        internal_name = internal_name ? internal_name + 1 : nullptr;
        n00bb_tu_state_t *state = tu_state(unit);
        parse_namespace_t *builtins = state && state->annotations
            ? parse_symtab_ns(state->annotations->symtab,
                              CORE_STRING_STATIC("builtin")) : nullptr;
        if (internal_name && builtins && builtins->root)
            builtin_symbol = parse_symtab_lookup_in(builtins->root,
                core_string_borrow(internal_name, strlen(internal_name)));
        if (!n00bb_builtin_option_function(builtin_symbol)
            && !n00bb_builtin_array_read_function(builtin_symbol)
            && !n00bb_builtin_array_write_function(builtin_symbol)
            && !n00bb_builtin_string_operator(builtin_symbol))
            builtin_symbol = nullptr;
        if (builtin_symbol) {
            core_link_declaration_metadata_t metadata = {
                .kind = PARSE_SYM_FUNCTION,
                .visibility = 0,
                .contract = builtin_symbol->callable_contract
                    ? builtin_symbol->callable_contract : builtin_symbol->type,
            };
            core_link_declaration_t *declaration = nullptr;
            core_buffer_t builtin_key = {0};
            core_buffer_printf(&builtin_key, "%u:%s",
                (unsigned)PARSE_SYM_FUNCTION, method->root_name);
            core_string_t key_name = core_string_from_cstr(builtin_key.data);
            declaration = core_link_unit_find_declaration(link,
                CORE_STRING_STATIC("builtins"), key_name);
            bool registered = declaration || core_link_unit_require_external(
                link, CORE_STRING_STATIC("builtins"), key_name, &metadata,
                &declaration);
            core_buffer_release(&builtin_key);
            if (!registered) return nullptr;
        }
    }
    core_buffer_t key = {0};
    if (builtin_symbol) {
        core_buffer_printf(&key, "%u:%s", (unsigned)PARSE_SYM_FUNCTION,
                           method->root_name);
    }
    else {
        size_t module_length = strlen(method->module);
        const char *identity = method->qualified_name
            && !strncmp(method->qualified_name, method->module,
                        module_length)
            && method->qualified_name[module_length] == '.'
            ? method->qualified_name + module_length + 1 : nullptr;
        if (!identity || !*identity) return nullptr;
        core_buffer_printf(&key, "%u:%s", (unsigned)PARSE_SYM_FUNCTION,
                           identity);
    }
    if (!builtin_symbol && !strcmp(method->module, "builtins")
        && !linked_method_declaration(link, method)) {
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->link_analysis || !site->resolved_trait_name
            || !register_builtin_method_declaration(link,
                state->link_analysis, unit, site->resolved_trait_name,
                method)) {
            core_buffer_release(&key);
            return nullptr;
        }
    }
    bool resolved = core_link_unit_resolve_reference(link, reference,
        core_string_borrow(method->module, strlen(method->module)),
        core_string_from_cstr(key.data));
    core_buffer_release(&key);
    if (!resolved) return nullptr;
    core_link_declaration_t *declaration =
        core_link_reference_target(reference);
    if (!declaration || core_link_declaration_kind(declaration)
            != PARSE_SYM_FUNCTION) return nullptr;
    n00bb_link_payload_t *payload = declaration
        ? core_link_declaration_payload(declaration) : nullptr;
    parse_sym_entry_t *symbol = builtin_symbol
        ? builtin_symbol : payload ? payload->symbol
        : core_link_declaration_binding(declaration,
            CORE_STRING_STATIC("n00bb.builtin-symbol"));
    if (!symbol || symbol->kind != PARSE_SYM_FUNCTION
        || !parse_tree_set_resolution_symbol(site->node, symbol)
        || !parse_sym_add_use(symbol, site->node)) return nullptr;
    site->symbol = symbol;
    return symbol;
}

core_link_declaration_t *
linked_method_declaration(core_link_unit_t *link,
                          const th_env_method_t *method)
{
    if (!link || !method || !method->module || !method->qualified_name)
        return nullptr;
    size_t module_length = strlen(method->module);
    if (strncmp(method->qualified_name, method->module, module_length)
        || method->qualified_name[module_length] != '.') return nullptr;
    core_buffer_t key = {0};
    core_buffer_printf(&key, "%u:%s", (unsigned)PARSE_SYM_FUNCTION,
                       method->qualified_name + module_length + 1);
    core_link_declaration_t *found = nullptr;
    for (size_t i = 0; i < core_link_unit_declaration_count(link); i++) {
        core_link_declaration_t *candidate =
            core_link_unit_declaration_at(link, i);
        if (core_string_eq(core_link_declaration_module(candidate),
                core_string_borrow(method->module, module_length))
            && core_string_eq(core_link_declaration_key(candidate),
                              core_string_from_cstr(key.data))) {
            found = candidate;
            break;
        }
    }
    core_buffer_release(&key);
    return found;
}

static bool
same_semantic_type(th_ctx *types, th_type *left, th_type *right)
{
    left = left ? th_resolve(left) : nullptr;
    right = right ? th_resolve(right) : nullptr;
    if (!left || !right) return false;
    if (left == right) return true;
    const char *left_name = nullptr, *right_name = nullptr;
    size_t left_length = 0, right_length = 0;
    size_t left_args = 0, right_args = 0;
    bool left_variant = false, right_variant = false;
    if (th_named_info(left, &left_name, &left_length, &left_variant,
                      &left_args)
        && th_named_info(right, &right_name, &right_length, &right_variant,
                         &right_args)) {
        if (left_variant != right_variant || left_length != right_length
            || left_args != right_args
            || memcmp(left_name, right_name, left_length)) return false;
        for (size_t i = 0; i < left_args; i++) {
            const th_arg *a = th_named_arg(left, i);
            const th_arg *b = th_named_arg(right, i);
            if (!a || !b || a->kind != b->kind) return false;
            if (a->kind == TH_ARG_TYPE
                && !same_semantic_type(types, a->type, b->type)) return false;
            if (a->kind == TH_ARG_UINT && a->u != b->u) return false;
            if (a->kind == TH_ARG_SINT && a->s != b->s) return false;
            if (a->kind == TH_ARG_STR
                && (a->str.n != b->str.n
                    || memcmp(a->str.p, b->str.p, a->str.n))) return false;
        }
        return true;
    }
    const uint8_t *left_encoding = nullptr, *right_encoding = nullptr;
    size_t left_size = 0, right_size = 0;
    return types && th_encode(types, left, &left_encoding, &left_size)
        && th_encode(types, right, &right_encoding, &right_size)
        && left_size == right_size
        && !memcmp(left_encoding, right_encoding, left_size);
}

static void
append_unique_dispatch_type(th_ctx *types,
                            core_list_t(th_type_ptr_t) *receiver_types,
                            th_type *candidate)
{
    if (!candidate || !th_is_closed(types, candidate)) return;
    for (size_t i = 0; i < receiver_types->len; i++)
        if (same_semantic_type(types, receiver_types->data[i], candidate))
            return;
    core_list_push(*receiver_types, candidate);
}

static bool
plan_closed_trait_dispatch(core_link_unit_t *link,
                           n00bb_link_analysis_t *analysis,
                           const th_env *environment,
                           core_translation_unit_t *unit,
                           n00bb_link_use_site_t *site,
                           const char *trait_name,
                           const char *method_name)
{
    if (!link || !analysis || !environment || !unit || !site || !trait_name
        || !method_name || analysis->may_link_further) return true;
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->types) return false;
    core_list_t(th_type_ptr_t) receiver_types =
        core_list_new(th_type_ptr_t);
    for (size_t i = 0; i < th_env_declaration_count(environment); i++) {
        th_env_declaration_view_t declaration = {0};
        if (!th_env_declaration_at(environment, i, &declaration)
            || declaration.kind != TH_ENV_DECLARATION_TYPE
            || !declaration.qualified_name
            || !th_env_type_visible(state->types,
                                    declaration.qualified_name)) continue;
        th_type *candidate = th_name(state->types,
                                     declaration.qualified_name);
        if (candidate && th_env_implements(state->types, candidate,
                                           trait_name))
            append_unique_dispatch_type(state->types, &receiver_types,
                                        candidate);
    }
    size_t method_count = th_env_method_candidates(state->types, trait_name,
                                                    method_name, nullptr, 0);
    th_env_method_t *methods = method_count
        ? core_alloc_array(th_env_method_t, method_count) : nullptr;
    if (method_count && (!methods || th_env_method_candidates(state->types,
            trait_name, method_name, methods, method_count) != method_count)) {
        core_free(methods);
        core_list_free(receiver_types);
        return false;
    }
    for (size_t i = 0; i < method_count; i++) {
        const th_param *receiver = methods[i].signature
            ? th_func_param(methods[i].signature, 0) : nullptr;
        if (!receiver) continue;
        th_type *pattern = th_resolve(receiver->type);
        if ((th_kind_of(pattern) == TH_SEQUENCE
             || th_kind_of(pattern) == TH_INDIRECTION)
            && !th_is_closed(state->types, pattern)) {
            bool found = false;
            for (size_t j = 0; j < receiver_types.len; j++)
                if (same_semantic_type(state->types, receiver_types.data[j],
                                       pattern)) found = true;
            if (!found) core_list_push(receiver_types, pattern);
        } else append_unique_dispatch_type(state->types, &receiver_types,
                                           pattern);
    }
    if (!receiver_types.len) {
        core_free(methods);
        core_list_free(receiver_types);
        return true;
    }
    size_t closed_count = 0;
    for (size_t i = 0; i < receiver_types.len; i++) {
        if (!th_is_closed(state->types, receiver_types.data[i])) continue;
        th_type *temporary = receiver_types.data[closed_count];
        receiver_types.data[closed_count++] = receiver_types.data[i];
        receiver_types.data[i] = temporary;
    }
    th_env_dispatch_entry_t *entries = core_alloc_array(
        th_env_dispatch_entry_t, receiver_types.len);
    size_t required = receiver_types.len;
    bool direct = true;
    bool resolved = entries != nullptr;
    for (size_t i = 0; resolved && i < required; i++) {
        th_type *receiver = receiver_types.data[i];
        entries[i].receiver_type = receiver;
        if (i < closed_count) {
            resolved = th_env_method_resolve(state->types, receiver,
                trait_name, method_name, &entries[i].method)
                == TH_ENV_METHOD_FOUND;
        } else {
            const th_env_method_t *selected = nullptr;
            for (size_t j = 0; j < method_count; j++) {
                const th_param *formal = methods[j].signature
                    ? th_func_param(methods[j].signature, 0) : nullptr;
                if (!formal || !same_semantic_type(state->types,
                        formal->type, receiver)) continue;
                if (selected) { resolved = false; break; }
                selected = &methods[j];
            }
            if (!selected) resolved = false;
            else entries[i].method = *selected;
        }
        if (i && resolved && strcmp(entries[0].method.qualified_name,
                                  entries[i].method.qualified_name))
            direct = false;
    }
    core_free(methods);
    if (!resolved) {
        core_free(entries);
        core_list_free(receiver_types);
        return false;
    }
    n00bb_trait_dispatch_plan_t *plan = core_arena_alloc(
        &state->semantic_values, sizeof(*plan));
    n00bb_trait_dispatch_entry_t *planned = core_arena_alloc(
        &state->semantic_values, required * sizeof(*planned));
    bool ok = plan && planned;
    size_t planned_count = 0;
    bool has_generic = false;
    n00bb_trait_dispatch_entry_t generic = {0};
    for (size_t i = 0; ok && i < required; i++) {
        core_link_declaration_t *declaration = linked_method_declaration(
            link, &entries[i].method);
        if (!declaration)
            declaration = register_builtin_method_declaration(link,
                analysis, unit, trait_name, &entries[i].method);
        n00bb_link_payload_t *payload = declaration
            ? core_link_declaration_payload(declaration) : nullptr;
        parse_sym_entry_t *implementation = payload
            ? payload->symbol : declaration
                ? core_link_declaration_binding(declaration,
                    CORE_STRING_STATIC("n00bb.builtin-symbol"))
                : nullptr;
        if (!implementation
            || !parse_sym_add_use(implementation, site->node)) {
            ok = false;
            break;
        }
        const th_param *method_receiver = entries[i].method.signature
            ? th_func_param(entries[i].method.signature, 0) : nullptr;
        bool generic_receiver = method_receiver
            && th_kind_of(th_resolve(method_receiver->type)) == TH_VAR
            && th_var_constraint_count(method_receiver->type) == 0;
        n00bb_trait_dispatch_entry_t entry = {
            .receiver_type = entries[i].receiver_type,
            .implementation = implementation,
            .generic_fallback = generic_receiver,
        };
        if (generic_receiver) {
            generic = entry;
            has_generic = true;
        } else {
            planned[planned_count++] = entry;
        }
    }
    if (ok && has_generic) planned[planned_count++] = generic;
    if (ok) {
        parse_tree_t *receiver_node = site->node
            && core_tree_num_children(site->node)
            ? core_tree_child(site->node, 0) : nullptr;
        th_type *subject_type = receiver_node && !core_tree_is_leaf(receiver_node)
            ? core_tree_node_value(receiver_node).type : nullptr;
        th_type *receiver_root = th_resolve(subject_type);
        while (receiver_root
            && th_kind_of(receiver_root) == TH_INDIRECTION)
            receiver_root = th_resolve(th_ind_referent(receiver_root));
        bool bare_type_variable = receiver_root
            && th_kind_of(receiver_root) == TH_VAR;
        *plan = (n00bb_trait_dispatch_plan_t){
            .entries = planned,
            .count = planned_count,
            .direct = !bare_type_variable && (direct || planned_count == 1),
        };
        site->closed_dispatch = plan;
    }
    core_free(entries);
    core_list_free(receiver_types);
    return ok;
}



bool
has_deferred_method_constraint(th_type *receiver, core_string_t method_name)
{
    receiver = th_resolve(receiver);
    if (!receiver || th_kind_of(receiver) != TH_VAR) return false;
    for (size_t i = 0; i < th_var_constraint_count(receiver); i++) {
        th_constraint constraint;
        if (th_var_constraint_at(receiver, i, &constraint)
            && constraint.kind == TH_CON_METHOD
            && constraint.method.name
            && strlen(constraint.method.name) == method_name.u8_bytes
            && !memcmp(constraint.method.name, method_name.data,
                       method_name.u8_bytes))
            return true;
    }
    return false;
}

bool
n00bb_method_requirement_matches(th_type *receiver,
                                 core_string_t method_name,
                                 th_type *signature)
{
    receiver = th_resolve(receiver);
    if (!receiver || th_kind_of(receiver) != TH_VAR || !signature)
        return true;
    for (size_t i = 0; i < th_var_constraint_count(receiver); i++) {
        th_constraint constraint;
        if (!th_var_constraint_at(receiver, i, &constraint)
            || constraint.kind != TH_CON_METHOD
            || !constraint.method.name
            || strlen(constraint.method.name) != method_name.u8_bytes
            || memcmp(constraint.method.name, method_name.data,
                      method_name.u8_bytes)) continue;
        th_type *call = constraint.method.call;
        return call && th_kind_of(th_resolve(call)) == TH_FUNCTION
            && th_func_nparams(signature) == th_func_nparams(call) + 1
            && th_func_nkw(signature) == th_func_nkw(call);
    }
    return true;
}

static bool
bind_deferred_method_failure(core_translation_unit_t *unit,
                             n00bb_tu_state_t *state, parse_tree_t *site,
                             th_type *receiver, core_string_t method_name,
                             parse_sym_entry_t *requirement)
{
    th_type *variable = receiver ? th_resolve(receiver) : nullptr;
    if (!variable || th_kind_of(variable) != TH_VAR) return true;
    th_type *failure = requirement->error_type
        ? requirement->error_type
        : requirement->type ? th_func_error(requirement->type) : nullptr;
    for (size_t i = 0; i < th_var_constraint_count(variable); i++) {
        th_constraint constraint;
        if (!th_var_constraint_at(variable, i, &constraint)
            || constraint.kind != TH_CON_METHOD
            || !constraint.method.name
            || strlen(constraint.method.name) != method_name.u8_bytes
            || memcmp(constraint.method.name, method_name.data,
                      method_name.u8_bytes)) continue;
        if (failure && th_method_obligation_set_error(state->types,
                variable, constraint.method.name,
                constraint.method.result, failure)) continue;
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0001"),
            CORE_STRING_STATIC(
                "could not bind the trait method failure channel"),
            link_source_location(site));
        return false;
    }
    return true;
}

static bool
trait_name_matches(const char *qualified, core_string_t short_name)
{
    if (!qualified) return false;
    if (short_name.data
        && memchr(short_name.data, '.', short_name.u8_bytes))
        return strlen(qualified) == short_name.u8_bytes
            && !memcmp(qualified, short_name.data, short_name.u8_bytes);
    const char *name = strrchr(qualified, '.');
    name = name ? name + 1 : qualified;
    return strlen(name) == short_name.u8_bytes
        && !memcmp(name, short_name.data, short_name.u8_bytes);
}

bool
has_trait_constraint(th_type *receiver, const char *trait_name)
{
    receiver = th_resolve(receiver);
    if (!receiver || th_kind_of(receiver) != TH_VAR || !trait_name)
        return false;
    const char *short_name = strrchr(trait_name, '.');
    short_name = short_name ? short_name + 1 : trait_name;
    core_string_t trait = core_string_borrow(short_name, strlen(short_name));
    for (size_t i = 0; i < th_var_constraint_count(receiver); i++) {
        th_constraint constraint;
        if (th_var_constraint_at(receiver, i, &constraint)
            && constraint.kind == TH_CON_IMPLEMENTS
            && trait_name_matches(constraint.iface, trait)) return true;
    }
    return false;
}

static const char *
trait_requirement_name(th_ctx *types, core_string_t trait_name,
                     core_string_t method_name)
{
    char *method_text = core_strndup(method_name.data,
                                     method_name.u8_bytes);
    size_t count = th_env_method_requirements(types, method_text, nullptr, 0);
    th_env_method_requirement_t *requirements = count
        ? core_alloc_array(th_env_method_requirement_t, count) : nullptr;
    if (count)
        th_env_method_requirements(types, method_text, requirements, count);
    const char *found = nullptr;
    for (size_t i = 0; i < count; i++) {
        if (!trait_name_matches(requirements[i].trait_name, trait_name))
            continue;
        if (found && strcmp(found, requirements[i].trait_name)) {
            found = nullptr;
            break;
        }
        found = requirements[i].trait_name;
    }
    core_free(requirements);
    core_free(method_text);
    return found;
}

static parse_sym_entry_t *
builtin_trait_requirement_symbol(core_translation_unit_t *unit,
                                  core_string_t trait_name,
                                  core_string_t method_name)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_symtab_t *symtab = state && state->annotations
        ? state->annotations->symtab : nullptr;
    if (!symtab || !state->types || !trait_name.data || !method_name.data)
        return nullptr;
    for (size_t i = 0; i < trait_name.u8_bytes; i++)
        if (trait_name.data[i] == '.') return nullptr;
    parse_namespace_t *builtins = parse_symtab_ns(
        symtab, CORE_STRING_STATIC("builtin"));
    parse_sym_entry_t *trait = builtins
        ? parse_symtab_lookup_in(builtins->root, trait_name) : nullptr;
    parse_sym_entry_t *cached = trait && trait->member_scope
        ? parse_symtab_lookup_in(trait->member_scope, method_name) : nullptr;
    if (cached) return cached;

    char *method_text = core_strndup(method_name.data, method_name.u8_bytes);
    size_t count = th_env_method_requirements(state->types, method_text,
                                               nullptr, 0);
    th_env_method_requirement_t *requirements = count
        ? core_alloc_array(th_env_method_requirement_t, count) : nullptr;
    if (count) th_env_method_requirements(state->types, method_text,
                                           requirements, count);
    th_type *contract = nullptr;
    for (size_t i = 0; i < count; i++)
        if (requirements[i].trait_name
            && core_string_eq(core_string_from_cstr(
                requirements[i].trait_name), trait_name)) {
            contract = requirements[i].contract;
            break;
        }
    core_free(requirements);
    core_free(method_text);
    if (!contract || !builtins || !builtins->root) return nullptr;
    parse_scope_t *saved = builtins->current;
    parse_symtab_enter_scope(symtab, CORE_STRING_STATIC("builtin"),
                             builtins->root);
    if (!trait) {
        trait = parse_symtab_add(symtab, CORE_STRING_STATIC("builtin"),
                                 trait_name, PARSE_SYM_TYPEDEF, nullptr);
        if (trait) {
            trait->type = th_name(state->types, trait->name.data);
            trait->adt_kind = CORE_STRING_STATIC("trait");
            trait->visibility = CORE_STRING_STATIC("public");
            parse_sym_set_binding(trait,
                CORE_STRING_STATIC("n00bb.builtin-type"), trait);
            parse_symtab_push_scope(symtab, CORE_STRING_STATIC("builtin"),
                                    trait_name);
            trait->member_scope = builtins->current;
            trait->member_scope->scope_tag = CORE_STRING_STATIC("trait");
        }
    }
    else if (trait->member_scope)
        parse_symtab_enter_scope(symtab, CORE_STRING_STATIC("builtin"),
                                 trait->member_scope);
    parse_sym_entry_t *method = trait && trait->member_scope
        ? parse_symtab_add(symtab, CORE_STRING_STATIC("builtin"),
                            method_name, PARSE_SYM_FUNCTION, nullptr) : nullptr;
    if (method) {
        method->owner_type = trait;
        method->type = contract;
        method->callable_contract = contract;
        method->visibility = CORE_STRING_STATIC("public");
        parse_sym_set_binding(method,
            CORE_STRING_STATIC("n00bb.builtin-trait-method"), trait);
        parse_sym_set_phases(method, PARSE_SYM_PHASE_BOTH,
            PARSE_SYM_PHASE_NONE, PARSE_SYM_PHASE_NONE);
    }
    parse_symtab_enter_scope(symtab, CORE_STRING_STATIC("builtin"), saved);
    return method;
}

parse_sym_entry_t *
trait_method_requirement_symbol(parse_grammar_t *grammar,
                                n00bb_link_analysis_t *analysis,
                                core_translation_unit_t *caller,
                                core_string_t qualified_trait,
                                core_string_t method_name)
{
    if (!grammar || !analysis || !caller || !qualified_trait.data
        || !method_name.data) return nullptr;
    size_t trait_start = qualified_trait.u8_bytes;
    while (trait_start && qualified_trait.data[trait_start - 1] != '.')
        trait_start--;
    core_string_t module = trait_start
        ? core_string_borrow(qualified_trait.data, trait_start - 1)
        : (core_string_t){0};
    core_string_t trait = trait_start
        ? core_string_borrow(qualified_trait.data + trait_start,
                             qualified_trait.u8_bytes - trait_start)
        : qualified_trait;
    parse_sym_entry_t *found = nullptr;
    for (size_t i = 0; i < analysis->units.len; i++) {
        core_translation_unit_t *owner = analysis->units.data[i];
        if (module.data
            && !(module.data && core_string_eq(module,
                owner->module.identifier)))
            continue;
        if (!module.data && owner != caller
            && find_direct_import(caller, owner->module.identifier) != owner)
            continue;
        parse_sym_entry_t *trait_symbol = unit_trait_symbol(
            grammar, owner, trait, owner != caller);
        if (!trait_symbol || !trait_symbol->member_scope) continue;
        n00bb_tu_state_t *state = tu_state(owner);
        parse_sym_entry_t *method = state && state->annotations
            ? parse_symtab_lookup_scoped(state->annotations->symtab,
                trait_symbol->member_scope, CORE_STRING_STATIC(""),
                method_name) : nullptr;
        if (!method || method->kind != PARSE_SYM_FUNCTION) continue;
        if (found && found != method) return nullptr;
        found = method;
    }
    return found ? found : builtin_trait_requirement_symbol(caller,
        qualified_trait, method_name);
}

parse_sym_entry_t *
linkable_trait_requirement(n00bb_link_analysis_t *analysis,
                           core_translation_unit_t *unit,
                           n00bb_link_use_site_t *site,
                           parse_sym_entry_t *symbol)
{
    if (!analysis || !unit || !symbol) return nullptr;
    if (!analysis->may_link_further || (symbol->scope
        && core_string_eq(symbol->scope->scope_tag,
                          CORE_STRING_STATIC("trait")))) return symbol;
    if (!site || !site->resolved_trait_name) return nullptr;
    core_string_t method = site->dispatch_method.data
        ? site->dispatch_method : site->member_name.data
            ? site->member_name : symbol->name;
    return trait_method_requirement_symbol(analysis->grammar, analysis,
        unit, core_string_from_cstr(site->resolved_trait_name), method);
}

static parse_sym_entry_t *
deferred_trait_method_requirement(parse_grammar_t *grammar,
                                  n00bb_link_analysis_t *analysis,
                                  core_translation_unit_t *unit,
                                  th_type *receiver_type,
                                  core_string_t method_name)
{
    n00bb_tu_state_t *state = tu_state(unit);
    receiver_type = th_resolve(receiver_type);
    if (!state || !receiver_type || th_kind_of(receiver_type) != TH_VAR)
        return nullptr;
    parse_sym_entry_t *found = nullptr;
    bool has_interface_constraint = false;
    for (size_t i = 0; i < th_var_constraint_count(receiver_type); i++) {
        th_constraint constraint;
        if (!th_var_constraint_at(receiver_type, i, &constraint)
            || constraint.kind != TH_CON_IMPLEMENTS || !constraint.iface)
            continue;
        has_interface_constraint = true;
        parse_sym_entry_t *method = trait_method_requirement_symbol(
            grammar, analysis, unit, core_string_from_cstr(constraint.iface),
            method_name);
        if (!method) continue;
        if (found && found != method) return nullptr;
        found = method;
    }
    if (has_interface_constraint) return found;

    char *method_text = core_strndup(method_name.data,
                                     method_name.u8_bytes);
    size_t count = th_env_method_requirements(state->types, method_text,
                                               nullptr, 0);
    th_env_method_requirement_t *requirements = count
        ? core_alloc_array(th_env_method_requirement_t, count) : nullptr;
    if (count)
        th_env_method_requirements(state->types, method_text, requirements,
                                   count);
    for (size_t i = 0; i < count; i++) {
        if (!requirements[i].trait_name) continue;
        if (!n00bb_method_requirement_matches(receiver_type, method_name,
                                               requirements[i].signature))
            continue;
        parse_sym_entry_t *method = trait_method_requirement_symbol(
            grammar, analysis, unit,
            core_string_from_cstr(requirements[i].trait_name), method_name);
        if (!method) continue;
        if (found && found != method) {
            found = nullptr;
            break;
        }
        found = method;
    }
    core_free(requirements);
    core_free(method_text);
    return found;
}



static bool
tree_contains_node(parse_tree_t *root, parse_tree_t *wanted)
{
    if (!root || !wanted) return false;
    if (root == wanted) return true;
    if (core_tree_is_leaf(root)) return false;
    for (size_t i = 0; i < core_tree_num_children(root); i++)
        if (tree_contains_node(core_tree_child(root, i), wanted)) return true;
    return false;
}

static bool
field_use_is_write(parse_grammar_t *grammar, parse_sym_entry_t *field,
                   parse_tree_t *use,
                   bool *also_reads)
{
    for (parse_tree_t *parent = core_tree_node_value(use).parent;
         parent; parent = core_tree_node_value(parent).parent) {
        const parse_rule_t *rule = parse_tree_rule(grammar, parent);
        if (rule && parse_rule_find_annot(grammar, rule, PARSE_ANNOT_CALL)
            && core_tree_num_children(parent)) {
            parse_sym_entry_t *callee = bound_function_symbol(
                core_tree_child(parent, 0));
            bool store = callee && parse_sym_binding(callee,
                CORE_STRING_STATIC("n00bb.atomic.store"));
            store = store || (callee && parse_sym_binding(callee,
                CORE_STRING_STATIC("n00bb.atomic.store_relaxed")));
            store = store || (callee && parse_sym_binding(callee,
                CORE_STRING_STATIC("n00bb.atomic.store_release")));
            bool exchange = callee && parse_sym_binding(callee,
                CORE_STRING_STATIC("n00bb.atomic.exchange"));
            exchange = exchange || (callee && parse_sym_binding(callee,
                CORE_STRING_STATIC("n00bb.atomic.exchange_acquire")));
            exchange = exchange || (callee && parse_sym_binding(callee,
                CORE_STRING_STATIC("n00bb.atomic.exchange_relaxed")));
            bool cas = callee && parse_sym_binding(callee,
                CORE_STRING_STATIC("n00bb.atomic.compare_exchange"));
            bool fetch_or = callee && parse_sym_binding(callee,
                CORE_STRING_STATIC("n00bb.atomic.fetch_or"));
            fetch_or = fetch_or || (callee && parse_sym_binding(callee,
                CORE_STRING_STATIC("n00bb.atomic.fetch_or_release")));
            fetch_or = fetch_or || (callee && parse_sym_binding(callee,
                CORE_STRING_STATIC("n00bb.atomic.fetch_or_relaxed")));
            bool fetch_and = callee && parse_sym_binding(callee,
                CORE_STRING_STATIC("n00bb.atomic.fetch_and"));
            bool fetch_add = callee && parse_sym_binding(callee,
                CORE_STRING_STATIC("n00bb.atomic.fetch_add"));
            fetch_add = fetch_add || (callee && parse_sym_binding(callee,
                CORE_STRING_STATIC("n00bb.atomic.fetch_add_relaxed")));
            bool fetch_sub = callee && parse_sym_binding(callee,
                CORE_STRING_STATIC("n00bb.atomic.fetch_sub"));
            bool ref_write = callee && n00bb_builtin_function(callee)
                && core_string_eq(callee->name,
                                  CORE_STRING_STATIC("ref_write"));
            parse_tree_t *first = store || exchange || cas || fetch_or
                || fetch_and
                || fetch_add || fetch_sub
                || ref_write
                ? call_argument_for_parameter(grammar, parent, callee,
                                              0, false) : nullptr;
            if (first && tree_contains_node(first, use)
                && core_string_eq(first_token_text(first),
                                  CORE_STRING_STATIC("ref"))) {
                *also_reads = exchange || cas || fetch_or || fetch_and
                    || fetch_add
                    || fetch_sub;
                return true;
            }
        }
        if (!nt_is(grammar, parent, "assign-stmt")
            && !nt_is(grammar, parent, "binop-assign-stmt")) continue;
        if (core_tree_num_children(parent) < 2
            || !tree_contains_node(core_tree_child(parent, 0), use))
            continue;
        for (parse_tree_t *path = use; path != parent;
             path = core_tree_node_value(path).parent) {
            parse_tree_t *ancestor = core_tree_node_value(path).parent;
            if (nt_is(grammar, ancestor, "postfix-expr")
                && core_tree_num_children(ancestor) > 1
                && core_tree_child(ancestor, 0) == path) {
                core_string_t trait = {0}, method = {0};
                if (!field->type
                    || th_kind_of(th_resolve(field->type)) != TH_SEQUENCE
                    || !index_dispatch_for_node(grammar, ancestor,
                                                &trait, &method)
                    || (!core_string_eq(trait,
                        CORE_STRING_STATIC("Index_Write"))
                        && !core_string_eq(trait,
                            CORE_STRING_STATIC("Index_Slice_Write"))))
                    return false;
            }
        }
        *also_reads = nt_is(grammar, parent, "binop-assign-stmt");
        if (nt_is(grammar, parent, "assign-stmt")) {
            core_string_t op = first_token_text(core_tree_child(parent, 1));
            *also_reads = !core_string_eq(op, CORE_STRING_STATIC("="));
        }
        return true;
    }
    return false;
}

bool
diagnose_unused_link_symbols(parse_grammar_t *grammar,
                             n00bb_link_analysis_t *analysis)
{
    for (size_t i = 0; i < analysis->units.len; i++)
        if (core_diagnostics_failed(core_translation_unit_diagnostics(
                analysis->units.data[i]))) return true;

    for (size_t i = 0; i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->annotations) return false;
        for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
             scope; scope = scope->all_next) {
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                if (symbol->is_field && symbol->decl_node
                    && nt_is(grammar, symbol->decl_node, "type-decl-one")) {
                    size_t writes = symbol->definition_count, reads = 0;
                    for (size_t u = 0; u < symbol->use_count; u++) {
                        bool also_reads = false;
                        if (field_use_is_write(grammar, symbol,
                                               symbol->use_nodes[u],
                                               &also_reads)) {
                            writes++;
                            reads += also_reads;
                        }
                        else reads++;
                    }
                    if (writes && reads) continue;
                    core_buffer_t message = {0};
                    core_buffer_printf(&message, "field '%.*s' %s",
                        (int)symbol->name.u8_bytes, symbol->name.data,
                        writes ? "is set but never read" : "is never set");
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_WARNING,
                        writes ? CORE_STRING_STATIC("N00BB-W0002")
                               : CORE_STRING_STATIC("N00BB-W0001"),
                        core_string_from_cstr(message.data),
                        link_source_location(symbol->decl_node));
                    core_buffer_release(&message);
                    continue;
                }
                if (symbol->kind != PARSE_SYM_FUNCTION
                    || !symbol->decl_node
                    || !nt_is(grammar, symbol->decl_node, "func-def")
                    || symbol->use_count) continue;
                if (core_string_eq(symbol->name,
                                   CORE_STRING_STATIC("__finalize"))) continue;
                if (core_string_eq(unit->module.identifier,
                        CORE_STRING_STATIC("builtin.io.raw"))
                    && core_string_eq(symbol->name,
                        CORE_STRING_STATIC("__raw_repr_dispatch"))) continue;
                bool native_root = false;
                core_string_t module_name = unit->module.identifier;
                for (size_t r = 0; r < analysis->native_root_count; r++) {
                    if (native_root_matches(analysis->native_roots[r],
                            module_name, symbol->name)) {
                        native_root = true;
                        break;
                    }
                }
                for (size_t r = 0; r < n00bb_runtime_native_root_count; r++) {
                    const char *name = n00bb_runtime_native_roots[r].qualified_name;
                    if (strlen(name) == module_name.u8_bytes + 1
                            + symbol->name.u8_bytes
                        && !memcmp(name, module_name.data, module_name.u8_bytes)
                        && name[module_name.u8_bytes] == '.'
                        && !memcmp(name + module_name.u8_bytes + 1,
                                   symbol->name.data, symbol->name.u8_bytes)) {
                        native_root = true;
                        break;
                    }
                }
                if (native_root) continue;
                bool literal_handler = false;
                for (size_t r = 0; r < state->literal_modifiers.len; r++)
                    if (symbol == state->literal_modifiers.data[r].function) {
                        literal_handler = true;
                        break;
                    }
                if (literal_handler) continue;
                core_buffer_t message = {0};
                core_buffer_printf(&message, "function '%.*s' is never used",
                    (int)symbol->name.u8_bytes, symbol->name.data);
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_WARNING, CORE_STRING_STATIC("N00BB-W0003"),
                    core_string_from_cstr(message.data),
                    link_source_location(symbol->decl_node));
                core_buffer_release(&message);
            }
        }
    }
    return true;
}



static bool
validate_enclosing_return_type(parse_grammar_t *grammar,
                               core_translation_unit_t *unit,
                               n00bb_tu_state_t *state,
                               n00bb_link_use_site_t *site)
{
    for (parse_tree_t *at = site->node; at;
         at = core_tree_node_value(at).parent) {
        if (!nt_is(grammar, at, "return-stmt")) continue;
        parse_tree_t *expression = child_nt(grammar, at, "expression");
        parse_sym_entry_t *function = enclosing_function_symbol(grammar, at);
        th_type *signature = function ? function->type : nullptr;
        th_type *expected = signature ? th_func_ret(signature) : nullptr;
        th_type *actual = expression && !core_tree_is_leaf(expression)
            ? core_tree_node_value(expression).type : nullptr;
        if (!expected || !actual || !th_is_closed(state->types, expected))
            return true;
        if (th_unify(state->types, expected, actual)) return true;
        th_type *element = nullptr;
        if (n00bb_option_type_element(expected, &element)
            && (n00bb_nil_type(state->types, actual)
                || th_unify(state->types, element, actual))) return true;
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit, CORE_DIAG_ERROR,
            CORE_STRING_STATIC("N00BB-T0001"),
            CORE_STRING_STATIC(
                "function return expression does not match its declared result type after trait resolution"),
            link_source_location(at));
        return false;
    }
    return true;
}

static bool
validate_index_dispatch_result(parse_grammar_t *grammar,
                               core_translation_unit_t *unit,
                               n00bb_tu_state_t *state,
                               n00bb_link_use_site_t *site,
                               parse_sym_entry_t *symbol)
{
            bool result_dispatch = site->dispatch_trait.data
                && (core_string_eq(site->dispatch_method,
                                   CORE_STRING_STATIC("__read"))
                    || core_string_eq(site->dispatch_method,
                                      CORE_STRING_STATIC("__slice")));
    if (!result_dispatch || !symbol || symbol->kind != PARSE_SYM_FUNCTION
        || !symbol->type) return true;
    th_type *signature = symbol->callable_contract
        ? symbol->callable_contract : symbol->type;
    signature = th_type_clone(state->types, signature);
    th_type *expected = signature ? th_func_ret(signature) : nullptr;
    th_type *actual = core_tree_is_leaf(site->node)
        ? nullptr : core_tree_node_value(site->node).type;
    if (actual && expected
        && th_unify_in_place(state->types, actual, expected)) {
        core_tree_node_value(site->node).type = actual;
        return validate_enclosing_return_type(grammar, unit, state, site);
    }
    core_buffer_t message = {0};
    core_buffer_printf(&message,
        "index expression type %s does not match resolved trait result %s",
        actual ? n00bb_print_type(state->types, actual) : "unknown",
        expected ? n00bb_print_type(state->types, expected) : "unknown");
    core_diagnostics_report_at(
        core_translation_unit_diagnostics(unit), unit, CORE_DIAG_ERROR,
        CORE_STRING_STATIC("N00BB-T0001"),
        core_string_from_cstr(message.data),
        link_source_location(site->node));
    core_buffer_release(&message);
    return false;
}

static bool
validate_operator_dispatch_result(parse_grammar_t *grammar,
                                  core_translation_unit_t *unit,
                                  n00bb_tu_state_t *state,
                                  n00bb_link_use_site_t *site,
                                  parse_sym_entry_t *symbol)
{
    if (!site->operator_dispatch
        || core_string_eq(site->dispatch_trait,
                          CORE_STRING_STATIC("Equal"))
        || core_string_eq(site->dispatch_trait,
                          CORE_STRING_STATIC("Comparable"))) return true;
    th_type *contract = symbol && symbol->callable_contract
        ? symbol->callable_contract : symbol ? symbol->type : nullptr;
    th_type *signature = contract ? th_type_clone(state->types, contract)
                                  : nullptr;
    parse_tree_t *left = operator_receiver_node(site);
    bool unary = core_tree_num_children(site->node) == 2;
    parse_tree_t *right = unary ? nullptr : core_tree_child(site->node, 2);
    th_type *receiver = core_tree_node_value(left).type;
    th_type *argument = right ? core_tree_node_value(right).type : nullptr;
    th_type *result = core_tree_node_value(site->node).type;
    const th_param *self = signature && th_kind_of(signature) == TH_FUNCTION
        ? th_func_param(signature, 0) : nullptr;
    const th_param *other = signature && th_kind_of(signature) == TH_FUNCTION
        ? th_func_param(signature, 1) : nullptr;
    bool valid = self && (unary || other)
        && th_func_nparams(signature) == (unary ? 1 : 2)
        && !th_func_rest(signature)
        && th_mode_is_input_only(self->mode)
        && (unary || th_mode_is_input_only(other->mode))
        && receiver && (unary || argument) && result
        && th_unify_in_place(state->types, self->type, receiver)
        && (unary || th_unify_in_place(state->types, other->type, argument))
        && th_unify_in_place(state->types, th_func_ret(signature), result);
    if (valid)
        return validate_enclosing_return_type(grammar, unit, state, site);
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
        unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0001"),
        CORE_STRING_STATIC(
            "operator operands or result do not match the resolved trait method"),
        link_source_location(site->node));
    return false;
}

static bool
validate_trait_dispatch_result(parse_grammar_t *grammar,
                               core_translation_unit_t *unit,
                               n00bb_tu_state_t *state,
                               n00bb_link_use_site_t *site,
                               parse_sym_entry_t *symbol)
{
    return validate_index_dispatch_result(grammar, unit, state, site, symbol)
        && validate_operator_dispatch_result(grammar, unit, state, site,
                                             symbol);
}

typedef struct {
    parse_sym_entry_t *symbol;
    core_translation_unit_t *unit;
    th_type *receiver_pattern;
} n00bb_constructor_candidate_t;
core_list_decl(n00bb_constructor_candidate_t);

th_type *
constructor_signature_instance(th_ctx *types, parse_sym_entry_t *symbol)
{
    th_type *signature = symbol
        ? (symbol->callable_contract ? symbol->callable_contract
                                     : symbol->type) : nullptr;
    th_type *success = nullptr, *failure = nullptr;
    if (!signature || !th_env_signature_channels(types, signature,
                                                  &success, &failure))
        return nullptr;
    (void)failure;
    return th_type_clone(types, success);
}

static bool
constructor_arguments_match(parse_grammar_t *grammar,
                            core_translation_unit_t *unit,
                            parse_tree_t *construction,
                            parse_tree_t *existing_target,
                            parse_sym_entry_t *symbol,
                            th_type *signature)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!grammar || !state || !state->types || !construction || !symbol
        || !signature || th_kind_of(signature) != TH_FUNCTION
        || !th_func_nparams(signature)) return false;

    th_type *receiver_type = core_tree_node_value(
        existing_target ? existing_target : construction).type;
    const th_param *receiver = th_func_param(signature, 0);
    if (!receiver || !receiver_type
        || !th_unify_in_place(state->types, receiver->type, receiver_type))
        return false;

    parse_tree_t *declaration_signature = child_nt(grammar, symbol->decl_node,
                                                    "func-type-signature");
    parse_tree_t *formal_node = declaration_signature
        ? child_nt(grammar, declaration_signature, "param-decl") : nullptr;
    core_list_t(parse_sym_entry_ptr_t) formals =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *rest = nullptr;
    collect_ir_parameters(grammar, formal_node, &formals, &rest);

    size_t parameter_count = th_func_nparams(signature);
    bool *provided = parameter_count
        ? core_alloc_array(bool, parameter_count) : nullptr;
    bool valid = !parameter_count || provided;
    if (provided) memset(provided, 0, parameter_count * sizeof(bool));
    if (valid) provided[0] = true;

    parse_tree_t *call_args = child_nt(grammar, construction, "call-args");
    parse_tree_t *positional = child_nt(grammar, call_args, "positional-args");
    core_list_t(n00bb_tree_ptr_t) positional_nodes =
        core_list_new(n00bb_tree_ptr_t);
    collect_positional_nodes(grammar, positional, &positional_nodes);
    for (size_t i = existing_target ? 1 : 0;
         valid && i < positional_nodes.len; i++) {
        size_t index = existing_target ? i : i + 1;
        const th_param *parameter = index < parameter_count
            ? th_func_param(signature, index) : th_func_rest(signature);
        parse_tree_t *argument = positional_nodes.data[i];
        th_type *argument_type = core_tree_is_leaf(argument) ? nullptr
            : core_tree_node_value(argument).type;
        if (!parameter || !argument_type
            || (index < parameter_count && provided[index])
            || !th_unify_in_place(state->types, parameter->type,
                                  argument_type)) {
            valid = false;
            break;
        }
        if (index < parameter_count) provided[index] = true;
    }
    core_list_free(positional_nodes);

    parse_tree_t *keyword_args = child_nt(grammar, call_args, "keyword-args");
    core_list_t(n00bb_tree_ptr_t) keyword_nodes =
        core_list_new(n00bb_tree_ptr_t);
    collect_keyword_nodes(grammar, keyword_args, &keyword_nodes);
    for (size_t i = 0; valid && i < keyword_nodes.len; i++) {
        core_string_t name = call_keyword_name(keyword_nodes.data[i]);
        parse_tree_t *argument = child_nt(grammar, keyword_nodes.data[i],
                                           "expression");
        size_t index = 0;
        parse_sym_entry_t *formal = formal_symbol_named(grammar, formal_node,
                                                        name, &index);
        th_type *argument_type = argument && !core_tree_is_leaf(argument)
            ? core_tree_node_value(argument).type : nullptr;
        const th_param *parameter = index < parameter_count
            ? th_func_param(signature, index) : nullptr;
        if (!formal || index == 0 || !parameter || !argument_type
            || provided[index]
            || !th_unify_in_place(state->types, parameter->type,
                                  argument_type)) {
            valid = false;
            break;
        }
        provided[index] = true;
    }
    core_list_free(keyword_nodes);

    for (size_t i = 1; valid && i < parameter_count; i++) {
        if (provided[i]) continue;
        parse_sym_entry_t *formal = i < formals.len ? formals.data[i] : nullptr;
        if (!formal || !formal->is_optional) valid = false;
    }
    if (valid && rest && !th_func_rest(signature)) valid = false;
    core_free(provided);
    core_list_free(formals);
    return valid;
}

static bool
resolve_constructor_use_site(parse_grammar_t *grammar, core_link_unit_t *link,
                             n00bb_link_analysis_t *analysis,
                             core_translation_unit_t *unit,
                             n00bb_link_use_site_t *site)
{
    n00bb_tu_state_t *state = tu_state(unit);
    th_type *target = state && state->types
        ? core_tree_node_value(site->init_call ? site->receiver_node
                                               : site->node).type : nullptr;
    target = target ? th_resolve(target) : nullptr;
    if (!state || !target || th_kind_of(target) == TH_SEQUENCE
        || th_kind_of(target) == TH_VAR) return true;
    bool is_variant = false;
    if (th_named_info(target, nullptr, nullptr, &is_variant, nullptr)
        && is_variant) return true;

    core_list_t(n00bb_constructor_candidate_t) candidates =
        core_list_new(n00bb_constructor_candidate_t);
    const char *failed_constraint = nullptr;
    th_type *failed_constraint_type = nullptr;
    size_t constraint_failures = 0;
    for (size_t i = 0; i < analysis->units.len; i++) {
        core_translation_unit_t *candidate_unit = analysis->units.data[i];
        n00bb_tu_state_t *candidate_state = tu_state(candidate_unit);
        if (!candidate_state || !candidate_state->annotations) continue;
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(candidate_state);
        for (size_t s = 0; s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_FUNCTION
                    || !core_string_eq(symbol->name,
                                       CORE_STRING_STATIC("__init"))
                    || !symbol->type || !symbol->decl_node
                    || !symbol_is_public_declaration(symbol)) continue;
                th_type *pattern_signature = constructor_signature_instance(
                    state->types, symbol);
                const th_param *receiver = pattern_signature
                    && th_kind_of(pattern_signature) == TH_FUNCTION
                    ? th_func_param(pattern_signature, 0) : nullptr;
                if (!receiver || !th_env_type_pattern_subsumes(
                        receiver->type, target)) continue;
                th_type *argument_signature =
                    constructor_signature_instance(state->types, symbol);
                size_t errors_before = th_error_count(state->types);
                if (!argument_signature
                    || !constructor_arguments_match(grammar, unit,
                        site->node,
                        site->init_call ? site->receiver_node : nullptr,
                        symbol, argument_signature)) {
                    th_error_t detail = {0};
                    if (th_error_count(state->types) > errors_before
                        && th_error_at(state->types,
                            th_error_count(state->types) - 1, &detail)
                        && detail.constraint) {
                        failed_constraint = detail.constraint;
                        failed_constraint_type = detail.got;
                        constraint_failures++;
                    }
                    continue;
                }
                core_list_push(candidates,
                    ((n00bb_constructor_candidate_t){symbol,
                        candidate_unit, receiver->type}));
            }
        }
        core_list_free(scopes);
    }

    if (!candidates.len) {
        core_list_t(n00bb_tree_ptr_t) arguments =
            core_list_new(n00bb_tree_ptr_t);
        parse_tree_t *call_args = child_nt(grammar, site->node, "call-args");
        collect_positional_nodes(grammar,
            child_nt(grammar, call_args, "positional-args"), &arguments);
        parse_sym_entry_t *flagset_symbol = nullptr;
        bool default_flagset = n00bb_flagset_type(unit, state, target,
                                                  &flagset_symbol)
            && !arguments.len
            && !child_nt(grammar, call_args, "keyword-args");
        core_list_free(arguments);
        if (default_flagset) {
            site->constructor_call = false;
            site->symbol = flagset_symbol;
            parse_tree_set_resolution_symbol(site->node, flagset_symbol);
            core_list_free(candidates);
            return true;
        }
        core_buffer_t message = {0};
        if (constraint_failures == 1)
            core_buffer_printf(&message,
                "constructed type requires trait '%s'%s%s",
                failed_constraint,
                failed_constraint_type ? " for " : "",
                failed_constraint_type
                    ? n00bb_print_type(state->types, failed_constraint_type)
                    : "");
        else
            core_buffer_puts(&message,
                "no visible __init matches this constructed type and its arguments");
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0020"),
            core_string_borrow(message.data, message.byte_len),
            link_source_location(site->node));
        core_buffer_release(&message);
        core_list_free(candidates);
        return false;
    }

    size_t selected = SIZE_MAX;
    for (size_t i = 0; i < candidates.len; i++) {
        bool dominated = false;
        for (size_t j = 0; j < candidates.len; j++) {
            if (i == j) continue;
            bool i_general = th_env_type_pattern_subsumes(
                candidates.data[i].receiver_pattern,
                candidates.data[j].receiver_pattern);
            bool j_general = th_env_type_pattern_subsumes(
                candidates.data[j].receiver_pattern,
                candidates.data[i].receiver_pattern);
            if (i_general && !j_general) {
                dominated = true;
                break;
            }
        }
        if (dominated) continue;
        if (selected != SIZE_MAX) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0021"),
                CORE_STRING_STATIC(
                    "multiple equally specific __init functions match this construction"),
                link_source_location(site->node));
            core_list_free(candidates);
            return false;
        }
        selected = i;
    }
    if (selected == SIZE_MAX) {
        core_list_free(candidates);
        return false;
    }

    parse_sym_entry_t *symbol = candidates.data[selected].symbol;
    core_translation_unit_t *owner = candidates.data[selected].unit;
    core_buffer_t key = {0};
    if (!append_link_symbol_key(grammar, owner, symbol, &key)) {
        core_list_free(candidates);
        return false;
    }
    core_link_reference_t *reference = nullptr;
    bool ok = core_link_unit_reference_at_scoped_kind(link, unit,
        site->node, site->scope, link_symbol_module(owner, symbol),
        core_string_from_cstr(key.data), PARSE_SYM_FUNCTION, &reference);
    core_buffer_release(&key);
    if (ok) {
        core_string_t provenance = core_source_file_path(&unit->source);
        ok = core_link_reference_set_source_info(link, reference,
            link_source_location(site->node), provenance);
    }
    if (!ok) {
        core_list_free(candidates);
        return false;
    }
    core_buffer_t declaration_key = {0};
    if (!append_link_symbol_key(grammar, owner, symbol, &declaration_key)) {
        core_list_free(candidates);
        return false;
    }
    ok = core_link_unit_resolve_reference(link, reference,
        link_symbol_module(owner, symbol),
        core_string_from_cstr(declaration_key.data));
    core_buffer_release(&declaration_key);
    if (ok) {
        site->symbol = symbol;
        parse_sym_add_use(symbol, site->node);
        ok = parse_tree_set_resolution_symbol(site->node, symbol);
    }
    core_list_free(candidates);
    return ok;
}

bool
resolve_inferred_trait_references(parse_grammar_t *grammar,
                                  core_link_unit_t *link,
                                  n00bb_link_analysis_t *analysis,
                                  const th_env *environment)
{
    for (size_t i = 0; i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !th_ctx_set_environment_scope(state->types,
                unit->module.identifier.data,
                n00bb_package_id(unit).data)) return false;
        for (size_t s = 0; s < state->link_use_sites.len; s++) {
            n00bb_link_use_site_t *site = &state->link_use_sites.data[s];
            if (site->callback_call) continue;
            if (!site->symbol && site->member_name.data
                && site->receiver_node
                && n00bb_option_type_element(
                    core_tree_node_value(site->receiver_node).type, nullptr)) {
                const char *name = nullptr;
                if (core_string_eq(site->member_name,
                        CORE_STRING_STATIC("get"))) name = "__option_get";
                else if (core_string_eq(site->member_name,
                        CORE_STRING_STATIC("get_or_else")))
                    name = "__option_get_or_else";
                else if (core_string_eq(site->member_name,
                        CORE_STRING_STATIC("is_nil")))
                    name = "__option_is_nil";
                parse_namespace_t *builtins = name && state->annotations
                    ? parse_symtab_ns(state->annotations->symtab,
                        CORE_STRING_STATIC("builtin")) : nullptr;
                parse_sym_entry_t *builtin = builtins && builtins->root
                    ? parse_symtab_lookup_in(builtins->root,
                        core_string_from_cstr(name)) : nullptr;
                if (n00bb_builtin_option_function(builtin)) {
                    core_link_reference_t *reference =
                        site->reference;
                    core_link_declaration_metadata_t metadata = {
                        .kind = PARSE_SYM_FUNCTION,
                        .visibility = 0,
                        .contract = builtin->callable_contract
                            ? builtin->callable_contract : builtin->type,
                    };
                    core_buffer_t key = {0};
                    core_buffer_printf(&key, "%u:%s",
                        (unsigned)PARSE_SYM_FUNCTION, name);
                    core_string_t key_name = core_string_from_cstr(key.data);
                    core_link_declaration_t *declaration =
                        core_link_unit_find_declaration(link,
                            CORE_STRING_STATIC("builtins"), key_name);
                    bool linked = key.data && reference
                        && (declaration || core_link_unit_require_external(
                            link, CORE_STRING_STATIC("builtins"), key_name,
                            &metadata, &declaration))
                        && core_link_unit_resolve_reference(link, reference,
                            CORE_STRING_STATIC("builtins"), key_name);
                    core_buffer_release(&key);
                    if (!linked) return false;
                    site->symbol = builtin;
                    parse_sym_add_use(builtin, site->node);
                    if (!parse_tree_set_resolution_symbol(site->node,
                            builtin)) return false;
                }
            }
            if (n00bb_builtin_option_function(site->symbol)) continue;
            if (site->operator_dispatch
                && (!refine_intrinsic_binary_result(grammar, unit, state,
                                                     site->node)
                    || !refine_intrinsic_unary_result(grammar, unit, state,
                                                      site->node))) return false;
            if (site->constructor_call) {
                if (!resolve_constructor_use_site(grammar, link, analysis,
                                                  unit, site)) return false;
                continue;
            }
            core_link_reference_t *reference = site->reference;
            if (!reference
                && (site->operator_dispatch || site->semantic_dispatch)) {
                if (site->operator_dispatch
                    && primitive_operator_site(state, site))
                    continue;
                bool deferred = site->semantic_dispatch
                    ? core_link_unit_add_deferred_reference_at_scoped_kind(
                        link, unit, site->node, site->scope,
                        PARSE_SYM_FUNCTION,
                        CORE_STRING_STATIC(
                            "semantic dispatch awaits resolution"),
                        N00BB_LINK_STAGE_SEMANTICS, &reference)
                    : core_link_unit_defer_reference_at_scoped_kind(
                        link, unit, site->node, site->scope,
                        PARSE_SYM_FUNCTION,
                        CORE_STRING_STATIC(
                            "operator dispatch awaits semantic resolution"),
                        N00BB_LINK_STAGE_SEMANTICS, &reference);
                if (!deferred
                    || !core_link_reference_set_source_info(link, reference,
                        link_source_location(site->node),
                        core_source_file_path(&unit->source)))
                    return false;
                if (!n00bb_bind_site_reference(unit, site, reference))
                    return false;
            }
            if (site->callback_setup) {
                const char *trait_name = trait_requirement_name(
                    state->types, site->dispatch_trait,
                    site->dispatch_method);
                th_env_method_t method = {0};
                th_env_method_status_t status = trait_name
                    && site->dispatch_receiver_type
                    ? th_env_method_resolve(state->types,
                        site->dispatch_receiver_type, trait_name,
                        site->dispatch_method.data, &method)
                    : TH_ENV_METHOD_NOT_FOUND;
                if (status == TH_ENV_METHOD_AMBIGUOUS) {
                    size_t count = th_env_method_candidates(state->types,
                        trait_name, site->dispatch_method.data, nullptr, 0);
                    th_env_method_t *candidates = count
                        ? core_alloc_array(th_env_method_t, count) : nullptr;
                    if (candidates)
                        th_env_method_candidates(state->types, trait_name,
                            site->dispatch_method.data, candidates, count);
                    core_buffer_t canonical = {0};
                    bool one_origin = count != 0 && candidates;
                    for (size_t c = 0; one_origin && c < count; c++) {
                        th_type *signature = th_resolve(
                            candidates[c].signature);
                        const th_param *receiver = signature
                            && th_kind_of(signature) == TH_FUNCTION
                            ? th_func_param(signature, 0) : nullptr;
                        th_type *pattern = receiver
                            ? th_type_clone(state->types, receiver->type)
                            : nullptr;
                        th_type *actual = th_type_clone(state->types,
                            site->dispatch_receiver_type);
                        if (!pattern || !actual
                            || !th_unify_in_place(state->types, pattern,
                                                  actual))
                            continue;
                        core_buffer_t identity = {0};
                        bool has_origin = false;
                        for (size_t e = 0; e < analysis->linked_interface_count
                             && !has_origin; e++) {
                            const th_env *archive =
                                analysis->linked_interfaces[e];
                            for (size_t m = 0;
                                 m < th_env_metadata_count(archive); m++) {
                                th_env_metadata_view_t metadata = {0};
                                if (!th_env_metadata_at(archive, m, &metadata)
                                    || metadata.owner_kind !=
                                        TH_ENV_METADATA_FUNCTION
                                    || strcmp(metadata.owner_name,
                                        candidates[c].qualified_name)
                                    || strcmp(metadata.key,
                                              "n00b.export-origin")
                                    || metadata.version != 1
                                    || metadata.size < 12
                                    || memcmp(metadata.data, "N0EO", 4))
                                    continue;
                                uint32_t module_size = read_u32_le(
                                    metadata.data + 4);
                                uint32_t declaration_size = read_u32_le(
                                    metadata.data + 8);
                                if (module_size && declaration_size
                                    && (size_t)module_size + declaration_size
                                        == metadata.size - 12) {
                                    core_buffer_append(&identity,
                                        (const char *)metadata.data + 12
                                            + module_size,
                                        declaration_size);
                                    has_origin = true;
                                }
                                break;
                            }
                        }
                        if (!has_origin)
                            core_buffer_append(&identity,
                                candidates[c].qualified_name,
                                strlen(candidates[c].qualified_name));
                        if (!canonical.byte_len)
                            core_buffer_append(&canonical, identity.data,
                                               identity.byte_len);
                        else if (canonical.byte_len != identity.byte_len
                            || memcmp(canonical.data, identity.data,
                                      identity.byte_len))
                            one_origin = false;
                        if (one_origin
                            && ((!method.qualified_name)
                                || (has_origin
                                    && identity.byte_len == strlen(
                                        candidates[c].qualified_name)
                                    && !memcmp(identity.data,
                                        candidates[c].qualified_name,
                                        identity.byte_len))))
                            method = candidates[c];
                        core_buffer_release(&identity);
                    }
                    core_buffer_release(&canonical);
                    core_free(candidates);
                    if (one_origin && method.qualified_name)
                        status = TH_ENV_METHOD_FOUND;
                }
                parse_sym_entry_t *implementation =
                    status == TH_ENV_METHOD_FOUND
                    ? resolve_linked_method(link, site, unit, &method)
                    : nullptr;
                if (implementation
                    && !symbol_is_public_declaration(implementation))
                    implementation = nullptr;
                if (!implementation) {
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0025"),
                        status == TH_ENV_METHOD_AMBIGUOUS
                            ? CORE_STRING_STATIC(
                                "callback setup is ambiguous for the selected handler type")
                            : status == TH_ENV_METHOD_FOUND
                                ? CORE_STRING_STATIC(
                                    "callback setup implementation must not be private")
                            : CORE_STRING_STATIC(
                                "callback setup has no visible compatible implementation"),
                        link_source_location(site->node));
                    return false;
                }
                site->resolved_trait_name = trait_name;
                continue;
            }
            parse_sym_entry_t *dispatch_symbol = site->symbol;
            if (reference && !core_link_reference_is_deferred(reference)) {
                core_link_declaration_t *declaration =
                    core_link_reference_target(reference);
                n00bb_link_payload_t *payload = declaration
                    ? core_link_declaration_payload(declaration) : nullptr;
                if (payload && payload->symbol)
                    dispatch_symbol = payload->symbol;
            }
            if (!reference)
                continue;
            if (!core_link_reference_is_deferred(reference)) {
                if (!validate_trait_dispatch_result(grammar, unit, state,
                                                    site, dispatch_symbol))
                    return false;
                continue;
            }
            if (site->dispatch_trait.data) {
                parse_tree_t *receiver = site->operator_dispatch
                    ? operator_receiver_node(site)
                    : core_tree_num_children(site->node)
                        ? core_tree_child(site->node, 0) : nullptr;
                th_type *receiver_type = site->dispatch_receiver_type
                    ? site->dispatch_receiver_type
                    : receiver && !core_tree_is_leaf(receiver)
                        ? core_tree_node_value(receiver).type : nullptr;
                const char *trait_name = site->resolved_trait_name
                    ? site->resolved_trait_name
                    : state->types
                        ? trait_requirement_name(state->types,
                            site->dispatch_trait, site->dispatch_method)
                        : nullptr;
                th_env_method_t method = {0};
                th_env_method_status_t status = trait_name && receiver_type
                    ? th_env_method_resolve(state->types, receiver_type,
                        trait_name, site->dispatch_method.data, &method)
                    : TH_ENV_METHOD_NOT_FOUND;
                if (!trait_name && receiver_type) {
                    const char *selected_trait = nullptr;
                    status = th_env_method_resolve_by_name(state->types,
                        receiver_type, site->dispatch_method.data, &method,
                        &selected_trait);
                    if (status == TH_ENV_METHOD_FOUND && selected_trait
                        && trait_name_matches(selected_trait,
                                              site->dispatch_trait))
                        trait_name = selected_trait;
                    else status = TH_ENV_METHOD_NOT_FOUND;
                }
                if (status == TH_ENV_METHOD_NOT_FOUND
                    && core_string_eq(site->dispatch_trait,
                                      CORE_STRING_STATIC("Equal"))) {
                    const char *fallback = trait_requirement_name(
                        state->types, CORE_STRING_STATIC("Comparable"),
                        CORE_STRING_STATIC("__cmp"));
                    if (fallback && receiver_type) {
                        status = th_env_method_resolve(state->types,
                            receiver_type, fallback, "__cmp", &method);
                        if (status == TH_ENV_METHOD_FOUND) {
                            site->dispatch_trait = CORE_STRING_STATIC("Comparable");
                            site->dispatch_method = CORE_STRING_STATIC("__cmp");
                            trait_name = fallback;
                        }
                    }
                }
                site->resolved_trait_name = trait_name;
                parse_sym_entry_t *implementation =
                    status == TH_ENV_METHOD_FOUND
                        ? resolve_linked_method(link, site, unit, &method)
                        : nullptr;
                if (implementation) {
                    site->symbol = implementation;
                    parse_sym_add_use(implementation, site->node);
                    if (!site->secondary_dispatch
                        && !parse_tree_set_resolution_symbol(site->node,
                                                             implementation))
                        return false;
                }
                else if (trait_name && receiver_type
                         && (has_trait_constraint(receiver_type, trait_name)
                             || has_deferred_method_constraint(
                                 receiver_type, site->dispatch_method))) {
                    parse_sym_entry_t *requirement =
                        trait_method_requirement_symbol(grammar, analysis,
                            unit, core_string_from_cstr(trait_name),
                            site->dispatch_method);
                    if (!requirement) {
                        core_diagnostics_report_at(
                            core_translation_unit_diagnostics(unit), unit,
                            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0011"),
                            CORE_STRING_STATIC(
                                "generic trait constraint has no matching method declaration"),
                            link_source_location(site->node));
                        return false;
                    }
                    if (!bind_deferred_method_failure(unit, state,
                            site->node, receiver_type,
                            site->dispatch_method, requirement)) return false;
                    site->symbol = requirement;
                    parse_sym_add_use(requirement, site->node);
                    if (!site->secondary_dispatch
                        && !parse_tree_set_resolution_symbol(site->node,
                                                             requirement))
                        return false;
                    if (!validate_trait_dispatch_result(grammar, unit, state,
                                                        site, requirement))
                        return false;
                    if (has_trait_constraint(receiver_type, trait_name)
                        && !plan_closed_trait_dispatch(link, analysis,
                            environment, unit, site, trait_name,
                            site->dispatch_method.data)) return false;
                    if (!core_link_unit_defer_reference(link, reference,
                            CORE_STRING_STATIC(
                                "generic trait dispatch awaits specialization"),
                            N00BB_LINK_STAGE_IR_BINDINGS)) return false;
                    continue;
                }
                else {
                    core_buffer_t message = {0};
                    core_buffer_printf(&message,
                        "operator or indexing operation has no linked %s.%.*s implementation for %s",
                        trait_name ? trait_name : "<unknown trait>",
                        (int)site->dispatch_method.u8_bytes,
                        site->dispatch_method.data
                            ? site->dispatch_method.data : "",
                        receiver_type ? th_print(state->types, receiver_type)
                            : "<unknown receiver>");
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0011"),
                        core_string_borrow(message.data, message.byte_len),
                        link_source_location(site->node));
                    core_buffer_release(&message);
                    return false;
                }
                if (!validate_trait_dispatch_result(grammar, unit, state,
                                                    site, implementation))
                    return false;
            }
            parse_node_resolution_t resolution =
                parse_tree_resolution(site->node);
            parse_sym_entry_t *symbol = site->symbol ? site->symbol :
                resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
                    ? resolution.target.symbol
                    : core_tree_node_value(site->node).sym;
            if (n00bb_builtin_string_operator(symbol)
                || n00bb_builtin_array_read_function(symbol)
                || n00bb_builtin_array_write_function(symbol)
                || (symbol && parse_sym_binding(symbol,
                    CORE_STRING_STATIC("n00bb.builtin-operator")))) continue;
            core_translation_unit_t *owner = symbol
                ? unit_for_symbol(analysis->grammar, &analysis->units, symbol)
                : nullptr;
            if (!symbol || symbol->kind != PARSE_SYM_FUNCTION || !owner) {
                parse_tree_t *receiver = core_tree_num_children(site->node)
                    ? core_tree_child(site->node, 0) : nullptr;
                th_type *receiver_type = receiver && !core_tree_is_leaf(receiver)
                    ? core_tree_node_value(receiver).type : nullptr;
                core_string_t method_name = core_tree_num_children(site->node) >= 3
                    ? first_token_text(core_tree_child(site->node, 2))
                    : (core_string_t){0};
                if (method_name.data && state->types) {
                    parse_sym_entry_t *requirement =
                        deferred_trait_method_requirement(grammar, analysis,
                            unit, receiver_type, method_name);
                    if (!requirement) {
                        core_diagnostics_report_at(
                            core_translation_unit_diagnostics(unit), unit,
                            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0010"),
                            CORE_STRING_STATIC(
                                "generic trait constraint has no matching method declaration"),
                            link_source_location(site->node));
                        return false;
                    }
                    if (!bind_deferred_method_failure(unit, state,
                            site->node, receiver_type, method_name,
                            requirement)) return false;
                    site->symbol = requirement;
                    parse_sym_add_use(requirement, site->node);
                    if (!parse_tree_set_resolution_symbol(site->node,
                                                           requirement))
                        return false;
                    const char *trait_name = requirement->owner_type
                        && requirement->owner_type->name.data
                        ? trait_requirement_name(state->types,
                            requirement->owner_type->name, method_name)
                        : nullptr;
                    site->resolved_trait_name = trait_name;
                    if (trait_name
                        && (has_trait_constraint(receiver_type, trait_name)
                            || has_deferred_method_constraint(receiver_type,
                                                              method_name))
                        && !plan_closed_trait_dispatch(link, analysis,
                            environment, unit, site, trait_name,
                            method_name.data)) return false;
                    if (!core_link_unit_defer_reference(link, reference,
                            CORE_STRING_STATIC(
                                "generic trait dispatch awaits specialization"),
                            N00BB_LINK_STAGE_IR_BINDINGS)) return false;
                    continue;
                }
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0010"),
                    CORE_STRING_STATIC(
                        "trait dispatch did not resolve to a linked function declaration"),
                    link_source_location(site->node));
                return false;
            }
            core_buffer_t key = {0};
            if (!append_link_symbol_key(analysis->grammar, owner, symbol,
                                        &key)) return false;
            bool resolved = core_link_unit_resolve_reference(link, reference,
                link_symbol_module(owner, symbol),
                core_string_from_cstr(key.data));
            core_buffer_release(&key);
            if (!resolved) return false;
            site->symbol = symbol;
        }
    }
    return core_link_unit_validate(link)
        && bind_link_references_to_nodes(link);
}


core_link_declaration_t *
register_builtin_method_declaration(core_link_unit_t *link,
    n00bb_link_analysis_t *analysis, core_translation_unit_t *unit,
    const char *trait_name, const th_env_method_t *method)
{
    if (!method || !method->module || strcmp(method->module, "builtins")
        || !method->qualified_name || !method->contract) return nullptr;
    n00bb_tu_state_t *state = tu_state(unit);
    parse_symtab_t *symtab = state && state->annotations
        ? state->annotations->symtab : nullptr;
    parse_sym_entry_t *requirement = strchr(trait_name, '.')
        ? trait_method_requirement_symbol(analysis->grammar, analysis, unit,
            core_string_from_cstr(trait_name),
            core_string_from_cstr(method->root_name))
        : builtin_trait_requirement_symbol(unit,
            core_string_from_cstr(trait_name),
            core_string_from_cstr(method->root_name));
    parse_namespace_t *builtins = symtab
        ? parse_symtab_ns(symtab, CORE_STRING_STATIC("builtin")) : nullptr;
    if (!builtins || !requirement) return nullptr;
    parse_scope_t *saved = builtins->current;
    parse_symtab_enter_scope(symtab, CORE_STRING_STATIC("builtin"),
                             builtins->root);
    parse_sym_entry_t *symbol = parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"),
        core_string_from_cstr(method->qualified_name),
        PARSE_SYM_FUNCTION, nullptr);
    parse_symtab_enter_scope(symtab, CORE_STRING_STATIC("builtin"), saved);
    if (!symbol) return nullptr;
    symbol->type = symbol->callable_contract = method->contract;
    symbol->owner_type = requirement->owner_type;
    symbol->visibility = CORE_STRING_STATIC("public");
    if (!parse_sym_set_binding(symbol,
            CORE_STRING_STATIC("n00bb.builtin-operator"), requirement))
        return nullptr;
    parse_sym_set_phases(symbol, PARSE_SYM_PHASE_BOTH,
                         PARSE_SYM_PHASE_NONE, PARSE_SYM_PHASE_NONE);
    core_link_declaration_metadata_t metadata = {
        .kind = PARSE_SYM_FUNCTION,
        .visibility = TH_ENV_PUBLIC,
        .provenance = CORE_STRING_STATIC("builtins"),
        .provenance_version = 1,
        .contract = method->contract,
        .compatible = n00bb_link_contract_compatible,
        .compatibility_context = state->types,
    };
    core_buffer_t key = {0};
    size_t module_length = strlen(method->module);
    const char *identity = !strncmp(method->qualified_name, method->module,
                                    module_length)
        && method->qualified_name[module_length] == '.'
        ? method->qualified_name + module_length + 1 : nullptr;
    if (!identity) return nullptr;
    core_buffer_printf(&key, "%u:%s", (unsigned)PARSE_SYM_FUNCTION,
                       identity);
    core_link_declaration_t *declaration = nullptr;
    n00bb_link_payload_t *payload = core_alloc(n00bb_link_payload_t);
    if (payload) *payload = (n00bb_link_payload_t){.symbol = symbol};
    bool registered = key.data && payload && core_link_unit_define_external_typed(
        link, CORE_STRING_STATIC("builtins"), core_string_from_cstr(key.data),
        &metadata, payload, CORE_LINK_PAYLOAD_OWNED,
        free_n00bb_link_payload, &declaration);
    if (!registered) core_free(payload);
    core_buffer_release(&key);
    if (!registered || !core_link_declaration_set_binding(link, declaration,
            CORE_STRING_STATIC("n00bb.builtin-symbol"),
            N00BB_LINK_STAGE_SEMANTICS, symbol)) return nullptr;
    return core_dict_add(&analysis->declarations_by_symbol, symbol, declaration)
        ? declaration : nullptr;
}

static parse_tree_t *
enclosing_call_expression(parse_grammar_t *grammar, parse_tree_t *node)
{
    for (parse_tree_t *at = node; at;
         at = core_tree_node_value(at).parent) {
        const parse_rule_t *rule = parse_tree_rule(grammar, at);
        if (rule && parse_rule_find_annot(grammar, rule, PARSE_ANNOT_CALL))
            return at;
    }
    return nullptr;
}

static parse_cf_label_t *
call_label_for_expression(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                          core_translation_unit_t *unit, parse_tree_t *call,
                          parse_sym_entry_t *callee)
{
    if (!grammar || !state || !state->annotations || !unit || !call
        || !callee)
        return nullptr;
    core_dict_foreach(state->annotations->cf_labels, item) {
        parse_cf_label_t *label = item->value;
        if (!label || label->kind != PARSE_CF_CALL || !label->cond
            || enclosing_call_expression(grammar, label->cond) != call
            || bound_function_symbol(label->cond)
                != callee) continue;
        return label;
    }
    return nullptr;
}

static bool
add_trait_specialization_fact(core_link_unit_t *link,
    n00bb_link_analysis_t *analysis, uint32_t producer_stage,
    core_translation_unit_t *call_unit, parse_tree_t *call_node,
    parse_tree_t *trait_node, parse_sym_entry_t *generic_function,
    th_type *receiver_type, core_link_declaration_t *target,
    core_link_dependency_state_t state,
    n00bb_trait_specialization_t **out)
{
    if (out) *out = nullptr;
    core_link_declaration_t *from = link_declaration_for_symbol(
        analysis, generic_function);
    n00bb_trait_specialization_t *fact = core_alloc(
        n00bb_trait_specialization_t);
    if (!from || !fact) {
        core_free(fact);
        return false;
    }
    *fact = (n00bb_trait_specialization_t){
        .instantiation_site = call_node,
        .trait_site = member_access_node(analysis->grammar, trait_node),
        .generic_function = generic_function,
        .receiver_type = receiver_type,
    };
    if (!fact->trait_site) fact->trait_site = trait_node;
    if (!core_link_unit_add_dependency(link,
            N00BB_LINK_DEPENDENCY_TRAIT_SPECIALIZATION, producer_stage,
            state, from, target, call_unit, fact,
            link_source_location(call_node),
            core_source_file_path(&call_unit->source), 1, nullptr)) {
        core_free(fact);
        return false;
    }
    core_list_push(analysis->trait_specializations, fact);
    if (out) *out = fact;
    return true;
}

static bool
diagnose_ambiguous_generic_trait_call(parse_grammar_t *grammar,
    core_translation_unit_t *call_unit, parse_tree_t *call_node,
    core_translation_unit_t *function_unit,
    n00bb_tu_state_t *function_state,
    parse_sym_entry_t *generic_function,
    core_list_t(parse_sym_entry_ptr_t) *formals, th_ctx *types)
{
    if (!grammar || !call_unit || !call_node || !function_unit
        || !function_state || !types)
        return false;
    for (size_t t = 0; t < function_state->link_use_sites.len; t++) {
        n00bb_link_use_site_t *site = &function_state->link_use_sites.data[t];
        parse_sym_entry_t *requirement = site->symbol;
        if (site->function_owner != generic_function || !requirement
            || requirement->kind != PARSE_SYM_FUNCTION
            || !requirement->owner_type || !requirement->owner_type->type)
            continue;
        parse_tree_t *receiver = site->operator_dispatch
            ? operator_receiver_node(site)
            : core_tree_num_children(site->node)
                ? core_tree_child(site->node, 0) : nullptr;
        parse_sym_entry_t *receiver_symbol =
            simple_expression_bound_symbol(receiver);
        size_t parameter_index = 0;
        while (parameter_index < formals->len
               && formals->data[parameter_index] != receiver_symbol)
            parameter_index++;
        if (!receiver_symbol || parameter_index == formals->len) continue;
        parse_tree_t *argument = call_argument_for_parameter(grammar,
            call_node, generic_function, parameter_index, false);
        th_type *argument_type = argument && !core_tree_is_leaf(argument)
            ? core_tree_node_value(argument).type : nullptr;
        if (!argument_type || !th_is_closed(types, argument_type)) continue;

        const char *trait_name = nullptr;
        size_t trait_name_length = 0;
        if (!(th_trait_name(requirement->owner_type->type, &trait_name,
                            &trait_name_length)
              || th_named_info(requirement->owner_type->type, &trait_name,
                               &trait_name_length, nullptr, nullptr))
            || !trait_name || !trait_name_length) continue;
        if (!th_ctx_set_environment_scope(types,
                call_unit->module.identifier.data,
                n00bb_package_id(call_unit).data)) continue;
        th_env_method_t method = {0};
        th_env_method_status_t status = th_env_method_resolve(types,
            argument_type, trait_name, requirement->name.data,
            &method);
        if (status != TH_ENV_METHOD_AMBIGUOUS) continue;
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(call_unit), call_unit,
            CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0003"),
            CORE_STRING_STATIC(
                "generic instantiation has ambiguous trait dispatch"),
            link_source_location(call_node));
        return true;
    }
    return false;
}

bool
resolve_generic_trait_specializations(core_link_unit_t *link,
    n00bb_link_analysis_t *analysis, uint32_t producer_stage)
{
    core_dict_t seen_calls;
    core_dict_init(&seen_calls, nullptr, nullptr);
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *call_unit = analysis->units.data[u];
        n00bb_tu_state_t *call_state = tu_state(call_unit);
        if (!call_state) {
            core_dict_free(&seen_calls);
            return false;
        }
        for (size_t s = 0; s < call_state->link_use_sites.len; s++) {
            n00bb_link_use_site_t *call_use =
                &call_state->link_use_sites.data[s];
            parse_sym_entry_t *generic_function = call_use->symbol;
            th_type *generic_contract = generic_function
                ? (generic_function->callable_contract
                    ? generic_function->callable_contract
                    : generic_function->type)
                : nullptr;
            if (!generic_function
                || generic_function->kind != PARSE_SYM_FUNCTION
                || !generic_contract
                || th_is_closed(call_state->types, generic_contract))
                continue;
            parse_tree_t *call_node = enclosing_call_expression(
                analysis->grammar, call_use->node);
            if (!call_node || core_dict_contains(&seen_calls, call_node))
                continue;
            parse_tree_t *callee = core_tree_num_children(call_node)
                ? core_tree_child(call_node, 0) : nullptr;
            if (bound_function_symbol(callee)
                != generic_function)
                continue;
            core_dict_put(&seen_calls, call_node, call_node);

            core_translation_unit_t *function_unit = unit_for_symbol(
                analysis->grammar, &analysis->units, generic_function);
            if (!function_unit) continue;
            n00bb_tu_state_t *function_state = tu_state(function_unit);
            if (!generic_function->decl_node
                || !child_nt(analysis->grammar, generic_function->decl_node,
                             "body")) continue;
            parse_tree_t *signature = child_nt(analysis->grammar,
                generic_function->decl_node, "func-type-signature");
            parse_tree_t *param_decl = child_nt(analysis->grammar,
                signature, "param-decl");
            core_list_t(parse_sym_entry_ptr_t) formals =
                core_list_new(parse_sym_entry_ptr_t);
            parse_sym_entry_t *rest = nullptr;
            collect_ir_parameters(analysis->grammar, param_decl,
                                  &formals, &rest);
                if (!function_state || !function_state->types) {
                    core_list_free(formals);
                    core_dict_free(&seen_calls);
                    return false;
                }
                if (function_state->types != call_state->types) {
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(call_unit),
                        call_unit, CORE_DIAG_FATAL,
                        CORE_STRING_STATIC("N00BB-T0004"),
                        CORE_STRING_STATIC(
                            "trait specialization crossed type universes"),
                        link_source_location(call_node));
                    core_list_free(formals);
                    core_dict_free(&seen_calls);
                    return false;
                }

            for (size_t t = 0; t < function_state->link_use_sites.len; t++) {
                n00bb_link_use_site_t *trait_use =
                    &function_state->link_use_sites.data[t];
                parse_sym_entry_t *requirement = trait_use->symbol;
                if (trait_use->function_owner != generic_function
                    || !requirement
                    || requirement->kind != PARSE_SYM_FUNCTION
                    || !requirement->owner_type
                    || !requirement->owner_type->type) continue;
                parse_tree_t *receiver = trait_use->operator_dispatch
                    ? operator_receiver_node(trait_use)
                    : core_tree_num_children(trait_use->node)
                        ? core_tree_child(trait_use->node, 0) : nullptr;
                th_type *receiver_pattern = receiver
                    ? core_tree_node_value(receiver).type : nullptr;
                if (!receiver_pattern && receiver) {
                    parse_sym_entry_t *receiver_symbol =
                        simple_expression_bound_symbol(receiver);
                    receiver_pattern = receiver_symbol
                        ? receiver_symbol->type : nullptr;
                }
                if (!receiver_pattern) continue;

                parse_cf_label_t *trait_call = call_label_for_member(
                    analysis->grammar, function_state, trait_use->node);
                parse_tree_t *trait_call_node = enclosing_call_expression(
                    analysis->grammar, trait_use->node);
                th_type *function_contract = generic_function->callable_contract
                    ? generic_function->callable_contract
                    : generic_function->type;
                th_type *function_type = nullptr;
                th_type *function_failure = nullptr;
                if (!th_env_signature_channels(call_state->types,
                        function_contract, &function_type,
                        &function_failure)) {
                    core_list_free(formals);
                    core_dict_free(&seen_calls);
                    return false;
                }
                size_t signature_parameter_count =
                    th_func_nparams(function_type);
                th_type *function_result = th_func_ret(function_type);
                th_type *trait_result = trait_use->operator_dispatch
                    ? core_tree_node_value(trait_use->node).type
                    : trait_call && trait_call->self
                    && !core_tree_is_leaf(trait_call->self)
                    ? core_tree_node_value(trait_call->self).type : nullptr;
                if (trait_use->operator_dispatch
                    && core_string_eq(trait_use->dispatch_trait,
                                      CORE_STRING_STATIC("Comparable")))
                    trait_result = th_name(call_state->types, "Comparison");
                core_list_t(th_type_ptr_t) return_expression_types =
                    core_list_new(th_type_ptr_t);
                core_dict_foreach(function_state->annotations->cf_labels,
                                  exit_item) {
                    parse_cf_label_t *exit = exit_item->value;
                    if (!exit || exit->kind != PARSE_CF_JUMP
                        || !core_string_eq(exit->jump_kind,
                                           CORE_STRING_STATIC("return"))
                        || enclosing_function_symbol(analysis->grammar,
                            exit->self) != generic_function) continue;
                    parse_tree_t *expression = child_nt(analysis->grammar,
                        exit->self, "expression");
                    th_type *return_type = expression
                        && !core_tree_is_leaf(expression)
                        ? core_tree_node_value(expression).type : nullptr;
                    if (return_type)
                        core_list_push(return_expression_types, return_type);
                }
                th_type *requirement_type = requirement->callable_contract
                    ? requirement->callable_contract : requirement->type;
                size_t requirement_params = requirement_type
                    ? th_func_nparams(requirement_type) : 0;
                core_list_t(size_t) trait_argument_positions =
                    core_list_new(size_t);
                core_list_t(th_type_ptr_t) trait_argument_types =
                    core_list_new(th_type_ptr_t);
                for (size_t p = 1; (trait_call_node || trait_use->operator_dispatch)
                     && p < requirement_params; p++) {
                    parse_tree_t *argument = trait_use->operator_dispatch
                        ? core_tree_num_children(trait_use->node) == 3 && p == 1
                            ? core_tree_child(trait_use->node, 2) : nullptr
                        : call_argument_for_parameter(
                        analysis->grammar, trait_call_node, requirement, p,
                        true);
                    th_type *argument_type = argument
                        && !core_tree_is_leaf(argument)
                        ? core_tree_node_value(argument).type : nullptr;
                    if (!argument_type) continue;
                    core_list_push(trait_argument_positions, p);
                    core_list_push(trait_argument_types, argument_type);
                }
                size_t member_count = formals.len + 4
                    + signature_parameter_count
                    + trait_argument_types.len
                    + return_expression_types.len;
                th_member *members = core_alloc_array(th_member,
                                                       member_count);
                char *names = core_alloc_size(member_count * 32, 1);
                if (!members || !names) {
                    core_free(members);
                    core_free(names);
                    core_list_free(trait_argument_positions);
                    core_list_free(trait_argument_types);
                    core_list_free(return_expression_types);
                    core_list_free(formals);
                    core_dict_free(&seen_calls);
                    return false;
                }
                bool valid = true;
                for (size_t p = 0; p < formals.len; p++) {
                    if (!formals.data[p] || !formals.data[p]->type) {
                        valid = false;
                        break;
                    }
                    char *name = names + p * 32;
                    snprintf(name, 32, "parameter_%zu", p);
                    members[p] = (th_member){name,
                                             formals.data[p]->type};
                }
                snprintf(names + formals.len * 32, 32, "receiver");
                members[formals.len] = (th_member){
                    names + formals.len * 32, receiver_pattern,
                };
                size_t function_result_index = formals.len + 1;
                size_t trait_result_index = formals.len + 2;
                size_t function_failure_index = formals.len + 3;
                snprintf(names + function_result_index * 32, 32,
                         "function_result");
                snprintf(names + trait_result_index * 32, 32,
                         "trait_result");
                snprintf(names + function_failure_index * 32, 32,
                         "function_failure");
                members[function_result_index] = (th_member){
                    names + function_result_index * 32, function_result,
                };
                members[trait_result_index] = (th_member){
                    names + trait_result_index * 32,
                    trait_result ? trait_result : function_result,
                };
                members[function_failure_index] = (th_member){
                    names + function_failure_index * 32,
                    function_failure,
                };
                for (size_t p = 0; p < signature_parameter_count; p++) {
                    size_t index = formals.len + 4 + p;
                    snprintf(names + index * 32, 32,
                             "signature_parameter_%zu", p);
                    const th_param *parameter = th_func_param(function_type,
                                                               p);
                    if (!parameter) {
                        valid = false;
                        break;
                    }
                    members[index] = (th_member){
                        names + index * 32, parameter->type,
                    };
                }
                for (size_t a = 0; a < trait_argument_types.len; a++) {
                    size_t index = formals.len + 4
                        + signature_parameter_count + a;
                    size_t parameter = trait_argument_positions.data[a];
                    snprintf(names + index * 32, 32, "trait_arg_%zu",
                             parameter);
                    members[index] = (th_member){
                        names + index * 32, trait_argument_types.data[a],
                    };
                }
                for (size_t r = 0; r < return_expression_types.len; r++) {
                    size_t index = formals.len + 4
                        + signature_parameter_count
                        + trait_argument_types.len + r;
                    snprintf(names + index * 32, 32, "return_expr_%zu", r);
                    members[index] = (th_member){
                        names + index * 32, return_expression_types.data[r],
                    };
                }
                th_ctx *types = call_state->types;
                th_type *pattern = valid
                    ? th_record(types, members, member_count) : nullptr;
                th_type *copy = pattern
                    ? th_type_clone(types, pattern) : nullptr;
                core_free(members);
                core_free(names);
                if (!copy) {
                    core_list_free(trait_argument_positions);
                    core_list_free(trait_argument_types);
                    core_list_free(return_expression_types);
                    core_list_free(formals);
                    core_dict_free(&seen_calls);
                    return false;
                }

                bool signature_matches =
                    signature_parameter_count == formals.len;
                for (size_t p = 0; signature_matches
                     && p < signature_parameter_count; p++) {
                    char signature_field[40], parameter_field[32];
                    snprintf(signature_field, sizeof(signature_field),
                             "signature_parameter_%zu", p);
                    snprintf(parameter_field, sizeof(parameter_field),
                             "parameter_%zu", p);
                    th_type *signature_parameter = th_record_lookup(
                        copy, signature_field);
                    th_type *body_parameter = th_record_lookup(copy,
                                                                 parameter_field);
                    signature_matches = signature_parameter
                        && body_parameter
                        && th_unify_in_place(types, signature_parameter,
                                             body_parameter);
                }
                if (!signature_matches) {
                    core_list_free(trait_argument_positions);
                    core_list_free(trait_argument_types);
                    core_list_free(return_expression_types);
                    core_list_free(formals);
                    core_dict_free(&seen_calls);
                    return false;
                }

                bool arguments_valid = true;
                size_t failed_argument_index = SIZE_MAX;
                th_type *failed_parameter_type = nullptr;
                th_type *failed_argument_type = nullptr;
                const char *failed_unification_reason = nullptr;
                for (size_t p = 0; p < formals.len; p++) {
                    parse_tree_t *argument = call_argument_for_parameter(
                        analysis->grammar, call_node, generic_function, p,
                        false);
                    if (!argument && formals.data[p]->is_optional)
                        argument = child_nt(analysis->grammar,
                            formals.data[p]->decl_node, "expression");
                    th_type *argument_type = argument
                        ? core_tree_node_value(argument).type : nullptr;
                    if (!argument_type) continue;
                    char field[32];
                    snprintf(field, sizeof(field), "parameter_%zu", p);
                    th_type *parameter = th_record_lookup(copy, field);
                    size_t errors_before = th_error_count(types);
                    bool matched = parameter
                        && th_unify_in_place(types, parameter,
                                             argument_type);
                    if (!matched) {
                        arguments_valid = false;
                        failed_argument_index = p;
                        failed_parameter_type = parameter;
                        failed_argument_type = argument_type;
                        th_error_t error = {0};
                        if (th_error_count(types) > errors_before
                            && th_error_at(types, th_error_count(types) - 1,
                                           &error))
                            failed_unification_reason = error.message;
                        break;
                    }
                }
                if (!arguments_valid) {
                    core_list_free(trait_argument_positions);
                    core_list_free(trait_argument_types);
                    core_list_free(return_expression_types);
                    if (diagnose_ambiguous_generic_trait_call(
                            analysis->grammar, call_unit, call_node,
                            function_unit, function_state, generic_function,
                            &formals, types)) {
                        core_list_free(formals);
                        core_dict_free(&seen_calls);
                        return false;
                    }
                    core_buffer_t message = {0};
                    if (failed_argument_index != SIZE_MAX
                        && failed_parameter_type && failed_argument_type)
                        core_buffer_printf(&message,
                            "generic call argument %zu has type '%s', which does not satisfy parameter type '%s'%s%s",
                            failed_argument_index + 1,
                            n00bb_print_type(types, failed_argument_type),
                            n00bb_print_type(types, failed_parameter_type),
                            failed_unification_reason ? ": " : "",
                            failed_unification_reason
                                ? failed_unification_reason : "");
                    else
                        core_buffer_printf(&message,
                            "generic call arguments do not satisfy the function's type contract");
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(call_unit),
                        call_unit, CORE_DIAG_FATAL,
                        CORE_STRING_STATIC("N00BB-T0003"),
                        core_string_from_cstr(message.data),
                        link_source_location(call_node));
                    core_buffer_release(&message);
                    core_list_free(formals);
                    core_dict_free(&seen_calls);
                    return false;
                }
                th_type *copied_function_result =
                    th_record_lookup(copy, "function_result");
                bool returns_match = copied_function_result != nullptr;
                for (size_t r = 0; returns_match
                     && r < return_expression_types.len; r++) {
                    char field[32];
                    snprintf(field, sizeof(field), "return_expr_%zu", r);
                    th_type *return_type = th_record_lookup(copy, field);
                    th_type *element = nullptr;
                    bool optional_result = n00bb_option_type_element(
                        copied_function_result, &element);
                    th_type *returned_element = nullptr;
                    bool returned_option = return_type
                        && n00bb_option_type_element(return_type,
                                                     &returned_element);
                    returns_match = return_type && (optional_result
                        ? n00bb_nil_type(types, return_type)
                            || (returned_option
                                ? th_unify_in_place(types,
                                    copied_function_result, return_type)
                                    != nullptr
                                : th_unify_in_place(types, element,
                                                    return_type) != nullptr)
                        : th_unify_in_place(types, copied_function_result,
                                            return_type) != nullptr);
                }
                core_list_free(return_expression_types);
                if (!returns_match) {
                    core_list_free(trait_argument_positions);
                    core_list_free(trait_argument_types);
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(call_unit),
                        call_unit, CORE_DIAG_ERROR,
                        CORE_STRING_STATIC("N00BB-T0001"),
                        CORE_STRING_STATIC(
                            "specialized generic return values do not match the function result type"),
                        link_source_location(call_node));
                    continue;
                }
                const char *trait_name = nullptr;
                size_t trait_name_length = 0;
                if (!(th_trait_name(requirement->owner_type->type,
                                    &trait_name, &trait_name_length)
                      || th_named_info(requirement->owner_type->type,
                                       &trait_name, &trait_name_length,
                                       nullptr, nullptr))
                    || !trait_name_length)
                    trait_name = nullptr;
                th_type *receiver_type = th_resolve(
                    th_record_lookup(copy, "receiver"));
                if (!trait_name || !receiver_type) {
                    core_list_free(trait_argument_positions);
                    core_list_free(trait_argument_types);
                    core_list_free(formals);
                    core_dict_free(&seen_calls);
                    return false;
                }
                if (rest || !th_is_closed(types, receiver_type)) {
                    if (!add_trait_specialization_fact(link, analysis,
                            producer_stage, call_unit, call_node,
                            trait_use->node, generic_function,
                            receiver_type, nullptr,
                            CORE_LINK_DEPENDENCY_DEFERRED, nullptr)) {
                        core_list_free(trait_argument_positions);
                        core_list_free(trait_argument_types);
                        core_list_free(formals);
                        core_dict_free(&seen_calls);
                        return false;
                    }
                    core_list_free(trait_argument_positions);
                    core_list_free(trait_argument_types);
                    continue;
                }
                if (!th_ctx_set_environment_scope(types,
                        call_unit->module.identifier.data,
                        n00bb_package_id(call_unit).data)) {
                    core_list_free(trait_argument_positions);
                    core_list_free(trait_argument_types);
                    core_list_free(formals);
                    core_dict_free(&seen_calls);
                    return false;
                }
                th_env_method_t method = {0};
                th_env_method_status_t status = th_env_method_resolve(
                    types, receiver_type, trait_name,
                    requirement->name.data, &method);
                if (status != TH_ENV_METHOD_FOUND) {
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(call_unit),
                        call_unit, CORE_DIAG_FATAL,
                        CORE_STRING_STATIC("N00BB-T0003"),
                        status == TH_ENV_METHOD_AMBIGUOUS
                            ? CORE_STRING_STATIC(
                                "generic instantiation has ambiguous trait dispatch")
                            : CORE_STRING_STATIC(
                                "generic instantiation has no matching trait implementation"),
                        link_source_location(call_node));
                    core_list_free(trait_argument_positions);
                    core_list_free(trait_argument_types);
                    core_list_free(formals);
                    core_dict_free(&seen_calls);
                    return false;
                }
                core_link_declaration_t *target =
                    linked_method_declaration(link, &method);
                if (!target)
                    target = register_builtin_method_declaration(link,
                        analysis, function_unit, trait_name, &method);
                n00bb_link_payload_t *target_payload = target
                    ? core_link_declaration_payload(target) : nullptr;
                parse_sym_entry_t *target_symbol = target_payload
                    ? target_payload->symbol : target
                        ? core_link_declaration_binding(target,
                            CORE_STRING_STATIC("n00bb.builtin-symbol"))
                        : nullptr;
                th_type *target_contract = target_symbol
                    ? (target_symbol->callable_contract
                        ? target_symbol->callable_contract
                        : target_symbol->type)
                    : nullptr;
                th_type *target_signature = nullptr;
                th_type *target_failure = nullptr;
                if (!target_symbol
                    || !th_env_signature_channels(types, target_contract,
                        &target_signature, &target_failure)) {
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(call_unit),
                        call_unit, CORE_DIAG_FATAL,
                        CORE_STRING_STATIC("N00BB-L0047"),
                        CORE_STRING_STATIC(
                            "resolved generic trait target has no callable symbol signature"),
                        link_source_location(call_node));
                    core_list_free(trait_argument_positions);
                    core_list_free(trait_argument_types);
                    core_list_free(formals);
                    core_dict_free(&seen_calls);
                    return false;
                }
                n00bb_trait_specialization_t *specialization = nullptr;
                if (!target || !add_trait_specialization_fact(link, analysis,
                        producer_stage, call_unit, call_node, trait_use->node,
                        generic_function, receiver_type, target,
                        CORE_LINK_DEPENDENCY_RESOLVED, &specialization)) {
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(call_unit),
                        call_unit, CORE_DIAG_FATAL,
                        CORE_STRING_STATIC("N00BB-L0047"),
                        CORE_STRING_STATIC(
                            "resolved generic trait implementation has no canonical LinkUnit declaration"),
                        link_source_location(call_node));
                    core_list_free(trait_argument_positions);
                    core_list_free(trait_argument_types);
                    core_list_free(formals);
                    core_dict_free(&seen_calls);
                    return false;
                }
                const th_param *target_receiver =
                    th_func_param(target_signature, 0);
                th_type *copied_receiver = th_record_lookup(copy, "receiver");
                th_type *copied_trait_result =
                    th_record_lookup(copy, "trait_result");
                th_type *copied_function_failure =
                    th_record_lookup(copy, "function_failure");
                if (!target_receiver) {
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(call_unit),
                        call_unit, CORE_DIAG_FATAL,
                        CORE_STRING_STATIC("N00BB-L0047"),
                        CORE_STRING_STATIC(
                            "resolved generic trait target has no receiver parameter"),
                        link_source_location(call_node));
                    core_list_free(trait_argument_positions);
                    core_list_free(trait_argument_types);
                    core_list_free(formals);
                    core_dict_free(&seen_calls);
                    return false;
                }
                size_t target_parameter_count = th_func_nparams(target_signature);
                th_param *target_parameters = target_parameter_count
                    ? core_alloc_array(th_param, target_parameter_count)
                    : nullptr;
                bool target_matches = target_parameters != nullptr;
                for (size_t p = 0; target_matches
                     && p < target_parameter_count; p++) {
                    const th_param *parameter = th_func_param(target_signature, p);
                    target_parameters[p] = *parameter;
                    target_parameters[p].type = p == 0 ? copied_receiver
                        : th_type_clone(types, parameter->type);
                    target_matches = target_parameters[p].type != nullptr;
                }
                for (size_t a = 0; target_matches
                     && a < trait_argument_types.len; a++) {
                    size_t parameter_index =
                        trait_argument_positions.data[a];
                    char field[32];
                    snprintf(field, sizeof(field), "trait_arg_%zu",
                             parameter_index);
                    th_type *copied_argument = th_record_lookup(copy, field);
                    target_matches = parameter_index < target_parameter_count
                        && copied_argument;
                    if (target_matches)
                        target_parameters[parameter_index].type = copied_argument;
                }
                th_type *target_shape = target_matches
                    ? th_func_with_error(types, copied_trait_result,
                        copied_function_failure, target_parameters,
                        target_parameter_count, nullptr, nullptr, 0)
                    : nullptr;
                th_type *matched_signature = target_shape
                    ? th_unify(types, target_signature, target_shape) : nullptr;
                target_matches = matched_signature
                    && th_unify_in_place(types, matched_signature, target_shape);
                if (!target_matches)
                    report_call_type_error(call_unit, call_node, types,
                                           target_shape, target_signature);
                if (target_matches) {
                    th_type *resolved_failure = th_func_error(
                        matched_signature);
                    th_type *combined_failure = specialization
                        ? th_type_clone(types, resolved_failure) : nullptr;
                    if (!combined_failure) {
                        core_diagnostics_report_at(
                            core_translation_unit_diagnostics(call_unit),
                            call_unit, CORE_DIAG_ERROR,
                            CORE_STRING_STATIC("N00BB-T0001"),
                            CORE_STRING_STATIC(
                                "generic trait specializations have incompatible failure channels"),
                            link_source_location(call_node));
                        target_matches = false;
                    }
                    else specialization->failure_type = combined_failure;
                }
                core_free(target_parameters);
                core_list_free(trait_argument_positions);
                core_list_free(trait_argument_types);
                if (!target_matches) continue;
                parse_cf_label_t *generic_call = call_label_for_expression(
                    analysis->grammar, call_state, call_unit, call_node,
                    generic_function);
                th_type *callsite_result = generic_call && generic_call->self
                    && !core_tree_is_leaf(generic_call->self)
                    ? core_tree_node_value(generic_call->self).type
                    : call_node && !core_tree_is_leaf(call_node)
                        ? core_tree_node_value(call_node).type : nullptr;
                if (!callsite_result || !copied_function_result) {
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(call_unit),
                        call_unit, CORE_DIAG_FATAL,
                        CORE_STRING_STATIC("N00BB-T0004"),
                        CORE_STRING_STATIC(
                            "generic trait specialization has no cached call-result type"),
                        link_source_location(call_node));
                    core_list_free(formals);
                    core_dict_free(&seen_calls);
                    return false;
                }
                if (!unify_call_node_type(call_unit, call_state, call_node,
                        call_node, callsite_result, copied_function_result)) {
                    continue;
                }
            }
            core_list_free(formals);
        }
    }
    core_dict_free(&seen_calls);
    return true;
}


