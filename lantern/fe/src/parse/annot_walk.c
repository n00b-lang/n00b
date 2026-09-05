#include "parse/annot_walk.h"
#include "parse/tree_util.h"
#include "internal/parse/grammar_internal.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/hash.h"

#include <stdio.h>
#include <string.h>

// Registration phases. Every visitor in one phase has run over all of a node's
// annotations before the next phase starts, so an annotation that reads what
// another produced works whichever order the two were written in on the rule.
enum {
    PHASE_SCOPE  = 0,
    PHASE_DECL   = 1,
    PHASE_REFINE = 2,
    PHASE_CF     = 3,
};

typedef struct {
    ncc_annot_result_t *out;
    ncc_type_binder_t   binder;
    ncc_sym_entry_t    *last_sym;
    ncc_string_t        pending_visibility;
    int32_t             anon_counter;
} actx_t;

// Borrowed, never freed: the symbol table copies any string it keeps.
static ncc_string_t
borrow(const char *s, size_t len)
{
    return (ncc_string_t){
        .data       = (char *)s,
        .u8_bytes   = len,
        .codepoints = len,
    };
}

#define LIT(s) borrow((s), sizeof(s) - 1)

static ncc_string_t
ns_default(void)
{
    return LIT("");
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

static tdesc_t *
bind_from_tree(actx_t *a, ncc_visit_t *v, ncc_child_ref_t ref)
{
    if (!a->binder.from_tree || !ncc_child_ref_is_set(ref)) {
        return nullptr;
    }

    ncc_parse_tree_t *node = ncc_tree_resolve_child_ref(v->grammar, v->node,
                                                        ref);

    return node ? a->binder.from_tree(a->binder.ctx, v->grammar, node)
                : nullptr;
}

// ============================================================================
// Phase 0: scopes
// ============================================================================

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

    // A function scope carries a $return symbol so that a later constraint can
    // name the return type the way it names any other.
    if (name_is(an->scope_tag, "function")) {
        ncc_symtab_add(a->out->symtab, ns_default(), LIT("$return"),
                       NCC_SYM_VARIABLE, v->node);
    }

    return true;
}

