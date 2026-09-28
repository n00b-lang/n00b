#include <stdlib.h>
#include <string.h>

#include "private/ir/internal.h"

#include "lib/dict.h"

static void drop_index(void *p)
{
    core_dict_free(p);
    core_free(p);
}

static core_dict_t *index_of(ir_ctx_t *ctx, void **slot)
{
    core_dict_t *d = *slot;

    if (!d) {
        d = core_alloc(core_dict_t);

        core_dict_init(d, core_hash_cstring, core_dict_cstr_eq);
        ir_ctx_on_free(ctx, drop_index, d);
        *slot = d;
    }

    return d;
}

void ir_index_add(ir_ctx_t *ctx, void **slot, const char *name, void *node)
{
    core_dict_t *d = index_of(ctx, slot);

    if (d) {
        core_dict_put(d, name, node);
    }
}

void *ir_index_find(void *slot, const char *name)
{
    bool found = false;

    return slot ? core_dict_get((core_dict_t *)slot, name, &found) : nullptr;
}


ir_global_t *ir_global_add(ir_module_t *m, const char *name, th_type *type)
{
    if (!m || !name || !type || ir_global_find(m, name)) {
        return nullptr;
    }

    ir_global_t *g = ir_alloc(m->ctx, sizeof(ir_global_t));

    g->owner = m;
    g->identity = g;
    g->name = ir_strdup(m->ctx, name);
    g->type = ir_type_intern(m, type);
    g->span = IR_SPAN_UNKNOWN;

    m->globals = ir_grow(m->ctx, m->globals, &m->cap_globals,
                         m->n_globals + 1, sizeof(ir_global_t *));
    m->globals[m->n_globals++] = g;
    ir_index_add(m->ctx, &m->global_index, g->name, g);

    return g;
}

static ir_global_t *global_identity_root(ir_global_t *global)
{
    ir_global_t *slow = global;
    ir_global_t *fast = global;
    while (fast && fast->identity && fast->identity != fast) {
        slow = slow->identity;
        fast = fast->identity;
        if (fast && fast->identity && fast->identity != fast)
            fast = fast->identity;
        if (fast && fast->identity && fast->identity != fast
            && slow == fast) return nullptr;
    }
    while (global && global->identity && global->identity != global)
        global = global->identity;
    return global;
}

bool ir_global_set_identity(ir_ctx_t *ctx, ir_global_t *global,
                            ir_global_t *identity)
{
    if (!ctx || !global || !identity || !global->owner || !identity->owner
        || global->owner->ctx != ctx || identity->owner->ctx != ctx)
        return false;
    ir_global_t *root = global_identity_root(identity);
    if (!root || root->owner->ctx != ctx
        || strcmp(global->name, root->name)
        || !ir_type_eq(global->owner, global->type, root->type))
        return false;
    ir_global_t *current = global_identity_root(global);
    if (!current) return false;
    if (current != global && current != root) return false;
    global->identity = root;
    return true;
}

ir_global_t *ir_global_identity(const ir_global_t *global)
{
    return global_identity_root((ir_global_t *)global);
}

ir_module_t *ir_global_owner(const ir_global_t *global)
{
    return global ? global->owner : nullptr;
}

bool ir_global_set_string_initializer(ir_module_t *m, ir_global_t *global,
                                      const char *bytes, size_t length)
{
    ir_global_init_value_t value = {
        .kind = IR_GLOBAL_INIT_STRING,
        .string = {.bytes = bytes, .length = length},
    };
    return ir_global_set_static_initializer(m, global, &value);
}

ir_global_t *ir_global_find(ir_module_t *m, const char *name)
{
    if (!m || !name) {
        return nullptr;
    }

    return ir_index_find(m->global_index, name);
}

ir_value_t *ir_global_address(ir_module_t *m, ir_global_t *global)
{
    if (!m || !global) return nullptr;
    ir_global_t *addressed = global;
    if (ir_global_find(m, global->name) != global) {
        addressed = ir_global_find(m, global->name);
        if (!addressed || !addressed->is_extern ||
            !ir_type_eq(m, addressed->type, global->type))
            return nullptr;
    }
    ir_value_t *value = ir_alloc(m->ctx, sizeof(*value));
    value->kind = IR_VAL_GLOBAL;
    value->name = addressed->name;
    value->type = addressed->type;
    value->global = addressed;
    return value;
}

ir_value_t *ir_func_address(ir_module_t *m, ir_func_t *function)
{
    if (!m || !function || ir_func_find(m, function->name) != function)
        return nullptr;
    ir_value_t *value = ir_alloc(m->ctx, sizeof(*value));
    value->kind = IR_VAL_FUNC;
    value->name = function->name;
    th_type *contract = ir_func_callable_contract(function);
    value->type = contract ? contract : function->type;
    value->owner = function;
    return value;
}

bool
ir_func_set_callable_contract(ir_func_t *function, th_type *contract)
{
    if (!function || !function->owner || !function->owner->ctx || !contract
        || th_kind_of(contract) != TH_BINDER)
        return false;
    th_type *instance = th_instantiate(function->owner->ctx->types,
                                       contract);
    if (!instance || th_kind_of(instance) != TH_FUNCTION
        || !th_unify(function->owner->ctx->types, instance, function->type))
        return false;
    return ir_annot_set(function->owner->ctx, &function->annots,
        (ir_annot_t){
            .key = "ir.callable_contract",
            .kind = IR_ANNOT_TYPE,
            .t = contract,
        });
}

th_type *
ir_func_callable_contract(const ir_func_t *function)
{
    ir_annot_t *annotation = function
        ? ir_annot_get((ir_annots_t *)&function->annots,
                       "ir.callable_contract") : nullptr;
    return annotation && annotation->kind == IR_ANNOT_TYPE
        && annotation->t && th_kind_of(annotation->t) == TH_BINDER
        ? annotation->t : nullptr;
}

static bool global_init_integer_fits(ir_lit_t value, ir_machine_t machine)
{
    uint32_t width = machine.width;
    if (machine.class == IR_CLASS_BOOL)
        return !value.neg && value.mag <= 1;
    if (!width || width > 128) return false;
    if (machine.class == IR_CLASS_UINT) {
        if (value.neg) return false;
        return width == 128 || value.mag < ((ir_uint_t)1 << width);
    }
    if (machine.class != IR_CLASS_SINT) return false;
    if (width == 128) {
        ir_uint_t sign = (ir_uint_t)1 << 127;
        return value.neg ? value.mag <= sign : value.mag < sign;
    }
    ir_uint_t sign = (ir_uint_t)1 << (width - 1);
    return value.neg ? value.mag <= sign : value.mag < sign;
}

static bool global_init_named_type(const th_type *type, const char *name,
                                   size_t n_args)
{
    const char *actual = nullptr;
    size_t actual_len = 0, actual_args = 0;
    size_t wanted_len = strlen(name);
    return type && th_kind_of(type) == TH_NAMED
        && th_named_info(type, &actual, &actual_len, nullptr, &actual_args)
        && actual_len == wanted_len && !memcmp(actual, name, wanted_len)
        && actual_args == n_args;
}

static const th_type *global_init_named_arg(const th_type *type, size_t index)
{
    const th_arg *argument = th_named_arg(type, index);
    return argument && argument->kind == TH_ARG_TYPE ? argument->type
                                                     : nullptr;
}

