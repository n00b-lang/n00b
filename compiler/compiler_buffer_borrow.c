#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler_buffer_borrow.h"
#include "compiler_tu.h"

#include "ir/doms.h"
#include "ir/pass.h"

static const char borrow_diagnostic_text[] =
    "this use of a Buffer as a string outlives a safe borrow; consume it "
    "with to_string(b), or keep b with to_string(buffer_from_string(b))";

static const char write_lock_name[] = "builtin.containers.buffer.write_lock";

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
    ir_func_t *function;
    uint64_t writes[REACH_KINDS];
    uint64_t escapes[REACH_KINDS];
    uint64_t returns[REACH_KINDS];
    bool writes_nonlocal;
    bool returns_nonlocal;
    bool returns_fresh;
    bool opaque;
} effects_t;

typedef struct {
    effects_t *items;
    size_t len;
    size_t cap;
    pointer_map_t index;
    pointer_map_t contents_index;
    ir_value_t **contents;
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
    const char *name = nullptr;
    size_t length = 0, args = 0;
    return th_kind_of(type) == TH_NAMED
        && th_named_info(type, &name, &length, nullptr, &args) && !args
        && length == sizeof(n00bb_buffer_type_name) - 1
        && !memcmp(name, n00bb_buffer_type_name, length);
}

static bool type_may_hold_buffer(th_ctx *types, th_type *type,
                                 uint32_t depth);

