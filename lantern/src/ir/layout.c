#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "private/ir/internal.h"
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

static ir_descriptor_t *layout_of_exact(ir_module_t *m, th_type *t,
                                        const char *target)
{
    ir_typeslot_t *s = ir_type_slot(m, t);

    if (!s) {
        return nullptr;
    }

    for (size_t i = 0; i < s->n_descs; i++) {
        if (same_target(s->descs[i].target, target)) {
            return &s->descs[i].d;
        }
    }

    return nullptr;
}

ir_descriptor_t *ir_layout_of(ir_module_t *m, th_type *t, const char *target)
{
    return layout_of_exact(m, t, target ? target : m->target);
}

ir_descriptor_t *ir_layout_of_targetless(ir_module_t *m, th_type *t)
{
    return layout_of_exact(m, t, nullptr);
}

static void invalidate_value(ir_layout_val_t *value)
{
    if (value && !value->authored) *value = (ir_layout_val_t){0};
}

void ir_layout_invalidate_type(ir_module_t *m, th_type *t)
{
    ir_typeslot_t *slot = ir_type_slot(m, t);
    if (!slot) return;
    for (size_t i = 0; i < slot->n_descs; i++) {
        ir_descriptor_t *d = &slot->descs[i].d;
        d->done = false;
        d->final = nullptr;
        invalidate_value(&d->size);
        invalidate_value(&d->align);
        invalidate_value(&d->stride);
        invalidate_value(&d->indirect.words);
        invalidate_value(&d->variant.tag_bit_offset);
        invalidate_value(&d->variant.tag_bit_width);
        for (uint32_t j = 0; j < d->n_members; j++) {
            invalidate_value(&d->members[j].bit_offset);
            invalidate_value(&d->members[j].bit_width);
        }
    }
}

static ir_descriptor_t *layout_ensure_exact(ir_module_t *m, th_type *t,
                                            const char *target)
{
    ir_descriptor_t *found = layout_of_exact(m, t, target);

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

    *e = (ir_descentry_t){.target = ir_strdup(m->ctx, target)};

    e->d.type   = t;
    e->d.target = e->target;

    return &e->d;
}

ir_descriptor_t *ir_layout_ensure(ir_module_t *m, th_type *t,
                                  const char *target)
{
    return layout_ensure_exact(m, t, target ? target : m->target);
}

ir_descriptor_t *ir_layout_ensure_targetless(ir_module_t *m, th_type *t)
{
    return layout_ensure_exact(m, t, nullptr);
}

static bool valid_runs(const ir_layout_bit_run_t *runs, size_t count,
                       uint64_t extent)
{
    if (count && !runs) return false;
    uint64_t end = 0;
    for (size_t i = 0; i < count; i++) {
        if (!runs[i].width || runs[i].offset < end
            || runs[i].offset > extent
            || runs[i].width > extent - runs[i].offset) return false;
        end = runs[i].offset + runs[i].width;
    }
    return true;
}

static bool disjoint_runs(const ir_layout_bit_run_t *a, size_t na,
                          const ir_layout_bit_run_t *b, size_t nb)
{
    size_t i = 0, j = 0;
    while (i < na && j < nb) {
        uint64_t ae = a[i].offset + a[i].width;
        uint64_t be = b[j].offset + b[j].width;
        if (a[i].offset < be && b[j].offset < ae) return false;
        if (ae <= be) i++;
        else j++;
    }
    return true;
}

static bool member_overlaps_only_within(
    const ir_final_member_layout_t *a, const ir_final_member_layout_t *b,
    ir_layout_bit_run_t region)
{
    size_t i = 0, j = 0;
    while (i < a->n_occupied && j < b->n_occupied) {
        uint64_t ae = a->occupied[i].offset + a->occupied[i].width;
        uint64_t be = b->occupied[j].offset + b->occupied[j].width;
        uint64_t start = a->occupied[i].offset > b->occupied[j].offset
                       ? a->occupied[i].offset : b->occupied[j].offset;
        uint64_t end = ae < be ? ae : be;
        if (start < end
            && (!region.width || start < region.offset
                || end > region.offset + region.width)) return false;
        if (ae <= be) i++;
        else j++;
    }
    return true;
}

