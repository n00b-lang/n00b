#include <stdlib.h>

#include "compiler_buffer_borrow.h"

#include "ir/doms.h"
#include "ir/pass.h"

typedef struct {
    ir_lifetime_endpoint_t *items;
    size_t len;
    size_t cap;
    bool failed;
} borrow_endpoints_t;

static const char borrow_diagnostic_text[] =
    "this use of a Buffer as a string outlives a safe borrow; consume it "
    "with to_string(b), or keep b with to_string(buffer_from_string(b))";

static void
report_borrow(ir_diags_t *diagnostics, ir_func_t *function,
              const ir_instr_t *site, const char *label)
{
    ir_diag_t *diagnostic = ir_diag_begin_at(diagnostics,
        IR_DIAG_VERIFY_LIFETIME, site, site->block, function, label);
    ir_diag_message(diagnostic, diagnostics, borrow_diagnostic_text);
}

static bool
collect_endpoint(void *opaque, ir_lifetime_endpoint_t endpoint)
{
    borrow_endpoints_t *endpoints = opaque;
    if (endpoints->len == endpoints->cap) {
        size_t cap = endpoints->cap ? endpoints->cap * 2 : 4;
        ir_lifetime_endpoint_t *grown = realloc(endpoints->items,
            cap * sizeof(*grown));
        if (!grown) {
            endpoints->failed = true;
            return false;
        }
        endpoints->items = grown;
        endpoints->cap = cap;
    }
    endpoints->items[endpoints->len++] = endpoint;
    return true;
}

static uint32_t
instruction_position(const ir_instr_t *instruction)
{
    ir_block_t *block = instruction->block;
    for (uint32_t i = 0; i < block->n_instrs; i++)
        if (block->instrs[i] == instruction) return i;
    return block->n_instrs;
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

static uint32_t
first_non_phi(const ir_block_t *block)
{
    uint32_t at = 0;
    while (at < block->n_instrs && block->instrs[at]->op == IR_OP_PHI) at++;
    return at;
}

static bool
insert_borrow_end(ir_block_t *block, uint32_t at, ir_value_t *buffer,
                  ir_span_t span)
{
    ir_instr_t *end = ir_instr_generated(block, IR_OP_BUFFER_BORROW_END,
        span, "n00bb.buffer-borrow", at);
    return end && ir_instr_operand_value(end, buffer);
}

static bool
place_endpoint(ir_func_t *function, ir_lifetime_endpoint_t endpoint,
               ir_value_t *buffer, ir_span_t span)
{
    if (endpoint.point) {
        ir_instr_t *point = endpoint.point;
        uint32_t at = instruction_position(point);
        return insert_borrow_end(point->block,
            ir_op_is_terminator(point->op) ? at : at + 1, buffer, span);
    }
    if (!endpoint.from || !endpoint.to) return false;
    if (predecessor_count(function, endpoint.to) == 1)
        return insert_borrow_end(endpoint.to, first_non_phi(endpoint.to),
                                 buffer, span);
    ir_instr_t *terminator = ir_block_terminator(endpoint.from);
    if (terminator && ir_succ_count(terminator) == 1)
        return insert_borrow_end(endpoint.from,
            instruction_position(terminator), buffer, span);
    return false;
}

static bool
borrow_conversion(ir_diags_t *diagnostics, ir_func_t *function,
                  ir_instr_t *conversion)
{
    ir_value_t *view = conversion->dest;
    ir_value_t *buffer = conversion->n_operands == 1
        ? ir_operand_value(&conversion->operands[0]) : nullptr;
    if (!view || !buffer) return false;
    if (ir_value_alias_liveness_after(view, conversion)
            == IR_LIVENESS_UNKNOWN) {
        report_borrow(diagnostics, function, conversion,
            "the string escapes or is stored beyond the borrow");
        return false;
    }
    borrow_endpoints_t endpoints = {0};
    ir_liveness_query_t *query = ir_liveness_query_new(view);
    bool ok = query && ir_liveness_query_visit_endpoints(query,
        collect_endpoint, &endpoints) && !endpoints.failed;
    ir_liveness_query_free(query);
    if (ok && !endpoints.len)
        ok = insert_borrow_end(conversion->block,
            instruction_position(conversion) + 1, buffer, conversion->span);
    for (size_t i = 0; ok && i < endpoints.len; i++)
        ok = place_endpoint(function, endpoints.items[i], buffer,
                            conversion->span);
    free(endpoints.items);
    if (!ok)
        report_borrow(diagnostics, function, conversion,
            "the end of the borrow cannot be placed on every path");
    return ok;
}

bool
n00bb_buffer_borrow_pass(ir_module_t *module, ir_diags_t *diagnostics)
{
    bool ok = true;
    for (size_t f = 0; f < ir_func_count(module); f++) {
        ir_func_t *function = ir_func_at(module, f);
        size_t count = 0;
        for (uint32_t b = 0; b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++)
                if (function->blocks[b]->instrs[i]->op
                        == IR_OP_BUFFER_AS_STRING)
                    count++;
        if (!count) continue;
        ir_instr_t **conversions = calloc(count, sizeof(*conversions));
        if (!conversions) return false;
        size_t at = 0;
        for (uint32_t b = 0; b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++)
                if (function->blocks[b]->instrs[i]->op
                        == IR_OP_BUFFER_AS_STRING)
                    conversions[at++] = function->blocks[b]->instrs[i];
        for (size_t c = 0; c < count; c++)
            if (!borrow_conversion(diagnostics, function, conversions[c]))
                ok = false;
        free(conversions);
    }
    return ok;
}
