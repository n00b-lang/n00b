#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "private/ir/internal.h"
#include "ir/typelang.h"
#include "ir/walk.h"

#include "lib/dict.h"

void *ir_alloc(ir_ctx_t *ctx, size_t n)
{
    return core_arena_alloc(&ctx->arena, n);
}

size_t ir_ctx_bytes(const ir_ctx_t *ctx)
{
    size_t n = 0;

    for (const core_arena_block_t *b = ctx ? ctx->arena.first : nullptr; b;
         b = b->next) {
        n += b->capacity;
    }

    return n;
}

char *ir_strdup(ir_ctx_t *ctx, const char *s)
{
    if (!s) {
        return nullptr;
    }

    size_t n = strlen(s);
    char  *d = ir_alloc(ctx, n + 1);

    memcpy(d, s, n + 1);

    return d;
}

static size_t next_cap(size_t cap, size_t need)
{
    size_t want = cap ? cap * 2 : 8;

    while (want < need) {
        want *= 2;
    }

    return want;
}

void *ir_grow(ir_ctx_t *ctx, void *p, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap) {
        return p;
    }

    size_t ncap = next_cap(*cap, need);

    if (p && *cap
        && core_arena_extend(&ctx->arena, p, *cap * elem, ncap * elem)) {
        *cap = ncap;

        return p;
    }

    void *q = ir_alloc(ctx, ncap * elem);

    if (p && *cap) {
        memcpy(q, p, *cap * elem);
    }
    *cap = ncap;

    return q;
}

ir_ctx_t *ir_ctx_new(void)
{
    ir_ctx_t *ctx = core_alloc(ir_ctx_t);

    core_arena_init(&ctx->arena, 0);
    ctx->types = th_ctx_new();

    return ctx;
}

static void drop_dict(void *p)
{
    core_dict_free(p);
    core_free(p);
}

void ir_ctx_on_free(ir_ctx_t *ctx, void (*fn)(void *), void *arg)
{
    ir_cleanup_t *c = ir_alloc(ctx, sizeof(ir_cleanup_t));

    c->fn         = fn;
    c->arg        = arg;
    c->next       = ctx->cleanups;
    ctx->cleanups = c;
}

void ir_ctx_free(ir_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    for (ir_cleanup_t *k = ctx->cleanups; k; k = k->next) {
        k->fn(k->arg);
    }

    core_arena_free(&ctx->arena);
    th_ctx_free(ctx->types);
    core_free(ctx);
}

th_ctx *ir_ctx_types(ir_ctx_t *ctx)
{
    return ctx ? ctx->types : nullptr;
}

static void free_symtab(void *p)
{
    parse_symtab_free(p);
}

ir_module_t *ir_module_new(ir_ctx_t *ctx, const char *name,
                           const ir_profile_t *profile, const char *target)
{
    if (!ctx || !name || !profile) {
        return nullptr;
    }

    ir_module_t *m = ir_alloc(ctx, sizeof(ir_module_t));

    m->ctx     = ctx;
    m->name    = ir_strdup(ctx, name);
    m->target  = target ? ir_strdup(ctx, target) : nullptr;
    m->profile = profile;
    m->symtab  = parse_symtab_new();

    ir_ctx_on_free(ctx, free_symtab, m->symtab);

    return m;
}

void ir_module_set_tree(ir_module_t      *m,
                        parse_grammar_t    *g,
                        parse_tree_t *tree)
{
    if (!m) {
        return;
    }

    m->tree_grammar = g;
    m->tree         = tree;

    if (tree) {
        ir_module_establish(m, IR_PROP_TREE);
    }
    else {
        ir_module_invalidate(m, IR_PROP_TREE | IR_PROP_SCOPED
                                    | IR_PROP_SYMBOLS | IR_PROP_CFLOW
                                    | IR_PROP_TREE_TYPED);
    }
}

parse_tree_t *ir_module_tree(ir_module_t *m)
{
    return m ? m->tree : nullptr;
}