static bool runs_within(const ir_layout_bit_run_t *child, size_t nc,
                        const ir_layout_bit_run_t *parent, size_t np)
{
    size_t j = 0;
    for (size_t i = 0; i < nc; i++) {
        uint64_t at = child[i].offset;
        uint64_t end = at + child[i].width;
        while (at < end) {
            while (j < np && parent[j].offset + parent[j].width <= at) j++;
            if (j == np || parent[j].offset > at) return false;
            uint64_t parent_end = parent[j].offset + parent[j].width;
            at = parent_end < end ? parent_end : end;
        }
    }
    return true;
}

static ir_layout_bit_run_t *copy_runs(ir_ctx_t *ctx,
                                      const ir_layout_bit_run_t *runs,
                                      size_t count)
{
    if (!count) return nullptr;
    if (count > SIZE_MAX / sizeof(*runs)) return nullptr;
    ir_layout_bit_run_t *copy = ir_alloc(ctx, count * sizeof(*runs));
    if (copy) memcpy(copy, runs, count * sizeof(*runs));
    return copy;
}

static bool valid_access(ir_layout_access_t access, uint64_t intrinsic_bits)
{
    if (!access.unit_width_bits || access.unit_width_bits % 8
        || access.unit_bit_offset % 8 || !access.value_width_bits
        || access.lsb >= access.unit_width_bits
        || access.value_width_bits > access.unit_width_bits - access.lsb
        || intrinsic_bits > UINT64_MAX - 7) return false;
    uint64_t extent = (intrinsic_bits + 7) & ~UINT64_C(7);
    return access.unit_bit_offset <= extent
        && access.unit_width_bits <= extent - access.unit_bit_offset;
}

static ir_layout_access_t *copy_accesses(ir_ctx_t *ctx,
                                         const ir_layout_access_t *accesses,
                                         size_t count)
{
    if (!count) return nullptr;
    if (count > SIZE_MAX / sizeof(*accesses)) return nullptr;
    ir_layout_access_t *copy = ir_alloc(ctx, count * sizeof(*accesses));
    if (copy) memcpy(copy, accesses, count * sizeof(*accesses));
    return copy;
}

