#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/grammar.h"
#include "ir/layout.h"
#include "ir/text.h"
#include "ir/walk.h"

#include "parse/pwz.h"
#include "parse/tree_print.h"
#include "scanner/token_stream.h"

const char *ir_type_text(ir_module_t *m, th_type *t)
{
    if (!m || !t) {
        return "?";
    }

    const char *s = th_print(ir_ctx_types(m->ctx), t);

    return s ? s : "?";
}

static void put_type(ncc_buffer_t *b, ir_module_t *m, th_type *t)
{
    ncc_buffer_puts(b, ir_type_text(m, t));
}

static void put_span(ncc_buffer_t *b, ir_module_t *m, ir_span_t s)
{
    (void)m;

    if (!ir_span_known(s) || s.implied) {
        return;
    }
    ncc_buffer_printf(b, " !loc(%d, %u, %u, %u, %u, %u, %u)", s.file, s.start_byte,
              s.end_byte, s.start_line, s.start_col, s.end_line, s.end_col);
}

const char *ir_origin_kind_name(ir_origin_kind_t k)
{
    switch (k) {
    case IR_ORIGIN_SOURCE:
        return "source";
    case IR_ORIGIN_INLINED_FROM:
        return "inlined";
    case IR_ORIGIN_EXPANDED_FROM:
        return "expanded";
    case IR_ORIGIN_LOWERED_FROM:
        return "lowered";
    default:
        return "generated";
    }
}

bool ir_origin_kind_from_name(const char *s, ir_origin_kind_t *out)
{
    static const struct {
        const char      *name;
        ir_origin_kind_t k;
    } tab[] = {
        {"source", IR_ORIGIN_SOURCE},
        {"inlined", IR_ORIGIN_INLINED_FROM},
        {"expanded", IR_ORIGIN_EXPANDED_FROM},
        {"lowered", IR_ORIGIN_LOWERED_FROM},
        {"generated", IR_ORIGIN_GENERATED_BY},
    };

    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (!strcmp(tab[i].name, s)) {
            *out = tab[i].k;

            return true;
        }
    }

    return false;
}

static void put_origin(ncc_buffer_t *b, const ir_origin_t *o)
{
    for (; o; o = o->parent) {
        ncc_buffer_printf(b, " !origin(%s, ", ir_origin_kind_name(o->kind));

        if (o->agent) {
            ir_quote(b, o->agent);
        }
        else {
            ncc_buffer_puts(b, "\"\"");
        }
        ncc_buffer_printf(b, ", %d, %u, %u, %u, %u, %u, %u)", o->span.file,
                  o->span.start_byte, o->span.end_byte, o->span.start_line,
                  o->span.start_col, o->span.end_line, o->span.end_col);
    }
}

static void put_annots(ncc_buffer_t *b, ir_module_t *m, ir_annots_t *a)
{
    for (uint32_t i = 0; i < ir_annot_count(a); i++) {
        ir_annot_t *an = ir_annot_at(a, i);

        if (!strcmp(an->key, "ir.comment")) {
            continue;
        }

        ncc_buffer_printf(b, " !%s(", an->key);

        switch (an->kind) {
        case IR_ANNOT_BOOL:
            ncc_buffer_puts(b, an->b ? "true" : "false");
            break;
        case IR_ANNOT_INT:
            ir_lit_into(b, an->i);
            break;
        case IR_ANNOT_STR:
            ir_quote(b, an->s ? an->s : "");
            break;
        case IR_ANNOT_TYPE:
            put_type(b, m, an->t);
            break;
        case IR_ANNOT_VALUE:
            ncc_buffer_printf(b, "%%%s", an->v && an->v->name ? an->v->name : "?");
            break;
        }

        if (an->derived_from) {
            const char *p = ir_prop_name(an->derived_from);

            ncc_buffer_printf(b, " from %s", p ? p : "?");
        }
        ncc_buffer_puts(b, ")");
    }
}

