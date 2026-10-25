#include "comptime.h"
#include "lower/lower_internal.h"
#include "static_branches.h"

static uint64_t
dead_code_site_key(core_string_t module, uint32_t line, uint32_t column);
static void
note_dead_code_site(n00bb_link_analysis_t *analysis, core_string_t module,
                    uint32_t line, uint32_t column);
static ir_dead_code_t *
dead_code_of(n00bb_link_analysis_t *analysis, core_translation_unit_t *unit,
             ir_func_t *function);
static bool
narrowed_value(ir_func_t *function, ir_abcd_t *ranges,
               const ir_value_t *value, const ir_instr_t *at);
static bool
overflow_narrowed(ir_func_t *function, ir_abcd_t *ranges,
                  const ir_instr_t *in);
static core_string_t
format_interval(ir_interval_t range);
static void
shift_count_casts(const ir_func_t *function, core_dict_t *out);
static comparison_fact_t
compare_intervals(ir_op_t op, ir_interval_t a, ir_interval_t b);
static void
report_at(core_translation_unit_t *unit, n00bb_diag_t id,
          const ir_instr_t *in, n00bb_text_t message, n00bb_args_t args);
static void
check_constant_condition(core_translation_unit_t *unit, ir_abcd_t *ranges,
                         ir_instr_t *branch);
static bool
check_operand_domain(core_translation_unit_t *unit, ir_func_t *function,
                     ir_abcd_t *ranges, const ir_site_t *site);
static ir_lock_role_t
lock_role(const ir_instr_t *in);
static const ir_value_t *
copied_source(const ir_value_t *value);
static uint64_t
entry_param_mask(const core_list_t(entry_param_t) *params,
                 const ir_func_t *function);
static bool
add_entry_param(core_list_t(entry_param_t) *params, ir_func_t *function,
                uint64_t bit);
static void
visit_function_values(const core_list_t(n00bb_ir_func_ptr_t) *functions,
                      function_value_fn visit, void *context);
static void
add_taken_entry(void *context, const ir_value_t *value);
static void
note_taken_value(void *context, const ir_value_t *value);
static bool
atomic_access(const ir_instr_t *in);
static ir_lock_role_t
race_lock_role(void *context, const ir_instr_t *in);
static ir_func_t *
race_direct_target(void *context, const ir_instr_t *in);
static bool
race_atomic(void *context, const ir_instr_t *in, const ir_splace_t *place);
static race_start_t *
race_start_for(race_starts_t *starts, ir_func_t *entry);
static void
add_argument_places(race_start_t *start, ir_func_t *function,
                    const ir_value_t *argument);
static void
collect_race_starts(const core_list_t(n00bb_ir_func_ptr_t) *functions,
                    const core_list_t(entry_param_t) *params,
                    ir_summaries_t *summaries, race_starts_t *starts);
static void
report_race(void *context, const ir_race_t *race);
static void
warn_ignored_results(n00bb_link_analysis_t *analysis,
                     ir_func_t *const *functions, size_t count);
static void
report_dead_store(const ir_instr_t *store, void *context);
static bool
parameter_exempt(const ir_value_t *parameter);
static bool
function_signature_fixed(const ir_func_t *function, const core_dict_t *taken);
static core_hash_value_t
param_site_hash(void *key);
static bool
param_site_eq(const void *a, const void *b);
static void
note_taken_owner(void *context, const ir_value_t *value);
static parse_sym_entry_t *
ir_function_declaration(n00bb_link_analysis_t *analysis,
                        core_translation_unit_t *unit, ir_func_t *function);
static bool
parameter_location(parse_grammar_t *grammar, parse_sym_entry_t *function,
                   core_string_t name, core_diag_location_t *out);
static void
warn_dead_code(n00bb_link_analysis_t *analysis, ir_func_t *const *functions,
               size_t count, const core_dict_t *instances);
static bool
same_global(const ir_global_t *left, const ir_global_t *right);
static const ir_instr_t *
global_write_in(const ir_func_t *function, const ir_global_t *global);
static bool
identifier_byte(uint8_t byte);
static uint8_t
thread_runtime_entry_state(
    n00bb_runtime_init_analysis_t *runtime, ir_global_t *global,
    bool has_static_default, bool *phase_succeeds);
static size_t
initializer_order_index(core_list_t(core_translation_unit_ptr_t) *order,
                        core_translation_unit_t *unit);
static bool
initializer_write_precedes_read(
    n00bb_runtime_init_analysis_t *runtime,
    core_list_t(core_translation_unit_ptr_t) *initializer_order,
    const n00bb_initializer_read_t *read);
static parse_tree_t *
initializer_read_node(const n00bb_initializer_read_t *read);
static bool
global_has_writer(n00bb_link_analysis_t *analysis,
                  n00bb_runtime_init_analysis_t *runtime, ir_global_t *global);
static core_string_t
read_path(const ir_instr_t *read);

static uint64_t
dead_code_site_key(core_string_t module, uint32_t line, uint32_t column)
{
    uint64_t key = module.data
        ? core_xxh3_64bits_raw(module.data, module.u8_bytes) : 0;
    return (key ^ (((uint64_t)line << 32 | column) * 0x9e3779b97f4a7c15ull))
        | 1;
}

static void
note_dead_code_site(n00bb_link_analysis_t *analysis, core_string_t module,
                    uint32_t line, uint32_t column)
{
    if (!analysis->dead_code_sites_ready) {
        core_dict_init(&analysis->dead_code_sites, core_hash_word, nullptr);
        analysis->dead_code_sites_ready = true;
    }
    if (line)
        core_dict_put(&analysis->dead_code_sites,
                      dead_code_site_key(module, line, column), 1);
}

n00bb_diagnostic_filter_t *
n00bb_diagnostic_filter_take(n00bb_link_analysis_t *analysis)
{
    if (!analysis || (!analysis->dead_code_sites_ready
                      && !analysis->failed_regions.len)) return nullptr;
    n00bb_diagnostic_filter_t *filter = core_alloc(n00bb_diagnostic_filter_t);
    filter->dead_code_sites = analysis->dead_code_sites;
    filter->dead_code_sites_ready = analysis->dead_code_sites_ready;
    filter->failed_regions = analysis->failed_regions;
    analysis->dead_code_sites = (core_dict_t){0};
    analysis->dead_code_sites_ready = false;
    analysis->failed_regions = (core_list_t(n00bb_failed_region_t)){0};
    return filter;
}

void
n00bb_diagnostic_filter_free(n00bb_diagnostic_filter_t *filter)
{
    if (!filter) return;
    if (filter->dead_code_sites_ready) core_dict_free(&filter->dead_code_sites);
    for (size_t i = 0; i < filter->failed_regions.len; i++)
        core_free(filter->failed_regions.data[i].module.data);
    core_list_free(filter->failed_regions);
    core_free(filter);
}

bool
n00bb_diagnostic_visible(void *context, const core_diagnostic_t *diagnostic)
{
    n00bb_diagnostic_filter_t *filter = context;
    if (!filter || !core_diag_is_advisory(diagnostic->severity)
        || !diagnostic->location.has_location
        || core_string_eq(diagnostic->code,
                          n00bb_diag_code(N00B_DIAG_UNREACHABLE_CODE)))
        return true;
    for (size_t i = 0; i < filter->failed_regions.len; i++)
        if (core_string_eq(filter->failed_regions.data[i].module,
                           diagnostic->module)
            && span_contains(filter->failed_regions.data[i].span,
                             diagnostic->location))
            return false;
    if (!filter->dead_code_sites_ready) return true;
    bool dead = false;
    core_dict_get(&filter->dead_code_sites,
                  (void *)(uintptr_t)dead_code_site_key(diagnostic->module,
                      diagnostic->location.line, diagnostic->location.column),
                  &dead);
    return !dead;
}

static int
decide_type_test(void *context, const ir_instr_t *condition)
{
    (void)context;
    return condition->op == IR_OP_TYPE_TEST
        ? n00bb_static_type_test(condition) : -1;
}

static ir_dead_code_t *
dead_code_of(n00bb_link_analysis_t *analysis, core_translation_unit_t *unit,
             ir_func_t *function)
{
    dead_code_context_t context = {
        .facts = group_facts(analysis, function, false),
        .unit = unit,
    };
    return ir_dead_code_build(function, call_diverges_by_rule,
                              decide_type_test, &context);
}

static const ir_instr_t *
first_user_instruction(ir_block_t *block, const ir_instr_t *test)
{
    for (uint32_t depth = 0; block && depth < 8; depth++) {
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            const ir_instr_t *in = block->instrs[i];
            if (user_span(in)
                && (in->span.start_byte != test->span.start_byte
                    || in->span.end_byte != test->span.end_byte))
                return in;
        }
        ir_instr_t *term = ir_block_terminator(block);
        block = term && term->op == IR_OP_JUMP ? ir_succ_at(term, 0)
                                               : nullptr;
    }
    return nullptr;
}

void
warn_dead_type_arms(n00bb_link_analysis_t *analysis)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        ir_module_t *module = state && !state->interface_module
            ? state->ir_module : nullptr;
        for (size_t f = 0; module && f < ir_func_count(module); f++) {
            ir_func_t *function = ir_func_at(module, f);
            if (th_is_closed(ir_module_types(module), function->type)
                || !n00bb_instantiated(function))
                continue;
            for (uint32_t b = 0; b < function->n_blocks; b++) {
                ir_instr_t *term = ir_block_terminator(function->blocks[b]);
                ir_value_t *condition = term && term->op == IR_OP_BRANCH
                        && term->n_operands == 3
                    ? ir_operand_value(&term->operands[0]) : nullptr;
                ir_instr_t *test = condition ? condition->def : nullptr;
                if (!test || test->op != IR_OP_TYPE_TEST
                    || n00bb_static_type_test(test) >= 0) continue;
                uint64_t seen = n00bb_type_test_outcomes(test);
                int dead = !(seen & 2) ? 0 : seen == 2 ? 1 : -1;
                const ir_instr_t *site = dead >= 0
                    ? first_user_instruction(ir_succ_at(term, dead), test)
                    : nullptr;
                if (site)
                    n00bb_report(core_translation_unit_diagnostics(unit),
                        unit, N00B_DIAG_UNREACHABLE_CODE,
                        n00bb_span_location(site->span),
                        N00B_TEXT_CODE_NEVER_RUNS_NO_INSTANCE, N00BB_NO_ARGS);
            }
        }
    }
}

