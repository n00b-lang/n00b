#include "parse/annot_walk.h"
#include "parse/infer.h"
#include "ir/typelang.h"
#include "parse/tree_util.h"
#include "internal/parse/grammar_internal.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/hash.h"

#include <stdio.h>
#include <string.h>

enum {
    PHASE_SCOPE  = 0,
    PHASE_DECL   = 1,
    PHASE_REFINE = 2,
    PHASE_CF     = 3,
    PHASE_INFER  = 4,
};

enum {
    NCC_RECORD_MAX_FIELDS = 64,
};

typedef struct {
    ncc_annot_result_t *out;
    th_ctx             *types;
    ncc_sym_entry_t    *last_sym;
    ncc_string_t        pending_visibility;
    ncc_string_t        pending_ns;
    int32_t             anon_counter;
} actx_t;

static ncc_string_t
ns_default(void)
{
    return NCC_STRING_STATIC("");
}

static bool
name_is(ncc_string_t s, const char *want)
{
    size_t n = strlen(want);

    return s.data && s.u8_bytes == n && memcmp(s.data, want, n) == 0;
}

static ncc_string_t
ref_name(ncc_visit_t *v, ncc_child_ref_t ref)
{
    ncc_parse_tree_t *node = ncc_tree_resolve_child_ref(v->grammar, v->node,
                                                        ref);

    return node ? ncc_tree_extract_first_identifier(node) : (ncc_string_t){0};
}

static th_type *
type_of_child(actx_t *a, ncc_visit_t *v, ncc_child_ref_t ref)
{
    if (!a->types || !ncc_child_ref_is_set(ref)) {
        return nullptr;
    }

    ncc_parse_tree_t *node = ncc_tree_resolve_child_ref(v->grammar, v->node,
                                                        ref);

    return node ? ir_type_from_node(a->types, v->grammar, node) : nullptr;
}

static bool
scope_open(ncc_visit_t *v)
{
    actx_t           *a = (actx_t *)v->thunk;
    ncc_annotation_t *an = v->annot;
    ncc_string_t      scope_name = an->scope_tag;

    if (ncc_child_ref_is_set(an->name_ref)) {
        ncc_string_t resolved = ref_name(v, an->name_ref);

        if (resolved.u8_bytes > 0) {
            scope_name = resolved;
        }
    }

    ncc_symtab_push_scope(a->out->symtab, ns_default(), scope_name);

    ncc_scope_t *scope = ncc_symtab_current_scope(a->out->symtab, ns_default());

    v->pn->scope     = scope;
    scope->scope_tag = an->scope_tag;

    ncc_symtab_set_scope_node(a->out->symtab, ns_default(), v->node);

    if (name_is(an->scope_tag, "function")) {
        ncc_symtab_add(a->out->symtab, ns_default(),
                       NCC_STRING_STATIC("$return"), NCC_SYM_VARIABLE,
                       v->node);
    }

    return true;
}

static bool
adt_scope_open(ncc_visit_t *v)
{
    actx_t           *a  = (actx_t *)v->thunk;
    ncc_annotation_t *an = v->annot;
    ncc_string_t      scope_name = ref_name(v, an->name_ref);
    char              buf[32];

    if (scope_name.u8_bytes == 0) {
        int n = snprintf(buf, sizeof(buf), "anon_%d", a->anon_counter++);

        scope_name = ncc_string_borrow(buf, n > 0 ? (size_t)n : 0);
    }

    ncc_symtab_push_scope(a->out->symtab, ns_default(), scope_name);

    ncc_scope_t *scope = ncc_symtab_current_scope(a->out->symtab, ns_default());
    ncc_string_t kind  = an->adt_kind;

    if (ncc_child_ref_is_set(an->adt_keyword_ref)) {
        ncc_string_t kw = ref_name(v, an->adt_keyword_ref);

        if (kw.u8_bytes > 0) {
            kind = kw;
        }
    }

    v->pn->scope     = scope;
    scope->adt_kind  = kind;
    scope->scope_tag = an->scope_tag.data ? an->scope_tag : an->adt_kind;

    ncc_symtab_set_scope_node(a->out->symtab, ns_default(), v->node);

    return true;
}