bool ir_layout_publish_final(ir_module_t *m, th_type *t, const char *target,
                             const ir_final_layout_t *layout)
{
    if (!m || !t || !layout || !layout->alignment_bytes
        || (layout->alignment_bytes & (layout->alignment_bytes - 1))
        || !valid_runs(layout->occupied, layout->n_occupied,
                       layout->intrinsic_bits)
        || !valid_runs(layout->reserved, layout->n_reserved,
                       layout->intrinsic_bits)
        || !disjoint_runs(layout->occupied, layout->n_occupied,
                          layout->reserved, layout->n_reserved)
        || (layout->n_members && !layout->members)
        || layout->n_members > SIZE_MAX / sizeof(*layout->members)
        || (!layout->member_overlap_region.width
            && layout->member_overlap_region.offset)
        || (layout->member_overlap_region.width
            && (layout->member_overlap_region.offset > layout->intrinsic_bits
                || layout->member_overlap_region.width
                    > layout->intrinsic_bits
                      - layout->member_overlap_region.offset))
        || (layout->discriminant_is_niche && !layout->has_discriminant)
        || (layout->has_discriminant
            && !valid_access(layout->discriminant, layout->intrinsic_bits)))
        return false;

    for (size_t i = 0; i < layout->n_members; i++) {
        const ir_final_member_layout_t *member = &layout->members[i];
        if (!member->name || !member->type
            || ((member->inline_element_count != 0)
                != (member->inline_element_type != nullptr))
            || ((member->inline_element_count != 0)
                != (member->inline_element_stride_bits != 0))
            || (member->inline_element_count
                && (member->inline_element_count > UINT64_MAX
                    / member->inline_element_stride_bits
                    || member->bit_offset > layout->intrinsic_bits
                    || member->inline_element_count
                        * member->inline_element_stride_bits
                        > layout->intrinsic_bits - member->bit_offset))
            || (member->bit_offset > layout->intrinsic_bits
                && (member->n_occupied || member->n_accesses))
            || !valid_runs(member->occupied, member->n_occupied,
                           layout->intrinsic_bits)
            || !runs_within(member->occupied, member->n_occupied,
                            layout->occupied, layout->n_occupied)
            || (member->n_accesses && !member->accesses)
            || member->n_accesses > SIZE_MAX / sizeof(*member->accesses))
            return false;
        for (size_t j = 0; j < i; j++) {
            if (layout->members[j].ordinal == member->ordinal
                || !member_overlaps_only_within(
                    &layout->members[j], member,
                    layout->member_overlap_region)) return false;
        }
        for (size_t j = 0; j < member->n_accesses; j++)
            if (!valid_access(member->accesses[j], layout->intrinsic_bits))
                return false;
    }

    ir_descriptor_t *descriptor = ir_layout_ensure(m, t, target);
    if (!descriptor || descriptor->final) return false;

    ir_ctx_t *ctx = m->ctx;
    ir_final_layout_t *copy = ir_alloc(ctx, sizeof(*copy));
    if (!copy) return false;
    *copy = *layout;
    copy->occupied = copy_runs(ctx, layout->occupied, layout->n_occupied);
    copy->reserved = copy_runs(ctx, layout->reserved, layout->n_reserved);
    if ((layout->n_occupied && !copy->occupied)
        || (layout->n_reserved && !copy->reserved)) return false;

    if (layout->n_members) {
        ir_final_member_layout_t *members = ir_alloc(
            ctx, layout->n_members * sizeof(*members));
        if (!members) return false;
        for (size_t i = 0; i < layout->n_members; i++) {
            members[i] = layout->members[i];
            members[i].name = ir_strdup(ctx, layout->members[i].name);
            members[i].occupied = copy_runs(ctx,
                layout->members[i].occupied, layout->members[i].n_occupied);
            members[i].accesses = copy_accesses(ctx,
                layout->members[i].accesses, layout->members[i].n_accesses);
            if (!members[i].name
                || (layout->members[i].n_occupied
                    && !members[i].occupied)
                || (layout->members[i].n_accesses
                    && !members[i].accesses)) return false;
        }
        copy->members = members;
    }
    descriptor->final = copy;
    return true;
}

const ir_final_layout_t *ir_layout_final(ir_module_t *m, th_type *t,
                                        const char *target)
{
    ir_descriptor_t *d = m && t ? ir_layout_of(m, t, target) : nullptr;
    return d ? d->final : nullptr;
}

const ir_final_layout_t *ir_layout_final_visible(ir_module_t *m, th_type *t,
                                                const char *target)
{
    const ir_final_layout_t *local = ir_layout_final(m, t, target);
    if (local) return local;
    ir_module_t *owner = m && t ? ir_type_definition_owner(m, t) : nullptr;
    if (!owner || owner == m) return nullptr;
    return ir_layout_final(owner, t, target);
}

static bool pin(ir_layout_val_t *v, uint64_t value);

