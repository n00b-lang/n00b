#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/layout.h"
#include "ir/text.h"
#include "ir/walk.h"

#include "lib/dict.h"

const char *ir_layout_state_name(ir_layout_state_t s)
{
    switch (s) {
    case IR_LAYOUT_PINNED:
        return "pinned";
    case IR_LAYOUT_SYMBOLIC:
        return "symbolic";
    case IR_LAYOUT_RESOLVED:
        return "resolved";
    default:
        return "unset";
    }
}

const char *ir_symkind_name(ir_symkind_t k)
{
    switch (k) {
    case IR_SYM_ALIGNOF:
        return "alignof";
    case IR_SYM_OFFSETOF:
        return "offsetof";
    default:
        return "sizeof";
    }
}

bool ir_symkind_from_name(const char *s, ir_symkind_t *out)
{
    static const struct {
        const char  *name;
        ir_symkind_t k;
    } table[] = {
        {"sizeof", IR_SYM_SIZEOF},
        {"alignof", IR_SYM_ALIGNOF},
        {"offsetof", IR_SYM_OFFSETOF},
    };

    for (size_t i = 0; s && i < sizeof(table) / sizeof(table[0]); i++) {
        if (!strcmp(table[i].name, s)) {
            if (out) {
                *out = table[i].k;
            }

            return true;
        }
    }

    return false;
}

bool ir_layout_is_settled(const ir_layout_val_t *v)
{
    return v && (v->state == IR_LAYOUT_PINNED || v->state == IR_LAYOUT_RESOLVED);
}

static bool same_target(const char *a, const char *b)
{
    return a && b ? !strcmp(a, b) : a == b;
}

ir_descriptor_t *ir_layout_of(ir_module_t *m, th_type *t, const char *target)
{
    ir_typeslot_t *s = ir_type_slot(m, t);

    if (!s) {
        return nullptr;
    }

    const char *want = target ? target : m->target;

    for (size_t i = 0; i < s->n_descs; i++) {
        if (same_target(s->descs[i].target, want)) {
            return &s->descs[i].d;
        }
    }

    return nullptr;
}

ir_descriptor_t *ir_layout_ensure(ir_module_t *m, th_type *t,
                                  const char *target)
{
    ir_descriptor_t *found = ir_layout_of(m, t, target);

    if (found) {
        return found;
    }

    ir_typeslot_t *s = ir_type_slot(m, t);

    if (!s) {
        return nullptr;
    }

    s->descs = ir_grow(m->ctx, s->descs, &s->cap_descs, s->n_descs + 1,
                       sizeof(ir_descentry_t));

    if (!s->descs) {
        return nullptr;
    }

    ir_descentry_t *e = &s->descs[s->n_descs++];

    *e = (ir_descentry_t){.target = ir_strdup(m->ctx,
                                              target ? target : m->target)};

    e->d.type   = t;
    e->d.target = e->target;

    return &e->d;
}

size_t ir_layout_count(ir_module_t *m, th_type *t)
{
    ir_typeslot_t *s = ir_type_slot(m, t);

    return s ? s->n_descs : 0;
}

static bool pin(ir_layout_val_t *v, uint64_t value)
{
    if (v->state == IR_LAYOUT_PINNED && v->value != value) {
        return false;
    }

    *v = (ir_layout_val_t){.state    = IR_LAYOUT_PINNED,
                           .authored = true,
                           .value    = value};

    return true;
}

bool ir_layout_pin_size(ir_module_t *m, th_type *t, uint64_t bytes)
{
    ir_descriptor_t *d = ir_layout_ensure(m, t, nullptr);

    return d && pin(&d->size, bytes);
}

bool ir_layout_pin_stride(ir_module_t *m, th_type *t, uint64_t bytes)
{
    ir_descriptor_t *d = ir_layout_ensure(m, t, nullptr);

    return d && pin(&d->stride, bytes);
}

