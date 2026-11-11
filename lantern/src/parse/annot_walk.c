#include "parse/annot_walk.h"
#include "parse/tree_util.h"
#include "private/parse/grammar.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/hash.h"
#include "lib/module.h"

#include <string.h>

enum {
    PHASE_SCOPE  = 0,
    PHASE_DECL   = 1,
    PHASE_REFINE = 2,
    PHASE_CF     = 3,
};

typedef struct {
    parse_annot_result_t *out;
    parse_sym_entry_t    *last_sym;
    core_string_t        pending_visibility;
    core_string_t        pending_ns;
    bool                declared_ahead;
    bool                reentry;
} actx_t;

static bool link_type_members(parse_sym_entry_t *type, parse_scope_t *members);

static parse_scope_t *
reentered_scope(actx_t *a, parse_visit_t *v)
{
    parse_scope_t *scope = a->reentry ? parse_tree_scope(v->node) : nullptr;
    if (!scope || scope->node != v->node) return nullptr;
    parse_symtab_enter_scope_all(a->out->symtab, scope);
    return scope;
}

static parse_sym_entry_t *
declare_symbol(actx_t *a, core_string_t ns_name, core_string_t name,
               parse_sym_kind_t kind, parse_tree_t *node)
{
    if (a->reentry) {
        parse_namespace_t *ns = parse_symtab_ns(a->out->symtab, ns_name);
        parse_scope_t *scope = ns ? ns->current : nullptr;
        for (parse_sym_entry_t *existing = scope
                 ? parse_symtab_lookup_in(scope, name) : nullptr;
             existing; existing = existing->shadowed)
            if (existing->decl_node == node) return existing;
    }
    return parse_symtab_add(a->out->symtab, ns_name, name, kind, node);
}

static core_string_t
ns_default(void)
{
    return CORE_STRING_STATIC("");
}

static bool
name_is(core_string_t s, core_string_t want)
{
    return s.data && core_string_eq(s, want);
}

static core_string_t
cached_or_leaf_name(parse_tree_t *node)
{
    while (node) {
        if (core_tree_is_leaf(node)) return parse_tree_leaf_text(node);
        core_string_t unresolved = parse_tree_unresolved_name(node);
        if (unresolved.data) return unresolved;
        parse_sym_entry_t *sym = parse_tree_sym(node);
        if (sym && sym->name.data) return sym->name;
        if (core_tree_num_children(node) != 1) break;
        node = core_tree_child(node, 0);
    }
    return (core_string_t){0};
}

static core_string_t
ref_name(parse_visit_t *v, parse_child_ref_t ref)
{
    parse_tree_t *node = parse_tree_resolve_child_ref(v->grammar, v->node,
                                                       ref);
    return cached_or_leaf_name(node);
}

static bool
scope_open(parse_visit_t *v)
{
    actx_t           *a = (actx_t *)v->thunk;
    parse_annotation_t *an = v->annot;
    core_string_t      scope_name = an->scope_tag;

    if (reentered_scope(a, v)) return true;

    if (parse_child_ref_is_set(an->name_ref)) {
        core_string_t resolved = ref_name(v, an->name_ref);

        if (resolved.u8_bytes > 0) {
            scope_name = resolved;
        }
    }

    parse_symtab_push_scope_all(a->out->symtab, scope_name);

    parse_scope_t *scope = parse_symtab_current_scope(a->out->symtab, ns_default());

    parse_tree_set_scope(v->node, scope);
    for (int32_t i = 0; i < a->out->symtab->ns_count; i++) {
        parse_scope_t *peer = parse_symtab_scope_in_namespace(
            a->out->symtab, scope, a->out->symtab->namespaces[i]->ns_name);
        if (peer) peer->scope_tag = an->scope_tag;
    }
    parse_symtab_set_scope_node_all(a->out->symtab, v->node);

    if (name_is(an->scope_tag, CORE_STRING_STATIC("function"))) {
        parse_sym_entry_t *result = parse_symtab_add(a->out->symtab,
            ns_default(), CORE_STRING_STATIC("$return"),
            PARSE_SYM_VARIABLE, v->node);
        if (result) result->is_synthetic = true;
    }

    return true;
}