bool ir_layout_copy_final(ir_module_t *m, th_type *destination,
                          th_type *source, const char *target)
{
    if (!m || !destination || !source || destination == source
        || ir_layout_final(m, destination, target)) return false;
    const ir_final_layout_t *final = ir_layout_final(m, source, target);
    ir_descriptor_t *source_descriptor = ir_layout_of(m, source, target);
    ir_descriptor_t *destination_descriptor = ir_layout_ensure(m, destination,
                                                                target);
    uint64_t extent;
    if (!final || !source_descriptor || !destination_descriptor
        || !ir_layout_final_byte_extent(final, &extent)
        || !pin(&destination_descriptor->size, extent)
        || !pin(&destination_descriptor->align, final->alignment_bytes))
        return false;
    for (size_t i = 0; i < source_descriptor->n_members; i++) {
        const ir_member_layout_t *member = &source_descriptor->members[i];
        if (!ir_layout_is_settled(&member->bit_offset)
            || !ir_layout_is_settled(&member->bit_width)) continue;
        if (!ir_layout_descriptor_pin_member(m, destination_descriptor,
                member->name, member->bit_offset.value,
                member->bit_width.value)) return false;
    }
    return ir_layout_publish_final(m, destination, target, final);
}

bool ir_layout_final_byte_extent(const ir_final_layout_t *layout,
                                 uint64_t *bytes)
{
    if (!layout || !bytes) return false;
    *bytes = layout->intrinsic_bits / 8 + (layout->intrinsic_bits % 8 != 0);
    return true;
}

bool ir_layout_final_stride_bits(const ir_final_layout_t *layout,
                                 uint64_t *bits)
{
    uint64_t bytes;
    if (!layout || !bits || !layout->alignment_bytes
        || (layout->alignment_bytes & (layout->alignment_bytes - 1))
        || !ir_layout_final_byte_extent(layout, &bytes))
        return false;
    if (!bytes) {
        *bits = 0;
        return true;
    }
    uint64_t align = layout->alignment_bytes;
    if (bytes > UINT64_MAX - (align - 1)) return false;
    uint64_t aligned = (bytes + align - 1) & ~(align - 1);
    if (aligned > UINT64_MAX / 8) return false;
    *bits = aligned * 8;
    return true;
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
    if (!d || !pin(&d->size, bytes)) return false;
    d->final = nullptr;
    return true;
}

bool ir_layout_pin_stride(ir_module_t *m, th_type *t, uint64_t bytes)
{
    ir_descriptor_t *d = ir_layout_ensure(m, t, nullptr);

    if (!d || !pin(&d->stride, bytes)) return false;
    d->final = nullptr;
    return true;
}

bool ir_layout_pin_align(ir_module_t *m, th_type *t, uint64_t bytes)
{
    ir_descriptor_t *d = ir_layout_ensure(m, t, nullptr);

    if (!d || !bytes || (bytes & (bytes - 1))) {
        return false;
    }

    if (!pin(&d->align, bytes)) return false;
    d->final = nullptr;
    return true;
}

static void drop_members(void *p)
{
    core_dict_free(p);
    core_free(p);
}

