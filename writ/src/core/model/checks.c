#include "model.h"

static bool
is_out_array(const writ_model_t *model, const writ_slot_rec_t *slot)
{
    return slot->mode == WRIT_MODE_OUT && writ_type(model, slot->type)->key.kind == WRIT_TYPE_ARRAY;
}

static void
out_arrays_bounded(writ_model_t *model, const writ_fn_rec_t *fn, const writ_slot_rec_t *slot)
{
    (void)fn;
    if (is_out_array(model, slot) && slot->extent_kind != WRIT_EXTENT_BOUNDED) {
        writ_report(model->session, WRIT_D_OUT_NEEDS_BOUND, slot->loc, nullptr);
    }
}

static void
bounded_extent_cell(writ_model_t *model, const writ_fn_rec_t *fn, const writ_slot_rec_t *slot)
{
    (void)fn;
    if (slot->extent_kind != WRIT_EXTENT_BOUNDED) {
        return;
    }
    writ_ref_t cap = writ_expr_ref(model, slot->cap_expr);
    writ_ref_t len = writ_expr_ref(model, slot->len_expr);
    if (cap.kind != WRIT_REF_SLOT || cap.phase != WRIT_REF_BEFORE || len.kind != WRIT_REF_SLOT
        || len.phase != WRIT_REF_AFTER || cap.index != len.index) {
        writ_report(model->session, WRIT_D_BOUND_CELL, slot->loc, nullptr);
    }
}

static void
extents_integer(writ_model_t *model, const writ_fn_rec_t *fn, const writ_slot_rec_t *slot)
{
    (void)fn;
    if (slot->extent_kind != WRIT_EXTENT_COUNT) {
        return;
    }
    writ_ref_t count = writ_expr_ref(model, slot->count_expr);
    if (count.kind == WRIT_REF_SLOT && !writ_type_is_integer(model, model->slots.data[count.index].type)) {
        writ_report(model->session, WRIT_D_EXTENT_NOT_INTEGER, slot->loc, nullptr);
    }
}

static void
sizing_has_output(writ_model_t *model, const writ_fn_rec_t *fn, const writ_slot_rec_t *slot)
{
    if (!fn->sizing_expr || slot != &model->slots.data[fn->first_slot + fn->slot_count - 1]) {
        return;
    }
    for (uint32_t i = 0; i < fn->slot_count; i++) {
        const writ_slot_rec_t *candidate = &model->slots.data[fn->first_slot + i];
        if (is_out_array(model, candidate) && candidate->extent_kind == WRIT_EXTENT_BOUNDED) {
            return;
        }
    }
    writ_report(model->session, WRIT_D_SIZING_WITHOUT_OUTPUT, fn->loc, nullptr);
}

typedef void (*check_fn)(writ_model_t *model, const writ_fn_rec_t *fn, const writ_slot_rec_t *slot);

static const check_fn checks[] = {
#define WRIT_CHECK(name, section) name,
#include "checks.def"
#undef WRIT_CHECK
};

void
writ_model_check(writ_model_t *model)
{
    for (size_t c = 0; c < sizeof(checks) / sizeof(checks[0]); c++) {
        for (uint32_t f = 0; f < model->fns.len; f++) {
            const writ_fn_rec_t *fn = &model->fns.data[f];
            for (uint32_t s = 0; s < fn->slot_count; s++) {
                checks[c](model, fn, &model->slots.data[fn->first_slot + s]);
            }
        }
    }
}