static bool
narrowed_value(ir_func_t *function, ir_abcd_t *ranges,
               const ir_value_t *value, const ir_instr_t *at)
{
    ir_interval_t type;
    ir_abcd_range_t range;
    return value && ir_abcd_range_of_conditions(ranges, value, at, &range)
        && range.seen.lo != range.seen.hi
        && ir_abcd_type_bounds(function, value, &type)
        && (range.seen.lo > type.lo || range.seen.hi < type.hi);
}

static bool
overflow_narrowed(ir_func_t *function, ir_abcd_t *ranges,
                  const ir_instr_t *in)
{
    bool unary = in->op == IR_OP_NEG || in->op == IR_OP_CAST;
    return narrowed_value(function, ranges,
                          ir_operand_value(&in->operands[0]), in)
        || (!unary && in->n_operands > 1
            && narrowed_value(function, ranges,
                              ir_operand_value(&in->operands[1]), in));
}

static core_string_t
format_interval(ir_interval_t range)
{
    core_buffer_t *out = core_buffer_empty();
    if (range.lo != range.hi) core_buffer_puts(out, "[");
    ir_int_append(out, range.lo);
    if (range.lo != range.hi) {
        core_buffer_puts(out, ", ");
        ir_int_append(out, range.hi);
        core_buffer_puts(out, "]");
    }
    return core_buffer_take_string(out);
}

static void
shift_count_casts(const ir_func_t *function, core_dict_t *out)
{
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = function->blocks[b]->instrs[i];
            const ir_value_t *count = (in->op == IR_OP_SHL
                    || in->op == IR_OP_SHR) && in->n_operands == 2
                ? ir_operand_value(&in->operands[1]) : nullptr;
            if (count && count->kind == IR_VAL_INSTR
                && count->def->op == IR_OP_CAST)
                core_dict_put(out, count->def, count->def);
        }
}

static core_list_t(n00bb_ir_func_ptr_t)
source_functions(n00bb_link_analysis_t *analysis);

bool
diagnose_overflow(n00bb_link_analysis_t *analysis)
{
    bool ok = true;
    core_list_t(n00bb_ir_func_ptr_t) functions = source_functions(analysis);
    for (size_t f = 0; f < functions.len; f++) {
        ir_func_t *function = functions.data[f];
        core_translation_unit_t *unit =
            unit_for_ir_module(analysis, function->owner);
        if (!unit) continue;
        size_t scope = ir_analysis_scope_begin();
        ir_abcd_t *ranges = ir_func_abcd(function);
        core_dict_t counts;
        core_dict_init(&counts, nullptr, nullptr);
        shift_count_casts(function, &counts);
        for (uint32_t b = 0; ranges && b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                ir_instr_t *in = function->blocks[b]->instrs[i];
                if (!in->span.start_line || in->span.implied
                    || !ir_instr_has_mod(in, CORE_STRING_STATIC("trap"))
                    || core_dict_contains(&counts, in))
                    continue;
                ir_interval_t left = {0}, right = {0};
                ir_overflow_t overflow = ir_abcd_overflow(ranges, in, &left,
                                                           &right);
                bool unary = in->op == IR_OP_NEG || in->op == IR_OP_CAST;
                if (overflow == IR_OVERFLOW_MAY
                    && !overflow_narrowed(function, ranges, in))
                    continue;
                if (overflow != IR_OVERFLOW_MAY
                    && overflow != IR_OVERFLOW_ALWAYS) continue;
                bool always = overflow == IR_OVERFLOW_ALWAYS;
                core_string_t left_text = format_interval(left);
                core_string_t right_text = format_interval(right);
                report_at(unit, always ? N00B_DIAG_OVERFLOW_ALWAYS
                                       : N00B_DIAG_OVERFLOW_POSSIBLE, in,
                    unary ? always ? N00B_TEXT_ALWAYS_OVERFLOWS_UNARY
                                   : N00B_TEXT_MAY_OVERFLOW_UNARY
                          : always ? N00B_TEXT_ALWAYS_OVERFLOWS
                                   : N00B_TEXT_MAY_OVERFLOW,
                    N00BB_ARGS(ARG_TEXT(left_text), ARG_TEXT(right_text)));
                core_free(left_text.data);
                core_free(right_text.data);
                ok = ok && !always;
            }
        core_dict_free(&counts);
        ir_analysis_scope_end(scope);
    }
    core_list_free(functions);
    return ok;
}

void
dump_unit_ranges(core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    ir_module_t *module = unit_module(state);
    if (!module || state->interface_module) return;
    for (size_t f = 0; f < ir_func_count(module); f++) {
        ir_func_t *function = ir_func_at(module, f);
        size_t scope = ir_analysis_scope_begin();
        ir_abcd_t *ranges = function->n_blocks
            ? ir_func_abcd(function) : nullptr;
        for (uint32_t b = 0; ranges && b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                ir_instr_t *in = function->blocks[b]->instrs[i];
                ir_abcd_range_t found;
                ir_interval_t type;
                if (!in->dest || !in->span.start_line
                    || !ir_abcd_range_of(ranges, in->dest, nullptr, &found)
                    || !ir_abcd_type_bounds(function, in->dest, &type))
                    continue;
                ir_interval_t range = found.seen;
                core_buffer_t line = {0};
                core_buffer_printf(&line, "range %.*s %u:%u ir ",
                       (int)unit->module.identifier.u8_bytes,
                       unit->module.identifier.data, in->span.start_line,
                       in->span.start_col);
                ir_int_append(&line, range.lo);
                core_buffer_puts(&line, " ");
                ir_int_append(&line, range.hi);
                core_buffer_puts(&line, " type ");
                ir_int_append(&line, type.lo);
                core_buffer_puts(&line, " ");
                ir_int_append(&line, type.hi);
                core_string_t text = core_buffer_string(&line);
                printf("%.*s\n", (int)text.u8_bytes, text.data);
                core_buffer_release(&line);
            }
        ir_analysis_scope_end(scope);
    }
}

static comparison_fact_t
compare_intervals(ir_op_t op, ir_interval_t a, ir_interval_t b)
{
    switch (op) {
    case IR_OP_LT:
        return a.hi < b.lo ? COMPARISON_TRUE
            : a.lo >= b.hi ? COMPARISON_FALSE : COMPARISON_UNKNOWN;
    case IR_OP_LE:
        return a.hi <= b.lo ? COMPARISON_TRUE
            : a.lo > b.hi ? COMPARISON_FALSE : COMPARISON_UNKNOWN;
    case IR_OP_GT:
        return compare_intervals(IR_OP_LT, b, a);
    case IR_OP_GE:
        return compare_intervals(IR_OP_LE, b, a);
    case IR_OP_EQ:
        return a.lo == a.hi && b.lo == b.hi && a.lo == b.lo
            ? COMPARISON_TRUE
            : a.hi < b.lo || b.hi < a.lo ? COMPARISON_FALSE
                                         : COMPARISON_UNKNOWN;
    case IR_OP_NE: {
        comparison_fact_t equal = compare_intervals(IR_OP_EQ, a, b);
        return equal == COMPARISON_TRUE ? COMPARISON_FALSE
            : equal == COMPARISON_FALSE ? COMPARISON_TRUE
                                        : COMPARISON_UNKNOWN;
    }
    default:
        return COMPARISON_UNKNOWN;
    }
}

bool
user_span(const ir_instr_t *in)
{
    return in->span.start_line && in->span.start_byte && !in->span.implied;
}

static void
report_at(core_translation_unit_t *unit, n00bb_diag_t id,
          const ir_instr_t *in, n00bb_text_t message, n00bb_args_t args)
{
    n00bb_report(core_translation_unit_diagnostics(unit), unit, id,
                 n00bb_span_location(in->span), message, args);
}

static const ir_instr_t *
operand_anchor(const ir_instr_t *in, const ir_value_t *operand)
{
    const ir_instr_t *def = operand && operand->kind == IR_VAL_INSTR
        ? operand->def : nullptr;
    for (unsigned depth = 0; def && depth < 8; depth++) {
        bool inside = user_span(def)
            && def->span.start_byte >= in->span.start_byte
            && def->span.end_byte <= in->span.end_byte
            && (def->span.start_byte != in->span.start_byte
                || def->span.end_byte != in->span.end_byte);
        if (inside) return def;
        if ((def->op != IR_OP_COPY && def->op != IR_OP_CAST)
            || !def->n_operands) break;
        const ir_value_t *source = ir_operand_value(&def->operands[0]);
        def = source && source->kind == IR_VAL_INSTR ? source->def : nullptr;
    }
    return in;
}

static bool
comparison_op(ir_op_t op)
{
    return op == IR_OP_LT || op == IR_OP_LE || op == IR_OP_GT
        || op == IR_OP_GE || op == IR_OP_EQ || op == IR_OP_NE;
}