static const th_type *global_init_record_type(ir_module_t *module,
                                              const th_type *type)
{
    if (th_kind_of(type) == TH_RECORD) return type;
    return ir_type_definition(module, (th_type *)type);
}

static bool global_init_value_matches(ir_module_t *module,
                                      const ir_global_init_value_t *value,
                                      const th_type *type, size_t depth)
{
    if (!module || !value || !type || depth > 64) return false;
    type = th_resolve((th_type *)type);
    if (!type) return false;
    ir_machine_t machine = ir_machine_of(module, (th_type *)type);

    switch (value->kind) {
    case IR_GLOBAL_INIT_BOOL:
        return machine.class == IR_CLASS_BOOL;
    case IR_GLOBAL_INIT_INT: {
        bool fits = global_init_integer_fits(value->integer, machine);
        return machine.class != IR_CLASS_BOOL && fits;
    }
    case IR_GLOBAL_INIT_FLOAT:
        return machine.class == IR_CLASS_FLOAT;
    case IR_GLOBAL_INIT_STRING:
        return global_init_named_type(type, "string", 0)
            && (!value->string.length || value->string.bytes);
    case IR_GLOBAL_INIT_SYMBOL: {
        ir_value_t *symbol = value->symbol;
        th_type *referent = th_kind_of(type) == TH_INDIRECTION
            ? th_ind_referent(type) : nullptr;
        return symbol && referent && symbol->type
            && (symbol->kind == IR_VAL_FUNC || symbol->kind == IR_VAL_GLOBAL)
            && ir_type_eq(module, referent, symbol->type);
    }
    case IR_GLOBAL_INIT_PHASE_RESULT:
        return value->phase_result.identifier
            && value->phase_result.identifier[0];
    case IR_GLOBAL_INIT_SEQUENCE: {
        const th_type *element = nullptr;
        uint64_t extent = 0;
        bool has_extent = false;
        if (th_kind_of(type) == TH_SEQUENCE) {
            element = th_seq_elem(type);
            has_extent = th_seq_extent(type, &extent);
        }
        else if (global_init_named_type(type, "list", 1)
                 || global_init_named_type(type, "array", 1))
            element = global_init_named_arg(type, 0);
        if (!element || (value->aggregate.count && !value->aggregate.items)
            || (has_extent && extent != value->aggregate.count))
            return false;
        for (size_t i = 0; i < value->aggregate.count; i++)
            if (!global_init_value_matches(module, &value->aggregate.items[i],
                                           element, depth + 1)) return false;
        return true;
    }
    case IR_GLOBAL_INIT_TUPLE:
        if (!global_init_named_type(type, TH_TUPLE,
                                    th_named_nargs(type))
            || value->aggregate.count != th_named_nargs(type)
            || (value->aggregate.count && !value->aggregate.items))
            return false;
        for (size_t i = 0; i < value->aggregate.count; i++) {
            const th_type *element = global_init_named_arg(type, i);
            if (!element || value->aggregate.items[i].member
                || !global_init_value_matches(module,
                    &value->aggregate.items[i], element, depth + 1))
                return false;
        }
        return true;
    case IR_GLOBAL_INIT_RECORD: {
        const th_type *record = global_init_record_type(module, type);
        size_t count = value->aggregate.count;
        if (!record || th_kind_of(record) != TH_RECORD
            || count != th_record_len(record)
            || (count && !value->aggregate.items)) return false;
        for (size_t i = 0; i < count; i++) {
            const ir_global_init_value_t *item = &value->aggregate.items[i];
            const th_type *member = item->member
                ? th_record_lookup(record, item->member) : nullptr;
            if (!member) return false;
            for (size_t j = 0; j < i; j++) {
                const char *previous = value->aggregate.items[j].member;
                if (previous && !strcmp(item->member, previous)) return false;
            }
            if (!global_init_value_matches(module, item, member, depth + 1))
                return false;
        }
        return true;
    }
    case IR_GLOBAL_INIT_SET: {
        const th_type *element = global_init_named_type(type, "set", 1)
            ? global_init_named_arg(type, 0) : nullptr;
        if (!element || (value->aggregate.count && !value->aggregate.items))
            return false;
        for (size_t i = 0; i < value->aggregate.count; i++)
            if (value->aggregate.items[i].member
                || !global_init_value_matches(module,
                    &value->aggregate.items[i], element, depth + 1))
                return false;
        return true;
    }
    case IR_GLOBAL_INIT_DICTIONARY: {
        const th_type *key = global_init_named_type(type, "dict", 2)
            ? global_init_named_arg(type, 0) : nullptr;
        const th_type *element = key ? global_init_named_arg(type, 1) : nullptr;
        size_t count = value->aggregate.count;
        if (!key || !element || (count & 1u)
            || (count && !value->aggregate.items)) return false;
        for (size_t i = 0; i < count; i++)
            if (value->aggregate.items[i].member
                || !global_init_value_matches(module,
                    &value->aggregate.items[i], i & 1u ? element : key,
                    depth + 1)) return false;
        return true;
    }
    }
    return false;
}

static bool clone_global_init_value(ir_ctx_t *ctx,
                                    ir_global_init_value_t *destination,
                                    const ir_global_init_value_t *source,
                                    size_t depth)
{
    if (!ctx || !destination || !source || depth > 64) return false;
    *destination = *source;
    destination->member = source->member
        ? ir_strdup(ctx, source->member) : nullptr;
    if (source->member && !destination->member) return false;
    switch (source->kind) {
    case IR_GLOBAL_INIT_STRING: {
        if (source->string.length && !source->string.bytes) return false;
        char *copy = ir_alloc(ctx, source->string.length
                                  ? source->string.length : 1);
        if (!copy) return false;
        if (source->string.length)
            memcpy(copy, source->string.bytes, source->string.length);
        destination->string.bytes = copy;
        return true;
    }
    case IR_GLOBAL_INIT_SYMBOL:
        if (!source->symbol) return false;
        destination->symbol = ir_alloc(ctx, sizeof(*destination->symbol));
        if (!destination->symbol) return false;
        *destination->symbol = *source->symbol;
        destination->symbol->name = source->symbol->name
            ? ir_strdup(ctx, source->symbol->name) : nullptr;
        return !source->symbol->name || destination->symbol->name;
    case IR_GLOBAL_INIT_PHASE_RESULT:
        if (!source->phase_result.identifier
            || !source->phase_result.identifier[0]) return false;
        destination->phase_result.identifier = ir_strdup(
            ctx, source->phase_result.identifier);
        return destination->phase_result.identifier != nullptr;
    case IR_GLOBAL_INIT_SEQUENCE:
    case IR_GLOBAL_INIT_TUPLE:
    case IR_GLOBAL_INIT_RECORD:
    case IR_GLOBAL_INIT_SET:
    case IR_GLOBAL_INIT_DICTIONARY: {
        size_t count = source->aggregate.count;
        if (count && !source->aggregate.items) return false;
        ir_global_init_value_t *items = count
            ? ir_alloc(ctx, count * sizeof(*items)) : nullptr;
        if (count && !items) return false;
        destination->aggregate.items = items;
        for (size_t i = 0; i < count; i++)
            if (!clone_global_init_value(ctx, &items[i],
                    &source->aggregate.items[i], depth + 1)) return false;
        return true;
    }
    case IR_GLOBAL_INIT_BOOL:
    case IR_GLOBAL_INIT_INT:
    case IR_GLOBAL_INIT_FLOAT:
        return true;
    }
    return false;
}

