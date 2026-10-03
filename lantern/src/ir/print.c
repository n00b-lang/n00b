#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "private/ir/internal.h"
#include "ir/grammar.h"
#include "ir/layout.h"
#include "ir/shape.h"
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

static void put_type(core_buffer_t *b, ir_module_t *m, th_type *t)
{
    core_buffer_puts(b, ir_type_text(m, t));
}

static void put_span(core_buffer_t *b, ir_module_t *m, ir_span_t s)
{
    (void)m;

    if (!ir_span_known(s) || s.implied) {
        return;
    }
    core_buffer_printf(b, " !loc(%d, %u, %u, %u, %u, %u, %u)", s.file, s.start_byte,
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

static void put_origin(core_buffer_t *b, const ir_origin_t *o)
{
    for (; o; o = o->parent) {
        core_buffer_printf(b, " !origin(%s, ", ir_origin_kind_name(o->kind));

        if (o->agent) {
            ir_quote(b, o->agent);
        }
        else {
            core_buffer_puts(b, "\"\"");
        }
        core_buffer_printf(b, ", %d, %u, %u, %u, %u, %u, %u)", o->span.file,
                  o->span.start_byte, o->span.end_byte, o->span.start_line,
                  o->span.start_col, o->span.end_line, o->span.end_col);
    }
}

static void put_annots(core_buffer_t *b, ir_module_t *m, ir_annots_t *a)
{
    for (uint32_t i = 0; i < ir_annot_count(a); i++) {
        ir_annot_t *an = ir_annot_at(a, i);

        if (!strcmp(an->key, "ir.comment")) {
            continue;
        }

        core_buffer_printf(b, " !%s(", an->key);

        switch (an->kind) {
        case IR_ANNOT_BOOL:
            core_buffer_puts(b, an->b ? "true" : "false");
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
            if (an->v && an->v->kind == IR_VAL_LOCAL) {
                core_buffer_printf(b, "$%s", an->v->name ? an->v->name : "?");
            }
            else if (an->v && (an->v->kind == IR_VAL_GLOBAL
                               || an->v->kind == IR_VAL_FUNC)) {
                core_buffer_printf(b, "@%s", an->v->name ? an->v->name : "?");
            }
            else {
                core_buffer_printf(b, "%%%s",
                                   an->v && an->v->name ? an->v->name : "?");
            }
            break;
        }

        if (an->derived_from) {
            const char *p = ir_prop_name(an->derived_from);

            core_buffer_printf(b, " from %s", p ? p : "?");
        }
        core_buffer_puts(b, ")");
    }
}

static void put_comment(core_buffer_t *b, ir_annots_t *a, const char *indent)
{
    ir_annot_t *c = ir_annot_get(a, "ir.comment");

    if (!c || c->kind != IR_ANNOT_STR || !c->s) {
        return;
    }

    const char *p = c->s;

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t      n  = nl ? (size_t)(nl - p) : strlen(p);

        core_buffer_printf(b, "%s# ", indent);
        core_buffer_append(b, p, n);
        core_buffer_puts(b, "\n");

        if (!nl) {
            break;
        }
        p = nl + 1;
    }
}

static void put_operand(core_buffer_t *b, ir_module_t *m, const ir_operand_t *o)
{
    switch (o->kind) {
    case IR_OPD_VALUE:
        if (!o->value) {
            core_buffer_puts(b, "%?");
        }
        else if (o->value->kind == IR_VAL_GLOBAL
                 || o->value->kind == IR_VAL_FUNC) {
            core_buffer_printf(b, "@%s", o->value->name);
        }
        else if (o->value->kind == IR_VAL_LOCAL) {
            core_buffer_printf(b, "$%s", o->value->name);
        }
        else {
            core_buffer_printf(b, "%%%s", o->value->name);
        }
        break;

    case IR_OPD_SPREAD:
        core_buffer_printf(b, "*%%%s",
                           o->value && o->value->name ? o->value->name : "?");
        break;

    case IR_OPD_TYPE:
        put_type(b, m, o->type);
        break;

    case IR_OPD_NAME:
        ir_quote(b, o->name ? o->name : "");
        break;

    case IR_OPD_BLOCK:
        core_buffer_puts(b, o->block ? o->block->name : "?");
        break;

    case IR_OPD_DEFAULT_BLOCK:
        core_buffer_printf(b, "default: %s", o->block ? o->block->name : "?");
        break;

    case IR_OPD_CONST:
        if (o->lit.is_float) {
            if (o->lit.text) {
                core_buffer_puts(b, o->lit.text);
            }
            else {
                core_buffer_printf(b, "%.17g", o->lit.f);
            }
        }
        else {
            ir_lit_into(b, o->lit.i);
        }
        break;

    case IR_OPD_PHI_ARM:
        core_buffer_printf(b, "[%s: %%%s]", o->phi.block ? o->phi.block->name : "?",
                  o->phi.value ? o->phi.value->name : "?");
        break;

    case IR_OPD_NAMED_ARG:
        core_buffer_printf(b, "%s = ", o->named.name ? o->named.name : "?");

        if (!o->named.value) {
            core_buffer_puts(b, "out");
            break;
        }

        if (o->named.value && o->named.value->kind == IR_VAL_LOCAL) {
            core_buffer_printf(b, "$%s", o->named.value->name);
        }
        else if (o->named.value
                 && (o->named.value->kind == IR_VAL_GLOBAL
                     || o->named.value->kind == IR_VAL_FUNC)) {
            core_buffer_printf(b, "@%s", o->named.value->name);
        }
        else {
            core_buffer_printf(b, "%%%s",
                      o->named.value ? o->named.value->name : "?");
        }
        break;

    case IR_OPD_SWITCH_ARM:
        core_buffer_puts(b, "[");
        ir_lit_into(b, o->arm.value);
        core_buffer_printf(b, ": %s]",
                  o->arm.block ? o->arm.block->name : "?");
        break;

    case IR_OPD_ARG:
        break;

    case IR_OPD_OUT:
        core_buffer_puts(b, "out");
        break;

    case IR_OPD_FRESH:
        core_buffer_printf(b, "fresh %%%s",
            o->value ? o->value->name : "?");
        break;

    case IR_OPD_MISSING:
        core_buffer_puts(b, "missing");
        break;
    }
}

static void put_init(core_buffer_t *b, bool has, bool is_float,
                     bool is_string, const char *text,
                     const char *string, size_t string_length,
                     double f, ir_lit_t lit)
{
    if (!has) {
        return;
    }
    core_buffer_puts(b, " = ");

    if (is_string) {
        ir_quote_n(b, string, string_length);
        return;
    }

    if (is_float) {
        if (text) {
            core_buffer_puts(b, text);
        }
        else {
            core_buffer_printf(b, "%.17g", f);
        }

        return;
    }
    ir_lit_into(b, lit);
}

static void put_global_init_value(core_buffer_t *b,
                                  const ir_global_init_value_t *value)
{
    if (!value) {
        core_buffer_puts(b, "?");
        return;
    }
    switch (value->kind) {
    case IR_GLOBAL_INIT_BOOL:
        core_buffer_puts(b, value->boolean ? "true" : "false");
        break;
    case IR_GLOBAL_INIT_INT:
        ir_lit_into(b, value->integer);
        break;
    case IR_GLOBAL_INIT_FLOAT:
        core_buffer_printf(b, "%.17g", value->floating);
        break;
    case IR_GLOBAL_INIT_STRING:
        ir_quote_n(b, value->string.bytes, value->string.length);
        break;
    case IR_GLOBAL_INIT_SYMBOL:
        core_buffer_printf(b, "@%s",
            value->symbol && value->symbol->name ? value->symbol->name : "?");
        break;
    case IR_GLOBAL_INIT_PHASE_RESULT:
        core_buffer_puts(b, "$");
        ir_quote_n(b, value->phase_result.identifier,
            value->phase_result.identifier
                ? strlen(value->phase_result.identifier) : 0);
        break;
    case IR_GLOBAL_INIT_SEQUENCE:
        core_buffer_puts(b, "[");
        for (size_t i = 0; i < value->aggregate.count; i++) {
            if (i) core_buffer_puts(b, ", ");
            put_global_init_value(b, &value->aggregate.items[i]);
        }
        core_buffer_puts(b, "]");
        break;
    case IR_GLOBAL_INIT_TUPLE:
        core_buffer_puts(b, "(");
        for (size_t i = 0; i < value->aggregate.count; i++) {
            if (i) core_buffer_puts(b, ", ");
            put_global_init_value(b, &value->aggregate.items[i]);
        }
        if (value->aggregate.count == 1) core_buffer_puts(b, ",");
        core_buffer_puts(b, ")");
        break;
    case IR_GLOBAL_INIT_RECORD:
        core_buffer_puts(b, "{");
        for (size_t i = 0; i < value->aggregate.count; i++) {
            const ir_global_init_value_t *item = &value->aggregate.items[i];
            core_buffer_printf(b, "%s%s = ", i ? ", " : " ",
                item->member ? item->member : "?");
            put_global_init_value(b, item);
        }
        core_buffer_puts(b, value->aggregate.count ? " }" : "}");
        break;
    case IR_GLOBAL_INIT_SET:
        core_buffer_puts(b, "set{");
        for (size_t i = 0; i < value->aggregate.count; i++) {
            if (i) core_buffer_puts(b, ", ");
            put_global_init_value(b, &value->aggregate.items[i]);
        }
        core_buffer_puts(b, "}");
        break;
    case IR_GLOBAL_INIT_DICTIONARY:
        core_buffer_puts(b, "dict{");
        for (size_t i = 0; i + 1 < value->aggregate.count; i += 2) {
            if (i) core_buffer_puts(b, ", ");
            put_global_init_value(b, &value->aggregate.items[i]);
            core_buffer_puts(b, " => ");
            put_global_init_value(b, &value->aggregate.items[i + 1]);
        }
        core_buffer_puts(b, "}");
        break;
    }
}

static void put_local_slot(core_buffer_t *b, ir_module_t *m, const ir_local_t *l)
{
    core_buffer_printf(b, "$%s: ", l->name);
    put_type(b, m, l->type);
    put_init(b, l->has_init, l->init_is_float, false, l->init_text,
             nullptr, 0, l->init_f, l->init);
}

static bool put_layout_val(core_buffer_t *b, ir_module_t *m,
                           const ir_layout_val_t *v)
{
    if (v->state == IR_LAYOUT_PINNED) {
        core_buffer_printf(b, "%llu", (unsigned long long)v->value);

        return true;
    }

    if (v->state == IR_LAYOUT_SYMBOLIC) {
        core_buffer_printf(b, "%s(%s", ir_symkind_name(v->sym), ir_type_text(m, v->of));

        if (v->sym == IR_SYM_OFFSETOF) {
            core_buffer_puts(b, ", ");
            ir_quote(b, v->member ? v->member : "");
        }
        core_buffer_puts(b, ")");

        return true;
    }

    return false;
}

static bool authored_anywhere(const ir_descriptor_t *d)
{
    if (d->size.authored || d->align.authored || d->stride.authored
        || d->indirect.authored || d->variant.authored) {
        return true;
    }

    for (uint32_t i = 0; i < d->n_members; i++) {
        if (d->members[i].bit_offset.authored) {
            return true;
        }
    }

    return false;
}

static void put_layouts(core_buffer_t *b, ir_module_t *m)
{
    for (size_t i = 0; i < ir_type_count(m); i++) {
        ir_typeslot_t *slot = ir_type_slot_at(m, i);
        th_type       *t    = slot->type;

        for (size_t k = 0; k < slot->n_descs; k++) {
            ir_descriptor_t *d = &slot->descs[k].d;

            if (!authored_anywhere(d)) {
                continue;
            }

            core_buffer_printf(b, "\n  layout %s", ir_type_text(m, t));

            const char *module_target = ir_module_target(m);

            if (d->target && (!module_target
                              || strcmp(d->target, module_target))) {
                core_buffer_puts(b, " for ");
                ir_quote(b, d->target);
            }
            else if (!d->target && module_target) {
                core_buffer_puts(b, " without target");
            }
            core_buffer_puts(b, " {\n");

            if (d->size.authored) {
                core_buffer_puts(b, "    size = ");
                put_layout_val(b, m, &d->size);
                core_buffer_puts(b, "\n");
            }

            if (d->align.authored) {
                core_buffer_puts(b, "    align = ");
                put_layout_val(b, m, &d->align);
                core_buffer_puts(b, "\n");
            }

            if (d->stride.authored) {
                core_buffer_puts(b, "    stride = ");
                put_layout_val(b, m, &d->stride);
                core_buffer_puts(b, "\n");
            }

            if (d->indirect.authored) {
                core_buffer_printf(b, "    indirect = %s\n",
                                   d->indirect.fat ? "fat" : "thin");

                if (d->indirect.words.authored) {
                    core_buffer_puts(b, "    words = ");
                    put_layout_val(b, m, &d->indirect.words);
                    core_buffer_puts(b, "\n");
                }
            }

            if (d->variant.authored) {
                if (d->variant.tag_bit_offset.authored
                    && d->variant.tag_bit_width.authored) {
                    uint64_t lo = d->variant.tag_bit_offset.value;
                    uint64_t hi = lo + d->variant.tag_bit_width.value;

                    core_buffer_printf(b,
                                       "    variant tag @ bits %llu..%llu\n",
                                       (unsigned long long)lo,
                                       (unsigned long long)hi);
                }
                core_buffer_printf(b, "    niche = %s\n",
                                   d->variant.niche ? "true" : "false");
            }

            for (uint32_t j = 0; j < d->n_members; j++) {
                const ir_member_layout_t *ml = &d->members[j];

                if (!ml->bit_offset.authored) {
                    continue;
                }
                core_buffer_printf(b, "    %s @ bits %llu..%llu\n", ml->name,
                          (unsigned long long)ml->bit_offset.value,
                          (unsigned long long)(ml->bit_offset.value
                                               + ml->bit_width.value));
            }
            core_buffer_puts(b, "  }\n");
        }
    }
}

static void put_typedefs(core_buffer_t *b, ir_module_t *m)
{
    for (size_t i = 0; i < ir_type_count(m); i++) {
        ir_typeslot_t *slot = ir_type_slot_at(m, i);
        th_type       *t    = slot->type;
        th_type       *def  = slot->def;

        if (!def) {
            continue;
        }
        core_buffer_printf(b, "\n  type %s = %s\n", ir_type_text(m, t),
                  ir_type_text(m, def));
    }
}

static bool on_module_pre(ir_visit_t *v)
{
    core_buffer_t       *b  = v->user;
    ir_module_t *m  = v->module;
    ir_annots_t *ma = ir_module_annots(m);

    put_comment(b, ma, "");
    core_buffer_printf(b, "module %s {\n", ir_module_name(m));
    core_buffer_printf(b, "  profile %s\n", ir_module_profile(m)->name);

    if (ir_module_target(m)) {
        core_buffer_printf(b, "  target %s\n", ir_module_target(m));
    }

    bool any = false;

    for (uint32_t i = 0; i < ir_annot_count(ma); i++) {
        if (strcmp(ir_annot_at(ma, i)->key, "ir.comment")) {
            any = true;
        }
    }

    if (any) {
        core_buffer_puts(b, "  meta");
        put_annots(b, m, ma);
        core_buffer_puts(b, "\n");
    }

    put_typedefs(b, m);
    put_layouts(b, m);

    return true;
}

static bool on_module_post(ir_visit_t *v)
{
    core_buffer_puts((core_buffer_t *)v->user, "}\n");

    return true;
}

static bool on_global(ir_visit_t *v)
{
    core_buffer_t       *b = v->user;
    ir_module_t *m = v->module;
    ir_global_t *g = v->global;

    core_buffer_puts(b, "\n");
    put_comment(b, &g->annots, "  ");
    core_buffer_puts(b, "  ");

    if (g->is_static) {
        core_buffer_puts(b, "static ");
    }

    if (g->is_extern) {
        core_buffer_puts(b, "extern ");
    }

    if (g->mutability != IR_MUT_MUTABLE) {
        core_buffer_printf(b, "%s ", ir_mutability_name(g->mutability));
    }
    core_buffer_printf(b, "global @%s: ", g->name);
    put_type(b, m, g->type);
    if (g->has_static_initializer) {
        core_buffer_puts(b, " = ");
        put_global_init_value(b, g->static_initializer);
    }
    put_span(b, m, g->span);
    put_origin(b, g->origin);
    put_annots(b, m, &g->annots);
    core_buffer_puts(b, "\n");

    return true;
}

static void put_signature(core_buffer_t *b, ir_module_t *m, ir_func_t *f)
{
    core_buffer_printf(b, "func @%s(", f->name);

    bool first = true;

    for (uint32_t i = 0; i < f->n_params; i++) {
        if (f->params[i]->is_keyword || f->params[i]->is_rest) continue;
        core_buffer_printf(b, "%s%%%s: ", first ? "" : ", ", f->params[i]->name);
        put_type(b, m, f->params[i]->type);
        put_annots(b, m, ir_value_annots(f->params[i]));
        first = false;
    }

    if (!f->n_params) {
        for (size_t i = 0; i < th_func_nparams(f->type); i++) {
            const th_param *parameter = th_func_param(f->type, i);
            core_buffer_printf(b, "%s%%arg%zu: ", first ? "" : ", ", i);
            put_type(b, m, parameter->type);
            if (ir_func_parameter_is_optional(f, (uint32_t)i))
                core_buffer_puts(b, " !ir.optional_parameter(true)");
            first = false;
        }
    }

    const th_param *rest = th_func_rest(f->type);

    if (rest) {
        core_buffer_printf(b, "%s*", first ? "" : ", ");
        ir_value_t *rest_value = nullptr;
        for (uint32_t i = 0; i < f->n_params; i++)
            if (f->params[i]->is_rest) rest_value = f->params[i];
        if (rest_value) {
            core_buffer_printf(b, "%%%s: ", rest_value->name);
        }
        put_type(b, m, rest->type);
        if (rest_value) put_annots(b, m, ir_value_annots(rest_value));
        first = false;
    }

    for (size_t i = 0; i < th_func_nkw(f->type); i++) {
        const th_kwparam *k = th_func_kw(f->type, i);

        core_buffer_printf(b, "%s%s: ", first ? "" : ", ", k->name);
        put_type(b, m, k->type);
        ir_value_t *kw_value = ir_value_find(f, k->name);
        if (kw_value) put_annots(b, m, ir_value_annots(kw_value));
        first = false;
    }
    core_buffer_puts(b, ") -> ");
    put_type(b, m, th_func_ret(f->type));
    th_type *error = th_func_error(f->type);
    if (error) {
        core_buffer_puts(b, " fail: ");
        put_type(b, m, error);
    }
}

static bool on_func_pre(ir_visit_t *v)
{
    core_buffer_t       *b = v->user;
    ir_module_t *m = v->module;
    ir_func_t   *f = v->func;

    core_buffer_puts(b, "\n");
    put_comment(b, &f->annots, "  ");
    core_buffer_puts(b, "  ");

    switch (f->linkage) {
    case IR_LINK_INTERNAL:
        core_buffer_puts(b, "static ");
        break;
    case IR_LINK_EXTERN:
        core_buffer_puts(b, "extern ");
        break;
    default:
        break;
    }

    put_signature(b, m, f);
    put_span(b, m, f->span);
    put_origin(b, f->origin);
    put_annots(b, m, &f->annots);
    core_buffer_puts(b, " {\n");

    return true;
}

static bool on_func_post(ir_visit_t *v)
{
    core_buffer_puts((core_buffer_t *)v->user, "  }\n");

    return true;
}

static bool on_local(ir_visit_t *v)
{
    core_buffer_t       *b = v->user;
    ir_local_t  *l = v->local;

    put_comment(b, &l->annots, "    ");
    core_buffer_puts(b, "    ");

    if (l->mutability != IR_MUT_MUTABLE) {
        core_buffer_printf(b, "%s ", ir_mutability_name(l->mutability));
    }
    core_buffer_puts(b, "local ");
    put_local_slot(b, v->module, l);
    put_span(b, v->module, l->span);
    put_annots(b, v->module, &l->annots);
    core_buffer_puts(b, "\n");

    return true;
}

static bool on_block_pre(ir_visit_t *v)
{
    core_buffer_t       *b  = v->user;
    ir_block_t  *bb = v->block;

    if (v->index || v->func->n_locals) {
        core_buffer_puts(b, "\n");
    }
    put_comment(b, &bb->annots, "    ");
    core_buffer_printf(b, "    block %s:", bb->name);
    put_span(b, v->module, bb->span);
    put_origin(b, bb->origin);
    put_annots(b, v->module, &bb->annots);
    core_buffer_puts(b, "\n");

    return true;
}

static bool on_instr_pre(ir_visit_t *v)
{
    core_buffer_t       *b  = v->user;
    ir_module_t *m  = v->module;
    ir_instr_t  *in = v->instr;

    put_comment(b, &in->annots, "      ");
    core_buffer_puts(b, "      ");

    if (in->dest_kind == IR_DEST_VALUE && in->dest) {
        core_buffer_printf(b, "%%%s: ", in->dest->name);
        put_type(b, m, in->dest->type);
        core_buffer_puts(b, " = ");
    }
    else if (in->dest_kind == IR_DEST_LOCAL && in->dest_local) {
        core_buffer_printf(b, "$%s = ", in->dest_local->name);
    }

    core_buffer_puts(b, ir_module_op_spelling(m, in->op));

    for (uint32_t i = 0; i < in->n_mods; i++) {
        core_buffer_printf(b, " %s", in->mods[i]);
    }

    return true;
}

static void
put_shape_constraint(core_buffer_t *b, const ir_shape_t *shape)
{
    if (!shape) return;
    uint32_t rank = ir_shape_rank(shape);
    core_buffer_printf(b, " !ir.shape_constraint(rank=%u", rank);
    for (uint32_t i = 0; i < rank; i++) {
        ir_shape_extent_t extent = {0};
        if (!ir_shape_extent(shape, i, &extent)) continue;
        core_buffer_printf(b, ",dim%u=", i);
        switch (extent.kind) {
        case IR_SHAPE_EXTENT_STATIC:
            core_buffer_printf(b, "static:%llu",
                (unsigned long long)extent.static_value);
            break;
        case IR_SHAPE_EXTENT_COMPTIME:
            core_buffer_puts(b, "comptime");
            if (extent.producer)
                core_buffer_printf(b, ":producer=%s", extent.producer->name);
            else if (extent.producer_identifier)
                core_buffer_printf(b, ":producer=%s",
                                   extent.producer_identifier);
            break;
        case IR_SHAPE_EXTENT_RUNTIME:
            core_buffer_puts(b, "runtime");
            if (extent.value)
                core_buffer_printf(b, ":value=%%%s", extent.value->name);
            break;
        case IR_SHAPE_EXTENT_UNRESOLVED:
            core_buffer_puts(b, "unresolved");
            break;
        }
    }
    core_buffer_puts(b, ")");
}

static bool on_operand(ir_visit_t *v)
{
    core_buffer_t *b = v->user;

    core_buffer_puts(b, v->index ? ", " : " ");
    put_operand(b, v->module, v->operand);

    return true;
}

static bool on_instr_post(ir_visit_t *v)
{
    core_buffer_t      *b  = v->user;
    ir_instr_t *in = v->instr;

    put_span(b, v->module, in->span);
    put_shape_constraint(b, in->shape_constraint);
    put_origin(b, in->origin);
    put_annots(b, v->module, &in->annots);
    core_buffer_puts(b, "\n");

    return true;
}

char *ir_module_print(ir_module_t *m)
{
    if (!m) {
        return nullptr;
    }

    core_buffer_t *b = core_buffer_empty();
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

    return core_buffer_take(b);
}

char *ir_text_format(const char *text, int32_t width)
{
    parse_grammar_t *g = text ? ir_grammar() : nullptr;

    if (!g) {
        return nullptr;
    }

    size_t              len = strlen(text);
    parse_token_stream_t *ts  = ir_lex(text, len);
    parse_pwz_parser_t   *p
        = parse_pwz_new_for_start(g, ir_grammar_nt("module"));
    char               *out = nullptr;

    if (parse_pwz_parse(p, ts)) {
        parse_print_opts_t o = {
            .source = {.data = (char *)text, .u8_bytes = len},
            .width  = width,
            .indent = 2,
        };

        out = parse_tree_print(g, parse_pwz_get_tree(p), &o);
    }

    parse_pwz_free(p);
    scan_token_stream_free(ts);

    return out;
}
