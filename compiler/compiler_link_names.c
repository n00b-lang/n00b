#include "compiler_link.h"
#include "builtin_types.h"
#include "diagnostics.h"
#include "parse/tree_util.h"

static parse_token_info_t *
last_token(parse_tree_t *node)
{
    if (!node) return nullptr;
    if (core_tree_is_leaf(node)) return parse_node_token(node);
    for (size_t i = core_tree_num_children(node); i > 0; i--) {
        parse_token_info_t *token = last_token(core_tree_child(node, i - 1));
        if (token && parse_token_end_byte(token) > parse_token_start_byte(token))
            return token;
    }
    return nullptr;
}

core_diag_location_t
n00bb_node_span(parse_tree_t *node)
{
    core_diag_location_t first = link_source_location(node);
    parse_token_info_t *last = first.has_location ? last_token(node) : nullptr;
    if (!last) return first;
    uint32_t end_line = parse_token_endline(last);
    uint32_t end_column = parse_token_endcol(last);
    if (end_line > first.end_line
        || (end_line == first.end_line && end_column > first.end_column)) {
        first.end_line = end_line;
        first.end_column = end_column;
    }
    return first;
}

static bool
leaf_spells(parse_tree_t *leaf, void *name)
{
    return core_string_eq(parse_tree_leaf_text(leaf), *(core_string_t *)name);
}

static core_diag_location_t
declared_name_location(parse_sym_entry_t *symbol)
{
    core_string_t name = symbol->name;
    parse_tree_t *leaf = name.u8_bytes
        ? parse_tree_scan_leaves(symbol->decl_node, false, leaf_spells, &name)
        : nullptr;
    return link_source_location(leaf ? leaf : symbol->decl_node);
}

static n00bb_package_export_t *
package_export_of(n00bb_tu_state_t *state, parse_sym_entry_t *symbol,
                  n00bb_package_declaration_t **package)
{
    for (size_t p = 0; p < state->package_declarations.len; p++) {
        n00bb_package_declaration_t *declaration =
            &state->package_declarations.data[p];
        for (size_t e = 0; e < declaration->exports.len; e++)
            if (declaration->exports.data[e].symbol == symbol) {
                *package = declaration;
                return &declaration->exports.data[e];
            }
    }
    return nullptr;
}

static bool
declaration_site_key(parse_grammar_t *grammar, parse_sym_entry_t *symbol,
                     parse_sym_entry_t *owner, core_buffer_t *out)
{
    core_translation_unit_t *unit = symbol->owner_unit;
    n00bb_tu_state_t *state = unit ? tu_state(unit) : nullptr;
    if (!state) return false;
    if (owner) {
        if (!declaration_site_key(grammar, owner, owner->owner_type, out))
            return false;
        core_buffer_printf(out, "/%.*s", (int)symbol->name.u8_bytes,
                           symbol->name.data);
        return true;
    }
    n00bb_package_declaration_t *package = nullptr;
    n00bb_package_export_t *export = package_export_of(state, symbol,
                                                       &package);
    if (export) {
        core_string_t ns = symbol->scope && symbol->scope->ns
            ? symbol->scope->ns->ns_name : (core_string_t){0};
        core_buffer_printf(out, "e:%.*s:%.*s:%.*s", (int)ns.u8_bytes,
            ns.data, (int)package->name.u8_bytes, package->name.data,
            (int)export->export_name.u8_bytes, export->export_name.data);
        return true;
    }
    if (symbol->kind == PARSE_SYM_FUNCTION) {
        core_buffer_puts(out, "f:");
        const core_string_t *qualified = parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.interface-qualified-name"));
        if (qualified) {
            core_buffer_append_string(out, *qualified);
            return true;
        }
        return grammar && !state->interface_module
            && append_environment_symbol_name(grammar, unit, symbol, out);
    }
    core_buffer_printf(out, "%c:%.*s.%.*s",
        symbol->kind == PARSE_SYM_TYPEDEF ? 't' : 'v',
        (int)unit->module.identifier.u8_bytes, unit->module.identifier.data,
        (int)symbol->name.u8_bytes, symbol->name.data);
    return true;
}

