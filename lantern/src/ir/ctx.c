#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "private/ir/internal.h"
#include "ir/typelang.h"
#include "ir/walk.h"
#include "parse/annot_walk.h"

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

core_string_t ir_strdup(ir_ctx_t *ctx, core_string_t s)
{
    if (!s.data) {
        return (core_string_t){};
    }

    char *d = ir_alloc(ctx, s.u8_bytes + 1);

    memcpy(d, s.data, s.u8_bytes);
    d[s.u8_bytes] = 0;

    return (core_string_t){.data = d, .u8_bytes = s.u8_bytes};
}

void *ir_grow(ir_ctx_t *ctx, void *p, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap) return p;
    return core_arena_try_reserve_raw(&ctx->arena, &p, cap,
        need < 8 ? 8 : need, elem) ? p : nullptr;
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

ir_module_t *ir_module_new(ir_ctx_t *ctx, core_string_t name,
                           const ir_profile_t *profile, core_string_t target)
{
    if (!ctx || !name.data || !profile) {
        return nullptr;
    }

    ir_module_t *m = ir_alloc(ctx, sizeof(ir_module_t));

    m->ctx     = ctx;
    m->name    = ir_strdup(ctx, name);
    m->target  = ir_strdup(ctx, target);
    m->profile = profile;
    m->span    = IR_SPAN_UNKNOWN;
    ir_profile_install_builtins(ctx, profile);

    return m;
}

th_ctx *ir_module_types(ir_module_t *m)
{
    return m ? ir_ctx_types(m->ctx) : nullptr;
}

ir_ctx_t *ir_module_ctx(ir_module_t *m)
{
    return m ? m->ctx : nullptr;
}

core_string_t ir_module_name(const ir_module_t *m)
{
    return m ? m->name : (core_string_t){};
}

