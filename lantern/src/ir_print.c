#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/text.h"

const char *ir_type_text(ir_module_t *m, th_type *t)
{
    if (!m || !t) {
        return "?";
    }

    const char *s = th_print(ir_ctx_types(m->ctx), t);

    return s ? s : "?";
}

static void put_type(ir_sb *b, ir_module_t *m, th_type *t)
{
    ir_sb_str(b, ir_type_text(m, t));
}

static void put_span(ir_sb *b, ir_span_t s)
{
    if (!ir_span_known(s)) {
        return;
    }
    ir_sb_fmt(b, " !loc(%d, %u, %u, %u, %u, %u, %u)", s.file, s.start_byte,
              s.end_byte, s.start_line, s.start_col, s.end_line, s.end_col);
}

static const char *origin_kind_name(ir_origin_kind_t k)
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

static void put_origin(ir_sb *b, const ir_origin_t *o)
{
    for (; o; o = o->parent) {
        ir_sb_fmt(b, " !origin(%s, ", origin_kind_name(o->kind));

        if (o->agent) {
            ir_sb_quote(b, o->agent);
        }
        else {
            ir_sb_str(b, "\"\"");
        }
        ir_sb_fmt(b, ", %d, %u, %u, %u, %u, %u, %u)", o->span.file,
                  o->span.start_byte, o->span.end_byte, o->span.start_line,
                  o->span.start_col, o->span.end_line, o->span.end_col);
    }
}

static void put_annots(ir_sb *b, ir_module_t *m, ir_annots_t *a)
{
    for (uint32_t i = 0; i < ir_annot_count(a); i++) {
        ir_annot_t *an = ir_annot_at(a, i);

        if (!strcmp(an->key, "ir.comment")) {
            continue;
        }

        ir_sb_fmt(b, " !%s(", an->key);

        switch (an->kind) {
        case IR_ANNOT_BOOL:
            ir_sb_str(b, an->b ? "true" : "false");
            break;
        case IR_ANNOT_INT:
            ir_sb_fmt(b, "%" PRId64, an->i);
            break;
        case IR_ANNOT_STR:
            ir_sb_quote(b, an->s ? an->s : "");
            break;
        case IR_ANNOT_TYPE:
            put_type(b, m, an->t);
            break;
        case IR_ANNOT_VALUE:
            ir_sb_fmt(b, "%%%s", an->v && an->v->name ? an->v->name : "?");
            break;
        }

        if (an->derived_from) {
            const char *p = ir_prop_name(an->derived_from);

            ir_sb_fmt(b, " from %s", p ? p : "?");
        }
        ir_sb_str(b, ")");
    }
}

static void put_comment(ir_sb *b, ir_annots_t *a, const char *indent)
{
    ir_annot_t *c = ir_annot_get(a, "ir.comment");

    if (!c || c->kind != IR_ANNOT_STR || !c->s) {
        return;
    }

    const char *p = c->s;

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t      n  = nl ? (size_t)(nl - p) : strlen(p);

        ir_sb_fmt(b, "%s# ", indent);
        ir_sb_put(b, p, n);
        ir_sb_str(b, "\n");

        if (!nl) {
            break;
        }
        p = nl + 1;
    }
}

static void put_operand(ir_sb *b, ir_module_t *m, const ir_operand_t *o)
{
    switch (o->kind) {
    case IR_OPD_VALUE:
        if (!o->value) {
            ir_sb_str(b, "%?");
        }
        else if (o->value->kind == IR_VAL_GLOBAL
                 || o->value->kind == IR_VAL_FUNC) {
            ir_sb_fmt(b, "@%s", o->value->name);
        }
        else if (o->value->kind == IR_VAL_LOCAL) {
            ir_sb_fmt(b, "$%s", o->value->name);
        }
        else {
            ir_sb_fmt(b, "%%%s", o->value->name);
        }
        break;

    case IR_OPD_TYPE:
        put_type(b, m, o->type);
        break;

    case IR_OPD_NAME:
        ir_sb_quote(b, o->name ? o->name : "");
        break;

    case IR_OPD_BLOCK:
        ir_sb_str(b, o->block ? o->block->name : "?");
        break;

    case IR_OPD_DEFAULT_BLOCK:
        ir_sb_fmt(b, "default: %s", o->block ? o->block->name : "?");
        break;

    case IR_OPD_CONST:
        if (o->lit.is_float) {
            ir_sb_fmt(b, "%.17g", o->lit.f);
        }
        else {
            ir_sb_fmt(b, "%" PRId64, o->lit.i);
        }
        break;

    case IR_OPD_PHI_ARM:
        ir_sb_fmt(b, "[%s: %%%s]", o->phi.block ? o->phi.block->name : "?",
                  o->phi.value ? o->phi.value->name : "?");
        break;

    case IR_OPD_NAMED_ARG:
        ir_sb_fmt(b, "%s = ", o->named.name ? o->named.name : "?");

        if (o->named.value && o->named.value->kind == IR_VAL_LOCAL) {
            ir_sb_fmt(b, "$%s", o->named.value->name);
        }
        else if (o->named.value
                 && (o->named.value->kind == IR_VAL_GLOBAL
                     || o->named.value->kind == IR_VAL_FUNC)) {
            ir_sb_fmt(b, "@%s", o->named.value->name);
        }
        else {
            ir_sb_fmt(b, "%%%s",
                      o->named.value ? o->named.value->name : "?");
        }
        break;

    case IR_OPD_SWITCH_ARM:
        ir_sb_fmt(b, "[%" PRId64 ": %s]", o->arm.value,
                  o->arm.block ? o->arm.block->name : "?");
        break;

    case IR_OPD_ARG:
        break;
    }
}