static bool
scope_close(ncc_visit_t *v)
{
    actx_t *a = (actx_t *)v->thunk;

    ncc_symtab_pop_scope(a->out->symtab, ns_default());

    return true;
}

static bool
node_enter(ncc_visit_t *v)
{
    ((actx_t *)v->thunk)->last_sym = nullptr;

    return true;
}

static ncc_string_t
decl_ns(actx_t *a)
{
    ncc_string_t ns = a->pending_ns.data ? a->pending_ns : ns_default();

    a->pending_ns = (ncc_string_t){0};

    return ns;
}

static void
declared(actx_t *a, ncc_visit_t *v, ncc_sym_entry_t *sym)
{
    a->last_sym = sym;
    v->pn->sym  = sym;
}

static ncc_sym_entry_t *
subject(actx_t *a, ncc_visit_t *v)
{
    return v->pn->sym ? v->pn->sym : a->last_sym;
}

static ncc_sym_kind_t
sym_kind_of(ncc_string_t spec)
{
    if (name_is(spec, "param")) {
        return NCC_SYM_PARAM;
    }

    if (name_is(spec, "function")) {
        return NCC_SYM_FUNCTION;
    }

    if (name_is(spec, "module")) {
        return NCC_SYM_MODULE;
    }

    return NCC_SYM_VARIABLE;
}

static bool
annot_declares(ncc_visit_t *v)
{
    actx_t           *a  = (actx_t *)v->thunk;
    ncc_annotation_t *an = v->annot;
    ncc_string_t      nm = ref_name(v, an->name_ref);

    if (nm.u8_bytes == 0) {
        return true;
    }

    ncc_sym_kind_t   kind = sym_kind_of(an->sym_kind);
    ncc_sym_entry_t *sym  = ncc_symtab_add(a->out->symtab, decl_ns(a), nm,
                                           kind, v->node);

    sym->mutability = kind == NCC_SYM_PARAM ? NCC_SYM_IMMUTABLE
                                            : NCC_SYM_MUTABLE;
    sym->type       = type_of_child(a, v, an->type_ref);

    if (a->pending_visibility.data) {
        sym->visibility       = a->pending_visibility;
        a->pending_visibility = (ncc_string_t){0};
    }

    if (sym->shadowed) {
        ncc_list_push(a->out->shadowed, sym);
    }

    if (kind == NCC_SYM_PARAM) {
        ncc_list_push(a->out->params, sym);
    }

    declared(a, v, sym);

    return true;
}

static bool
annot_type_decl(ncc_visit_t *v)
{
    actx_t           *a  = (actx_t *)v->thunk;
    ncc_annotation_t *an = v->annot;
    ncc_string_t      nm = ref_name(v, an->name_ref);

    if (nm.u8_bytes == 0) {
        return true;
    }

    ncc_sym_entry_t *sym = ncc_symtab_add(a->out->symtab, ns_default(), nm,
                                          NCC_SYM_TYPEDEF, v->node);

    sym->type = type_of_child(a, v, an->name_ref);

    declared(a, v, sym);

    return true;
}

static bool
annot_adt(ncc_visit_t *v)
{
    actx_t           *a  = (actx_t *)v->thunk;
    ncc_annotation_t *an = v->annot;
    ncc_string_t      nm = ref_name(v, an->name_ref);

    if (nm.u8_bytes == 0) {
        return true;
    }

    ncc_sym_entry_t *sym = ncc_symtab_add(a->out->symtab,
                                          NCC_STRING_STATIC("tag"), nm,
                                          NCC_SYM_TAG, v->node);
    ncc_string_t     kind = an->adt_kind;

    if (ncc_child_ref_is_set(an->adt_keyword_ref)) {
        ncc_string_t kw = ref_name(v, an->adt_keyword_ref);

        if (kw.u8_bytes > 0) {
            kind = kw;
        }
    }

    sym->adt_kind = kind;

    declared(a, v, sym);

    return true;
}

