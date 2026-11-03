#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler_axioms.h"
#include "compiler_buffer_borrow.h"
#include "compiler_tu.h"
#include "cached_body.h"
#include "diagnostics.h"
#include "sequence_transfer.h"

#include "ir/doms.h"
#include "ir/liveness.h"
#include "ir/pass.h"

static core_diag_location_t
borrow_location(ir_span_t span)
{
    if (!ir_span_known(span)) return (core_diag_location_t){0};
    return (core_diag_location_t){
        .line = span.start_line,
        .column = span.start_col,
        .end_line = span.end_line,
        .end_column = span.end_col,
        .has_location = true,
    };
}

static core_string_t
source_name_of(core_string_t name)
{
    for (size_t i = name.u8_bytes; i > 0; i--)
        if (name.data[i - 1] == '.')
            return core_string_slice(name, i, name.u8_bytes - i);
    return name;
}

static void
borrow_internal(ir_diags_t *diagnostics, ir_func_t *function,
                const ir_instr_t *at, const char *what)
{
    core_diagnostics_t *destination = ir_diags_destination(diagnostics);
    if (!destination) return;
    n00bb_internal_error(destination, ir_diags_unit(diagnostics),
        borrow_location(at ? at->span : function->span),
        "checking borrows", "%s in `%.*s`", what,
        (int)function->name.u8_bytes, function->name.data);
}

static const core_string_t buffer_state_type_name =
    CORE_STRING_STATIC("builtin.containers.buffer.Buffer"
                       n00bb_object_shape_suffix);

static const core_string_t buffer_borrow_pass =
    CORE_STRING_STATIC("n00bb.buffer-borrow");

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
    pointer_map_t flow_index;
    n00bb_flow_t **flows;
    uint32_t flows_len;
    uint32_t flows_cap;
    th_ctx *types;
} flow_cache_t;

