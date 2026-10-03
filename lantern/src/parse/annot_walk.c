#include "parse/annot_walk.h"
#include "parse/infer.h"
#include "ir/typelang.h"
#include "ir/layout.h"
#include "private/th/internal.h"
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
    PHASE_INFER  = 4,
};

core_list_decl(th_arg);
core_list_decl(th_member);

typedef struct {
    parse_annot_result_t *out;
    th_ctx             *types;
    parse_sym_entry_t    *last_sym;
    core_string_t        pending_visibility;
    core_string_t        pending_ns;
    ir_module_t        *module;
    int32_t             anon_counter;
} actx_t;

static bool link_type_members(parse_sym_entry_t *type, parse_scope_t *members);

static core_string_t
ns_default(void)
{
    return CORE_STRING_STATIC("");
}

static bool
name_is(core_string_t s, const char *want)
{
    size_t n = strlen(want);

    return s.data && s.u8_bytes == n && memcmp(s.data, want, n) == 0;
}

static core_string_t
ref_name(parse_visit_t *v, parse_child_ref_t ref)
{
    parse_tree_t *node = parse_tree_resolve_child_ref(v->grammar, v->node,
                                                        ref);

    return node ? parse_tree_extract_first_identifier(node) : (core_string_t){0};
}

static th_type *
type_of_child(actx_t *a, parse_visit_t *v, parse_child_ref_t ref)
{
    if (!a->types || !parse_child_ref_is_set(ref)) {
        return nullptr;
    }

    parse_tree_t *node = parse_tree_resolve_child_ref(v->grammar, v->node,
                                                        ref);

    return node ? parse_annot_resolve_type_node(a->out, a->types,
        v->grammar, node) : nullptr;
}

static bool
scope_open(parse_visit_t *v)
{
    actx_t           *a = (actx_t *)v->thunk;
    parse_annotation_t *an = v->annot;
    core_string_t      scope_name = an->scope_tag;

    if (parse_child_ref_is_set(an->name_ref)) {
        core_string_t resolved = ref_name(v, an->name_ref);

        if (resolved.u8_bytes > 0) {
            scope_name = resolved;
        }
    }

    parse_symtab_push_scope_all(a->out->symtab, scope_name);

    parse_scope_t *scope = parse_symtab_current_scope(a->out->symtab, ns_default());

    v->pn->scope     = scope;
    for (int32_t i = 0; i < a->out->symtab->ns_count; i++) {
        parse_scope_t *peer = parse_symtab_scope_in_namespace(
            a->out->symtab, scope, a->out->symtab->namespaces[i]->ns_name);
        if (peer) peer->scope_tag = an->scope_tag;
    }
    parse_symtab_set_scope_node_all(a->out->symtab, v->node);

    if (name_is(an->scope_tag, "function")) {
        parse_symtab_add(a->out->symtab, ns_default(),
                       CORE_STRING_STATIC("$return"), PARSE_SYM_VARIABLE,
                       v->node);
    }

    return true;
}

static bool
adt_scope_open(parse_visit_t *v)
{
    actx_t           *a  = (actx_t *)v->thunk;
    parse_annotation_t *an = v->annot;
    core_string_t      scope_name = ref_name(v, an->name_ref);
    char              buf[32];

    if (scope_name.u8_bytes == 0) {
        int n = snprintf(buf, sizeof(buf), "anon_%d", a->anon_counter++);

        scope_name = core_string_borrow(buf, n > 0 ? (size_t)n : 0);
    }

    parse_symtab_push_scope_all(a->out->symtab, scope_name);

    parse_scope_t *scope = parse_symtab_current_scope(a->out->symtab, ns_default());
    core_string_t kind  = an->adt_kind;

    if (parse_child_ref_is_set(an->adt_keyword_ref)) {
        core_string_t kw = ref_name(v, an->adt_keyword_ref);

        if (kw.u8_bytes > 0) {
            kind = kw;
        }
    }

    v->pn->scope     = scope;
    for (int32_t i = 0; i < a->out->symtab->ns_count; i++) {
        parse_scope_t *peer = parse_symtab_scope_in_namespace(
            a->out->symtab, scope, a->out->symtab->namespaces[i]->ns_name);
        if (!peer) continue;
        peer->adt_kind = kind;
        peer->scope_tag = an->scope_tag.data ? an->scope_tag : an->adt_kind;
    }

    parse_symtab_set_scope_node_all(a->out->symtab, v->node);

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

    if (a->out->on_node) {
        a->out->on_node(v, true, a->out->on_node_context);
    }

    return true;
}