static void put_comment(ncc_buffer_t *b, ir_annots_t *a, const char *indent)
{
    ir_annot_t *c = ir_annot_get(a, "ir.comment");

    if (!c || c->kind != IR_ANNOT_STR || !c->s) {
        return;
    }

    const char *p = c->s;

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t      n  = nl ? (size_t)(nl - p) : strlen(p);

        ncc_buffer_printf(b, "%s# ", indent);
        ncc_buffer_append(b, p, n);
        ncc_buffer_puts(b, "\n");

        if (!nl) {
            break;
        }
        p = nl + 1;
    }
}

static void put_operand(ncc_buffer_t *b, ir_module_t *m, const ir_operand_t *o)
{
    switch (o->kind) {
    case IR_OPD_VALUE:
        if (!o->value) {
            ncc_buffer_puts(b, "%?");
        }
        else if (o->value->kind == IR_VAL_GLOBAL
                 || o->value->kind == IR_VAL_FUNC) {
            ncc_buffer_printf(b, "@%s", o->value->name);
        }
        else if (o->value->kind == IR_VAL_LOCAL) {
            ncc_buffer_printf(b, "$%s", o->value->name);
        }
        else {
            ncc_buffer_printf(b, "%%%s", o->value->name);
        }
        break;

    case IR_OPD_TYPE:
        put_type(b, m, o->type);
        break;

    case IR_OPD_NAME:
        ir_quote(b, o->name ? o->name : "");
        break;

    case IR_OPD_BLOCK:
        ncc_buffer_puts(b, o->block ? o->block->name : "?");
        break;

    case IR_OPD_DEFAULT_BLOCK:
        ncc_buffer_printf(b, "default: %s", o->block ? o->block->name : "?");
        break;

    case IR_OPD_CONST:
        if (o->lit.is_float) {
            if (o->lit.text) {
                ncc_buffer_puts(b, o->lit.text);
            }
            else {
                ncc_buffer_printf(b, "%.17g", o->lit.f);
            }
        }
        else {
            ir_lit_into(b, o->lit.i);
        }
        break;

    case IR_OPD_PHI_ARM:
        ncc_buffer_printf(b, "[%s: %%%s]", o->phi.block ? o->phi.block->name : "?",
                  o->phi.value ? o->phi.value->name : "?");
        break;

    case IR_OPD_NAMED_ARG:
        ncc_buffer_printf(b, "%s = ", o->named.name ? o->named.name : "?");

        if (o->named.value && o->named.value->kind == IR_VAL_LOCAL) {
            ncc_buffer_printf(b, "$%s", o->named.value->name);
        }
        else if (o->named.value
                 && (o->named.value->kind == IR_VAL_GLOBAL
                     || o->named.value->kind == IR_VAL_FUNC)) {
            ncc_buffer_printf(b, "@%s", o->named.value->name);
        }
        else {
            ncc_buffer_printf(b, "%%%s",
                      o->named.value ? o->named.value->name : "?");
        }
        break;

    case IR_OPD_SWITCH_ARM:
        ncc_buffer_puts(b, "[");
        ir_lit_into(b, o->arm.value);
        ncc_buffer_printf(b, ": %s]",
                  o->arm.block ? o->arm.block->name : "?");
        break;

    case IR_OPD_ARG:
        break;
    }
}

static void put_init(ncc_buffer_t *b, bool has, bool is_float, const char *text,
                     double f, ir_lit_t lit)
{
    if (!has) {
        return;
    }
    ncc_buffer_puts(b, " = ");

    if (is_float) {
        if (text) {
            ncc_buffer_puts(b, text);
        }
        else {
            ncc_buffer_printf(b, "%.17g", f);
        }

        return;
    }
    ir_lit_into(b, lit);
}

static void put_local_slot(ncc_buffer_t *b, ir_module_t *m, const ir_local_t *l)
{
    ncc_buffer_printf(b, "$%s: ", l->name);
    put_type(b, m, l->type);
    put_init(b, l->has_init, l->init_is_float, l->init_text, l->init_f,
             l->init);
}