bool ir_layout_pin_align(ir_module_t *m, th_type *t, uint64_t bytes)
{
    ir_descriptor_t *d = ir_layout_ensure(m, t, nullptr);

    if (!d || !bytes || (bytes & (bytes - 1))) {
        return false;
    }

    return pin(&d->align, bytes);
}

static void drop_members(void *p)
{
    ncc_dict_free(p);
    ncc_free(p);
}

static ir_member_layout_t *member_slot(ir_module_t *m, ir_descriptor_t *d,
                                       const char *field)
{
    if (!d->member_index) {
        ncc_dict_t *ix = ncc_alloc(ncc_dict_t);

        ncc_dict_init(ix, ncc_hash_cstring, ncc_dict_cstr_eq);
        ir_ctx_on_free(m->ctx, drop_members, ix);
        d->member_index = ix;
    }

    bool   found = false;
    size_t at    = (size_t)(uintptr_t)ncc_dict_get((ncc_dict_t *)
                                                       d->member_index,
                                                   field, &found);

    if (found) {
        return &d->members[at];
    }

    size_t cap = d->n_members;

    d->members = ir_grow(m->ctx, d->members, &cap, (size_t)d->n_members + 1,
                         sizeof(ir_member_layout_t));

    if (!d->members) {
        return nullptr;
    }

    ir_member_layout_t *slot = &d->members[d->n_members];

    *slot = (ir_member_layout_t){.name = ir_strdup(m->ctx, field)};
    ncc_dict_put((ncc_dict_t *)d->member_index, slot->name, d->n_members);
    d->n_members++;

    return slot;
}

bool ir_layout_pin_member(ir_module_t *m, th_type *t, const char *field,
                          uint64_t bit_offset, uint64_t bit_width)
{
    ir_descriptor_t *d = ir_layout_ensure(m, t, nullptr);

    if (!d || !field || !bit_width) {
        return false;
    }

    ir_member_layout_t *slot = member_slot(m, d, field);

    return slot && pin(&slot->bit_offset, bit_offset)
        && pin(&slot->bit_width, bit_width);
}

bool ir_layout_pin_variant(ir_module_t *m, th_type *t, ir_variant_repr_t r)
{
    ir_descriptor_t *d = ir_layout_ensure(m, t, nullptr);

    if (!d) {
        return false;
    }
    r.tag_bit_offset.state    = IR_LAYOUT_PINNED;
    r.tag_bit_offset.authored = true;
    r.tag_bit_width.state     = IR_LAYOUT_PINNED;
    r.tag_bit_width.authored  = true;
    d->variant             = r;

    return true;
}

typedef struct {
    ir_module_t *m;
    ir_diags_t  *d;
    bool         ok;
    bool         settled;
    th_type    **open;
    uint32_t     n_open;
    size_t       cap_open;
} L;

static void refuse(L *l, th_type *t, const char *what, ...)
{
    l->ok = false;

    if (!l->d) {
        return;
    }

    ir_diag_t *dg = ir_diag_begin(l->d, IR_DIAG_LAYOUT_PIN,
                                  ir_module_span(l->m), "this layout");

    if (!dg) {
        return;
    }

    ncc_buffer_t *b = ncc_buffer_empty();
    va_list ap;

    va_start(ap, what);
    ncc_buffer_vprintf(b, what, ap);
    va_end(ap);

    ncc_buffer_printf(b, " (laying out %s)", ir_type_text(l->m, t));

    char *msg = ncc_buffer_take(b);

    ir_diag_message(dg, l->d, msg);
    ncc_free(msg);
}

static ir_layout_val_t resolved(uint64_t v)
{
    return (ir_layout_val_t){.state = IR_LAYOUT_RESOLVED, .value = v};
}

static ir_layout_val_t symbolic(ir_symkind_t k, th_type *of,
                                const char *member)
{
    return (ir_layout_val_t){.state  = IR_LAYOUT_SYMBOLIC,
                             .sym    = k,
                             .of     = of,
                             .member = member};
}

static uint64_t round_up(uint64_t v, uint64_t to)
{
    return to ? (v + to - 1) / to * to : v;
}