static bool
arguments_may_hold_buffer(th_ctx *types, th_type *type, uint32_t depth)
{
    size_t args = 0;
    if (!th_named_info(type, nullptr, nullptr, nullptr, &args)) return false;
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
        if (!contents || !holds) return nullptr;
        table->contents_hold_buffer = holds;
        table->contents_cap = cap;
    }
    ir_value_t *root = calloc(1, sizeof(*root));
    if (!root) return nullptr;
    *root = (ir_value_t){
        .kind = IR_VAL_LOCAL,
        .name = "<contents>",
        .type = base->type,
        .owner = base->owner,
        .index = base->index,
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

static void
effects_table_free(effects_table_t *table)
{
    for (uint32_t i = 0; i < table->contents_len; i++)
        free(table->contents[i]);
    free(table->contents);
    free(table->contents_hold_buffer);
    free(table->items);
    pointer_map_free(&table->index);
    pointer_map_free(&table->contents_index);
}

static bool
function_is_native(const ir_func_t *function)
{
    return ir_annot_get((ir_annots_t *)&function->annots, "n00bb.abi.native")
        != nullptr;
}

static bool
function_is_write_lock(const ir_func_t *function)
{
    const char *module = function->owner ? ir_module_name(function->owner)
                                         : nullptr;
    if (!module || !function->name) return false;
    size_t module_len = strlen(module);
    return !strncmp(write_lock_name, module, module_len)
        && write_lock_name[module_len] == '.'
        && !strcmp(write_lock_name + module_len + 1, function->name);
}

static effects_t *
effects_for(effects_table_t *table, ir_func_t *function)
{
    uint32_t slot = 0;
    if (pointer_map_get(&table->index, function, &slot))
        return &table->items[slot];
    if (table->len == table->cap) {
        size_t cap = table->cap ? table->cap * 2 : 64;
        effects_t *items = realloc(table->items, cap * sizeof(*items));
        if (!items) {
            table->failed = true;
            return nullptr;
        }
        table->items = items;
        table->cap = cap;
    }
    effects_t *effects = &table->items[table->len];
    *effects = (effects_t){.function = function};
    if (!function->n_blocks && !function_is_native(function)) {
        for (int kind = 0; kind < REACH_KINDS; kind++)
            effects->writes[kind] = effects->escapes[kind]
                = effects->returns[kind] = UINT64_MAX;
        effects->writes_nonlocal = effects->returns_nonlocal = true;
        effects->opaque = true;
    }
    else if (!function->n_blocks) {
        effects->returns[REACH_SELF] = UINT64_MAX;
        effects->returns[REACH_CONTENTS] = UINT64_MAX;
        effects->returns_fresh = true;
    }
    if (function_is_write_lock(function)) effects->writes[REACH_SELF] = 1;
    if (!pointer_map_put(&table->index, function, (uint32_t)table->len)) {
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
    return target && target->kind == IR_VAL_FUNC ? target->owner : nullptr;
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
        return root_set_add(into, &nonlocal_root, changed);
    case IR_VAL_FUNC:
        return true;
    default: {
        root_set_t *origin = origin_slot(state, value, false);
        return origin && root_set_union(into, origin, changed);
    }
    }
}

static bool
expand_contents(origins_t *state, root_set_t *roots)
{
    bool changed = true;
    while (changed) {
        changed = false;
        uint32_t len = roots->len;
        for (uint32_t i = 0; i < len; i++) {
            if (!is_allocation(roots->items[i])) continue;
            root_set_t *memory = origin_slot(state, roots->items[i], true);
            if (!memory || !root_set_union(roots, memory, &changed))
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
        if (is_allocation(root)) {
            root_set_t *memory = origin_slot(state, root, true);
            if (!memory || !root_set_union(into, memory, nullptr))
                return false;
        }
        else if (root != &nonlocal_root
                 && !is_contents_root(state->effects, root)) {
            ir_value_t *contents = contents_root(state->effects, root);
            if (!contents || !root_set_add(into, contents, nullptr))
                return false;
        }
        else if (!root_set_add(into, root, nullptr)) return false;
    }
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

static bool
record_escape(origins_t *state, const root_set_t *roots, bool *changed)
{
    root_set_t expanded = {0};
    bool ok = root_set_union(&expanded, roots, nullptr)
        && expand_contents(state, &expanded)
        && root_set_union(&state->escaped, &expanded, changed);
    free(expanded.items);
    return ok;
}

static bool
transfer_call(origins_t *state, ir_instr_t *instruction, root_set_t *result,
              bool *changed)
{
    ir_func_t *callee = direct_callee(instruction);
    effects_t *effects = callee ? effects_for(state->effects, callee)
                                : nullptr;
    if (callee && !effects) return false;
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
            && load_roots(state, &roots, &contents);
        uint32_t index = i - first;
        bool carry = contents_carry_references(state, argument);
        for (int kind = 0; ok && kind < REACH_KINDS; kind++) {
            if (kind == REACH_CONTENTS && !carry) continue;
            root_set_t *reached = kind == REACH_SELF ? &roots : &contents;
            bool escapes = opaque
                || effect_bit(effects->escapes, index, kind);
            bool returned = opaque
                || effect_bit(effects->returns, index, kind);
            if (ok && escapes) ok = record_escape(state, reached, changed);
            if (ok && escapes) ok = root_set_union(&kept, reached, nullptr);
            if (ok && returned)
                ok = root_set_union(result, reached, nullptr);
        }
        free(roots.items);
        free(contents.items);
    }
    for (uint32_t i = first; ok && i < instruction->n_operands; i++) {
        root_set_t roots = {0};
        ok = seed_value(state, operand_at(instruction, i), &roots, nullptr);
        for (uint32_t r = 0; ok && r < roots.len; r++) {
            if (!is_allocation(roots.items[r])) continue;
            root_set_t *memory = origin_slot(state, roots.items[r], true);
            ok = memory && root_set_union(memory, &kept, changed)
                && root_set_add(memory, roots.items[r], changed);
        }
        free(roots.items);
    }
    free(kept.items);
    if (!ok) return false;
    if (opaque || effects->returns_nonlocal)
        if (!root_set_add(result, &nonlocal_root, nullptr)) return false;
    if (instruction->dest && effects && effects->returns_fresh)
        if (!root_set_add(result, instruction->dest, nullptr)) return false;
    return true;
}

static ir_value_t *
output_argument(const ir_instr_t *output, const ir_instr_t *call)
{
    ir_func_t *callee = call ? direct_callee(call) : nullptr;
    th_type *signature = callee && callee->type
        ? th_resolve(callee->type) : nullptr;
    if (!signature || th_kind_of(signature) != TH_FUNCTION
        || output->n_operands < 2
        || output->operands[1].kind != IR_OPD_CONST) return nullptr;
    uint64_t wanted = output->operands[1].lit.i.mag;
    uint64_t seen = 0;
    for (size_t i = 0; i < th_func_nparams(signature); i++) {
        const th_param *formal = th_func_param(signature, i);
        if (!formal || !th_mode_produces_output(formal->mode)) continue;
        if (seen++ != wanted) continue;
        return i + 1 < call->n_operands ? operand_at(call, (uint32_t)i + 1)
                                        : nullptr;
    }
    return nullptr;
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
    case IR_OP_MEMCPY: {
        uint32_t value_at = instruction->op == IR_OP_STORE
            || instruction->op == IR_OP_MEMCPY ? 1 : 2;
        if (instruction->n_operands <= value_at) break;
        root_set_t address = {0};
        root_set_t stored = {0};
        ok = seed_value(state, operand_at(instruction, 0), &address, nullptr)
            && seed_value(state, operand_at(instruction, value_at), &stored,
                          nullptr);
        if (ok && instruction->op == IR_OP_MEMCPY) {
            ir_value_t *destination = operand_at(instruction, 0);
            th_type *pointer = destination && destination->type
                ? th_resolve(destination->type) : nullptr;
            th_type *element = pointer
                && th_kind_of(pointer) == TH_INDIRECTION
                ? th_ind_referent(pointer) : nullptr;
            root_set_t contents = {0};
            if (!element
                || type_may_contain_reference_alias(state->types, element))
                ok = load_roots(state, &stored, &contents);
            free(stored.items);
            stored = contents;
        }
        for (uint32_t i = 0; ok && i < address.len; i++) {
            ir_value_t *root = address.items[i];
            if (is_allocation(root)) {
                root_set_t *memory = origin_slot(state, root, true);
                ok = memory && root_set_union(memory, &stored, changed);
            }
            else ok = record_escape(state, &stored, changed);
        }
        free(address.items);
        free(stored.items);
        break;
    }
    case IR_OP_GLOBAL_STORE: {
        if (instruction->n_operands < 2) break;
        root_set_t stored = {0};
        ok = seed_value(state, operand_at(instruction, 1), &stored, nullptr)
            && record_escape(state, &stored, changed);
        free(stored.items);
        break;
    }
    case IR_OP_RETURN:
        break;
    case IR_OP_CALL:
    case IR_OP_CALLBACK_INVOKE:
        ok = transfer_call(state, instruction, &gathered, changed);
        break;
    case IR_OP_CALL_OUTPUT: {
        ir_value_t *call_value = instruction->n_operands
            ? operand_at(instruction, 0) : nullptr;
        ir_instr_t *call = call_value && call_value->kind == IR_VAL_INSTR
            ? call_value->def : nullptr;
        uint32_t first = call && call->op == IR_OP_CALL ? 1 : 0;
        ir_value_t *argument = output_argument(instruction, call);
        if (argument)
            ok = seed_value(state, argument, &gathered, nullptr);
        else
            for (uint32_t i = first; ok && call && i < call->n_operands; i++)
                ok = seed_value(state, operand_at(call, i), &gathered,
                                nullptr);
        ok = ok && root_set_add(&gathered, instruction->dest, nullptr);
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
            && (!contents || load_roots(state, &roots, written));
        free(roots.items);
        if (!ok) return false;
    }
    return expand_contents(state, written);
}

static uint64_t
parameter_bit(const ir_value_t *root)
{
    return root->index < 64 ? UINT64_C(1) << root->index : UINT64_MAX;
}

static bool
reach_kind(const effects_table_t *table, const ir_value_t *root, int *kind)
{
    if (root == &nonlocal_root) return false;
    if (root->kind == IR_VAL_PARAM) {
        *kind = REACH_SELF;
        return true;
    }
    if (is_contents_root(table, root)) {
        *kind = REACH_CONTENTS;
        return true;
    }
    return false;
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
    uint64_t returns[REACH_KINDS] = {0};
    bool writes_nonlocal = false;
    bool returns_nonlocal = false;
    bool returns_fresh = false;
    int kind = 0;
    for (uint32_t b = 0; ok && b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; ok && i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            root_set_t roots = {0};
            if (is_call(instruction)) {
                bool nonlocal = false;
                ok = call_write_roots(&state, instruction, &roots,
                                      &nonlocal);
                if (nonlocal) writes_nonlocal = true;
                for (uint32_t r = 0; ok && r < roots.len; r++) {
                    ir_value_t *root = roots.items[r];
                    if (root == &nonlocal_root) writes_nonlocal = true;
                    else if (reach_kind(table, root, &kind))
                        writes[kind] |= parameter_bit(root);
                }
            }
            else if (instruction->op == IR_OP_RETURN) {
                for (uint32_t o = 0; ok && o < instruction->n_operands; o++)
                    ok = seed_value(&state, operand_at(instruction, o),
                                    &roots, nullptr);
                for (uint32_t r = 0; ok && r < roots.len; r++) {
                    ir_value_t *root = roots.items[r];
                    if (root == &nonlocal_root) returns_nonlocal = true;
                    else if (reach_kind(table, root, &kind))
                        returns[kind] |= parameter_bit(root);
                    else returns_fresh = true;
                }
            }
            free(roots.items);
        }
    }
    for (uint32_t r = 0; ok && r < state.escaped.len; r++) {
        ir_value_t *root = state.escaped.items[r];
        if (reach_kind(table, root, &kind))
            escapes[kind] |= parameter_bit(root);
    }
    origins_free(&state);
    if (!ok) return false;
    if (memcmp(writes, effects->writes, sizeof(writes))
        || memcmp(escapes, effects->escapes, sizeof(escapes))
        || memcmp(returns, effects->returns, sizeof(returns))
        || writes_nonlocal != effects->writes_nonlocal
        || returns_nonlocal != effects->returns_nonlocal
        || returns_fresh != effects->returns_fresh) {
        memcpy(effects->writes, writes, sizeof(writes));
        memcpy(effects->escapes, escapes, sizeof(escapes));
        memcpy(effects->returns, returns, sizeof(returns));
        effects->writes_nonlocal = writes_nonlocal;
        effects->returns_nonlocal = returns_nonlocal;
        effects->returns_fresh = returns_fresh;
        *changed = true;
    }
    return true;
}

static bool
collect_reachable(effects_table_t *table, ir_func_t *root)
{
    size_t start = table->len;
    if (!effects_for(table, root)) return false;
    for (size_t at = start; at < table->len; at++) {
        ir_func_t *function = table->items[at].function;
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
            if (!summarize(table, types, &table->items[i], &changed))
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
              const char *label)
{
    ir_diag_t *diagnostic = ir_diag_begin_at(window->diagnostics,
        IR_DIAG_VERIFY_LIFETIME, window->conversion,
        window->conversion->block, window->function,
        "this Buffer is borrowed as a string here");
    char message[512];
    if (offender && label && offender->span.start_line)
        snprintf(message, sizeof(message), "%s (%s, at line %u, column %u)",
                 borrow_diagnostic_text, label, offender->span.start_line,
                 offender->span.start_col);
    else if (label)
        snprintf(message, sizeof(message), "%s (%s)", borrow_diagnostic_text,
                 label);
    else
        snprintf(message, sizeof(message), "%s", borrow_diagnostic_text);
    ir_diag_message(diagnostic, window->diagnostics, message);
    if (offender && label)
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

static ir_value_t *
defined_value(window_t *window, const ir_instr_t *instruction)
{
    if (instruction->dest) return instruction->dest;
    if (instruction->dest_local)
        return ir_local_value(window->function, instruction->dest_local);
    return nullptr;
}

static bool
collect_tracked(window_t *window)
{
    ir_value_t *view = window->conversion->dest;
    origins_t *origins = window->origins;
    for (size_t i = 0; i < origins->slots.cap; i++) {
        const ir_value_t *value = origins->slots.keys[i];
        if (!value) continue;
        uint32_t slot = origins->slots.slots[i];
        if ((root_set_has(&origins->origins[slot], view)
             && value->kind != IR_VAL_PARAM)
            || (is_allocation(value)
                && root_set_has(&origins->memory[slot], view)))
            if (!track(window, (ir_value_t *)value)) return false;
    }
    return track(window, view);
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

static bool
instruction_touches(window_t *window, const ir_instr_t *instruction)
{
    uint32_t index = 0;
    if (instruction == window->conversion) return true;
    if (tracked_index(window, defined_value(window, instruction), &index))
        return true;
    for (uint32_t o = 0; o < instruction->n_operands; o++)
        if (tracked_index(window, operand_at(instruction, o), &index))
            return true;
    return false;
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
    snprintf(name, sizeof(name), "__n00bb_borrow_edge_%u_%u", from->index,
             to->index);
    ir_block_t *middle = ir_block_add(function, name);
    if (!middle) return nullptr;
    ir_instr_t *jump = ir_instr_generated(middle, IR_OP_JUMP, span,
        "n00bb.buffer-borrow", 0);
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
    uint32_t serial;
} held_t;

static bool
insert_held_assignment(window_t *window, held_t *held, ir_block_t *block,
                       uint32_t at, ir_value_t *buffer)
{
    char name[64];
    snprintf(name, sizeof(name), "__n00bb_borrow_held_%u", held->serial++);
    ir_instr_t *wrap = ir_instr_generated(block,
        buffer ? IR_OP_OPTION_SOME : IR_OP_OPTION_NONE,
        window->conversion->span, "n00bb.buffer-borrow", at);
    if (!wrap || !ir_instr_set_dest(wrap, name, held->type)
        || (buffer && !ir_instr_operand_value(wrap, buffer))) return false;
    ir_instr_t *copy = ir_instr_generated(block, IR_OP_COPY,
        window->conversion->span, "n00bb.buffer-borrow", at + 1);
    return copy && ir_instr_set_dest_local(copy, held->local)
        && ir_instr_operand_value(copy, wrap->dest);
}

static bool
insert_end(window_t *window, ir_block_t *block, uint32_t at, held_t *held)
{
    ir_instr_t *end = ir_instr_generated(block, IR_OP_BUFFER_BORROW_END,
        window->conversion->span, "n00bb.buffer-borrow", at);
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
    static uint32_t next_local;
    ir_func_t *function = window->function;
    th_type *type = th_option(ir_module_types(function->owner),
                              window->buffer->type);
    char name[64];
    snprintf(name, sizeof(name), "__n00bb_borrow_held_local_%u",
             next_local++);
    held_t held = {
        .local = type ? ir_local_add(function, name, type) : nullptr,
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
    const char *reason;
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
blame(walk_t *walk, ir_instr_t *offender, const char *reason)
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
        for (uint32_t o = 0; o < instruction->n_operands; o++)
            if (tracked_index(window, operand_at(instruction, o), &index))
                return true;
        return false;
    default:
        return false;
    }
}

static bool
check_instruction(walk_t *walk, ir_instr_t *instruction)
{
    window_t *window = walk->window;
    if (stores_into_aggregate(window, instruction))
        blame(walk, instruction,
              "the borrowed string is stored into a field, element or "
              "container here");
    if (instruction->op == IR_OP_RETURN) {
        uint32_t index = 0;
        for (uint32_t o = 0; o < instruction->n_operands; o++)
            if (tracked_index(window, operand_at(instruction, o), &index))
                blame(walk, instruction,
                      "the borrowed string is returned here");
        return true;
    }
    if (!is_call(instruction)) return true;
    ir_func_t *callee = direct_callee(instruction);
    effects_t *effects = callee
        ? effects_for(window->origins->effects, callee) : nullptr;
    if (!effects || effects->opaque) {
        blame(walk, instruction,
              "this indirect or unknown call may write the borrowed Buffer");
        return true;
    }
    root_set_t written = {0};
    bool writes_nonlocal = false;
    bool ok = call_write_roots(window->origins, instruction, &written,
                               &writes_nonlocal);
    if (ok && (buffer_roots_meet(window->origins->effects, &written,
                                 &walk->source)
               || (walk->source_global
                   && (writes_nonlocal
                       || roots_reach_global(walk, &written)))))
        blame(walk, instruction, "this call may write the borrowed Buffer");
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
            blame(walk, terminator,
                  "the borrow is still live around this loop");
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

static void
check_escapes(window_t *window, walk_t *walk)
{
    origins_t *origins = window->origins;
    if (!root_set_has(&origins->escaped, window->conversion->dest)) return;
    for (uint32_t b = 0; b < window->function->n_blocks; b++) {
        ir_block_t *block = window->function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            if ((instruction->op == IR_OP_STORE
                 || instruction->op == IR_OP_GLOBAL_STORE
                 || instruction->op == IR_OP_SEQUENCE_SET
                 || instruction->op == IR_OP_ATOMIC_RMW
                 || instruction->op == IR_OP_ATOMIC_CAS
                 || instruction->op == IR_OP_MEMCPY || is_call(instruction))
                && instruction != window->conversion
                && instruction_touches(window, instruction)) {
                blame(walk, instruction,
                      "the borrowed string is stored or passed where it "
                      "may be kept");
                return;
            }
        }
    }
    blame(walk, window->conversion, "the borrowed string may be kept");
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
        check_escapes(&window, &walk);
        if (!walk.offender) ok = walk_window(&walk);
    }
    if (ok && walk.offender)
        report_borrow(&window, walk.offender, walk.reason);
    else if (ok && walk.conditional)
        ok = insert_held_ends(&window, &walk.sites);
    else if (ok)
        ok = insert_ends(&window, &walk.sites, nullptr);
    if (!ok)
        report_borrow(&window, nullptr, nullptr);
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