static bool
scope_close(parse_visit_t *v)
{
    actx_t *a = (actx_t *)v->thunk;

    parse_symtab_pop_scope_all(a->out->symtab);

    return true;
}

static bool
node_enter(parse_visit_t *v)
{
    actx_t *a = (actx_t *)v->thunk;
    a->last_sym = nullptr;

    parse_scope_t *current = parse_tree_scope(v->node);
    if (v->pn && (!current || (a->reentry && current->node != v->node))) {
        parse_scope_t *scope = parse_symtab_current_scope(a->out->symtab,
                                                          ns_default());
        if (scope) parse_tree_set_scope(v->node, scope);
    }

    if (a->out->on_node) {
        a->out->on_node(v, true, a->out->on_node_context);
    }

    return true;
}

static bool
node_leave(parse_visit_t *v)
{
    actx_t *a = (actx_t *)v->thunk;

    parse_sym_entry_t *sym = parse_tree_sym(v->node);
    if (sym && sym->kind == PARSE_SYM_TYPEDEF && sym->member_scope) {
        (void)link_type_members(sym, sym->member_scope);
    }

    if (a->out->on_node) {
        a->out->on_node(v, false, a->out->on_node_context);
    }

    return true;
}

static core_string_t
decl_ns(actx_t *a)
{
    core_string_t ns = a->pending_ns.data ? a->pending_ns : ns_default();

    a->pending_ns = (core_string_t){0};

    return ns;
}

static void
declared(actx_t *a, parse_visit_t *v, parse_sym_entry_t *sym)
{
    a->last_sym = sym;
    (void)parse_tree_set_resolution_symbol(v->node, sym);
}

static parse_sym_entry_t *
subject(actx_t *a, parse_visit_t *v)
{
    parse_sym_entry_t *sym = parse_tree_sym(v->node);
    return sym ? sym : a->last_sym;
}

static parse_sym_kind_t
sym_kind_of(core_string_t spec)
{
    if (name_is(spec, CORE_STRING_STATIC("param"))) {
        return PARSE_SYM_PARAM;
    }

    if (name_is(spec, CORE_STRING_STATIC("function"))) {
        return PARSE_SYM_FUNCTION;
    }

    if (name_is(spec, CORE_STRING_STATIC("module"))) {
        return PARSE_SYM_MODULE;
    }

    return PARSE_SYM_VARIABLE;
}

static parse_tree_t *child(parse_visit_t *v, parse_child_ref_t ref);

static parse_tree_t *
declaration_initializer(parse_visit_t *v, parse_annotation_t *declares)
{
    if (!parse_grammar_declares_after_initializer(v->grammar)) return nullptr;
    parse_annotation_t *assigns = parse_rule_find_annot(v->grammar, v->rule,
                                                        PARSE_ANNOT_ASSIGNS);
    if (!assigns
        || child(v, assigns->name_ref) != child(v, declares->name_ref))
        return nullptr;
    return child(v, assigns->value_ref);
}

static bool
annot_declares(parse_visit_t *v)
{
    actx_t           *a  = (actx_t *)v->thunk;
    parse_annotation_t *an = v->annot;
    core_string_t      nm = ref_name(v, an->name_ref);

    if (nm.u8_bytes == 0) {
        return true;
    }

    parse_sym_kind_t   kind = sym_kind_of(an->sym_kind);
    core_string_t namespace_name = decl_ns(a);
    parse_namespace_t *ns = parse_symtab_ns(a->out->symtab, namespace_name);
    parse_scope_t *decl_scope = ns ? ns->current : nullptr;
    bool scope_is_decl_body = parse_tree_scope(v->node) && decl_scope
                           && decl_scope->node == v->node
                           && decl_scope->parent;

    if (scope_is_decl_body) {
        parse_symtab_enter_scope(a->out->symtab,
                                 decl_scope->ns->ns_name,
                                 decl_scope->parent);
    }

    parse_sym_entry_t *sym  = declare_symbol(a, namespace_name, nm, kind,
                                             v->node);

    if (scope_is_decl_body) {
        parse_symtab_enter_scope(a->out->symtab,
                                 decl_scope->ns->ns_name, decl_scope);
    }

    sym->mutability = kind == PARSE_SYM_PARAM ? PARSE_SYM_IMMUTABLE
                                            : PARSE_SYM_MUTABLE;
    sym->initializer_node = declaration_initializer(v, an);

    if (a->pending_visibility.data && kind != PARSE_SYM_PARAM) {
        sym->visibility       = a->pending_visibility;
        a->pending_visibility = (core_string_t){0};
    }

    declared(a, v, sym);

    return true;
}