parse_grammar_t *ir_module_grammar(ir_module_t *m)
{
    return m ? m->tree_grammar : nullptr;
}

parse_symtab_t *ir_module_symtab(ir_module_t *m)
{
    return m ? m->symtab : nullptr;
}

th_ctx *ir_module_types(ir_module_t *m)
{
    return m ? ir_ctx_types(m->ctx) : nullptr;
}

const char *ir_module_name(const ir_module_t *m)
{
    return m ? m->name : nullptr;
}

const char *ir_module_target(const ir_module_t *m)
{
    return m ? m->target : nullptr;
}

const ir_profile_t *ir_module_profile(const ir_module_t *m)
{
    return m ? m->profile : nullptr;
}

ir_props_t ir_module_props(const ir_module_t *m)
{
    return m ? m->props : 0;
}

void ir_module_establish(ir_module_t *m, ir_props_t p)
{
    if (m) {
        m->props |= p;
    }
}

static bool drop_on_global(ir_visit_t *vi)
{
    ir_annot_invalidate(&vi->global->annots, *(ir_props_t *)vi->user);

    return true;
}

static bool drop_on_func(ir_visit_t *vi)
{
    ir_props_t p = *(ir_props_t *)vi->user;

    vi->func->props &= (ir_props_t) ~(uint16_t)p;
    ir_annot_invalidate(&vi->func->annots, p);

    if (p & IR_PROP_STRUCTURED) {
        vi->func->regions = nullptr;
    }

    return true;
}

static bool drop_on_local(ir_visit_t *vi)
{
    ir_annot_invalidate(&vi->local->annots, *(ir_props_t *)vi->user);

    return true;
}

static bool drop_on_block(ir_visit_t *vi)
{
    ir_annot_invalidate(&vi->block->annots, *(ir_props_t *)vi->user);

    return true;
}

static bool drop_on_instr(ir_visit_t *vi)
{
    ir_annot_invalidate(&vi->instr->annots, *(ir_props_t *)vi->user);

    return false;
}

void ir_module_invalidate(ir_module_t *m, ir_props_t p)
{
    if (!m) {
        return;
    }

    m->props &= (ir_props_t) ~(uint16_t)p;
    ir_annot_invalidate(&m->annots, p);

    ir_walk_t *w = ir_walk_new(&p);

    ir_walk_on_global(w, drop_on_global, nullptr);
    ir_walk_on_func(w, drop_on_func, nullptr, nullptr);
    ir_walk_on_local(w, drop_on_local, nullptr);
    ir_walk_on_block(w, drop_on_block, nullptr, nullptr);
    ir_walk_on_instr(w, drop_on_instr, nullptr, nullptr);
    ir_walk_run(w, m);
    ir_walk_free(w);
}

void ir_module_set_profile(ir_module_t *m, const ir_profile_t *p)
{
    if (!m || !p) {
        return;
    }

    m->profile       = p;
    m->rules         = nullptr;
    m->scalar_hashes = nullptr;
    m->scalars_ready = false;
}

ir_span_t ir_module_span(const ir_module_t *m)
{
    return m ? m->span : IR_SPAN_UNKNOWN;
}

void ir_module_set_span(ir_module_t *m, ir_span_t s)
{
    if (m) {
        m->span = s;
    }
}

void ir_module_set_target(ir_module_t *m, const char *target)
{
    if (m && target) {
        m->target = ir_strdup(m->ctx, target);
    }
}

ir_annots_t *ir_module_annots(ir_module_t *m)
{
    return m ? &m->annots : nullptr;
}

const char *ir_mutability_name(ir_mutability_t m)
{
    switch (m) {
    case IR_MUT_MUTABLE:
        return "mutable";
    case IR_MUT_IMMUTABLE:
        return "immutable";
    case IR_MUT_CONST:
        return "const";
    default:
        return nullptr;
    }
}

bool ir_mutability_from_name(const char *s, ir_mutability_t *out)
{
    if (!s) {
        return false;
    }

    for (ir_mutability_t m = IR_MUT_MUTABLE; m <= IR_MUT_CONST; m++) {
        if (!strcmp(s, ir_mutability_name(m))) {
            if (out) {
                *out = m;
            }

            return true;
        }
    }

    return false;
}