static bool
member_decl(ncc_visit_t *v, bool is_method)
{
    actx_t           *a  = (actx_t *)v->thunk;
    ncc_annotation_t *an = v->annot;
    ncc_string_t      nm = ref_name(v, an->name_ref);

    if (nm.u8_bytes == 0) {
        return true;
    }

    ncc_sym_entry_t *sym = ncc_symtab_add(
        a->out->symtab, ns_default(), nm,
        is_method ? NCC_SYM_FUNCTION : NCC_SYM_VARIABLE, v->node);

    sym->is_field  = !is_method;
    sym->is_method = is_method;
    sym->type = type_of_child(a, v, an->type_ref);

    declared(a, v, sym);

    return true;
}

static bool
annot_field(ncc_visit_t *v)
{
    return member_decl(v, false);
}

static bool
annot_method(ncc_visit_t *v)
{
    return member_decl(v, true);
}

static bool
annot_mutability(ncc_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    ncc_sym_entry_t *sym = subject(a, v);
    ncc_string_t     m   = v->annot->mutability;

    if (!sym || !m.data) {
        return true;
    }

    if (name_is(m, "immutable")) {
        sym->mutability = NCC_SYM_IMMUTABLE;
    }
    else if (name_is(m, "const")) {
        sym->mutability = NCC_SYM_CONST;
    }
    else {
        sym->mutability = NCC_SYM_MUTABLE;
    }

    return true;
}

static bool
annot_namespace(ncc_visit_t *v)
{
    actx_t *a = (actx_t *)v->thunk;

    if (v->annot->ns_name.data) {
        a->pending_ns = v->annot->ns_name;
    }

    return true;
}

static bool
annot_direction(ncc_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    ncc_sym_entry_t *sym = subject(a, v);

    if (sym && v->annot->direction.data) {
        sym->direction = v->annot->direction;
    }

    return true;
}

static bool
annot_constrain(ncc_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    ncc_sym_entry_t *sym = subject(a, v);

    if (sym && v->annot->constraints.data) {
        sym->constraints = v->annot->constraints;
    }

    return true;
}

static bool
annot_repr(ncc_visit_t *v)
{
    actx_t           *a  = (actx_t *)v->thunk;
    ncc_annotation_t *an = v->annot;
    int64_t           val = 0;
    bool              have = false;

    if (an->has_val_int) {
        val  = an->val_int;
        have = true;
    }
    else if (ncc_child_ref_is_set(an->val_ref)) {
        ncc_parse_tree_t *k = ncc_tree_resolve_child_ref(v->grammar, v->node,
                                                         an->val_ref);
        ncc_token_info_t *t = k ? ncc_tree_find_first_terminal(k) : nullptr;

        if (t && ncc_option_is_set(t->value)) {
            ncc_string_t s = ncc_option_get(t->value);

            if (s.data) {
                val  = strtoll(s.data, nullptr, 10);
                have = true;
            }
        }
    }

    if (!have) {
        return true;
    }

    ncc_repr_pin_t *pin = &v->pn->repr;

    switch (an->kind) {
    case NCC_ANNOT_SIZE:
        pin->size     = val;
        pin->has_size = true;
        break;
    case NCC_ANNOT_ALIGNMENT:
        pin->align     = val;
        pin->has_align = true;
        break;
    case NCC_ANNOT_OFFSET:
        pin->offset     = val;
        pin->has_offset = true;
        break;
    default:
        pin->width     = val;
        pin->has_width = true;
        break;
    }

    (void)a;

    return true;
}

static bool
annot_literal(ncc_visit_t *v)
{
    actx_t           *a  = (actx_t *)v->thunk;
    ncc_annotation_t *an = v->annot;

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
        th_unify(a->types, v->pn->type, t);
    }
    else {
        v->pn->type = t;
    }

    return true;
}

