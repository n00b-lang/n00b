#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler_buffer_borrow.h"
#include "compiler_tu.h"
#include "cached_body.h"

#include "ir/doms.h"
#include "ir/pass.h"

static const core_string_t borrow_diagnostic_text = CORE_STRING_STATIC(
    "this use of a Buffer as a string outlives a safe borrow; consume it "
    "with to_string(b), or keep b with to_string(buffer_from_string(b))");

static const core_string_t write_lock_name =
    CORE_STRING_STATIC("builtin.containers.buffer.write_lock");

static const core_string_t buffer_state_type_name =
    CORE_STRING_STATIC("builtin.containers.buffer.Buffer_State");

static const core_string_t buffer_borrow_pass =
    CORE_STRING_STATIC("n00bb.buffer-borrow");

static ir_value_t nonlocal_root;

typedef struct {
    const void **keys;
    uint32_t *slots;
    size_t cap;
    size_t len;
} pointer_map_t;

static size_t
pointer_hash(const void *key)
{
    uintptr_t bits = (uintptr_t)key;
    bits ^= bits >> 17;
    bits *= 0xed5ad4bbU;
    bits ^= bits >> 11;
    return (size_t)bits;
}

static bool
pointer_map_grow(pointer_map_t *map)
{
    size_t cap = map->cap ? map->cap * 2 : 64;
    const void **keys = calloc(cap, sizeof(*keys));
    uint32_t *slots = calloc(cap, sizeof(*slots));
    if (!keys || !slots) {
        free(keys);
        free(slots);
        return false;
    }
    for (size_t i = 0; i < map->cap; i++) {
        if (!map->keys[i]) continue;
        size_t at = pointer_hash(map->keys[i]) & (cap - 1);
        while (keys[at]) at = (at + 1) & (cap - 1);
        keys[at] = map->keys[i];
        slots[at] = map->slots[i];
    }
    free(map->keys);
    free(map->slots);
    map->keys = keys;
    map->slots = slots;
    map->cap = cap;
    return true;
}

static bool
pointer_map_get(const pointer_map_t *map, const void *key, uint32_t *slot)
{
    if (!map->cap || !key) return false;
    size_t at = pointer_hash(key) & (map->cap - 1);
    while (map->keys[at]) {
        if (map->keys[at] == key) {
            *slot = map->slots[at];
            return true;
        }
        at = (at + 1) & (map->cap - 1);
    }
    return false;
}

static bool
pointer_map_put(pointer_map_t *map, const void *key, uint32_t slot)
{
    if ((map->len + 1) * 2 > map->cap && !pointer_map_grow(map)) return false;
    size_t at = pointer_hash(key) & (map->cap - 1);
    while (map->keys[at] && map->keys[at] != key)
        at = (at + 1) & (map->cap - 1);
    if (!map->keys[at]) map->len++;
    map->keys[at] = key;
    map->slots[at] = slot;
    return true;
}

static void
pointer_map_free(pointer_map_t *map)
{
    free(map->keys);
    free(map->slots);
    *map = (pointer_map_t){0};
}

typedef struct {
    ir_value_t **items;
    uint32_t len;
    uint32_t cap;
} root_set_t;

static bool
root_set_has(const root_set_t *set, const ir_value_t *root)
{
    for (uint32_t i = 0; i < set->len; i++)
        if (set->items[i] == root) return true;
    return false;
}

static bool
root_set_add(root_set_t *set, ir_value_t *root, bool *changed)
{
    if (!root || root_set_has(set, root)) return true;
    if (set->len == set->cap) {
        uint32_t cap = set->cap ? set->cap * 2 : 4;
        ir_value_t **items = realloc(set->items, cap * sizeof(*items));
        if (!items) return false;
        set->items = items;
        set->cap = cap;
    }
    set->items[set->len++] = root;
    if (changed) *changed = true;
    return true;
}

static bool
root_set_union(root_set_t *into, const root_set_t *from, bool *changed)
{
    for (uint32_t i = 0; i < from->len; i++)
        if (!root_set_add(into, from->items[i], changed)) return false;
    return true;
}

enum { REACH_SELF, REACH_CONTENTS, REACH_KINDS };

typedef struct {
    uint64_t from[REACH_KINDS];
    bool nonlocal;
    bool fresh;
    bool shared;
} carried_t;

typedef struct {
    ir_func_t *function;
    uint64_t writes[REACH_KINDS];
    uint64_t escapes[REACH_KINDS];
    uint64_t leaks[REACH_KINDS];
    uint64_t receives;
    bool receives_fresh;
    bool receives_nonlocal;
    carried_t *carried;
    uint32_t n_carried;
    bool writes_nonlocal;
    bool opaque;
} effects_t;

typedef struct {
    effects_t **items;
    size_t len;
    size_t cap;
    pointer_map_t index;
    pointer_map_t contents_index;
    ir_value_t **contents;
    ir_value_t **contents_base;
    bool *contents_hold_buffer;
    uint32_t contents_len;
    uint32_t contents_cap;
    th_ctx *types;
    bool failed;
} effects_table_t;

static th_type *
contents_type(th_type *type)
{
    th_type *resolved = type ? th_resolve(type) : nullptr;
    if (!resolved) return nullptr;
    if (th_kind_of(resolved) == TH_SEQUENCE) return th_seq_elem(resolved);
    if (th_kind_of(resolved) == TH_INDIRECTION)
        return th_ind_referent(resolved);
    return resolved;
}

static bool
is_buffer_type(th_type *type)
{
    core_string_t name = {};
    size_t args = 0;
    return th_kind_of(type) == TH_NAMED
        && th_named_info(type, &name, nullptr, &args) && !args
        && core_string_eq(name, core_string_borrow(n00bb_buffer_type_name,
               sizeof(n00bb_buffer_type_name) - 1));
}

static bool
may_be_buffer_state(th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    if (!type || th_kind_of(type) == TH_VAR) return true;
    core_string_t name = {};
    size_t args = 0;
    return th_kind_of(type) == TH_NAMED
        && th_named_info(type, &name, nullptr, &args) && !args
        && core_string_eq(name, buffer_state_type_name);
}


static bool type_may_hold_buffer(th_ctx *types, th_type *type,
                                 uint32_t depth);

static bool
arguments_may_hold_buffer(th_ctx *types, th_type *type, uint32_t depth)
{
    size_t args = 0;
    if (!th_named_info(type, nullptr, nullptr, &args)) return false;
    for (size_t i = 0; i < args; i++) {
        const th_arg *argument = th_named_arg(type, i);
        if (argument && argument->kind == TH_ARG_TYPE
            && type_may_hold_buffer(types, argument->type, depth + 1))
            return true;
    }
    return false;
}

static bool
type_may_hold_buffer(th_ctx *types, th_type *type, uint32_t depth)
{
    type = type ? th_resolve(type) : nullptr;
    if (!type || depth > 16 || th_kind_of(type) == TH_VAR) return true;
    if (!type_may_contain_reference_alias(types, type)) return false;
    switch (th_kind_of(type)) {
    case TH_NAMED: {
        if (is_buffer_type(type)) return true;
        if (arguments_may_hold_buffer(types, type, depth)) return true;
        th_type *definition = th_type_definition(types, type);
        return definition && th_resolve(definition) != type
            && type_may_hold_buffer(types, definition, depth + 1);
    }
    case TH_RECORD:
        for (size_t i = 0; i < th_record_len(type); i++) {
            const th_member *member = th_record_member(type, i);
            if (!member || type_may_hold_buffer(types, member->type,
                                                depth + 1))
                return true;
        }
        return false;
    case TH_SEQUENCE:
        return type_may_hold_buffer(types, th_seq_elem(type), depth + 1);
    case TH_INDIRECTION:
        return type_may_hold_buffer(types, th_ind_referent(type),
                                    depth + 1);
    default:
        if (!th_sum_len(type)) return true;
        for (size_t i = 0; i < th_sum_len(type); i++)
            if (type_may_hold_buffer(types, th_sum_arm(type, i), depth + 1))
                return true;
        return false;
    }
}

static bool
contents_may_hold_buffer(th_ctx *types, th_type *type, uint32_t depth)
{
    type = type ? th_resolve(type) : nullptr;
    if (!type || depth > 16 || th_kind_of(type) == TH_VAR) return true;
    switch (th_kind_of(type)) {
    case TH_SEQUENCE:
        return type_may_hold_buffer(types, th_seq_elem(type), depth + 1);
    case TH_INDIRECTION:
        return type_may_hold_buffer(types, th_ind_referent(type),
                                    depth + 1);
    case TH_NAMED: {
        if (is_buffer_type(type)) return false;
        if (arguments_may_hold_buffer(types, type, depth)) return true;
        th_type *definition = th_type_definition(types, type);
        return definition && th_resolve(definition) != type
            && contents_may_hold_buffer(types, definition, depth + 1);
    }
    case TH_RECORD:
        for (size_t i = 0; i < th_record_len(type); i++) {
            const th_member *member = th_record_member(type, i);
            if (!member || contents_may_hold_buffer(types, member->type,
                                                    depth + 1))
                return true;
        }
        return false;
    default:
        if (!type_may_contain_reference_alias(types, type)) return false;
        if (!th_sum_len(type)) return true;
        for (size_t i = 0; i < th_sum_len(type); i++)
            if (contents_may_hold_buffer(types, th_sum_arm(type, i),
                                         depth + 1))
                return true;
        return false;
    }
}