static void
append_location(core_buffer_t *out, core_diag_location_t at)
{
    append_u32_le(out, at.has_location ? at.line : 0);
    append_u32_le(out, at.column);
    append_u32_le(out, at.end_line);
    append_u32_le(out, at.end_column);
}

static core_diag_location_t
read_location(const uint8_t *data)
{
    core_diag_location_t at = {
        .line = read_u32_le(data),
        .column = read_u32_le(data + 4),
        .end_line = read_u32_le(data + 8),
        .end_column = read_u32_le(data + 12),
    };
    at.has_location = at.line != 0;
    return at;
}

static const size_t declaration_site_bytes = 32;

static void
record_declaration_site(parse_grammar_t *grammar, core_buffer_t *sites,
                        parse_sym_entry_t *symbol, parse_sym_entry_t *owner)
{
    if (!symbol->decl_node) return;
    core_buffer_t key = {0};
    if (declaration_site_key(grammar, symbol, owner, &key)
        && key.byte_len <= UINT32_MAX) {
        append_u32_le(sites, (uint32_t)key.byte_len);
        core_buffer_append(sites, key.data, key.byte_len);
        append_location(sites, declared_name_location(symbol));
        append_location(sites, link_source_location(symbol->decl_node));
    }
    core_buffer_release(&key);
}

static void
record_scope_sites(parse_grammar_t *grammar, core_buffer_t *sites,
                   core_list_t(parse_scope_ptr_t) *scopes)
{
    for (size_t s = 0; s < scopes->len; s++)
        for (parse_sym_entry_t *symbol = scopes->data[s]->first_in_scope;
             symbol; symbol = symbol->next_in_scope) {
            record_declaration_site(grammar, sites, symbol, nullptr);
            if (symbol->kind != PARSE_SYM_TYPEDEF || !symbol->member_scope)
                continue;
            for (parse_sym_entry_t *member =
                     symbol->member_scope->first_in_scope;
                 member; member = member->next_in_scope)
                record_declaration_site(grammar, sites, member, symbol);
        }
}

void
n00bb_record_declaration_sites(parse_grammar_t *grammar,
                               core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = unit ? tu_state(unit) : nullptr;
    if (!state || state->interface_module || !state->annotations) return;
    core_buffer_t *sites = &state->declaration_sites;
    sites->byte_len = 0;
    core_list_t(parse_scope_ptr_t) values = module_scopes(state);
    core_list_t(parse_scope_ptr_t) types = module_scopes_in_namespace(state,
        CORE_STRING_STATIC("type"));
    record_scope_sites(grammar, sites, &values);
    record_scope_sites(grammar, sites, &types);
    core_list_free(types);
    core_list_free(values);
    for (size_t p = 0; p < state->package_declarations.len; p++) {
        n00bb_package_declaration_t *package =
            &state->package_declarations.data[p];
        for (size_t e = 0; e < package->exports.len; e++)
            if (package->exports.data[e].symbol)
                record_declaration_site(grammar, sites,
                    package->exports.data[e].symbol, nullptr);
    }
}

static core_diag_location_t
interface_declaration_site(parse_sym_entry_t *symbol, bool name)
{
    n00bb_tu_state_t *state = symbol && symbol->owner_unit
        ? tu_state(symbol->owner_unit) : nullptr;
    if (!state || !state->interface_module
        || !state->declaration_sites.byte_len)
        return (core_diag_location_t){0};
    core_buffer_t key = {0};
    core_diag_location_t found = {0};
    if (declaration_site_key(nullptr, symbol, symbol->owner_type, &key)) {
        const uint8_t *data = (const uint8_t *)state->declaration_sites.data;
        size_t size = state->declaration_sites.byte_len;
        size_t at = 0;
        while (size - at >= 4) {
            size_t length = read_u32_le(data + at);
            at += 4;
            if (length > size - at
                || size - at - length < declaration_site_bytes) break;
            bool match = core_string_eq(
                core_string_borrow((const char *)data + at, length),
                core_buffer_string(&key));
            at += length;
            if (match) {
                found = read_location(data + at + (name ? 0 : 16));
                break;
            }
            at += declaration_site_bytes;
        }
    }
    core_buffer_release(&key);
    return found;
}