bool ir_global_static_initializer_valid(ir_module_t *module,
                                        const ir_global_t *global)
{
    return module && global && ir_global_find(module, global->name) == global
        && global->has_static_initializer && global->static_initializer
        && !global->is_extern
        && global_init_value_matches(module, global->static_initializer,
                                     global->type, 0);
}

bool ir_global_set_static_initializer(ir_module_t *module,
                                      ir_global_t *global,
                                      const ir_global_init_value_t *value)
{
    if (!module || !global || ir_global_find(module, global->name) != global
        || global->is_extern || global->has_static_initializer
        || !global_init_value_matches(module, value, global->type, 0))
        return false;
    ir_global_init_value_t *copy = ir_alloc(module->ctx, sizeof(*copy));
    if (!copy || !clone_global_init_value(module->ctx, copy, value, 0))
        return false;
    global->static_initializer = copy;
    global->has_static_initializer = true;
    return true;
}

bool ir_global_set_phase_result_initializer(ir_module_t *module,
                                            ir_global_t *global,
                                            const char *identifier)
{
    if (!module || !global || ir_global_find(module, global->name) != global
        || global->is_extern || !identifier || !identifier[0]) return false;
    if (global->has_static_initializer) {
        const ir_global_init_value_t *existing = global->static_initializer;
        if (existing && existing->kind == IR_GLOBAL_INIT_PHASE_RESULT)
            return existing->phase_result.identifier
                && !strcmp(existing->phase_result.identifier, identifier);
    }
    ir_global_init_value_t value = {
        .kind = IR_GLOBAL_INIT_PHASE_RESULT,
        .phase_result = {.identifier = identifier},
    };
    if (!global_init_value_matches(module, &value, global->type, 0))
        return false;
    ir_global_init_value_t *copy = ir_alloc(module->ctx, sizeof(*copy));
    if (!copy || !clone_global_init_value(module->ctx, copy, &value, 0))
        return false;
    global->static_initializer = copy;
    global->has_static_initializer = true;
    return true;
}

bool ir_global_set_record_initializer(
    ir_module_t *m, ir_global_t *global,
    const ir_global_init_value_t *values, size_t count)
{
    ir_global_init_value_t value = {
        .kind = IR_GLOBAL_INIT_RECORD,
        .aggregate = {.items = values, .count = count},
    };
    return ir_global_set_static_initializer(m, global, &value);
}

size_t ir_global_count(const ir_module_t *m)
{
    return m ? m->n_globals : 0;
}

ir_global_t *ir_global_at(ir_module_t *m, size_t i)
{
    return m && i < m->n_globals ? m->globals[i] : nullptr;
}

ir_func_t *ir_func_add(ir_module_t *m, const char *name, th_type *type,
                       ir_linkage_t linkage)
{
    if (!m || !name || !type || ir_func_find(m, name)) {
        return nullptr;
    }

    if (th_kind_of(type) != TH_FUNCTION) {
        return nullptr;
    }

    ir_func_t *f = ir_alloc(m->ctx, sizeof(ir_func_t));

    f->name    = ir_strdup(m->ctx, name);
    f->type    = ir_type_intern(m, type);
    f->linkage = linkage;
    f->span    = IR_SPAN_UNKNOWN;
    f->owner   = m;

    m->funcs = ir_grow(m->ctx, m->funcs, &m->cap_funcs, m->n_funcs + 1,
                       sizeof(ir_func_t *));
    m->funcs[m->n_funcs++] = f;
    ir_index_add(m->ctx, &m->func_index, f->name, f);

    return f;
}

ir_func_t *ir_func_find(ir_module_t *m, const char *name)
{
    if (!m || !name) {
        return nullptr;
    }

    return ir_index_find(m->func_index, name);
}

size_t ir_func_count(const ir_module_t *m)
{
    return m ? m->n_funcs : 0;
}

ir_func_t *ir_func_at(ir_module_t *m, size_t i)
{
    return m && i < m->n_funcs ? m->funcs[i] : nullptr;
}

ir_value_t *ir_param_add(ir_func_t *f, const char *name, th_type *type)
{
    if (!f || !name || !type) {
        return nullptr;
    }

    ir_ctx_t   *ctx = f->owner->ctx;
    ir_value_t *v   = ir_alloc(ctx, sizeof(ir_value_t));

    v->kind  = IR_VAL_PARAM;
    v->name  = ir_strdup(ctx, name);
    v->type  = ir_type_intern(f->owner, type);
    v->owner = f;
    v->index = f->n_params;

    size_t cap = f->cap_params;

    f->params     = ir_grow(ctx, f->params, &cap, f->n_params + 1u,
                            sizeof(ir_value_t *));
    f->cap_params = (uint32_t)cap;
    f->params[f->n_params++] = v;
    ir_index_add(ctx, &f->value_index, v->name, v);

    return v;
}

ir_value_t *ir_kwparam_add(ir_func_t *f, const char *name, th_type *type)
{
    if (!f || !name || !type || ir_value_find(f, name)) return nullptr;
    const th_kwparam *declared = th_func_kw_lookup(f->type, name);
    if (!declared || !ir_type_eq(f->owner, declared->type, type))
        return nullptr;
    ir_value_t *value = ir_param_add(f, name, type);
    if (value) value->is_keyword = true;
    return value;
}

ir_value_t *ir_restparam_add(ir_func_t *f, const char *name, th_type *type)
{
    const th_param *rest = f ? th_func_rest(f->type) : nullptr;
    if (!rest || !name || !*name || !type || ir_value_find(f, name) ||
        !ir_type_eq(f->owner, rest->type, type))
        return nullptr;
    for (uint32_t i = 0; i < f->n_params; i++)
        if (f->params[i]->is_rest)
            return nullptr;
    ir_value_t *value = ir_param_add(f, name, type);
    if (value) value->is_rest = true;
    return value;
}

ir_value_t *ir_value_find(ir_func_t *f, const char *name)
{
    if (!f || !name) {
        return nullptr;
    }

    return ir_index_find(f->value_index, name);
}

ir_value_t *ir_local_value(ir_func_t *f, ir_local_t *local)
{
    if (!f || !local || ir_local_find(f, local->name) != local) {
        return nullptr;
    }
    if (!local->value) {
        ir_value_t *value = ir_alloc(f->owner->ctx, sizeof(*value));
        value->kind = IR_VAL_LOCAL;
        value->name = local->name;
        value->type = local->type;
        value->local = local;
        value->owner = f;
        local->value = value;
    }
    return local->value;
}

ir_annots_t *ir_value_annots(ir_value_t *value)
{
    if (!value) return nullptr;
    switch (value->kind) {
    case IR_VAL_PARAM:
        return &value->annots;
    case IR_VAL_INSTR:
        return value->def ? &value->def->annots : nullptr;
    case IR_VAL_LOCAL:
        return value->local ? &value->local->annots : nullptr;
    case IR_VAL_GLOBAL:
        return value->global ? &value->global->annots : nullptr;
    case IR_VAL_FUNC:
        return value->owner ? &value->owner->annots : nullptr;
    }
    return nullptr;
}