static comparison_fact_t
condition_fact(ir_abcd_t *ranges, const ir_value_t *value,
               const ir_instr_t *at, unsigned depth)
{
    ir_instr_t *test = value && value->kind == IR_VAL_INSTR
        ? value->def : nullptr;
    if (test && test->op == IR_OP_CONST && test->n_operands == 1
        && test->operands[0].kind == IR_OPD_CONST
        && !test->operands[0].lit.is_float)
        return test->operands[0].lit.i.mag ? COMPARISON_TRUE
                                           : COMPARISON_FALSE;
    if (test && comparison_op(test->op) && test->n_operands == 2
        && depth < 8) {
        const ir_value_t *left = ir_operand_value(&test->operands[0]);
        const ir_value_t *right = ir_operand_value(&test->operands[1]);
        comparison_fact_t inner_left = condition_fact(ranges, left, at,
                                                      depth + 1);
        comparison_fact_t inner_right = condition_fact(ranges, right, at,
                                                       depth + 1);
        if (inner_left != COMPARISON_UNKNOWN
            && inner_right != COMPARISON_UNKNOWN
            && (test->op == IR_OP_EQ || test->op == IR_OP_NE))
            return (inner_left == inner_right) == (test->op == IR_OP_EQ)
                ? COMPARISON_TRUE : COMPARISON_FALSE;
        ir_abcd_range_t a, b;
        if (!ir_abcd_range_of(ranges, left, at, &a)
            || !ir_abcd_range_of(ranges, right, at, &b)
            || a.seen.lo > a.seen.hi || b.seen.lo > b.seen.hi)
            return COMPARISON_UNKNOWN;
        comparison_fact_t fact = compare_intervals(test->op, a.seen, b.seen);
        if (fact != COMPARISON_UNKNOWN) return fact;
        int relational = ir_abcd_compare(ir_func_abcd(test->block->owner),
                                         test);
        return relational == 1 ? COMPARISON_TRUE
            : relational == 0 ? COMPARISON_FALSE : COMPARISON_UNKNOWN;
    }
    ir_abcd_range_t found;
    if (!value || !ir_abcd_range_of(ranges, value, at, &found)
        || found.unbounded_min || found.unbounded_max
        || found.seen.lo != found.seen.hi) return COMPARISON_UNKNOWN;
    return found.seen.lo ? COMPARISON_TRUE : COMPARISON_FALSE;
}

static void
check_constant_condition(core_translation_unit_t *unit, ir_abcd_t *ranges,
                         ir_instr_t *branch)
{
    const ir_value_t *condition = branch->n_operands == 3
        ? ir_operand_value(&branch->operands[0]) : nullptr;
    ir_instr_t *test = condition && condition->kind == IR_VAL_INSTR
        ? condition->def : nullptr;
    if (!test || !user_span(test) || test->n_operands != 2
        || !comparison_op(test->op)) return;
    comparison_fact_t fact = condition_fact(ranges, condition, branch, 0);
    if (fact == COMPARISON_UNKNOWN) return;
    report_at(unit, N00B_DIAG_CONSTANT_CONDITION, test,
              fact == COMPARISON_TRUE ? N00B_TEXT_CONDITION_ALWAYS_TRUE
                                      : N00B_TEXT_CONDITION_ALWAYS_FALSE,
              N00BB_NO_ARGS);
}

static bool
check_operand_domain(core_translation_unit_t *unit, ir_func_t *function,
                     ir_abcd_t *ranges, const ir_site_t *site)
{
    ir_instr_t *in = site->instr;
    const ir_value_t *operand = site->operand < in->n_operands
        ? ir_operand_value(&in->operands[site->operand]) : nullptr;
    const ir_instr_t *anchor = operand_anchor(in, operand);
    bool division = site->kind == IR_SITE_DIVISOR;
    const ir_instr_t *at = in;
    if (!division && operand && operand->kind == IR_VAL_INSTR
        && operand->def->op == IR_OP_CAST && operand->def->n_operands == 1) {
        at = operand->def;
        operand = ir_operand_value(&operand->def->operands[0]);
    }
    ir_abcd_range_t found;
    ir_interval_t type;
    if (!operand || !ir_abcd_range_of(ranges, operand, at, &found)
        || !ir_abcd_type_bounds(function, operand, &type)) return true;
    ir_interval_t range = found.seen;
    bool narrowed = narrowed_value(function, ranges, operand, at);
    ir_int_t width = 0;
    if (!division) {
        ir_interval_t shifted;
        if (!in->dest || !ir_abcd_type_bounds(function, in->dest, &shifted))
            return true;
        width = (ir_int_t)ir_machine_of(function->owner, in->dest->type).width;
    }
    bool always = division ? range.lo == 0 && range.hi == 0
                           : range.lo >= width || range.hi < 0;
    bool may = division ? range.lo <= 0 && range.hi >= 0
                        : range.hi >= width || range.lo < 0;
    if (!always && !(may && narrowed)) return true;
    core_string_t shown = format_interval(range);
    report_at(unit, division
                  ? always ? N00B_DIAG_DIVISOR_ZERO
                           : N00B_DIAG_DIVISOR_MAY_BE_ZERO
                  : always ? N00B_DIAG_SHIFT_OUT_OF_RANGE
                           : N00B_DIAG_SHIFT_MAY_BE_OUT_OF_RANGE, anchor,
              division ? always ? N00B_TEXT_DIVISOR_ALWAYS_INVALID
                                : N00B_TEXT_DIVISOR_MAY_BE_INVALID
                       : always ? N00B_TEXT_SHIFT_ALWAYS_INVALID
                                : N00B_TEXT_SHIFT_MAY_BE_INVALID,
              N00BB_ARGS(ARG_TEXT(shown)));
    core_free(shown.data);
    return !always;
}

static core_list_t(n00bb_ir_func_ptr_t)
source_functions(n00bb_link_analysis_t *analysis)
{
    core_list_t(n00bb_ir_func_ptr_t) functions =
        core_list_new(n00bb_ir_func_ptr_t);
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        ir_module_t *module = state && !state->interface_module
            ? state->ir_module : nullptr;
        for (size_t f = 0; module && f < ir_func_count(module); f++) {
            ir_func_t *function = ir_func_at(module, f);
            if (function->n_blocks
                && th_is_closed(ir_module_types(module), function->type))
                core_list_push(functions, function);
        }
    }
    return functions;
}

static bool *
feasible_blocks(ir_abcd_t *ranges, ir_func_t *function)
{
    bool *seen = core_alloc_array(bool, function->n_blocks);
    core_list_t(uint32_t) work = core_list_new(uint32_t);
    if (!seen) return nullptr;
    seen[0] = true;
    core_list_push(work, 0);
    while (work.len) {
        ir_block_t *block = function->blocks[work.data[--work.len]];
        ir_instr_t *last = block->n_instrs
            ? block->instrs[block->n_instrs - 1] : nullptr;
        comparison_fact_t fact = last && last->op == IR_OP_BRANCH
                && last->n_operands == 3
            ? condition_fact(ranges, ir_operand_value(&last->operands[0]),
                             last, 0)
            : COMPARISON_UNKNOWN;
        for (uint32_t o = 0; last && o < last->n_operands; o++) {
            const ir_operand_t *operand = &last->operands[o];
            ir_block_t *next = operand->kind == IR_OPD_BLOCK
                    || operand->kind == IR_OPD_DEFAULT_BLOCK
                ? operand->block
                : operand->kind == IR_OPD_SWITCH_ARM ? operand->arm.block
                : nullptr;
            if (!next || seen[next->index]
                || (fact == COMPARISON_TRUE && o == 2)
                || (fact == COMPARISON_FALSE && o == 1)) continue;
            seen[next->index] = true;
            core_list_push(work, next->index);
        }
    }
    core_list_free(work);
    return seen;
}

bool
diagnose_ranges(n00bb_link_analysis_t *analysis)
{
    bool ok = true;
    core_list_t(n00bb_ir_func_ptr_t) functions = source_functions(analysis);
    for (size_t f = 0; f < functions.len; f++) {
        ir_func_t *function = functions.data[f];
        core_translation_unit_t *unit = unit_for_ir_module(analysis,
                                                           function->owner);
        if (!unit) continue;
        size_t scope = ir_analysis_scope_begin();
        ir_abcd_t *ranges = ir_func_abcd(function);
        ir_sites_t *sites = ranges ? ir_func_sites(function) : nullptr;
        bool *feasible = sites ? feasible_blocks(ranges, function) : nullptr;
        for (uint32_t s = 0; feasible && s < ir_sites_count(sites); s++) {
            const ir_site_t *site = ir_site_at(sites, s);
            if (!site->instr->block
                || !feasible[site->instr->block->index]) continue;
            if ((site->kind == IR_SITE_BRANCH
                 || site->kind == IR_SITE_LOOP_BOUND)
                && site->instr->op == IR_OP_BRANCH)
                check_constant_condition(unit, ranges, site->instr);
            else if ((site->kind == IR_SITE_DIVISOR
                      || site->kind == IR_SITE_SHIFT_AMOUNT)
                     && user_span(site->instr))
                ok = check_operand_domain(unit, function, ranges, site)
                    && ok;
        }
        core_free(feasible);
        ir_analysis_scope_end(scope);
    }
    core_list_free(functions);
    return ok;
}

static ir_lock_role_t
lock_role(const ir_instr_t *in)
{
    const n00bb_axiom_t *axiom = n00bb_axiom_of_function(
        n00bb_direct_call_target(in));
    uint8_t roles = axiom ? axiom->roles : 0;
    if (roles & N00BB_AXIOM_LOCK_ACQUIRE) return IR_LOCK_ACQUIRE;
    if (roles & N00BB_AXIOM_LOCK_RELEASE) return IR_LOCK_RELEASE;
    if (roles & N00BB_AXIOM_LOCK_SYNC) return IR_LOCK_SYNC;
    return IR_LOCK_NONE;
}

static const ir_value_t *
copied_source(const ir_value_t *value)
{
    for (uint32_t depth = 0; value && value->kind == IR_VAL_INSTR
         && value->def && depth < 8; depth++) {
        const ir_instr_t *def = value->def;
        if ((def->op != IR_OP_COPY && def->op != IR_OP_RETAIN_REF
             && def->op != IR_OP_ADDRESS_OF) || def->n_operands != 1)
            break;
        value = ir_operand_value(&def->operands[0]);
    }
    return value;
}

static uint64_t
entry_param_mask(const core_list_t(entry_param_t) *params,
                 const ir_func_t *function)
{
    const n00bb_axiom_t *axiom = n00bb_axiom_of_function(function);
    if (axiom && axiom->entries) return axiom->entries;
    for (size_t i = 0; i < params->len; i++)
        if (params->data[i].function == function)
            return params->data[i].mask;
    return 0;
}