bool ir_func_set_execution_phase(ir_ctx_t *ctx, ir_func_t *function,
                                 ir_execution_phase_t phase)
{
    if (!ctx || !function || phase > IR_PHASE_COMPTIME) return false;
    return ir_annot_set(ctx, &function->annots, (ir_annot_t){
        .key = "ir.execution_phase",
        .kind = IR_ANNOT_INT,
        .i = {.mag = (ir_uint_t)phase},
    });
}

ir_execution_phase_t ir_func_execution_phase(const ir_func_t *function)
{
    if (!function) return IR_PHASE_RUNTIME;
    ir_annot_t *phase = ir_annot_get(
        (ir_annots_t *)&function->annots, "ir.execution_phase");
    if (!phase || phase->kind != IR_ANNOT_INT || phase->i.neg
        || phase->i.mag > IR_PHASE_COMPTIME)
        return IR_PHASE_RUNTIME;
    return (ir_execution_phase_t)phase->i.mag;
}

bool ir_func_set_initializer(ir_ctx_t *ctx, ir_func_t *function,
                             bool is_initializer)
{
    if (!ctx || !function) return false;
    return ir_annot_set(ctx, &function->annots, (ir_annot_t){
        .key = "ir.initializer",
        .kind = IR_ANNOT_BOOL,
        .b = is_initializer,
    });
}

bool ir_func_is_initializer(const ir_func_t *function)
{
    if (!function) return false;
    ir_annot_t *initializer = ir_annot_get(
        (ir_annots_t *)&function->annots, "ir.initializer");
    return initializer && initializer->kind == IR_ANNOT_BOOL
        && initializer->b;
}

static bool valid_phase_access(ir_phase_access_t access);

bool ir_value_set_phase_access(ir_ctx_t *ctx, ir_value_t *value,
                               ir_phase_access_t available,
                               ir_phase_access_t mutable_in,
                               ir_phase_access_t initializable_in)
{
    if (!ctx || !value || !valid_phase_access(available)
        || !valid_phase_access(mutable_in)
        || !valid_phase_access(initializable_in)) return false;
    ir_annots_t *annots = ir_value_annots(value);
    return annots
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = "ir.binding_phases",
               .kind = IR_ANNOT_INT,
               .i = {.mag = available},
           })
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = "ir.binding_mutable_phases",
               .kind = IR_ANNOT_INT,
               .i = {.mag = mutable_in},
           })
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = "ir.binding_initializer_phases",
               .kind = IR_ANNOT_INT,
               .i = {.mag = initializable_in},
           });
}

static bool value_phase_allows(const ir_value_t *value, const char *key,
                               ir_execution_phase_t phase)
{
    if (!value || phase > IR_PHASE_COMPTIME) return false;
    ir_annots_t *annots = ir_value_annots((ir_value_t *)value);
    ir_annot_t *available = ir_annot_get(annots, "ir.binding_phases");
    ir_annot_t *mutable = ir_annot_get(
        annots, "ir.binding_mutable_phases");
    ir_annot_t *initializable = ir_annot_get(
        annots, "ir.binding_initializer_phases");
    if (!available && !mutable && !initializable) return true;
    if (!available || !mutable || !initializable) return false;
    ir_annot_t *access = ir_annot_get(annots, key);
    ir_annot_t *facts[] = {available, mutable, initializable};
    for (size_t i = 0; i < sizeof(facts) / sizeof(facts[0]); i++)
        if (facts[i]->kind != IR_ANNOT_INT || facts[i]->i.neg
            || facts[i]->i.mag > IR_PHASE_ACCESS_BOTH) return false;
    ir_phase_access_t phase_bit = phase == IR_PHASE_COMPTIME
        ? IR_PHASE_ACCESS_COMPTIME : IR_PHASE_ACCESS_RUNTIME;
    return ((ir_phase_access_t)access->i.mag & phase_bit) != 0;
}