static bool
annot_record(ncc_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    ncc_sym_entry_t *sym = subject(a, v);
    ncc_string_t     nm  = ref_name(v, v->annot->name_ref);

    if (!a->types) {
        return true;
    }

    th_member members[NCC_RECORD_MAX_FIELDS];
    size_t    n = 0;

    for (ncc_scope_t *s = v->pn->scope; s && !n; s = nullptr) {
        for (ncc_sym_entry_t *e = s->first_in_scope;
             e && n < NCC_RECORD_MAX_FIELDS;
             e = e->next_in_scope) {
            if (!e->is_field) {
                continue;
            }

            if (!e->type) {
                e->type = th_var(a->types);
            }

            members[n].name = e->name.data;
            members[n].type = e->type;
            n++;
        }
    }

    th_type *rec = th_record(a->types, members, n);

    if (!rec) {
        return true;
    }

    v->pn->type = rec;

    if (sym) {
        sym->type = rec;
    }

    if (nm.u8_bytes && !sym) {
        ncc_scope_t *inner = v->pn->scope;
        ncc_scope_t *outer = inner ? inner->parent : nullptr;

        if (outer) {
            ncc_symtab_enter_scope(a->out->symtab, ns_default(), outer);
        }

        ncc_sym_entry_t *bound = ncc_symtab_add(a->out->symtab, ns_default(),
                                                nm, NCC_SYM_TYPEDEF,
                                                v->node);

        if (outer) {
            ncc_symtab_enter_scope(a->out->symtab, ns_default(), inner);
        }

        if (bound) {
            bound->type = rec;
            declared(a, v, bound);
        }
    }

    return true;
}