static bool lay_out(L *l, th_type *t, ir_descriptor_t **out);

static bool settled_size(L *l, th_type *t, uint64_t *bytes, uint64_t *align)
{
    ir_descriptor_t *d = nullptr;

    if (!lay_out(l, t, &d) || !d) {
        return false;
    }

    if (!ir_layout_is_settled(&d->size) || !ir_layout_is_settled(&d->align)) {
        return false;
    }
    *bytes = d->size.value;
    *align = d->align.value;

    return true;
}

static uint64_t natural_align(uint64_t size)
{
    uint64_t a = 1;

    while (a < size && a <= UINT64_MAX / 2) {
        a *= 2;
    }

    return a;
}

static bool lay_out_scalar(L *l, th_type *t, ir_descriptor_t *d)
{
    ir_machine_t k = ir_machine_of(l->m, t);

    if (k.class == IR_CLASS_VOID) {
        if (d->size.state == IR_LAYOUT_UNSET) {
            d->size = resolved(0);
        }

        if (d->align.state == IR_LAYOUT_UNSET) {
            d->align = resolved(1);
        }

        return true;
    }

    if (th_kind_of(t) == TH_INDIRECTION
        && d->indirect.words.state == IR_LAYOUT_UNSET) {
        d->indirect = (ir_indirect_repr_t){.fat = false, .words = resolved(1)};
    }

    if (k.class == IR_CLASS_PTR || k.class == IR_CLASS_OPAQUE) {
        if (d->size.state == IR_LAYOUT_UNSET) {
            d->size = symbolic(IR_SYM_SIZEOF, t, nullptr);
        }

        if (d->align.state == IR_LAYOUT_UNSET) {
            d->align = symbolic(IR_SYM_ALIGNOF, t, nullptr);
        }

        return true;
    }

    uint64_t bytes = (k.width + 7) / 8;

    if (d->size.state == IR_LAYOUT_UNSET) {
        d->size = resolved(bytes);
    }

    if (d->align.state == IR_LAYOUT_UNSET) {
        d->align = resolved(natural_align(bytes));
    }

    return true;
}

static bool lay_out_record(L *l, th_type *t, th_type *rec, ir_descriptor_t *d)
{
    size_t   n    = th_record_len(rec);
    uint64_t at   = 0;
    uint64_t most = 1;
    bool     firm = true;

    for (size_t i = 0; i < n; i++) {
        const th_member    *mem  = th_record_member(rec, i);
        ir_member_layout_t *slot = member_slot(l->m, d, mem->name);
        uint64_t            size = 0;
        uint64_t            algn = 0;

        if (!slot) {
            return false;
        }

        if (!settled_size(l, mem->type, &size, &algn)) {
            firm = false;
            continue;
        }
        most = algn > most ? algn : most;

        if (slot->bit_offset.state == IR_LAYOUT_PINNED) {
            if (slot->bit_offset.value % 8 == 0) {
                at = slot->bit_offset.value / 8 + size;
            }
        }
        else {
            at             = round_up(at, algn);
            slot->bit_offset = resolved(at * 8);
            at += size;
        }

        if (slot->bit_width.state != IR_LAYOUT_PINNED) {
            slot->bit_width = resolved(size * 8);
        }
    }

    if (!firm) {
        if (d->size.state == IR_LAYOUT_UNSET) {
            d->size = symbolic(IR_SYM_SIZEOF, t, nullptr);
        }

        if (d->align.state == IR_LAYOUT_UNSET) {
            d->align = symbolic(IR_SYM_ALIGNOF, t, nullptr);
        }

        return true;
    }

    if (d->align.state == IR_LAYOUT_UNSET) {
        d->align = resolved(most);
    }

    uint64_t want = round_up(at, ir_layout_is_settled(&d->align)
                                     ? d->align.value
                                     : most);

    if (d->size.state == IR_LAYOUT_PINNED) {
        if (d->size.value < at) {
            refuse(l, t,
                   "the pinned size %llu is smaller than the %llu bytes its "
                   "members need", (unsigned long long)d->size.value,
                   (unsigned long long)at);

            return false;
        }
    }
    else if (d->size.state == IR_LAYOUT_UNSET) {
        d->size = resolved(want);
    }

    return true;
}