static ir_value_t *
contents_root(effects_table_t *table, ir_value_t *base)
{
    uint32_t slot = 0;
    if (pointer_map_get(&table->contents_index, base, &slot))
        return table->contents[slot];
    if (table->contents_len == table->contents_cap) {
        uint32_t cap = table->contents_cap ? table->contents_cap * 2 : 32;
        ir_value_t **contents = realloc(table->contents,
                                        cap * sizeof(*contents));
        if (contents) table->contents = contents;
        bool *holds = contents ? realloc(table->contents_hold_buffer,
                                         cap * sizeof(*holds)) : nullptr;
        if (holds) table->contents_hold_buffer = holds;
        ir_value_t **bases = holds ? realloc(table->contents_base,
                                             cap * sizeof(*bases)) : nullptr;
        if (!bases) return nullptr;
        table->contents_base = bases;
        table->contents_cap = cap;
    }
    ir_value_t *root = calloc(1, sizeof(*root));
    if (!root) return nullptr;
    *root = (ir_value_t){
        .kind = IR_VAL_LOCAL,
        .name = CORE_STRING_STATIC("<contents>"),
        .type = base->type,
        .owner = base->owner,
    };
    uint32_t at = table->contents_len;
    if (!pointer_map_put(&table->contents_index, root, at)) {
        free(root);
        return nullptr;
    }
    if (!pointer_map_put(&table->contents_index, base, at)) {
        free(root);
        return nullptr;
    }
    table->contents[at] = root;
    table->contents_base[at] = base;
    table->contents_hold_buffer[at] = contents_may_hold_buffer(table->types,
        base->type, 0);
    table->contents_len++;
    return root;
}

static bool
is_contents_root(const effects_table_t *table, const ir_value_t *root)
{
    uint32_t slot = 0;
    return root && root->kind != IR_VAL_PARAM
        && pointer_map_get(&table->contents_index, root, &slot)
        && table->contents[slot] == root;
}

static ir_value_t *
contents_base(const effects_table_t *table, const ir_value_t *root)
{
    uint32_t slot = 0;
    return is_contents_root(table, root)
        && pointer_map_get(&table->contents_index, root, &slot)
        ? table->contents_base[slot] : nullptr;
}

static void
effects_table_free(effects_table_t *table)
{
    for (uint32_t i = 0; i < table->contents_len; i++)
        free(table->contents[i]);
    free(table->contents);
    free(table->contents_base);
    free(table->contents_hold_buffer);
    for (size_t i = 0; i < table->len; i++) {
        free(table->items[i]->carried);
        free(table->items[i]);
    }
    free(table->items);
    pointer_map_free(&table->index);
    pointer_map_free(&table->contents_index);
}

static bool
function_is_native(const ir_func_t *function)
{
    return ir_annot_get((ir_annots_t *)&function->annots,
                        CORE_STRING_STATIC("n00bb.abi.native")) != nullptr;
}

static bool
native_calls_n00b(const ir_func_t *function)
{
    return ir_annot_get((ir_annots_t *)&function->annots,
                        CORE_STRING_STATIC("n00bb.native.calls-n00b"))
        != nullptr;
}

static bool
function_is_write_lock(const ir_func_t *function)
{
    core_string_t module = function->owner ? ir_module_name(function->owner)
                                           : (core_string_t){};
    if (!module.data || !function->name.data) return false;
    size_t module_len = module.u8_bytes;
    return write_lock_name.u8_bytes > module_len
        && core_string_starts_with(write_lock_name, module)
        && write_lock_name.data[module_len] == '.'
        && core_string_eq(core_string_slice(write_lock_name, module_len + 1,
               write_lock_name.u8_bytes - module_len - 1), function->name);
}

static th_type *
function_signature(const ir_func_t *function)
{
    th_type *signature = function && function->type
        ? th_resolve(function->type) : nullptr;
    return signature && th_kind_of(signature) == TH_FUNCTION ? signature
                                                             : nullptr;
}

static uint32_t
carried_count(const ir_func_t *function)
{
    th_type *signature = function_signature(function);
    return 1 + (signature ? (uint32_t)ir_call_output_count(signature) : 0);
}

static effects_t *
effects_for(effects_table_t *table, ir_func_t *function)
{
    uint32_t slot = 0;
    if (pointer_map_get(&table->index, function, &slot))
        return table->items[slot];
    if (table->len == table->cap) {
        size_t cap = table->cap ? table->cap * 2 : 64;
        effects_t **items = realloc(table->items, cap * sizeof(*items));
        if (!items) {
            table->failed = true;
            return nullptr;
        }
        table->items = items;
        table->cap = cap;
    }
    effects_t *effects = calloc(1, sizeof(*effects));
    uint32_t n_carried = carried_count(function);
    carried_t *carried = effects ? calloc(n_carried, sizeof(*carried))
                                 : nullptr;
    if (!carried) {
        free(effects);
        table->failed = true;
        return nullptr;
    }
    table->items[table->len] = effects;
    *effects = (effects_t){
        .function = function, .carried = carried, .n_carried = n_carried,
    };
    bool opaque = !function->n_blocks
        && (!function_is_native(function) || native_calls_n00b(function));
    if (opaque) {
        for (int kind = 0; kind < REACH_KINDS; kind++)
            effects->writes[kind] = effects->escapes[kind]
                = effects->leaks[kind] = UINT64_MAX;
        effects->receives = UINT64_MAX;
        effects->receives_fresh = effects->receives_nonlocal = true;
        effects->writes_nonlocal = true;
        effects->opaque = true;
    }
    for (uint32_t slot = 0; !function->n_blocks && slot < n_carried;
         slot++)
        carried[slot] = (carried_t){
            .from = {UINT64_MAX, UINT64_MAX},
            .nonlocal = opaque,
            .fresh = !opaque,
        };
    if (function_is_write_lock(function)) effects->writes[REACH_SELF] = 1;
    if (!pointer_map_put(&table->index, function, (uint32_t)table->len)) {
        free(carried);
        free(effects);
        table->failed = true;
        return nullptr;
    }
    table->len++;
    return effects;
}

static ir_value_t *
operand_at(const ir_instr_t *instruction, uint32_t index)
{
    const ir_operand_t *operand = &instruction->operands[index];
    if (operand->kind == IR_OPD_ARG) return operand->value;
    return ir_operand_value(operand);
}

static ir_func_t *
direct_callee(const ir_instr_t *instruction)
{
    if (instruction->op != IR_OP_CALL || !instruction->n_operands)
        return nullptr;
    ir_value_t *target = operand_at(instruction, 0);
    return target && target->kind == IR_VAL_FUNC
        ? n00bb_cached_body(target->owner) : nullptr;
}

static bool
is_call(const ir_instr_t *instruction)
{
    return instruction->op == IR_OP_CALL
        || instruction->op == IR_OP_CALLBACK_INVOKE;
}

typedef struct {
    ir_func_t *function;
    th_ctx *types;
    effects_table_t *effects;
    pointer_map_t slots;
    root_set_t *origins;
    root_set_t *memory;
    size_t len;
    size_t cap;
    root_set_t escaped;
    root_set_t leaked;
    root_set_t received;
    uint64_t receives;
    bool failed;
} origins_t;

static root_set_t *
origin_slot(origins_t *state, const ir_value_t *value, bool memory)
{
    if (!value) return nullptr;
    uint32_t slot = 0;
    if (!pointer_map_get(&state->slots, value, &slot)) {
        if (state->len == state->cap) {
            size_t cap = state->cap ? state->cap * 2 : 64;
            root_set_t *origins = realloc(state->origins,
                                          cap * sizeof(*origins));
            if (origins) state->origins = origins;
            root_set_t *memories = origins
                ? realloc(state->memory, cap * sizeof(*memories)) : nullptr;
            if (!origins || !memories) {
                state->failed = true;
                return nullptr;
            }
            state->memory = memories;
            state->cap = cap;
        }
        slot = (uint32_t)state->len++;
        state->origins[slot] = (root_set_t){0};
        state->memory[slot] = (root_set_t){0};
        if (!pointer_map_put(&state->slots, value, slot)) {
            state->failed = true;
            return nullptr;
        }
    }
    return memory ? &state->memory[slot] : &state->origins[slot];
}

static bool
is_allocation(const ir_value_t *root)
{
    return root && root->kind == IR_VAL_INSTR && root->def
        && root->def->op == IR_OP_ALLOC;
}

static bool
value_carries_references(origins_t *state, const ir_value_t *value)
{
    return value && value->type
        && type_may_contain_reference_alias(state->types, value->type);
}

static bool
seed_value(origins_t *state, ir_value_t *value, root_set_t *into,
           bool *changed)
{
    if (!value) return true;
    switch (value->kind) {
    case IR_VAL_PARAM:
        return root_set_add(into, value, changed);
    case IR_VAL_GLOBAL:
        return !value_carries_references(state, value)
            || root_set_add(into, &nonlocal_root, changed);
    case IR_VAL_FUNC:
        return true;
    default: {
        root_set_t *origin = origin_slot(state, value, false);
        return origin && root_set_union(into, origin, changed);
    }
    }
}

static bool
is_location(const ir_value_t *root)
{
    return is_allocation(root)
        || (root && root->kind == IR_VAL_LOCAL && root->local);
}

static root_set_t *
location_memory(origins_t *state, const ir_value_t *root)
{
    return origin_slot(state, root, is_allocation(root));
}

static const root_set_t *
held_memory(const origins_t *state, const ir_value_t *root)
{
    uint32_t slot = 0;
    return root && root != &nonlocal_root && !is_location(root)
        && pointer_map_get(&state->slots, root, &slot)
        ? &state->memory[slot] : nullptr;
}