// A function that starts threads without saying which argument runs on
// them, such as an FFI binding declared `effects threads, of entry`: any
// callback it is given may be a thread's entry.
static bool
starts_threads(ir_summaries_t *summaries, const ir_func_t *function)
{
    ir_summary_t *summary = summaries
        ? ir_summary_of(summaries, (ir_func_t *)function) : nullptr;
    return summary
        && (ir_summary_flags(summary) & IR_SUMMARY_STARTS_THREADS);
}

// The function a thread entry argument runs: a function value, or the
// handler of a callback made from one.
static const ir_value_t *
entry_function_value(const ir_value_t *argument)
{
    const ir_value_t *value = copied_source(argument);
    const ir_instr_t *creation = value ? value->def : nullptr;
    if (creation && creation->op == IR_OP_CALLBACK_CREATE
        && creation->n_operands && creation->operands[0].kind == IR_OPD_VALUE)
        value = copied_source(creation->operands[0].value);
    return value;
}

static bool
add_entry_param(core_list_t(entry_param_t) *params, ir_func_t *function,
                uint64_t bit)
{
    for (size_t i = 0; i < params->len; i++)
        if (params->data[i].function == function) {
            if (params->data[i].mask & bit) return false;
            params->data[i].mask |= bit;
            return true;
        }
    core_list_push(*params, ((entry_param_t){function, bit}));
    return true;
}

static void
visit_function_values(const core_list_t(n00bb_ir_func_ptr_t) *functions,
                      function_value_fn visit, void *context)
{
    for (size_t f = 0; f < functions->len; f++) {
        ir_func_t *function = functions->data[f];
        for (uint32_t b = 0; b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                ir_instr_t *in = function->blocks[b]->instrs[i];
                for (uint32_t o = in->op == IR_OP_CALL ? 1 : 0;
                     o < in->n_operands; o++) {
                    const ir_value_t *value =
                        ir_operand_value(&in->operands[o]);
                    if (value && value->kind == IR_VAL_FUNC && value->owner)
                        visit(context, value);
                }
            }
    }
}

static void
add_taken_entry(void *context, const ir_value_t *value)
{
    taken_entries_t *taken = context;
    ir_func_t *body = n00bb_cached_body(value->owner);
    if (body->n_blocks && core_dict_add(&taken->listed, body, 1))
        core_list_push(*taken->entries, body);
}

void
add_address_taken_functions(const core_list_t(n00bb_ir_func_ptr_t) *functions,
                            core_list_t(n00bb_ir_func_ptr_t) *entries)
{
    taken_entries_t taken = {.entries = entries};
    core_dict_init(&taken.listed, nullptr, nullptr);
    for (size_t e = 0; e < entries->len; e++)
        core_dict_put(&taken.listed, entries->data[e], 1);
    visit_function_values(functions, add_taken_entry, &taken);
    core_dict_free(&taken.listed);
}

static void
note_taken_value(void *context, const ir_value_t *value)
{
    taken_values_t *taken = context;
    const n00bb_axiom_t *axiom = n00bb_axiom_of_function(value->owner);
    taken->entry_natives |= axiom && axiom->entries;
    core_dict_put(&taken->taken, n00bb_cached_body(value->owner), 1);
}

core_list_t(n00bb_ir_func_ptr_t)
thread_entries(const core_list_t(n00bb_ir_func_ptr_t) *functions,
               ir_summaries_t *summaries, bool *complete,
               core_list_t(entry_param_t) *params_out)
{
    core_list_t(entry_param_t) params = core_list_new(entry_param_t);
    core_list_t(n00bb_ir_func_ptr_t) entries =
        core_list_new(n00bb_ir_func_ptr_t);
    core_dict_t listed;
    core_dict_init(&listed, nullptr, nullptr);
    *complete = true;
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t f = 0; f < functions->len; f++) {
            ir_func_t *function = functions->data[f];
            for (uint32_t b = 0; b < function->n_blocks; b++)
                for (uint32_t i = 0; i < function->blocks[b]->n_instrs;
                     i++) {
                    ir_instr_t *in = function->blocks[b]->instrs[i];
                    ir_func_t *target = n00bb_direct_call_target(in);
                    uint64_t mask = target
                        ? entry_param_mask(&params, target) : 0;
                    bool by_effect = !mask && target
                        && starts_threads(summaries, target);
                    if (by_effect) mask = UINT64_MAX;
                    for (uint32_t p = 0; mask && p < 64
                         && p + 1 < in->n_operands; p++) {
                        if (!(mask >> p & 1)) continue;
                        const ir_value_t *argument = entry_function_value(
                            ir_operand_value(&in->operands[p + 1]));
                        ir_func_t *entry = argument
                                && argument->kind == IR_VAL_FUNC
                                && argument->owner
                            ? n00bb_cached_body(argument->owner) : nullptr;
                        if (entry) {
                            if (core_dict_add(&listed, entry, 1))
                                core_list_push(entries, entry);
                        }
                        else if (!by_effect
                                 && (!argument
                                     || argument->kind != IR_VAL_PARAM))
                            *complete = false;
                        for (uint32_t q = 0; argument
                             && argument->kind == IR_VAL_PARAM
                             && q < function->n_params && q < 64; q++)
                            if (function->params[q] == argument)
                                changed |= add_entry_param(&params, function,
                                                           UINT64_C(1) << q);
                    }
                }
        }
    }
    core_dict_free(&listed);
    if (*complete) {
        taken_values_t taken = {0};
        core_dict_init(&taken.taken, nullptr, nullptr);
        visit_function_values(functions, note_taken_value, &taken);
        *complete = !taken.entry_natives;
        for (size_t i = 0; *complete && i < params.len; i++)
            *complete = !core_dict_contains(&taken.taken,
                                            params.data[i].function);
        core_dict_free(&taken.taken);
    }
    if (params_out) *params_out = params;
    else core_list_free(params);
    return entries;
}

bool
atomic_type(th_type *type)
{
    core_string_t name = named_type_name(type);
    return name.data && (core_string_eq(name, CORE_STRING_STATIC("Atomic"))
        || core_string_ends_with(name, CORE_STRING_STATIC(".Atomic")));
}

static bool
atomic_access(const ir_instr_t *in)
{
    return in->op == IR_OP_ATOMIC_RMW || in->op == IR_OP_ATOMIC_CAS
        || ir_instr_has_mod((ir_instr_t *)in, CORE_STRING_STATIC("atomic"));
}

static ir_lock_role_t
race_lock_role(void *context, const ir_instr_t *in)
{
    (void)context;
    return lock_role(in);
}

static ir_func_t *
race_direct_target(void *context, const ir_instr_t *in)
{
    (void)context;
    return n00bb_direct_call_target(in);
}

static bool
race_atomic(void *context, const ir_instr_t *in, const ir_splace_t *place)
{
    (void)context;
    return (in && atomic_access(in))
        || (place && place->global && atomic_type(place->global->type));
}

static race_start_t *
race_start_for(race_starts_t *starts, ir_func_t *entry)
{
    size_t slot = (uintptr_t)core_dict_get(&starts->index, entry, nullptr);
    if (slot) return &starts->list.data[slot - 1];
    core_list_push(starts->list, ((race_start_t){
        .entry = entry, .arguments = core_list_new(ir_splace_t)}));
    core_dict_put(&starts->index, entry, starts->list.len);
    return &starts->list.data[starts->list.len - 1];
}

static void
add_argument_places(race_start_t *start, ir_func_t *function,
                    const ir_value_t *argument)
{
    ir_places_t *places = ir_func_places(function);
    const uint32_t *ids = nullptr;
    uint32_t count = 0;
    if (!places || !argument
        || !ir_places_of(places, argument, &ids, &count)) return;
    for (uint32_t p = 0; p < count; p++) {
        ir_splace_t place;
        if (ir_summary_place_of(function, ir_place_at(places, ids[p]),
                                &place))
            core_list_push(start->arguments, place);
    }
}

static void
collect_race_starts(const core_list_t(n00bb_ir_func_ptr_t) *functions,
                    const core_list_t(entry_param_t) *params,
                    ir_summaries_t *summaries, race_starts_t *starts)
{
    for (size_t f = 0; f < functions->len; f++) {
        ir_func_t *function = functions->data[f];
        size_t scope = ir_analysis_scope_begin();
        ir_live_t *live = ir_func_liveness(function);
        for (uint32_t b = 0; b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                ir_instr_t *in = function->blocks[b]->instrs[i];
                ir_func_t *target = n00bb_direct_call_target(in);
                uint64_t mask = target ? entry_param_mask(params, target) : 0;
                if (!mask && target && starts_threads(summaries, target))
                    mask = UINT64_MAX;
                for (uint32_t p = 0; mask && p < 64
                     && p + 1 < in->n_operands; p++) {
                    if (!(mask >> p & 1)) continue;
                    const ir_value_t *argument = entry_function_value(
                        ir_operand_value(&in->operands[p + 1]));
                    if (!argument || argument->kind != IR_VAL_FUNC
                        || !argument->owner) continue;
                    race_start_t *start = race_start_for(starts,
                        n00bb_cached_body(argument->owner));
                    start->starts += ir_live_block_on_cycle(live, in->block)
                        ? 2 : 1;
                    for (uint32_t o = 1; o < in->n_operands; o++)
                        if (o != p + 1)
                            add_argument_places(start, function,
                                ir_operand_value(&in->operands[o]));
                }
            }
        ir_analysis_scope_end(scope);
    }
}

bool
annotated_true(const ir_func_t *function, core_string_t key)
{
    ir_annot_t *annot = ir_annot_get((ir_annots_t *)&function->annots, key);
    return annot && annot->kind == IR_ANNOT_BOOL && annot->b;
}