static bool
link_type_members(parse_sym_entry_t *type, parse_scope_t *members)
{
    if (!type || !members) return false;
    type->member_scope = members;
    for (parse_sym_entry_t *member = members->first_in_scope; member;
         member = member->next_in_scope) {
        if (member->owner_type && member->owner_type != type) return false;
        member->owner_type = type;
    }
    return true;
}

static bool
annot_type(parse_visit_t *v)
{
    actx_t           *a  = (actx_t *)v->thunk;
    parse_annotation_t *an = v->annot;
    core_string_t      nm = ref_name(v, an->name_ref);

    if (nm.u8_bytes == 0) {
        return true;
    }

    core_string_t namespace_name = decl_ns(a);
    parse_namespace_t *ns = parse_symtab_ns(a->out->symtab, namespace_name);
    parse_scope_t *decl_scope = ns ? ns->current : nullptr;
    bool scope_is_decl_body = parse_tree_scope(v->node) && decl_scope
                           && decl_scope->node == v->node
                           && decl_scope->parent;

    if (scope_is_decl_body) {
        parse_symtab_enter_scope(a->out->symtab, namespace_name,
                                 decl_scope->parent);
    }

    parse_sym_entry_t *sym = declare_symbol(a, namespace_name, nm,
                                            PARSE_SYM_TYPEDEF, v->node);

    if (scope_is_decl_body) {
        parse_symtab_enter_scope(a->out->symtab, namespace_name, decl_scope);
    }

    parse_scope_t *members = decl_scope && decl_scope->lexical_scope
        ? decl_scope->lexical_scope : decl_scope;
    if (members && !link_type_members(sym, members)) return false;

    declared(a, v, sym);

    return true;
}

static bool
annot_field(parse_visit_t *v)
{
    actx_t           *a  = (actx_t *)v->thunk;
    parse_annotation_t *an = v->annot;
    core_string_t      nm = ref_name(v, an->name_ref);

    if (nm.u8_bytes == 0) {
        return true;
    }

    parse_sym_entry_t *sym = declare_symbol(a, ns_default(), nm,
                                            PARSE_SYM_VARIABLE, v->node);

    sym->is_field = true;
    if (a->pending_visibility.data) {
        sym->visibility = a->pending_visibility;
        a->pending_visibility = (core_string_t){0};
    }

    parse_scope_t *member_scope = parse_tree_scope(v->node);
    parse_sym_entry_t *owner = member_scope
        ? parse_tree_sym(member_scope->node) : nullptr;
    if (owner && owner->kind == PARSE_SYM_TYPEDEF) {
        sym->owner_type = owner;
        if (owner->member_scope && owner->member_scope != member_scope)
            return false;
        owner->member_scope = member_scope;
    }

    declared(a, v, sym);

    return true;
}

static bool
annot_namespace(parse_visit_t *v)
{
    actx_t *a = (actx_t *)v->thunk;

    if (v->annot->ns_name.data) {
        a->pending_ns = v->annot->ns_name;
    }

    return true;
}

static bool
annot_direction(parse_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    parse_sym_entry_t *sym = subject(a, v);

    if (!sym || !v->annot->direction.data) {
        return true;
    }

    static const struct {
        core_string_t spelling;
        th_mode      mode;
    } modes[] = {
        {CORE_STRING_STATIC("in"),    TH_IN   },
        {CORE_STRING_STATIC("out"),   TH_OUT  },
        {CORE_STRING_STATIC("inout"), TH_INOUT},
        {CORE_STRING_STATIC("modify"), TH_INOUT},
        {CORE_STRING_STATIC("consume"), TH_CONSUME},
        {CORE_STRING_STATIC("view"), TH_VIEW},
    };

    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (core_string_eq(modes[i].spelling, v->annot->direction)) {
            sym->mode     = modes[i].mode;
            sym->has_mode = true;
            sym->direction = v->annot->direction;
            break;
        }
    }

    return true;
}