static bool alias_annotation_count(ir_annots_t *annots, const char *key,
                                   uint32_t *count)
{
    if (!annots || !key || !count) return false;
    ir_annot_t *annotation = ir_annot_get(annots, key);
    if (!annotation) {
        *count = 0;
        return true;
    }
    if (annotation->kind != IR_ANNOT_INT || annotation->i.neg
        || annotation->i.mag > UINT32_MAX
        || annotation->i.mag > ir_annot_count(annots)) return false;
    *count = (uint32_t)annotation->i.mag;
    return true;
}

bool ir_value_set_alias_targets(ir_ctx_t *ctx, ir_value_t *value,
                                const ir_alias_target_t *targets,
                                size_t target_count, bool complete)
{
    ir_annots_t *annots = ir_value_annots(value);
    if (!ctx || !value || !value->type || !annots
        || (complete && !target_count) || (target_count && !targets)
        || target_count > UINT32_MAX) return false;

    uint32_t existing = 0;
    if (!alias_annotation_count(annots, "ir.alias_count", &existing)
        || existing) return false;

    size_t unique_count = 0;
    bool escapes = false;
    for (size_t i = 0; i < target_count; i++) {
        ir_value_t *target = targets[i].value;
        ir_annots_t *target_annots = ir_value_annots(target);
        if (!target || target == value || !target_annots) return false;
        bool duplicate = false;
        for (size_t j = 0; j < i; j++)
            duplicate |= targets[j].value == target;
        if (duplicate) continue;
        uint32_t reverse_count = 0;
        if (!alias_annotation_count(target_annots,
                "ir.aliased_by_count", &reverse_count)
            || reverse_count == UINT32_MAX) return false;
        unique_count++;
        escapes |= targets[i].escapes;
    }
    if (complete && !unique_count) return false;

    for (size_t i = 0; i < target_count; i++) {
        ir_value_t *target = targets[i].value;
        bool duplicate = false;
        for (size_t j = 0; j < i; j++)
            duplicate |= targets[j].value == target;
        if (duplicate) continue;
        ir_annots_t *target_annots = ir_value_annots(target);
        uint32_t reverse_count = 0;
        if (!alias_annotation_count(target_annots,
                "ir.aliased_by_count", &reverse_count)) return false;
        char key[48];
        snprintf(key, sizeof(key), "ir.aliased_by%u", reverse_count);
        if (!ir_annot_set(ctx, target_annots, (ir_annot_t){
                .key = ir_strdup(ctx, key), .kind = IR_ANNOT_VALUE,
                .v = value,
            })) return false;
        snprintf(key, sizeof(key), "ir.aliased_by%u_escapes", reverse_count);
        if (!ir_annot_set(ctx, target_annots, (ir_annot_t){
                .key = ir_strdup(ctx, key), .kind = IR_ANNOT_BOOL,
                .b = targets[i].escapes,
            })
            || !ir_annot_set(ctx, target_annots, (ir_annot_t){
                .key = "ir.aliased_by_count", .kind = IR_ANNOT_INT,
                .i = {.mag = reverse_count + 1},
            })) return false;
    }

    if (!ir_annot_set(ctx, annots, (ir_annot_t){
            .key = "ir.alias_count", .kind = IR_ANNOT_INT,
            .i = {.mag = unique_count},
        })
        || !ir_annot_set(ctx, annots, (ir_annot_t){
            .key = "ir.aliased_by_count", .kind = IR_ANNOT_INT,
            .i = {.mag = 0},
        })) return false;
    size_t alias_index = 0;
    for (size_t i = 0; i < target_count; i++) {
        bool duplicate = false;
        for (size_t j = 0; j < i; j++)
            duplicate |= targets[j].value == targets[i].value;
        if (duplicate) continue;
        char key[48];
        snprintf(key, sizeof(key), "ir.alias%zu", alias_index);
        if (!ir_annot_set(ctx, annots, (ir_annot_t){
                .key = ir_strdup(ctx, key), .kind = IR_ANNOT_VALUE,
                .v = targets[i].value,
            })) return false;
        snprintf(key, sizeof(key), "ir.alias%zu_escapes", alias_index++);
        if (!ir_annot_set(ctx, annots, (ir_annot_t){
                .key = ir_strdup(ctx, key), .kind = IR_ANNOT_BOOL,
                .b = targets[i].escapes,
            })) return false;
    }
    return ir_annot_set(ctx, annots, (ir_annot_t){
               .key = "ir.lifetime_escapes", .kind = IR_ANNOT_BOOL,
               .b = escapes,
           })
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = "ir.alias_analysis_complete", .kind = IR_ANNOT_BOOL,
               .b = complete,
           })
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = "ir.alias_unresolved", .kind = IR_ANNOT_BOOL,
               .b = !complete,
           });
}