static void
report_race(void *context, const ir_race_t *race)
{
    n00bb_link_analysis_t *analysis = context;
    core_translation_unit_t *unit = unit_for_ir_module(analysis,
        race->site->block->owner->owner);
    if (!unit || tu_state(unit)->interface_module) return;
    core_string_t global = n00bb_name_leaf(race->place.global->name);
    report_at(unit, N00B_DIAG_DATA_RACE, race->site,
              race->self ? N00B_TEXT_RACE_SAME_ENTRY : N00B_TEXT_RACE,
              N00BB_ARGS(ARG_TEXT(global), ARG_TEXT(race->entry->name)));
    n00bb_report_help(core_translation_unit_diagnostics(unit),
        N00B_TEXT_MAKE_ATOMIC_OR_GUARD_EVERY,
        N00BB_ARGS(ARG_TEXT(global)));
}

void
warn_shared_global_races(n00bb_link_analysis_t *analysis,
    const core_list_t(n00bb_ir_func_ptr_t) *runtime_entries)
{
    core_list_t(n00bb_ir_func_ptr_t) main_roots =
        core_list_new(n00bb_ir_func_ptr_t);
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    for (size_t u = 0; u < analysis->units.len; u++) {
        ir_module_t *module = unit_module(tu_state(analysis->units.data[u]));
        for (size_t f = 0; module && f < ir_func_count(module); f++) {
            ir_func_t *function = ir_func_at(module, f);
            if (annotated_true(function, CORE_STRING_STATIC("ir.initializer"))
                && core_dict_add(&seen, function, 1))
                core_list_push(main_roots, function);
        }
    }
    for (size_t f = 0; f < runtime_entries->len; f++)
        if (core_dict_add(&seen, runtime_entries->data[f], 1))
            core_list_push(main_roots, runtime_entries->data[f]);
    core_dict_free(&seen);
    core_dict_init(&seen, nullptr, nullptr);
    core_list_t(n00bb_ir_func_ptr_t) everything =
        core_list_new(n00bb_ir_func_ptr_t);
    for (size_t f = 0; f < analysis->emitted_functions.len; f++)
        if (core_dict_add(&seen, analysis->emitted_functions.data[f], 1))
            core_list_push(everything, analysis->emitted_functions.data[f]);
    for (size_t f = 0; f < main_roots.len; f++)
        if (core_dict_add(&seen, main_roots.data[f], 1))
            core_list_push(everything, main_roots.data[f]);
    for (size_t f = 0; f < everything.len; f++) {
        ir_func_t *function = everything.data[f];
        for (uint32_t b = 0; b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                ir_func_t *callee = n00bb_direct_call_target(
                    function->blocks[b]->instrs[i]);
                if (callee && callee->n_blocks
                    && core_dict_add(&seen, callee, 1))
                    core_list_push(everything, callee);
            }
    }
    core_dict_free(&seen);
    bool complete = true;
    core_list_t(entry_param_t) params = {0};
    core_list_t(n00bb_ir_func_ptr_t) entries = thread_entries(
        &everything, analysis->summaries, &complete, &params);
    if (!complete) add_address_taken_functions(&everything, &entries);
    race_starts_t starts = {.list = core_list_new(race_start_t)};
    core_dict_init(&starts.index, nullptr, nullptr);
    collect_race_starts(&everything, &params, analysis->summaries, &starts);
    core_list_free(params);
    core_list_free(everything);
    core_dict_t threaded;
    core_dict_init(&threaded, nullptr, nullptr);
    core_list_t(ir_race_entry_t) race_entries =
        core_list_new(ir_race_entry_t);
    for (size_t e = 0; e < entries.len; e++) {
        core_dict_put(&threaded, entries.data[e], 1);
        race_start_t *start = race_start_for(&starts, entries.data[e]);
        core_list_push(race_entries, ((ir_race_entry_t){
            .entry = entries.data[e],
            .starts = start->starts ? start->starts : 2,
            .arguments = start->arguments.data,
            .argument_count = (uint32_t)start->arguments.len,
        }));
    }
    core_list_t(n00bb_ir_func_ptr_t) main_entries =
        core_list_new(n00bb_ir_func_ptr_t);
    for (size_t f = 0; f < main_roots.len; f++)
        if (!core_dict_contains(&threaded, main_roots.data[f]))
            core_list_push(main_entries, main_roots.data[f]);
    core_dict_free(&threaded);
    core_list_free(main_roots);
    ir_race_hooks_t hooks = {
        .atomic = race_atomic, .lock_role = race_lock_role,
        .direct_target = race_direct_target,
    };
    if (race_entries.len)
        ir_races_find(analysis->summaries, main_entries.data,
                      main_entries.len, race_entries.data, race_entries.len,
                      &hooks, report_race, analysis);
    for (size_t s = 0; s < starts.list.len; s++)
        core_list_free(starts.list.data[s].arguments);
    core_list_free(starts.list);
    core_dict_free(&starts.index);
    core_list_free(race_entries);
    core_list_free(main_entries);
    core_list_free(entries);
}

core_string_t
named_type_name(th_type *type)
{
    core_string_t name = {0};
    type = type ? th_resolve(type) : nullptr;
    if (!type || th_kind_of(type) != TH_NAMED
        || !th_named_info(type, &name, nullptr, nullptr))
        return (core_string_t){0};
    return name;
}

static bool
calls_failed_function(n00bb_link_analysis_t *analysis, const ir_instr_t *in)
{
    const ir_func_t *target = ir_call_function(in);
    core_translation_unit_t *unit = target && ir_span_known(target->span)
        ? unit_for_ir_module(analysis, target->owner) : nullptr;
    core_diagnostics_t *diagnostics = unit
        ? core_translation_unit_diagnostics(unit) : nullptr;
    for (size_t i = 0; diagnostics && i < core_diagnostics_count(diagnostics);
         i++) {
        const core_diagnostic_t *item = core_diagnostics_at(diagnostics, i);
        if ((item->severity == CORE_DIAG_ERROR
             || item->severity == CORE_DIAG_FATAL)
            && item->location.has_location
            && span_contains(target->span, item->location))
            return true;
    }
    return false;
}

static void
warn_ignored_results(n00bb_link_analysis_t *analysis,
                     ir_func_t *const *functions, size_t count)
{
    for (size_t f = 0; f < count; f++) {
        ir_func_t *function = functions[f];
        core_translation_unit_t *unit = function->n_blocks
            ? unit_for_ir_module(analysis, function->owner) : nullptr;
        if (!unit || tu_state(unit)->interface_module) continue;
        ir_summary_facts_t *facts = group_facts(analysis, function, false);
        for (uint32_t b = 0; facts && b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                const ir_instr_t *in = function->blocks[b]->instrs[i];
                if (calls_failed_function(analysis, in)
                    || !ir_summary_facts_ignored_result(facts, in)) continue;
                n00bb_rule_chain_t chain = {.ignored = true};
                ir_summary_facts_explain_ignored_result(
                    group_facts(analysis, function, true), in,
                    describe_rule_step, &chain);
                report_chain(core_translation_unit_diagnostics(unit), unit,
                    N00B_DIAG_CALL_WITHOUT_EFFECT, n00bb_span_location(in->span),
                    N00B_TEXT_RESULT_IGNORED, N00B_TEXT_RESULT_IGNORED_BECAUSE,
                    &chain);
            }
    }
}

static void
report_dead_store(const ir_instr_t *store, void *context)
{
    core_translation_unit_t *unit = context;
    const ir_value_t *stored = store->op == IR_OP_STORE
            && store->n_operands > 1
        ? ir_operand_value(&store->operands[1]) : nullptr;
    if (stored && stored->kind == IR_VAL_PARAM) return;
    n00bb_report(core_translation_unit_diagnostics(unit), unit,
        N00B_DIAG_UNUSED_ASSIGNMENT, n00bb_span_location(store->span),
        N00B_TEXT_THIS_STORED_VALUE_IS_OVERWRITTEN,
        N00BB_NO_ARGS);
}

void
warn_dead_stores(n00bb_link_analysis_t *analysis)
{
    for (size_t f = 0; f < analysis->emitted_functions.len; f++) {
        ir_func_t *function = analysis->emitted_functions.data[f];
        core_translation_unit_t *unit = function->n_blocks
            ? unit_for_ir_module(analysis, function->owner) : nullptr;
        if (!unit || tu_state(unit)->interface_module) continue;
        ir_dead_stores(function, report_dead_store, unit);
    }
}

static bool
parameter_exempt(const ir_value_t *parameter)
{
    core_string_t name = parameter ? parameter->name : (core_string_t){0};
    return !name.data || !name.u8_bytes || name.data[0] == '_'
        || core_string_eq(name, CORE_STRING_STATIC("self"));
}

static bool
function_signature_fixed(const ir_func_t *function, const core_dict_t *taken)
{
    static const core_string_t generated[] = {
        CORE_STRING_STATIC("n00bb.generated-value-method"),
        CORE_STRING_STATIC("n00bb.generated-literal"),
        CORE_STRING_STATIC("n00bb.builtin-trait-method"),
        CORE_STRING_STATIC("ir.initializer"),
    };
    for (size_t g = 0; g < sizeof(generated) / sizeof(*generated); g++)
        if (ir_annot_get((ir_annots_t *)&function->annots, generated[g]))
            return true;
    return core_dict_contains((core_dict_t *)taken, (void *)function)
        || (function->name.u8_bytes > 1 && function->name.data[0] == '_'
            && function->name.data[1] == '_');
}

static core_hash_value_t
param_site_hash(void *key)
{
    const param_site_t *site = key;
    return core_hash_raw(site, sizeof(*site));
}

static bool
param_site_eq(const void *a, const void *b)
{
    const param_site_t *left = a;
    const param_site_t *right = b;
    return left->file == right->file && left->start == right->start
        && left->param == right->param;
}

static void
note_taken_owner(void *context, const ir_value_t *value)
{
    core_dict_put(context, value->owner, 1);
}

static parse_sym_entry_t *
ir_function_declaration(n00bb_link_analysis_t *analysis,
                        core_translation_unit_t *unit, ir_func_t *function)
{
    n00bb_tu_state_t *state = tu_state(unit);
    for (parse_scope_t *scope = state && state->annotations
             ? state->annotations->symtab->all_scopes : nullptr;
         scope; scope = scope->all_next)
        for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
             symbol = symbol->next_in_scope) {
            if (symbol->kind != PARSE_SYM_FUNCTION || !symbol->decl_node)
                continue;
            core_link_declaration_t *declaration =
                link_declaration_for_symbol(analysis, symbol);
            if (declaration && core_link_declaration_binding(declaration,
                    CORE_STRING_STATIC("n00bb.ir.function")) == function)
                return symbol;
        }
    return nullptr;
}