static void
union_objects(ir_pto_list_t *into, const ir_pto_list_t *from)
{
    for (size_t i = 0; i < from->len; i++) ir_pto_add(into, from->data[i]);
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

static void
flow_cache_free(flow_cache_t *table)
{
    for (uint32_t i = 0; i < table->flows_len; i++)
        core_free(table->flows[i]);
    free(table->flows);
    pointer_map_free(&table->flow_index);
}

static uint64_t
axiom_writes(const ir_func_t *function)
{
    const n00bb_axiom_t *axiom = n00bb_axiom_of_function(function);
    return axiom ? axiom->writes : 0;
}

static th_type *
function_signature(const ir_func_t *function)
{
    th_type *signature = function && function->type
        ? th_resolve(function->type) : nullptr;
    return signature && th_kind_of(signature) == TH_FUNCTION ? signature
                                                             : nullptr;
}

static n00bb_flow_t
flow_opaque(void)
{
    n00bb_flow_t flow = {.writes_nonlocal = true, .opaque = true};
    for (int kind = 0; kind < N00BB_FLOW_KINDS; kind++)
        flow.writes[kind] = flow.mutates[kind] = flow.escapes[kind]
            = flow.leaks[kind] = UINT64_MAX;
    return flow;
}

static const n00bb_flow_t *
cached_flow(flow_cache_t *table, const ir_summary_t *summary)
{
    uint32_t slot = 0;
    if (pointer_map_get(&table->flow_index, summary, &slot))
        return table->flows[slot];
    if (table->flows_len == table->flows_cap) {
        uint32_t cap = table->flows_cap ? table->flows_cap * 2 : 64;
        n00bb_flow_t **flows = realloc(table->flows, cap * sizeof(*flows));
        if (!flows) return nullptr;
        table->flows     = flows;
        table->flows_cap = cap;
    }
    n00bb_flow_t *flow = core_alloc(n00bb_flow_t);
    n00bb_summary_flow(summary, flow);
    if (!pointer_map_put(&table->flow_index, summary, table->flows_len)) {
        core_free(flow);
        return nullptr;
    }
    table->flows[table->flows_len++] = flow;
    return flow;
}

static ir_value_t *
operand_at(const ir_instr_t *instruction, uint32_t index)
{
    const ir_operand_t *operand = &instruction->operands[index];
    if (operand->kind == IR_OPD_ARG) return operand->value;
    return ir_operand_value(operand);
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
    flow_cache_t *flows;
    ir_summaries_t *summaries;
    ir_points_to_t *points_to;
    bool constants;
} analysis_t;

static bool
is_constant_global(const ir_value_t *value)
{
    return value && value->kind == IR_VAL_GLOBAL && value->global
        && value->global->mutability == IR_MUT_CONST;
}

static bool
guards_constant(const analysis_t *analysis, const ir_pto_t *object)
{
    return analysis->constants && object->kind == IR_PTO_GLOBAL
        && object->global && object->global->mutability == IR_MUT_CONST
        && n00bb_type_reaches_shared_state(analysis->types,
                                           object->global->type);
}

static bool
is_nonlocal(const analysis_t *analysis, const ir_pto_t *object)
{
    return object->kind == IR_PTO_UNKNOWN
        || (object->kind == IR_PTO_GLOBAL
            && !guards_constant(analysis, object));
}

static bool
is_location(const ir_pto_t *object)
{
    if (object->kind != IR_PTO_STORAGE) return false;
    if (object->local || object->value) return true;
    ir_op_t op = object->instr ? object->instr->op : IR_OP_CALL;
    return op == IR_OP_ALLOC || op == IR_OP_ALLOCA || op == IR_OP_ADDRESS_OF;
}

static const ir_value_t *
object_parameter(const analysis_t *analysis, const ir_pto_t *object)
{
    if (object->kind != IR_PTO_PARAM && object->kind != IR_PTO_CONTENTS)
        return nullptr;
    return object->param < analysis->function->n_params
        ? analysis->function->params[object->param] : nullptr;
}

static void
value_objects(analysis_t *analysis, const ir_instr_t *at,
              const ir_value_t *value, ir_pto_list_t *into)
{
    if (value && value->kind != IR_VAL_FUNC)
        ir_points_to_value(analysis->points_to, at, value, into);
}

static void
reach(analysis_t *analysis, ir_pto_list_t *objects, bool contents)
{
    ir_points_to_reach(analysis->points_to, objects, contents);
}

static bool
escaped(analysis_t *analysis, const ir_pto_t *object)
{
    const ir_instr_t *where = nullptr;
    return ir_points_to_escapes(analysis->points_to, object, &where);
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
    return !signature || root->is_keyword || root->is_rest
        || root->index >= 64 ? UINT64_MAX : UINT64_C(1) << root->index;
}

typedef struct {
    const ir_summary_t *summary;
    uint32_t targets;
} callee_summary_t;

static void
collect_callee_summary(void *context, const ir_func_t *target,
                       const ir_summary_t *summary)
{
    (void)target;
    callee_summary_t *callee = context;
    callee->summary = callee->targets++ ? nullptr : summary;
}

static const n00bb_flow_t *
call_effects(analysis_t *analysis, const ir_instr_t *call)
{
    callee_summary_t callee = {0};
    return ir_summary_call_callees(analysis->summaries, call,
                                   collect_callee_summary, &callee)
        && callee.summary ? cached_flow(analysis->flows, callee.summary)
                          : nullptr;
}

static uint32_t
first_argument(const ir_instr_t *call, const n00bb_flow_t *effects)
{
    return call->op == IR_OP_CALL || (effects && !effects->opaque) ? 1 : 0;
}

static bool
fresh_copy(ir_instr_t *instruction)
{
    if (instruction->op != IR_OP_COPY || instruction->n_operands != 1)
        return false;
    if (ir_instr_is_fresh(instruction))
        return true;
    ir_annot_t *promoted = ir_annot_get(&instruction->annots,
        CORE_STRING_STATIC("ir.promoted_local_write"));
    th_type *copied = instruction->dest_kind == IR_DEST_LOCAL
            && instruction->dest_local ? instruction->dest_local->type
        : promoted && promoted->kind == IR_ANNOT_BOOL && promoted->b
            && instruction->dest ? instruction->dest->type : nullptr;
    return copied && th_kind_of(th_resolve(copied)) == TH_SEQUENCE
        && !n00bb_sequence_transfers_storage(operand_at(instruction, 0));
}

static bool
foreign_reference(th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    th_type *item = type ? th_option_item(type) : nullptr;
    if (item) type = th_resolve(item);
    uint8_t attrs = 0;
    return type && th_kind_of(type) == TH_INDIRECTION
        && th_ind_attrs(type, &attrs) && (attrs & TH_FOREIGN);
}

static th_type *
defined_type(const ir_instr_t *instruction)
{
    if (instruction->dest) return instruction->dest->type;
    return instruction->dest_kind == IR_DEST_LOCAL && instruction->dest_local
        ? instruction->dest_local->type : nullptr;
}

static ir_pt_fresh_t
borrow_fresh(void *context, const ir_instr_t *instruction)
{
    (void)context;
    if (instruction->op == IR_OP_BUFFER_AS_STRING) return IR_PT_FRESH_HOLDING;
    return fresh_copy((ir_instr_t *)instruction)
        || foreign_reference(defined_type(instruction))
        ? IR_PT_FRESH_EMPTY : IR_PT_FRESH_NONE;
}

static bool
analysis_solve(analysis_t *analysis)
{
    static const ir_points_to_hooks_t hooks = {
        .fresh = borrow_fresh,
    };
    analysis->points_to = ir_points_to_new(analysis->summaries,
                                           analysis->function, &hooks);
    return analysis->points_to;
}

static void
contents_beyond(analysis_t *analysis, const ir_pto_list_t *self,
                ir_pto_list_t *into)
{
    union_objects(into, self);
    reach(analysis, into, true);
    size_t kept = 0;
    for (size_t i = 0; i < into->len; i++)
        if (!is_location(&into->data[i]) || !ir_pto_has(self, &into->data[i]))
            into->data[kept++] = into->data[i];
    into->len = kept;
}

static void
call_write_roots(analysis_t *analysis, ir_instr_t *instruction,
                 uint64_t trusted, ir_pto_list_t *written,
                 ir_pto_list_t *mutated, bool *writes_nonlocal)
{
    const n00bb_flow_t *effects = call_effects(analysis, instruction);
    bool opaque = !effects || effects->opaque;
    if (opaque || effects->writes_nonlocal) *writes_nonlocal = true;
    uint32_t first = first_argument(instruction, effects);
    const uint64_t *writes = effects ? effects->writes : nullptr;
    const uint64_t *mutates = effects ? effects->mutates : nullptr;
    for (uint32_t i = first; i < instruction->n_operands; i++) {
        uint32_t index = i - first;
        if (index < 64 && trusted >> index & 1) continue;
        if (instruction->op == IR_OP_CALLBACK_INVOKE && i == first) continue;
        ir_value_t *argument = operand_at(instruction, i);
        if (opaque && argument && argument->type
            && !n00bb_type_reaches_shared_state(analysis->types,
                                                argument->type))
            continue;
        bool write_self = written
            && (opaque || effect_bit(writes, index, N00BB_FLOW_SELF));
        bool write_contents = written
            && (opaque || effect_bit(writes, index, N00BB_FLOW_CONTENTS));
        bool mutate_self = mutated
            && (opaque || effect_bit(mutates, index, N00BB_FLOW_SELF));
        bool mutate_contents = mutated
            && (opaque || effect_bit(mutates, index, N00BB_FLOW_CONTENTS));
        if (!write_self && !write_contents && !mutate_self
            && !mutate_contents) continue;
        ir_pto_list_t self = core_list_new(ir_pto_t);
        ir_pto_list_t contents = core_list_new(ir_pto_t);
        value_objects(analysis, instruction, argument, &self);
        if (write_contents || mutate_contents)
            contents_beyond(analysis, &self, &contents);
        if (write_self) union_objects(written, &self);
        if (write_contents) union_objects(written, &contents);
        if (mutate_self) union_objects(mutated, &self);
        if (mutate_contents) union_objects(mutated, &contents);
        core_list_free(self);
        core_list_free(contents);
    }
    if (written) reach(analysis, written, false);
    if (mutated) reach(analysis, mutated, false);
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
        if (!type || is_buffer_type(type)
            || (th_kind_of(type) == TH_INDIRECTION
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

static void
instruction_write_roots(analysis_t *analysis, ir_instr_t *instruction,
                        uint64_t trusted, ir_pto_list_t *written,
                        ir_pto_list_t *mutated, bool *writes_nonlocal)
{
    if (is_call(instruction)) {
        call_write_roots(analysis, instruction, trusted, written, mutated,
                         writes_nonlocal);
        return;
    }
    if (!writes_memory(instruction) || !instruction->n_operands) return;
    bool buffer = written && inside_buffer_state(operand_at(instruction, 0));
    if (!buffer && !mutated) return;
    ir_pto_list_t address = core_list_new(ir_pto_t);
    value_objects(analysis, instruction, operand_at(instruction, 0),
                  &address);
    for (size_t r = 0; r < address.len; r++) {
        const ir_pto_t *object = &address.data[r];
        if (is_location(object)) continue;
        if (buffer && is_nonlocal(analysis, object)) *writes_nonlocal = true;
        if (mutated) ir_pto_add(mutated, *object);
        if (buffer) ir_pto_add(written, *object);
    }
    core_list_free(address);
}

static bool
external_root(ir_splace_kind_t root)
{
    return root == IR_SPLACE_GLOBAL || root == IR_SPLACE_THREAD
        || root == IR_SPLACE_UNKNOWN;
}

static uint64_t
path_parameter_bit(const ir_func_t *function, const ir_spath_t *path)
{
    return path->param < function->n_params && function->params[path->param]
        ? parameter_bit(function->params[path->param]) : UINT64_MAX;
}

static void
add_source(uint64_t *from, const ir_func_t *function, const ir_spath_t *path)
{
    if (path->root != IR_SPLACE_PARAM) return;
    uint64_t bit    = path_parameter_bit(function, path);
    bool     derefs = ir_spath_derefs(path);
    if (!derefs) from[N00BB_FLOW_SELF] |= bit;
    if (derefs || (path->flags & IR_SPATH_DEEP))
        from[N00BB_FLOW_CONTENTS] |= bit;
}

static bool
path_in_buffer_state(th_ctx *types, const ir_func_t *function,
                     const ir_spath_t *path)
{
    th_type *type = nullptr;
    for (uint32_t d = 0; d <= path->depth; d++) {
        type = ir_summary_path_type(function, path, d);
        type = type ? th_resolve(type) : nullptr;
        if (!type || is_buffer_type(type) || may_be_buffer_state(type)
            || (th_kind_of(type) == TH_INDIRECTION
                && may_be_buffer_state(th_ind_referent(type))))
            return true;
    }
    return (path->flags & IR_SPATH_DEEP)
        && type_may_hold_buffer(types, type, 0);
}

static void
derive_writes(th_ctx *types, const ir_func_t *function,
              const ir_summary_t *summary, n00bb_flow_t *out)
{
    for (uint32_t i = 0; i < ir_summary_path_count(summary); i++) {
        const ir_spath_t *path = ir_summary_path_at(summary, i);
        if (!(path->flags & IR_SPATH_WRITE)) continue;
        bool buffer = !(path->flags & IR_SPATH_TYPED)
            || path_in_buffer_state(types, function, path);
        if (external_root(path->root)) {
            out->writes_nonlocal |= buffer;
            continue;
        }
        if (path->root != IR_SPLACE_PARAM) continue;
        uint64_t bit    = path_parameter_bit(function, path);
        bool     inside = ir_summary_path_inline(function, path);
        bool     deep   = path->flags & IR_SPATH_DEEP;
        bool     self   = inside || deep;
        bool     beyond = !inside || deep;
        out->mutates[N00BB_FLOW_SELF] |= self ? bit : 0;
        out->mutates[N00BB_FLOW_CONTENTS] |= beyond ? bit : 0;
        if (!buffer) continue;
        out->writes[N00BB_FLOW_SELF] |= self ? bit : 0;
        out->writes[N00BB_FLOW_CONTENTS] |= beyond ? bit : 0;
    }
}

static void
derive_stores(const ir_func_t *function, const ir_summary_t *summary,
              n00bb_flow_t *out)
{
    uint64_t heap[N00BB_FLOW_KINDS] = {0};
    bool escaped = false, leaked = false;
    uint32_t dests   = ir_summary_flow_count(summary, IR_SFLOW_STORES);
    uint32_t sources = ir_summary_flow_source_count(summary, IR_SFLOW_STORES);
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t d = 0; d < dests; d++)
            for (uint32_t s = 0; s < sources; s++) {
                if (!ir_summary_flow_has(summary, IR_SFLOW_STORES, d, s,
                                         IR_SFLOW_ALIAS)) continue;
                const ir_spath_t *to   = ir_summary_flow_to(summary,
                                                            IR_SFLOW_STORES,
                                                            d);
                const ir_spath_t *from = ir_summary_flow_source(
                    summary, IR_SFLOW_STORES, s);
                if (!pass) {
                    if (to->root == IR_SPLACE_HEAP)
                        add_source(heap, function, from);
                    continue;
                }
                bool leak = external_root(to->root);
                if (!leak && to->root != IR_SPLACE_PARAM) continue;
                if (to->root == IR_SPLACE_PARAM
                    && from->root == IR_SPLACE_PARAM
                    && from->param == to->param) continue;
                add_source(out->escapes, function, from);
                if (leak) add_source(out->leaks, function, from);
                escaped |= from->root == IR_SPLACE_HEAP;
                leaked |= leak && from->root == IR_SPLACE_HEAP;
            }
    for (int kind = 0; kind < N00BB_FLOW_KINDS; kind++) {
        if (escaped) out->escapes[kind] |= heap[kind];
        if (leaked) out->leaks[kind] |= heap[kind];
    }
}

static void
derive_unknown_writes(const ir_summary_t *summary, n00bb_flow_t *out)
{
    bool written = false;
    for (uint32_t i = 0; !written && i < ir_summary_path_count(summary); i++) {
        const ir_spath_t *path = ir_summary_path_at(summary, i);
        written = path->root == IR_SPLACE_UNKNOWN
            && (path->flags & IR_SPATH_WRITE);
    }
    if (!written) return;
    uint64_t self    = out->leaks[N00BB_FLOW_SELF];
    uint64_t reached = self | out->leaks[N00BB_FLOW_CONTENTS];
    out->mutates[N00BB_FLOW_SELF] |= self;
    out->writes[N00BB_FLOW_SELF] |= self;
    out->mutates[N00BB_FLOW_CONTENTS] |= reached;
    out->writes[N00BB_FLOW_CONTENTS] |= reached;
}

void
n00bb_summary_flow(const ir_summary_t *summary, n00bb_flow_t *out)
{
    const ir_func_t *function = summary ? ir_summary_function(summary)
                                        : nullptr;
    if (!function || !ir_summary_results_known(summary)
        || n00bb_native_calls_n00b(function)) {
        *out = flow_opaque();
        return;
    }
    *out = (n00bb_flow_t){0};
    out->writes[N00BB_FLOW_SELF] = axiom_writes(function);
    out->mutates[N00BB_FLOW_SELF] = axiom_writes(function);
    derive_writes(ir_module_types(function->owner), function, summary, out);
    derive_stores(function, summary, out);
    derive_unknown_writes(summary, out);
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
    analysis_t *analysis;
    ir_pto_t view;
    value_list_t tracked;
    pointer_map_t tracked_index;
    ir_live_t *liveness;
    bool failed;
} window_t;

static void
report_borrow(window_t *window, const ir_instr_t *offender,
              n00bb_text_t label)
{
    if (label == N00B_TEXT_COUNT) {
        borrow_internal(window->diagnostics, window->function,
            window->conversion, "the Buffer borrow analysis could not "
            "complete");
        return;
    }
    core_diagnostics_t *destination =
        ir_diags_destination(window->diagnostics);
    if (!destination) return;
    n00bb_report(destination, ir_diags_unit(window->diagnostics),
        N00B_DIAG_BUFFER_BORROW_OUTLIVED,
        borrow_location(window->conversion->span),
        N00B_TEXT_THIS_BUFFER_IS_USED_AS,
        N00BB_NO_ARGS);
    n00bb_report_label(destination, N00B_TEXT_BORROWED_AS_A_STRING_HERE,
        N00BB_NO_ARGS);
    if (offender && ir_span_known(offender->span))
        n00bb_report_secondary(destination, nullptr,
            borrow_location(offender->span), label, N00BB_NO_ARGS);
    n00bb_report_help(destination, N00B_TEXT_CONSUME_THE_BUFFER_WITH_TO,
        N00BB_NO_ARGS);
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

static void
track_holder(void *context, const ir_value_t *value, const ir_local_t *local)
{
    window_t *window = context;
    ir_value_t *held = local ? ir_local_value(window->function,
                                              (ir_local_t *)local)
                             : (ir_value_t *)value;
    if (!held || !track(window, held)) window->failed = true;
}

static bool
collect_tracked(window_t *window)
{
    ir_points_to_holders(window->analysis->points_to, &window->view,
                         track_holder, window);
    if (window->failed || !track(window, window->conversion->dest))
        return false;
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
    bool *after;
    uint32_t after_cap;
    ir_instr_t *offender;
    n00bb_text_t reason;
    ir_pto_list_t source;
    bool source_global;
    bool conditional;
} walk_t;

static th_type *
storage_type(const ir_pto_t *object)
{
    if (object->local) return object->local->type;
    if (object->value) return object->value->type;
    return object->instr && object->instr->dest ? object->instr->dest->type
                                                : nullptr;
}

static bool
root_is_buffer(analysis_t *analysis, const ir_pto_t *object)
{
    th_ctx *types = analysis->types;
    const ir_value_t *parameter = object_parameter(analysis, object);
    switch (object->kind) {
    case IR_PTO_PARAM:
        return !parameter || type_may_hold_buffer(types, parameter->type, 0);
    case IR_PTO_CONTENTS:
        return !parameter
            || contents_may_hold_buffer(types, parameter->type, 0);
    case IR_PTO_GLOBAL:
        return !guards_constant(analysis, object)
            || type_may_hold_buffer(types, object->global->type, 0);
    case IR_PTO_UNKNOWN:
        return true;
    case IR_PTO_STORAGE:
        return (object->instr && is_call(object->instr))
            || type_may_hold_buffer(types, storage_type(object), 0);
    }
    return true;
}

static bool
buffer_roots_meet(analysis_t *analysis, const ir_pto_list_t *left,
                  const ir_pto_list_t *right)
{
    for (size_t i = 0; i < left->len; i++)
        if (root_is_buffer(analysis, &left->data[i])
            && ir_pto_has(right, &left->data[i]))
            return true;
    return false;
}

static bool
roots_reach_global(analysis_t *analysis, const ir_pto_list_t *objects)
{
    for (size_t i = 0; i < objects->len; i++) {
        const ir_pto_t *object = &objects->data[i];
        if (!root_is_buffer(analysis, object)) continue;
        if (object->kind != IR_PTO_STORAGE || escaped(analysis, object))
            return true;
    }
    return false;
}

static void
blame(walk_t *walk, ir_instr_t *offender, n00bb_text_t reason)
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
    ir_pto_list_t objects = core_list_new(ir_pto_t);
    value_objects(window->analysis, nullptr, value, &objects);
    reach(window->analysis, &objects, false);
    bool carries = ir_pto_has(&objects, &window->view);
    core_list_free(objects);
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
    ir_value_t *callee = instruction->op == IR_OP_CALL
            && instruction->n_operands ? operand_at(instruction, 0) : nullptr;
    if ((!signature || th_kind_of(signature) != TH_FUNCTION) && callee
        && callee->kind == IR_VAL_FUNC)
        signature = function_signature(callee->owner);
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
        blame(walk, instruction, N00B_TEXT_BORROW_STORED);
    if (instruction->op == IR_OP_RETURN || instruction->op == IR_OP_FAIL) {
        uint32_t index = 0;
        for (uint32_t o = 0; o < instruction->n_operands; o++)
            if (tracked_index(window, operand_at(instruction, o), &index))
                blame(walk, instruction, instruction->op == IR_OP_RETURN
                      ? N00B_TEXT_BORROW_RETURNED
                      : N00B_TEXT_BORROW_FAILURE_VALUE);
        return true;
    }
    bool call = is_call(instruction);
    if (!call && !writes_memory(instruction)) return true;
    analysis_t *analysis = window->analysis;
    const n00bb_flow_t *effects = call
        ? call_effects(analysis, instruction) : nullptr;
    if (call && (!effects || effects->opaque)) {
        blame(walk, instruction, N00B_TEXT_BORROW_UNKNOWN_CALL_WRITES);
        return true;
    }
    ir_pto_list_t written = core_list_new(ir_pto_t);
    bool writes_nonlocal = false;
    instruction_write_roots(analysis, instruction, 0, &written, nullptr,
                            &writes_nonlocal);
    if (buffer_roots_meet(analysis, &written, &walk->source)
        || (walk->source_global
            && (writes_nonlocal || roots_reach_global(analysis, &written))))
        blame(walk, instruction, call
              ? N00B_TEXT_BORROW_CALL_WRITES
              : N00B_TEXT_BORROW_STORE_WRITES);
    core_list_free(written);
    return true;
}

static bool
walk_block(walk_t *walk, ir_block_t *block, uint32_t start,
           bool *reaches_end)
{
    window_t *window = walk->window;
    if (block->n_instrs > walk->after_cap) {
        bool *after = realloc(walk->after,
                              (size_t)block->n_instrs * sizeof(*after));
        if (!after) return false;
        walk->after = after;
        walk->after_cap = block->n_instrs;
    }
    if (!ir_live_set_after_each(window->liveness, window->tracked.items,
                                window->tracked.len, block, walk->after))
        return false;
    ir_instr_t *terminator = ir_block_terminator(block);
    for (uint32_t i = start; i < block->n_instrs; i++) {
        ir_instr_t *instruction = block->instrs[i];
        if (!check_instruction(walk, instruction)) return false;
        if (instruction == terminator) break;
        if (!walk->after[i]) {
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
    walk_frame_t *stack = calloc(count + 1, sizeof(*stack));
    bool ok = walk->state && walk->entered && walk->continued && stack;
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
        if (!ir_live_set_on_edge(window->liveness, window->tracked.items,
                                 window->tracked.len, frame->block,
                                 successor)) {
            ok = add_end_site(&walk->sites, (end_site_t){
                .from = frame->block, .to = successor,
            });
            continue;
        }
        walk->continued[(size_t)successor->index * count
                        + frame->block->index] = true;
        if (walk->state[successor->index] == BLOCK_ACTIVE
            || successor == origin) {
            blame(walk, terminator, N00B_TEXT_BORROW_LIVE_AROUND_LOOP);
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
    const ir_instr_t *where = nullptr;
    if (!ir_points_to_escapes(window->analysis->points_to, &window->view,
                              &where))
        return;
    if (where) blame(walk, (ir_instr_t *)where, N00B_TEXT_BORROW_KEPT_HERE);
    else blame(walk, window->conversion, N00B_TEXT_BORROW_MAY_BE_KEPT);
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
                  analysis_t *analysis, ir_instr_t *conversion)
{
    window_t window = {
        .diagnostics = diagnostics,
        .function = function,
        .conversion = conversion,
        .buffer = conversion->n_operands == 1
            ? operand_at(conversion, 0) : nullptr,
        .analysis = analysis,
        .view = {.kind = IR_PTO_STORAGE, .instr = conversion},
    };
    walk_t walk = {.window = &window, .source = core_list_new(ir_pto_t)};
    bool ok = window.buffer && conversion->dest
        && collect_tracked(&window)
        && (window.liveness = ir_func_liveness(function));
    if (ok) {
        value_objects(analysis, conversion, window.buffer, &walk.source);
        reach(analysis, &walk.source, false);
        walk.source_global = roots_reach_global(analysis, &walk.source);
        check_escapes(&window, &walk);
        if (!walk.offender) ok = walk_window(&walk);
    }
    if (ok && walk.offender)
        report_borrow(&window, walk.offender, walk.reason);
    else if (ok && walk.conditional)
        ok = capture_buffer(&window) && insert_held_ends(&window, &walk.sites);
    else if (ok)
        ok = capture_buffer(&window)
            && insert_ends(&window, &walk.sites, nullptr);
    if (!ok)
        report_borrow(&window, nullptr, N00B_TEXT_COUNT);
    bool accepted = ok && !walk.offender;
    free(walk.sites.items);
    free(walk.state);
    free(walk.entered);
    free(walk.continued);
    free(walk.after);
    core_list_free(walk.source);
    free(window.tracked.items);
    pointer_map_free(&window.tracked_index);
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
borrow_function(ir_diags_t *diagnostics, analysis_t *analysis, size_t count)
{
    ir_instr_t **conversions = calloc(count, sizeof(*conversions));
    if (!conversions) return false;
    collect_conversions(analysis->function, conversions);
    bool ok = analysis_solve(analysis);
    bool accepted = ok;
    for (size_t c = 0; ok && c < count; c++)
        if (!borrow_conversion(diagnostics, analysis->function, analysis,
                               conversions[c]))
            accepted = false;
    if (!ok)
        borrow_internal(diagnostics, analysis->function, nullptr,
                        "the Buffer borrow analysis could not complete");
    free(conversions);
    return accepted;
}

static uint64_t
view_inputs(th_type *signature)
{
    uint64_t views = 0;
    uint32_t inputs = 0;
    for (size_t i = 0; signature && i < th_func_nparams(signature); i++) {
        const th_param *formal = th_func_param(signature, i);
        if (!formal || !th_mode_accepts_input(formal->mode)) continue;
        if (formal->mode == TH_VIEW && inputs < 64)
            views |= UINT64_C(1) << inputs;
        inputs++;
    }
    return views;
}

static uint64_t
lent_array_inputs(th_type *signature)
{
    uint64_t arrays = 0;
    uint32_t inputs = 0;
    for (size_t i = 0; signature && i < th_func_nparams(signature); i++) {
        const th_param *formal = th_func_param(signature, i);
        if (!formal || !th_mode_accepts_input(formal->mode)) continue;
        th_type *type = formal->type ? th_resolve(formal->type) : nullptr;
        uint64_t extent = 0;
        if (formal->mode == TH_IN && inputs < 64 && type
            && th_kind_of(type) == TH_SEQUENCE
            && !th_seq_extent(type, &extent))
            arrays |= UINT64_C(1) << inputs;
        inputs++;
    }
    return arrays;
}

static uint64_t
mutable_view_inputs(th_ctx *types, th_type *signature)
{
    uint64_t views = view_inputs(signature);
    uint32_t inputs = 0;
    for (size_t i = 0; signature && i < th_func_nparams(signature); i++) {
        const th_param *formal = th_func_param(signature, i);
        if (!formal || !th_mode_accepts_input(formal->mode)) continue;
        if (inputs < 64
            && !n00bb_type_reaches_shared_state(types, formal->type))
            views &= ~(UINT64_C(1) << inputs);
        inputs++;
    }
    return views;
}

static uint64_t
call_view_inputs(const ir_instr_t *call)
{
    ir_value_t *callee = call->op == IR_OP_CALL && call->n_operands
        ? operand_at(call, 0) : nullptr;
    th_type *type = callee && callee->type ? th_resolve(callee->type)
                                           : nullptr;
    for (uint32_t depth = 0; type && depth < 8
         && th_kind_of(type) == TH_INDIRECTION; depth++)
        type = th_resolve(th_ind_referent(type));
    return type && th_kind_of(type) == TH_FUNCTION ? view_inputs(type) : 0;
}

static bool
references_constant(th_ctx *types, const ir_func_t *function)
{
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            const ir_instr_t *instruction = function->blocks[b]->instrs[i];
            for (uint32_t o = 0; o < instruction->n_operands; o++) {
                ir_value_t *operand = operand_at(instruction, o);
                if (is_constant_global(operand) && operand->type
                    && n00bb_type_reaches_shared_state(types, operand->type))
                    return true;
            }
        }
    return false;
}

typedef struct {
    const ir_value_t  *parameter;
    const ir_global_t *global;
} guarded_t;

static guarded_t
guarded_root(analysis_t *analysis, const ir_pto_t *object, uint64_t views)
{
    const ir_value_t *parameter = object_parameter(analysis, object);
    if (parameter && parameter->index < 64 && views >> parameter->index & 1)
        return (guarded_t){.parameter = parameter};
    if (guards_constant(analysis, object))
        return (guarded_t){.global = object->global};
    return (guarded_t){0};
}

static void
report_guarded_write(ir_diags_t *diagnostics, ir_func_t *function,
                     const ir_instr_t *offender, guarded_t guarded)
{
    core_diagnostics_t *destination = ir_diags_destination(diagnostics);
    if (!destination) return;
    bool constant = guarded.global;
    core_string_t name = source_name_of(constant ? guarded.global->name
                                                 : guarded.parameter->name);
    bool call = offender && is_call(offender);
    n00bb_text_t message = constant
        ? call ? N00B_TEXT_CALL_MAY_WRITE_CONSTANT : N00B_TEXT_WRITES_CONSTANT
        : call ? N00B_TEXT_CALL_MAY_WRITE_VIEW : N00B_TEXT_WRITES_VIEW;
    n00bb_report(destination, ir_diags_unit(diagnostics), N00B_DIAG_VIEW_WRITE,
        borrow_location(offender ? offender->span : function->span), message,
        N00BB_ARGS(ARG_TEXT(name)));
    if (constant && ir_span_known(guarded.global->span))
        n00bb_report_secondary(destination, nullptr,
            borrow_location(guarded.global->span), N00B_TEXT_IS_DECLARED_HERE,
            N00BB_ARGS(ARG_TEXT(name)));
    n00bb_report_help(destination, constant
        ? N00B_TEXT_WRITE_TO_A_COPY : N00B_TEXT_TAKE_IT_AS_MODIFY,
        N00BB_NO_ARGS);
}

static const ir_value_t *
view_reference(analysis_t *analysis, const ir_instr_t *at, ir_value_t *value,
               bool contents, uint64_t views, uint64_t arrays)
{
    ir_pto_list_t reached = core_list_new(ir_pto_t);
    if (contents)
        ir_points_to_loaded(analysis->points_to, at, value, &reached);
    else value_objects(analysis, at, value, &reached);
    const ir_value_t *found = nullptr;
    for (size_t r = 0; arrays && !found && r < reached.len; r++) {
        const ir_value_t *parameter = object_parameter(analysis,
                                                       &reached.data[r]);
        if (parameter && parameter->index < 64
            && arrays >> parameter->index & 1)
            found = parameter;
    }
    reach(analysis, &reached, false);
    for (size_t r = 0; !found && r < reached.len; r++) {
        const ir_value_t *parameter = object_parameter(analysis,
                                                       &reached.data[r]);
        if (parameter && parameter->index < 64
            && views >> parameter->index & 1)
            found = parameter;
    }
    core_list_free(reached);
    return found;
}

static bool
outlives_call(analysis_t *analysis, const ir_instr_t *at,
              ir_value_t *address)
{
    ir_pto_list_t objects = core_list_new(ir_pto_t);
    value_objects(analysis, at, address, &objects);
    bool outlives = false;
    for (size_t r = 0; !outlives && r < objects.len; r++)
        outlives = !is_location(&objects.data[r]);
    core_list_free(objects);
    return outlives;
}

static const ir_value_t *
escaped_view(analysis_t *analysis, ir_instr_t *instruction, uint64_t views,
             uint64_t arrays)
{
    if (is_call(instruction)) {
        const n00bb_flow_t *effects = call_effects(analysis, instruction);
        bool opaque = !effects || effects->opaque;
        uint32_t first = first_argument(instruction, effects);
        uint64_t trusted = call_view_inputs(instruction);
        const ir_value_t *found = nullptr;
        for (uint32_t i = first; !found && i < instruction->n_operands; i++) {
            uint32_t index = i - first;
            if (index < 64 && trusted >> index & 1) continue;
            bool kept = !opaque
                && (effect_bit(effects->escapes, index, N00BB_FLOW_SELF)
                    || effect_bit(effects->leaks, index, N00BB_FLOW_SELF));
            bool escapes = opaque || kept
                || effect_bit(effects->escapes, index, N00BB_FLOW_CONTENTS)
                || effect_bit(effects->leaks, index, N00BB_FLOW_CONTENTS);
            if (escapes)
                found = view_reference(analysis, instruction,
                                       operand_at(instruction, i), false,
                                       views, kept ? arrays : 0);
        }
        return found;
    }
    if (!ir_op_transfers(instruction->op)) return nullptr;
    int32_t  source   = ir_op_source_operand(instruction->op);
    uint32_t value_at = (uint32_t)(source >= 0
        ? source : ir_op_stored_operand(instruction->op));
    uint32_t address  = (uint32_t)ir_op_address_operand(instruction->op);
    if (instruction->n_operands <= value_at) return nullptr;
    if (instruction->op != IR_OP_GLOBAL_STORE
        && !outlives_call(analysis, instruction,
                          operand_at(instruction, address)))
        return nullptr;
    return view_reference(analysis, instruction,
                          operand_at(instruction, value_at), source >= 0,
                          views, source >= 0 ? 0 : arrays);
}

static void
report_view_escape(ir_diags_t *diagnostics, const ir_instr_t *offender,
                   const ir_value_t *parameter, bool array)
{
    core_diagnostics_t *destination = ir_diags_destination(diagnostics);
    if (!destination) return;
    n00bb_text_t message = array
        ? is_call(offender) ? N00B_TEXT_CALL_MAY_KEEP_ARRAY
                            : N00B_TEXT_STORES_ARRAY
        : is_call(offender) ? N00B_TEXT_CALL_MAY_KEEP_VIEW
                            : N00B_TEXT_STORES_VIEW;
    n00bb_report(destination, ir_diags_unit(diagnostics),
        array ? N00B_DIAG_ARRAY_KEPT : N00B_DIAG_VIEW_ESCAPE,
        borrow_location(offender->span), message,
        N00BB_ARGS(ARG_TEXT(parameter->name)));
    const ir_value_t *target = offender->n_operands
        && offender->operands[0].kind == IR_OPD_VALUE
        ? offender->operands[0].value : nullptr;
    if (!is_call(offender) && target && target->global
        && ir_span_known(target->global->span)) {
        core_string_t name = source_name_of(target->global->name);
        n00bb_report_secondary(destination, nullptr,
            borrow_location(target->global->span), N00B_TEXT_OUTLIVES_THE_CALL,
            N00BB_ARGS(ARG_TEXT(name)));
    }
    else if (!is_call(offender) && target && target->def
             && ir_span_known(target->def->span))
        n00bb_report_secondary(destination, nullptr,
            borrow_location(target->def->span),
            N00B_TEXT_THIS_PLACE_OUTLIVES_THE_CALL,
            N00BB_NO_ARGS);
    if (array)
        n00bb_report_help(destination, N00B_TEXT_CONSUME_OR_COPY_ARRAY,
            N00BB_ARGS(ARG_TEXT(parameter->name)));
    else
        n00bb_report_help(destination, N00B_TEXT_STORE_A_COPY_OR_RETURN,
            N00BB_NO_ARGS);
}

static bool
guard_function(ir_diags_t *diagnostics, analysis_t *analysis, uint64_t views,
               uint64_t arrays)
{
    ir_func_t *function = analysis->function;
    if (!analysis_solve(analysis)) {
        borrow_internal(diagnostics, function, nullptr,
                        "the write analysis could not complete");
        return false;
    }
    bool accepted = true;
    ir_pto_list_t mutated = core_list_new(ir_pto_t);
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            bool nonlocal = false;
            mutated.len = 0;
            uint64_t trusted = is_call(instruction)
                ? call_view_inputs(instruction) : 0;
            if (!is_call(instruction)
                || !ir_points_to_call_writes(analysis->points_to,
                                             instruction, trusted, &mutated))
                instruction_write_roots(analysis, instruction, trusted,
                                        nullptr, &mutated, &nonlocal);
            guarded_t guarded = {0};
            for (size_t r = 0; !guarded.parameter && !guarded.global
                 && r < mutated.len; r++)
                guarded = guarded_root(analysis, &mutated.data[r], views);
            if (guarded.parameter || guarded.global) {
                report_guarded_write(diagnostics, function, instruction,
                                     guarded);
                accepted = false;
                continue;
            }
            const ir_value_t *escaped = views | arrays
                ? escaped_view(analysis, instruction, views, arrays)
                : nullptr;
            if (escaped) {
                report_view_escape(diagnostics, instruction, escaped,
                    escaped->index < 64 && arrays >> escaped->index & 1);
                accepted = false;
            }
        }
    }
    core_list_free(mutated);
    return accepted;
}

bool
n00bb_buffer_borrow_pass(ir_module_t *module, ir_summaries_t *summaries,
                         ir_diags_t *diagnostics)
{
    th_ctx *types = ir_module_types(module);
    flow_cache_t flows = {.types = types};
    bool ok = true;
    bool shared_constants = false;
    for (size_t g = 0; !shared_constants && g < ir_global_count(module); g++) {
        ir_global_t *global = ir_global_at(module, g);
        shared_constants = global && global->mutability == IR_MUT_CONST
            && n00bb_type_reaches_shared_state(types, global->type);
    }
    for (size_t f = 0; f < ir_func_count(module); f++) {
        ir_func_t *function = ir_func_at(module, f);
        analysis_t analysis = {
            .function = function, .types = types, .flows = &flows,
            .summaries = summaries,
        };
        size_t count = collect_conversions(function, nullptr);
        if (count && !borrow_function(diagnostics, &analysis, count))
            ok = false;
        ir_points_to_free(analysis.points_to);
        analysis.points_to = nullptr;
        uint64_t views = mutable_view_inputs(types,
                                             function_signature(function));
        uint64_t arrays = lent_array_inputs(function_signature(function));
        analysis.constants = shared_constants
            && references_constant(types, function);
        if ((views || arrays || analysis.constants)
            && !guard_function(diagnostics, &analysis, views, arrays))
            ok = false;
        ir_points_to_free(analysis.points_to);
    }
    flow_cache_free(&flows);
    return ok;
}