core_diag_location_t
n00bb_symbol_location(parse_sym_entry_t *symbol)
{
    parse_sym_entry_t *origin = symbol ? canonical_symbol(symbol) : nullptr;
    if (!origin) return (core_diag_location_t){0};
    return origin->decl_node ? declared_name_location(origin)
                             : interface_declaration_site(origin, true);
}

core_diag_location_t
n00bb_symbol_declaration_location(parse_sym_entry_t *symbol)
{
    if (!symbol) return (core_diag_location_t){0};
    return symbol->decl_node ? link_source_location(symbol->decl_node)
                             : interface_declaration_site(symbol, false);
}

void
n00bb_diag_declared_here(core_diagnostics_t *diagnostics,
                         parse_sym_entry_t *symbol, n00bb_text_t text)
{
    parse_sym_entry_t *origin = symbol ? canonical_symbol(symbol) : nullptr;
    core_diag_location_t at = n00bb_symbol_location(origin);
    if (!at.has_location || !origin->owner_unit) return;
    n00bb_report_secondary(diagnostics, origin->owner_unit, at, text,
                           N00BB_ARGS(ARG_TEXT(origin->name)));
}

static bool
suggestable(core_string_t name)
{
    return name.u8_bytes && name.data[0] != '$';
}

static void
push_name(core_list_t(core_string_t) *out, core_string_t name)
{
    if (suggestable(name)) core_list_push(*out, name);
}

static void
push_scope_chain(parse_scope_t *scope, core_list_t(core_string_t) *out)
{
    for (parse_scope_t *at = scope; at; at = at->parent)
        for (parse_sym_entry_t *symbol = at->first_in_scope; symbol;
             symbol = symbol->next_in_scope)
            if (!symbol->is_synthetic && !symbol->is_field)
                push_name(out, symbol->name);
}

static void
push_visible_members(core_translation_unit_t *consumer,
                     core_translation_unit_t *provider, parse_scope_t *scope,
                     core_list_t(core_string_t) *out)
{
    for (parse_sym_entry_t *symbol = scope ? scope->first_in_scope : nullptr;
         symbol; symbol = symbol->next_in_scope) {
        parse_sym_entry_t *origin = canonical_symbol(symbol);
        core_translation_unit_t *owner = provider ? provider
                                                  : origin->owner_unit;
        if (!symbol->is_synthetic && symbol->kind != PARSE_SYM_MODULE
            && (!owner || n00bb_symbol_visible_to(consumer, owner, origin)))
            push_name(out, symbol->name);
    }
}

static parse_scope_t *
scope_in_namespace(n00bb_tu_state_t *state, parse_scope_t *scope,
                   core_string_t ns)
{
    parse_symtab_t *symtab = state && state->annotations
        ? state->annotations->symtab : nullptr;
    return symtab && scope ? parse_symtab_scope_in_namespace(symtab, scope, ns)
                           : nullptr;
}

static void
push_builtin_type_names(core_list_t(core_string_t) *out)
{
    static const core_string_t named[] = {
        CORE_STRING_STATIC("string"), CORE_STRING_STATIC("char"),
        CORE_STRING_STATIC("byte"), CORE_STRING_STATIC("Any"),
        CORE_STRING_STATIC("List"), CORE_STRING_STATIC("Dict"),
        CORE_STRING_STATIC("Set"), CORE_STRING_STATIC("Array"),
        CORE_STRING_STATIC("Ref"), CORE_STRING_STATIC("View"),
        CORE_STRING_STATIC("Atomic"), CORE_STRING_STATIC("Tuple"),
        CORE_STRING_STATIC("Foreign"), CORE_STRING_STATIC("Buffer"),
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++)
        push_name(out, named[i]);
    const ir_profile_t *profile = n00bb_type_profile();
    for (size_t i = 0; profile && i < profile->n_scalars; i++)
        push_name(out, profile->scalars[i].type_text);
}