static bool
parameter_location(parse_grammar_t *grammar, parse_sym_entry_t *function,
                   core_string_t name, core_diag_location_t *out)
{
    parse_tree_t *signature = function ? child_nt(grammar,
        function->decl_node, CORE_STRING_STATIC("func-type-signature"))
        : nullptr;
    parse_tree_t *formals = child_nt(grammar, signature ? signature
        : function ? function->decl_node : nullptr,
        CORE_STRING_STATIC("param-decl"));
    core_list_t(parse_sym_entry_ptr_t) parameters =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *rest = nullptr;
    if (formals) collect_ir_parameters(grammar, formals, &parameters, &rest);
    if (rest) core_list_push(parameters, rest);
    bool found = false;
    for (size_t i = 0; !found && i < parameters.len; i++)
        if (parameters.data[i] && parameters.data[i]->decl_node
            && core_string_eq(parameters.data[i]->name, name)) {
            *out = link_source_location(parameters.data[i]->decl_node);
            found = true;
        }
    core_list_free(parameters);
    return found;
}

void
warn_unused_params(n00bb_link_analysis_t *analysis)
{
    core_dict_t taken;
    core_dict_init(&taken, nullptr, nullptr);
    core_dict_t reported;
    core_dict_init(&reported, param_site_hash, param_site_eq);
    visit_function_values(&analysis->emitted_functions, note_taken_owner,
                          &taken);
    for (size_t f = 0; f < analysis->emitted_functions.len; f++) {
        ir_func_t *function = analysis->emitted_functions.data[f];
        core_translation_unit_t *unit = function->n_blocks
            ? unit_for_ir_module(analysis, function->owner) : nullptr;
        if (!unit || tu_state(unit)->interface_module
            || function_signature_fixed(function, &taken)) continue;
        ir_span_t span = ir_func_report_span(function);
        if (!span.start_line) continue;
        uint32_t *unused = core_alloc_array(uint32_t, function->n_params + 1);
        uint32_t count = ir_unused_params(function, unused, function->n_params);
        for (uint32_t u = 0; u < count; u++) {
            ir_value_t *parameter = function->params[unused[u]];
            param_site_t site = {
                .file = span.file, .start = span.start_byte,
                .param = unused[u],
            };
            if (parameter_exempt(parameter)
                || core_dict_contains(&reported, &site))
                continue;
            param_site_t *key = core_alloc(param_site_t);
            *key = site;
            core_dict_put(&reported, key, 1);
            core_diag_location_t location = {
                .line = span.start_line,
                .column = span.start_col,
                .end_line = span.end_line,
                .end_column = span.end_col,
                .has_location = true,
            };
            (void)parameter_location(analysis->grammar,
                ir_function_declaration(analysis, unit, function),
                parameter->name, &location);
            n00bb_report(core_translation_unit_diagnostics(unit), unit,
                N00B_DIAG_UNUSED_PARAMETER, location,
                N00B_TEXT_PARAMETER_NEVER_USED,
                N00BB_ARGS(ARG_TEXT(parameter->name)));
            n00bb_report_help(core_translation_unit_diagnostics(unit),
                N00B_TEXT_IF_A_SIGNATURE_REQUIRES_IT,
                N00BB_ARGS(ARG_TEXT(parameter->name)));
        }
        core_free(unused);
    }
    core_dict_foreach(&reported, item) core_free(item->key);
    core_dict_free(&taken);
    core_dict_free(&reported);
}

static void
warn_dead_code(n00bb_link_analysis_t *analysis, ir_func_t *const *functions,
               size_t count, const core_dict_t *instances)
{
    for (size_t f = 0; f < count; f++) {
        ir_func_t *function = functions[f];
        core_translation_unit_t *unit = function->n_blocks
            && !core_dict_contains((core_dict_t *)instances, function)
            ? unit_for_ir_module(analysis, function->owner) : nullptr;
        if (!unit || tu_state(unit)->interface_module) continue;
        ir_dead_code_t *dead = dead_code_of(analysis, unit, function);
        for (uint32_t b = 0; dead && b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                const ir_instr_t *in = function->blocks[b]->instrs[i];
                if (!ir_dead_code_instr(dead, in) || in->span.implied)
                    continue;
                note_dead_code_site(analysis, unit->module.identifier,
                    in->span.start_line, in->span.start_col);
                if (in != ir_block_terminator(function->blocks[b])
                    && ir_span_known(in->span))
                    core_list_push(analysis->dead_regions,
                        ((n00bb_failed_region_t){
                            .module = unit->module.identifier,
                            .span = in->span}));
            }
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(unit);
        for (uint32_t d = 0; d < ir_dead_code_site_count(dead); d++) {
            const ir_dead_site_t *site = ir_dead_code_site_at(dead, d);
            core_diag_location_t at = n00bb_span_location(site->site->span);
            n00bb_rule_chain_t chain = {0};
            if (site->kind == IR_DEAD_AFTER_DIVERGING_CALL) {
                ir_summary_facts_explain_divergence(
                    group_facts(analysis, function, true), site->cause,
                    describe_rule_step, &chain);
                report_chain(diagnostics, unit, N00B_DIAG_UNREACHABLE_CODE,
                    at, N00B_TEXT_CODE_NEVER_RUNS,
                    N00B_TEXT_CODE_NEVER_RUNS_BECAUSE, &chain);
            }
            else
                n00bb_report(diagnostics, unit, N00B_DIAG_UNREACHABLE_CODE,
                    at, site->kind == IR_DEAD_CONSTANT_CONDITION
                        ? N00B_TEXT_CODE_NEVER_RUNS_CONSTANT
                        : N00B_TEXT_CODE_NEVER_RUNS_NO_PATH, N00BB_NO_ARGS);
            if (site->kind == IR_DEAD_AFTER_DIVERGING_CALL && site->cause
                && ir_span_known(site->cause->span))
                n00bb_report_secondary(diagnostics, nullptr,
                    n00bb_span_location(site->cause->span),
                    N00B_TEXT_THIS_CALL_NEVER_RETURNS,
                    N00BB_NO_ARGS);
        }
        ir_dead_code_free(dead);
    }
}

void
warn_rule_diagnostics(n00bb_link_analysis_t *analysis)
{
    n00bb_rule_facts_t *rules = analysis->rule_facts;
    core_dict_t instances;
    core_dict_init(&instances, nullptr, nullptr);
    for (size_t i = 0; i < analysis->specializations.len; i++)
        core_dict_put(&instances, analysis->specializations.data[i].instance,
                      analysis->specializations.data[i].instance);
    for (size_t g = 0; rules && g < rules->count; g++) {
        ir_func_t *const *members = rules->members + rules->starts[g];
        size_t count = rules->starts[g + 1] - rules->starts[g];
        warn_dead_code(analysis, members, count, &instances);
        warn_ignored_results(analysis, members, count);
    }
    core_dict_free(&instances);
}

void
warn_unbounded_recursion(n00bb_link_analysis_t *analysis)
{
    for (size_t f = 0; f < analysis->emitted_functions.len; f++) {
        ir_func_t *function = analysis->emitted_functions.data[f];
        ir_summary_t *summary = ir_summary_of(analysis->summaries, function);
        core_translation_unit_t *unit = summary
                && (ir_summary_flags(summary)
                    & IR_SUMMARY_UNBOUNDED_RECURSION)
            ? unit_for_ir_module(analysis, function->owner) : nullptr;
        if (!unit || tu_state(unit)->interface_module) continue;
        ir_report_unbounded_recursion(analysis->summaries, function,
            attach_ir_diags(analysis->ir_context, nullptr, unit, nullptr));
    }
}

static bool
same_global(const ir_global_t *left, const ir_global_t *right)
{
    if (!left || !right) return false;
    const ir_global_t *a = left->identity ? left->identity : left;
    const ir_global_t *b = right->identity ? right->identity : right;
    return a == b || core_string_eq(left->name, right->name);
}

static const ir_instr_t *
global_write_in(const ir_func_t *function, const ir_global_t *global)
{
    for (uint32_t b = 0; function && b < function->n_blocks; b++) {
        const ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; block && i < block->n_instrs; i++) {
            const ir_instr_t *in = block->instrs[i];
            if (in->op != IR_OP_GLOBAL_STORE || !in->n_operands
                || in->operands[0].kind != IR_OPD_VALUE
                || !in->operands[0].value
                || !ir_span_known(in->span)) continue;
            if (!global || same_global(in->operands[0].value->global, global))
                return in;
        }
    }
    return nullptr;
}