bool ir_value_add_alias_target(ir_ctx_t *ctx, ir_value_t *value,
                               ir_alias_target_t target)
{
    ir_annots_t *annots = ir_value_annots(value);
    ir_annots_t *target_annots = ir_value_annots(target.value);
    if (!ctx || !value || !value->type || !annots || !target.value
        || target.value == value || !target_annots) return false;

    uint32_t count = 0;
    if (!alias_annotation_count(annots, "ir.alias_count", &count))
        return false;
    if (!count && !ir_annot_get(annots, "ir.alias_analysis_complete"))
        return ir_value_set_alias_targets(ctx, value, &target, 1, false);

    ir_annot_t *analysis = ir_annot_get(annots,
                                        "ir.alias_analysis_complete");
    ir_annot_t *escape_summary = ir_annot_get(annots,
                                              "ir.lifetime_escapes");
    if (!analysis || analysis->kind != IR_ANNOT_BOOL
        || !escape_summary || escape_summary->kind != IR_ANNOT_BOOL)
        return false;

    for (uint32_t i = 0; i < count; i++) {
        char key[48];
        snprintf(key, sizeof(key), "ir.alias%u", i);
        ir_annot_t *existing = ir_annot_get(annots, key);
        snprintf(key, sizeof(key), "ir.alias%u_escapes", i);
        ir_annot_t *existing_escapes = ir_annot_get(annots, key);
        if (!existing || existing->kind != IR_ANNOT_VALUE || !existing->v
            || !existing_escapes
            || existing_escapes->kind != IR_ANNOT_BOOL) return false;
        if (existing->v != target.value) continue;
        if (!target.escapes || existing_escapes->b) return true;

        if (!ir_annot_set(ctx, annots, (ir_annot_t){
                .key = ir_strdup(ctx, key), .kind = IR_ANNOT_BOOL,
                .b = true,
            })) return false;
        uint32_t reverse_count = 0;
        if (!alias_annotation_count(target_annots,
                "ir.aliased_by_count", &reverse_count)) return false;
        for (uint32_t r = 0; r < reverse_count; r++) {
            char reverse_key[48];
            snprintf(reverse_key, sizeof(reverse_key), "ir.aliased_by%u", r);
            ir_annot_t *reverse = ir_annot_get(target_annots, reverse_key);
            snprintf(reverse_key, sizeof(reverse_key),
                     "ir.aliased_by%u_escapes", r);
            ir_annot_t *reverse_escapes = ir_annot_get(target_annots,
                                                       reverse_key);
            if (!reverse || reverse->kind != IR_ANNOT_VALUE
                || !reverse_escapes || reverse_escapes->kind != IR_ANNOT_BOOL)
                return false;
            if (reverse->v == value)
                return reverse_escapes->b || ir_annot_set(ctx,
                    target_annots, (ir_annot_t){
                        .key = ir_strdup(ctx, reverse_key),
                        .kind = IR_ANNOT_BOOL, .b = true,
                    });
        }
        return false;
    }

    uint32_t reverse_count = 0;
    if (!alias_annotation_count(target_annots, "ir.aliased_by_count",
                                 &reverse_count)
        || reverse_count == UINT32_MAX || count == UINT32_MAX) return false;
    char key[48];
    snprintf(key, sizeof(key), "ir.aliased_by%u", reverse_count);
    if (!ir_annot_set(ctx, target_annots, (ir_annot_t){
            .key = ir_strdup(ctx, key), .kind = IR_ANNOT_VALUE, .v = value,
        })) return false;
    snprintf(key, sizeof(key), "ir.aliased_by%u_escapes", reverse_count);
    if (!ir_annot_set(ctx, target_annots, (ir_annot_t){
            .key = ir_strdup(ctx, key), .kind = IR_ANNOT_BOOL,
            .b = target.escapes,
        })
        || !ir_annot_set(ctx, target_annots, (ir_annot_t){
            .key = "ir.aliased_by_count", .kind = IR_ANNOT_INT,
            .i = {.mag = reverse_count + 1},
        })) return false;

    snprintf(key, sizeof(key), "ir.alias%u", count);
    if (!ir_annot_set(ctx, annots, (ir_annot_t){
            .key = ir_strdup(ctx, key), .kind = IR_ANNOT_VALUE,
            .v = target.value,
        })) return false;
    snprintf(key, sizeof(key), "ir.alias%u_escapes", count);
    if (!ir_annot_set(ctx, annots, (ir_annot_t){
            .key = ir_strdup(ctx, key), .kind = IR_ANNOT_BOOL,
            .b = target.escapes,
        })
        || !ir_annot_set(ctx, annots, (ir_annot_t){
            .key = "ir.alias_count", .kind = IR_ANNOT_INT,
            .i = {.mag = count + 1},
        })) return false;

    bool escapes = escape_summary->b || target.escapes;
    return ir_annot_set(ctx, annots, (ir_annot_t){
               .key = "ir.lifetime_escapes", .kind = IR_ANNOT_BOOL,
               .b = escapes,
           })
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = "ir.alias_analysis_complete", .kind = IR_ANNOT_BOOL,
               .b = false,
           })
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = "ir.alias_unresolved", .kind = IR_ANNOT_BOOL,
               .b = true,
           });
}

bool ir_value_copy_alias_targets(ir_ctx_t *ctx, ir_value_t *destination,
                                 const ir_value_t *source)
{
    if (!ctx || !destination || !source || !destination->type
        || !source->type) return false;
    if (destination == source) return true;
    ir_annots_t *source_annots = ir_value_annots((ir_value_t *)source);
    ir_annot_t *count_annot = source_annots
        ? ir_annot_get(source_annots, "ir.alias_count") : nullptr;
    if (!count_annot) return true;
    if (count_annot->kind != IR_ANNOT_INT || count_annot->i.neg
        || count_annot->i.mag > UINT32_MAX
        || count_annot->i.mag > ir_annot_count(source_annots)) return false;
    ir_annot_t *complete_annot = ir_annot_get(source_annots,
        "ir.alias_analysis_complete");
    if (!complete_annot || complete_annot->kind != IR_ANNOT_BOOL)
        return false;

    size_t count = (size_t)count_annot->i.mag;
    ir_alias_target_t *targets = count
        ? calloc(count, sizeof(*targets)) : nullptr;
    if (count && !targets) return false;
    bool valid = true;
    for (size_t i = 0; valid && i < count; i++) {
        char key[48];
        snprintf(key, sizeof(key), "ir.alias%zu", i);
        ir_annot_t *target = ir_annot_get(source_annots, key);
        snprintf(key, sizeof(key), "ir.alias%zu_escapes", i);
        ir_annot_t *escapes = ir_annot_get(source_annots, key);
        valid = target && target->kind == IR_ANNOT_VALUE && target->v
            && escapes && escapes->kind == IR_ANNOT_BOOL;
        if (valid) targets[i] = (ir_alias_target_t){
            .value = target->v,
            .escapes = escapes->b,
        };
    }
    bool copied = valid && ir_value_set_alias_targets(ctx, destination,
        targets, count, complete_annot->b);
    free(targets);
    return copied;
}

ir_local_t *ir_local_add(ir_func_t *f, const char *name, th_type *type)
{
    if (!f || !name || !type || ir_local_find(f, name)) {
        return nullptr;
    }

    ir_ctx_t   *ctx = f->owner->ctx;
    ir_local_t *l   = ir_alloc(ctx, sizeof(ir_local_t));

    l->name  = ir_strdup(ctx, name);
    l->type  = ir_type_intern(f->owner, type);
    l->span  = IR_SPAN_UNKNOWN;
    l->index = f->n_locals;

    size_t cap = f->cap_locals;

    f->locals     = ir_grow(ctx, f->locals, &cap, f->n_locals + 1u,
                            sizeof(ir_local_t *));
    f->cap_locals = (uint32_t)cap;
    f->locals[f->n_locals++] = l;
    ir_index_add(ctx, &f->local_index, l->name, l);

    return l;
}

ir_local_t *ir_local_find(ir_func_t *f, const char *name)
{
    if (!f || !name) {
        return nullptr;
    }

    return ir_index_find(f->local_index, name);
}

ir_block_t *ir_block_add(ir_func_t *f, const char *name)
{
    if (!f || !name || ir_block_find(f, name)) {
        return nullptr;
    }

    ir_ctx_t   *ctx = f->owner->ctx;
    ir_block_t *b   = ir_alloc(ctx, sizeof(ir_block_t));

    b->name  = ir_strdup(ctx, name);
    b->span  = IR_SPAN_UNKNOWN;
    b->owner = f;
    b->index = f->n_blocks;

    size_t cap = f->cap_blocks;

    f->blocks     = ir_grow(ctx, f->blocks, &cap, f->n_blocks + 1u,
                            sizeof(ir_block_t *));
    f->cap_blocks = (uint32_t)cap;
    f->blocks[f->n_blocks++] = b;
    ir_index_add(ctx, &f->block_index, b->name, b);

    return b;
}

ir_block_t *ir_block_find(ir_func_t *f, const char *name)
{
    if (!f || !name) {
        return nullptr;
    }

    return ir_index_find(f->block_index, name);
}

ir_block_t *ir_block_entry(ir_func_t *f)
{
    return f && f->n_blocks ? f->blocks[0] : nullptr;
}

ir_instr_t *ir_block_terminator(ir_block_t *b)
{
    if (!b || !b->n_instrs) {
        return nullptr;
    }

    ir_instr_t *last = b->instrs[b->n_instrs - 1];

    return ir_opinfo_is_terminator(
               ir_module_op_info(b->owner->owner, last->op))
               ? last
               : nullptr;
}