core_list_t(core_string_t)
n00bb_visible_names(core_translation_unit_t *unit, parse_scope_t *scope,
                    n00bb_name_kinds_t kinds)
{
    core_list_t(core_string_t) names = core_list_new(core_string_t);
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state) return names;
    parse_scope_t *values = scope ? scope : module_scope(state);
    if (kinds & N00BB_NAMES_VALUES) {
        push_scope_chain(scope_in_namespace(state, values,
                                            CORE_STRING_STATIC("")), &names);
        for (size_t i = 0; i < state->import_aliases.len; i++)
            push_name(&names, state->import_aliases.data[i].name);
    }
    if (kinds & N00BB_NAMES_TYPES) {
        parse_scope_t *types = scope_in_namespace(state, values,
                                                  CORE_STRING_STATIC("type"));
        push_scope_chain(types ? types : type_module_scope(state), &names);
        push_builtin_type_names(&names);
    }
    const core_list_t(n00bb_import_source_t) *sources =
        unqualified_import_sources(unit);
    for (size_t i = 0; sources && i < sources->len; i++) {
        const n00bb_import_source_t *source = &sources->data[i];
        if (source->brought && !source->brought->all) {
            push_name(&names, source->brought->local);
            continue;
        }
        if (kinds & N00BB_NAMES_VALUES)
            push_visible_members(unit, source->provider, source->values,
                                 &names);
        if (kinds & N00BB_NAMES_TYPES)
            push_visible_members(unit, source->provider, source->types,
                                 &names);
    }
    return names;
}

bool
n00bb_suggest_visible_name(core_diagnostics_t *diagnostics,
                           core_translation_unit_t *unit,
                           parse_scope_t *scope, core_string_t name,
                           n00bb_name_kinds_t kinds)
{
    core_list_t(core_string_t) names = n00bb_visible_names(unit, scope,
                                                           kinds);
    bool suggested = n00bb_diag_did_you_mean(diagnostics, name, names.data,
                                             names.len);
    core_list_free(names);
    return suggested;
}

bool
n00bb_suggest_scope_member(core_diagnostics_t *diagnostics,
                           core_translation_unit_t *consumer,
                           core_translation_unit_t *provider,
                           parse_scope_t *scope, core_string_t name)
{
    core_list_t(core_string_t) names = core_list_new(core_string_t);
    push_visible_members(consumer, provider, scope, &names);
    bool suggested = n00bb_diag_did_you_mean(diagnostics, name, names.data,
                                             names.len);
    core_list_free(names);
    return suggested;
}

static core_string_t
import_name_for(n00bb_tu_state_t *state, core_translation_unit_t *imported)
{
    for (size_t i = 0; i < state->import_aliases.len; i++)
        if (state->import_aliases.data[i].unit == imported)
            return state->import_aliases.data[i].name;
    return (core_string_t){0};
}

static parse_sym_entry_t *
declared_in_module(core_translation_unit_t *owner, core_string_t name,
                   bool type_reference)
{
    n00bb_tu_state_t *state = tu_state(owner);
    parse_scope_t *scope = type_reference ? type_module_scope(state)
                                          : module_scope(state);
    parse_sym_entry_t *symbol = scope ? parse_symtab_lookup_in(scope, name)
                                      : nullptr;
    if (!symbol && !type_reference && state)
        symbol = parse_symtab_lookup_in(type_module_scope(state), name);
    return symbol && symbol->decl_node && !symbol->is_synthetic
        && symbol->kind != PARSE_SYM_MODULE ? symbol : nullptr;
}

static bool
scope_encloses(parse_scope_t *outer, parse_scope_t *inner)
{
    parse_scope_t *outer_lexical = outer && outer->lexical_scope
        ? outer->lexical_scope : outer;
    for (parse_scope_t *at = inner; at; at = at->parent) {
        parse_scope_t *lexical = at->lexical_scope ? at->lexical_scope : at;
        if (lexical == outer_lexical) return true;
    }
    return false;
}

