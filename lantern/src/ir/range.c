#include "ir/range.h"

#include "ir/doms.h"
#include "lib/list.h"
#include "logic/clpfd_store.h"

#include <limits.h>
#include <stdlib.h>

typedef struct {
    const ir_value_t *value;
    core_csp_var_id_t var;
    int64_t minimum;
    int64_t maximum;
    bool has_bounds;
} value_var_t;

core_list_decl(value_var_t);

struct ir_range_analysis {
    ir_module_t *module;
    core_csp_store_t *store;
    core_list_t(value_var_t) values;
    size_t unsupported;
    ir_range_status_t status;
    ir_instr_t *failure;
};

static bool
integer_bounds(ir_module_t *module, const ir_value_t *value,
               int64_t *lo, int64_t *hi)
{
    ir_machine_t machine = value
        ? ir_machine_of(module, value->type) : (ir_machine_t){0};
    uint64_t width = machine.width;
    bool is_signed = machine.class == IR_CLASS_SINT;
    if (!value || (machine.class != IR_CLASS_SINT
                   && machine.class != IR_CLASS_UINT)
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
    for (size_t i = 0; i < a->values.len; i++) {
        if (a->values.data[i].value == value) {
            return &a->values.data[i];
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
    if (!integer_bounds(a->module, value, &lo, &hi)) {
        return false;
    }
    if (!core_list_try_reserve(a->values, a->values.len + 1)) return false;
    core_csp_var_id_t var = core_csp_new_var(
        a->store, CORE_STRING_STATIC("ir-value"), core_csp_dom_range(lo, hi));
    if (var < 0) {
        return false;
    }
    a->values.data[a->values.len++] = (value_var_t){
        .value = value,
        .var = var,
    };
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

typedef struct {
    ir_block_t *block;
    ir_block_t *predecessor;
} range_path_block_t;

typedef struct {
    ir_instr_t *branch;
    bool taken;
} range_path_edge_t;

typedef struct {
    ir_func_t *function;
    ir_range_analysis_t *result;
    ir_diags_t *diagnostics;
    range_path_block_t *blocks;
    size_t block_count;
    range_path_edge_t *edges;
    size_t edge_count;
    bool *active;
    size_t path_count;
    bool satisfiable_path;
    bool unsupported;
    bool saw_unsatisfiable;
    ir_instr_t *failure;
} range_walk_t;

#define IR_RANGE_MAX_PATHS 512u

static value_var_t *
find_value_mut(ir_range_analysis_t *a, const ir_value_t *value)
{
    for (size_t i = 0; i < a->values.len; i++)
        if (a->values.data[i].value == value) return &a->values.data[i];
    return nullptr;
}

static bool
merge_path_bounds(ir_range_analysis_t *result,
                  const ir_range_analysis_t *path)
{
    for (size_t i = 0; i < path->values.len; i++) {
        const value_var_t *entry = &path->values.data[i];
        core_csp_domain_result_t domain_result = core_csp_var_domain(
            path->store, entry->var);
        const core_csp_domain_t *domain = core_result_get_ok(domain_result);
        if (!domain || core_csp_dom_is_empty(domain)) continue;
        int64_t minimum = core_csp_dom_min(domain);
        int64_t maximum = core_csp_dom_max(domain);
        value_var_t *merged = find_value_mut(result, entry->value);
        if (!merged) {
            if (!core_list_try_push(result->values,
                    ((value_var_t){.value = entry->value, .var = -1})))
                return false;
            merged = &result->values.data[result->values.len - 1];
        }
        if (!merged->has_bounds) {
            merged->minimum = minimum;
            merged->maximum = maximum;
            merged->has_bounds = true;
        }
        else {
            if (minimum < merged->minimum) merged->minimum = minimum;
            if (maximum > merged->maximum) merged->maximum = maximum;
        }
    }
    return true;
}

static bool
post_branch_relation(ir_range_analysis_t *a, ir_instr_t *comparison,
                     bool truth)
{
    if (!comparison || comparison->n_operands != 2
        || comparison->operands[0].kind != IR_OPD_VALUE
        || comparison->operands[1].kind != IR_OPD_VALUE) return true;
    const value_var_t *left = find_value(a, comparison->operands[0].value);
    const value_var_t *right = find_value(a, comparison->operands[1].value);
    if (!left || !right) return true;

    bool ok = true;
    switch (comparison->op) {
    case IR_OP_EQ:
        ok = truth
            ? core_csp_post_eq_with_provenance(a->store, left->var,
                                               right->var, comparison)
            : core_csp_post_ne_with_provenance(a->store, left->var,
                                               right->var, comparison);
        break;
    case IR_OP_NE:
        ok = truth
            ? core_csp_post_ne_with_provenance(a->store, left->var,
                                               right->var, comparison)
            : core_csp_post_eq_with_provenance(a->store, left->var,
                                               right->var, comparison);
        break;
    case IR_OP_LT:
        ok = truth
            ? core_csp_post_lt_with_provenance(a->store, left->var,
                                               right->var, comparison)
            : core_csp_post_le_with_provenance(a->store, right->var,
                                               left->var, comparison);
        break;
    case IR_OP_LE:
        ok = truth
            ? core_csp_post_le_with_provenance(a->store, left->var,
                                               right->var, comparison)
            : core_csp_post_lt_with_provenance(a->store, right->var,
                                               left->var, comparison);
        break;
    case IR_OP_GT:
        ok = truth
            ? core_csp_post_lt_with_provenance(a->store, right->var,
                                               left->var, comparison)
            : core_csp_post_le_with_provenance(a->store, left->var,
                                               right->var, comparison);
        break;
    case IR_OP_GE:
        ok = truth
            ? core_csp_post_le_with_provenance(a->store, right->var,
                                               left->var, comparison)
            : core_csp_post_lt_with_provenance(a->store, left->var,
                                               right->var, comparison);
        break;
    default:
        return true;
    }
    return ok && fail_if_unsat(a, comparison);
}

static bool
post_branch_condition(ir_range_analysis_t *a, ir_value_t *condition,
                      bool truth)
{
    for (uint32_t depth = 0; condition && condition->def && depth < 16;
         depth++) {
        ir_instr_t *definition = condition->def;
        if ((definition->op == IR_OP_COPY || definition->op == IR_OP_NOT)
            && definition->n_operands == 1
            && definition->operands[0].kind == IR_OPD_VALUE) {
            if (definition->op == IR_OP_NOT) truth = !truth;
            condition = definition->operands[0].value;
            continue;
        }
        return post_branch_relation(a, definition, truth);
    }
    return true;
}

static bool
post_phi_for_predecessor(ir_range_analysis_t *a, ir_instr_t *phi,
                         ir_block_t *predecessor)
{
    if (!phi->dest || !find_value(a, phi->dest)) return true;
    ir_value_t *incoming = nullptr;
    for (uint32_t i = 0; i < phi->n_operands; i++) {
        ir_operand_t *operand = &phi->operands[i];
        if (operand->kind == IR_OPD_PHI_ARM
            && operand->phi.block == predecessor) {
            incoming = operand->phi.value;
            break;
        }
    }
    const value_var_t *source = find_value(a, incoming);
    const value_var_t *target = find_value(a, phi->dest);
    if (!source || !target) {
        mark_unsupported(a);
        return true;
    }
    if (!core_csp_post_eq_with_provenance(a->store, target->var,
                                          source->var, phi)) {
        (void)fail_if_unsat(a, phi);
        return false;
    }
    return fail_if_unsat(a, phi);
}

static range_path_edge_t *
path_edge_for_branch(range_walk_t *walk, ir_instr_t *branch)
{
    for (size_t i = 0; i < walk->edge_count; i++)
        if (walk->edges[i].branch == branch) return &walk->edges[i];
    return nullptr;
}

static void
analyze_path(range_walk_t *walk)
{
    if (++walk->path_count > IR_RANGE_MAX_PATHS) {
        walk->unsupported = true;
        return;
    }
    ir_range_analysis_t *path = calloc(1, sizeof(*path));
    if (!path) {
        walk->unsupported = true;
        return;
    }
    path->module = walk->function->owner;
    path->store = core_csp_store_new();
    path->status = path->store ? IR_RANGE_COMPLETE : IR_RANGE_UNSUPPORTED;
    if (!path->store) {
        walk->unsupported = true;
        free(path);
        return;
    }
    for (uint32_t i = 0; i < walk->function->n_params; i++) {
        ir_machine_t machine = ir_machine_of(path->module,
            walk->function->params[i]->type);
        int64_t minimum = 0, maximum = 0;
        if (machine.class != IR_CLASS_SINT && machine.class != IR_CLASS_UINT)
            continue;
        if (!integer_bounds(path->module, walk->function->params[i],
                            &minimum, &maximum)
            || !track_value(path, walk->function->params[i]))
            mark_unsupported(path);
    }

    for (size_t b = 0; b < walk->block_count; b++) {
        ir_block_t *block = walk->blocks[b].block;
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];
            int64_t minimum = 0, maximum = 0;
            if (in->dest_kind == IR_DEST_VALUE && in->dest) {
                ir_machine_t machine = ir_machine_of(path->module,
                                                      in->dest->type);
                if (machine.class != IR_CLASS_SINT
                    && machine.class != IR_CLASS_UINT) continue;
                if (!integer_bounds(path->module, in->dest, &minimum,
                                    &maximum)
                    || !track_value(path, in->dest))
                    mark_unsupported(path);
            }
        }
    }

    bool possible = path->status != IR_RANGE_UNSUPPORTED;
    for (size_t b = 0; possible && b < walk->block_count; b++) {
        ir_block_t *block = walk->blocks[b].block;
        for (uint32_t i = 0; possible && i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];
            if (in == ir_block_terminator(block)) continue;
            if (in->op == IR_OP_PHI)
                possible = post_phi_for_predecessor(path, in,
                    walk->blocks[b].predecessor);
            else if (in->dest_kind == IR_DEST_VALUE && in->dest
                && find_value(path, in->dest))
                possible = post_instruction(path, in);
        }
        ir_instr_t *terminator = ir_block_terminator(block);
        if (possible && terminator && terminator->op == IR_OP_BRANCH) {
            range_path_edge_t *edge = path_edge_for_branch(walk, terminator);
            if (!edge || terminator->n_operands < 1
                || terminator->operands[0].kind != IR_OPD_VALUE) {
                mark_unsupported(path);
            }
            else
                possible = post_branch_condition(path,
                    terminator->operands[0].value, edge->taken);
        }
    }

    if (path->status == IR_RANGE_UNSATISFIABLE || !possible) {
        walk->saw_unsatisfiable = true;
        if (!walk->failure) walk->failure = path->failure;
    }
    else if (path->status == IR_RANGE_UNSUPPORTED) {
        walk->unsupported = true;
    }
    else {
        walk->satisfiable_path = true;
        if (path->status == IR_RANGE_PARTIAL
            && walk->result->status == IR_RANGE_COMPLETE)
            walk->result->status = IR_RANGE_PARTIAL;
        walk->result->unsupported += path->unsupported;
        if (!merge_path_bounds(walk->result, path))
            walk->unsupported = true;
    }
    ir_range_analysis_free(path);
}

static bool
path_push_block(range_walk_t *walk, ir_block_t *block,
                ir_block_t *predecessor)
{
    if (!block || block->index >= walk->function->n_blocks
        || walk->function->blocks[block->index] != block
        || walk->active[block->index]) return false;
    walk->active[block->index] = true;
    walk->blocks[walk->block_count++] = (range_path_block_t){
        .block = block,
        .predecessor = predecessor,
    };
    return true;
}

static void
walk_paths(range_walk_t *walk, ir_block_t *block, ir_block_t *predecessor)
{
    if (walk->unsupported) return;
    if (!path_push_block(walk, block, predecessor)) {
        walk->unsupported = true;
        return;
    }
    ir_instr_t *terminator = ir_block_terminator(block);
    if (!terminator) {
        if (walk->function->n_blocks == 1) analyze_path(walk);
        else walk->unsupported = true;
    }
    else if (terminator->op == IR_OP_RETURN || terminator->op == IR_OP_FAIL
             || terminator->op == IR_OP_TRAP
             || terminator->op == IR_OP_UNREACHABLE) {
        analyze_path(walk);
    }
    else if (terminator->op == IR_OP_JUMP && terminator->n_operands == 1
             && terminator->operands[0].kind == IR_OPD_BLOCK) {
        walk_paths(walk, terminator->operands[0].block, block);
    }
    else if (terminator->op == IR_OP_BRANCH && terminator->n_operands == 3
             && terminator->operands[0].kind == IR_OPD_VALUE
             && terminator->operands[1].kind == IR_OPD_BLOCK
             && terminator->operands[2].kind == IR_OPD_BLOCK) {
        if (walk->edge_count == IR_RANGE_MAX_PATHS) {
            walk->unsupported = true;
        }
        else {
            walk->edges[walk->edge_count++] = (range_path_edge_t){
                .branch = terminator, .taken = true,
            };
            walk_paths(walk, terminator->operands[1].block, block);
            walk->edge_count--;
            if (!walk->unsupported) {
                walk->edges[walk->edge_count++] = (range_path_edge_t){
                    .branch = terminator, .taken = false,
                };
                walk_paths(walk, terminator->operands[2].block, block);
                walk->edge_count--;
            }
        }
    }
    else {
        walk->unsupported = true;
    }
    walk->block_count--;
    walk->active[block->index] = false;
}

ir_range_analysis_t *
ir_range_analyze_function(ir_func_t *function, ir_diags_t *diagnostics)
{
    if (!function) return nullptr;
    ir_range_analysis_t *result = calloc(1, sizeof(*result));
    if (!result) return nullptr;
    result->module = function->owner;
    result->status = IR_RANGE_COMPLETE;
    if (!function->n_blocks) {
        result->status = IR_RANGE_UNSUPPORTED;
        result->unsupported = 1;
        return result;
    }

    range_path_block_t *blocks = calloc(function->n_blocks, sizeof(*blocks));
    range_path_edge_t *edges = calloc(IR_RANGE_MAX_PATHS, sizeof(*edges));
    bool *active = calloc(function->n_blocks, sizeof(*active));
    if (!blocks || !edges || !active) {
        free(blocks);
        free(edges);
        free(active);
        ir_range_analysis_free(result);
        return nullptr;
    }
    range_walk_t walk = {
        .function = function,
        .result = result,
        .diagnostics = diagnostics,
        .blocks = blocks,
        .edges = edges,
        .active = active,
    };
    walk_paths(&walk, ir_block_entry(function), nullptr);

    if (walk.unsupported) {
        result->status = IR_RANGE_UNSUPPORTED;
        result->unsupported++;
        for (size_t i = 0; i < result->values.len; i++)
            result->values.data[i].has_bounds = false;
    }
    else if (!walk.path_count || !walk.satisfiable_path) {
        result->status = IR_RANGE_UNSATISFIABLE;
        result->failure = walk.failure;
    }
    if (result->status == IR_RANGE_UNSATISFIABLE && diagnostics
        && result->failure) {
        ir_diag_t *dg = ir_diag_begin_at(diagnostics, IR_DIAG_RANGE_UNSAT,
            result->failure, result->failure->block, function,
            "inconsistent integer ranges");
        ir_diag_message(dg, diagnostics,
                        "integer value constraints have no solution");
    }
    free(blocks);
    free(edges);
    free(active);
    return result;
}

void
ir_range_analysis_free(ir_range_analysis_t *a)
{
    if (!a) {
        return;
    }
    core_csp_store_free(a->store);
    core_list_free(a->values);
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
    if (!a || a->status == IR_RANGE_UNSUPPORTED
        || a->status == IR_RANGE_UNSATISFIABLE || !value || !minimum
        || !maximum) {
        return false;
    }
    const value_var_t *entry = find_value(a, value);
    if (!entry || !entry->has_bounds) {
        return false;
    }
    *minimum = entry->minimum;
    *maximum = entry->maximum;
    return true;
}