core_string_t ir_module_target(const ir_module_t *m)
{
    return m ? m->target : (core_string_t){};
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
    m->scalar_hashes = nullptr;
    m->scalars_ready = false;
    ir_profile_install_builtins(m->ctx, p);
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

void ir_module_set_target(ir_module_t *m, core_string_t target)
{
    if (m && target.data) {
        m->target = ir_strdup(m->ctx, target);
    }
}

ir_annots_t *ir_module_annots(ir_module_t *m)
{
    return m ? &m->annots : nullptr;
}

core_string_t ir_mutability_name(ir_mutability_t m)
{
    switch (m) {
    case IR_MUT_MUTABLE:
        return CORE_STRING_STATIC("mutable");
    case IR_MUT_IMMUTABLE:
        return CORE_STRING_STATIC("immutable");
    case IR_MUT_CONST:
        return CORE_STRING_STATIC("const");
    default:
        return (core_string_t){};
    }
}

bool ir_mutability_from_name(core_string_t s, ir_mutability_t *out)
{
    if (!s.data) {
        return false;
    }

    for (ir_mutability_t m = IR_MUT_MUTABLE; m <= IR_MUT_CONST; m++) {
        if (core_string_eq(s, ir_mutability_name(m))) {
            if (out) {
                *out = m;
            }

            return true;
        }
    }

    return false;
}

bool ir_func_set_initializer(ir_ctx_t *ctx, ir_func_t *function,
                             bool is_initializer)
{
    if (!ctx || !function) return false;
    return ir_annot_set(ctx, &function->annots, (ir_annot_t){
        .key = CORE_STRING_STATIC("ir.initializer"),
        .kind = IR_ANNOT_BOOL,
        .b = is_initializer,
    });
}

bool ir_func_is_initializer(const ir_func_t *function)
{
    if (!function) return false;
    ir_annot_t *initializer = ir_annot_get(
        (ir_annots_t *)&function->annots, CORE_STRING_STATIC("ir.initializer"));
    return initializer && initializer->kind == IR_ANNOT_BOOL
        && initializer->b;
}

bool ir_func_set_always_inline(ir_ctx_t *ctx, ir_func_t *function,
                               bool enabled)
{
    if (!ctx || !function) return false;
    return ir_annot_set(ctx, &function->annots, (ir_annot_t){
        .key = CORE_STRING_STATIC("ir.always_inline"),
        .kind = IR_ANNOT_BOOL,
        .b = enabled,
    });
}

bool ir_func_is_always_inline(const ir_func_t *function)
{
    if (!function) return false;
    ir_annot_t *inline_attr = ir_annot_get(
        (ir_annots_t *)&function->annots, CORE_STRING_STATIC("ir.always_inline"));
    return inline_attr && inline_attr->kind == IR_ANNOT_BOOL
        && inline_attr->b;
}

bool ir_value_set_external_writable_storage(ir_ctx_t *ctx, ir_value_t *value)
{
    ir_annots_t *annots = value ? ir_value_annots(value) : nullptr;
    return ctx && annots && ir_annot_set(ctx, annots, (ir_annot_t){
        .key = CORE_STRING_STATIC("ir.external_writable_storage"), .kind = IR_ANNOT_BOOL,
        .b = true,
    });
}

bool ir_value_has_external_writable_storage(const ir_value_t *value)
{
    ir_annots_t *annots = value ? ir_value_annots((ir_value_t *)value)
                                : nullptr;
    ir_annot_t *marker = annots
        ? ir_annot_get(annots, CORE_STRING_STATIC("ir.external_writable_storage")) : nullptr;
    return marker && marker->kind == IR_ANNOT_BOOL && marker->b;
}

bool ir_value_set_indirect_access(ir_ctx_t *ctx, ir_value_t *value,
                                  ir_indirect_access_t access)
{
    th_type *type = value && value->type ? th_resolve(value->type) : nullptr;
    th_type *definition = ctx && type && th_kind_of(type) == TH_NAMED
        ? th_type_definition(ir_ctx_types(ctx), type) : nullptr;
    bool reference = type && (th_kind_of(type) == TH_INDIRECTION
        || (definition && th_kind_of(th_resolve(definition)) == TH_INDIRECTION));
    if (!ctx || !reference
        || access < IR_INDIRECT_ACCESS_READ
        || access > IR_INDIRECT_ACCESS_READ_WRITE) return false;
    return ir_annot_set(ctx, ir_value_annots(value), (ir_annot_t){
        .key = CORE_STRING_STATIC("ir.indirect_access"),
        .kind = IR_ANNOT_INT,
        .i = {.mag = access},
    });
}

ir_indirect_access_t ir_value_indirect_access(const ir_value_t *value)
{
    if (!value || !value->type) return IR_INDIRECT_ACCESS_UNSPECIFIED;
    ir_annot_t *access = ir_annot_get(
        ir_value_annots((ir_value_t *)value), CORE_STRING_STATIC("ir.indirect_access"));
    if (!access || access->kind != IR_ANNOT_INT || access->i.neg
        || access->i.mag < IR_INDIRECT_ACCESS_READ
        || access->i.mag > IR_INDIRECT_ACCESS_READ_WRITE)
        return IR_INDIRECT_ACCESS_UNSPECIFIED;
    return (ir_indirect_access_t)access->i.mag;
}

bool ir_global_set_storage_class(ir_ctx_t *ctx, ir_global_t *global,
                                 core_string_t requested, bool required,
                                 core_string_t selected)
{
    if (!ctx || !global
        || (required && (!requested.u8_bytes || !selected.data
                         || !core_string_eq(requested, selected))))
        return false;
    core_string_t request = ir_strdup(
        ctx, requested.data ? requested : CORE_STRING_STATIC(""));
    core_string_t selection = ir_strdup(
        ctx, selected.data ? selected : CORE_STRING_STATIC(""));
    return request.data && selection.data
        && ir_annot_set(ctx, &global->annots, (ir_annot_t){
               .key = CORE_STRING_STATIC("ir.storage_requested"),
               .kind = IR_ANNOT_STR,
               .s = request,
           })
        && ir_annot_set(ctx, &global->annots, (ir_annot_t){
               .key = CORE_STRING_STATIC("ir.storage_selected"),
               .kind = IR_ANNOT_STR,
               .s = selection,
           })
        && ir_annot_set(ctx, &global->annots, (ir_annot_t){
               .key = CORE_STRING_STATIC("ir.storage_required"),
               .kind = IR_ANNOT_BOOL,
               .b = required,
           });
}

core_string_t ir_global_storage_class(const ir_global_t *global,
                                      bool *required,
                                      core_string_t *requested)
{
    if (required) *required = false;
    if (requested) *requested = (core_string_t){};
    if (!global) return (core_string_t){};
    ir_annots_t *annots = (ir_annots_t *)&global->annots;
    ir_annot_t *need = ir_annot_get(annots, CORE_STRING_STATIC("ir.storage_required"));
    ir_annot_t *request = ir_annot_get(annots, CORE_STRING_STATIC("ir.storage_requested"));
    ir_annot_t *selection = ir_annot_get(annots, CORE_STRING_STATIC("ir.storage_selected"));
    if (required && need && need->kind == IR_ANNOT_BOOL)
        *required = need->b;
    if (requested && request && request->kind == IR_ANNOT_STR
        && request->s.u8_bytes) *requested = request->s;
    return selection && selection->kind == IR_ANNOT_STR
        && selection->s.u8_bytes ? selection->s : (core_string_t){};
}

bool ir_global_set_storage_duration(ir_ctx_t *ctx, ir_global_t *global,
                                    ir_storage_duration_t duration)
{
    if (!ctx || !global || duration > IR_STORAGE_DURATION_DYNAMIC)
        return false;
    return ir_annot_set(ctx, &global->annots, (ir_annot_t){
        .key = CORE_STRING_STATIC("ir.storage_duration"),
        .kind = IR_ANNOT_INT,
        .i = {.mag = duration},
    });
}

ir_storage_duration_t ir_global_storage_duration(
    const ir_global_t *global)
{
    if (!global) return IR_STORAGE_DURATION_UNSPECIFIED;
    ir_annot_t *duration = ir_annot_get(
        (ir_annots_t *)&global->annots, CORE_STRING_STATIC("ir.storage_duration"));
    if (!duration || duration->kind != IR_ANNOT_INT || duration->i.neg
        || duration->i.mag > IR_STORAGE_DURATION_DYNAMIC)
        return IR_STORAGE_DURATION_UNSPECIFIED;
    return (ir_storage_duration_t)duration->i.mag;
}

core_string_t ir_prop_name(ir_props_t one)
{
    switch (one) {
    case IR_PROP_PARSED:
        return CORE_STRING_STATIC("parsed");
    case IR_PROP_TYPED:
        return CORE_STRING_STATIC("typed");
    case IR_PROP_SSA:
        return CORE_STRING_STATIC("ssa");
    case IR_PROP_STRUCTURED:
        return CORE_STRING_STATIC("structured");
    case IR_PROP_LAID_OUT:
        return CORE_STRING_STATIC("laid-out");
    case IR_PROP_NO_CRIT_EDGE:
        return CORE_STRING_STATIC("no-critical-edges");
    case IR_PROP_VERIFIED:
        return CORE_STRING_STATIC("verified");
    default:
        return (core_string_t){};
    }
}

static core_dict_t *index_of(ir_ctx_t *ctx, void **slot, bool string_keys)
{
    if (!*slot) {
        core_dict_t *ix = core_alloc(core_dict_t);

        if (string_keys) {
            core_dict_init_string(ix);
        }
        else {
            core_dict_init(ix, core_hash_word, nullptr);
        }
        ir_ctx_on_free(ctx, drop_dict, ix);
        *slot = ix;
    }

    return *slot;
}

static ir_typeslot_t *type_find(ir_module_t *m, uint64_t h)
{
    core_dict_t *ix = index_of(m->ctx, &m->type_index, false);
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
    m->types[m->n_types] = (ir_typeslot_t){.hash = h, .type = t};
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

ir_typeslot_t *ir_type_slot_find(ir_module_t *m, th_type *t)
{
    return m && t ? type_find(m, ir_type_id(m, t)) : nullptr;
}

ir_typeslot_t *ir_type_slot_at(ir_module_t *m, size_t i)
{
    return m && i < m->n_types ? &m->types[i] : nullptr;
}

static bool merge_type_slot(ir_module_t *m, ir_typeslot_t *into,
                            const ir_typeslot_t *from)
{
    if (!into->def) {
        into->def = from->def;
    }
    for (size_t i = 0; i < from->n_descs; i++) {
        bool present = false;

        for (size_t j = 0; !present && j < into->n_descs; j++) {
            present = core_string_eq(into->descs[j]->target,
                                     from->descs[i]->target);
        }
        if (present) {
            continue;
        }
        into->descs = ir_grow(m->ctx, into->descs, &into->cap_descs,
                              into->n_descs + 1, sizeof(ir_descentry_t *));
        if (!into->descs) {
            return false;
        }
        into->descs[into->n_descs++] = from->descs[i];
    }

    return true;
}

bool ir_module_reintern_types(ir_module_t *m)
{
    if (!m) {
        return false;
    }

    core_dict_t *ix   = index_of(m->ctx, &m->type_index, false);
    size_t       kept = 0;
    bool         ok   = true;

    core_dict_free(ix);
    core_dict_init(ix, core_hash_word, nullptr);
    for (size_t i = 0; i < m->n_types; i++) {
        ir_typeslot_t slot  = m->types[i];
        uint64_t      h     = ir_type_id(m, slot.type);
        bool          found = false;
        size_t        at    = (size_t)(uintptr_t)core_dict_get(ix, h, &found);

        if (found) {
            ok = merge_type_slot(m, &m->types[at - 1], &slot) && ok;
            continue;
        }
        slot.hash       = h;
        m->types[kept++] = slot;
        core_dict_put(ix, h, kept);
    }
    m->n_types = kept;

    return ok;
}

th_type *ir_type_at(ir_module_t *m, size_t i)
{
    return m && i < m->n_types ? m->types[i].type : nullptr;
}

bool ir_type_define(ir_module_t *m, th_type *named, th_type *def)
{
    named = th_resolve(named);
    def = th_resolve(def);
    if (!m || !named || !def || th_kind_of(named) != TH_NAMED) {
        return false;
    }
    if (!th_type_define_owned(m->ctx->types, named, def, m)) return false;

    ir_type_intern(m, named);
    ir_type_intern(m, def);

    ir_typeslot_t *slot = type_find(m, ir_type_id(m, named));
    th_type *canonical = th_type_definition(m->ctx->types, named);

    if (!slot || !canonical) {
        return false;
    }
    bool changed = slot->def != canonical;
    slot->def = canonical;
    if (changed) ir_layout_invalidate_type(m, named);

    return true;
}

th_type *ir_type_definition(ir_module_t *m, th_type *named)
{
    named = th_resolve(named);
    if (!m || !named || th_kind_of(named) != TH_NAMED) {
        return nullptr;
    }

    th_type *definition = th_type_definition(m->ctx->types, named);
    if (definition) {
        if (th_is_closed(m->ctx->types, named)
            && !th_type_definition_owner(m->ctx->types, named)
            && !ir_type_define(m, named, definition)) return nullptr;
        return definition;
    }
    ir_typeslot_t *slot = type_find(m, ir_type_id(m, named));
    return slot ? slot->def : nullptr;
}

ir_module_t *ir_type_definition_owner(ir_module_t *m, th_type *named)
{
    if (!m || !named || th_kind_of(named) != TH_NAMED) return nullptr;
    return th_type_definition_owner(m->ctx->types, named);
}

size_t ir_call_output_count(th_type *callable)
{
    callable = th_resolve(callable);
    if (!callable || th_kind_of(callable) != TH_FUNCTION) return 0;
    size_t output_count = 0;
    for (size_t i = 0; i < th_func_nparams(callable); i++) {
        const th_param *parameter = th_func_param(callable, i);
        if (parameter && th_mode_produces_output(parameter->mode)) output_count++;
    }
    for (size_t i = 0; i < th_func_nkw(callable); i++) {
        const th_kwparam *parameter = th_func_kw(callable, i);
        if (parameter && th_mode_produces_output(parameter->mode)) output_count++;
    }
    const th_param *rest = th_func_rest(callable);
    if (rest && th_mode_produces_output(rest->mode)) output_count++;
    return output_count;
}

th_type *ir_call_function_type(const ir_instr_t *call)
{
    if (!call) return nullptr;
    th_type *signature = ir_instr_call_signature(call);
    if (!signature && call->n_operands) {
        const ir_value_t *callee = ir_operand_value(&call->operands[0]);
        signature = callee ? callee->type : nullptr;
    }
    signature = signature ? th_resolve(signature) : nullptr;
    if (signature && th_kind_of(signature) == TH_INDIRECTION)
        signature = th_resolve(th_ind_referent(signature));
    if (signature && th_kind_of(signature) == TH_BINDER)
        signature = th_binder_body(signature);
    return signature && th_kind_of(signature) == TH_FUNCTION ? signature
                                                             : nullptr;
}

const ir_instr_t *ir_call_output_source(const ir_instr_t *output,
                                        size_t *ordinal)
{
    if (!output || output->op != IR_OP_CALL_OUTPUT || output->n_operands != 2
        || output->operands[1].kind != IR_OPD_CONST
        || output->operands[1].lit.is_float
        || output->operands[1].lit.i.neg
        || output->operands[1].lit.i.mag > SIZE_MAX) return nullptr;
    const ir_value_t *source = ir_operand_value(&output->operands[0]);
    const ir_instr_t *call = source && source->kind == IR_VAL_INSTR
        ? source->def : nullptr;
    if (call && call->op == IR_OP_VARIANT_PAYLOAD) {
        source = call->n_operands ? ir_operand_value(&call->operands[0])
                                  : nullptr;
        call = source && source->kind == IR_VAL_INSTR ? source->def : nullptr;
    }
    if (!call || (call->op != IR_OP_CALL
                  && call->op != IR_OP_CALLBACK_INVOKE)) return nullptr;
    if (ordinal) *ordinal = (size_t)output->operands[1].lit.i.mag;
    return call;
}

size_t ir_call_output_param(th_type *callable, size_t output_index)
{
    callable = th_resolve(callable);
    if (!callable || th_kind_of(callable) != TH_FUNCTION) return SIZE_MAX;
    for (size_t i = 0; i < th_func_nparams(callable); i++) {
        const th_param *parameter = th_func_param(callable, i);
        if (parameter && th_mode_produces_output(parameter->mode)
            && !output_index--) return i;
    }
    return SIZE_MAX;
}

size_t ir_call_param_output(th_type *callable, size_t param)
{
    callable = th_resolve(callable);
    if (!callable || th_kind_of(callable) != TH_FUNCTION
        || param >= th_func_nparams(callable)) return SIZE_MAX;
    size_t output = 0;
    for (size_t i = 0; i < param; i++) {
        const th_param *parameter = th_func_param(callable, i);
        output += parameter && th_mode_produces_output(parameter->mode);
    }
    const th_param *parameter = th_func_param(callable, param);
    return parameter && th_mode_produces_output(parameter->mode) ? output
                                                                 : SIZE_MAX;
}

bool ir_callable_returns_value(ir_module_t *m, th_type *callable)
{
    callable = callable ? th_resolve(callable) : nullptr;
    th_type *ret = callable && th_kind_of(callable) == TH_FUNCTION
        ? th_func_ret(callable) : nullptr;
    return ret && ir_machine_of(m, ret).class != IR_CLASS_VOID;
}

size_t ir_return_output_operand(ir_module_t *m, th_type *callable,
                                size_t output_index)
{
    return (ir_callable_returns_value(m, callable) ? 1 : 0) + output_index;
}

th_type *ir_call_output_type(th_type *callable, size_t output_index)
{
    callable = th_resolve(callable);
    if (!callable || th_kind_of(callable) != TH_FUNCTION) return nullptr;
    for (size_t i = 0; i < th_func_nparams(callable); i++) {
        const th_param *parameter = th_func_param(callable, i);
        if (parameter && th_mode_produces_output(parameter->mode)) {
            if (!output_index--) return parameter->type;
        }
    }
    for (size_t i = 0; i < th_func_nkw(callable); i++) {
        const th_kwparam *parameter = th_func_kw(callable, i);
        if (parameter && th_mode_produces_output(parameter->mode)) {
            if (!output_index--) return parameter->type;
        }
    }
    const th_param *rest = th_func_rest(callable);
    return rest && th_mode_produces_output(rest->mode) && !output_index
        ? rest->type : nullptr;
}

th_type *ir_call_outcome_type(ir_module_t *m, th_type *callable)
{
    if (!m || !callable) return nullptr;
    callable = th_resolve(callable);
    if (!callable || th_kind_of(callable) != TH_FUNCTION) return nullptr;

    th_type *ret = th_func_ret(callable);
    th_type *error = th_func_error(callable);
    if (!ret) return nullptr;
    if (!error
        || (th_is_closed(m->ctx->types, error)
            && ir_machine_of(m, error).class == IR_CLASS_VOID))
        return ir_type_intern(m, ret);

    th_arg arguments[] = {
        {.kind = TH_ARG_TYPE, .type = ret},
        {.kind = TH_ARG_TYPE, .type = error},
    };
    th_type *outcome = th_variant(
        m->ctx->types, CORE_STRING_STATIC("__lantern_call_outcome_t"),
        arguments, 2);
    th_member arms[] = {{CORE_STRING_STATIC("ok"), ret},
                        {CORE_STRING_STATIC("error"), error}};
    th_type *definition = th_record(m->ctx->types, arms, 2);
    th_type *existing = ir_type_definition(m, outcome);
    if (existing && !ir_type_eq(m, existing, definition))
        return nullptr;
    ir_typeslot_t *slot = ir_type_slot(m, outcome);
    if (!slot) return nullptr;
    if (!slot->def
        && !ir_type_define(m, outcome, existing ? existing : definition))
        return nullptr;
    return ir_type_intern(m, outcome);
}

th_type *ir_type_parse(ir_module_t *m, core_string_t text)
{
    if (!m || !text.data) {
        return nullptr;
    }

    core_string_t err = {};
    th_type      *t   = ir_typelang_parse(m->ctx->types, text, &err);

    core_free(err.data);

    return t ? ir_type_intern(m, t) : nullptr;
}

uint64_t ir_type_id(ir_module_t *m, th_type *t)
{
    if (!m || !t) {
        return 0;
    }

    core_dict_t *ix = (core_dict_t *)m->id_index;
    bool found = false;
    size_t at = ix ? (size_t)(uintptr_t)core_dict_get(ix, t, &found) : 0;
    if (found) return m->ids[at - 1];

    if (!th_is_closed(m->ctx->types, t)) {
        uint64_t h = th_hash(m->ctx->types, t);
        th_type **vars = nullptr;
        size_t n = th_free_vars(m->ctx->types, t, &vars);
        for (size_t i = 0; i < n; i++) {
            uintptr_t key[] = {(uintptr_t)h, (uintptr_t)vars[i]};
            h = core_xxh3_64bits_raw(key, sizeof(key));
        }
        return h;
    }

    ix = index_of(m->ctx, &m->id_index, false);

    if (!ix) {
        return th_hash(m->ctx->types, t);
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

int32_t ir_file_add(ir_module_t *m, core_string_t path)
{
    if (!m || !path.data) {
        return -1;
    }

    core_dict_t *ix = index_of(m->ctx, &m->file_index, true);

    if (!ix) {
        return -1;
    }

    bool   found = false;
    size_t at    = (size_t)(uintptr_t)core_dict_get(ix, &path, &found);

    if (found) {
        return (int32_t)at;
    }

    m->files = ir_grow(m->ctx, m->files, &m->cap_files, m->n_files + 1,
                       sizeof(core_string_t));

    core_string_t *key = ir_alloc(m->ctx, sizeof(core_string_t));

    *key                 = ir_strdup(m->ctx, path);
    m->files[m->n_files] = *key;
    core_dict_put((core_dict_t *)m->file_index, key, m->n_files);

    return (int32_t)m->n_files++;
}

core_string_t ir_file_path(const ir_module_t *m, int32_t id)
{
    if (!m || id < 0 || (size_t)id >= m->n_files) {
        return (core_string_t){};
    }

    return m->files[id];
}

size_t ir_file_count(const ir_module_t *m)
{
    return m ? m->n_files : 0;
}