static bool
annot_pass_by(parse_visit_t *v)
{
    actx_t *a = v->thunk;
    parse_sym_entry_t *sym = subject(a, v);
    core_string_t mode = v->annot->pass_by;
    static const core_string_t allowed[] = {
        CORE_STRING_STATIC("in"),  CORE_STRING_STATIC("out"),
        CORE_STRING_STATIC("inout"), CORE_STRING_STATIC("ref"),
        CORE_STRING_STATIC("consume"), CORE_STRING_STATIC("view"),
    };

    if (!sym || !mode.data) return true;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (name_is(mode, allowed[i])) {
            sym->pass_by = mode;
            break;
        }
    }
    return true;
}

static bool
annot_exposes(parse_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    parse_sym_entry_t *sym = subject(a, v);

    parse_scope_t *scope = parse_tree_scope(v->node);
    if (sym && scope) {
        sym->exposed_scope = scope;
    }

    return true;
}

static bool
annot_visibility(parse_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    parse_sym_entry_t *sym = parse_tree_sym(v->node);

    if (v->annot->visibility_spec.u8_bytes > 0) {
        parse_sym_entry_t *parent_declaration = parse_tree_sym(v->parent);
        parse_namespace_t *ns = parse_symtab_ns(a->out->symtab, ns_default());
        parse_scope_t *scope = ns ? ns->current : nullptr;
        parse_sym_entry_t *field = nullptr;
        bool in_scope = false;
        if (scope && scope->node) {
            for (size_t distance = 0; ; distance++) {
                parse_tree_t *ancestor = parse_visit_ancestor(v, distance);
                if (!ancestor) break;
                if (!field) {
                    parse_sym_entry_t *candidate = parse_tree_sym(ancestor);
                    if (candidate && candidate->is_field) field = candidate;
                }
                if (ancestor == scope->node) {
                    in_scope = true;
                    break;
                }
            }
        }
        bool type_member = in_scope && field;
        if ((!parent_declaration ||
             parent_declaration->kind == PARSE_SYM_PARAM) && !type_member &&
            scope && scope->node)
            parent_declaration = parse_tree_sym(scope->node);
        if (type_member) {
            sym = field;
            parent_declaration = nullptr;
        }
        if (parent_declaration &&
            parent_declaration->kind != PARSE_SYM_PARAM)
            sym = parent_declaration;
        if (sym && sym->kind != PARSE_SYM_PARAM) {
            sym->visibility = v->annot->visibility_spec;
        }
        else {
            a->pending_visibility = v->annot->visibility_spec;
        }
    }

    return true;
}

static parse_cf_label_t *
cf_label(parse_visit_t *v, parse_cf_kind_t kind)
{
    actx_t         *a     = (actx_t *)v->thunk;
    parse_cf_label_t *label = core_alloc(parse_cf_label_t);

    memset(label, 0, sizeof(*label));
    label->kind = kind;
    label->self = v->node;

    bool found = false;
    parse_cf_label_t *previous = core_dict_get(a->out->cf_labels, v->node,
                                               &found);
    core_dict_put(a->out->cf_labels, v->node, label);
    core_list_t(parse_cf_label_ptr_t) *order = &a->out->cf_label_order;
    size_t at = 0;
    while (found && at < order->len && order->data[at] != previous) at++;
    if (found && at < order->len) order->data[at] = label;
    else core_list_push(*order, label);

    return label;
}

static parse_tree_t *
child(parse_visit_t *v, parse_child_ref_t ref)
{
    return parse_tree_resolve_child_ref(v->grammar, v->node, ref);
}

static bool
cf_branch(parse_visit_t *v)
{
    parse_cf_label_t *l = cf_label(v, PARSE_CF_BRANCH);

    l->cond      = child(v, v->annot->name_ref);
    l->then_body = child(v, v->annot->type_ref);
    l->else_body = child(v, v->annot->value_ref);

    return true;
}

static bool
cf_two_part(parse_visit_t *v, parse_cf_kind_t kind)
{
    parse_cf_label_t *l = cf_label(v, kind);

    l->cond      = child(v, v->annot->name_ref);
    l->then_body = child(v, v->annot->type_ref);

    return true;
}