static bool put_layout_val(ncc_buffer_t *b, ir_module_t *m,
                           const ir_layout_val_t *v)
{
    if (v->state == IR_LAYOUT_PINNED) {
        ncc_buffer_printf(b, "%llu", (unsigned long long)v->value);

        return true;
    }

    if (v->state == IR_LAYOUT_SYMBOLIC) {
        ncc_buffer_printf(b, "%s(%s", ir_symkind_name(v->sym), ir_type_text(m, v->of));

        if (v->sym == IR_SYM_OFFSETOF) {
            ncc_buffer_puts(b, ", ");
            ir_quote(b, v->member ? v->member : "");
        }
        ncc_buffer_puts(b, ")");

        return true;
    }

    return false;
}

static bool authored_anywhere(const ir_descriptor_t *d)
{
    if (d->size.authored || d->align.authored || d->stride.authored) {
        return true;
    }

    for (uint32_t i = 0; i < d->n_members; i++) {
        if (d->members[i].bit_offset.authored) {
            return true;
        }
    }

    return false;
}

static void put_layouts(ncc_buffer_t *b, ir_module_t *m)
{
    for (size_t i = 0; i < ir_type_count(m); i++) {
        ir_typeslot_t   *slot = ir_type_slot_at(m, i);
        th_type         *t    = slot->type;
        ir_descriptor_t *d    = nullptr;

        for (size_t k = 0; k < slot->n_descs; k++) {
            const char *have = slot->descs[k].target;
            const char *want = ir_module_target(m);

            if (have && want ? !strcmp(have, want) : have == want) {
                d = &slot->descs[k].d;
            }
        }

        if (!d || !authored_anywhere(d)) {
            continue;
        }

        ncc_buffer_printf(b, "\n  layout %s {\n", ir_type_text(m, t));

        if (d->size.authored) {
            ncc_buffer_puts(b, "    size = ");
            put_layout_val(b, m, &d->size);
            ncc_buffer_puts(b, "\n");
        }

        if (d->align.authored) {
            ncc_buffer_puts(b, "    align = ");
            put_layout_val(b, m, &d->align);
            ncc_buffer_puts(b, "\n");
        }

        if (d->stride.authored) {
            ncc_buffer_puts(b, "    stride = ");
            put_layout_val(b, m, &d->stride);
            ncc_buffer_puts(b, "\n");
        }

        for (uint32_t k = 0; k < d->n_members; k++) {
            const ir_member_layout_t *ml = &d->members[k];

            if (!ml->bit_offset.authored) {
                continue;
            }
            ncc_buffer_printf(b, "    %s @ bits %llu..%llu\n", ml->name,
                      (unsigned long long)ml->bit_offset.value,
                      (unsigned long long)(ml->bit_offset.value
                                           + ml->bit_width.value));
        }
        ncc_buffer_puts(b, "  }\n");
    }
}

static void put_typedefs(ncc_buffer_t *b, ir_module_t *m)
{
    for (size_t i = 0; i < ir_type_count(m); i++) {
        ir_typeslot_t *slot = ir_type_slot_at(m, i);
        th_type       *t    = slot->type;
        th_type       *def  = slot->def;

        if (!def) {
            continue;
        }
        ncc_buffer_printf(b, "\n  type %s = %s\n", ir_type_text(m, t),
                  ir_type_text(m, def));
    }
}

static bool on_module_pre(ir_visit_t *v)
{
    ncc_buffer_t       *b  = v->user;
    ir_module_t *m  = v->module;
    ir_annots_t *ma = ir_module_annots(m);

    put_comment(b, ma, "");
    ncc_buffer_printf(b, "module %s {\n", ir_module_name(m));
    ncc_buffer_printf(b, "  profile %s\n", ir_module_profile(m)->name);

    if (ir_module_target(m)) {
        ncc_buffer_printf(b, "  target %s\n", ir_module_target(m));
    }

    bool any = false;

    for (uint32_t i = 0; i < ir_annot_count(ma); i++) {
        if (strcmp(ir_annot_at(ma, i)->key, "ir.comment")) {
            any = true;
        }
    }

    if (any) {
        ncc_buffer_puts(b, "  meta");
        put_annots(b, m, ma);
        ncc_buffer_puts(b, "\n");
    }

    put_typedefs(b, m);
    put_layouts(b, m);

    return true;
}