static bool
expand_contents(origins_t *state, root_set_t *roots)
{
    bool changed = true;
    while (changed) {
        changed = false;
        uint32_t len = roots->len;
        for (uint32_t i = 0; i < len; i++) {
            const root_set_t *memory = is_location(roots->items[i])
                ? location_memory(state, roots->items[i])
                : held_memory(state, roots->items[i]);
            if (is_location(roots->items[i]) && !memory) return false;
            if (memory && !root_set_union(roots, memory, &changed))
                return false;
        }
    }
    return true;
}

static bool
load_roots(origins_t *state, const root_set_t *address, root_set_t *into)
{
    for (uint32_t i = 0; i < address->len; i++) {
        ir_value_t *root = address->items[i];
        if (is_location(root)) {
            root_set_t *memory = location_memory(state, root);
            if (!memory || !root_set_union(into, memory, nullptr))
                return false;
        }
        else if (root != &nonlocal_root
                 && !is_contents_root(state->effects, root)) {
            ir_value_t *contents = contents_root(state->effects, root);
            if (!contents || !root_set_add(into, contents, nullptr)
                || (root->kind != IR_VAL_PARAM
                    && !root_set_add(into, root, nullptr)))
                return false;
        }
        else if (!root_set_add(into, root, nullptr)) return false;
        const root_set_t *held = held_memory(state, root);
        if (held && !root_set_union(into, held, nullptr)) return false;
    }
    return true;
}

static bool
reached_roots(origins_t *state, const root_set_t *roots, root_set_t *into)
{
    if (!load_roots(state, roots, into)) return false;
    for (uint32_t i = 0; i < roots->len; i++)
        if (!is_location(roots->items[i])
            && !root_set_add(into, roots->items[i], nullptr))
            return false;
    return true;
}

static bool
contents_carry_references(origins_t *state, const ir_value_t *argument)
{
    th_type *contents = argument ? contents_type(argument->type) : nullptr;
    return !contents
        || th_kind_of(th_resolve(contents)) == TH_VAR
        || type_may_contain_reference_alias(state->types, contents);
}

static bool
effect_bit(const uint64_t *mask, uint32_t index, int kind)
{
    return index >= 64 || (mask[kind] >> index & 1);
}

static uint64_t
parameter_bit(const ir_value_t *root)
{
    th_type *signature = function_signature(root->owner);
    if (!signature || root->is_keyword || root->is_rest) return UINT64_MAX;
    uint32_t inputs = 0;
    for (size_t i = 0; i < th_func_nparams(signature) && i < 64; i++) {
        const th_param *formal = th_func_param(signature, i);
        if (!formal || !th_mode_accepts_input(formal->mode)) continue;
        if (inputs++ == root->index) return UINT64_C(1) << i;
    }
    return UINT64_MAX;
}

static const ir_value_t *
reached_parameter(const effects_table_t *table, const ir_value_t *root,
                  int *kind)
{
    if (root == &nonlocal_root) return nullptr;
    if (root->kind == IR_VAL_PARAM) {
        *kind = REACH_SELF;
        return root;
    }
    const ir_value_t *base = contents_base(table, root);
    if (base && base->kind == IR_VAL_PARAM) {
        *kind = REACH_CONTENTS;
        return base;
    }
    return nullptr;
}

static bool
record_escape(origins_t *state, const root_set_t *roots, bool leak,
              bool *changed)
{
    root_set_t expanded = {0};
    bool ok = root_set_union(&expanded, roots, nullptr)
        && expand_contents(state, &expanded)
        && root_set_union(&state->escaped, &expanded, changed)
        && (!leak || root_set_union(&state->leaked, &expanded, changed));
    free(expanded.items);
    return ok;
}

static bool
record_received(origins_t *state, const root_set_t *roots, bool *changed)
{
    root_set_t expanded = {0};
    bool ok = root_set_union(&expanded, roots, nullptr)
        && expand_contents(state, &expanded)
        && root_set_union(&state->received, &expanded, changed);
    free(expanded.items);
    return ok;
}

static bool
escape_into(origins_t *state, ir_value_t *target, const root_set_t *stored,
            bool *changed)
{
    ir_value_t *contents = target == &nonlocal_root
        || is_contents_root(state->effects, target)
        ? target : contents_root(state->effects, target);
    if (!contents) return false;
    root_set_t leaving = {0};
    bool ok = true;
    for (uint32_t i = 0; ok && i < stored->len; i++)
        if (stored->items[i] != target && stored->items[i] != contents)
            ok = root_set_add(&leaving, stored->items[i], nullptr);
    int kind = 0;
    const ir_value_t *parameter = reached_parameter(state->effects, target,
                                                    &kind);
    if (parameter && leaving.len) {
        state->receives |= parameter_bit(parameter);
        ok = ok && record_received(state, &leaving, changed);
    }
    if (parameter || target == &nonlocal_root)
        ok = ok && record_escape(state, &leaving, target == &nonlocal_root,
                                 changed);
    else {
        root_set_t *held = origin_slot(state, target, true);
        ok = ok && held && root_set_union(held, &leaving, changed);
    }
    free(leaving.items);
    return ok;
}

static bool
receive_into(origins_t *state, const root_set_t *targets,
             const root_set_t *incoming, bool *changed)
{
    bool ok = true;
    int kind = 0;
    for (uint32_t i = 0; ok && incoming->len && i < targets->len; i++) {
        ir_value_t *target = targets->items[i];
        const ir_value_t *parameter = reached_parameter(state->effects,
                                                        target, &kind);
        if (target == &nonlocal_root)
            ok = record_escape(state, incoming, true, changed);
        else if (is_location(target)) {
            root_set_t *memory = location_memory(state, target);
            ok = memory && root_set_union(memory, incoming, changed);
        }
        else if (parameter) {
            state->receives |= parameter_bit(parameter);
            ok = record_received(state, incoming, changed);
        }
        else {
            root_set_t *held = origin_slot(state, target, true);
            ok = held && root_set_union(held, incoming, changed);
        }
    }
    return ok;
}

static bool
call_effects(origins_t *state, const ir_instr_t *call, effects_t **effects)
{
    ir_func_t *callee = direct_callee(call);
    *effects = callee ? effects_for(state->effects, callee) : nullptr;
    return !callee || *effects;
}

static bool
call_carried(origins_t *state, const ir_instr_t *call, uint32_t slot,
             ir_value_t *fresh, root_set_t *into)
{
    effects_t *effects = nullptr;
    if (!call_effects(state, call, &effects)) return false;
    const carried_t *carried = effects && !effects->opaque
        && slot < effects->n_carried ? &effects->carried[slot] : nullptr;
    uint32_t first = call->op == IR_OP_CALL ? 1 : 0;
    bool ok = true;
    for (uint32_t i = first; ok && i < call->n_operands; i++) {
        ir_value_t *argument = operand_at(call, i);
        if (!argument) continue;
        root_set_t roots = {0};
        root_set_t contents = {0};
        ok = seed_value(state, argument, &roots, nullptr)
            && reached_roots(state, &roots, &contents);
        uint32_t index = i - first;
        if (ok && (!carried
                   || effect_bit(carried->from, index, REACH_SELF)))
            ok = root_set_union(into, &roots, nullptr);
        if (ok && contents_carry_references(state, argument)
            && (!carried
                || effect_bit(carried->from, index, REACH_CONTENTS)))
            ok = root_set_union(into, &contents, nullptr);
        free(roots.items);
        free(contents.items);
    }
    if (ok && (!carried || carried->nonlocal))
        ok = root_set_add(into, &nonlocal_root, nullptr);
    ir_value_t *identity = carried && carried->shared && call->dest
        ? call->dest : fresh;
    if (ok && identity && carried && carried->fresh)
        ok = root_set_add(into, identity, nullptr);
    return ok;
}

static ir_instr_t *
output_call(const ir_instr_t *output)
{
    ir_value_t *source = output->n_operands ? operand_at(output, 0)
                                            : nullptr;
    ir_instr_t *call = source && source->kind == IR_VAL_INSTR ? source->def
                                                              : nullptr;
    if (call && call->op == IR_OP_VARIANT_PAYLOAD && call->n_operands) {
        source = operand_at(call, 0);
        call = source && source->kind == IR_VAL_INSTR ? source->def
                                                      : nullptr;
    }
    return call && is_call(call) ? call : nullptr;
}

static uint32_t
output_slot(const ir_instr_t *output)
{
    if (output->n_operands < 2 || output->operands[1].kind != IR_OPD_CONST
        || output->operands[1].lit.i.neg
        || output->operands[1].lit.i.mag >= UINT32_MAX)
        return UINT32_MAX;
    return 1 + (uint32_t)output->operands[1].lit.i.mag;
}