bool
n00bb_explain_hidden_name(core_diagnostics_t *diagnostics,
                          core_translation_unit_t *unit,
                          parse_scope_t *scope, core_string_t name,
                          bool type_reference)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !name.data) return false;
    for (size_t i = 0; i < core_translation_unit_import_count(unit); i++) {
        core_translation_unit_t *imported =
            core_translation_unit_import(unit, i);
        parse_sym_entry_t *symbol = declared_in_module(imported, name,
                                                       type_reference);
        if (!symbol) continue;
        core_string_t module = imported->module.identifier;
        core_string_t local = import_name_for(state, imported);
        if (!n00bb_symbol_visible_to(unit, imported, symbol)) {
            n00bb_report_note(diagnostics, N00B_TEXT_A_EXISTS_IN_MODULE_BUT,
                N00BB_ARGS(ARG_TEXT(name), ARG_TEXT(module)));
            n00bb_diag_declared_here(diagnostics, symbol,
                N00B_TEXT_PRIVATE_IS_DECLARED_HERE);
            return true;
        }
        n00bb_diag_declared_here(diagnostics, symbol,
            N00B_TEXT_IS_DECLARED_HERE);
        if (local.data)
            n00bb_report_help(diagnostics,
                N00B_TEXT_IS_DECLARED_IN_MODULE_WRITE,
                N00BB_ARGS(ARG_TEXT(name), ARG_TEXT(module), ARG_TEXT(local),
                           ARG_TEXT(name), ARG_TEXT(name), ARG_TEXT(module)));
        else
            n00bb_report_help(diagnostics, N00B_TEXT_IS_DECLARED_IN_MODULE_ADD,
                N00BB_ARGS(ARG_TEXT(name), ARG_TEXT(module), ARG_TEXT(name),
                           ARG_TEXT(module)));
        return true;
    }
    parse_symtab_t *symtab = state->annotations
        ? state->annotations->symtab : nullptr;
    for (parse_scope_t *other = symtab ? symtab->all_scopes : nullptr; other;
         other = other->all_next) {
        if (other->owner_unit && other->owner_unit != unit) continue;
        if (scope && scope_encloses(other, scope)) continue;
        for (parse_sym_entry_t *symbol = other->first_in_scope; symbol;
             symbol = symbol->next_in_scope) {
            if (!core_string_eq(symbol->name, name) || !symbol->decl_node
                || symbol->is_synthetic || symbol->is_field
                || symbol->owner_type) continue;
            core_diag_location_t at = link_source_location(symbol->decl_node);
            if (!at.has_location) continue;
            n00bb_report_secondary(diagnostics, unit, at,
                N00B_TEXT_IS_DECLARED_HERE_IN_A,
                N00BB_ARGS(ARG_TEXT(name)));
            return true;
        }
    }
    return false;
}

static int32_t
declaration_start(const parse_sym_entry_t *symbol)
{
    return symbol->decl_node ? core_tree_node_value(symbol->decl_node).start
                             : 0;
}

static void
push_record_scope_fields(parse_scope_t *scope,
                         core_list_t(core_string_t) *out)
{
    core_list_t(parse_sym_entry_ptr_t) ordered =
        core_list_new(parse_sym_entry_ptr_t);
    for (parse_sym_entry_t *symbol = scope ? scope->first_in_scope : nullptr;
         symbol; symbol = symbol->next_in_scope) {
        if (!symbol->is_field || symbol->is_synthetic) continue;
        size_t at = ordered.len;
        core_list_push(ordered, symbol);
        while (at > 0 && declaration_start(ordered.data[at - 1])
                             > declaration_start(symbol)) {
            ordered.data[at] = ordered.data[at - 1];
            at--;
        }
        ordered.data[at] = symbol;
    }
    for (size_t i = 0; i < ordered.len; i++)
        push_name(out, ordered.data[i]->name);
    core_list_free(ordered);
}

static void
push_structural_fields(th_type *type, core_list_t(core_string_t) *out)
{
    type = type ? th_resolve(type) : nullptr;
    if (!type || th_kind_of(type) != TH_RECORD) return;
    for (size_t i = 0; i < th_record_len(type); i++) {
        const th_member *member = th_record_member(type, i);
        if (member) push_name(out, member->name);
    }
}