ir_value_t *ir_operand_value(const ir_operand_t *od)
{
    switch (od->kind) {
    case IR_OPD_VALUE:
    case IR_OPD_SPREAD:
        return od->value;
    case IR_OPD_NAMED_ARG:
        return od->named.value;
    case IR_OPD_PHI_ARM:
        return od->phi.value;
    default:
        return nullptr;
    }
}

ir_local_t *ir_operand_local(const ir_operand_t *od)
{
    ir_value_t *v = ir_operand_value(od);

    return v && v->kind == IR_VAL_LOCAL ? v->local : nullptr;
}

bool ir_local_read_earlier(const ir_instr_t *in, uint32_t o, ir_local_t *l)
{
    for (uint32_t i = 0; i < o; i++) {
        if (ir_operand_local(&in->operands[i]) == l) {
            return true;
        }
    }

    return false;
}

ir_instr_t *ir_instr_add(ir_block_t *b, ir_op_t op)
{
    if (!b || !ir_module_op_info(b->owner->owner, op)) {
        return nullptr;
    }

    ir_ctx_t   *ctx = b->owner->owner->ctx;
    ir_instr_t *in  = ir_alloc(ctx, sizeof(ir_instr_t));

    in->op    = op;
    in->span  = IR_SPAN_UNKNOWN;
    in->block = b;
    in->index = b->n_instrs;

    size_t cap = b->cap_instrs;

    b->instrs     = ir_grow(ctx, b->instrs, &cap, b->n_instrs + 1u,
                            sizeof(ir_instr_t *));
    b->cap_instrs = (uint32_t)cap;
    b->instrs[b->n_instrs++] = in;

    return in;
}

bool ir_instr_set_storage_class(ir_instr_t *allocation,
                                const char *storage_class, bool required)
{
    if (!allocation || allocation->op != IR_OP_ALLOC
        || (required && (!storage_class || !*storage_class))) {
        return false;
    }

    ir_ctx_t *ctx = allocation->block->owner->owner->ctx;
    const char *value = storage_class ? ir_strdup(ctx, storage_class)
                                      : ir_strdup(ctx, "");

    return ir_annot_set(ctx, &allocation->annots, (ir_annot_t){
        .key = "ir.storage_class",
        .kind = IR_ANNOT_STR,
        .s = value,
    }) && ir_annot_set(ctx, &allocation->annots, (ir_annot_t){
        .key = "ir.storage_required",
        .kind = IR_ANNOT_BOOL,
        .b = required,
    });
}

static bool
address_of_set_alias(ir_instr_t *in)
{
    if (!in || in->op != IR_OP_ADDRESS_OF || !in->dest
        || in->n_operands != 1) return true;
    ir_value_t *target = ir_operand_value(&in->operands[0]);
    ir_annots_t *annots = ir_value_annots(in->dest);
    if (!target || !annots) return false;
    ir_ctx_t *ctx = in->block->owner->owner->ctx;
    ir_annots_t *target_annots = ir_value_annots(target);
    ir_annot_t *target_complete = target_annots
        ? ir_annot_get(target_annots, "ir.alias_analysis_complete") : nullptr;
    ir_annot_t *target_escapes = target_annots
        ? ir_annot_get(target_annots, "ir.lifetime_escapes") : nullptr;
    ir_annot_t *target_unresolved = target_annots
        ? ir_annot_get(target_annots, "ir.alias_unresolved") : nullptr;
    bool complete = target_complete
        && target_complete->kind == IR_ANNOT_BOOL && target_complete->b;
    bool escapes = !target_escapes
        || target_escapes->kind != IR_ANNOT_BOOL || target_escapes->b;
    bool unresolved = !complete || (target_unresolved
        && (target_unresolved->kind != IR_ANNOT_BOOL
            || target_unresolved->b));
    return ir_annot_set(ctx, annots, (ir_annot_t){
        .key = "ir.alias_count", .kind = IR_ANNOT_INT,
        .i = {.mag = 1},
    }) && ir_annot_set(ctx, annots, (ir_annot_t){
        .key = "ir.alias0", .kind = IR_ANNOT_VALUE, .v = target,
    }) && ir_annot_set(ctx, annots, (ir_annot_t){
        .key = "ir.alias0_escapes", .kind = IR_ANNOT_BOOL,
        .b = escapes || unresolved,
    }) && ir_annot_set(ctx, annots, (ir_annot_t){
        .key = "ir.aliased_by_count", .kind = IR_ANNOT_INT,
        .i = {.mag = 0},
    }) && ir_annot_set(ctx, annots, (ir_annot_t){
        .key = "ir.lifetime_escapes", .kind = IR_ANNOT_BOOL,
        .b = escapes,
    }) && ir_annot_set(ctx, annots, (ir_annot_t){
        .key = "ir.alias_analysis_complete", .kind = IR_ANNOT_BOOL,
        .b = complete && !unresolved,
    }) && ir_annot_set(ctx, annots, (ir_annot_t){
        .key = "ir.alias_unresolved", .kind = IR_ANNOT_BOOL,
        .b = unresolved,
    });
}

const char *ir_instr_storage_class(const ir_instr_t *allocation,
                                   bool *required)
{
    if (required) {
        *required = false;
    }
    if (!allocation || allocation->op != IR_OP_ALLOC) {
        return nullptr;
    }

    ir_annot_t *storage = ir_annot_get(
        (ir_annots_t *)&allocation->annots, "ir.storage_class");
    ir_annot_t *is_required = ir_annot_get(
        (ir_annots_t *)&allocation->annots, "ir.storage_required");

    if (required && is_required && is_required->kind == IR_ANNOT_BOOL) {
        *required = is_required->b;
    }

    return storage && storage->kind == IR_ANNOT_STR && storage->s
        && *storage->s ? storage->s : nullptr;
}

bool ir_instr_set_binding_initialization(ir_instr_t *instruction,
                                         bool initializes)
{
    if (!instruction || !instruction->block
        || !instruction->block->owner
        || !instruction->block->owner->owner) return false;
    ir_ctx_t *ctx = instruction->block->owner->owner->ctx;
    return ir_annot_set(ctx, &instruction->annots, (ir_annot_t){
               .key = "ir.binding_initialization",
               .kind = IR_ANNOT_BOOL,
               .b = initializes,
           });
}

bool ir_instr_is_binding_initialization(const ir_instr_t *instruction)
{
    if (!instruction) return false;
    ir_annot_t *initializes = ir_annot_get(
        (ir_annots_t *)&instruction->annots, "ir.binding_initialization");
    return initializes && initializes->kind == IR_ANNOT_BOOL
        && initializes->b;
}

bool ir_instr_set_shape_constraint(ir_instr_t *instruction,
                                   ir_shape_t *shape)
{
    if (!instruction || instruction->op != IR_OP_SEQUENCE_SHAPE_CHECK
        || !shape || instruction->shape_constraint) return false;
    instruction->shape_constraint = shape;
    return true;
}

bool ir_instr_set_aggregate_form(ir_instr_t *instruction,
                                 ir_aggregate_form_t form)
{
    if (!instruction || instruction->op != IR_OP_AGGREGATE
        || !instruction->block || !instruction->block->owner
        || !instruction->block->owner->owner
        || (form != IR_AGGREGATE_ELEMENTS && form != IR_AGGREGATE_PAIRS))
        return false;
    return ir_annot_set(instruction->block->owner->owner->ctx,
        &instruction->annots, (ir_annot_t){
            .key = "ir.aggregate.form",
            .kind = IR_ANNOT_STR,
            .s = form == IR_AGGREGATE_ELEMENTS ? "elements" : "pairs",
        });
}