static bool
transfer_call(origins_t *state, ir_instr_t *instruction, root_set_t *result,
              bool *changed)
{
    effects_t *effects = nullptr;
    if (!call_effects(state, instruction, &effects)) return false;
    bool opaque = !effects || effects->opaque;
    uint32_t first = instruction->op == IR_OP_CALL ? 1 : 0;
    root_set_t kept = {0};
    bool ok = true;
    for (uint32_t i = first; ok && i < instruction->n_operands; i++) {
        ir_value_t *argument = operand_at(instruction, i);
        if (!argument) continue;
        root_set_t roots = {0};
        root_set_t contents = {0};
        ok = seed_value(state, argument, &roots, nullptr)
            && reached_roots(state, &roots, &contents);
        uint32_t index = i - first;
        bool carry = contents_carry_references(state, argument);
        for (int kind = 0; ok && kind < REACH_KINDS; kind++) {
            if (kind == REACH_CONTENTS && !carry) continue;
            root_set_t *reached = kind == REACH_SELF ? &roots : &contents;
            if (!opaque && !effect_bit(effects->escapes, index, kind))
                continue;
            bool leak = opaque || effect_bit(effects->leaks, index, kind);
            ok = record_escape(state, reached, leak, changed)
                && root_set_union(&kept, reached, nullptr);
        }
        free(roots.items);
        free(contents.items);
    }
    root_set_t incoming = {0};
    ok = ok && root_set_union(&incoming, &kept, nullptr)
        && (!(opaque || effects->receives_fresh) || !instruction->dest
            || root_set_add(&incoming, instruction->dest, nullptr))
        && (!(opaque || effects->receives_nonlocal)
            || root_set_add(&incoming, &nonlocal_root, nullptr));
    for (uint32_t i = first; ok && i < instruction->n_operands; i++) {
        root_set_t roots = {0};
        ok = seed_value(state, operand_at(instruction, i), &roots, nullptr);
        for (uint32_t r = 0; ok && r < roots.len; r++) {
            if (!is_location(roots.items[r])) continue;
            root_set_t *memory = location_memory(state, roots.items[r]);
            ok = memory && root_set_union(memory, &kept, changed)
                && root_set_add(memory, roots.items[r], changed);
        }
        if (ok && (opaque || effect_bit(&effects->receives, i - first, 0)))
            ok = receive_into(state, &roots, &incoming, changed);
        free(roots.items);
    }
    free(incoming.items);
    free(kept.items);
    return ok && call_carried(state, instruction, 0, instruction->dest,
                              result);
}

static bool
transfer(origins_t *state, ir_instr_t *instruction, bool *changed)
{
    root_set_t gathered = {0};
    bool ok = true;
    switch (instruction->op) {
    case IR_OP_ALLOC:
        ok = instruction->dest
            && root_set_add(&gathered, instruction->dest, nullptr);
        break;
    case IR_OP_LOAD:
    case IR_OP_SEQUENCE_GET: {
        root_set_t address = {0};
        ok = instruction->n_operands >= 1
            && seed_value(state, operand_at(instruction, 0), &address,
                          nullptr)
            && load_roots(state, &address, &gathered);
        free(address.items);
        break;
    }
    case IR_OP_STORE:
    case IR_OP_SEQUENCE_SET:
    case IR_OP_ATOMIC_RMW:
    case IR_OP_ATOMIC_CAS:
    case IR_OP_MEMCPY:
    case IR_OP_MEMMOVE: {
        bool copies = instruction->op == IR_OP_MEMCPY
            || instruction->op == IR_OP_MEMMOVE;
        uint32_t value_at = copies || instruction->op == IR_OP_STORE ? 1 : 2;
        if (instruction->n_operands <= value_at) break;
        root_set_t address = {0};
        root_set_t stored = {0};
        ok = seed_value(state, operand_at(instruction, 0), &address, nullptr)
            && seed_value(state, operand_at(instruction, value_at), &stored,
                          nullptr);
        if (ok && copies) {
            root_set_t contents = {0};
            ok = load_roots(state, &stored, &contents);
            free(stored.items);
            stored = contents;
        }
        for (uint32_t i = 0; ok && i < address.len; i++) {
            ir_value_t *root = address.items[i];
            if (is_location(root)) {
                root_set_t *memory = location_memory(state, root);
                ok = memory && root_set_union(memory, &stored, changed);
            }
            else ok = escape_into(state, root, &stored, changed);
        }
        free(address.items);
        free(stored.items);
        break;
    }
    case IR_OP_GLOBAL_STORE: {
        if (instruction->n_operands < 2) break;
        root_set_t stored = {0};
        ok = seed_value(state, operand_at(instruction, 1), &stored, nullptr)
            && record_escape(state, &stored, true, changed);
        free(stored.items);
        break;
    }
    case IR_OP_ADDRESS_OF: {
        ir_value_t *target = instruction->n_operands
            ? operand_at(instruction, 0) : nullptr;
        ok = is_location(target)
            ? root_set_add(&gathered, target, nullptr)
            : seed_value(state, target, &gathered, nullptr);
        break;
    }
    case IR_OP_RETURN:
    case IR_OP_FAIL:
        break;
    case IR_OP_CALL:
    case IR_OP_CALLBACK_INVOKE:
        ok = transfer_call(state, instruction, &gathered, changed);
        break;
    case IR_OP_CALL_OUTPUT: {
        ir_instr_t *call = output_call(instruction);
        ok = call ? call_carried(state, call, output_slot(instruction),
                                 instruction->dest, &gathered)
                  : instruction->n_operands
                && seed_value(state, operand_at(instruction, 0), &gathered,
                              nullptr);
        break;
    }
    default: {
        bool sourced = false;
        for (uint32_t i = 0; ok && i < instruction->n_operands; i++) {
            ir_value_t *operand = operand_at(instruction, i);
            if (operand && operand->kind != IR_VAL_FUNC) sourced = true;
            ok = seed_value(state, operand, &gathered, nullptr);
        }
        if (ok && !sourced && instruction->dest)
            ok = root_set_add(&gathered, instruction->dest, nullptr);
        break;
    }
    }
    ir_value_t *target = instruction->dest;
    if (!target && instruction->dest_local)
        target = ir_local_value(state->function, instruction->dest_local);
    if (ok && target && (instruction->op == IR_OP_ALLOC
                         || value_carries_references(state, target))) {
        root_set_t *origin = origin_slot(state, target, false);
        ok = origin && root_set_union(origin, &gathered, changed);
    }
    free(gathered.items);
    return ok;
}

static void
origins_free(origins_t *state)
{
    for (size_t i = 0; i < state->len; i++) {
        free(state->origins[i].items);
        free(state->memory[i].items);
    }
    free(state->origins);
    free(state->memory);
    free(state->escaped.items);
    free(state->leaked.items);
    free(state->received.items);
    pointer_map_free(&state->slots);
}

static bool
origins_solve(origins_t *state)
{
    bool changed = true;
    while (changed && !state->failed) {
        changed = false;
        for (uint32_t b = 0; b < state->function->n_blocks; b++) {
            ir_block_t *block = state->function->blocks[b];
            for (uint32_t i = 0; i < block->n_instrs; i++)
                if (!transfer(state, block->instrs[i], &changed)) {
                    state->failed = true;
                    return false;
                }
        }
    }
    return !state->failed;
}

static bool
call_write_roots(origins_t *state, ir_instr_t *instruction,
                 root_set_t *written, bool *writes_nonlocal)
{
    ir_func_t *callee = direct_callee(instruction);
    effects_t *effects = callee ? effects_for(state->effects, callee)
                                : nullptr;
    if (callee && !effects) return false;
    bool opaque = !effects || effects->opaque;
    if (opaque || effects->writes_nonlocal) *writes_nonlocal = true;
    uint32_t first = instruction->op == IR_OP_CALL ? 1 : 0;
    for (uint32_t i = first; i < instruction->n_operands; i++) {
        uint32_t index = i - first;
        bool self = opaque || effect_bit(effects->writes, index, REACH_SELF);
        bool contents = opaque
            || effect_bit(effects->writes, index, REACH_CONTENTS);
        if (!self && !contents) continue;
        root_set_t roots = {0};
        bool ok = seed_value(state, operand_at(instruction, i), &roots,
                             nullptr)
            && (!self || root_set_union(written, &roots, nullptr))
            && (!contents || reached_roots(state, &roots, written));
        free(roots.items);
        if (!ok) return false;
    }
    return expand_contents(state, written);
}

static bool
writes_memory(const ir_instr_t *instruction)
{
    switch (instruction->op) {
    case IR_OP_STORE:
        return !ir_instr_has_mod(instruction, CORE_STRING_STATIC("atomic"));
    case IR_OP_SEQUENCE_SET:
    case IR_OP_MEMCPY:
    case IR_OP_MEMMOVE:
    case IR_OP_MEMSET:
        return true;
    default:
        return false;
    }
}

static bool
inside_buffer_state(const ir_value_t *address)
{
    for (uint32_t depth = 0; address && depth < 64; depth++) {
        th_type *type = address->type ? th_resolve(address->type) : nullptr;
        if (!type || (th_kind_of(type) == TH_INDIRECTION
                      && may_be_buffer_state(th_ind_referent(type))))
            return true;
        const ir_instr_t *step = address->kind == IR_VAL_INSTR
            ? address->def : nullptr;
        if (!step || !step->n_operands
            || (step->op != IR_OP_FIELD && step->op != IR_OP_INDEX
                && step->op != IR_OP_OFFSET))
            return false;
        address = operand_at(step, 0);
    }
    return true;
}

static bool
instruction_write_roots(origins_t *state, ir_instr_t *instruction,
                        root_set_t *written, bool *writes_nonlocal)
{
    if (is_call(instruction))
        return call_write_roots(state, instruction, written,
                                writes_nonlocal);
    if (!writes_memory(instruction) || !instruction->n_operands
        || !inside_buffer_state(operand_at(instruction, 0)))
        return true;
    root_set_t address = {0};
    bool ok = seed_value(state, operand_at(instruction, 0), &address,
                         nullptr);
    for (uint32_t r = 0; ok && r < address.len; r++) {
        ir_value_t *root = address.items[r];
        if (is_location(root)) continue;
        if (root == &nonlocal_root) *writes_nonlocal = true;
        ok = root_set_add(written, root, nullptr);
    }
    free(address.items);
    return ok;
}