static bool
identifier_byte(uint8_t byte)
{
    return byte == '_' || byte >= 0x80 || (byte >= '0' && byte <= '9')
        || (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
}

core_diag_location_t
n00bb_name_location(core_translation_unit_t *unit, ir_span_t span,
              core_string_t name)
{
    core_string_t source = unit ? core_source_file_contents(&unit->source)
                                : (core_string_t){0};
    char *loaded = nullptr;
    size_t loaded_size = 0;
    if (!source.u8_bytes && unit
        && core_os_read_file(core_source_file_path(&unit->source), &loaded,
                             &loaded_size))
        source = core_string_borrow(loaded, loaded_size);
    core_diag_location_t found = {0};
    if (!source.data || !name.u8_bytes || !ir_span_known(span)
        || span.end_byte > source.u8_bytes || span.start_byte > span.end_byte) {
        core_free(loaded);
        return found;
    }
    for (size_t at = span.start_byte;
         !found.has_location && at + name.u8_bytes <= span.end_byte; at++) {
        if (!core_string_eq(core_string_slice(source, at, name.u8_bytes),
                            name)) continue;
        bool starts = at == 0 || !identifier_byte((uint8_t)source.data[at - 1]);
        size_t after = at + name.u8_bytes;
        bool ends = after >= source.u8_bytes
            || !identifier_byte((uint8_t)source.data[after]);
        if (starts && ends)
            found = core_diag_location_span(source, at, after);
    }
    core_free(loaded);
    return found;
}

bool
validate_assert_side_effects(n00bb_link_analysis_t *analysis)
{
    bool ok = true;
    for (size_t i = 0; i < analysis->assert_sites.len; i++) {
        n00bb_assert_site_t *site = &analysis->assert_sites.data[i];
        ir_summary_facts_t *facts = ir_summary_of(analysis->summaries,
                                                  site->function)
            ? group_facts(analysis, site->function, false) : nullptr;
        const ir_instr_t *call = facts
            ? assert_side_effect_call(facts, site) : nullptr;
        if (!call) continue;
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(site->unit);
        n00bb_rule_chain_t chain = {0};
        ir_summary_facts_explain_side_effect(
            group_facts(analysis, site->function, true), call,
            describe_rule_step, &chain);
        report_chain(diagnostics, site->unit, N00B_DIAG_ASSERT_SIDE_EFFECT,
            n00bb_span_location(site->span), N00B_TEXT_ASSERT_SIDE_EFFECT,
            N00B_TEXT_ASSERT_SIDE_EFFECT_BECAUSE, &chain);
        if (ir_span_known(call->span)
            && (call->span.start_byte != site->span.start_byte
                || call->span.end_byte != site->span.end_byte))
            n00bb_report_secondary(diagnostics, nullptr,
                n00bb_span_location(call->span),
                N00B_TEXT_THE_SIDE_EFFECT_STARTS_HERE,
                N00BB_NO_ARGS);
        const ir_instr_t *write = chain.writer
            ? global_write_in(chain.writer, chain.written) : nullptr;
        core_translation_unit_t *writer_unit = chain.writer
            ? unit_for_ir_module(analysis, chain.writer->owner) : nullptr;
        if (write && writer_unit && chain.written) {
            core_string_t global = n00bb_name_leaf(chain.written->name);
            n00bb_report_secondary(diagnostics, writer_unit,
                n00bb_span_location(write->span), N00B_TEXT_IS_WRITTEN_HERE,
                N00BB_ARGS(ARG_TEXT(global)));
        }
        n00bb_report_help(diagnostics, N00B_TEXT_ASSERT_CONDITIONS_DO_NOT_RUN,
            N00BB_NO_ARGS);
        ok = false;
    }
    core_list_free(analysis->assert_sites);
    analysis->assert_sites = (core_list_t(n00bb_assert_site_t)){0};
    return ok;
}

bool
place_unit_allocations(n00bb_link_analysis_t *analysis)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        if (state && state->ir_module && !state->interface_module
            && !n00bb_place_storage(analysis->ir_context, analysis->escapes,
                                    state->ir_module)) return false;
    }
    return true;
}

bool
n00bb_recompute_summaries(n00bb_link_analysis_t *analysis)
{
    core_list_t(n00bb_ir_func_ptr_t) none = {0};
    return build_function_summaries(analysis, &none, &none);
}

static uint8_t
thread_runtime_entry_state(
    n00bb_runtime_init_analysis_t *runtime, ir_global_t *global,
    bool has_static_default, bool *phase_succeeds)
{
    bool definitely_initialized, has_successful_write;
    runtime_global_final_state(runtime, runtime->initializer_roots.data,
        runtime->initializer_roots.len, global, phase_succeeds,
        &definitely_initialized, &has_successful_write);
    if (!*phase_succeeds) return 0;
    if (has_static_default || definitely_initialized) return IR_PATH_PASSED;
    if (has_successful_write) return IR_PATH_UNPASSED | IR_PATH_PASSED;
    return IR_PATH_UNPASSED;
}

static size_t
initializer_order_index(core_list_t(core_translation_unit_ptr_t) *order,
                        core_translation_unit_t *unit)
{
    for (size_t i = 0; order && i < order->len; i++)
        if (order->data[i] == unit) return i;
    return SIZE_MAX;
}

static bool
initializer_write_precedes_read(
    n00bb_runtime_init_analysis_t *runtime,
    core_list_t(core_translation_unit_ptr_t) *initializer_order,
    const n00bb_initializer_read_t *read)
{
    bool different_units = read->target != read->source;
    if (different_units) {
        size_t target_order = initializer_order_index(initializer_order,
                                                      read->target);
        size_t source_order = initializer_order_index(initializer_order,
                                                      read->source);
        if (target_order == SIZE_MAX || source_order == SIZE_MAX
            || target_order >= source_order) return false;
    }
    ir_module_t *target_module = unit_module(tu_state(read->target));
    ir_func_t *initializer = target_module
        ? n00bb_cached_body(ir_func_find(target_module,
                       CORE_STRING_STATIC(N00BB_MODULE_INIT_NAME)))
        : nullptr;
    if (!initializer || !runtime_init_is_target(runtime, read->global))
        return false;
    if (different_units) {
        bool succeeds, initialized, has_write;
        runtime_global_final_state(runtime, &initializer, 1, read->global,
            &succeeds, &initialized, &has_write);
        return succeeds && initialized && has_write;
    }
    ir_splace_t place = ir_splace_global(read->global);
    return ir_summary_state_before(runtime->summaries, read->initializer,
               &place, IR_PATH_UNPASSED, read->read) == IR_PATH_PASSED;
}

static parse_tree_t *
initializer_read_node(const n00bb_initializer_read_t *read)
{
    n00bb_tu_state_t *state = tu_state(read->source);
    for (size_t i = 0; state && i < state->link_use_sites.len; i++) {
        n00bb_link_use_site_t *use = &state->link_use_sites.data[i];
        parse_token_info_t *token = use->symbol == read->symbol
                && use->in_module_initializer
            ? parse_tree_find_first_terminal(use->node) : nullptr;
        if (token && span_covers(read->read->span, parse_token_line(token),
                                 parse_token_column(token)))
            return use->node;
    }
    return nullptr;
}

static bool
global_has_writer(n00bb_link_analysis_t *analysis,
                  n00bb_runtime_init_analysis_t *runtime, ir_global_t *global)
{
    core_dict_foreach(&runtime->reachable, item)
        if (summary_writes_global(ir_summary_of(analysis->summaries,
                                                (ir_func_t *)item->key),
                                  global))
            return true;
    return false;
}

static core_string_t
read_path(const ir_instr_t *read)
{
    const ir_func_t *callee = ir_call_function(read);
    if (callee)
        return n00bb_text(N00B_TEXT_READ_THROUGH_CALL,
                          N00BB_ARGS(ARG_TEXT(callee->name)));
    if (read && (read->op == IR_OP_CALL || read->op == IR_OP_CALLBACK_INVOKE))
        return n00bb_text(N00B_TEXT_READ_THROUGH_INDIRECT_CALL,
                          N00BB_NO_ARGS);
    return (core_string_t){0};
}

bool
validate_uninitialized_global_reads(
    core_link_unit_t *link, n00bb_link_analysis_t *analysis,
    n00bb_runtime_init_analysis_t *runtime,
    core_list_t(core_translation_unit_ptr_t) *initializer_order,
    const core_list_t(n00bb_initializer_read_t) *reads)
{
    if (!link || !analysis) return false;
    for (size_t i = 0; i < reads->len; i++) {
        const n00bb_initializer_read_t *read = &reads->data[i];
        parse_sym_entry_t *target = read->symbol;
        parse_tree_t *node = initializer_read_node(read);
        bool explicit_initializer = global_has_explicit_initializer(
            analysis->grammar, target);
        if (read->source == read->target && explicit_initializer
            && dynamic_initializer_declaration(analysis->grammar,
                   link_declaration_for_symbol(analysis, target))) {
            core_diag_location_t location = initializer_read_location(read);
            parse_token_info_t *declared = parse_tree_find_first_terminal(
                symbol_binding_node(target));
            if (declared && location.line
                && (location.line < parse_token_line(declared)
                    || (location.line == parse_token_line(declared)
                        && location.column < parse_token_column(declared)))) {
                core_string_t path = read_path(read->read);
                core_diagnostics_t *diagnostics =
                    core_translation_unit_diagnostics(read->source);
                n00bb_report(diagnostics, read->source,
                    N00B_DIAG_GLOBAL_READ_BEFORE_INIT, location,
                    N00B_TEXT_READS_BEFORE_INITIALIZER,
                    N00BB_ARGS(ARG_TEXT(target->name), ARG_TEXT(path)));
                core_free(path.data);
                n00bb_report_secondary(diagnostics, nullptr,
                    link_source_location(symbol_binding_node(target)),
                    N00B_TEXT_IS_INITIALIZED_HERE,
                    N00BB_ARGS(ARG_TEXT(target->name)));
                n00bb_report_help(diagnostics,
                    N00B_TEXT_MOVE_THE_DECLARATION_OF_ABOVE,
                    N00BB_ARGS(ARG_TEXT(target->name)));
            }
            continue;
        }
        if (explicit_initializer
            || (node && initializer_node_is_reference_store_target(
                    analysis->grammar, read->source, node, target))
            || initializer_write_precedes_read(runtime, initializer_order,
                                               read))
            continue;
        core_translation_unit_t *source_unit = read->source;
        if (n00bb_const_initializer(ir_call_function(read->read))) {
            core_diagnostics_t *diagnostics =
                core_translation_unit_diagnostics(source_unit);
            n00bb_report(diagnostics, source_unit,
                N00B_DIAG_CONST_READS_RUNTIME_VAR,
                initializer_read_location(read),
                N00B_TEXT_CONST_READS_RUNTIME_VAR,
                N00BB_ARGS(ARG_TEXT(target->name)));
            n00bb_report_secondary(diagnostics, read->target,
                link_source_location(target->decl_node),
                N00B_TEXT_IS_DECLARED_HERE,
                N00BB_ARGS(ARG_TEXT(target->name)));
            n00bb_report_help(diagnostics, N00B_TEXT_SET_IT_BEFORE_THE_CONST,
                              N00BB_ARGS(ARG_TEXT(target->name)));
            continue;
        }
        bool same_cycle = initializer_units_share_cycle(
            link, source_unit, read->target);
        bool write_candidate = global_has_writer(analysis, runtime,
                                                 read->global);
        core_string_t global = n00bb_text(target->is_thread_local
                ? N00B_TEXT_PLACE_THREAD_GLOBAL : N00B_TEXT_PLACE_GLOBAL,
            N00BB_ARGS(ARG_TEXT(target->name)));
        core_string_t path = read_path(read->read);
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(source_unit);
        n00bb_report(diagnostics, source_unit,
            N00B_DIAG_GLOBAL_READ_BEFORE_INIT, initializer_read_location(read),
            N00B_TEXT_INITIALIZER_READS_GLOBAL,
            N00BB_ARGS(ARG_PHRASE_ID(same_cycle
                           ? N00B_TEXT_CYCLE_INITIALIZATION
                           : N00B_TEXT_THIS_INITIALIZER),
                       ARG_TEXT(global),
                       ARG_PHRASE_ID(write_candidate || target->is_thread_local
                           ? N00B_TEXT_BEFORE_GUARANTEED_INIT
                           : N00B_TEXT_HAS_NO_EXPLICIT_INITIALIZER),
                       ARG_TEXT(path)));
        core_free(global.data);
        core_free(path.data);
        n00bb_report_secondary(diagnostics, read->target,
            link_source_location(target->decl_node), N00B_TEXT_IS_DECLARED_HERE,
            N00BB_ARGS(ARG_TEXT(target->name)));
    }
    return true;
}

