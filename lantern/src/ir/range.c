#include "ir/range.h"

#include "ir/doms.h"
#include "logic/clpfd_store.h"

#include <limits.h>
#include <stdlib.h>

typedef struct {
    const ir_value_t *value;
    core_csp_var_id_t var;
} value_var_t;

struct ir_range_analysis {
    core_csp_store_t *store;
    value_var_t *values;
    size_t n_values;
    size_t cap_values;
    size_t unsupported;
    ir_range_status_t status;
    ir_instr_t *failure;
};

static bool
integer_bounds(const ir_value_t *value, int64_t *lo, int64_t *hi)
{
    uint64_t width = 0;
    bool is_signed = false;
    if (!value || !th_bitint_info(value->type, &width, &is_signed)
        || width == 0 || width > 64 || (!is_signed && width > 63)) {
        return false;
    }

    if (is_signed) {
        if (width == 64) {
            *lo = INT64_MIN;
            *hi = INT64_MAX;
        }
        else {
            *lo = -(INT64_C(1) << (width - 1));
            *hi = (INT64_C(1) << (width - 1)) - 1;
        }
    }
    else {
        *lo = 0;
        *hi = (int64_t)((UINT64_C(1) << width) - 1);
    }
    return true;
}

static const value_var_t *
find_value(const ir_range_analysis_t *a, const ir_value_t *value)
{
    for (size_t i = 0; i < a->n_values; i++) {
        if (a->values[i].value == value) {
            return &a->values[i];
        }
    }
    return nullptr;
}

static bool
track_value(ir_range_analysis_t *a, const ir_value_t *value)
{
    if (find_value(a, value)) {
        return true;
    }
    int64_t lo = 0, hi = 0;
    if (!integer_bounds(value, &lo, &hi)) {
        return false;
    }
    if (a->n_values == a->cap_values) {
        size_t cap = a->cap_values ? a->cap_values * 2 : 16;
        value_var_t *values = realloc(a->values, cap * sizeof(*values));
        if (!values) {
            return false;
        }
        a->values = values;
        a->cap_values = cap;
    }
    core_csp_var_id_t var = core_csp_new_var(
        a->store, CORE_STRING_STATIC("ir-value"), core_csp_dom_range(lo, hi));
    if (var < 0) {
        return false;
    }
    a->values[a->n_values++] = (value_var_t){.value = value, .var = var};
    return true;
}

static void
mark_unsupported(ir_range_analysis_t *a)
{
    a->unsupported++;
    if (a->status == IR_RANGE_COMPLETE) {
        a->status = IR_RANGE_PARTIAL;
    }
}

static bool
fail_if_unsat(ir_range_analysis_t *a, ir_instr_t *in)
{
    if (core_csp_propagate(a->store)) {
        return true;
    }
    a->status = IR_RANGE_UNSATISFIABLE;
    a->failure = core_csp_failed_provenance(a->store);
    if (!a->failure) {
        a->failure = in;
    }
    return false;
}

static bool
post_instruction(ir_range_analysis_t *a, ir_instr_t *in)
{
    if (in->dest_kind != IR_DEST_VALUE || !in->dest) {
        return true;
    }
    const value_var_t *dest = find_value(a, in->dest);
    if (!dest) {
        mark_unsupported(a);
        return true;
    }

    if (in->op == IR_OP_CONST) {
        if (in->n_operands != 1 || in->operands[0].kind != IR_OPD_CONST
            || in->operands[0].lit.is_float) {
            mark_unsupported(a);
            return true;
        }
        ir_lit_t literal = in->operands[0].lit.i;
        ir_uint_t max_magnitude = literal.neg
                                      ? (ir_uint_t)INT64_MAX + 1
                                      : (ir_uint_t)INT64_MAX;
        if (literal.mag > max_magnitude) {
            core_csp_domain_t empty = core_csp_dom_empty();
            if (!core_csp_post_in_with_provenance(a->store, dest->var, empty,
                                                  in)) {
                (void)fail_if_unsat(a, in);
                return false;
            }
        }
        int64_t value = literal.neg
                            ? (literal.mag == (ir_uint_t)INT64_MAX + 1
                                   ? INT64_MIN
                                   : -(int64_t)literal.mag)
                            : (int64_t)literal.mag;
        if (!core_csp_post_eq_const_with_provenance(a->store, dest->var,
                                                    value, in)) {
            (void)fail_if_unsat(a, in);
            return false;
        }
        return fail_if_unsat(a, in);
    }

    if (in->op == IR_OP_COPY && in->n_operands == 1
        && in->operands[0].kind == IR_OPD_VALUE) {
        const value_var_t *src = find_value(a, in->operands[0].value);
        if (!src) {
            mark_unsupported(a);
            return true;
        }
        if (!core_csp_post_eq_with_provenance(a->store, dest->var, src->var,
                                              in)) {
            (void)fail_if_unsat(a, in);
            return false;
        }
        return fail_if_unsat(a, in);
    }

    if ((in->op == IR_OP_ADD || in->op == IR_OP_SUB)
        && in->n_operands == 2 && ir_instr_has_mod(in, "trap")
        && in->operands[0].kind == IR_OPD_VALUE
        && in->operands[1].kind == IR_OPD_VALUE) {
        const value_var_t *left = find_value(a, in->operands[0].value);
        const value_var_t *right = find_value(a, in->operands[1].value);
        if (!left || !right) {
            mark_unsupported(a);
            return true;
        }
        core_csp_var_id_t vars[] = {dest->var, left->var, right->var};
        int64_t coeffs[] = {1, -1, -1};
        if (in->op == IR_OP_SUB) {
            coeffs[2] = 1;
        }
        if (!core_csp_post_linear_with_provenance(a->store, vars, coeffs, 3,
                                                  0, in)) {
            (void)fail_if_unsat(a, in);
            return false;
        }
        return fail_if_unsat(a, in);
    }

    mark_unsupported(a);
    return true;
}