static bool lay_out_sequence(L *l, th_type *t, ir_descriptor_t *d)
{
    th_type *elem = th_seq_elem(t);
    uint64_t size = 0;
    uint64_t algn = 0;
    uint64_t n    = 0;

    if (!elem || !settled_size(l, elem, &size, &algn)) {
        if (d->size.state == IR_LAYOUT_UNSET) {
            d->size = symbolic(IR_SYM_SIZEOF, t, nullptr);
        }

        if (d->align.state == IR_LAYOUT_UNSET) {
            d->align = symbolic(IR_SYM_ALIGNOF, t, nullptr);
        }

        return true;
    }

    if (d->stride.state == IR_LAYOUT_UNSET) {
        d->stride = resolved(round_up(size, algn));
    }

    if (d->align.state == IR_LAYOUT_UNSET) {
        d->align = resolved(algn);
    }

    if (!th_seq_extent(t, &n)) {
        if (d->size.state == IR_LAYOUT_UNSET) {
            d->size = symbolic(IR_SYM_SIZEOF, t, nullptr);
        }

        return true;
    }

    uint64_t want = ir_layout_is_settled(&d->stride) ? d->stride.value * n : 0;

    if (d->size.state == IR_LAYOUT_PINNED) {
        if (d->size.value < want) {
            refuse(l, t,
                   "the pinned size %llu cannot hold %llu elements of %llu "
                   "bytes", (unsigned long long)d->size.value,
                   (unsigned long long)n, (unsigned long long)d->stride.value);

            return false;
        }
    }
    else if (d->size.state == IR_LAYOUT_UNSET) {
        d->size = resolved(want);
    }

    return true;
}

static bool lay_out_variant(L *l, th_type *t, ir_descriptor_t *d)
{
    th_type *arms = ir_type_definition(l->m, t);
    uint64_t most = 1;
    uint64_t big  = 0;
    bool     firm = arms && th_kind_of(arms) == TH_RECORD;

    for (size_t i = 0; firm && i < th_record_len(arms); i++) {
        const th_member *arm  = th_record_member(arms, i);
        uint64_t         size = 0;
        uint64_t         algn = 0;

        if (!settled_size(l, arm->type, &size, &algn)) {
            firm = false;
            break;
        }
        big  = size > big ? size : big;
        most = algn > most ? algn : most;
    }

    if (d->variant.tag_bit_width.state == IR_LAYOUT_UNSET) {
        d->variant.tag_bit_width = resolved(64);
    }

    if (d->variant.tag_bit_offset.state == IR_LAYOUT_UNSET) {
        d->variant.tag_bit_offset = resolved(0);
    }

    if (!firm) {
        if (d->size.state == IR_LAYOUT_UNSET) {
            d->size = symbolic(IR_SYM_SIZEOF, t, nullptr);
        }

        if (d->align.state == IR_LAYOUT_UNSET) {
            d->align = symbolic(IR_SYM_ALIGNOF, t, nullptr);
        }

        return true;
    }

    uint64_t tag = d->variant.niche ? 0
                 : ir_layout_is_settled(&d->variant.tag_bit_width)
                       ? (d->variant.tag_bit_width.value + 7) / 8
                       : 8;

    most = most < 8 && !d->variant.niche ? 8 : most;

    if (d->align.state == IR_LAYOUT_UNSET) {
        d->align = resolved(most);
    }

    if (d->size.state == IR_LAYOUT_UNSET) {
        d->size = resolved(round_up(round_up(tag, most) + big, most));
    }

    return true;
}