static bool
cf_loop(parse_visit_t *v)
{
    return cf_two_part(v, PARSE_CF_LOOP);
}

static bool
cf_switch(parse_visit_t *v)
{
    return cf_two_part(v, PARSE_CF_SWITCH);
}

static bool
cf_call(parse_visit_t *v)
{
    return cf_two_part(v, PARSE_CF_CALL);
}

static bool
cf_jump(parse_visit_t *v)
{
    parse_cf_label_t *l = cf_label(v, PARSE_CF_JUMP);

    l->jump_kind = v->annot->scope_tag;

    return true;
}

static bool
cf_assigns(parse_visit_t *v)
{
    parse_cf_label_t *l = cf_label(v, PARSE_CF_ASSIGNS);

    l->cond      = child(v, v->annot->name_ref);
    l->then_body = child(v, v->annot->value_ref);
    parse_sym_entry_t *symbol = parse_tree_sym(v->node);
    if (symbol && !parse_sym_add_definition(symbol, v->node)) return false;

    return true;
}

static bool
cf_varref(parse_visit_t *v)
{
    actx_t           *a = (actx_t *)v->thunk;
    parse_tree_t *k = child(v, v->annot->name_ref);

    cf_label(v, PARSE_CF_VARREF)->cond = k;

    core_string_t nm = cached_or_leaf_name(k);

    if (!nm.u8_bytes) {
        return true;
    }
    parse_tree_set_unresolved_name(v->node, nm);

    parse_scope_t *scope = parse_tree_scope(v->node)
                           ? parse_tree_scope(v->node)
                           : parse_symtab_current_scope(a->out->symtab,
                                                      ns_default());
    parse_tree_set_scope(v->node, scope);
    parse_sym_entry_t *sym = parse_symtab_lookup_visible(a->out->symtab, scope,
                                                     ns_default(), nm, v->node);

    if (sym) {
        if (!parse_sym_add_use(sym, v->node)) return false;
        if (!parse_tree_set_resolution_symbol(v->node, sym)) return false;
        if (!parse_tree_mark_direct_symbol_reference(v->node)) return false;
    }
    else {
        core_list_push(a->out->undefined, v->node);
    }

    return true;
}

static bool
cf_operator(parse_visit_t *v)
{
    if (name_is(v->annot->op_kind, CORE_STRING_STATIC("unwrap_result"))) {
        cf_label(v, PARSE_CF_UNWRAP_RESULT);
    }

    return true;
}

static bool
cf_notrivia(parse_visit_t *v)
{
    parse_tree_t *target = child(v, v->annot->notrivia_ref);
    parse_token_info_t *tok    = target ? parse_tree_find_first_terminal(target)
                                      : nullptr;

    if (tok) {
        parse_trivia_free(parse_token_leading_trivia(tok));
        parse_trivia_free(parse_token_trailing_trivia(tok));
        parse_token_set_leading_trivia(tok, nullptr);
        parse_token_set_trailing_trivia(tok, nullptr);
        parse_token_set_no_trivia(tok, true);
    }

    return true;
}

parse_annot_result_t *
parse_annot_result_new(parse_symtab_t *borrowed)
{
    parse_annot_result_t *r = core_alloc(parse_annot_result_t);

    memset(r, 0, sizeof(*r));

    r->symtab          = borrowed ? borrowed : parse_symtab_new();
    r->borrowed_symtab = borrowed != nullptr;
    r->cf_labels       = core_alloc(core_dict_t);

    core_dict_init(r->cf_labels, core_hash_word, nullptr);
    r->cf_label_order = core_list_new(parse_cf_label_ptr_t);

    return r;
}

void
parse_annot_result_set_node_callback(parse_annot_result_t *result,
                                     parse_annot_node_callback_fn callback,
                                     void *context)
{
    if (!result) {
        return;
    }

    result->on_node = callback;
    result->on_node_context = context;
}

void
parse_annot_result_set_diagnostics(parse_annot_result_t *result,
                                   core_diagnostics_t *diagnostics,
                                   const core_translation_unit_t *unit)
{
    if (!result) return;
    result->diagnostics = diagnostics;
    result->diagnostic_unit = unit;
}