bool ir_value_can_read(const ir_value_t *value, ir_execution_phase_t phase)
{
    return value_phase_allows(value, "ir.binding_phases", phase);
}

bool ir_value_can_write(const ir_value_t *value, ir_execution_phase_t phase)
{
    return ir_value_can_read(value, phase)
        && value_phase_allows(value, "ir.binding_mutable_phases", phase);
}

bool ir_value_can_initialize(const ir_value_t *value,
                             ir_execution_phase_t phase)
{
    return value_phase_allows(value, "ir.binding_initializer_phases", phase);
}

bool ir_value_set_indirect_access(ir_ctx_t *ctx, ir_value_t *parameter,
                                  ir_indirect_access_t access)
{
    if (!ctx || !parameter || parameter->kind != IR_VAL_PARAM
        || !parameter->type
        || th_kind_of(parameter->type) != TH_INDIRECTION
        || access < IR_INDIRECT_ACCESS_READ
        || access > IR_INDIRECT_ACCESS_READ_WRITE) return false;
    return ir_annot_set(ctx, ir_value_annots(parameter), (ir_annot_t){
        .key = "ir.indirect_access",
        .kind = IR_ANNOT_INT,
        .i = {.mag = access},
    });
}

ir_indirect_access_t ir_value_indirect_access(const ir_value_t *parameter)
{
    if (!parameter || parameter->kind != IR_VAL_PARAM) return
        IR_INDIRECT_ACCESS_UNSPECIFIED;
    ir_annot_t *access = ir_annot_get(
        ir_value_annots((ir_value_t *)parameter), "ir.indirect_access");
    if (!access || access->kind != IR_ANNOT_INT || access->i.neg
        || access->i.mag < IR_INDIRECT_ACCESS_READ
        || access->i.mag > IR_INDIRECT_ACCESS_READ_WRITE)
        return IR_INDIRECT_ACCESS_UNSPECIFIED;
    return (ir_indirect_access_t)access->i.mag;
}

bool ir_global_set_comptime_mutability(ir_ctx_t *ctx, ir_global_t *global,
                                       ir_mutability_t mutability)
{
    if (!ctx || !global || mutability > IR_MUT_CONST) return false;
    return ir_annot_set(ctx, &global->annots, (ir_annot_t){
        .key = "ir.comptime_mutability",
        .kind = IR_ANNOT_INT,
        .i = {.mag = (ir_uint_t)mutability},
    });
}

ir_mutability_t ir_global_comptime_mutability(const ir_global_t *global)
{
    if (!global) return IR_MUT_MUTABLE;
    ir_annot_t *mutability = ir_annot_get(
        (ir_annots_t *)&global->annots, "ir.comptime_mutability");
    if (!mutability || mutability->kind != IR_ANNOT_INT
        || mutability->i.neg || mutability->i.mag > IR_MUT_CONST)
        return global->mutability;
    return (ir_mutability_t)mutability->i.mag;
}

static bool valid_phase_access(ir_phase_access_t access)
{
    return access <= IR_PHASE_ACCESS_BOTH;
}

bool ir_global_set_phase_access(ir_ctx_t *ctx, ir_global_t *global,
                                ir_phase_access_t available,
                                ir_phase_access_t mutable_in,
                                ir_phase_access_t initializable_in)
{
    if (!ctx || !global || !valid_phase_access(available)
        || !valid_phase_access(mutable_in)
        || !valid_phase_access(initializable_in)) return false;
    return ir_annot_set(ctx, &global->annots, (ir_annot_t){
               .key = "ir.global_available_phases",
               .kind = IR_ANNOT_INT,
               .i = {.mag = available},
           })
        && ir_annot_set(ctx, &global->annots, (ir_annot_t){
               .key = "ir.global_mutable_phases",
               .kind = IR_ANNOT_INT,
               .i = {.mag = mutable_in},
           })
        && ir_annot_set(ctx, &global->annots, (ir_annot_t){
               .key = "ir.global_initializer_phases",
               .kind = IR_ANNOT_INT,
               .i = {.mag = initializable_in},
           });
}