static bool
annot_type(ncc_visit_t *v)
{
    actx_t           *a   = (actx_t *)v->thunk;
    ncc_annotation_t *an  = v->annot;
    ncc_sym_entry_t  *sym = subject(a, v);

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
annot_exposes(ncc_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    ncc_sym_entry_t *sym = subject(a, v);

    if (sym && v->pn->scope) {
        sym->exposed_scope = v->pn->scope;
    }

    return true;
}

static bool
annot_inherits(ncc_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    ncc_string_t     nm  = ref_name(v, v->annot->name_ref);
    ncc_sym_entry_t *sym = subject(a, v);

    if (sym && nm.u8_bytes > 0) {
        sym->inherits_name = nm;
    }

    return true;
}

static bool
annot_implements(ncc_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    ncc_string_t     nm  = ref_name(v, v->annot->name_ref);
    ncc_sym_entry_t *sym = subject(a, v);

    if (sym && nm.u8_bytes > 0) {
        ncc_list_push(sym->implements, nm);
    }

    return true;
}

static bool
annot_visibility(ncc_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    ncc_sym_entry_t *sym = subject(a, v);

    if (v->annot->visibility_spec.u8_bytes > 0) {
        if (sym) {
            sym->visibility = v->annot->visibility_spec;
        }
        else {
            a->pending_visibility = v->annot->visibility_spec;
        }
    }

    return true;
}

static bool
annot_static(ncc_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    ncc_sym_entry_t *sym = subject(a, v);

    if (sym) {
        sym->is_static = true;
    }

    return true;
}

static bool
annot_abstract(ncc_visit_t *v)
{
    actx_t          *a   = (actx_t *)v->thunk;
    ncc_sym_entry_t *sym = subject(a, v);

    if (sym) {
        sym->is_abstract = true;
    }

    return true;
}

static ncc_cf_label_t *
cf_label(ncc_visit_t *v, ncc_cf_kind_t kind)
{
    actx_t         *a     = (actx_t *)v->thunk;
    ncc_cf_label_t *label = ncc_alloc(ncc_cf_label_t);

    memset(label, 0, sizeof(*label));
    label->kind = kind;
    label->self = v->node;

    ncc_dict_put(a->out->cf_labels, v->node, label);

    return label;
}

static ncc_parse_tree_t *
child(ncc_visit_t *v, ncc_child_ref_t ref)
{
    return ncc_tree_resolve_child_ref(v->grammar, v->node, ref);
}

static bool
cf_branch(ncc_visit_t *v)
{
    ncc_cf_label_t *l = cf_label(v, NCC_CF_BRANCH);

    l->cond      = child(v, v->annot->name_ref);
    l->then_body = child(v, v->annot->type_ref);
    l->else_body = child(v, v->annot->value_ref);

    return true;
}

static bool
cf_two_part(ncc_visit_t *v, ncc_cf_kind_t kind)
{
    ncc_cf_label_t *l = cf_label(v, kind);

    l->cond      = child(v, v->annot->name_ref);
    l->then_body = child(v, v->annot->type_ref);

    return true;
}

static bool
cf_loop(ncc_visit_t *v)
{
    return cf_two_part(v, NCC_CF_LOOP);
}

static bool
cf_switch(ncc_visit_t *v)
{
    return cf_two_part(v, NCC_CF_SWITCH);
}

static bool
cf_call(ncc_visit_t *v)
{
    return cf_two_part(v, NCC_CF_CALL);
}

static bool
cf_jump(ncc_visit_t *v)
{
    ncc_cf_label_t *l = cf_label(v, NCC_CF_JUMP);

    l->jump_kind = v->annot->scope_tag;

    if (!l->jump_kind.data) {
        l->jump_kind = ncc_tree_extract_first_identifier(v->node);
    }

    return true;
}

static bool
cf_capture(ncc_visit_t *v)
{
    ncc_cf_label_t *l = cf_label(v, NCC_CF_CAPTURE);

    l->tag            = v->annot->scope_tag;
    l->capture_by_tag = v->annot->capture_by_tag;

    return true;
}

static bool
cf_assigns(ncc_visit_t *v)
{
    ncc_cf_label_t *l = cf_label(v, NCC_CF_ASSIGNS);

    l->cond      = child(v, v->annot->name_ref);
    l->then_body = child(v, v->annot->value_ref);

    return true;
}

static bool
cf_varref(ncc_visit_t *v)
{
    actx_t           *a = (actx_t *)v->thunk;
    ncc_parse_tree_t *k = child(v, v->annot->name_ref);

    cf_label(v, NCC_CF_VARREF)->cond = k;

    ncc_string_t nm = k ? ncc_tree_extract_first_identifier(k)
                        : (ncc_string_t){0};

    if (!nm.u8_bytes) {
        return true;
    }

    ncc_scope_t *scope = v->pn->scope
                           ? v->pn->scope
                           : ncc_symtab_current_scope(a->out->symtab,
                                                      ns_default());
    ncc_sym_entry_t *sym = ncc_symtab_lookup_scoped(a->out->symtab, scope,
                                                    ns_default(), nm);

    if (sym) {
        sym->uses++;
        v->pn->sym = sym;
    }
    else {
        ncc_list_push(a->out->undefined, v->node);
    }

    return true;
}

static bool
cf_operator(ncc_visit_t *v)
{
    if (name_is(v->annot->op_kind, "unwrap_result")) {
        cf_label(v, NCC_CF_UNWRAP_RESULT);
    }

    return true;
}

static bool
cf_notrivia(ncc_visit_t *v)
{
    ncc_parse_tree_t *target = child(v, v->annot->notrivia_ref);
    ncc_token_info_t *tok    = target ? ncc_tree_find_first_terminal(target)
                                      : nullptr;

    if (tok) {
        tok->leading_trivia  = nullptr;
        tok->trailing_trivia = nullptr;
        tok->no_trivia       = true;
    }

    return true;
}

static bool
annot_infer(ncc_visit_t *v)
{
    actx_t           *a  = (actx_t *)v->thunk;
    ncc_annotation_t *an = v->annot;

    if (!a->types || !an->infer_ast) {
        return true;
    }

    bool     ok = false;
    th_type *t  = ncc_infer_eval(an->infer_ast, v, a->types, a->out->symtab,
                                 &ok);

    if (!ok || !t) {
        return true;
    }

    if (ncc_child_ref_is_set(an->name_ref)) {
        ncc_parse_tree_t *k = ncc_tree_resolve_child_ref(v->grammar, v->node,
                                                         an->name_ref);

        if (k && !ncc_tree_is_leaf(k)) {
            ncc_nt_node_t *pn = &ncc_tree_node_value(k);

            if (pn->type) {
                th_unify(a->types, pn->type, t);
            }
            else {
                pn->type = t;
            }
        }

        return true;
    }

    if (v->pn->type) {
        th_unify(a->types, v->pn->type, t);
    }
    else {
        v->pn->type = t;
    }

    if (v->pn->sym) {
        if (v->pn->sym->type) {
            th_unify(a->types, v->pn->sym->type, v->pn->type);
        }
        else {
            v->pn->sym->type = v->pn->type;
        }
    }

    return true;
}

static bool
infer_propagate(ncc_visit_t *v)
{
    actx_t *a = (actx_t *)v->thunk;

    if (!a->types || v->pn->type) {
        return true;
    }

    ncc_parse_tree_t *sole = nullptr;
    size_t            nc   = ncc_tree_num_children(v->node);

    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *k = ncc_tree_child(v->node, i);

        if (!k || ncc_tree_is_leaf(k)) {
            continue;
        }

        if (sole) {
            return true;
        }

        sole = k;
    }

    if (sole) {
        v->pn->type = ncc_tree_node_value(sole).type;
    }

    return true;
}

