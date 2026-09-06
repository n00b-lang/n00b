#include "ir_internal.h"
#include "ir/tree_pass.h"
#include "ir/typelang.h"

#include "parse/annot_walk.h"
#include "parse/tree_visit.h"

static ncc_annot_result_t *
annots_of(ir_module_t *m)
{
    if (!m->tree_annots) {
        m->tree_annots = ncc_annot_result_new(m->symtab);
    }

    return m->tree_annots;
}

ncc_annot_result_t *ir_module_tree_annots(ir_module_t *m)
{
    return m ? annots_of(m) : nullptr;
}

typedef struct {
    ir_tree_hook_fn hook;
    void           *hook_state;
} tree_hook_t;

static bool
run_annot_pass(ir_module_t *m, ncc_annot_pass_t which, void *state)
{
    tree_hook_t *h = state;

    if (!m || !m->tree || !m->tree_grammar) {
        return false;
    }

    ncc_annot_result_t *r = annots_of(m);
    ncc_tree_walk_t    *w = ncc_tree_walk_new(m->tree_grammar, r);

    ncc_annot_register_pass(w, r, ir_module_types(m), which);

    if (h && h->hook) {
        h->hook(w, m, h->hook_state);
    }

    ncc_tree_walk_run(w, m->tree);
    ncc_tree_walk_free(w);

    return true;
}

#define TREE_PASS(sym, phase, req, est)                                       \
    static bool sym##_run(ir_module_t *m, ir_diags_t *d, void *state)         \
    {                                                                         \
        (void)d;                                                              \
                                                                              \
        return run_annot_pass(m, phase, state);                               \
    }                                                                         \
                                                                              \
    const ir_pass_t sym = {                                                   \
        .name        = #phase,                                                \
        .requires    = (req),                                                 \
        .establishes = (est),                                                 \
        .run         = sym##_run,                                             \
    }

TREE_PASS(ir_pass_annot_scope,
          NCC_ANNOT_PASS_SCOPE,
          IR_PROP_TREE,
          IR_PROP_SCOPED);

TREE_PASS(ir_pass_annot_symbols,
          NCC_ANNOT_PASS_SYMBOLS,
          IR_PROP_TREE | IR_PROP_SCOPED,
          IR_PROP_SYMBOLS);

TREE_PASS(ir_pass_annot_refine,
          NCC_ANNOT_PASS_REFINE,
          IR_PROP_TREE | IR_PROP_SCOPED | IR_PROP_SYMBOLS,
          IR_PROP_TREE_TYPED);

TREE_PASS(ir_pass_annot_cflow,
          NCC_ANNOT_PASS_CFLOW,
          IR_PROP_TREE | IR_PROP_SCOPED | IR_PROP_SYMBOLS,
          IR_PROP_CFLOW);

TREE_PASS(ir_pass_annot_infer,
          NCC_ANNOT_PASS_INFER,
          IR_PROP_TREE | IR_PROP_SCOPED | IR_PROP_SYMBOLS,
          IR_PROP_TREE_TYPED);

#undef TREE_PASS

static const ir_pass_t *const TREE_PASSES[] = {
    &ir_pass_annot_scope,
    &ir_pass_annot_symbols,
    &ir_pass_annot_refine,
    &ir_pass_annot_cflow,
    &ir_pass_annot_infer,
    &ir_pass_symcheck,
    &ir_pass_type_diags,
};

size_t ir_tree_pass_count(void)
{
    return sizeof(TREE_PASSES) / sizeof(TREE_PASSES[0]);
}

const ir_pass_t *ir_tree_pass_at(size_t i)
{
    return i < ir_tree_pass_count() ? TREE_PASSES[i] : nullptr;
}

ir_pass_result_t ir_tree_pass_run(const ir_pass_t *p,
                                  ir_module_t     *m,
                                  ir_tree_hook_fn  hook,
                                  void            *hook_state,
                                  ir_diags_t      *d)
{
    if (!p || !m) {
        return IR_PASS_FAILED;
    }

    tree_hook_t h = {.hook = hook, .hook_state = hook_state};

    return ir_pass_run(p, m, &h, nullptr, d);
}

bool ir_tree_passes_run(ir_module_t *m, ir_diags_t *d)
{
    for (size_t i = 0; i < ir_tree_pass_count(); i++) {
        if (ir_tree_pass_run(TREE_PASSES[i], m, nullptr, nullptr, d)
            != IR_PASS_OK) {
            return false;
        }
    }

    return true;
}