static bool global_phase_allows(const ir_global_t *global, const char *key,
                                ir_execution_phase_t phase)
{
    if (!global || phase > IR_PHASE_COMPTIME) return false;
    ir_annots_t *annots = (ir_annots_t *)&global->annots;
    ir_annot_t *available = ir_annot_get(
        annots, "ir.global_available_phases");
    ir_annot_t *mutable = ir_annot_get(
        annots, "ir.global_mutable_phases");
    ir_annot_t *initializable = ir_annot_get(
        annots, "ir.global_initializer_phases");
    if (!available && !mutable && !initializable) return true;
    if (!available || !mutable || !initializable) return false;
    ir_annot_t *access = ir_annot_get(annots, key);
    ir_annot_t *facts[] = {available, mutable, initializable};
    for (size_t i = 0; i < sizeof(facts) / sizeof(facts[0]); i++)
        if (facts[i]->kind != IR_ANNOT_INT || facts[i]->i.neg
            || facts[i]->i.mag > IR_PHASE_ACCESS_BOTH) return false;
    ir_phase_access_t phase_bit = phase == IR_PHASE_COMPTIME
        ? IR_PHASE_ACCESS_COMPTIME : IR_PHASE_ACCESS_RUNTIME;
    return ((ir_phase_access_t)access->i.mag & phase_bit) != 0;
}

bool ir_global_can_read(const ir_global_t *global, ir_execution_phase_t phase)
{
    return global_phase_allows(global, "ir.global_available_phases", phase);
}

bool ir_global_can_write(const ir_global_t *global,
                         ir_execution_phase_t phase)
{
    return ir_global_can_read(global, phase)
        && global_phase_allows(global, "ir.global_mutable_phases", phase);
}

bool ir_global_can_initialize(const ir_global_t *global,
                              ir_execution_phase_t phase)
{
    return global_phase_allows(global, "ir.global_initializer_phases", phase);
}

bool ir_global_set_storage_class(ir_ctx_t *ctx, ir_global_t *global,
                                 const char *requested, bool required,
                                 const char *selected)
{
    if (!ctx || !global
        || (required && (!requested || !*requested || !selected
                         || strcmp(requested, selected)))) return false;
    const char *request = ir_strdup(ctx, requested ? requested : "");
    const char *selection = ir_strdup(ctx, selected ? selected : "");
    return request && selection
        && ir_annot_set(ctx, &global->annots, (ir_annot_t){
               .key = "ir.storage_requested",
               .kind = IR_ANNOT_STR,
               .s = request,
           })
        && ir_annot_set(ctx, &global->annots, (ir_annot_t){
               .key = "ir.storage_selected",
               .kind = IR_ANNOT_STR,
               .s = selection,
           })
        && ir_annot_set(ctx, &global->annots, (ir_annot_t){
               .key = "ir.storage_required",
               .kind = IR_ANNOT_BOOL,
               .b = required,
           });
}

const char *ir_global_storage_class(const ir_global_t *global,
                                    bool *required,
                                    const char **requested)
{
    if (required) *required = false;
    if (requested) *requested = nullptr;
    if (!global) return nullptr;
    ir_annots_t *annots = (ir_annots_t *)&global->annots;
    ir_annot_t *need = ir_annot_get(annots, "ir.storage_required");
    ir_annot_t *request = ir_annot_get(annots, "ir.storage_requested");
    ir_annot_t *selection = ir_annot_get(annots, "ir.storage_selected");
    if (required && need && need->kind == IR_ANNOT_BOOL)
        *required = need->b;
    if (requested && request && request->kind == IR_ANNOT_STR
        && request->s && *request->s) *requested = request->s;
    return selection && selection->kind == IR_ANNOT_STR && selection->s
        && *selection->s ? selection->s : nullptr;
}