static bool
summarize_carried(origins_t *state, const ir_instr_t *exit,
                  carried_t *carried, root_set_t *fresh, uint32_t n_carried)
{
    uint32_t outputs = exit->op == IR_OP_RETURN ? n_carried - 1 : 0;
    uint32_t result_at = exit->n_operands > outputs
        ? exit->n_operands - outputs : 0;
    bool ok = true;
    for (uint32_t o = 0; ok && o < exit->n_operands; o++) {
        uint32_t slot = o < result_at ? 0 : 1 + o - result_at;
        if (slot >= n_carried) slot = 0;
        root_set_t roots = {0};
        ok = seed_value(state, operand_at(exit, o), &roots, nullptr)
            && expand_contents(state, &roots);
        int kind = 0;
        for (uint32_t r = 0; ok && r < roots.len; r++) {
            ir_value_t *root = roots.items[r];
            const ir_value_t *parameter = reached_parameter(state->effects,
                                                            root, &kind);
            if (root == &nonlocal_root) carried[slot].nonlocal = true;
            else if (parameter)
                carried[slot].from[kind] |= parameter_bit(parameter);
            else {
                carried[slot].fresh = true;
                ok = root_set_add(&fresh[slot], root, nullptr);
                if (root_set_has(&state->escaped, root))
                    carried[slot].from[REACH_CONTENTS] |= state->receives;
                if (root_set_has(&state->leaked, root))
                    carried[slot].nonlocal = true;
                if (root_set_has(&state->received, root))
                    carried[slot].shared = true;
            }
        }
        free(roots.items);
    }
    return ok;
}

static void
mark_shared(carried_t *carried, const root_set_t *fresh, uint32_t n_carried)
{
    for (uint32_t s = 0; s < n_carried; s++)
        for (uint32_t t = 0; t < n_carried && !carried[s].shared; t++)
            for (uint32_t r = 0; t != s && r < fresh[s].len; r++)
                if (root_set_has(&fresh[t], fresh[s].items[r])) {
                    carried[s].shared = true;
                    break;
                }
}

static bool
summarize(effects_table_t *table, th_ctx *types, effects_t *effects,
          bool *changed)
{
    ir_func_t *function = effects->function;
    if (!function->n_blocks || effects->opaque) return true;
    origins_t state = {
        .function = function, .types = types, .effects = table,
    };
    bool ok = origins_solve(&state);
    uint64_t writes[REACH_KINDS] = {
        effects->writes[REACH_SELF], effects->writes[REACH_CONTENTS],
    };
    uint64_t escapes[REACH_KINDS] = {0};
    carried_t *carried = calloc(effects->n_carried, sizeof(*carried));
    root_set_t *fresh = calloc(effects->n_carried, sizeof(*fresh));
    ok = ok && carried && fresh;
    bool writes_nonlocal = false;
    int kind = 0;
    for (uint32_t b = 0; ok && b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; ok && i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            root_set_t roots = {0};
            bool nonlocal = false;
            ok = instruction_write_roots(&state, instruction, &roots,
                                         &nonlocal);
            if (nonlocal) writes_nonlocal = true;
            for (uint32_t r = 0; ok && r < roots.len; r++) {
                const ir_value_t *parameter
                    = reached_parameter(table, roots.items[r], &kind);
                if (roots.items[r] == &nonlocal_root) writes_nonlocal = true;
                else if (parameter) writes[kind] |= parameter_bit(parameter);
            }
            if (ok && (instruction->op == IR_OP_RETURN
                       || instruction->op == IR_OP_FAIL))
                ok = summarize_carried(&state, instruction, carried, fresh,
                                       effects->n_carried);
            free(roots.items);
        }
    }
    for (uint32_t r = 0; ok && r < state.escaped.len; r++) {
        const ir_value_t *parameter
            = reached_parameter(table, state.escaped.items[r], &kind);
        if (parameter) escapes[kind] |= parameter_bit(parameter);
    }
    uint64_t leaks[REACH_KINDS] = {0};
    for (uint32_t r = 0; ok && r < state.leaked.len; r++) {
        const ir_value_t *parameter
            = reached_parameter(table, state.leaked.items[r], &kind);
        if (parameter) leaks[kind] |= parameter_bit(parameter);
    }
    uint64_t receives = state.receives;
    bool receives_fresh = false;
    bool receives_nonlocal = false;
    for (uint32_t r = 0; ok && r < state.received.len; r++) {
        ir_value_t *root = state.received.items[r];
        if (root == &nonlocal_root
            || root_set_has(&state.leaked, root))
            receives_nonlocal = true;
        if (root != &nonlocal_root
            && !reached_parameter(table, root, &kind))
            receives_fresh = true;
    }
    if (ok) mark_shared(carried, fresh, effects->n_carried);
    for (uint32_t s = 0; fresh && s < effects->n_carried; s++)
        free(fresh[s].items);
    free(fresh);
    origins_free(&state);
    if (!ok) {
        free(carried);
        return false;
    }
    if (memcmp(writes, effects->writes, sizeof(writes))
        || memcmp(escapes, effects->escapes, sizeof(escapes))
        || memcmp(carried, effects->carried,
                  effects->n_carried * sizeof(*carried))
        || memcmp(leaks, effects->leaks, sizeof(leaks))
        || receives != effects->receives
        || receives_fresh != effects->receives_fresh
        || receives_nonlocal != effects->receives_nonlocal
        || writes_nonlocal != effects->writes_nonlocal) {
        memcpy(effects->writes, writes, sizeof(writes));
        memcpy(effects->escapes, escapes, sizeof(escapes));
        memcpy(effects->leaks, leaks, sizeof(leaks));
        effects->receives = receives;
        effects->receives_fresh = receives_fresh;
        effects->receives_nonlocal = receives_nonlocal;        free(effects->carried);
        effects->carried = carried;
        effects->writes_nonlocal = writes_nonlocal;
        *changed = true;
    }
    else free(carried);
    return true;
}

static bool
collect_reachable(effects_table_t *table, ir_func_t *root)
{
    size_t start = table->len;
    if (!effects_for(table, root)) return false;
    for (size_t at = start; at < table->len; at++) {
        ir_func_t *function = table->items[at]->function;
        for (uint32_t b = 0; b < function->n_blocks; b++) {
            ir_block_t *block = function->blocks[b];
            for (uint32_t i = 0; i < block->n_instrs; i++) {
                ir_func_t *callee = direct_callee(block->instrs[i]);
                if (callee && !effects_for(table, callee)) return false;
            }
        }
    }
    return !table->failed;
}

static bool
solve_effects(effects_table_t *table, th_ctx *types)
{
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < table->len; i++)
            if (!summarize(table, types, table->items[i], &changed))
                return false;
    }
    return true;
}

typedef struct {
    ir_value_t **items;
    uint32_t len;
    uint32_t cap;
} value_list_t;

typedef struct {
    ir_diags_t *diagnostics;
    ir_func_t *function;
    ir_instr_t *conversion;
    ir_value_t *buffer;
    origins_t *origins;
    value_list_t tracked;
    pointer_map_t tracked_index;
    uint64_t *live_in;
    uint64_t *live_out;
    uint32_t words;
} window_t;

static void
report_borrow(window_t *window, const ir_instr_t *offender,
              core_string_t label)
{
    ir_diag_t *diagnostic = ir_diag_begin_at(window->diagnostics,
        label.data ? IR_DIAG_VERIFY_LIFETIME : IR_DIAG_INTERNAL,
        window->conversion, window->conversion->block, window->function,
        CORE_STRING_STATIC("this Buffer is borrowed as a string here"));
    core_buffer_t message = {0};
    if (!label.data)
        core_buffer_puts(&message,
                         "the Buffer borrow analysis could not complete for this "
                         "conversion");
    else if (offender && offender->span.start_line)
        core_buffer_printf(&message, "%.*s (%.*s, at line %u, column %u)",
                           (int)borrow_diagnostic_text.u8_bytes,
                           borrow_diagnostic_text.data,
                           (int)label.u8_bytes, label.data,
                           offender->span.start_line,
                           offender->span.start_col);
    else
        core_buffer_printf(&message, "%.*s (%.*s)",
                           (int)borrow_diagnostic_text.u8_bytes,
                           borrow_diagnostic_text.data,
                           (int)label.u8_bytes, label.data);
    core_string_t text = core_buffer_string(&message);
    if (text.u8_bytes > 511) text.u8_bytes = 511;
    ir_diag_message(diagnostic, window->diagnostics, text);
    core_buffer_release(&message);
    if (offender && label.data)
        ir_diag_secondary(window->diagnostics, diagnostic, offender->span,
                          label);
}

static void
bitset_set(uint64_t *bits, uint32_t index)
{
    bits[index / 64] |= UINT64_C(1) << (index % 64);
}

static void
bitset_clear(uint64_t *bits, uint32_t index)
{
    bits[index / 64] &= ~(UINT64_C(1) << (index % 64));
}

static bool
bitset_empty(const uint64_t *bits, uint32_t words)
{
    for (uint32_t i = 0; i < words; i++)
        if (bits[i]) return false;
    return true;
}

static bool
tracked_index(const window_t *window, const ir_value_t *value,
              uint32_t *index)
{
    return value && pointer_map_get(&window->tracked_index, value, index);
}

static bool
track(window_t *window, ir_value_t *value)
{
    uint32_t index = 0;
    if (tracked_index(window, value, &index)) return true;
    if (window->tracked.len == window->tracked.cap) {
        uint32_t cap = window->tracked.cap ? window->tracked.cap * 2 : 8;
        ir_value_t **items = realloc(window->tracked.items,
                                     cap * sizeof(*items));
        if (!items) return false;
        window->tracked.items = items;
        window->tracked.cap = cap;
    }
    if (!pointer_map_put(&window->tracked_index, value,
                         window->tracked.len)) return false;
    window->tracked.items[window->tracked.len++] = value;
    return true;
}

