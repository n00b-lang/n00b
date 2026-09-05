#include <string.h>

#include "ir_internal.h"

bool ir_annot_key_ok(const char *key, const char *profile)
{
    if (!key) {
        return false;
    }

    const char *dot = strchr(key, '.');

    if (!dot || dot == key || !dot[1]) {
        return false;
    }

    size_t n = (size_t)(dot - key);

    if (n == 2 && !strncmp(key, "ir", 2)) {
        return true;
    }

    if (!profile) {
        return true;
    }

    return strlen(profile) == n && !strncmp(key, profile, n);
}

ir_annot_t *ir_annot_get(ir_annots_t *m, const char *key)
{
    if (!m || !key) {
        return nullptr;
    }

    for (uint32_t i = 0; i < m->n; i++) {
        if (!strcmp(m->a[i].key, key)) {
            return &m->a[i];
        }
    }

    return nullptr;
}

bool ir_annot_set(ir_ctx_t *ctx, ir_annots_t *m, ir_annot_t a)
{
    if (!ctx || !m || !a.key) {
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
    m->n = out;
}