bool ir_global_set_storage_duration(ir_ctx_t *ctx, ir_global_t *global,
                                    ir_storage_duration_t duration)
{
    if (!ctx || !global || duration > IR_STORAGE_DURATION_DYNAMIC)
        return false;
    return ir_annot_set(ctx, &global->annots, (ir_annot_t){
        .key = "ir.storage_duration",
        .kind = IR_ANNOT_INT,
        .i = {.mag = duration},
    });
}

ir_storage_duration_t ir_global_storage_duration(
    const ir_global_t *global)
{
    if (!global) return IR_STORAGE_DURATION_UNSPECIFIED;
    ir_annot_t *duration = ir_annot_get(
        (ir_annots_t *)&global->annots, "ir.storage_duration");
    if (!duration || duration->kind != IR_ANNOT_INT || duration->i.neg
        || duration->i.mag > IR_STORAGE_DURATION_DYNAMIC)
        return IR_STORAGE_DURATION_UNSPECIFIED;
    return (ir_storage_duration_t)duration->i.mag;
}

const char *ir_prop_name(ir_props_t one)
{
    switch (one) {
    case IR_PROP_PARSED:
        return "parsed";
    case IR_PROP_TYPED:
        return "typed";
    case IR_PROP_SSA:
        return "ssa";
    case IR_PROP_STRUCTURED:
        return "structured";
    case IR_PROP_LAID_OUT:
        return "laid-out";
    case IR_PROP_NO_CRIT_EDGE:
        return "no-critical-edges";
    case IR_PROP_VERIFIED:
        return "verified";
    case IR_PROP_TREE:
        return "tree";
    case IR_PROP_SCOPED:
        return "scoped";
    case IR_PROP_SYMBOLS:
        return "symbols";
    case IR_PROP_CFLOW:
        return "control-flow";
    case IR_PROP_TREE_TYPED:
        return "tree-typed";
    default:
        return nullptr;
    }
}

static core_dict_t *index_of(ir_ctx_t *ctx, void **slot, core_hash_fn hash,
                            core_dict_key_eq_fn eq)
{
    if (!*slot) {
        core_dict_t *ix = core_alloc(core_dict_t);

        core_dict_init(ix, hash, eq);
        ir_ctx_on_free(ctx, drop_dict, ix);
        *slot = ix;
    }

    return *slot;
}

static ir_typeslot_t *type_find(ir_module_t *m, uint64_t h)
{
    core_dict_t *ix = index_of(m->ctx, &m->type_index, core_hash_word, nullptr);
    bool        found = false;
    size_t      at    = (size_t)(uintptr_t)core_dict_get(ix, h, &found);

    return found ? &m->types[at - 1] : nullptr;
}

th_type *ir_type_intern(ir_module_t *m, th_type *t)
{
    if (!m || !t) {
        return nullptr;
    }

    uint64_t       h    = ir_type_id(m, t);
    ir_typeslot_t *slot = type_find(m, h);

    if (slot) {
        return slot->type;
    }

    m->types = ir_grow(m->ctx, m->types, &m->cap_types, m->n_types + 1,
                       sizeof(ir_typeslot_t));
    m->types[m->n_types].hash = h;
    m->types[m->n_types].type = t;
    m->types[m->n_types].def  = nullptr;
    m->n_types++;
    core_dict_put((core_dict_t *)m->type_index, h, m->n_types);

    return t;
}

ir_typeslot_t *ir_type_slot(ir_module_t *m, th_type *t)
{
    if (!m || !t) {
        return nullptr;
    }
    ir_type_intern(m, t);

    return type_find(m, ir_type_id(m, t));
}

ir_typeslot_t *ir_type_slot_at(ir_module_t *m, size_t i)
{
    return m && i < m->n_types ? &m->types[i] : nullptr;
}

th_type *ir_type_at(ir_module_t *m, size_t i)
{
    return m && i < m->n_types ? m->types[i].type : nullptr;
}