static ir_member_layout_t *member_slot(ir_module_t *m, ir_descriptor_t *d,
                                       const char *field)
{
    if (!d->member_index) {
        core_dict_t *ix = core_alloc(core_dict_t);

        core_dict_init(ix, core_hash_cstring, core_dict_cstr_eq);
        ir_ctx_on_free(m->ctx, drop_members, ix);
        d->member_index = ix;
    }

    bool   found = false;
    size_t at    = (size_t)(uintptr_t)core_dict_get((core_dict_t *)
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
    core_dict_put((core_dict_t *)d->member_index, slot->name, d->n_members);
    d->n_members++;

    return slot;
}

bool ir_layout_pin_member(ir_module_t *m, th_type *t, const char *field,
                          uint64_t bit_offset, uint64_t bit_width)
{
    ir_descriptor_t *d = ir_layout_ensure(m, t, nullptr);

    return ir_layout_descriptor_pin_member(m, d, field, bit_offset,
                                           bit_width);
}

bool ir_layout_descriptor_pin_member(ir_module_t *m, ir_descriptor_t *d,
                                     const char *field, uint64_t bit_offset,
                                     uint64_t bit_width)
{

    if (!d || !field) {
        return false;
    }

    ir_member_layout_t *slot = member_slot(m, d, field);

    if (!slot || !pin(&slot->bit_offset, bit_offset)
        || !pin(&slot->bit_width, bit_width)) return false;
    d->final = nullptr;
    return true;
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
    r.authored                = true;
    d->variant             = r;
    d->final               = nullptr;

    return true;
}

bool ir_layout_pin_indirect(ir_module_t *m, th_type *t, ir_indirect_repr_t r)
{
    ir_descriptor_t *d = ir_layout_ensure(m, t, nullptr);

    if (!d || !r.words.value) {
        return false;
    }

    r.words.state    = IR_LAYOUT_PINNED;
    r.words.authored = true;
    r.authored       = true;
    d->indirect      = r;
    d->final         = nullptr;

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

    core_buffer_t *b = core_buffer_empty();
    va_list ap;

    va_start(ap, what);
    core_buffer_vprintf(b, what, ap);
    va_end(ap);

    core_buffer_printf(b, " (laying out %s)", ir_type_text(l->m, t));

    char *msg = core_buffer_take(b);

    ir_diag_message(dg, l->d, msg);
    core_free(msg);
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
    if (!d->align.value || d->size.value > UINT64_MAX - (d->align.value - 1))
        return false;
    *bytes = round_up(d->size.value, d->align.value);
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
            uint64_t width = slot->bit_width.state == IR_LAYOUT_PINNED
                                 ? slot->bit_width.value
                                 : size * 8;
            uint64_t end   = (slot->bit_offset.value + width + 7) / 8;
            at             = end > at ? end : at;
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

    bool ok;
    if (is_variant) ok = lay_out_variant(l, t, d);
    else if (th_kind_of(t) == TH_RECORD)
        ok = lay_out_record(l, t, t, d);
    else if (def && th_kind_of(def) == TH_RECORD)
        ok = lay_out_record(l, t, def, d);
    else if (def) {
        ir_descriptor_t *base = nullptr;
        ok = lay_out(l, def, &base);
        if (ok && base) {
            if (d->size.state == IR_LAYOUT_UNSET) d->size = base->size;
            if (d->align.state == IR_LAYOUT_UNSET) d->align = base->align;
            if (d->stride.state == IR_LAYOUT_UNSET)
                d->stride = base->stride;
            if (d->indirect.words.state == IR_LAYOUT_UNSET)
                d->indirect = base->indirect;
            if (!d->variant.authored) d->variant = base->variant;
            for (uint32_t i = 0; ok && i < base->n_members; i++) {
                ir_member_layout_t *member = member_slot(
                    l->m, d, base->members[i].name);
                if (!member) {
                    ok = false;
                    break;
                }
                if (member->bit_offset.state == IR_LAYOUT_UNSET)
                    member->bit_offset = base->members[i].bit_offset;
                if (member->bit_width.state == IR_LAYOUT_UNSET)
                    member->bit_width = base->members[i].bit_width;
            }
        }
    }
    else if (th_kind_of(t) == TH_SEQUENCE) ok = lay_out_sequence(l, t, d);
    else ok = lay_out_scalar(l, t, d);

    l->n_open--;
    d->done = ok;

    return ok;
}

bool ir_layout_compute(ir_module_t *m, th_type *type, ir_diags_t *d)
{
    if (!m || !type) return false;
    L layout = {.m = m, .d = d, .ok = true, .settled = true};
    ir_descriptor_t *descriptor = nullptr;
    bool ok = lay_out(&layout, type, &descriptor);
    return ok && layout.ok && descriptor
        && ir_layout_is_settled(&descriptor->size)
        && ir_layout_is_settled(&descriptor->align);
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

    if ((od->kind == IR_OPD_VALUE || od->kind == IR_OPD_SPREAD
         || od->kind == IR_OPD_FRESH) && od->value) {
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