bool ir_instr_aggregate_form(const ir_instr_t *instruction,
                             ir_aggregate_form_t *form)
{
    if (!instruction || !form || instruction->op != IR_OP_AGGREGATE)
        return false;
    ir_annot_t *annotation = ir_annot_get(
        (ir_annots_t *)&instruction->annots, "ir.aggregate.form");
    if (!annotation || annotation->kind != IR_ANNOT_STR || !annotation->s)
        return false;
    if (!strcmp(annotation->s, "elements")) {
        *form = IR_AGGREGATE_ELEMENTS;
        return true;
    }
    if (!strcmp(annotation->s, "pairs")) {
        *form = IR_AGGREGATE_PAIRS;
        return true;
    }
    return false;
}

bool ir_instr_set_const_bytes(ir_instr_t *instruction,
                              const uint8_t *bytes, size_t byte_len)
{
    if (!instruction || instruction->op != IR_OP_CONST
        || instruction->n_operands || (byte_len && !bytes)
        || byte_len > (SIZE_MAX - 1) / 2
        || !instruction->block || !instruction->block->owner
        || !instruction->block->owner->owner) return false;
    ir_ctx_t *ctx = instruction->block->owner->owner->ctx;
    char *hex = ir_alloc(ctx, byte_len * 2 + 1);
    if (!hex) return false;
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < byte_len; i++) {
        hex[i * 2] = digits[bytes[i] >> 4];
        hex[i * 2 + 1] = digits[bytes[i] & 0xf];
    }
    hex[byte_len * 2] = '\0';
    return ir_annot_set(ctx, &instruction->annots, (ir_annot_t){
        .key = "ir.const.bytes",
        .kind = IR_ANNOT_STR,
        .s = hex,
    });
}

const char *ir_instr_const_bytes_hex(const ir_instr_t *instruction,
                                     size_t *byte_len)
{
    ir_annot_t *annotation = instruction && instruction->op == IR_OP_CONST
        ? ir_annot_get((ir_annots_t *)&instruction->annots,
                       "ir.const.bytes") : nullptr;
    if (!annotation || annotation->kind != IR_ANNOT_STR || !annotation->s)
        return nullptr;
    size_t hex_length = strlen(annotation->s);
    if (hex_length & 1u) return nullptr;
    if (byte_len) *byte_len = hex_length / 2;
    return annotation->s;
}

bool ir_instr_set_call_signature(ir_instr_t *call, th_type *signature)
{
    if (!call || call->op != IR_OP_CALL || !signature
        || th_kind_of(th_resolve(signature)) != TH_FUNCTION)
        return false;
    ir_ctx_t *ctx = call->block && call->block->owner
        && call->block->owner->owner
        ? call->block->owner->owner->ctx : nullptr;
    return ctx && ir_annot_set(ctx, &call->annots, (ir_annot_t){
        .key = "ir.call_signature",
        .kind = IR_ANNOT_TYPE,
        .t = signature,
    });
}

th_type *ir_instr_call_signature(const ir_instr_t *call)
{
    ir_annot_t *annotation = call
        ? ir_annot_get((ir_annots_t *)&call->annots, "ir.call_signature")
        : nullptr;
    return annotation && annotation->kind == IR_ANNOT_TYPE
        && annotation->t ? annotation->t : nullptr;
}

bool
ir_instr_set_phase_result(ir_ctx_t *ctx, ir_instr_t *instruction,
                          const char *identifier)
{
    if (!ctx || !instruction || !instruction->block
        || !instruction->block->owner
        || !instruction->block->owner->owner
        || instruction->block->owner->owner->ctx != ctx
        || instruction->op != IR_OP_PHASE_RESULT
        || instruction->dest_kind != IR_DEST_VALUE || !instruction->dest
        || !identifier || !identifier[0]) return false;
    ir_annot_t *existing = ir_annot_get(&instruction->annots,
                                        "ir.phase_result");
    if (existing)
        return existing->kind == IR_ANNOT_STR && existing->s
            && !strcmp(existing->s, identifier);
    const char *stored = ir_strdup(ctx, identifier);
    return stored && ir_annot_set(ctx, &instruction->annots, (ir_annot_t){
        .key = "ir.phase_result",
        .kind = IR_ANNOT_STR,
        .s = stored,
    });
}

const char *
ir_instr_phase_result(const ir_instr_t *instruction)
{
    ir_annot_t *annotation = instruction
        ? ir_annot_get((ir_annots_t *)&instruction->annots,
                       "ir.phase_result") : nullptr;
    return annotation && annotation->kind == IR_ANNOT_STR
        ? annotation->s : nullptr;
}

bool ir_value_set_binding_reentry(ir_value_t *value,
                                 ir_block_t *loop_header)
{
    if (!value || !value->owner || !value->owner->owner
        || !loop_header
        || (value->kind != IR_VAL_PARAM && value->kind != IR_VAL_LOCAL
            && value->kind != IR_VAL_INSTR)
        || loop_header->owner != value->owner
        || loop_header->index >= value->owner->n_blocks
        || value->owner->blocks[loop_header->index] != loop_header
        || !loop_header->name) return false;
    ir_ctx_t *ctx = value->owner->owner->ctx;
    const char *name = ir_strdup(ctx, loop_header->name);
    return name && ir_annot_set(ctx, ir_value_annots(value), (ir_annot_t){
        .key = "ir.binding_reentry_header",
        .kind = IR_ANNOT_STR,
        .s = name,
    });
}

bool ir_instr_set_dest(ir_instr_t *in, const char *name, th_type *type)
{
    if (!in || !name || !type
        || !ir_opinfo_has_result(
               ir_module_op_info(in->block->owner->owner, in->op))) {
        return false;
    }

    ir_func_t  *f   = in->block->owner;
    ir_ctx_t   *ctx = f->owner->ctx;
    ir_value_t *v   = ir_alloc(ctx, sizeof(ir_value_t));

    v->kind        = IR_VAL_INSTR;
    v->name        = ir_strdup(ctx, name);
    v->type        = ir_type_intern(f->owner, type);
    v->def         = in;
    v->owner       = f;
    in->dest_kind  = IR_DEST_VALUE;
    in->dest       = v;
    in->dest_local = nullptr;
    ir_index_add(ctx, &f->value_index, v->name, v);

    return address_of_set_alias(in);
}

bool ir_instr_set_dest_local(ir_instr_t *in, ir_local_t *l)
{
    if (!in || !l
        || !ir_opinfo_has_result(
               ir_module_op_info(in->block->owner->owner, in->op))) {
        return false;
    }

    in->dest_kind  = IR_DEST_LOCAL;
    in->dest_local = l;
    in->dest       = nullptr;

    return true;
}