bool ir_type_define(ir_module_t *m, th_type *named, th_type *def)
{
    if (!m || !named || !def || th_kind_of(named) != TH_NAMED) {
        return false;
    }

    ir_type_intern(m, named);
    ir_type_intern(m, def);

    ir_typeslot_t *slot = type_find(m, th_hash(m->ctx->types, named));

    if (!slot) {
        return false;
    }
    uint64_t hash = th_hash(m->ctx->types, named);
    for (size_t i = 0; i < m->ctx->type_definition_count; i++) {
        ir_context_type_definition_t *entry =
            &m->ctx->type_definitions[i];
        if (entry->hash != hash) continue;
        if (th_hash(m->ctx->types, entry->definition)
            != th_hash(m->ctx->types, def))
            return false;
        bool changed = slot->def == nullptr;
        slot->def = entry->definition;
        if (changed) ir_layout_invalidate_type(m, named);
        return true;
    }
    m->ctx->type_definitions = ir_grow(m->ctx,
        m->ctx->type_definitions, &m->ctx->type_definition_capacity,
        m->ctx->type_definition_count + 1,
        sizeof(*m->ctx->type_definitions));
    m->ctx->type_definitions[m->ctx->type_definition_count++] =
        (ir_context_type_definition_t){
            .hash = hash,
            .type = named,
            .definition = def,
            .owner = m,
        };
    slot->def = def;
    ir_layout_invalidate_type(m, named);

    return true;
}

th_type *ir_type_definition(ir_module_t *m, th_type *named)
{
    if (!m || !named || th_kind_of(named) != TH_NAMED) {
        return nullptr;
    }

    uint64_t hash = th_hash(m->ctx->types, named);
    for (size_t i = 0; i < m->ctx->type_definition_count; i++) {
        ir_context_type_definition_t *entry =
            &m->ctx->type_definitions[i];
        if (entry->hash == hash) return entry->definition;
    }
    ir_typeslot_t *slot = type_find(m, hash);
    return slot ? slot->def : nullptr;
}

ir_module_t *ir_type_definition_owner(ir_module_t *m, th_type *named)
{
    if (!m || !named || th_kind_of(named) != TH_NAMED) return nullptr;
    uint64_t hash = th_hash(m->ctx->types, named);
    for (size_t i = 0; i < m->ctx->type_definition_count; i++) {
        ir_context_type_definition_t *entry =
            &m->ctx->type_definitions[i];
        if (entry->hash == hash) return entry->owner;
    }
    return nullptr;
}

size_t ir_call_output_count(th_type *callable)
{
    callable = th_resolve(callable);
    if (!callable || th_kind_of(callable) != TH_FUNCTION) return 0;
    size_t output_count = 0;
    for (size_t i = 0; i < th_func_nparams(callable); i++) {
        const th_param *parameter = th_func_param(callable, i);
        if (parameter && parameter->mode != TH_IN) output_count++;
    }
    for (size_t i = 0; i < th_func_nkw(callable); i++) {
        const th_kwparam *parameter = th_func_kw(callable, i);
        if (parameter && parameter->mode != TH_IN) output_count++;
    }
    const th_param *rest = th_func_rest(callable);
    if (rest && rest->mode != TH_IN) output_count++;
    return output_count;
}

th_type *ir_call_output_type(th_type *callable, size_t output_index)
{
    callable = th_resolve(callable);
    if (!callable || th_kind_of(callable) != TH_FUNCTION) return nullptr;
    for (size_t i = 0; i < th_func_nparams(callable); i++) {
        const th_param *parameter = th_func_param(callable, i);
        if (parameter && parameter->mode != TH_IN) {
            if (!output_index--) return parameter->type;
        }
    }
    for (size_t i = 0; i < th_func_nkw(callable); i++) {
        const th_kwparam *parameter = th_func_kw(callable, i);
        if (parameter && parameter->mode != TH_IN) {
            if (!output_index--) return parameter->type;
        }
    }
    const th_param *rest = th_func_rest(callable);
    return rest && rest->mode != TH_IN && !output_index ? rest->type : nullptr;
}

