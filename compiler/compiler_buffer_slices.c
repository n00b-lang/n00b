#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler_axioms.h"
#include "compiler_buffer_slices.h"
#include "compiler_tu.h"
#include "cached_body.h"
#include "diagnostics.h"
#include "sequence_transfer.h"

#include "ir/doms.h"
#include "ir/liveness.h"
#include "ir/pass.h"

static core_diag_location_t
slice_location(ir_span_t span)
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

static void
slice_internal(ir_diags_t *diagnostics, ir_func_t *function,
               const ir_instr_t *at, const char *what)
{
    core_diagnostics_t *destination = ir_diags_destination(diagnostics);
    if (!destination) return;
    n00bb_internal_error(destination, ir_diags_unit(diagnostics),
        slice_location(at ? at->span : function->span),
        "placing Buffer slice ends", "%s in `%.*s`", what,
        (int)function->name.u8_bytes, function->name.data);
}

static const core_string_t buffer_state_type_name =
    CORE_STRING_STATIC("builtin.containers.buffer.Buffer"
                       n00bb_object_shape_suffix);

static const core_string_t buffer_slice_pass =
    CORE_STRING_STATIC("n00bb.buffer-slice");

static const core_string_t slice_copy_key =
    CORE_STRING_STATIC(N00BB_SLICE_COPY_ANNOTATION);

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
} analysis_t;

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
    value_list_t tracked;
    pointer_map_t tracked_index;
    ir_live_t *liveness;
    ir_local_t *locals[64];
    uint32_t local_count;
    uint64_t *local_live_in;
} window_t;

static uint64_t
local_bits_read(const window_t *window, const ir_instr_t *instruction)
{
    uint64_t bits = 0;
    for (uint32_t o = 0; o < instruction->n_operands; o++) {
        ir_local_t *local = ir_operand_local(&instruction->operands[o]);
        for (uint32_t l = 0; local && l < window->local_count; l++)
            if (window->locals[l] == local) bits |= UINT64_C(1) << l;
    }
    return bits;
}

static uint64_t
local_bits_written(const window_t *window, const ir_instr_t *instruction)
{
    uint64_t bits = 0;
    for (uint32_t l = 0; instruction->dest_kind == IR_DEST_LOCAL
         && l < window->local_count; l++)
        if (window->locals[l] == instruction->dest_local)
            bits |= UINT64_C(1) << l;
    return bits;
}

static uint64_t
local_live_out(const window_t *window, const ir_block_t *block)
{
    ir_instr_t *terminator = ir_block_terminator((ir_block_t *)block);
    uint64_t live = 0;
    for (uint32_t s = 0; terminator && s < ir_succ_count(terminator); s++) {
        ir_block_t *successor = ir_succ_at(terminator, s);
        if (successor) live |= window->local_live_in[successor->index];
    }
    return live;
}

static bool
local_liveness(window_t *window)
{
    if (!window->local_count) return true;
    ir_func_t *function = window->function;
    window->local_live_in = calloc(function->n_blocks ? function->n_blocks
                                                      : 1, sizeof(uint64_t));
    if (!window->local_live_in) return false;
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t b = function->n_blocks; b-- > 0;) {
            ir_block_t *block = function->blocks[b];
            uint64_t live = local_live_out(window, block);
            for (uint32_t i = block->n_instrs; i-- > 0;)
                live = (live & ~local_bits_written(window, block->instrs[i]))
                    | local_bits_read(window, block->instrs[i]);
            if (live != window->local_live_in[b]) {
                window->local_live_in[b] = live;
                changed = true;
            }
        }
    }
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
output_carries_param(const ir_summary_t *summary, uint8_t output,
                     uint32_t param)
{
    static const ir_sflow_kind_t kinds[] = {
        IR_SFLOW_DATA, IR_SFLOW_COPY, IR_SFLOW_ALIAS,
    };
    uint32_t flows = ir_summary_flow_count(summary, IR_SFLOW_RESULTS);
    uint32_t sources = ir_summary_flow_source_count(summary,
                                                    IR_SFLOW_RESULTS);
    for (uint32_t f = 0; f < flows; f++) {
        const ir_spath_t *to = ir_summary_flow_to(summary, IR_SFLOW_RESULTS,
                                                  f);
        if (!to || to->output != output) continue;
        for (uint32_t s = 0; s < sources; s++) {
            const ir_spath_t *from = ir_summary_flow_source(summary,
                IR_SFLOW_RESULTS, s);
            if (!from || from->root != IR_SPLACE_PARAM
                || from->param != param) continue;
            for (size_t k = 0; k < sizeof(kinds) / sizeof(*kinds); k++)
                if (ir_summary_flow_has(summary, IR_SFLOW_RESULTS, f, s,
                                        kinds[k])) return true;
        }
    }
    return false;
}