static bool
scope_reenter(ncc_visit_t *v)
{
    actx_t *a = (actx_t *)v->thunk;

    if (v->pn->scope) {
        ncc_symtab_enter_scope(a->out->symtab, ns_default(), v->pn->scope);
    }

    return true;
}

static bool
scope_leave(ncc_visit_t *v)
{
    actx_t *a = (actx_t *)v->thunk;

    if (v->pn->scope) {
        ncc_symtab_leave_scope(a->out->symtab, ns_default());
        a->last_sym = nullptr;
    }

    return true;
}

ncc_string_t
ncc_annot_pass_name(ncc_annot_pass_t p)
{
    switch (p) {
    case NCC_ANNOT_PASS_SCOPE:
        return NCC_STRING_STATIC("annot.scope");
    case NCC_ANNOT_PASS_SYMBOLS:
        return NCC_STRING_STATIC("annot.symbols");
    case NCC_ANNOT_PASS_REFINE:
        return NCC_STRING_STATIC("annot.refine");
    case NCC_ANNOT_PASS_CFLOW:
        return NCC_STRING_STATIC("annot.cflow");
    case NCC_ANNOT_PASS_INFER:
        return NCC_STRING_STATIC("annot.infer");
    case NCC_ANNOT_PASS_ALL:
        return NCC_STRING_STATIC("annot.all");
    }

    return NCC_STRING_STATIC("?");
}

ncc_annot_result_t *
ncc_annot_result_new(ncc_symtab_t *borrowed)
{
    ncc_annot_result_t *r = ncc_alloc(ncc_annot_result_t);

    memset(r, 0, sizeof(*r));

    r->symtab          = borrowed ? borrowed : ncc_symtab_new();
    r->borrowed_symtab = borrowed != nullptr;
    r->cf_labels       = ncc_alloc(ncc_dict_t);

    ncc_dict_init(r->cf_labels, ncc_hash_word, nullptr);

    return r;
}