static bool lay_out(L *l, th_type *t, ir_descriptor_t **out)
{
    if (!t) {
        return false;
    }

    ir_descriptor_t *d = ir_layout_ensure(l->m, t, nullptr);

    if (out) {
        *out = d;
    }

    if (!d) {
        return false;
    }

    if (d->done) {
        return true;
    }

    for (uint32_t i = 0; i < l->n_open; i++) {
        if (ir_type_eq(l->m, l->open[i], t)) {
            refuse(l, t, "this type contains itself by value");

            return false;
        }
    }
    l->open = ir_grow(l->m->ctx, l->open, &l->cap_open, l->n_open + 1u,
                      sizeof(th_type *));
    l->open[l->n_open++] = t;

    th_type *def = th_kind_of(t) == TH_NAMED ? ir_type_definition(l->m, t)
                                             : nullptr;
    bool     is_variant = false;

    if (th_kind_of(t) == TH_NAMED) {
        th_named_info(t, nullptr, nullptr, &is_variant, nullptr);
    }

    bool ok = is_variant                        ? lay_out_variant(l, t, d)
            : th_kind_of(t) == TH_RECORD        ? lay_out_record(l, t, t, d)
            : def && th_kind_of(def) == TH_RECORD ? lay_out_record(l, t, def, d)
            : th_kind_of(t) == TH_SEQUENCE      ? lay_out_sequence(l, t, d)
                                                : lay_out_scalar(l, t, d);

    l->n_open--;
    d->done = ok;

    return ok;
}

static void reach(L *l, th_type *t)
{
    ir_descriptor_t *d = nullptr;

    if (!t) {
        return;
    }
    lay_out(l, t, &d);

    if (!d || !ir_layout_is_settled(&d->size)
        || !ir_layout_is_settled(&d->align)) {
        l->settled = false;
    }
}

static bool reach_global(ir_visit_t *vi)
{
    reach(vi->user, vi->global->type);

    return true;
}

static bool reach_param(ir_visit_t *vi)
{
    reach(vi->user, vi->param->type);

    return true;
}

static bool reach_local(ir_visit_t *vi)
{
    reach(vi->user, vi->local->type);

    return true;
}

static bool reach_dest(ir_visit_t *vi)
{
    if (vi->instr->dest_kind == IR_DEST_VALUE && vi->instr->dest) {
        reach(vi->user, vi->instr->dest->type);
    }

    return true;
}

static bool reach_operand(ir_visit_t *vi)
{
    ir_operand_t *od = vi->operand;

    if (od->kind == IR_OPD_TYPE) {
        reach(vi->user, od->type);
    }

    if (od->kind == IR_OPD_VALUE && od->value) {
        reach(vi->user, od->value->type);
    }

    if (od->kind == IR_OPD_NAMED_ARG && od->named.value) {
        reach(vi->user, od->named.value->type);
    }

    if (od->kind == IR_OPD_PHI_ARM && od->phi.value) {
        reach(vi->user, od->phi.value->type);
    }

    return true;
}

bool ir_layout_run(ir_module_t *m, ir_diags_t *d)
{
    if (!m) {
        return false;
    }

    L l = {.m = m, .d = d, .ok = true, .settled = true};

    ir_walk_t *w = ir_walk_new(&l);

    ir_walk_on_global(w, reach_global, nullptr);
    ir_walk_on_param(w, reach_param, nullptr);
    ir_walk_on_local(w, reach_local, nullptr);
    ir_walk_on_instr(w, reach_dest, nullptr, nullptr);
    ir_walk_on_operand(w, reach_operand, nullptr);
    ir_walk_run(w, m);
    ir_walk_free(w);

    if (l.ok && l.settled) {
        ir_module_establish(m, IR_PROP_LAID_OUT);
    }

    return l.ok;
}

static bool layout_pass(ir_module_t *m, ir_diags_t *d, void *state)
{
    (void)state;

    return ir_layout_run(m, d);
}

const ir_pass_t ir_pass_layout = {
    .name         = "layout",
    .requires     = IR_PROP_TYPED,
    .establishes  = IR_PROP_LAID_OUT,
    .run          = layout_pass,
};