static bool on_module_post(ir_visit_t *v)
{
    ncc_buffer_puts((ncc_buffer_t *)v->user, "}\n");

    return true;
}

static bool on_global(ir_visit_t *v)
{
    ncc_buffer_t       *b = v->user;
    ir_module_t *m = v->module;
    ir_global_t *g = v->global;

    ncc_buffer_puts(b, "\n");
    put_comment(b, &g->annots, "  ");
    ncc_buffer_puts(b, "  ");

    if (g->is_static) {
        ncc_buffer_puts(b, "static ");
    }

    if (g->is_extern) {
        ncc_buffer_puts(b, "extern ");
    }

    if (g->mutability != IR_MUT_MUTABLE) {
        ncc_buffer_printf(b, "%s ", ir_mutability_name(g->mutability));
    }
    ncc_buffer_printf(b, "global @%s: ", g->name);
    put_type(b, m, g->type);
    put_init(b, g->has_init, g->init_is_float, g->init_text, g->init_f,
             g->init);
    put_span(b, m, g->span);
    put_origin(b, g->origin);
    put_annots(b, m, &g->annots);
    ncc_buffer_puts(b, "\n");

    return true;
}

static void put_signature(ncc_buffer_t *b, ir_module_t *m, ir_func_t *f)
{
    ncc_buffer_printf(b, "func @%s(", f->name);

    bool first = true;

    for (uint32_t i = 0; i < f->n_params; i++) {
        ncc_buffer_printf(b, "%s%%%s: ", first ? "" : ", ", f->params[i]->name);
        put_type(b, m, f->params[i]->type);
        first = false;
    }

    const th_param *rest = th_func_rest(f->type);

    if (rest) {
        ncc_buffer_printf(b, "%s*", first ? "" : ", ");
        put_type(b, m, rest->type);
        first = false;
    }

    for (size_t i = 0; i < th_func_nkw(f->type); i++) {
        const th_kwparam *k = th_func_kw(f->type, i);

        ncc_buffer_printf(b, "%s%s: ", first ? "" : ", ", k->name);
        put_type(b, m, k->type);
        first = false;
    }
    ncc_buffer_puts(b, ") -> ");
    put_type(b, m, th_func_ret(f->type));
}

static bool on_func_pre(ir_visit_t *v)
{
    ncc_buffer_t       *b = v->user;
    ir_module_t *m = v->module;
    ir_func_t   *f = v->func;

    ncc_buffer_puts(b, "\n");
    put_comment(b, &f->annots, "  ");
    ncc_buffer_puts(b, "  ");

    switch (f->linkage) {
    case IR_LINK_INTERNAL:
        ncc_buffer_puts(b, "static ");
        break;
    case IR_LINK_EXTERN:
        ncc_buffer_puts(b, "extern ");
        break;
    default:
        break;
    }

    put_signature(b, m, f);
    put_span(b, m, f->span);
    put_origin(b, f->origin);
    put_annots(b, m, &f->annots);
    ncc_buffer_puts(b, " {\n");

    return true;
}

static bool on_func_post(ir_visit_t *v)
{
    ncc_buffer_puts((ncc_buffer_t *)v->user, "  }\n");

    return true;
}

static bool on_local(ir_visit_t *v)
{
    ncc_buffer_t       *b = v->user;
    ir_local_t  *l = v->local;

    put_comment(b, &l->annots, "    ");
    ncc_buffer_puts(b, "    ");

    if (l->mutability != IR_MUT_MUTABLE) {
        ncc_buffer_printf(b, "%s ", ir_mutability_name(l->mutability));
    }
    ncc_buffer_puts(b, "local ");
    put_local_slot(b, v->module, l);
    put_span(b, v->module, l->span);
    put_annots(b, v->module, &l->annots);
    ncc_buffer_puts(b, "\n");

    return true;
}