void
ncc_annot_register_pass(ncc_tree_walk_t    *w,
                        ncc_annot_result_t *out,
                        th_ctx             *types,
                        ncc_annot_pass_t    which)
{
    if (!w || !out) {
        return;
    }

    actx_t *a = out->impl;

    if (!a) {
        a = ncc_alloc(actx_t);

        memset(a, 0, sizeof(*a));
        a->out    = out;
        out->impl = a;
    }

    a->types = types;

    bool all = which == NCC_ANNOT_PASS_ALL;

    ncc_walk_on_node(w, node_enter, nullptr, a);

    if (all || which == NCC_ANNOT_PASS_SCOPE) {
        ncc_walk_on_annot_phase(w, NCC_ANNOT_SCOPE_OPEN, PHASE_SCOPE,
                                scope_open, scope_close, a);
        ncc_walk_on_annot_phase(w, NCC_ANNOT_ADT, PHASE_SCOPE, adt_scope_open,
                                scope_close, a);
    }
    else {
        ncc_walk_on_annot_phase(w, NCC_ANNOT_SCOPE_OPEN, PHASE_SCOPE,
                                scope_reenter, scope_leave, a);
        ncc_walk_on_annot_phase(w, NCC_ANNOT_ADT, PHASE_SCOPE, scope_reenter,
                                scope_leave, a);
    }

    bool symbols = all || which == NCC_ANNOT_PASS_SYMBOLS;
    bool refine  = all || which == NCC_ANNOT_PASS_REFINE;
    bool cflow   = all || which == NCC_ANNOT_PASS_CFLOW;

    if (all || which == NCC_ANNOT_PASS_INFER) {
        ncc_walk_on_node(w, nullptr, infer_propagate, a);
        ncc_walk_on_annot_phase(w, NCC_ANNOT_INFER, PHASE_INFER, nullptr,
                                annot_infer, a);
    }

    if (symbols) {
    ncc_walk_on_annot_phase(w, NCC_ANNOT_NAMESPACE, PHASE_SCOPE,
                            annot_namespace, nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_DECLARES, PHASE_DECL, annot_declares,
                            nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_TYPE_DECL, PHASE_DECL,
                            annot_type_decl, nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_ADT, PHASE_DECL, annot_adt, nullptr,
                            a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_FIELD, PHASE_DECL, annot_field,
                            nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_METHOD, PHASE_DECL, annot_method,
                            nullptr, a);
    }

    if (refine) {
    ncc_walk_on_annot_phase(w, NCC_ANNOT_TYPE, PHASE_REFINE, annot_type,
                            nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_EXPOSES, PHASE_REFINE, annot_exposes,
                            nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_INHERITS, PHASE_REFINE,
                            annot_inherits, nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_IMPLEMENTS, PHASE_REFINE,
                            annot_implements, nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_VISIBILITY, PHASE_REFINE,
                            annot_visibility, nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_STATIC, PHASE_REFINE, annot_static,
                            nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_ABSTRACT, PHASE_REFINE,
                            annot_abstract, nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_LITERAL, PHASE_REFINE, nullptr,
                            annot_literal, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_RECORD, PHASE_REFINE, nullptr,
                            annot_record, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_MUTABILITY, PHASE_REFINE,
                            annot_mutability, nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_DIRECTION, PHASE_REFINE,
                            annot_direction, nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_CONSTRAIN, PHASE_REFINE,
                            annot_constrain, nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_SIZE, PHASE_REFINE, annot_repr,
                            nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_ALIGNMENT, PHASE_REFINE, annot_repr,
                            nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_OFFSET, PHASE_REFINE, annot_repr,
                            nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_WIDTH, PHASE_REFINE, annot_repr,
                            nullptr, a);
    }

    if (cflow) {
    ncc_walk_on_annot_phase(w, NCC_ANNOT_BRANCH, PHASE_CF, cf_branch, nullptr,
                            a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_LOOP, PHASE_CF, cf_loop, nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_SWITCH, PHASE_CF, cf_switch, nullptr,
                            a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_CALL, PHASE_CF, cf_call, nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_JUMP, PHASE_CF, cf_jump, nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_CAPTURE, PHASE_CF, cf_capture,
                            nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_ASSIGNS, PHASE_CF, cf_assigns,
                            nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_VARREF, PHASE_CF, cf_varref, nullptr,
                            a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_OPERATOR, PHASE_CF, cf_operator,
                            nullptr, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_NOTRIVIA, PHASE_CF, cf_notrivia,
                            nullptr, a);
    }
}

void
ncc_annot_register(ncc_tree_walk_t    *w,
                   ncc_annot_result_t *out,
                   th_ctx             *types)
{
    ncc_annot_register_pass(w, out, types, NCC_ANNOT_PASS_ALL);
}

ncc_annot_result_t *
ncc_annot_walk_tree(ncc_grammar_t    *g,
                    ncc_parse_tree_t *tree,
                    th_ctx           *types)
{
    if (!g || !tree) {
        return nullptr;
    }

    ncc_annot_result_t *r = ncc_annot_result_new(nullptr);
    ncc_tree_walk_t    *w = ncc_tree_walk_new(g, r);

    ncc_annot_register(w, r, types);
    ncc_tree_walk_run(w, tree);
    ncc_tree_walk_free(w);

    return r;
}

void
ncc_annot_result_free(ncc_annot_result_t *r)
{
    if (!r) {
        return;
    }

    ncc_dict_foreach (r->cf_labels, e) {
        ncc_free(e->value);
    }

    ncc_dict_free(r->cf_labels);
    ncc_free(r->cf_labels);

    if (!r->borrowed_symtab) {
        ncc_symtab_free(r->symtab);
    }

    ncc_free(r->params.data);
    ncc_free(r->shadowed.data);
    ncc_free(r->undefined.data);
    ncc_free(r->impl);

    ncc_free(r);
}

ncc_cf_label_t *
ncc_cf_label_lookup(ncc_annot_result_t *r, ncc_parse_tree_t *node)
{
    if (!r || !node) {
        return nullptr;
    }

    bool  found = false;
    void *val   = ncc_dict_get(r->cf_labels, node, &found);

    return found ? (ncc_cf_label_t *)val : nullptr;
}