static bool
collect_tracked(window_t *window)
{
    ir_value_t *view = window->conversion->dest;
    origins_t *origins = window->origins;
    size_t count = origins->slots.len;
    ir_value_t **values = calloc(count ? count : 1, sizeof(*values));
    if (!values) return false;
    size_t len = 0;
    for (size_t i = 0; i < origins->slots.cap && len < count; i++)
        if (origins->slots.keys[i]
            && ((const ir_value_t *)origins->slots.keys[i])->kind
                != IR_VAL_PARAM)
            values[len++] = (ir_value_t *)origins->slots.keys[i];
    bool ok = true;
    for (size_t i = 0; ok && i < len; i++) {
        root_set_t reached = {0};
        ok = seed_value(origins, values[i], &reached, nullptr)
            && expand_contents(origins, &reached)
            && (!root_set_has(&reached, view) || track(window, values[i]));
        free(reached.items);
    }
    free(values);
    if (!ok || !track(window, view)) return false;
    for (uint32_t b = 0; b < window->function->n_blocks; b++) {
        ir_block_t *block = window->function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *output = block->instrs[i];
            uint32_t index = 0;
            if (output->op != IR_OP_CALL_OUTPUT
                || !tracked_index(window, output->dest, &index))
                continue;
            ir_value_t *source = operand_at(output, 0);
            if (!source || !track(window, source)) return false;
            if (source->kind == IR_VAL_INSTR && source->def
                && source->def->op == IR_OP_VARIANT_PAYLOAD
                && source->def->n_operands
                && !track(window, operand_at(source->def, 0)))
                return false;
        }
    }
    return true;
}

static void
uses_and_defs(window_t *window, const ir_instr_t *instruction,
              uint64_t *live)
{
    uint32_t index = 0;
    if (instruction->dest
        && tracked_index(window, instruction->dest, &index))
        bitset_clear(live, index);
    if (instruction->op == IR_OP_COPY && instruction->dest_local
        && tracked_index(window, ir_local_value(window->function,
                                                instruction->dest_local),
                         &index))
        bitset_clear(live, index);
    if (instruction->op == IR_OP_PHI) return;
    for (uint32_t o = 0; o < instruction->n_operands; o++)
        if (tracked_index(window, operand_at(instruction, o), &index))
            bitset_set(live, index);
}

static void
phi_edge_uses(window_t *window, const ir_block_t *from, const ir_block_t *to,
              uint64_t *live)
{
    for (uint32_t i = 0; i < to->n_instrs && to->instrs[i]->op == IR_OP_PHI;
         i++) {
        const ir_instr_t *phi = to->instrs[i];
        for (uint32_t o = 0; o < phi->n_operands; o++) {
            const ir_operand_t *operand = &phi->operands[o];
            uint32_t index = 0;
            if (operand->kind == IR_OPD_PHI_ARM && operand->phi.block == from
                && tracked_index(window, operand->phi.value, &index))
                bitset_set(live, index);
        }
    }
}

static bool
edge_live(window_t *window, const ir_block_t *from, const ir_block_t *to,
          bool *live_edge)
{
    uint64_t *live = calloc(window->words, sizeof(*live));
    if (!live) return false;
    memcpy(live, &window->live_in[(size_t)to->index * window->words],
           window->words * sizeof(*live));
    phi_edge_uses(window, from, to, live);
    *live_edge = !bitset_empty(live, window->words);
    free(live);
    return true;
}

static bool
solve_liveness(window_t *window)
{
    ir_func_t *function = window->function;
    window->words = (window->tracked.len + 63) / 64;
    size_t total = (size_t)function->n_blocks * window->words;
    window->live_in = calloc(total ? total : 1, sizeof(uint64_t));
    window->live_out = calloc(total ? total : 1, sizeof(uint64_t));
    uint64_t *scratch = calloc(window->words, sizeof(uint64_t));
    if (!window->live_in || !window->live_out || !scratch) {
        free(scratch);
        return false;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t b = function->n_blocks; b-- > 0;) {
            ir_block_t *block = function->blocks[b];
            memset(scratch, 0, window->words * sizeof(*scratch));
            ir_instr_t *terminator = ir_block_terminator(block);
            for (uint32_t s = 0; terminator && s < ir_succ_count(terminator);
                 s++) {
                ir_block_t *successor = ir_succ_at(terminator, s);
                for (uint32_t w = 0; w < window->words; w++)
                    scratch[w] |= window->live_in[
                        (size_t)successor->index * window->words + w];
                phi_edge_uses(window, block, successor, scratch);
            }
            memcpy(&window->live_out[(size_t)b * window->words], scratch,
                   window->words * sizeof(*scratch));
            for (uint32_t i = block->n_instrs; i-- > 0;)
                uses_and_defs(window, block->instrs[i], scratch);
            uint64_t *in = &window->live_in[(size_t)b * window->words];
            if (memcmp(in, scratch, window->words * sizeof(*scratch))) {
                memcpy(in, scratch, window->words * sizeof(*scratch));
                changed = true;
            }
        }
    }
    free(scratch);
    return true;
}

static void
live_after_each(window_t *window, const ir_block_t *block, uint64_t *after,
                uint64_t *live)
{
    memcpy(live, &window->live_out[(size_t)block->index * window->words],
           window->words * sizeof(*live));
    for (uint32_t i = block->n_instrs; i-- > 0;) {
        memcpy(&after[(size_t)i * window->words], live,
               window->words * sizeof(*live));
        uses_and_defs(window, block->instrs[i], live);
    }
}

static uint32_t
instruction_position(const ir_instr_t *instruction)
{
    ir_block_t *block = instruction->block;
    if (instruction->index < block->n_instrs
        && block->instrs[instruction->index] == instruction)
        return instruction->index;
    for (uint32_t i = 0; i < block->n_instrs; i++)
        if (block->instrs[i] == instruction) return i;
    return block->n_instrs;
}

static uint32_t
first_non_phi(const ir_block_t *block)
{
    uint32_t at = 0;
    while (at < block->n_instrs && block->instrs[at]->op == IR_OP_PHI) at++;
    return at;
}

static uint32_t
predecessor_count(ir_func_t *function, const ir_block_t *target)
{
    uint32_t count = 0;
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_instr_t *terminator = ir_block_terminator(function->blocks[b]);
        for (uint32_t s = 0; terminator && s < ir_succ_count(terminator);
             s++)
            if (ir_succ_at(terminator, s) == target) count++;
    }
    return count;
}

typedef struct {
    ir_block_t *block;
    ir_instr_t *after;
    ir_block_t *from;
    ir_block_t *to;
} end_site_t;

typedef struct {
    end_site_t *items;
    uint32_t len;
    uint32_t cap;
} end_sites_t;

static bool
add_end_site(end_sites_t *sites, end_site_t site)
{
    if (sites->len == sites->cap) {
        uint32_t cap = sites->cap ? sites->cap * 2 : 4;
        end_site_t *items = realloc(sites->items, cap * sizeof(*items));
        if (!items) return false;
        sites->items = items;
        sites->cap = cap;
    }
    sites->items[sites->len++] = site;
    return true;
}

static ir_block_t *
split_edge(ir_func_t *function, ir_block_t *from, ir_block_t *to,
           ir_span_t span)
{
    char name[64];
    int length = snprintf(name, sizeof(name), "__n00bb_borrow_edge_%u_%u",
                          from->index, to->index);
    if (length < 0 || (size_t)length >= sizeof(name)) return nullptr;
    ir_block_t *middle = ir_block_add(function,
        core_string_borrow(name, (size_t)length));
    if (!middle) return nullptr;
    ir_instr_t *jump = ir_instr_generated(middle, IR_OP_JUMP, span,
        buffer_borrow_pass, 0);
    if (!jump || !ir_instr_operand_block(jump, to)) return nullptr;
    ir_instr_t *terminator = ir_block_terminator(from);
    for (uint32_t o = 0; terminator && o < terminator->n_operands; o++) {
        ir_operand_t *operand = &terminator->operands[o];
        if ((operand->kind == IR_OPD_BLOCK
             || operand->kind == IR_OPD_DEFAULT_BLOCK)
            && operand->block == to)
            operand->block = middle;
        else if (operand->kind == IR_OPD_SWITCH_ARM
                 && operand->arm.block == to)
            operand->arm.block = middle;
    }
    for (uint32_t i = 0; i < to->n_instrs && to->instrs[i]->op == IR_OP_PHI;
         i++) {
        ir_instr_t *phi = to->instrs[i];
        for (uint32_t o = 0; o < phi->n_operands; o++)
            if (phi->operands[o].kind == IR_OPD_PHI_ARM
                && phi->operands[o].phi.block == from)
                phi->operands[o].phi.block = middle;
    }
    return middle;
}

typedef struct {
    ir_local_t *local;
    th_type *type;
    uint32_t ordinal;
    uint32_t serial;
} held_t;

static bool
insert_held_assignment(window_t *window, held_t *held, ir_block_t *block,
                       uint32_t at, ir_value_t *buffer)
{
    char name[64];
    int length = snprintf(name, sizeof(name), "__n00bb_borrow_held_%u_%u",
                          held->ordinal, held->serial++);
    if (length < 0 || (size_t)length >= sizeof(name)) return false;
    ir_instr_t *wrap = ir_instr_generated(block,
        buffer ? IR_OP_OPTION_SOME : IR_OP_OPTION_NONE,
        window->conversion->span, buffer_borrow_pass, at);
    if (!wrap || !ir_instr_set_dest(wrap,
            core_string_borrow(name, (size_t)length), held->type)
        || (buffer && !ir_instr_operand_value(wrap, buffer))) return false;
    ir_instr_t *copy = ir_instr_generated(block, IR_OP_COPY,
        window->conversion->span, buffer_borrow_pass, at + 1);
    return copy && ir_instr_set_dest_local(copy, held->local)
        && ir_instr_operand_value(copy, wrap->dest);
}