static ir_operand_t *push(ir_instr_t *in, ir_opdkind_t kind)
{
    if (!in) {
        return nullptr;
    }

    ir_module_t       *mod  = in->block->owner->owner;
    ir_ctx_t          *ctx  = mod->ctx;
    const ir_opinfo_t *info = ir_module_op_info(mod, in->op);

    ir_opdkind_t want = ir_opinfo_operand_kind(info, in->n_operands);

    if (want != kind
        && !(want == IR_OPD_ARG
             && (kind == IR_OPD_VALUE || kind == IR_OPD_SPREAD ||
                 kind == IR_OPD_NAMED_ARG))) {
        return nullptr;
    }

    if (info->max_operands != IR_VAR_ARITY
        && in->n_operands + 1u > info->max_operands) {
        return nullptr;
    }

    size_t        cap   = in->cap_operands;
    ir_operand_t *grown = ir_grow(ctx, in->operands, &cap, in->n_operands + 1u,
                                  sizeof(ir_operand_t));

    in->operands     = grown;
    in->cap_operands = (uint32_t)cap;

    ir_operand_t *o = &in->operands[in->n_operands++];

    memset(o, 0, sizeof(*o));
    o->kind = kind;

    return o;
}

bool ir_instr_operand_value(ir_instr_t *in, ir_value_t *v)
{
    ir_operand_t *o = v ? push(in, IR_OPD_VALUE) : nullptr;

    if (o) {
        o->value = v;
        if (in->op == IR_OP_ADDRESS_OF && v->kind == IR_VAL_LOCAL
            && v->local)
            v->local->address_taken = true;
    }

    return o && address_of_set_alias(in);
}

bool ir_instr_operand_spread(ir_instr_t *in, ir_value_t *v)
{
    ir_operand_t *o = in && in->op == IR_OP_CALL && v && v->is_rest
        ? push(in, IR_OPD_SPREAD) : nullptr;
    if (o) o->value = v;
    return o != nullptr;
}

bool ir_instr_operand_named(ir_instr_t *in, const char *name, ir_value_t *v)
{
    ir_operand_t *o = name && v ? push(in, IR_OPD_NAMED_ARG) : nullptr;

    if (o) {
        o->named.name  = ir_strdup(in->block->owner->owner->ctx, name);
        o->named.value = v;
    }

    return o != nullptr;
}

bool ir_instr_operand_local(ir_instr_t *in, ir_local_t *l)
{
    if (!in || !l) {
        return false;
    }

    return ir_instr_operand_value(
        in, ir_local_value(in->block->owner, l));
}

bool ir_instr_operand_type(ir_instr_t *in, th_type *t)
{
    ir_operand_t *o = t ? push(in, IR_OPD_TYPE) : nullptr;

    if (o) {
        o->type = ir_type_intern(in->block->owner->owner, t);
    }

    return o != nullptr;
}

bool ir_instr_operand_name(ir_instr_t *in, const char *n)
{
    ir_operand_t *o = n ? push(in, IR_OPD_NAME) : nullptr;

    if (o) {
        o->name = ir_strdup(in->block->owner->owner->ctx, n);
    }

    return o != nullptr;
}

bool ir_instr_operand_block(ir_instr_t *in, ir_block_t *b)
{
    ir_operand_t *o = b ? push(in, IR_OPD_BLOCK) : nullptr;

    if (o) {
        o->block = b;
    }

    return o != nullptr;
}

bool ir_instr_operand_default(ir_instr_t *in, ir_block_t *b)
{
    ir_operand_t *o = b ? push(in, IR_OPD_DEFAULT_BLOCK) : nullptr;

    if (o) {
        o->block = b;
    }

    return o != nullptr;
}

ir_lit_t ir_lit_of(ir_int_t v)
{
    if (v < 0) {
        return (ir_lit_t){.mag = (ir_uint_t)0 - (ir_uint_t)v, .neg = true};
    }

    return (ir_lit_t){.mag = (ir_uint_t)v};
}

ir_lit_t ir_lit_hex(ir_uint_t v)
{
    return (ir_lit_t){.mag = v, .hex = true};
}

ir_int_t ir_lit_signed(ir_lit_t v)
{
    return v.neg ? -(ir_int_t)v.mag : (ir_int_t)v.mag;
}

bool ir_lit_eq(ir_lit_t a, ir_lit_t b)
{
    if (!a.mag && !b.mag) {
        return true;
    }

    return a.mag == b.mag && a.neg == b.neg;
}

bool ir_instr_operand_int_lit(ir_instr_t *in, ir_lit_t v)
{
    ir_operand_t *o = push(in, IR_OPD_CONST);

    if (o) {
        o->lit.i = v;
    }

    return o != nullptr;
}

bool ir_instr_operand_int(ir_instr_t *in, ir_int_t v)
{
    return ir_instr_operand_int_lit(in, ir_lit_of(v));
}

bool ir_instr_operand_uint(ir_instr_t *in, ir_uint_t v)
{
    return ir_instr_operand_int_lit(in, (ir_lit_t){.mag = v});
}

bool ir_instr_operand_float(ir_instr_t *in, double v)
{
    ir_operand_t *o = push(in, IR_OPD_CONST);

    if (o) {
        o->lit.f        = v;
        o->lit.is_float = true;
    }

    return o != nullptr;
}

bool ir_instr_operand_float_text(ir_instr_t *in, const char *text)
{
    if (!text) {
        return false;
    }

    ir_operand_t *o = push(in, IR_OPD_CONST);

    if (o) {
        o->lit.f        = strtod(text, nullptr);
        o->lit.text     = ir_strdup(in->block->owner->owner->ctx, text);
        o->lit.is_float = true;
    }

    return o != nullptr;
}

bool ir_instr_operand_phi_arm(ir_instr_t *in, ir_block_t *b, ir_value_t *v)
{
    ir_operand_t *o = b && v ? push(in, IR_OPD_PHI_ARM) : nullptr;

    if (o) {
        o->phi.block = b;
        o->phi.value = v;
    }

    return o != nullptr;
}

bool ir_instr_operand_switch_arm_lit(ir_instr_t *in, ir_lit_t v, ir_block_t *b)
{
    ir_operand_t *o = b ? push(in, IR_OPD_SWITCH_ARM) : nullptr;

    if (o) {
        o->arm.value = v;
        o->arm.block = b;
    }

    return o != nullptr;
}

bool ir_instr_operand_switch_arm(ir_instr_t *in, ir_int_t v, ir_block_t *b)
{
    return ir_instr_operand_switch_arm_lit(in, ir_lit_of(v), b);
}

bool ir_instr_add_mod(ir_instr_t *in, const char *mod)
{
    if (!in || !mod) {
        return false;
    }

    ir_module_t *m = in->block->owner->owner;

    if (!ir_opinfo_accepts_mod(ir_module_op_info(m, in->op), mod)) {
        return false;
    }

    ir_ctx_t *ctx = m->ctx;
    size_t    cap = in->cap_mods;

    in->mods = ir_grow(ctx, in->mods, &cap, in->n_mods + 1u, sizeof(char *));

    if (!in->mods) {
        return false;
    }
    in->cap_mods           = (uint32_t)cap;
    in->mods[in->n_mods++] = ir_strdup(ctx, mod);

    return true;
}

bool ir_instr_has_mod(const ir_instr_t *in, const char *mod)
{
    if (!in || !mod) {
        return false;
    }

    for (uint32_t i = 0; i < in->n_mods; i++) {
        if (!strcmp(in->mods[i], mod)) {
            return true;
        }
    }

    return false;
}