static bool
output_holds_tracked(window_t *window, const ir_instr_t *output)
{
    size_t ordinal = 0;
    const ir_instr_t *call = ir_call_output_source(output, &ordinal);
    callee_summary_t callee = {0};
    if (!call || ordinal + 1 >= IR_SOUTPUT_STATUS
        || !ir_summary_call_callees(window->analysis->summaries, call,
                                    collect_callee_summary, &callee)
        || !callee.summary) return false;
    for (uint32_t o = 1; o < call->n_operands; o++) {
        uint32_t index = 0;
        ir_value_t *argument = operand_at(call, o);
        if (argument && tracked_index(window, argument, &index)
            && output_carries_param(callee.summary, (uint8_t)(ordinal + 1),
                                    o - 1)) return true;
    }
    return false;
}

static bool
collect_tracked(window_t *window)
{
    if (!track(window, window->conversion->dest)) return false;
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
    bool ok = true;
    for (bool changed = true; ok && changed;) {
        changed = false;
        for (uint32_t b = 0; ok && b < window->function->n_blocks; b++) {
            ir_block_t *block = window->function->blocks[b];
            for (uint32_t i = 0; ok && i < block->n_instrs; i++) {
                ir_instr_t *copy = block->instrs[i];
                uint32_t index = 0;
                if (copy->op == IR_OP_CALL_OUTPUT && copy->dest
                    && !tracked_index(window, copy->dest, &index)
                    && output_holds_tracked(window, copy)) {
                    ir_value_t *source = operand_at(copy, 0);
                    ok = track(window, copy->dest)
                        && (!source || track(window, source));
                    changed = true;
                    continue;
                }
                if (copy->op != IR_OP_COPY || copy->n_operands != 1) continue;
                ir_value_t *value = operand_at(copy, 0);
                if (copy->dest_kind == IR_DEST_LOCAL && copy->dest_local
                    && value && tracked_index(window, value, &index)
                    && !local_bits_written(window, copy)) {
                    ok = window->local_count
                        < sizeof(window->locals) / sizeof(*window->locals);
                    if (ok) window->locals[window->local_count++] =
                        copy->dest_local;
                    changed = true;
                }
                if (ok && copy->dest && local_bits_read(window, copy)
                    && !tracked_index(window, copy->dest, &index)) {
                    ok = track(window, copy->dest);
                    changed = true;
                }
            }
        }
    }
    return ok && local_liveness(window);
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
    int length = snprintf(name, sizeof(name), "__n00bb_slice_edge_%u_%u",
                          from->index, to->index);
    if (length < 0 || (size_t)length >= sizeof(name)) return nullptr;
    ir_block_t *middle = ir_block_add(function,
        core_string_borrow(name, (size_t)length));
    if (!middle) return nullptr;
    ir_instr_t *jump = ir_instr_generated(middle, IR_OP_JUMP, span,
        buffer_slice_pass, 0);
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
hold(window_t *window, held_t *held, const char *what, ir_value_t *value)
{
    ir_func_t *function = window->function;
    held->type = th_option(ir_module_types(function->owner), value->type);
    char name[64];
    int length = snprintf(name, sizeof(name), "__n00bb_slice_%s_%u", what,
                          function->n_locals);
    if (!held->type || length < 0 || (size_t)length >= sizeof(name))
        return false;
    held->local = ir_local_add(function,
        core_string_borrow(name, (size_t)length), held->type);
    return held->local;
}

static bool
insert_held_assignment(window_t *window, held_t *held, ir_block_t *block,
                       uint32_t at, ir_value_t *value)
{
    char name[64];
    int length = snprintf(name, sizeof(name), "__n00bb_slice_held_%u_%u",
                          held->local->index, held->serial++);
    if (length < 0 || (size_t)length >= sizeof(name)) return false;
    ir_instr_t *wrap = ir_instr_generated(block,
        value ? IR_OP_OPTION_SOME : IR_OP_OPTION_NONE,
        window->conversion->span, buffer_slice_pass, at);
    if (!wrap || !ir_instr_set_dest(wrap,
            core_string_borrow(name, (size_t)length), held->type)
        || (value && !ir_instr_operand_value(wrap, value))) return false;
    ir_instr_t *copy = ir_instr_generated(block, IR_OP_COPY,
        window->conversion->span, buffer_slice_pass, at + 1);
    return copy && ir_instr_set_dest_local(copy, held->local)
        && ir_instr_operand_value(copy, wrap->dest);
}

static bool
insert_end(window_t *window, ir_block_t *block, uint32_t at, held_t *held)
{
    ir_instr_t *end = ir_instr_generated(block, IR_OP_BUFFER_SLICE_END,
        window->conversion->span, buffer_slice_pass, at);
    if (!end) return false;
    if (!held)
        return ir_instr_operand_value(end, window->buffer)
            && ir_instr_operand_value(end, window->conversion->dest);
    return ir_instr_operand_value(end,
            ir_local_value(window->function, held[0].local))
        && ir_instr_operand_value(end,
            ir_local_value(window->function, held[1].local))
        && insert_held_assignment(window, &held[0], block, at + 1, nullptr)
        && insert_held_assignment(window, &held[1], block, at + 3, nullptr);
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
    held_t held[2] = {0};
    ir_block_t *entry = ir_block_entry(window->function);
    if (!entry || !hold(window, &held[0], "buffer", window->buffer)
        || !hold(window, &held[1], "text", window->conversion->dest)
        || !insert_held_assignment(window, &held[0], entry,
                                   first_non_phi(entry), nullptr)
        || !insert_held_assignment(window, &held[1], entry,
                                   first_non_phi(entry), nullptr))
        return false;
    uint32_t after = instruction_position(window->conversion) + 1;
    return insert_held_assignment(window, &held[0],
               window->conversion->block, after, window->buffer)
        && insert_held_assignment(window, &held[1],
               window->conversion->block, after + 2,
               window->conversion->dest)
        && insert_ends(window, sites, held);
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
    bool kept;
    bool conditional;
} walk_t;

static void
keep(walk_t *walk)
{
    walk->kept = true;
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
keeps_parameter(const uint64_t *bits, uint32_t parameter)
{
    return parameter >= 64 ? bits[N00BB_FLOW_SELF] == UINT64_MAX
                           : (bits[N00BB_FLOW_SELF] >> parameter) & 1;
}

static bool
check_instruction(walk_t *walk, ir_instr_t *instruction)
{
    window_t *window = walk->window;
    if (stores_into_aggregate(window, instruction)) keep(walk);
    if (instruction->op == IR_OP_RETURN || instruction->op == IR_OP_FAIL) {
        uint32_t index = 0;
        for (uint32_t o = 0; o < instruction->n_operands; o++)
            if (tracked_index(window, operand_at(instruction, o), &index))
                keep(walk);
        return true;
    }
    if (!is_call(instruction)) return true;
    const n00bb_flow_t *effects = call_effects(window->analysis, instruction);
    for (uint32_t p = 0; p < ir_call_argument_count(instruction); p++) {
        uint32_t index = 0;
        if (!tracked_index(window, ir_call_argument(instruction, p).value,
                           &index)) continue;
        if (!effects || effects->opaque
            || keeps_parameter(effects->escapes, p)
            || keeps_parameter(effects->leaks, p))
            keep(walk);
    }
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
    uint64_t held = window->local_count ? local_live_out(window, block) : 0;
    for (uint32_t i = block->n_instrs; window->local_count && i-- > 0;) {
        walk->after[i] = walk->after[i] || held;
        held = (held & ~local_bits_written(window, block->instrs[i]))
            | local_bits_read(window, block->instrs[i]);
    }
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
    for (uint32_t b = 0; b < count && !walk->kept; b++) {
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
    while (ok && depth && !walk->kept) {
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
                                 successor)
            && !(window->local_live_in
                 && window->local_live_in[successor->index])) {
            ok = add_end_site(&walk->sites, (end_site_t){
                .from = frame->block, .to = successor,
            });
            continue;
        }
        walk->continued[(size_t)successor->index * count
                        + frame->block->index] = true;
        if (walk->state[successor->index] == BLOCK_ACTIVE
            || successor == origin) {
            keep(walk);
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
capture_buffer(window_t *window)
{
    ir_value_t *buffer = window->buffer;
    ir_instr_t *conversion = window->conversion;
    if (buffer->kind != IR_VAL_GLOBAL && buffer->kind != IR_VAL_LOCAL)
        return true;
    uint32_t at = instruction_position(conversion);
    char name[64];
    int length = snprintf(name, sizeof(name), "__n00bb_slice_source_%u_%u",
                          conversion->block->index, at);
    if (length < 0 || (size_t)length >= sizeof(name)) return false;
    ir_instr_t *copy = ir_instr_generated(conversion->block, IR_OP_COPY,
        conversion->span, buffer_slice_pass, at);
    if (!copy || !ir_instr_set_dest(copy,
            core_string_borrow(name, (size_t)length), buffer->type)
        || !ir_instr_operand_value(copy, buffer)) return false;
    conversion->operands[0] = (ir_operand_t){
        .kind = IR_OPD_VALUE, .value = copy->dest,
    };
    ir_instr_use_added(conversion, 0);
    window->buffer = copy->dest;
    return true;
}

static bool
slice_conversion(ir_diags_t *diagnostics, ir_func_t *function,
                 analysis_t *analysis, ir_instr_t *conversion)
{
    window_t window = {
        .diagnostics = diagnostics,
        .function = function,
        .conversion = conversion,
        .buffer = conversion->n_operands == 1
            ? operand_at(conversion, 0) : nullptr,
        .analysis = analysis,
    };
    walk_t walk = {.window = &window};
    bool ok = window.buffer && conversion->dest
        && collect_tracked(&window)
        && (window.liveness = ir_func_liveness(function))
        && walk_window(&walk);
    if (ok && !walk.kept && walk.conditional)
        ok = capture_buffer(&window) && insert_held_ends(&window, &walk.sites);
    else if (ok && !walk.kept)
        ok = capture_buffer(&window)
            && insert_ends(&window, &walk.sites, nullptr);
    else if (ok)
        ok = ir_annot_set(ir_module_ctx(function->owner), &conversion->annots,
            (ir_annot_t){.key = slice_copy_key, .kind = IR_ANNOT_BOOL,
                         .b = true});
    if (!ok)
        slice_internal(diagnostics, function, conversion,
                       "the end of a Buffer slice could not be placed");
    bool accepted = ok;
    free(walk.sites.items);
    free(walk.state);
    free(walk.entered);
    free(walk.continued);
    free(walk.after);
    free(window.tracked.items);
    free(window.local_live_in);
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
slice_function(ir_diags_t *diagnostics, analysis_t *analysis, size_t count)
{
    ir_instr_t **conversions = calloc(count, sizeof(*conversions));
    if (!conversions) return false;
    collect_conversions(analysis->function, conversions);
    bool accepted = true;
    for (size_t c = 0; c < count; c++)
        if (!slice_conversion(diagnostics, analysis->function, analysis,
                              conversions[c]))
            accepted = false;
    free(conversions);
    return accepted;
}

bool
n00bb_buffer_slice_pass(ir_module_t *module, ir_summaries_t *summaries,
                        ir_diags_t *diagnostics)
{
    th_ctx *types = ir_module_types(module);
    flow_cache_t flows = {.types = types};
    bool ok = true;
    for (size_t f = 0; f < ir_func_count(module); f++) {
        ir_func_t *function = ir_func_at(module, f);
        analysis_t analysis = {
            .function = function, .types = types, .flows = &flows,
            .summaries = summaries,
        };
        size_t count = collect_conversions(function, nullptr);
        if (count && !slice_function(diagnostics, &analysis, count))
            ok = false;
    }
    flow_cache_free(&flows);
    return ok;
}