static bool
insert_end(window_t *window, ir_block_t *block, uint32_t at, held_t *held)
{
    ir_instr_t *end = ir_instr_generated(block, IR_OP_BUFFER_BORROW_END,
        window->conversion->span, buffer_borrow_pass, at);
    if (!end) return false;
    if (!held) return ir_instr_operand_value(end, window->buffer);
    return ir_instr_operand_value(end,
            ir_local_value(window->function, held->local))
        && insert_held_assignment(window, held, block, at + 1, nullptr);
}

static bool
insert_ends(window_t *window, end_sites_t *sites, held_t *held)
{
    for (uint32_t i = 0; i < sites->len; i++) {
        end_site_t *site = &sites->items[i];
        if (site->after) {
            if (!insert_end(window, site->after->block,
                            instruction_position(site->after) + 1, held))
                return false;
            continue;
        }
        if (predecessor_count(window->function, site->to) == 1) {
            if (!insert_end(window, site->to, first_non_phi(site->to),
                            held))
                return false;
            continue;
        }
        ir_instr_t *terminator = ir_block_terminator(site->from);
        if (terminator && ir_succ_count(terminator) == 1) {
            if (!insert_end(window, site->from,
                            instruction_position(terminator), held))
                return false;
            continue;
        }
        ir_block_t *middle = split_edge(window->function, site->from,
                                        site->to, window->conversion->span);
        if (!middle || !insert_end(window, middle, 0, held)) return false;
    }
    return true;
}

static bool
insert_held_ends(window_t *window, end_sites_t *sites)
{
    ir_func_t *function = window->function;
    th_type *type = th_option(ir_module_types(function->owner),
                              window->buffer->type);
    uint32_t ordinal = function->n_locals;
    char name[64];
    int length = snprintf(name, sizeof(name), "__n00bb_borrow_held_local_%u",
                          ordinal);
    if (length < 0 || (size_t)length >= sizeof(name)) length = 0;
    held_t held = {
        .ordinal = ordinal,
        .local = type && length ? ir_local_add(function,
            core_string_borrow(name, (size_t)length), type) : nullptr,
        .type = type,
    };
    ir_block_t *entry = ir_block_entry(function);
    return held.local && entry
        && insert_held_assignment(window, &held, entry,
                                  first_non_phi(entry), nullptr)
        && insert_held_assignment(window, &held, window->conversion->block,
               instruction_position(window->conversion) + 1, window->buffer)
        && insert_ends(window, sites, &held);
}

enum { BLOCK_UNSEEN, BLOCK_ACTIVE, BLOCK_DONE };

typedef struct {
    ir_block_t *block;
    uint32_t successor;
} walk_frame_t;

typedef struct {
    window_t *window;
    end_sites_t sites;
    uint8_t *state;
    bool *entered;
    bool *continued;
    uint64_t *after;
    uint64_t *scratch;
    uint32_t after_cap;
    ir_instr_t *offender;
    core_string_t reason;
    root_set_t source;
    bool source_global;
    bool conditional;
} walk_t;

static bool
root_is_buffer(const effects_table_t *table, const ir_value_t *root)
{
    uint32_t slot = 0;
    if (root == &nonlocal_root) return true;
    if (is_contents_root(table, root)
        && pointer_map_get(&table->contents_index, root, &slot))
        return table->contents_hold_buffer[slot];
    if (root->kind == IR_VAL_INSTR && root->def && is_call(root->def))
        return true;
    return type_may_hold_buffer(table->types, root->type, 0);
}

static bool
buffer_roots_meet(const effects_table_t *table, const root_set_t *left,
                  const root_set_t *right)
{
    for (uint32_t i = 0; i < left->len; i++)
        if (root_is_buffer(table, left->items[i])
            && root_set_has(right, left->items[i]))
            return true;
    return false;
}

static bool
roots_reach_global(const walk_t *walk, const root_set_t *roots)
{
    const effects_table_t *table = walk->window->origins->effects;
    for (uint32_t i = 0; i < roots->len; i++) {
        ir_value_t *root = roots->items[i];
        if (!root_is_buffer(table, root)) continue;
        if (root == &nonlocal_root || root->kind == IR_VAL_PARAM
            || is_contents_root(walk->window->origins->effects, root)
            || root_set_has(&walk->window->origins->escaped, root))
            return true;
    }
    return false;
}

static void
blame(walk_t *walk, ir_instr_t *offender, core_string_t reason)
{
    if (walk->offender) return;
    walk->offender = offender;
    walk->reason = reason;
}

static bool
variable_slot(const ir_value_t *address)
{
    return address && address->kind == IR_VAL_INSTR && address->def
        && (address->def->op == IR_OP_ALLOC
            || address->def->op == IR_OP_ADDRESS_OF);
}

static bool
stores_into_aggregate(window_t *window, const ir_instr_t *instruction)
{
    uint32_t index = 0;
    switch (instruction->op) {
    case IR_OP_STORE:
        return instruction->n_operands >= 2
            && tracked_index(window, operand_at(instruction, 1), &index)
            && !variable_slot(operand_at(instruction, 0));
    case IR_OP_INSERT:
    case IR_OP_SEQUENCE_SET:
        return instruction->n_operands >= 3
            && tracked_index(window, operand_at(instruction, 2), &index);
    case IR_OP_AGGREGATE:
    case IR_OP_ARRAY_NEW:
    case IR_OP_ARRAY_NEW_USING:
    case IR_OP_VARIANT_NEW:
    case IR_OP_VARIANT_NEW_USING:
    case IR_OP_VARIANT_MAKE:
    case IR_OP_BOX:
    case IR_OP_ATOMIC_WRAP:
    case IR_OP_ATOMIC_RMW:
    case IR_OP_ATOMIC_CAS:
    case IR_OP_GLOBAL_STORE:
    case IR_OP_MEMCPY:
    case IR_OP_MEMMOVE:
        for (uint32_t o = 0; o < instruction->n_operands; o++)
            if (tracked_index(window, operand_at(instruction, o), &index))
                return true;
        return false;
    default:
        return false;
    }
}

static bool
carries_view(window_t *window, ir_value_t *value)
{
    root_set_t roots = {0};
    bool carries = seed_value(window->origins, value, &roots, nullptr)
        && root_set_has(&roots, window->conversion->dest);
    free(roots.items);
    return carries;
}

static bool
is_view_type(window_t *window, th_type *type)
{
    return type && ir_type_eq(window->function->owner, type,
                              window->conversion->dest->type);
}

static bool
call_wraps_view(window_t *window, const ir_instr_t *instruction)
{
    if (!instruction->dest || !carries_view(window, instruction->dest))
        return false;
    if (instruction->op == IR_OP_CALL_OUTPUT)
        return !is_view_type(window, instruction->dest->type);
    if (!is_call(instruction)) return false;
    th_type *signature = ir_instr_call_signature(instruction);
    signature = signature ? th_resolve(signature) : nullptr;
    if (!signature || th_kind_of(signature) != TH_FUNCTION)
        signature = function_signature(direct_callee(instruction));
    if (!signature) return false;
    return !is_view_type(window, th_func_ret(signature))
        && !is_view_type(window, th_func_error(signature));
}

static bool
check_instruction(walk_t *walk, ir_instr_t *instruction)
{
    window_t *window = walk->window;
    if (stores_into_aggregate(window, instruction)
        || call_wraps_view(window, instruction))
        blame(walk, instruction, CORE_STRING_STATIC(
              "the borrowed string is stored into a field, element or "
              "container here"));
    if (instruction->op == IR_OP_RETURN || instruction->op == IR_OP_FAIL) {
        uint32_t index = 0;
        for (uint32_t o = 0; o < instruction->n_operands; o++)
            if (tracked_index(window, operand_at(instruction, o), &index))
                blame(walk, instruction, instruction->op == IR_OP_RETURN
                      ? CORE_STRING_STATIC("the borrowed string is returned here")
                      : CORE_STRING_STATIC(
                          "the borrowed string is the failure value here"));
        return true;
    }
    bool call = is_call(instruction);
    if (!call && !writes_memory(instruction)) return true;
    ir_func_t *callee = call ? direct_callee(instruction) : nullptr;
    effects_t *effects = callee
        ? effects_for(window->origins->effects, callee) : nullptr;
    if (call && (!effects || effects->opaque)) {
        blame(walk, instruction, CORE_STRING_STATIC(
              "this indirect or unknown call may write the borrowed Buffer"));
        return true;
    }
    root_set_t written = {0};
    bool writes_nonlocal = false;
    bool ok = instruction_write_roots(window->origins, instruction, &written,
                                      &writes_nonlocal);
    if (ok && (buffer_roots_meet(window->origins->effects, &written,
                                 &walk->source)
               || (walk->source_global
                   && (writes_nonlocal
                       || roots_reach_global(walk, &written)))))
        blame(walk, instruction, call
              ? CORE_STRING_STATIC("this call may write the borrowed Buffer")
              : CORE_STRING_STATIC("this store may write the borrowed Buffer"));
    free(written.items);
    return ok;
}

