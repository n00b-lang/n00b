#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cached_body.h"
#include "call_plan.h"
#include "compiler_axioms.h"
#include "compiler_cache.h"
#include "compiler_link.h"
#include "compiler_tu.h"
#include "diagnostics.h"
#include "parse/tree_util.h"
#include "compiler_native_roots.h"
#include "record_layout_request.h"
#include "qualified_names.h"
#include "lib/buffer.h"
#include "lib/hash.h"

typedef th_type *th_type_ptr_t;
core_list_decl(th_type_ptr_t);
core_list_decl(size_t);

enum { N00BB_SPECIALIZATION_FIELD_SIZE = 40 };

core_link_declaration_t *
linked_method_declaration(core_link_unit_t *link,
                          const th_env_method_t *method)
{
    if (!link || !method || !method->module.data
        || !method->qualified_name.data)
        return nullptr;
    size_t module_length = method->module.u8_bytes;
    core_string_t qualified = method->qualified_name;
    if (qualified.u8_bytes <= module_length
        || !core_string_starts_with(qualified, method->module)
        || qualified.data[module_length] != '.') return nullptr;
    core_buffer_t key = {0};
    core_buffer_printf(&key, "%u:%.*s", (unsigned)PARSE_SYM_FUNCTION,
                       (int)(qualified.u8_bytes - module_length - 1),
                       qualified.data + module_length + 1);
    core_link_declaration_t *found = nullptr;
    for (size_t i = 0; i < core_link_unit_declaration_count(link); i++) {
        core_link_declaration_t *candidate =
            core_link_unit_declaration_at(link, i);
        if (core_string_eq(core_link_declaration_module(candidate),
                           method->module)
            && core_string_eq(core_link_declaration_key(candidate),
                              core_buffer_string(&key))) {
            found = candidate;
            break;
        }
    }
    core_buffer_release(&key);
    return found;
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
            grammar, owner, trait, caller);
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
    return found;
}

bool
n00bb_field_use_is_write(parse_grammar_t *grammar, parse_sym_entry_t *field,
                         parse_tree_t *use, bool *also_reads)
{
    for (parse_tree_t *parent = parse_tree_parent(use);
         parent; parent = parse_tree_parent(parent)) {
        const parse_rule_t *rule = parse_tree_rule(grammar, parent);
        if (rule && parse_rule_find_annot(grammar, rule, PARSE_ANNOT_CALL)
            && core_tree_num_children(parent)) {
            parse_sym_entry_t *callee = bound_function_symbol(
                core_tree_child(parent, 0));
            const n00bb_axiom_t *axiom = n00bb_axiom_of_symbol(callee);
            parse_tree_t *first = axiom
                && (axiom->writes & N00BB_AXIOM_PARAM(0))
                ? call_argument_for_parameter(grammar, parent, callee,
                                              0, false) : nullptr;
            if (first && node_is_within(use, first)
                && core_string_eq(first_token_text(first),
                                  CORE_STRING_STATIC("ref"))) {
                *also_reads = axiom->roles & N00BB_AXIOM_READS_TARGET;
                return true;
            }
        }
        if (!nt_is(grammar, parent, CORE_STRING_STATIC("assign-stmt"))
            && !nt_is(grammar, parent, CORE_STRING_STATIC("binop-assign-stmt"))) continue;
        if (core_tree_num_children(parent) < 2
            || !node_is_within(use, core_tree_child(parent, 0)))
            continue;
        for (parse_tree_t *path = use; path != parent;
             path = parse_tree_parent(path)) {
            parse_tree_t *ancestor = parse_tree_parent(path);
            if (n00bb_postfix_node(grammar, ancestor)
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
        *also_reads = nt_is(grammar, parent, CORE_STRING_STATIC("binop-assign-stmt"));
        if (nt_is(grammar, parent, CORE_STRING_STATIC("assign-stmt"))) {
            core_string_t op = first_token_text(core_tree_child(parent, 1));
            *also_reads = !core_string_eq(op, CORE_STRING_STATIC("="));
        }
        return true;
    }
    return false;
}

bool
n00bb_field_shared_outside(parse_sym_entry_t *field)
{
    n00bb_record_layout_request_t *request = field->owner_type
        ? parse_sym_binding(field->owner_type,
                            CORE_STRING_STATIC("n00bb.record-layout-request"))
        : nullptr;
    return request && request->policy == N00BB_RECORD_C;
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
    th_type *signature = n00bb_symbol_signature(symbol);
    th_type *success = nullptr, *failure = nullptr;
    if (!signature || !th_env_signature_channels(types, signature,
                                                  &success, &failure))
        return nullptr;
    (void)failure;
    return th_type_clone(types, success);
}