static bool
adt_scope_open(ncc_visit_t *v)
{
    actx_t           *a  = (actx_t *)v->thunk;
    ncc_annotation_t *an = v->annot;
    ncc_string_t      scope_name = ref_name(v, an->name_ref);

    if (scope_name.u8_bytes == 0) {
        char buf[32];

        int n = snprintf(buf, sizeof(buf), "anon_%d", a->anon_counter++);

        scope_name = borrow(buf, n > 0 ? (size_t)n : 0);
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

// ============================================================================
// Node entry: last_sym is per node
// ============================================================================

static bool
node_enter(ncc_visit_t *v)
{
    ((actx_t *)v->thunk)->last_sym = nullptr;

    return true;
}

// ============================================================================
// Phase 1: symbol creation
// ============================================================================

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
    ncc_sym_entry_t *sym  = ncc_symtab_add(a->out->symtab, ns_default(), nm,
                                           kind, v->node);

    sym->mutability = kind == NCC_SYM_PARAM ? NCC_SYM_IMMUTABLE
                                            : NCC_SYM_MUTABLE;
    sym->type       = bind_from_tree(a, v, an->type_ref);

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

    a->last_sym = sym;

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

    sym->type   = bind_from_tree(a, v, an->name_ref);
    a->last_sym = sym;

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

    ncc_sym_entry_t *sym = ncc_symtab_add(a->out->symtab, LIT("tag"), nm,
                                          NCC_SYM_TAG, v->node);
    ncc_string_t     kind = an->adt_kind;

    if (ncc_child_ref_is_set(an->adt_keyword_ref)) {
        ncc_string_t kw = ref_name(v, an->adt_keyword_ref);

        if (kw.u8_bytes > 0) {
            kind = kw;
        }
    }

    sym->adt_kind = kind;
    a->last_sym   = sym;

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
    sym->type      = bind_from_tree(a, v, an->type_ref);
    a->last_sym    = sym;

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

// ============================================================================
// Phase 2: refinement of the symbol phase 1 created
// ============================================================================

static bool
annot_type(ncc_visit_t *v)
{
    actx_t           *a  = (actx_t *)v->thunk;
    ncc_annotation_t *an = v->annot;

    if (!a->last_sym) {
        return true;
    }

    if (an->type_spec.data && a->binder.from_spec) {
        a->last_sym->type = a->binder.from_spec(a->binder.ctx, an->type_spec);
    }
    else {
        tdesc_t *t = bind_from_tree(a, v, an->name_ref);

        if (t) {
            a->last_sym->type = t;
        }
    }

    return true;
}

static bool
annot_exposes(ncc_visit_t *v)
{
    actx_t *a = (actx_t *)v->thunk;

    if (a->last_sym && v->pn->scope) {
        a->last_sym->exposed_scope = v->pn->scope;
    }

    return true;
}

static bool
annot_inherits(ncc_visit_t *v)
{
    actx_t      *a  = (actx_t *)v->thunk;
    ncc_string_t nm = ref_name(v, v->annot->name_ref);

    if (a->last_sym && nm.u8_bytes > 0) {
        a->last_sym->inherits_name = nm;
    }

    return true;
}

static bool
annot_implements(ncc_visit_t *v)
{
    actx_t      *a  = (actx_t *)v->thunk;
    ncc_string_t nm = ref_name(v, v->annot->name_ref);

    if (a->last_sym && nm.u8_bytes > 0) {
        ncc_list_push(a->last_sym->implements, nm);
    }

    return true;
}

static bool
annot_visibility(ncc_visit_t *v)
{
    actx_t *a = (actx_t *)v->thunk;

    if (v->annot->visibility_spec.u8_bytes > 0) {
        if (a->last_sym) {
            a->last_sym->visibility = v->annot->visibility_spec;
        }
        else {
            // Stamped onto the next symbol a @declares creates.
            a->pending_visibility = v->annot->visibility_spec;
        }
    }

    return true;
}

static bool
annot_static(ncc_visit_t *v)
{
    actx_t *a = (actx_t *)v->thunk;

    if (a->last_sym) {
        a->last_sym->is_static = true;
    }

    return true;
}

static bool
annot_abstract(ncc_visit_t *v)
{
    actx_t *a = (actx_t *)v->thunk;

    if (a->last_sym) {
        a->last_sym->is_abstract = true;
    }

    return true;
}

// ============================================================================
// Phase 3: control flow labels
// ============================================================================

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
    cf_label(v, NCC_CF_VARREF)->cond = child(v, v->annot->name_ref);

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

    if (tok && tok->leading_trivia) {
        fprintf(stderr,
                "warning: whitespace before modifier at line %u col %u\n",
                tok->line, tok->column);
    }

    return true;
}

// ============================================================================
// Registration
// ============================================================================

void
ncc_annot_register(ncc_tree_walk_t *w, ncc_annot_result_t *out,
                   const ncc_type_binder_t *binder)
{
    if (!w || !out) {
        return;
    }

    actx_t *a = ncc_alloc(actx_t);

    memset(a, 0, sizeof(*a));
    a->out    = out;
    out->impl = a;

    if (binder) {
        a->binder = *binder;
    }

    ncc_walk_on_node(w, node_enter, nullptr, a);

    ncc_walk_on_annot_phase(w, NCC_ANNOT_SCOPE_OPEN, PHASE_SCOPE, scope_open,
                            scope_close, a);
    ncc_walk_on_annot_phase(w, NCC_ANNOT_ADT, PHASE_SCOPE, adt_scope_open,
                            scope_close, a);

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

ncc_annot_result_t *
ncc_annot_walk_tree(ncc_grammar_t *g, ncc_parse_tree_t *tree,
                    const ncc_type_binder_t *binder)
{
    if (!g || !tree) {
        return nullptr;
    }

    ncc_annot_result_t *r = ncc_alloc(ncc_annot_result_t);

    memset(r, 0, sizeof(*r));
    r->symtab    = ncc_symtab_new();
    r->cf_labels = ncc_alloc(ncc_dict_t);

    ncc_dict_init(r->cf_labels, ncc_hash_word, nullptr);

    ncc_tree_walk_t *w = ncc_tree_walk_new(g, r);

    ncc_annot_register(w, r, binder);
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

    for (size_t b = 0; b < r->cf_labels->capacity; b++) {
        if (r->cf_labels->buckets[b].state == _NCC_BUCKET_OCCUPIED) {
            ncc_free(r->cf_labels->buckets[b].value);
        }
    }

    ncc_dict_free(r->cf_labels);
    ncc_free(r->cf_labels);
    ncc_symtab_free(r->symtab);
    ncc_free(r->params.data);
    ncc_free(r->shadowed.data);
    ncc_free(r->impl);

    // Type descriptors are not freed here. One descriptor is shared by the
    // symbol, the parse node and the IR node that refer to the same type, so
    // it outlives any one of them; whoever the binder allocates from owns it.
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