static bool
node_leave(parse_visit_t *v)
{
    actx_t *a = (actx_t *)v->thunk;

    parse_sym_entry_t *sym = v->pn ? v->pn->sym : nullptr;
    if (sym && sym->kind == PARSE_SYM_TYPEDEF && sym->member_scope
        && !link_type_members(sym, sym->member_scope)) {
        a->out->inference_failed = true;
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
    if (!parse_tree_set_resolution_symbol(v->node, sym))
        a->out->inference_failed = true;
}

static parse_sym_entry_t *
subject(actx_t *a, parse_visit_t *v)
{
    return v->pn->sym ? v->pn->sym : a->last_sym;
}

static parse_sym_kind_t
sym_kind_of(core_string_t spec)
{
    if (name_is(spec, "param")) {
        return PARSE_SYM_PARAM;
    }

    if (name_is(spec, "function")) {
        return PARSE_SYM_FUNCTION;
    }

    if (name_is(spec, "module")) {
        return PARSE_SYM_MODULE;
    }

    return PARSE_SYM_VARIABLE;
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
    bool scope_is_decl_body = v->pn->scope && decl_scope
                           && decl_scope->node == v->node
                           && decl_scope->parent;

    if (scope_is_decl_body) {
        parse_symtab_enter_scope(a->out->symtab,
                                 decl_scope->ns->ns_name,
                                 decl_scope->parent);
    }

    parse_sym_entry_t *sym  = parse_symtab_add(a->out->symtab, namespace_name, nm,
                                           kind, v->node);

    if (scope_is_decl_body) {
        parse_symtab_enter_scope(a->out->symtab,
                                 decl_scope->ns->ns_name, decl_scope);
    }

    sym->mutability = kind == PARSE_SYM_PARAM ? PARSE_SYM_IMMUTABLE
                                            : PARSE_SYM_MUTABLE;
    sym->type       = type_of_child(a, v, an->type_ref);

    if (a->pending_visibility.data && kind != PARSE_SYM_PARAM) {
        sym->visibility       = a->pending_visibility;
        a->pending_visibility = (core_string_t){0};
    }

    if (sym->shadowed) {
        core_list_push(a->out->shadowed, sym);
    }

    if (kind == PARSE_SYM_PARAM) {
        core_list_push(a->out->params, sym);
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
annot_type_decl(parse_visit_t *v)
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
    bool scope_is_decl_body = v->pn->scope && decl_scope
                           && decl_scope->node == v->node
                           && decl_scope->parent;

    if (scope_is_decl_body) {
        parse_symtab_enter_scope(a->out->symtab, namespace_name,
                                 decl_scope->parent);
    }

    parse_sym_entry_t *sym = parse_symtab_add(a->out->symtab, namespace_name,
                                          nm, PARSE_SYM_TYPEDEF, v->node);

    if (scope_is_decl_body) {
        parse_symtab_enter_scope(a->out->symtab, namespace_name, decl_scope);
    }

    sym->type = type_of_child(a, v, an->name_ref);
    parse_scope_t *members = decl_scope && decl_scope->lexical_scope
        ? decl_scope->lexical_scope : decl_scope;
    if (members && !link_type_members(sym, members)) return false;

    declared(a, v, sym);

    return true;
}

static bool
annot_adt(parse_visit_t *v)
{
    actx_t           *a  = (actx_t *)v->thunk;
    parse_annotation_t *an = v->annot;
    core_string_t      nm = ref_name(v, an->name_ref);

    if (nm.u8_bytes == 0) {
        return true;
    }

    parse_sym_entry_t *sym = parse_symtab_add(a->out->symtab,
                                          CORE_STRING_STATIC("tag"), nm,
                                          PARSE_SYM_TAG, v->node);
    core_string_t     kind = an->adt_kind;

    if (parse_child_ref_is_set(an->adt_keyword_ref)) {
        core_string_t kw = ref_name(v, an->adt_keyword_ref);

        if (kw.u8_bytes > 0) {
            kind = kw;
        }
    }

    sym->adt_kind = kind;

    declared(a, v, sym);

    return true;
}

static bool
member_decl(parse_visit_t *v, bool is_method)
{
    actx_t           *a  = (actx_t *)v->thunk;
    parse_annotation_t *an = v->annot;
    core_string_t      nm = ref_name(v, an->name_ref);

    if (nm.u8_bytes == 0) {
        return true;
    }

    parse_sym_entry_t *sym = parse_symtab_add(
        a->out->symtab, ns_default(), nm,
        is_method ? PARSE_SYM_FUNCTION : PARSE_SYM_VARIABLE, v->node);

    sym->is_field  = !is_method;
    sym->is_method = is_method;
    sym->type = type_of_child(a, v, an->type_ref);
    if (a->pending_visibility.data) {
        sym->visibility = a->pending_visibility;
        a->pending_visibility = (core_string_t){0};
    }

    parse_scope_t *member_scope = v->pn->scope;
    parse_sym_entry_t *owner = member_scope && member_scope->node
        && !core_tree_is_leaf(member_scope->node)
        ? core_tree_node_value(member_scope->node).sym : nullptr;
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
annot_field(parse_visit_t *v)
{
    return member_decl(v, false);
}

static bool
annot_method(parse_visit_t *v)
{
    return member_decl(v, true);
}

static bool
annot_mutability(parse_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    parse_sym_entry_t *sym = subject(a, v);
    core_string_t     m   = v->annot->mutability;

    if (!sym || !m.data) {
        return true;
    }

    if (name_is(m, "immutable")) {
        sym->mutability = PARSE_SYM_IMMUTABLE;
    }
    else if (name_is(m, "const")) {
        sym->mutability = PARSE_SYM_CONST;
    }
    else {
        sym->mutability = PARSE_SYM_MUTABLE;
    }

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
    static const char *const allowed[] = {"in", "out", "inout", "ref"};

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
annot_constrain(parse_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    parse_sym_entry_t *sym = v->pn->sym;

    if (core_list_len(v->annot->constraints) == 0) {
        return true;
    }

    if (!a->types) {
        return true;
    }

    th_type **target = sym ? &sym->type : &v->pn->type;
    if (!*target) *target = th_var(a->types);

    for (size_t i = 0; i < core_list_len(v->annot->constraints); i++) {
        parse_con_spec_t *s = &v->annot->constraints.data[i];
        th_constraint   c = {.kind = s->kind};
        size_t          nt = core_list_len(s->types);

        switch (s->kind) {
        case TH_CON_UNIFIES:
        case TH_CON_EXCLUDES:
        case TH_CON_PROMOTES:
            if (nt != 1) {
                continue;
            }
            c.target = ir_type_from_text(a->types, s->types.data[0]);
            break;

        case TH_CON_ONE_OF:
            if (nt == 0) {
                continue;
            }
            c.one_of.types = core_alloc_size(nt, sizeof(th_type *));
            c.one_of.n     = nt;

            for (size_t j = 0; j < nt; j++) {
                c.one_of.types[j] = ir_type_from_text(a->types,
                                                      s->types.data[j]);
            }
            break;

        case TH_CON_HAS_FIELD:
            if (nt != 1) {
                continue;
            }
            c.field.name = s->name.data;
            c.field.type = ir_type_from_text(a->types, s->types.data[0]);
            break;

        case TH_CON_HAS_PARAM:
            if (nt != 1) {
                continue;
            }
            c.param.index = s->index;
            c.param.type  = ir_type_from_text(a->types, s->types.data[0]);
            break;

        case TH_CON_IMPLEMENTS:
            c.iface = s->name.data;
            break;

        case TH_CON_METHOD:
            /* This obligation is generated by inference, not annotations. */
            continue;
        }

        th_var_constrain(a->types, *target, c);
    }

    return true;
}

static bool
annot_repr(parse_visit_t *v)
{
    actx_t           *a  = (actx_t *)v->thunk;
    parse_annotation_t *an = v->annot;
    int64_t           val = 0;
    bool              have = false;

    if (an->has_val_int) {
        val  = an->val_int;
        have = true;
    }
    else if (parse_child_ref_is_set(an->val_ref)) {
        parse_tree_t *k = parse_tree_resolve_child_ref(v->grammar, v->node,
                                                         an->val_ref);
        parse_token_info_t *t = k ? parse_tree_find_first_terminal(k) : nullptr;

        if (t && core_option_is_set(t->value)) {
            core_string_t s = core_option_get(t->value);

            if (s.data) {
                val  = strtoll(s.data, nullptr, 10);
                have = true;
            }
        }
    }

    if (!have) {
        return true;
    }

    if (!a->module) {
        return true;
    }

    switch (an->kind) {
    case PARSE_ANNOT_SIZE:
        if (v->pn->type) {
            ir_layout_pin_size(a->module, v->pn->type, (uint64_t)val);
        }
        break;

    case PARSE_ANNOT_ALIGNMENT:
        if (v->pn->type) {
            ir_layout_pin_align(a->module, v->pn->type, (uint64_t)val);
        }
        break;

    case PARSE_ANNOT_OFFSET:
        if (v->pn->sym) {
            v->pn->sym->bit_offset     = (uint64_t)val;
            v->pn->sym->has_bit_offset = true;
        }
        break;

    case PARSE_ANNOT_WIDTH:
        if (v->pn->sym) {
            v->pn->sym->bit_width     = (uint64_t)val;
            v->pn->sym->has_bit_width = true;
        }
        break;

    default:
        break;
    }

    return true;
}

static bool
annot_literal(parse_visit_t *v)
{
    actx_t           *a  = (actx_t *)v->thunk;
    parse_annotation_t *an = v->annot;

    if (!a->types) {
        return true;
    }

    th_type *t = type_of_child(a, v, an->type_ref);

    if (!t && an->op_kind.data && an->op_kind.u8_bytes) {
        t = th_name(a->types, an->op_kind.data);
    }

    if (!t) {
        return true;
    }

    if (v->pn->type) {
        th_type *unified = th_unify(a->types, v->pn->type, t);
        if (unified) {
            v->pn->type = unified;
        }
    }
    else {
        v->pn->type = t;
    }

    return true;
}

static bool
annot_adt_close(parse_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    parse_sym_entry_t *sym = v->pn->sym;

    if (!a->types || !v->pn->scope) {
        return true;
    }

    core_list_t(th_arg) arms = core_list_new(th_arg);

    for (parse_sym_entry_t *e = v->pn->scope->first_in_scope; e;
         e = e->next_in_scope) {
        if (!e->type) {
            e->type = th_var(a->types);
        }

        core_list_push(arms, ((th_arg){.kind = TH_ARG_TYPE, .type = e->type}));
    }

    core_string_t nm = sym && sym->name.data ? sym->name
                                            : v->pn->scope->name;

    if (!nm.data) {
        core_list_free(arms);

        return true;
    }

    th_type *var = th_variant(a->types, nm.data, arms.data, arms.len);

    core_list_free(arms);

    if (!var) {
        return true;
    }

    v->pn->type = var;

    if (sym) {
        sym->type = var;
    }

    return true;
}

static bool
collect_record_members(actx_t *a, parse_tree_t *node, parse_scope_t *scope,
                       core_list_t(th_member) *members)
{
    if (!node || core_tree_is_leaf(node)) return true;
    parse_sym_entry_t *field = core_tree_node_value(node).sym;
    if (field && field->is_field && field->scope == scope) {
        if (!field->type) field->type = th_var(a->types);
        core_list_push(*members, ((th_member){
            .name = field->name.data,
            .type = field->type,
        }));
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!collect_record_members(a, core_tree_child(node, i), scope,
                                    members)) return false;
    return true;
}

static bool
annot_record(parse_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    parse_sym_entry_t *sym = subject(a, v);
    core_string_t     nm  = ref_name(v, v->annot->name_ref);

    if (!a->types) {
        return true;
    }

    core_list_t(th_member) members = core_list_new(th_member);
    collect_record_members(a, v->node, v->pn->scope, &members);

    th_type *rec = th_record(a->types, members.data, members.len);

    core_list_free(members);

    if (!rec) {
        return true;
    }

    v->pn->type = rec;

    if (sym) {
        sym->type = rec;
        if (v->pn->scope && !link_type_members(sym, v->pn->scope))
            return false;
    }

    for (parse_scope_t *s = v->pn->scope; s; s = nullptr) {
        for (parse_sym_entry_t *e = s->first_in_scope; e;
             e = e->next_in_scope) {
            if (!e->is_method || !e->name.data) {
                continue;
            }

            if (!e->type) {
                e->type = th_var(a->types);
            }

            th_register_method(a->types, rec, e->name.data, e->type);
        }
    }

    if (a->module) {
        for (parse_scope_t *s = v->pn->scope; s; s = nullptr) {
            for (parse_sym_entry_t *e = s->first_in_scope; e;
                 e = e->next_in_scope) {
                if (!e->is_field
                    || !(e->has_bit_offset || e->has_bit_width)) {
                    continue;
                }

                ir_layout_pin_member(a->module, rec, e->name.data,
                                     e->bit_offset, e->bit_width);
            }
        }
    }

    if (nm.u8_bytes && !sym) {
        parse_scope_t *inner = v->pn->scope;
        parse_scope_t *outer = inner ? inner->parent : nullptr;

        if (outer) {
            parse_symtab_enter_scope_all(a->out->symtab, outer);
        }

        parse_sym_entry_t *bound = parse_symtab_add(a->out->symtab, ns_default(),
                                                nm, PARSE_SYM_TYPEDEF,
                                                v->node);

        if (outer) {
            parse_symtab_enter_scope_all(a->out->symtab, inner);
        }

        if (bound) {
            bound->type = rec;
            if (inner && !link_type_members(bound, inner)) return false;
            declared(a, v, bound);
        }
    }

    return true;
}

static bool
annot_type(parse_visit_t *v)
{
    actx_t           *a   = (actx_t *)v->thunk;
    parse_annotation_t *an  = v->annot;
    parse_sym_entry_t  *sym = subject(a, v);

    if (!sym) {
        return true;
    }

    if (an->type_spec.data && a->types) {
        sym->type = ir_type_from_text(a->types, an->type_spec);
    }
    else {
        th_type *t = type_of_child(a, v, an->name_ref);

        if (t) {
            sym->type = t;
        }
    }

    return true;
}

static bool
annot_exposes(parse_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    parse_sym_entry_t *sym = subject(a, v);

    if (sym && v->pn->scope) {
        sym->exposed_scope = v->pn->scope;
    }

    return true;
}

static bool
annot_inherits(parse_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    core_string_t     nm  = ref_name(v, v->annot->name_ref);
    parse_sym_entry_t *sym = subject(a, v);

    if (sym && nm.u8_bytes > 0) {
        sym->inherits_name = nm;
    }

    return true;
}

static bool
annot_implements(parse_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    core_string_t     nm  = ref_name(v, v->annot->name_ref);
    parse_sym_entry_t *sym = subject(a, v);

    if (!sym || nm.u8_bytes == 0 || !a->types || !sym->type) {
        return true;
    }

    if (!th_implements(a->types, sym->type, nm.data)) {
        th_err(a->types, TH_ERR_CONSTRAINT, "implements", sym->type,
               nullptr, "the declared type does not satisfy the interface");
    }

    return true;
}

static bool
field_symbol_for_node(parse_tree_t *root, parse_tree_t *target,
                      parse_sym_entry_t *field,
                      parse_sym_entry_t **result)
{
    if (!root) return false;
    if (!core_tree_is_leaf(root)) {
        parse_sym_entry_t *symbol = core_tree_node_value(root).sym;
        if (symbol && symbol->is_field) field = symbol;
    }
    if (root == target) {
        *result = field;
        return true;
    }
    if (core_tree_is_leaf(root)) return false;
    for (size_t i = 0; i < core_tree_num_children(root); i++)
        if (field_symbol_for_node(core_tree_child(root, i), target, field,
                                  result)) return true;
    return false;
}

static bool
annot_visibility(parse_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    parse_sym_entry_t *sym = v->pn->sym;

    if (v->annot->visibility_spec.u8_bytes > 0) {
        parse_sym_entry_t *parent_declaration = v->parent &&
            !core_tree_is_leaf(v->parent)
            ? core_tree_node_value(v->parent).sym : nullptr;
        parse_namespace_t *ns = parse_symtab_ns(a->out->symtab, ns_default());
        parse_scope_t *scope = ns ? ns->current : nullptr;
        parse_sym_entry_t *field = nullptr;
        bool type_member = scope && scope->node
            && core_string_eq(scope->scope_tag,
                              CORE_STRING_STATIC("type"))
            && field_symbol_for_node(scope->node, v->node, nullptr, &field)
            && field;
        if ((!parent_declaration ||
             parent_declaration->kind == PARSE_SYM_PARAM) && !type_member &&
            scope && scope->node)
            parent_declaration = core_tree_node_value(scope->node).sym;
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

static bool
annot_static(parse_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    parse_sym_entry_t *sym = subject(a, v);

    if (sym) {
        sym->is_static = true;
    }

    return true;
}

static bool
annot_abstract(parse_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    parse_sym_entry_t *sym = subject(a, v);

    if (sym) {
        sym->is_abstract = true;
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

    core_dict_put(a->out->cf_labels, v->node, label);

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

    if (!l->jump_kind.data) {
        l->jump_kind = parse_tree_extract_first_identifier(v->node);
    }

    return true;
}

static bool
cf_capture(parse_visit_t *v)
{
    parse_cf_label_t *l = cf_label(v, PARSE_CF_CAPTURE);

    l->tag            = v->annot->scope_tag;
    l->capture_by_tag = v->annot->capture_by_tag;

    return true;
}

static bool
cf_assigns(parse_visit_t *v)
{
    parse_cf_label_t *l = cf_label(v, PARSE_CF_ASSIGNS);

    l->cond      = child(v, v->annot->name_ref);
    l->then_body = child(v, v->annot->value_ref);
    parse_sym_entry_t *symbol = v->pn ? v->pn->sym : nullptr;
    if (symbol && !parse_sym_add_definition(symbol, v->node)) return false;

    return true;
}

static bool
cf_varref(parse_visit_t *v)
{
    actx_t           *a = (actx_t *)v->thunk;
    parse_tree_t *k = child(v, v->annot->name_ref);

    cf_label(v, PARSE_CF_VARREF)->cond = k;

    core_string_t nm = k ? parse_tree_extract_first_identifier(k)
                        : (core_string_t){0};

    if (!nm.u8_bytes) {
        return true;
    }

    parse_scope_t *scope = v->pn->scope
                           ? v->pn->scope
                           : parse_symtab_current_scope(a->out->symtab,
                                                      ns_default());
    v->pn->scope = scope;
    parse_sym_entry_t *sym = parse_symtab_lookup_scoped(a->out->symtab, scope,
                                                    ns_default(), nm);

    if (sym) {
        if (!parse_sym_add_use(sym, v->node)) return false;
        if (!parse_tree_set_resolution_symbol(v->node, sym)) return false;
    }
    else {
        core_list_push(a->out->undefined, v->node);
    }

    return true;
}

static bool
cf_operator(parse_visit_t *v)
{
    if (name_is(v->annot->op_kind, "unwrap_result")) {
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
        parse_trivia_free(tok->leading_trivia);
        parse_trivia_free(tok->trailing_trivia);
        tok->leading_trivia  = nullptr;
        tok->trailing_trivia = nullptr;
        tok->no_trivia       = true;
    }

    return true;
}

static void
annot_report_infer_failure(parse_visit_t *v, actx_t *a)
{
    a->out->inference_failed = true;
    if (!a->out->diagnostics) return;
    parse_token_info_t *token = parse_tree_find_first_terminal(v->node);
    core_diagnostics_report_at(a->out->diagnostics,
        a->out->diagnostic_unit, CORE_DIAG_FATAL,
        CORE_STRING_STATIC("LANTERN-T0001"),
        CORE_STRING_STATIC("type inference failed"),
        token ? (core_diag_location_t){
            .line = token->line, .column = token->column,
            .end_line = token->endline,
            .end_column = token->endcol, .has_location = true,
        } : (core_diag_location_t){0});
}

static bool
annot_infer(parse_visit_t *v)
{
    actx_t           *a  = (actx_t *)v->thunk;
    parse_annotation_t *an = v->annot;

    if (!a->types || !an->infer_ast) {
        return true;
    }

    bool     ok = false;
    th_type *t = parse_infer_eval_with_type_node_resolver(an->infer_ast, v,
        a->types, a->out->symtab, a->out->type_node_resolver,
        a->out->type_node_resolver_context, &ok);

    if (!ok || !t) {
        annot_report_infer_failure(v, a);
        return true;
    }

    if (parse_child_ref_is_set(an->name_ref)) {
        parse_tree_t *k = parse_tree_resolve_child_ref(v->grammar, v->node,
                                                         an->name_ref);

        if (k && !core_tree_is_leaf(k)) {
            parse_nt_node_t *pn = &core_tree_node_value(k);

            if (pn->type) {
                th_type *unified = th_unify_in_place(a->types, pn->type, t);
                if (unified) {
                    pn->type = unified;
                }
            }
            else {
                pn->type = t;
            }
        }

        return true;
    }

    if (v->pn->type) {
        th_type *unified = th_unify_in_place(a->types, v->pn->type, t);
        if (unified) {
            v->pn->type = unified;
            t = unified;
        }
    }
    else {
        v->pn->type = t;
    }

    if (v->pn->sym && v->pn->sym->kind == PARSE_SYM_FUNCTION)
        return true;

    if (v->pn->sym
        && (!v->pn->sym->type || !th_is_scheme(v->pn->sym->type))) {
        if (v->pn->sym->type) {
            th_type *unified = th_unify_in_place(a->types,
                                                 v->pn->sym->type,
                                                 v->pn->type);
            if (unified) {
                v->pn->sym->type = unified;
                v->pn->type = unified;
            }
        }
        else {
            v->pn->sym->type = v->pn->type;
        }
    }

    return true;
}

static bool
resolve_symbol_use_type(th_ctx *types, parse_sym_entry_t *symbol,
                        th_type **out)
{
    if (!types || !symbol || !symbol->type || !out) return false;
    if (th_is_scheme(symbol->type)) {
        *out = th_instantiate(types, symbol->type);
        return true;
    }
    if (symbol->kind != PARSE_SYM_FUNCTION) {
        *out = symbol->type;
        return true;
    }
    size_t capacity = 0;
    for (parse_sym_entry_t *entry = symbol->scope
            ? symbol->scope->first_in_scope : nullptr;
         entry;
         entry = entry->next_in_scope) {
        if (entry->kind == PARSE_SYM_FUNCTION) continue;
        capacity += (entry->type != nullptr) + (entry->error_type != nullptr);
    }
    th_type **environment = capacity
        ? core_alloc_array(th_type *, capacity) : nullptr;
    size_t count = 0;
    for (parse_sym_entry_t *entry = symbol->scope
            ? symbol->scope->first_in_scope : nullptr;
         entry;
         entry = entry->next_in_scope) {
        if (entry->kind == PARSE_SYM_FUNCTION) continue;
        if (entry->type) environment[count++] = entry->type;
        if (entry->error_type) environment[count++] = entry->error_type;
    }
    th_type *scheme = th_generalize_excluding(types, symbol->type,
                                               environment, count);
    core_free(environment);
    *out = th_instantiate(types, scheme);
    return true;
}

static bool
infer_function_reference(actx_t *a, parse_nt_node_t *node,
                         parse_sym_entry_t *symbol)
{
    if (node->type) return true;
    resolve_symbol_use_type(a->types, symbol, &node->type);
    return true;
}

static bool
infer_propagate(parse_visit_t *v)
{
    actx_t *a = (actx_t *)v->thunk;

    if (!a->types) {
        return true;
    }

    for (size_t i = 0; i < parse_nt_annot_count(v->nt); i++) {
        parse_annotation_t *annotation = parse_nt_annot_at(v->nt, i);
        if (annotation && annotation->kind == PARSE_ANNOT_INFER)
            return true;
    }
    if (parse_rule_find_annot(v->grammar, v->rule, PARSE_ANNOT_INFER))
        return true;

    if (v->pn->sym && v->pn->sym->type) {
        if (v->pn->sym->kind == PARSE_SYM_FUNCTION)
            return infer_function_reference(a, v->pn, v->pn->sym);
        if (th_is_scheme(v->pn->sym->type)) {
            if (!v->pn->type)
                v->pn->type = th_instantiate(a->types, v->pn->sym->type);
            return true;
        }

        if (v->pn->type) {
            th_type *unified = th_unify_in_place(a->types, v->pn->type,
                                                 v->pn->sym->type);
            if (unified) {
                v->pn->type = unified;
                v->pn->sym->type = unified;
            }
        }
        else {
            v->pn->type = v->pn->sym->type;
        }

        return true;
    }

    parse_tree_t *sole = nullptr;
    size_t            nc   = core_tree_num_children(v->node);

    for (size_t i = 0; i < nc; i++) {
        parse_tree_t *k = core_tree_child(v->node, i);

        if (!k || core_tree_is_leaf(k)) {
            continue;
        }

        if (sole) {
            return true;
        }

        sole = k;
    }

    if (sole) {
        th_type *child_type = core_tree_node_value(sole).type;
        if (child_type && v->pn->type) {
            th_type *unified = th_unify_in_place(a->types, v->pn->type,
                                                 child_type);
            if (unified) v->pn->type = unified;
        }
        else if (child_type) v->pn->type = child_type;
    }

    return true;
}

static bool
scope_reenter(parse_visit_t *v)
{
    actx_t *a = (actx_t *)v->thunk;

    if (v->pn->scope) {
        parse_symtab_enter_scope_all(a->out->symtab, v->pn->scope);
    }

    return true;
}

static bool
scope_leave(parse_visit_t *v)
{
    actx_t *a = (actx_t *)v->thunk;

    if (v->pn->scope) {
        parse_symtab_leave_scope_all(a->out->symtab);
        a->last_sym = nullptr;
    }

    return true;
}

core_string_t
parse_annot_pass_name(parse_annot_pass_t p)
{
    switch (p) {
    case PARSE_ANNOT_PASS_SCOPE:
        return CORE_STRING_STATIC("annot.scope");
    case PARSE_ANNOT_PASS_SYMBOLS:
        return CORE_STRING_STATIC("annot.symbols");
    case PARSE_ANNOT_PASS_INITIAL_SYMBOLS:
        return CORE_STRING_STATIC("annot.initial-symbols");
    case PARSE_ANNOT_PASS_REFINE:
        return CORE_STRING_STATIC("annot.refine");
    case PARSE_ANNOT_PASS_CFLOW:
        return CORE_STRING_STATIC("annot.cflow");
    case PARSE_ANNOT_PASS_INFER:
        return CORE_STRING_STATIC("annot.infer");
    case PARSE_ANNOT_PASS_ALL:
        return CORE_STRING_STATIC("annot.all");
    }

    return CORE_STRING_STATIC("?");
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
parse_annot_result_set_type_node_resolver(
    parse_annot_result_t *result,
    parse_infer_type_node_resolver_fn resolver, void *context)
{
    if (!result) return;
    result->type_node_resolver = resolver;
    result->type_node_resolver_context = context;
}

th_type *
parse_annot_resolve_type_node(parse_annot_result_t *result, th_ctx *types,
                              parse_grammar_t *grammar, parse_tree_t *node)
{
    if (!types || !grammar || !node) return nullptr;
    th_type *resolved = result && result->type_node_resolver
        ? result->type_node_resolver(types, grammar, node,
              result->type_node_resolver_context) : nullptr;
    return resolved ? resolved : core_tree_node_value(node).type;
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
        parse_nt_node_t *pn = node && !core_tree_is_leaf(node)
            ? &core_tree_node_value(node) : nullptr;
        parse_sym_entry_t *symbol = pn && pn->scope
            ? parse_symtab_resolve_node(result->symtab, node, ns_default())
            : nullptr;
        if (symbol) {
            if (symbol->type && result->impl) {
                actx_t *a = result->impl;
                if (a->types) {
                    th_type *symbol_type = nullptr;
                    resolve_symbol_use_type(a->types, symbol,
                                            &symbol_type);
                    if (pn->type) {
                        th_type *unified = th_unify_in_place(
                            a->types, pn->type, symbol_type);
                        if (unified) pn->type = unified;
                    }
                    else pn->type = symbol_type;
                }
            }
            if (!parse_sym_add_use(symbol, node)) {
                result->inference_failed = true;
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

void
parse_annot_register_pass(parse_tree_walk_t    *w,
                        parse_annot_result_t *out,
                        th_ctx             *types,
                        ir_module_t        *m,
                        parse_annot_pass_t    which)
{
    if (!w || !out) {
        return;
    }

    actx_t *a = out->impl;

    if (!a) {
        a = core_alloc(actx_t);

        memset(a, 0, sizeof(*a));
        a->out    = out;
        out->impl = a;
    }

    a->types  = types;
    a->module = m;

    bool all = which == PARSE_ANNOT_PASS_ALL;
    bool initial_symbols = which == PARSE_ANNOT_PASS_INITIAL_SYMBOLS;

    parse_walk_on_node(w, node_enter, node_leave, a);

    if (all || initial_symbols || which == PARSE_ANNOT_PASS_SCOPE) {
        parse_walk_on_annot_phase(w, PARSE_ANNOT_SCOPE_OPEN, PHASE_SCOPE,
                                scope_open, scope_close, a);
        parse_walk_on_annot_phase(w, PARSE_ANNOT_ADT, PHASE_SCOPE, adt_scope_open,
                                scope_close, a);
    }
    else {
        parse_walk_on_annot_phase(w, PARSE_ANNOT_SCOPE_OPEN, PHASE_SCOPE,
                                scope_reenter, scope_leave, a);
        parse_walk_on_annot_phase(w, PARSE_ANNOT_ADT, PHASE_SCOPE, scope_reenter,
                                scope_leave, a);
    }

    bool symbols = all || initial_symbols || which == PARSE_ANNOT_PASS_SYMBOLS;
    bool refine  = all || initial_symbols || which == PARSE_ANNOT_PASS_REFINE;
    bool cflow   = all || which == PARSE_ANNOT_PASS_CFLOW;

    if (all || initial_symbols || which == PARSE_ANNOT_PASS_INFER) {
        parse_walk_on_node(w, nullptr, infer_propagate, a);
        parse_walk_on_annot_phase(w, PARSE_ANNOT_INFER, PHASE_INFER, nullptr,
                                annot_infer, a);
        parse_walk_on_annot_phase(w, PARSE_ANNOT_SIZE, PHASE_INFER, annot_repr,
                                nullptr, a);
        parse_walk_on_annot_phase(w, PARSE_ANNOT_ALIGNMENT, PHASE_INFER,
                                annot_repr, nullptr, a);
    }

    if (symbols) {
    parse_walk_on_annot_phase(w, PARSE_ANNOT_NAMESPACE, PHASE_SCOPE,
                            annot_namespace, nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_DECLARES, PHASE_DECL, annot_declares,
                            nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_TYPE_DECL, PHASE_DECL,
                            annot_type_decl, nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_ADT, PHASE_REFINE, nullptr,
                            annot_adt_close, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_ADT, PHASE_DECL, annot_adt, nullptr,
                            a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_FIELD, PHASE_DECL, annot_field,
                            nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_METHOD, PHASE_DECL, annot_method,
                            nullptr, a);
    }

    if (refine) {
    parse_walk_on_annot_phase(w, PARSE_ANNOT_TYPE, PHASE_REFINE, annot_type,
                            nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_EXPOSES, PHASE_REFINE, annot_exposes,
                            nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_INHERITS, PHASE_REFINE,
                            annot_inherits, nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_IMPLEMENTS, PHASE_REFINE,
                            annot_implements, nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_VISIBILITY, PHASE_REFINE,
                            annot_visibility, nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_STATIC, PHASE_REFINE, annot_static,
                            nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_ABSTRACT, PHASE_REFINE,
                            annot_abstract, nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_LITERAL, PHASE_REFINE, nullptr,
                            annot_literal, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_RECORD, PHASE_REFINE, nullptr,
                            annot_record, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_MUTABILITY, PHASE_REFINE,
                            annot_mutability, nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_DIRECTION, PHASE_REFINE,
                            annot_direction, nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_PASS_BY, PHASE_REFINE,
                            annot_pass_by, nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_CONSTRAIN, PHASE_REFINE,
                            annot_constrain, nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_OFFSET, PHASE_REFINE, annot_repr,
                            nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_WIDTH, PHASE_REFINE, annot_repr,
                            nullptr, a);
    }

    if (cflow) {
    parse_walk_on_annot_phase(w, PARSE_ANNOT_BRANCH, PHASE_CF, cf_branch, nullptr,
                            a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_LOOP, PHASE_CF, cf_loop, nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_SWITCH, PHASE_CF, cf_switch, nullptr,
                            a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_CALL, PHASE_CF, cf_call, nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_JUMP, PHASE_CF, cf_jump, nullptr, a);
    parse_walk_on_annot_phase(w, PARSE_ANNOT_CAPTURE, PHASE_CF, cf_capture,
                            nullptr, a);
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
parse_annot_register(parse_tree_walk_t    *w,
                   parse_annot_result_t *out,
                   th_ctx             *types,
                   ir_module_t        *m)
{
    parse_annot_register_pass(w, out, types, m, PARSE_ANNOT_PASS_ALL);
}

parse_annot_result_t *
parse_annot_walk_tree(parse_grammar_t    *g,
                    parse_tree_t *tree,
                    th_ctx           *types,
                    ir_module_t      *m)
{
    if (!g || !tree) {
        return nullptr;
    }

    parse_annot_result_t *r = parse_annot_result_new(nullptr);
    parse_tree_walk_t    *w = parse_tree_walk_new(g, r);

    parse_annot_register(w, r, types, m);
    parse_tree_walk_run(w, tree);
    parse_tree_walk_free(w);

    return r;
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

    if (!r->borrowed_symtab) {
        parse_symtab_free(r->symtab);
    }

    core_free(r->params.data);
    core_free(r->shadowed.data);
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