static ir_block_t **
linear_block_path(ir_func_t *function, size_t *path_count)
{
    *path_count = 0;
    if (!function->n_blocks) return nullptr;
    ir_block_t **path = calloc(function->n_blocks, sizeof(*path));
    bool *seen = calloc(function->n_blocks, sizeof(*seen));
    if (!path || !seen) {
        free(path);
        free(seen);
        return nullptr;
    }

    ir_block_t *block = ir_block_entry(function);
    bool valid = block != nullptr;
    bool returned = false;
    while (valid && block) {
        if (block->index >= function->n_blocks ||
            function->blocks[block->index] != block || seen[block->index]) {
            valid = false;
            break;
        }
        seen[block->index] = true;
        path[(*path_count)++] = block;
        ir_instr_t *terminator = ir_block_terminator(block);
        if (!terminator) {
            returned = function->n_blocks == 1;
            valid = returned;
            block = nullptr;
        } else if (terminator->op == IR_OP_JUMP &&
                   ir_succ_count(terminator) == 1) {
            block = ir_succ_of(&terminator->operands[0]);
            valid = block != nullptr;
        } else if (terminator->op == IR_OP_RETURN &&
                   ir_succ_count(terminator) == 0) {
            returned = true;
            block = nullptr;
        } else {
            valid = false;
        }
    }

    free(seen);
    if (!valid || !returned) {
        free(path);
        *path_count = 0;
        return nullptr;
    }
    return path;
}

ir_range_analysis_t *
ir_range_analyze_function(ir_func_t *function, ir_diags_t *diagnostics)
{
    if (!function) {
        return nullptr;
    }
    ir_range_analysis_t *a = calloc(1, sizeof(*a));
    if (!a) {
        return nullptr;
    }
    a->store = core_csp_store_new();
    a->status = IR_RANGE_COMPLETE;
    if (!a->store) {
        free(a);
        return nullptr;
    }

    size_t n_blocks = 0;
    ir_block_t **blocks = linear_block_path(function, &n_blocks);
    if (!blocks) {
        a->status = IR_RANGE_UNSUPPORTED;
        a->unsupported++;
        return a;
    }
    for (uint32_t i = 0; i < function->n_params; i++) {
        if (!track_value(a, function->params[i])) {
            mark_unsupported(a);
        }
    }
    for (size_t b = 0; b < n_blocks &&
                        a->status != IR_RANGE_UNSATISFIABLE; b++) {
        ir_block_t *block = blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];
            if (in->dest_kind == IR_DEST_VALUE && in->dest &&
                !track_value(a, in->dest)) {
                mark_unsupported(a);
            }
        }
    }

    for (size_t b = 0; b < n_blocks &&
                        a->status != IR_RANGE_UNSATISFIABLE; b++) {
        ir_block_t *block = blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            if (!post_instruction(a, block->instrs[i])) {
                break;
            }
        }
    }
    if (a->status == IR_RANGE_UNSATISFIABLE && diagnostics) {
        ir_diag_t *dg = ir_diag_begin_at(diagnostics, IR_DIAG_RANGE_UNSAT,
                                         a->failure, a->failure->block, function,
                                         "inconsistent integer ranges");
        ir_diag_message(dg, diagnostics,
                        "integer value constraints have no solution");
    }
    free(blocks);
    return a;
}

void
ir_range_analysis_free(ir_range_analysis_t *a)
{
    if (!a) {
        return;
    }
    core_csp_store_free(a->store);
    free(a->values);
    free(a);
}

ir_range_status_t
ir_range_analysis_status(const ir_range_analysis_t *a)
{
    return a ? a->status : IR_RANGE_UNSUPPORTED;
}

size_t
ir_range_analysis_unsupported_count(const ir_range_analysis_t *a)
{
    return a ? a->unsupported : 0;
}

ir_instr_t *
ir_range_analysis_failure_source(const ir_range_analysis_t *a)
{
    return a ? a->failure : nullptr;
}

bool
ir_range_analysis_bounds(const ir_range_analysis_t *a, const ir_value_t *value,
                         int64_t *minimum, int64_t *maximum)
{
    if (!a || !value || !minimum || !maximum) {
        return false;
    }
    const value_var_t *entry = find_value(a, value);
    if (!entry) {
        return false;
    }
    core_csp_domain_result_t result = core_csp_var_domain(a->store, entry->var);
    const core_csp_domain_t *domain = core_result_get_ok(result);
    if (!domain || core_csp_dom_is_empty(domain)) {
        return false;
    }
    *minimum = core_csp_dom_min(domain);
    *maximum = core_csp_dom_max(domain);
    return true;
}