static void
describe_fields(core_diagnostics_t *diagnostics, core_string_t type_text,
                const core_list_t(core_string_t) *fields)
{
    if (!fields->len) {
        n00bb_report_note(diagnostics, N00B_TEXT_HAS_NO_FIELDS,
            N00BB_ARGS(ARG_TEXT(type_text)));
        return;
    }
    n00bb_report_note(diagnostics, N00B_TEXT_TYPE_HAS_FIELDS,
        N00BB_ARGS(ARG_NAMES(fields->data, fields->len), ARG_TEXT(type_text)));
}

void
n00bb_report_missing_field(core_translation_unit_t *unit, th_ctx *types,
                           parse_tree_t *member_node, th_type *receiver_type,
                           core_string_t member, parse_scope_t *record_scope,
                           parse_sym_entry_t *record_symbol)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t type_text = receiver_type && types
        ? n00bb_print_type(types, receiver_type)
        : record_symbol ? record_symbol->name : CORE_STRING_STATIC("record");
    n00bb_report(diagnostics, unit, N00B_DIAG_FIELD_MISSING,
        n00bb_node_span(member_node), N00B_TEXT_HAS_NO_FIELD,
        N00BB_ARGS(ARG_TEXT(type_text), ARG_TEXT(member)));
    n00bb_report_label(diagnostics, N00B_TEXT_UNKNOWN_FIELD, N00BB_NO_ARGS);
    core_list_t(core_string_t) fields = core_list_new(core_string_t);
    if (record_scope) push_record_scope_fields(record_scope, &fields);
    else push_structural_fields(receiver_type, &fields);
    if (!n00bb_diag_did_you_mean(diagnostics, member, fields.data,
                                 fields.len))
        describe_fields(diagnostics, type_text, &fields);
    core_list_free(fields);
    if (record_symbol)
        n00bb_diag_declared_here(diagnostics, record_symbol,
                                 N00B_TEXT_TYPE_IS_DECLARED_HERE);
}

void
n00bb_report_missing_method(core_translation_unit_t *unit, th_ctx *types,
                            parse_tree_t *member, th_type *receiver_type,
                            core_string_t method, bool ambiguous)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    size_t children = member && !core_tree_is_leaf(member)
        ? core_tree_num_children(member) : 0;
    parse_tree_t *name_node = children >= 3 ? core_tree_child(member, 2)
                                            : member;
    core_string_t type_text = n00bb_print_type(types, receiver_type);
    if (ambiguous) {
        n00bb_report(diagnostics, unit,
            N00B_DIAG_TRAIT_IMPLEMENTATION_AMBIGUOUS,
            n00bb_node_span(name_node),
            N00B_TEXT_MORE_THAN_ONE_IMPLEMENTATION_OF,
            N00BB_ARGS(ARG_TEXT(method), ARG_TEXT(type_text)));
        n00bb_report_help(diagnostics, N00B_TEXT_ANNOTATE_THE_VALUE_WITH_A,
            N00BB_NO_ARGS);
        return;
    }
    n00bb_report(diagnostics, unit, N00B_DIAG_METHOD_NOT_FOUND,
        n00bb_node_span(name_node), N00B_TEXT_HAS_NO_METHOD,
        N00BB_ARGS(ARG_TEXT(type_text), ARG_TEXT(method)));
    n00bb_report_label(diagnostics, N00B_TEXT_UNKNOWN_METHOD, N00BB_NO_ARGS);
    n00bb_report_note(diagnostics, N00B_TEXT_METHODS_COME_FROM_TRAITS_THAT,
        N00BB_ARGS(ARG_TEXT(type_text), ARG_TEXT(method)));
}