static bool
walk_block(walk_t *walk, ir_block_t *block, uint32_t start,
           bool *reaches_end)
{
    window_t *window = walk->window;
    if (block->n_instrs > walk->after_cap) {
        uint64_t *after = realloc(walk->after,
            (size_t)block->n_instrs * window->words * sizeof(uint64_t));
        if (!after) return false;
        walk->after = after;
        walk->after_cap = block->n_instrs;
    }
    live_after_each(window, block, walk->after, walk->scratch);
    ir_instr_t *terminator = ir_block_terminator(block);
    for (uint32_t i = start; i < block->n_instrs; i++) {
        ir_instr_t *instruction = block->instrs[i];
        if (!check_instruction(walk, instruction)) return false;
        if (instruction == terminator) break;
        if (bitset_empty(&walk->after[(size_t)i * window->words],
                         window->words)) {
            *reaches_end = false;
            return add_end_site(&walk->sites,
                                (end_site_t){.after = instruction});
        }
    }
    *reaches_end = true;
    return true;
}

static bool
check_joins(walk_t *walk)
{
    ir_func_t *function = walk->window->function;
    uint32_t count = function->n_blocks;
    for (uint32_t b = 0; b < count && !walk->offender; b++) {
        if (!walk->entered[b]) continue;
        ir_block_t *block = function->blocks[b];
        for (uint32_t p = 0; p < count; p++) {
            ir_instr_t *terminator = ir_block_terminator(function->blocks[p]);
            for (uint32_t s = 0; terminator && s < ir_succ_count(terminator);
                 s++)
                if (ir_succ_at(terminator, s) == block
                    && !walk->continued[(size_t)b * count + p])
                    walk->conditional = true;
        }
    }
    return true;
}

static bool
walk_window(walk_t *walk)
{
    window_t *window = walk->window;
    ir_func_t *function = window->function;
    uint32_t count = function->n_blocks;
    walk->state = calloc(count, sizeof(*walk->state));
    walk->entered = calloc(count, sizeof(*walk->entered));
    walk->continued = calloc((size_t)count * count,
                             sizeof(*walk->continued));
    walk->scratch = calloc(window->words, sizeof(uint64_t));
    walk_frame_t *stack = calloc(count + 1, sizeof(*stack));
    bool ok = walk->state && walk->entered && walk->continued
        && walk->scratch && stack;
    ir_block_t *origin = window->conversion->block;
    bool reaches_end = false;
    ok = ok && walk_block(walk, origin,
                          instruction_position(window->conversion),
                          &reaches_end);
    uint32_t depth = 0;
    if (ok && reaches_end) {
        walk->state[origin->index] = BLOCK_ACTIVE;
        stack[depth++] = (walk_frame_t){.block = origin};
    }
    while (ok && depth && !walk->offender) {
        walk_frame_t *frame = &stack[depth - 1];
        ir_instr_t *terminator = ir_block_terminator(frame->block);
        if (!terminator || frame->successor >= ir_succ_count(terminator)) {
            walk->state[frame->block->index] = BLOCK_DONE;
            depth--;
            continue;
        }
        ir_block_t *successor = ir_succ_at(terminator, frame->successor++);
        bool live = false;
        ok = edge_live(window, frame->block, successor, &live);
        if (!ok) break;
        if (!live) {
            ok = add_end_site(&walk->sites, (end_site_t){
                .from = frame->block, .to = successor,
            });
            continue;
        }
        walk->continued[(size_t)successor->index * count
                        + frame->block->index] = true;
        if (walk->state[successor->index] == BLOCK_ACTIVE
            || successor == origin) {
            blame(walk, terminator, CORE_STRING_STATIC(
                  "the borrow is still live around this loop"));
            break;
        }
        if (walk->state[successor->index] == BLOCK_DONE) continue;
        walk->entered[successor->index] = true;
        ok = walk_block(walk, successor, 0, &reaches_end);
        if (ok && reaches_end) {
            walk->state[successor->index] = BLOCK_ACTIVE;
            stack[depth++] = (walk_frame_t){.block = successor};
        }
        else walk->state[successor->index] = BLOCK_DONE;
    }
    free(stack);
    return ok && check_joins(walk);
}

static bool
escapes_at(window_t *window, ir_instr_t *instruction, bool *escapes)
{
    origins_t *origins = window->origins;
    root_set_t escaped = origins->escaped;
    origins->escaped = (root_set_t){0};
    bool changed = false;
    bool ok = transfer(origins, instruction, &changed);
    *escapes = ok && root_set_has(&origins->escaped,
                                  window->conversion->dest);
    free(origins->escaped.items);
    origins->escaped = escaped;
    return ok;
}

static bool
check_escapes(window_t *window, walk_t *walk)
{
    if (!root_set_has(&window->origins->escaped, window->conversion->dest))
        return true;
    for (uint32_t b = 0; b < window->function->n_blocks; b++) {
        ir_block_t *block = window->function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            bool escapes = false;
            if (!escapes_at(window, block->instrs[i], &escapes))
                return false;
            if (!escapes) continue;
            blame(walk, block->instrs[i], CORE_STRING_STATIC(
                  "the borrowed string is stored or passed where it may be "
                  "kept"));
            return true;
        }
    }
    blame(walk, window->conversion,
          CORE_STRING_STATIC("the borrowed string may be kept"));
    return true;
}

static bool
capture_buffer(window_t *window)
{
    ir_value_t *buffer = window->buffer;
    ir_instr_t *conversion = window->conversion;
    if (buffer->kind != IR_VAL_GLOBAL && buffer->kind != IR_VAL_LOCAL)
        return true;
    uint32_t at = instruction_position(conversion);
    char name[64];
    int length = snprintf(name, sizeof(name), "__n00bb_borrow_source_%u_%u",
                          conversion->block->index, at);
    if (length < 0 || (size_t)length >= sizeof(name)) return false;
    ir_instr_t *copy = ir_instr_generated(conversion->block, IR_OP_COPY,
        conversion->span, buffer_borrow_pass, at);
    if (!copy || !ir_instr_set_dest(copy,
            core_string_borrow(name, (size_t)length), buffer->type)
        || !ir_instr_operand_value(copy, buffer)) return false;
    conversion->operands[0] = (ir_operand_t){
        .kind = IR_OPD_VALUE, .value = copy->dest,
    };
    window->buffer = copy->dest;
    return true;
}

static bool
borrow_conversion(ir_diags_t *diagnostics, ir_func_t *function,
                  origins_t *origins, ir_instr_t *conversion)
{
    window_t window = {
        .diagnostics = diagnostics,
        .function = function,
        .conversion = conversion,
        .buffer = conversion->n_operands == 1
            ? operand_at(conversion, 0) : nullptr,
        .origins = origins,
    };
    walk_t walk = {.window = &window};
    bool ok = window.buffer && conversion->dest
        && collect_tracked(&window) && solve_liveness(&window)
        && seed_value(origins, window.buffer, &walk.source, nullptr)
        && expand_contents(origins, &walk.source);
    if (ok) {
        walk.source_global = roots_reach_global(&walk, &walk.source);
        ok = check_escapes(&window, &walk);
        if (ok && !walk.offender) ok = walk_window(&walk);
    }
    if (ok && walk.offender)
        report_borrow(&window, walk.offender, walk.reason);
    else if (ok && walk.conditional)
        ok = capture_buffer(&window) && insert_held_ends(&window, &walk.sites);
    else if (ok)
        ok = capture_buffer(&window)
            && insert_ends(&window, &walk.sites, nullptr);
    if (!ok)
        report_borrow(&window, nullptr, (core_string_t){});
    bool accepted = ok && !walk.offender;
    free(walk.sites.items);
    free(walk.state);
    free(walk.entered);
    free(walk.continued);
    free(walk.after);
    free(walk.scratch);
    free(walk.source.items);
    free(window.tracked.items);
    pointer_map_free(&window.tracked_index);
    free(window.live_in);
    free(window.live_out);
    return accepted;
}

static size_t
collect_conversions(const ir_func_t *function, ir_instr_t **into)
{
    size_t count = 0;
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++)
            if (function->blocks[b]->instrs[i]->op
                    == IR_OP_BUFFER_AS_STRING) {
                if (into) into[count] = function->blocks[b]->instrs[i];
                count++;
            }
    return count;
}

static bool
borrow_function(ir_diags_t *diagnostics, effects_table_t *table,
                th_ctx *types, ir_func_t *function, size_t count)
{
    ir_instr_t **conversions = calloc(count, sizeof(*conversions));
    if (!conversions) return false;
    collect_conversions(function, conversions);
    origins_t origins = {
        .function = function, .types = types, .effects = table,
    };
    bool ok = true;
    for (size_t c = 0; ok && c < count; c++) {
        root_set_t *origin = origin_slot(&origins, conversions[c]->dest,
                                         false);
        ok = origin && root_set_add(origin, conversions[c]->dest, nullptr);
    }
    ok = ok && origins_solve(&origins);
    bool accepted = ok;
    for (size_t c = 0; ok && c < count; c++)
        if (!borrow_conversion(diagnostics, function, &origins,
                               conversions[c]))
            accepted = false;
    free(conversions);
    origins_free(&origins);
    return accepted;
}

bool
n00bb_buffer_borrow_pass(ir_module_t *module, ir_diags_t *diagnostics)
{
    th_ctx *types = ir_module_types(module);
    effects_table_t table = {.types = types};
    bool ok = true;
    bool solved = false;
    for (size_t f = 0; ok && f < ir_func_count(module); f++) {
        ir_func_t *function = ir_func_at(module, f);
        size_t count = collect_conversions(function, nullptr);
        if (!count) continue;
        if (!solved) {
            for (size_t g = 0; ok && g < ir_func_count(module); g++)
                if (collect_conversions(ir_func_at(module, g), nullptr))
                    ok = collect_reachable(&table, ir_func_at(module, g));
            ok = ok && solve_effects(&table, types);
            solved = true;
        }
        if (ok && !borrow_function(diagnostics, &table, types, function,
                                   count))
            ok = false;
    }
    effects_table_free(&table);
    return ok;
}