bool
validate_thread_function_reads(
    core_link_unit_t *link, n00bb_link_analysis_t *analysis,
    n00bb_runtime_init_analysis_t *runtime,
    bool comptime_phase_succeeds)
{
    if (!link || !analysis || !runtime) return false;
    if (!comptime_phase_succeeds) return true;
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        if (!symtab) return false;
        for (parse_scope_t *scope = symtab->all_scopes; scope;
             scope = scope->all_next) {
            for (parse_sym_entry_t *thread_global = scope->first_in_scope;
                 thread_global; thread_global = thread_global->next_in_scope) {
                if (!is_module_global_symbol(analysis->grammar,
                        thread_global) || !thread_global->is_thread_local)
                    continue;
                ir_global_t *global = link_ir_global_for_symbol(analysis,
                                                                thread_global);
                if (!global || !runtime_init_is_target(runtime, global))
                    return false;
                ir_splace_t place = ir_splace_global(global);
                bool runtime_succeeds = false;
                uint8_t entry_state = thread_runtime_entry_state(
                    runtime, global,
                    global->has_static_initializer
                        || (thread_global->has_comptime_value
                            && thread_global->comptime_value)
                        || n00bb_comptime_seeds(unit_for_ir_module(analysis,
                               ir_global_identity(global)->owner), global),
                    &runtime_succeeds);
                for (size_t f = 0; runtime_succeeds
                     && f < runtime->entry_functions.len; f++) {
                    ir_func_t *function = runtime->entry_functions.data[f];
                    ir_instr_t *read = first_uninitialized_read(
                        analysis, function, &place, entry_state);
                    core_translation_unit_t *unit = read
                        ? unit_for_ir_module(analysis, function->owner)
                        : nullptr;
                    if (!unit) continue;
                    n00bb_report(core_translation_unit_diagnostics(unit), unit,
                        N00B_DIAG_GLOBAL_READ_BEFORE_INIT,
                        n00bb_span_location(read->span),
                        N00B_TEXT_THREAD_READ_BEFORE_INIT,
                        N00BB_ARGS(ARG_TEXT(thread_global->name)));
                }
            }
        }
    }
    return true;
}

typedef struct {
    ir_summary_facts_t *facts;
    const ir_instr_t *origin;
    const ir_local_t *result;
    const ir_value_t *storage;
} missing_return_t;

static uint8_t
missing_return_step(void *context, const ir_instr_t *in,
                    ir_path_outcome_t outcome)
{
    (void)outcome;
    missing_return_t *walk = context;
    if (walk->facts && (in->op == IR_OP_CALL
                        || in->op == IR_OP_CALLBACK_INVOKE)
        && ir_summary_facts_call_diverges(walk->facts, in))
        return 0;
    if (in != walk->origin && walk->result && in->dest_kind == IR_DEST_LOCAL
        && in->dest_local == walk->result)
        return 0;
    if (in != walk->origin && walk->storage && in->op == IR_OP_STORE
        && in->n_operands
        && ir_operand_value(&in->operands[0]) == walk->storage)
        return 0;
    return IR_PATH_UNPASSED;
}

static bool
gap_reaches(ir_func_t *function, missing_return_t *walk, ir_instr_t *gap,
            ir_instr_t *exit)
{
    bool *seen = core_alloc_array(bool, function->n_blocks);
    core_list_t(n00bb_ir_block_ptr_t) work =
        core_list_new(n00bb_ir_block_ptr_t);
    bool reached = false;
    for (uint32_t i = 0; i < ir_succ_count(gap); i++)
        core_list_push(work, ir_succ_at(gap, i));
    while (!reached && work.len) {
        ir_block_t *block = work.data[--work.len];
        if (!block || seen[block->index]) continue;
        seen[block->index] = true;
        bool killed = false;
        for (uint32_t i = 0; !killed && !reached && i < block->n_instrs; i++) {
            reached = block->instrs[i] == exit;
            killed = !reached && !missing_return_step(walk, block->instrs[i],
                                                      IR_PATH_ANY);
        }
        ir_instr_t *last = ir_block_terminator(block);
        for (uint32_t i = 0; !killed && !reached && last
             && i < ir_succ_count(last); i++)
            core_list_push(work, ir_succ_at(last, i));
    }
    core_list_free(work);
    core_free(seen);
    return reached;
}

static ir_instr_t *
switch_gap(ir_func_t *function, missing_return_t *walk, ir_instr_t *exit)
{
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_instr_t *last = ir_block_terminator(function->blocks[b]);
        if (last && ir_annot_get(&last->annots, N00BB_SWITCH_MISSING_KEY)
            && gap_reaches(function, walk, last, exit))
            return last;
    }
    return nullptr;
}

static void
report_missing_return(n00bb_link_analysis_t *analysis,
                      core_translation_unit_t *unit, ir_func_t *function,
                      ir_instr_t *exit, ir_instr_t *gap)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_diag_location_t name = n00bb_name_location(unit, function->span,
                                              function->name);
    th_type *returns = function->type
        ? th_func_ret(th_resolve(function->type)) : nullptr;
    core_string_t return_text = returns
        ? n00bb_print_type(ir_ctx_types(analysis->ir_context), returns)
        : (core_string_t){0};
    ir_annot_t *missing = gap
        ? ir_annot_get(&gap->annots, N00BB_SWITCH_MISSING_KEY) : nullptr;
    if (missing) {
        n00bb_report(diagnostics, unit, N00B_DIAG_SWITCH_NOT_EXHAUSTIVE,
            n00bb_span_location(gap->span), N00B_TEXT_THIS_SWITCH_HAS_NO_CASE,
            N00BB_ARGS(ARG_TEXT(missing->s), ARG_TEXT(function->name)));
        n00bb_report_secondary(diagnostics, nullptr, name, N00B_TEXT_RETURNS_2,
            N00BB_ARGS(ARG_TEXT(function->name), ARG_TEXT(return_text)));
        n00bb_report_help(diagnostics, N00B_TEXT_ADD_THE_MISSING_CASES_AN,
            N00BB_NO_ARGS);
        return;
    }
    ir_span_t end = function->span;
    if (!exit->span.implied) end = exit->span;
    else {
        end.start_line = end.end_line;
        end.start_col = end.end_col > 1 ? end.end_col - 1 : end.end_col;
        end.start_byte = end.end_byte > 0 ? end.end_byte - 1 : end.end_byte;
    }
    n00bb_report(diagnostics, unit, N00B_DIAG_MISSING_RETURN,
        name.has_location ? name : n00bb_span_location(end),
        N00B_TEXT_CAN_REACH_THE_END_OF, N00BB_ARGS(ARG_TEXT(function->name)));
    if (return_text.u8_bytes)
        n00bb_report_label(diagnostics, N00B_TEXT_RETURNS,
            N00BB_ARGS(ARG_TEXT(return_text)));
    n00bb_report_secondary(diagnostics, nullptr, n00bb_span_location(end),
        N00B_TEXT_A_PATH_THROUGH_THE_BODY, N00BB_NO_ARGS);
    n00bb_report_help(diagnostics, N00B_TEXT_ADD_A_RETURN_ON_EVERY,
        N00BB_NO_ARGS);
}

bool
validate_missing_returns(n00bb_link_analysis_t *analysis)
{
    bool ok = true;
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        for (size_t i = 0; i < state->result_defaults.len; i++) {
            ir_instr_t *write = n00bb_declared_result_default(
                state->result_defaults.data[i]);
            ir_func_t *function = write ? write->block->owner : nullptr;
            if (!function) continue;
            missing_return_t walk = {
                .facts = ir_summary_of(analysis->summaries, function)
                    ? group_facts(analysis, function, false) : nullptr,
                .origin = write,
                .result = write->dest_kind == IR_DEST_LOCAL
                    ? write->dest_local : nullptr,
                .storage = write->op == IR_OP_STORE && write->n_operands
                    ? ir_operand_value(&write->operands[0]) : nullptr,
            };
            ir_instr_t *exit = nullptr;
            for (uint32_t b = 0; !exit && b < function->n_blocks; b++) {
                ir_instr_t *last = ir_block_terminator(function->blocks[b]);
                if (last && last->op == IR_OP_RETURN
                    && ir_paths_state(function, IR_PATH_UNPASSED,
                                      missing_return_step, &walk, last))
                    exit = last;
            }
            if (!exit) continue;
            report_missing_return(analysis, unit, function, exit,
                                  switch_gap(function, &walk, exit));
            ok = false;
        }
    }
    return ok;
}