th_type *ir_call_outcome_type(ir_module_t *m, th_type *callable)
{
    if (!m || !callable) return nullptr;
    callable = th_resolve(callable);
    if (!callable || th_kind_of(callable) != TH_FUNCTION) return nullptr;

    th_type *ret = th_func_ret(callable);
    th_type *error = th_func_error(callable);
    if (!ret) return nullptr;
    if (!error || ir_machine_of(m, error).class == IR_CLASS_VOID)
        return ir_type_intern(m, ret);

    th_type *parts[] = {ret, error};
    th_type *key = th_tuple(m->ctx->types, parts, 2);
    if (!key) return nullptr;
    char name[64];
    snprintf(name, sizeof(name), "__lantern_call_outcome_%016llx",
             (unsigned long long)th_hash(m->ctx->types, key));
    th_type *outcome = th_variant(m->ctx->types, name, nullptr, 0);
    th_member arms[] = {{"ok", ret}, {"error", error}};
    th_type *definition = th_record(m->ctx->types, arms, 2);
    th_type *existing = ir_type_definition(m, outcome);
    if (!existing && !ir_type_define(m, outcome, definition)) return nullptr;
    if (existing && th_hash(m->ctx->types, existing)
                        != th_hash(m->ctx->types, definition))
        return nullptr;
    return ir_type_intern(m, outcome);
}

th_type *ir_type_parse(ir_module_t *m, const char *text)
{
    if (!m || !text) {
        return nullptr;
    }

    char    *err = nullptr;
    th_type *t   = ir_typelang_parse(m->ctx->types, text, &err);

    core_free(err);

    return t ? ir_type_intern(m, t) : nullptr;
}

uint64_t ir_type_id(ir_module_t *m, th_type *t)
{
    if (!m || !t) {
        return 0;
    }

    // Inference variables can change their resolved shape after unification.
    // Their identities are therefore live hashes, not cacheable IDs.
    if (!th_is_closed(m->ctx->types, t)) {
        return th_hash(m->ctx->types, t);
    }

    core_dict_t *ix = index_of(m->ctx, &m->id_index, core_hash_word, nullptr);

    if (!ix) {
        return th_hash(m->ctx->types, t);
    }

    bool   found = false;
    size_t at    = (size_t)(uintptr_t)core_dict_get(ix, t, &found);

    if (found) {
        return m->ids[at - 1];
    }

    m->ids = ir_grow(m->ctx, m->ids, &m->cap_ids, m->n_ids + 1,
                     sizeof(uint64_t));
    m->ids[m->n_ids] = th_hash(m->ctx->types, t);
    m->n_ids++;
    core_dict_put(ix, t, m->n_ids);

    return m->ids[m->n_ids - 1];
}

bool ir_type_eq(ir_module_t *m, th_type *a, th_type *b)
{
    if (a == b) {
        return true;
    }

    if (!m || !a || !b) {
        return false;
    }

    return ir_type_id(m, a) == ir_type_id(m, b);
}

size_t ir_type_count(const ir_module_t *m)
{
    return m ? m->n_types : 0;
}

int32_t ir_file_add(ir_module_t *m, const char *path)
{
    if (!m || !path) {
        return -1;
    }

    core_dict_t *ix = index_of(m->ctx, &m->file_index, core_hash_cstring,
                              core_dict_cstr_eq);

    if (!ix) {
        return -1;
    }

    bool   found = false;
    size_t at    = (size_t)(uintptr_t)core_dict_get(ix, path, &found);

    if (found) {
        return (int32_t)at;
    }

    m->files = ir_grow(m->ctx, m->files, &m->cap_files, m->n_files + 1,
                       sizeof(char *));
    m->files[m->n_files] = ir_strdup(m->ctx, path);
    core_dict_put((core_dict_t *)m->file_index, m->files[m->n_files],
                 m->n_files);

    return (int32_t)m->n_files++;
}

const char *ir_file_path(const ir_module_t *m, int32_t id)
{
    if (!m || id < 0 || (size_t)id >= m->n_files) {
        return nullptr;
    }

    return m->files[id];
}

size_t ir_file_count(const ir_module_t *m)
{
    return m ? m->n_files : 0;
}