static bool on_block_pre(ir_visit_t *v)
{
    ncc_buffer_t       *b  = v->user;
    ir_block_t  *bb = v->block;

    if (v->index || v->func->n_locals) {
        ncc_buffer_puts(b, "\n");
    }
    put_comment(b, &bb->annots, "    ");
    ncc_buffer_printf(b, "    block %s:", bb->name);
    put_span(b, v->module, bb->span);
    put_origin(b, bb->origin);
    put_annots(b, v->module, &bb->annots);
    ncc_buffer_puts(b, "\n");

    return true;
}

static bool on_instr_pre(ir_visit_t *v)
{
    ncc_buffer_t       *b  = v->user;
    ir_module_t *m  = v->module;
    ir_instr_t  *in = v->instr;

    put_comment(b, &in->annots, "      ");
    ncc_buffer_puts(b, "      ");

    if (in->dest_kind == IR_DEST_VALUE && in->dest) {
        ncc_buffer_printf(b, "%%%s: ", in->dest->name);
        put_type(b, m, in->dest->type);
        ncc_buffer_puts(b, " = ");
    }
    else if (in->dest_kind == IR_DEST_LOCAL && in->dest_local) {
        ncc_buffer_printf(b, "$%s = ", in->dest_local->name);
    }

    ncc_buffer_puts(b, ir_module_op_spelling(m, in->op));

    for (uint32_t i = 0; i < in->n_mods; i++) {
        ncc_buffer_printf(b, " %s", in->mods[i]);
    }

    return true;
}

static bool on_operand(ir_visit_t *v)
{
    ncc_buffer_t *b = v->user;

    ncc_buffer_puts(b, v->index ? ", " : " ");
    put_operand(b, v->module, v->operand);

    return true;
}

static bool on_instr_post(ir_visit_t *v)
{
    ncc_buffer_t      *b  = v->user;
    ir_instr_t *in = v->instr;

    put_span(b, v->module, in->span);
    put_origin(b, in->origin);
    put_annots(b, v->module, &in->annots);
    ncc_buffer_puts(b, "\n");

    return true;
}

char *ir_module_print(ir_module_t *m)
{
    if (!m) {
        return nullptr;
    }

    ncc_buffer_t *b = ncc_buffer_empty();
    ir_walk_t *w = ir_walk_new(b);

    ir_walk_on_module(w, on_module_pre, on_module_post, nullptr);
    ir_walk_on_global(w, on_global, nullptr);
    ir_walk_on_func(w, on_func_pre, on_func_post, nullptr);
    ir_walk_on_local(w, on_local, nullptr);
    ir_walk_on_block(w, on_block_pre, nullptr, nullptr);
    ir_walk_on_instr(w, on_instr_pre, on_instr_post, nullptr);
    ir_walk_on_operand(w, on_operand, nullptr);

    ir_walk_run(w, m);
    ir_walk_free(w);

    return ncc_buffer_take(b);
}

char *ir_text_format(const char *text, int32_t width)
{
    ncc_grammar_t *g = text ? ir_grammar() : nullptr;

    if (!g) {
        return nullptr;
    }

    ncc_grammar_set_start_id(g, ir_grammar_nt("module"));

    size_t              len = strlen(text);
    ncc_token_stream_t *ts  = ir_lex(text, len);
    ncc_pwz_parser_t   *p   = ncc_pwz_new(g);
    char               *out = nullptr;

    if (ncc_pwz_parse(p, ts)) {
        ncc_print_opts_t o = {
            .source = {.data = (char *)text, .u8_bytes = len},
            .width  = width,
            .indent = 2,
        };

        out = ncc_tree_print(g, ncc_pwz_get_tree(p), &o);
    }

    ncc_pwz_free(p);
    ncc_token_stream_free(ts);

    return out;
}
