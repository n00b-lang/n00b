#include "private/ir/internal.h"

bool ir_annot_key_ok(core_string_t key, core_string_t profile)
{
    if (!key.data) {
        return false;
    }

    int64_t dot = core_string_find_byte(key, '.');

    if (dot <= 0 || (size_t)dot + 1 >= key.u8_bytes) {
        return false;
    }

    core_string_t head = core_string_slice(key, 0, (size_t)dot);

    if (core_string_eq(head, CORE_STRING_STATIC("ir"))) {
        return true;
    }

    if (!profile.data) {
        return true;
    }

    return core_string_eq(head, profile);
}

#define ANNOT_INDEX_MIN 16

static void annot_reindex(ir_ctx_t *ctx, ir_annots_t *m)
{
    m->index = nullptr;

    for (uint32_t i = 0; i < m->n; i++) {
        ir_index_add(ctx, &m->index, m->a[i].key, (void *)(uintptr_t)(i + 1));
    }
}

ir_annot_t *ir_annot_get(ir_annots_t *m, core_string_t key)
{
    if (!m || !key.data) {
        return nullptr;
    }

    if (m->index) {
        uintptr_t at = (uintptr_t)ir_index_find(m->index, key);

        return at ? &m->a[at - 1] : nullptr;
    }

    for (uint32_t i = 0; i < m->n; i++) {
        if (core_string_eq(m->a[i].key, key)) {
            return &m->a[i];
        }
    }

    return nullptr;
}

bool ir_annot_set(ir_ctx_t *ctx, ir_annots_t *m, ir_annot_t a)
{
    if (!ctx || !m || !a.key.data) {
        return false;
    }

    ir_annot_t *slot = ir_annot_get(m, a.key);

    if (slot) {
        if (slot->derived_from == 0 && a.derived_from != 0) {
            return false;
        }
        a.key  = slot->key;
        *slot  = a;

        return true;
    }

    size_t cap = m->cap;

    m->a   = ir_grow(ctx, m->a, &cap, m->n + 1u, sizeof(ir_annot_t));
    m->cap = (uint32_t)cap;

    a.key      = ir_strdup(ctx, a.key);
    m->a[m->n] = a;
    m->n++;

    if (m->index) {
        ir_index_add(ctx, &m->index, m->a[m->n - 1].key,
                     (void *)(uintptr_t)m->n);
    }
    else if (m->n >= ANNOT_INDEX_MIN) {
        annot_reindex(ctx, m);
    }

    return true;
}

uint32_t ir_annot_count(const ir_annots_t *m)
{
    return m ? m->n : 0;
}

ir_annot_t *ir_annot_at(ir_annots_t *m, uint32_t i)
{
    return m && i < m->n ? &m->a[i] : nullptr;
}

void ir_annot_invalidate(ir_annots_t *m, ir_props_t lost)
{
    if (!m) {
        return;
    }

    uint32_t out = 0;

    for (uint32_t i = 0; i < m->n; i++) {
        if (m->a[i].derived_from && (m->a[i].derived_from & lost)) {
            continue;
        }

        if (out != i) {
            m->a[out] = m->a[i];
        }
        out++;
    }

    if (out != m->n) {
        m->index = nullptr;
    }
    m->n = out;
}