static void put_local_slot(ir_sb *b, ir_module_t *m, const ir_local_t *l)
{
    ir_sb_fmt(b, "$%s: ", l->name);
    put_type(b, m, l->type);
}

static void put_instr(ir_sb *b, ir_module_t *m, const ir_instr_t *in)
{
    put_comment(b, (ir_annots_t *)&in->annots, "      ");
    ir_sb_str(b, "      ");

    if (in->dest_kind == IR_DEST_VALUE && in->dest) {
        ir_sb_fmt(b, "%%%s: ", in->dest->name);
        put_type(b, m, in->dest->type);
        ir_sb_str(b, " = ");
    }
    else if (in->dest_kind == IR_DEST_LOCAL && in->dest_local) {
        ir_sb_fmt(b, "$%s = ", in->dest_local->name);
    }

    ir_sb_str(b, ir_module_op_spelling(m, in->op));

    for (uint32_t i = 0; i < in->n_mods; i++) {
        ir_sb_fmt(b, " %s", in->mods[i]);
    }

    for (uint32_t i = 0; i < in->n_operands; i++) {
        ir_sb_str(b, i ? ", " : " ");
        put_operand(b, m, &in->operands[i]);
    }

    put_span(b, in->span);
    put_origin(b, in->origin);
    put_annots(b, m, (ir_annots_t *)&in->annots);
    ir_sb_str(b, "\n");
}

static void put_func(ir_sb *b, ir_module_t *m, ir_func_t *f)
{
    put_comment(b, &f->annots, "  ");
    ir_sb_str(b, "  ");

    switch (f->linkage) {
    case IR_LINK_INTERNAL:
        ir_sb_str(b, "static ");
        break;
    case IR_LINK_EXTERN:
        ir_sb_str(b, "extern ");
        break;
    default:
        break;
    }

    ir_sb_fmt(b, "func @%s(", f->name);

    bool first = true;

    for (uint32_t i = 0; i < f->n_params; i++) {
        ir_sb_fmt(b, "%s%%%s: ", first ? "" : ", ", f->params[i]->name);
        put_type(b, m, f->params[i]->type);
        first = false;
    }

    const th_param *rest = th_func_rest(f->type);

    if (rest) {
        ir_sb_fmt(b, "%s*", first ? "" : ", ");
        put_type(b, m, rest->type);
        first = false;
    }

    for (size_t i = 0; i < th_func_nkw(f->type); i++) {
        const th_kwparam *k = th_func_kw(f->type, i);

        ir_sb_fmt(b, "%s%s: ", first ? "" : ", ", k->name);
        put_type(b, m, k->type);
        first = false;
    }
    ir_sb_str(b, ") -> ");
    put_type(b, m, th_func_ret(f->type));
    put_span(b, f->span);
    put_origin(b, f->origin);
    put_annots(b, m, &f->annots);
    ir_sb_str(b, " {\n");

    for (uint32_t i = 0; i < f->n_locals; i++) {
        put_comment(b, &f->locals[i]->annots, "    ");
        ir_sb_str(b, "    local ");
        put_local_slot(b, m, f->locals[i]);
        put_span(b, f->locals[i]->span);
        put_annots(b, m, &f->locals[i]->annots);
        ir_sb_str(b, "\n");
    }

    if (f->n_locals && f->n_blocks) {
        ir_sb_str(b, "\n");
    }

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        ir_block_t *bb = f->blocks[i];

        if (i) {
            ir_sb_str(b, "\n");
        }
        put_comment(b, &bb->annots, "    ");
        ir_sb_fmt(b, "    block %s:", bb->name);
        put_span(b, bb->span);
        put_origin(b, bb->origin);
        put_annots(b, m, &bb->annots);
        ir_sb_str(b, "\n");

        for (uint32_t k = 0; k < bb->n_instrs; k++) {
            put_instr(b, m, bb->instrs[k]);
        }
    }
    ir_sb_str(b, "  }\n");
}

char *ir_module_print(ir_module_t *m)
{
    if (!m) {
        return nullptr;
    }

    ir_sb b = {0};

    put_comment(&b, ir_module_annots(m), "");
    ir_sb_fmt(&b, "module %s {\n", ir_module_name(m));
    ir_sb_fmt(&b, "  profile %s\n", ir_module_profile(m)->name);

    if (ir_module_target(m)) {
        ir_sb_fmt(&b, "  target %s\n", ir_module_target(m));
    }

    ir_annots_t *ma = ir_module_annots(m);

    if (ir_annot_count(ma)) {
        bool any = false;

        for (uint32_t i = 0; i < ir_annot_count(ma); i++) {
            if (strcmp(ir_annot_at(ma, i)->key, "ir.comment")) {
                any = true;
            }
        }

        if (any) {
            ir_sb_str(&b, "  meta");
            put_annots(&b, m, ma);
            ir_sb_str(&b, "\n");
        }
    }

    for (size_t i = 0; i < ir_global_count(m); i++) {
        ir_global_t *g = ir_global_at(m, i);

        ir_sb_str(&b, "\n");
        put_comment(&b, &g->annots, "  ");
        ir_sb_str(&b, "  ");

        if (g->is_static) {
            ir_sb_str(&b, "static ");
        }

        if (g->is_extern) {
            ir_sb_str(&b, "extern ");
        }
        ir_sb_fmt(&b, "global @%s: ", g->name);
        put_type(&b, m, g->type);
        put_span(&b, g->span);
        put_origin(&b, g->origin);
        put_annots(&b, m, &g->annots);
        ir_sb_str(&b, "\n");
    }

    for (size_t i = 0; i < ir_func_count(m); i++) {
        ir_sb_str(&b, "\n");
        put_func(&b, m, ir_func_at(m, i));
    }
    ir_sb_str(&b, "}\n");

    return ir_sb_take(&b);
}