void
n00bb_report_inference_failure(void *context, parse_visit_t *visit,
                               th_ctx *types, const th_error_t *reason)
{
    core_translation_unit_t *unit = context;
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    if (!diagnostics) return;
    core_diag_location_t at = link_source_location(visit->node);
    th_type *called = nullptr;
    if (parse_annot_infer_calls_value(reason, &called)) {
        core_string_t text = called ? n00bb_print_type(types, called)
                                    : (core_string_t){0};
        if (called)
            n00bb_report(diagnostics, unit, N00B_DIAG_NOT_CALLABLE, at,
                N00B_TEXT_THIS_VALUE_IS_NOT_A,
                N00BB_ARGS(ARG_TEXT(text)));
        else
            n00bb_report(diagnostics, unit, N00B_DIAG_NOT_CALLABLE, at,
                N00B_TEXT_THIS_VALUE_IS_AN_INTEGER,
                N00BB_NO_ARGS);
        n00bb_report_label(diagnostics, N00B_TEXT_CALLED_HERE, N00BB_NO_ARGS);
        return;
    }
    th_type *expected = reason ? reason->expected : nullptr;
    th_type *got = reason ? reason->got : nullptr;
    core_string_t why = reason && reason->message.data
        ? reason->message : (core_string_t){0};
    th_err_kind kind = reason ? reason->kind : TH_ERR_UNIFY;
    if (kind == TH_ERR_FREE_VAR) {
        n00bb_internal_error(diagnostics, unit, at, "inferring types",
                             "%.*s", (int)why.u8_bytes, why.data);
        return;
    }
    if (kind == TH_ERR_BAD_NAME) {
        n00bb_report(diagnostics, unit, N00B_DIAG_INVALID_TYPE_NAME, at,
            N00B_TEXT_THIS_TYPE_NAME_IS_NOT,
            N00BB_ARGS(ARG_TEXT(why)));
        return;
    }
    if (kind == TH_ERR_OCCURS) {
        n00bb_report(diagnostics, unit, N00B_DIAG_RECURSIVE_TYPE, at,
            N00B_TEXT_THIS_VALUE_WOULD_NEED_A,
            N00BB_NO_ARGS);
        return;
    }
    if (kind == TH_ERR_NO_SUCH_FIELD && got
        && th_kind_of(th_resolve(got)) == TH_RECORD
        && member_access_node(visit->grammar, visit->node) == visit->node)
        return;
    core_string_t expected_text = expected
        ? n00bb_print_type(types, expected) : (core_string_t){0};
    core_string_t got_text = got ? n00bb_print_type(types, got)
                                 : (core_string_t){0};
    if (kind == TH_ERR_NO_SUCH_FIELD && got) {
        n00bb_report(diagnostics, unit, N00B_DIAG_FIELD_MISSING, at,
            N00B_TEXT_HAS_NO_SUCH_FIELD,
            N00BB_ARGS(ARG_TEXT(got_text)));
        return;
    }
    if (expected && got) {
        n00bb_report(diagnostics, unit, N00B_DIAG_TYPE_MISMATCH, at,
            N00B_TEXT_EXPECTED_FOUND,
            N00BB_ARGS(ARG_TEXT(expected_text), ARG_TEXT(got_text)));
        if (why.u8_bytes)
            n00bb_report_label(diagnostics, N00B_TEXT_TEXT,
                N00BB_ARGS(ARG_TEXT(why)));
    }
    else if (got) {
        n00bb_report(diagnostics, unit, N00B_DIAG_TYPE_REQUIREMENT, at,
            N00B_TEXT_CANNOT_BE_USED_HERE,
            N00BB_ARGS(ARG_TEXT(got_text)));
        if (why.u8_bytes)
            n00bb_report_label(diagnostics, N00B_TEXT_TEXT,
                N00BB_ARGS(ARG_TEXT(why)));
    }
    else if (why.u8_bytes)
        n00bb_report(diagnostics, unit, N00B_DIAG_TYPE_MISMATCH, at,
            N00B_TEXT_TEXT,
            N00BB_ARGS(ARG_TEXT(why)));
    else
        n00bb_report(diagnostics, unit, N00B_DIAG_TYPE_MISMATCH, at,
            N00B_TEXT_THE_TYPES_IN_THIS_EXPRESSION,
            N00BB_NO_ARGS);
    if (reason && reason->has_related && reason->related.start_line)
        n00bb_report_secondary(diagnostics, unit, (core_diag_location_t){
                .line = reason->related.start_line,
                .column = reason->related.start_col,
                .end_line = reason->related.end_line,
                .end_column = reason->related.end_col,
                .has_location = true,
            }, N00B_TEXT_THE_OTHER_USE_IS_HERE,
            N00BB_NO_ARGS);
}