size_t
parse_annot_resolve_undefined(parse_annot_result_t *result)
{
    if (!result || !result->symtab) return 0;

    size_t resolved = 0;
    size_t retained = 0;
    for (size_t i = 0; i < result->undefined.len; i++) {
        parse_tree_t *node = result->undefined.data[i];
        parse_sym_entry_t *symbol = parse_tree_scope(node)
            ? parse_symtab_resolve_node(result->symtab, node, ns_default())
            : nullptr;
        if (symbol) {
            if (!parse_sym_add_use(symbol, node)) {
                result->undefined.data[retained++] = node;
                continue;
            }
            if (!parse_tree_mark_direct_symbol_reference(node)) {
                result->undefined.data[retained++] = node;
                continue;
            }
            resolved++;
        }
        else result->undefined.data[retained++] = node;
    }
    result->undefined.len = retained;
    return resolved;
}

static actx_t *
annot_context(parse_annot_result_t *out)
{
    actx_t *a = out->impl;

    if (!a) {
        a = core_alloc(actx_t);

        memset(a, 0, sizeof(*a));
        a->out    = out;
        out->impl = a;
    }
    a->last_sym = nullptr;
    a->pending_visibility = (core_string_t){0};
    a->pending_ns = (core_string_t){0};
    return a;
}

static void
register_binding(parse_tree_walk_t *w, actx_t *a)
{
    parse_walk_on_node(w, node_enter, node_leave, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_SCOPE_OPEN, PHASE_SCOPE,
                              scope_open, scope_close, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_NAMESPACE, PHASE_SCOPE,
                              annot_namespace, nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_DECLARES, PHASE_DECL,
                              annot_declares, nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_TYPE, PHASE_DECL, annot_type,
                              nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_FIELD, PHASE_DECL, annot_field,
                              nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_EXPOSES, PHASE_REFINE,
                              annot_exposes, nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_VISIBILITY, PHASE_REFINE,
                              annot_visibility, nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_DIRECTION, PHASE_REFINE,
                              annot_direction, nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_PASS_BY, PHASE_REFINE,
                              annot_pass_by, nullptr, a);
}

void
parse_annot_register_declarations(parse_tree_walk_t *w,
                                  parse_annot_result_t *out)
{
    if (!w || !out) return;

    actx_t *a = annot_context(out);
    a->reentry = a->declared_ahead;
    a->declared_ahead = true;

    register_binding(w, a);
}

void
parse_annot_register(parse_tree_walk_t *w, parse_annot_result_t *out)
{
    if (!w || !out) {
        return;
    }

    actx_t *a = annot_context(out);

    a->reentry = a->declared_ahead;

    register_binding(w, a);

    {
    parse_walk_on_annot_phase(w, PARSE_ANNOT_BRANCH, PHASE_CF, cf_branch, nullptr,
                            a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_LOOP, PHASE_CF, cf_loop, nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_SWITCH, PHASE_CF, cf_switch, nullptr,
                            a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_CALL, PHASE_CF, cf_call, nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_JUMP, PHASE_CF, cf_jump, nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_ASSIGNS, PHASE_CF, cf_assigns,
                            nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_VARREF, PHASE_CF, cf_varref, nullptr,
                            a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_OPERATOR, PHASE_CF, cf_operator,
                            nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_NOTRIVIA, PHASE_CF, cf_notrivia,
                            nullptr, a);
    }
}

void
parse_annot_result_free(parse_annot_result_t *r)
{
    if (!r) {
        return;
    }

    core_dict_foreach (r->cf_labels, e) {
        core_free(e->value);
    }

    core_dict_free(r->cf_labels);
    core_free(r->cf_labels);
    core_list_free(r->cf_label_order);

    if (!r->borrowed_symtab) {
        parse_symtab_free(r->symtab);
    }

    core_free(r->undefined.data);
    core_free(r->impl);

    core_free(r);
}

parse_cf_label_t *
parse_cf_label_lookup(parse_annot_result_t *r, parse_tree_t *node)
{
    if (!r || !node) {
        return nullptr;
    }

    bool  found = false;
    void *val   = core_dict_get(r->cf_labels, node, &found);

    return found ? (parse_cf_label_t *)val : nullptr;
}
