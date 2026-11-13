#include "lower_internal.h"

static bool
collect_elif_arms(parse_grammar_t *grammar, parse_annot_result_t *annotations,
                  parse_tree_t *node, core_string_t clause_kind,
                  core_list_t(n00bb_branch_arm_t) *arms,
                  parse_tree_t **else_body, bool *has_else_clause);
static parse_tree_t *
condition_core(parse_grammar_t *grammar, parse_tree_t *node);
static bool
n00bb_jump_targets(n00bb_lower_context_t *lower, parse_grammar_t *grammar,
                   parse_tree_t *node, ir_block_t **on_true,
                   ir_block_t **on_false);
static core_string_t
n00bb_failure_text(n00bb_lower_context_t *lower, core_string_t text);
static bool
n00bb_reportable_operand(ir_module_t *module, ir_value_t *value);
static bool
n00bb_comparison_node(parse_grammar_t *grammar, parse_tree_t *node);
static size_t
n00bb_failed_comparisons(parse_grammar_t *grammar, parse_tree_t *expression,
                         bool always, parse_tree_t **out, size_t count);
static bool
n00bb_open_condition_scratch(ir_lower_context_t *context,
                             n00bb_lower_context_t *lower,
                             parse_tree_t *node, ir_span_t span);
static bool
n00bb_lower_range_body_enter(ir_lower_context_t *context,
                             n00bb_lower_context_t *lower,
                             n00bb_loop_frame_t *frame);
static ir_value_t *
n00bb_project_node(ir_lower_context_t *context, ir_value_t *value,
                   parse_tree_t *node, ir_span_t span);
static bool
n00bb_lower_container_body_enter(ir_lower_context_t *context,
                                 n00bb_lower_context_t *lower,
                                 n00bb_loop_frame_t *frame);
static bool
n00bb_store_try_value(ir_lower_context_t *context,
                      n00bb_try_frame_t *frame, parse_grammar_t *grammar,
                      parse_tree_t *body);
static bool
n00bb_store_try_error(ir_lower_context_t *context,
                      n00bb_lower_context_t *lower, n00bb_try_frame_t *frame,
                      ir_value_t *error, ir_span_t span);
static bool
n00bb_emit_case_tests(ir_lower_context_t *context,
                      n00bb_lower_context_t *lower,
                      n00bb_switch_frame_t *frame, size_t case_index);
static th_type *n00bb_case_test_type(th_ctx *types, parse_sym_entry_t *function,
                                     th_type *type);
static bool
n00bb_typecase_collect(parse_grammar_t *grammar, parse_tree_t *node,
                       n00bb_typecase_frame_t *frame,
                       ir_lower_context_t *context);
static bool
n00bb_typecase_emit_dispatch(ir_lower_context_t *context,
                             n00bb_typecase_frame_t *frame);
static bool
n00bb_lower_typecase_annotation(ir_lower_context_t *context,
                                parse_visit_t *visit, bool entering);

ir_instr_t *
n00bb_branch_on_outcome(ir_lower_context_t *context, ir_value_t *outcome,
                        ir_block_t *ok_block, ir_block_t *error_block,
                        ir_span_t span)
{
    th_type *uint_type = n00bb_ir_builtin_type(context->module, CORE_STRING_STATIC("uint"));
    th_type *bool_type = n00bb_ir_builtin_type(context->module, CORE_STRING_STATIC("bool"));
    ir_instr_t *tag = ir_lower_emit_instruction(context, IR_OP_VARIANT_TAG,
                                                span);
    ir_instr_t *literal = ir_lower_emit_instruction(context, IR_OP_CONST, span);
    ir_instr_t *test = ir_lower_emit_instruction(context, IR_OP_EQ, span);
    bool emitted = uint_type && bool_type
        && tag && ir_lower_set_temp_dest(context, tag, CORE_STRING_STATIC("__n00bb_tag_"),
                                          uint_type)
        && ir_instr_operand_value(tag, outcome)
        && literal && ir_lower_set_temp_dest(context, literal,
                                              CORE_STRING_STATIC("__n00bb_tag_lit_"), uint_type)
        && ir_instr_operand_uint(literal, th_arm_tag(CORE_STRING_STATIC("ok")))
        && test && ir_lower_set_temp_dest(context, test, CORE_STRING_STATIC("__n00bb_is_ok_"),
                                          bool_type)
        && ir_instr_operand_value(test, tag->dest)
        && ir_instr_operand_value(test, literal->dest);
    ir_instr_t *branch = emitted
        ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
    return branch && ir_instr_operand_value(branch, test->dest)
        && ir_instr_operand_block(branch, ok_block)
        && ir_instr_operand_block(branch, error_block) ? branch : nullptr;
}

static bool
collect_elif_arms(parse_grammar_t *grammar, parse_annot_result_t *annotations,
                  parse_tree_t *node, core_string_t clause_kind,
                  core_list_t(n00bb_branch_arm_t) *arms,
                  parse_tree_t **else_body, bool *has_else_clause)
{
    if (!node || core_tree_is_leaf(node)) return true;
    bool value_form = core_string_eq(clause_kind,
                                     CORE_STRING_STATIC("value-elif-clause"));
    core_string_t else_kind = value_form
        ? CORE_STRING_STATIC("value-else-clause")
        : CORE_STRING_STATIC("else-clause");
    if (nt_is(grammar, node, else_kind)) {
        *has_else_clause = true;
        *else_body = child_nt(grammar, node,
            value_form ? CORE_STRING_STATIC("value-body")
                       : CORE_STRING_STATIC("body"));
        return true;
    }
    if (nt_is(grammar, node, clause_kind)) {
        parse_cf_label_t *label = parse_cf_label_lookup(annotations, node);
        if (!label || label->kind != PARSE_CF_BRANCH
            || !label->cond || !label->then_body)
            return false;
        core_list_push(*arms, ((n00bb_branch_arm_t){
            .condition = label->cond,
            .body = label->then_body,
        }));
        return true;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!collect_elif_arms(grammar, annotations,
                               core_tree_child(node, i), clause_kind, arms,
                               else_body, has_else_clause))
            return false;
    return true;
}

static parse_tree_t *
condition_core(parse_grammar_t *grammar, parse_tree_t *node)
{
    while (node && !core_tree_is_leaf(node)) {
        if (core_tree_num_children(node) == 1
            && !core_tree_is_leaf(core_tree_child(node, 0)))
            node = core_tree_child(node, 0);
        else if (nt_is(grammar, node, CORE_STRING_STATIC("tuple-or-paren"))
                 && core_tree_num_children(node) == 3)
            node = core_tree_child(node, 1);
        else break;
    }
    return node;
}

bool
jumping_condition(parse_grammar_t *grammar, parse_tree_t *node)
{
    parse_tree_t *core = condition_core(grammar, node);
    return core && core_tree_num_children(core) == 3
        && (nt_is(grammar, core, CORE_STRING_STATIC("or-expr"))
            || nt_is(grammar, core, CORE_STRING_STATIC("and-expr")));
}

static bool
n00bb_jump_targets(n00bb_lower_context_t *lower, parse_grammar_t *grammar,
                   parse_tree_t *node, ir_block_t **on_true,
                   ir_block_t **on_false)
{
    for (size_t i = lower->branches.len; i > 0; i--) {
        n00bb_branch_frame_t *frame = &lower->branches.data[i - 1];
        if (!frame->active) continue;
        if (frame->jumping) {
            n00bb_branch_arm_t *arm = &frame->arms.data[0];
            bool is_or = nt_is(grammar, frame->node,
                               CORE_STRING_STATIC("or-expr"));
            if (condition_core(grammar, arm->condition) == node) {
                *on_true = is_or ? frame->jump_true : arm->body_block;
                *on_false = is_or ? arm->body_block : frame->jump_false;
                return true;
            }
            if (condition_core(grammar, arm->body) == node) {
                *on_true = frame->jump_true;
                *on_false = frame->jump_false;
                return true;
            }
            continue;
        }
        if (frame->short_circuit) {
            n00bb_branch_arm_t *arm = &frame->arms.data[0];
            if (!frame->short_block
                || condition_core(grammar, arm->condition) != node) continue;
            bool is_or = nt_is(grammar, frame->node,
                               CORE_STRING_STATIC("or-expr"));
            *on_true = is_or ? frame->short_block : arm->body_block;
            *on_false = is_or ? arm->body_block : frame->short_block;
            return true;
        }
        for (size_t a = 0; a < frame->arms.len; a++) {
            if (condition_core(grammar, frame->arms.data[a].condition) != node)
                continue;
            *on_true = frame->arms.data[a].body_block;
            *on_false = a + 1 < frame->arms.len
                ? frame->arms.data[a + 1].test_block
                : frame->else_block ? frame->else_block : frame->join;
            return true;
        }
    }
    for (size_t i = lower->loops.len; i > 0; i--) {
        n00bb_loop_frame_t *frame = &lower->loops.data[i - 1];
        if (!frame->active || frame->is_range || frame->is_container
            || condition_core(grammar, frame->condition) != node) continue;
        *on_true = frame->body_block;
        *on_false = frame->exit;
        return true;
    }
    for (size_t i = lower->requires.len; i > 0; i--) {
        n00bb_require_frame_t *frame = &lower->requires.data[i - 1];
        if (condition_core(grammar, frame->condition) != node) continue;
        *on_true = frame->join;
        *on_false = frame->failure;
        return true;
    }
    return false;
}

static core_string_t
n00bb_failure_text(n00bb_lower_context_t *lower, core_string_t text)
{
    core_buffer_t flat = {0};
    bool space = false;
    for (size_t i = 0; i < text.u8_bytes; i++) {
        char c = text.data[i];
        bool blank = c == ' ' || c == '\t' || c == '\n' || c == '\r';
        if (blank) {
            space = flat.byte_len != 0;
            continue;
        }
        if (space) core_buffer_putc(&flat, ' ');
        space = false;
        core_buffer_putc(&flat, c == N00BB_FAILURE_OPERAND_SEPARATOR ? ' '
                                                                     : c);
    }
    core_string_t result = ir_strdup(lower->ir_context,
                                     core_buffer_string(&flat));
    core_buffer_release(&flat);
    return result;
}

bool
n00bb_describe_failure(n00bb_lower_context_t *lower, ir_instr_t *trap,
                       n00bb_failure_kind_t kind, core_string_t text,
                       core_string_t detail)
{
    bool ok = ir_annot_set(lower->ir_context, &trap->annots, (ir_annot_t){
        .key = CORE_STRING_STATIC(N00BB_FAILURE_KIND_ANNOTATION),
        .kind = IR_ANNOT_INT, .i = ir_lit_of(kind),
    });
    if (ok && text.data)
        ok = ir_annot_set(lower->ir_context, &trap->annots, (ir_annot_t){
            .key = CORE_STRING_STATIC(N00BB_FAILURE_TEXT_ANNOTATION),
            .kind = IR_ANNOT_STR, .s = n00bb_failure_text(lower, text),
        });
    if (ok && detail.data)
        ok = ir_annot_set(lower->ir_context, &trap->annots, (ir_annot_t){
            .key = CORE_STRING_STATIC(N00BB_FAILURE_DETAIL_ANNOTATION),
            .kind = IR_ANNOT_STR, .s = ir_strdup(lower->ir_context, detail),
        });
    return ok;
}

static bool
n00bb_reportable_operand(ir_module_t *module, ir_value_t *value)
{
    if (!value || !value->type) return false;
    if (value->def && value->def->op == IR_OP_CONST) return false;
    ir_machine_t machine = ir_machine_of(module, value->type);
    return machine.class == IR_CLASS_SINT || machine.class == IR_CLASS_UINT
        || machine.class == IR_CLASS_BOOL || machine.class == IR_CLASS_FLOAT
        || th_builtin_kind_of(th_resolve(value->type)) == TH_BUILTIN_STRING;
}

static bool
n00bb_comparison_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    return node && core_tree_num_children(node) == 3
        && (nt_is(grammar, node, CORE_STRING_STATIC("equality-expr"))
            || nt_is(grammar, node, CORE_STRING_STATIC("compare-expr")));
}

static size_t
n00bb_failed_comparisons(parse_grammar_t *grammar, parse_tree_t *expression,
                         bool always, parse_tree_t **out, size_t count)
{
    parse_tree_t *core = condition_core(grammar, expression);
    if (!core || count >= N00BB_FAILURE_COMPARISONS) return count;
    if (n00bb_comparison_node(grammar, core)) {
        out[count++] = core;
        return count;
    }
    if (core_tree_num_children(core) != 3) return count;
    bool is_or = nt_is(grammar, core, CORE_STRING_STATIC("or-expr"));
    bool is_and = nt_is(grammar, core, CORE_STRING_STATIC("and-expr"));
    if (!is_or && !is_and) return count;
    count = n00bb_failed_comparisons(grammar, core_tree_child(core, 0),
                                     always || is_and, out, count);
    if (is_or && !always)
        count = n00bb_failed_comparisons(grammar, core_tree_child(core, 2),
                                         false, out, count);
    return count;
}

ir_value_t *
n00bb_condition_operands(ir_lower_context_t *context,
                         n00bb_lower_context_t *lower,
                         parse_grammar_t *grammar, parse_tree_t *expression,
                         ir_block_t *failure, ir_span_t span,
                         core_string_t *texts)
{
    *texts = (core_string_t){0};
    parse_tree_t *comparisons[N00BB_FAILURE_COMPARISONS] = {0};
    size_t comparison_count = n00bb_failed_comparisons(grammar, expression,
        false, comparisons, 0);
    ir_value_t *values[2 * N00BB_FAILURE_COMPARISONS] = {0};
    th_type *types[2 * N00BB_FAILURE_COMPARISONS] = {0};
    core_string_t names[2 * N00BB_FAILURE_COMPARISONS] = {0};
    core_buffer_t joined = {0};
    size_t count = 0;
    for (size_t c = 0; c < comparison_count; c++)
        for (size_t side = 0; side < 3; side += 2) {
            parse_tree_t *child = core_tree_child(comparisons[c], side);
            ir_value_t *value = ir_lower_node_value(context, child);
            if (!value)
                value = ir_lower_node_value(context,
                                            condition_core(grammar, child));
            if (!n00bb_reportable_operand(context->module, value)) continue;
            core_string_t text = n00bb_failure_text(lower,
                n00bb_node_source_text(lower->unit, child,
                                       (core_string_t){0}));
            bool repeated = false;
            for (size_t i = 0; i < count && !repeated; i++)
                repeated = core_string_eq(names[i], text);
            if (repeated) continue;
            if (count) core_buffer_putc(&joined,
                                        N00BB_FAILURE_OPERAND_SEPARATOR);
            core_buffer_append_string(&joined, text);
            names[count] = text;
            values[count] = value;
            types[count++] = value->type;
        }
    th_type *tuple = count
        ? th_tuple(ir_module_types(context->module), types, count) : nullptr;
    ir_instr_t *aggregate = tuple ? ir_instr_add(failure, IR_OP_AGGREGATE)
                                  : nullptr;
    bool ok = aggregate && ir_lower_set_temp_dest(context, aggregate,
        CORE_STRING_STATIC("__n00bb_failure_"), tuple);
    for (size_t i = 0; ok && i < count; i++)
        ok = ir_instr_operand_value(aggregate, values[i]);
    if (ok) {
        aggregate->span = span;
        *texts = ir_strdup(lower->ir_context, core_buffer_string(&joined));
    }
    core_buffer_release(&joined);
    return ok ? aggregate->dest : nullptr;
}

bool
n00bb_close_condition_scratch(n00bb_lower_context_t *lower)
{
    if (!lower->condition_scratch) return true;
    ir_instr_t *unreachable = ir_instr_add(lower->condition_scratch,
                                           IR_OP_UNREACHABLE);
    lower->condition_scratch = nullptr;
    return unreachable != nullptr;
}

static bool
n00bb_open_condition_scratch(ir_lower_context_t *context,
                             n00bb_lower_context_t *lower,
                             parse_tree_t *node, ir_span_t span)
{
    ir_block_t *scratch = n00bb_branch_block(context, lower,
                                             "condition_value");
    th_type *bool_type = n00bb_ir_builtin_type(context->module,
                                               CORE_STRING_STATIC("bool"));
    if (!scratch || !bool_type) return false;
    context->block = scratch;
    context->reachable = true;
    ir_instr_t *literal = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                    span);
    if (!literal || !ir_lower_set_temp_dest(context, literal,
            CORE_STRING_STATIC("__n00bb_condition_"), bool_type)
        || !ir_instr_operand_uint(literal, 1)
        || !ir_lower_set_node_value(context, node, literal->dest))
        return false;
    literal->span.implied = true;
    lower->condition_scratch = scratch;
    return true;
}

bool
n00bb_emit_branch(ir_lower_context_t *context, ir_value_t *condition,
                  ir_block_t *on_true, ir_block_t *on_false, ir_span_t span)
{
    ir_instr_t *branch = ir_instr_add(context->block, IR_OP_BRANCH);
    if (!branch || !ir_instr_operand_value(branch, condition)
        || !ir_instr_operand_block(branch, on_true)
        || !ir_instr_operand_block(branch, on_false)) {
        lower_internal(context, span,
                    "could not emit conditional branch");
        return false;
    }
    branch->span = span;
    return true;
}

bool
n00bb_lower_branch_annotation(ir_lower_context_t *context,
                              parse_visit_t *visit, bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (!lower) return false;
    if (!entering) {
        if (lower->branches.len
            && lower->branches.data[lower->branches.len - 1].node == visit->node) {
            n00bb_branch_frame_t frame =
                lower->branches.data[--lower->branches.len];
            if (frame.jumping) {
                core_list_free(frame.arms);
                return n00bb_open_condition_scratch(context, lower,
                    frame.node, lower_span_for_tree(context, frame.node));
            }
            if (frame.active && !frame.join_reachable) {
                ir_instr_t *unreachable =
                    ir_instr_add(frame.join, IR_OP_UNREACHABLE);
                if (!unreachable) return false;
                unreachable->span = lower_span_for_tree(context, frame.node);
            }
            if (frame.active) {
                if (frame.expression && frame.join_reachable) {
                    ir_instr_t *read = ir_instr_add(frame.join, IR_OP_COPY);
                    if (!read || !ir_lower_set_temp_dest(context, read,
                        CORE_STRING_STATIC("__n00bb_if_value_"), frame.result_local->type)
                        || !ir_instr_operand_local(read, frame.result_local)
                        || !ir_lower_set_node_value(context, frame.node,
                                                    read->dest)) {
                        core_list_free(frame.arms);
                        lower_internal(context,
                            lower_span_for_tree(context, frame.node),
                            "could not read the value from the if-expression join");
                        return false;
                    }
                    read->span = lower_span_for_tree(context, frame.node);
                }
                context->block = frame.join;
                context->reachable = frame.join_reachable;
            }
            core_list_free(frame.arms);
        }
        return true;
    }

    if (nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("elif-clause"))
        || nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("value-elif-clause"))) {
        for (parse_tree_t *parent = parse_tree_parent(visit->node);
             parent; parent = parse_tree_parent(parent)) {
            if (nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("elif-clause"))
                && nt_is(visit->grammar, parent, CORE_STRING_STATIC("if-stmt"))) return true;
            if (nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("value-elif-clause"))
                && nt_is(visit->grammar, parent, CORE_STRING_STATIC("if-value-expr"))) return true;
        }
    }
    bool short_circuit = nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("and-expr"))
        || nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("or-expr"));
    bool expression_form = short_circuit
        || nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("if-value-expr"));
    if (!expression_form
        && !nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("if-stmt"))) {
        lower_internal(context, lower_span_for_tree(context, visit->node),
                       "branch lowering reached a node that is not a branch");
        return false;
    }

    n00bb_tu_state_t *tu = tu_state(lower->unit);
    parse_cf_label_t *label = tu
        ? parse_cf_label_lookup(tu->annotations, visit->node) : nullptr;
    if (!label || label->kind != PARSE_CF_BRANCH || !label->cond
        || !label->then_body)
        return false;
    core_list_t(n00bb_branch_arm_t) arms =
        core_list_new(n00bb_branch_arm_t);
    parse_tree_t *else_body = nullptr;
    bool has_else_clause = false;
    core_list_push(arms, ((n00bb_branch_arm_t){
        .condition = label->cond,
        .body = label->then_body,
    }));
    if (!collect_elif_arms(visit->grammar, tu->annotations,
                           label->else_body,
                           expression_form
                               ? CORE_STRING_STATIC("value-elif-clause")
                               : CORE_STRING_STATIC("elif-clause"),
                           &arms, &else_body, &has_else_clause)) {
        core_list_free(arms);
        return false;
    }
    if (has_else_clause && !else_body) {
        core_list_free(arms);
        lower_internal(context, lower_span_for_tree(context, visit->node),
                       "an else clause has no body");
        return false;
    }
    if (expression_form && !short_circuit && !else_body) {
        core_list_free(arms);
        lower_report(context, N00B_DIAG_IF_VALUE_WITHOUT_ELSE,
            lower_span_for_tree(context, visit->node),
            N00B_TEXT_AN_IF_USED_AS_A,
            N00BB_NO_ARGS);
        lower_help(context, N00B_TEXT_ADD_ELSE_WITH_THE_VALUE, N00BB_NO_ARGS);
        return false;
    }
    if (!context->reachable) {
        core_list_push(lower->branches, ((n00bb_branch_frame_t){
            .node = visit->node,
            .arms = arms,
            .else_body = else_body,
            .expression = expression_form,
            .short_circuit = short_circuit,
            .active = false,
        }));
        return true;
    }
    ir_block_t *jump_true = nullptr, *jump_false = nullptr;
    bool jumping = short_circuit
        && n00bb_jump_targets(lower, visit->grammar, visit->node,
                              &jump_true, &jump_false);
    if (jumping) {
        ir_block_t *rhs = n00bb_branch_block_named(context, lower,
            nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("or-expr"))
                ? CORE_STRING_STATIC("condition_or")
                : CORE_STRING_STATIC("condition_and"));
        if (!rhs) {
            core_list_free(arms);
            return false;
        }
        arms.data[0].body_block = rhs;
        core_list_push(lower->branches, ((n00bb_branch_frame_t){
            .node = visit->node,
            .arms = arms,
            .jump_true = jump_true,
            .jump_false = jump_false,
            .short_circuit = true,
            .jumping = true,
            .active = true,
        }));
        return true;
    }
    ir_block_t *join = n00bb_branch_block(context, lower, "if_join");
    ir_block_t *else_block = else_body
        ? n00bb_branch_block(context, lower, "if_else") : nullptr;
    th_type *result_type = !expression_form ? nullptr
        : short_circuit ? n00bb_ir_builtin_type(context->module,
                                                CORE_STRING_STATIC("bool"))
        : n00bb_node_result_type(context, visit->node);
    ir_local_t *result_local = nullptr;
    if (expression_form && result_type) {
        char name_buffer[48];
        core_string_t name = n00bb_format_name(name_buffer,
            sizeof(name_buffer), "__n00bb_if_result_%llu",
            (unsigned long long)context->next_value_id++);
        result_local = ir_local_add(context->function, name, result_type);
    }
    bool blocks_ready = join && (!else_body || else_block)
        && (!expression_form || result_local);
    for (size_t i = 0; blocks_ready && i < arms.len; i++) {
        arms.data[i].body_block = n00bb_branch_block(context, lower,
                                                     "if_then");
        if (i) arms.data[i].test_block = n00bb_branch_block(
            context, lower, "if_test");
        blocks_ready = arms.data[i].body_block
            && (!i || arms.data[i].test_block);
    }
    if (!blocks_ready) {
        core_list_free(arms);
        return false;
    }
    ir_block_t *short_block = nullptr;
    if (short_circuit && result_local
        && jumping_condition(visit->grammar, arms.data[0].condition)) {
        short_block = n00bb_branch_block(context, lower, "short_circuit");
        ir_block_t *resume = context->block;
        ir_span_t span = lower_span_for_tree(context, visit->node);
        th_type *bool_type = n00bb_ir_builtin_type(context->module,
                                                   CORE_STRING_STATIC("bool"));
        bool is_or = nt_is(visit->grammar, visit->node,
                           CORE_STRING_STATIC("or-expr"));
        context->block = short_block;
        ir_instr_t *literal = short_block && bool_type
            ? ir_lower_emit_instruction(context, IR_OP_CONST, span) : nullptr;
        bool emitted = literal
            && ir_lower_set_temp_dest(context, literal,
                CORE_STRING_STATIC("__n00bb_short_"), bool_type)
            && ir_instr_operand_uint(literal, is_or ? 1 : 0);
        ir_instr_t *store = emitted
            ? ir_instr_add(short_block, IR_OP_COPY) : nullptr;
        ir_instr_t *jump = store && ir_instr_set_dest_local(store, result_local)
                && ir_instr_operand_value(store, literal->dest)
            ? ir_instr_add(short_block, IR_OP_JUMP) : nullptr;
        context->block = resume;
        if (!jump || !ir_instr_operand_block(jump, join)) {
            core_list_free(arms);
            lower_internal(context, span,
                        "could not emit the short-circuit result");
            return false;
        }
        store->span = span;
        jump->span = span;
    }
    core_list_push(lower->branches, ((n00bb_branch_frame_t){
        .node = visit->node,
        .arms = arms,
        .else_body = else_body,
        .join = join,
        .else_block = else_block,
        .result_local = result_local,
        .short_block = short_block,
        .expression = expression_form,
        .short_circuit = short_circuit,
        .join_reachable = !else_body,
        .active = true,
    }));
    return true;
}

static bool
n00bb_lower_range_body_enter(ir_lower_context_t *context,
                             n00bb_lower_context_t *lower,
                             n00bb_loop_frame_t *frame)
{
    ir_span_t span = lower_span_for_tree(context, frame->node);
    ir_value_t *start = ir_lower_node_value(context, frame->range_start);
    ir_value_t *end = ir_lower_node_value(context, frame->range_end);
    ir_value_t *step = frame->range_step
        ? ir_lower_node_value(context, frame->range_step) : nullptr;
    th_type *iterator_type = frame->iterator ? frame->iterator->type : nullptr;
    th_type *bool_type = th_name(tu_state(lower->unit)->types, CORE_STRING_STATIC("bool"));
    if (!frame->range_step && iterator_type) {
        ir_instr_t *one = ir_lower_emit_instruction(context, IR_OP_CONST, span);
        if (one && ir_lower_set_temp_dest(context, one, CORE_STRING_STATIC("__n00bb_"),
                                           iterator_type)
            && ir_instr_operand_int_lit(one, ir_lit_of(1))) {
            step = one->dest;
        }
    }
    if (iterator_type && is_integer_type(iterator_type)) {
        if (start && is_integer_type(start->type))
            start = n00bb_widen_integer_operand(context, start,
                                                iterator_type, span);
        if (end && is_integer_type(end->type))
            end = n00bb_widen_integer_operand(context, end, iterator_type,
                                              span);
        if (step && is_integer_type(step->type))
            step = n00bb_widen_integer_operand(context, step, iterator_type,
                                               span);
    }
    th_ctx *types = ir_module_types(context->module);
    bool open = start && end && step && iterator_type
        && (!th_is_closed(types, iterator_type)
            || !th_is_closed(types, start->type)
            || !th_is_closed(types, end->type)
            || !th_is_closed(types, step->type));
    if (!start || !end || !step || !iterator_type || !bool_type
        || (!open
            && (!is_integer_type(iterator_type)
                || !ir_type_eq(context->module, start->type, iterator_type)
                || !ir_type_eq(context->module, end->type, iterator_type)
                || !ir_type_eq(context->module, step->type,
                               iterator_type)))) {
        lower_internal(context, span,
                       "range loop endpoints and step do not share one "
                       "integer type");
        return false;
    }
    int64_t step_value = 0;
    frame->range_step_value = step;
    ir_instr_t *initialize = ir_lower_emit_instruction(context, IR_OP_COPY, span);
    if (!initialize || !ir_instr_set_dest_local(initialize, frame->iterator)
        || !ir_instr_operand_value(initialize, start)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not initialize range loop variable");
        return false;
    }
    ir_machine_t step_machine = ir_machine_of(context->module, step->type);
    bool step_known = !frame->range_step;
    if (!frame->range_step) step_value = 1;
    ir_instr_t *zero = step_known ? nullptr
        : ir_lower_emit_instruction(context, IR_OP_CONST, span);
    bool zero_ok = zero && ir_lower_set_temp_dest(context, zero, CORE_STRING_STATIC("__n00bb_"),
                                                 iterator_type)
        && ir_instr_operand_int_lit(zero, ir_lit_of(0));
    ir_value_t *is_zero = zero_ok
        ? n00bb_emit_binary_ir(context, IR_OP_EQ, bool_type, step, zero->dest,
                               span) : nullptr;
    ir_block_t *zero_step = is_zero
        ? n00bb_branch_block(context, lower, "for_zero_step") : nullptr;
    ir_instr_t *step_branch = ir_instr_add(context->block,
        step_known ? IR_OP_JUMP : IR_OP_BRANCH);
    ir_instr_t *trap = zero_step
        ? ir_instr_add(zero_step, IR_OP_TRAP) : nullptr;
    bool entered = step_known
        ? step_branch && ir_instr_operand_block(step_branch, frame->header)
        : step_branch && trap && ir_instr_operand_value(step_branch, is_zero)
            && ir_instr_operand_block(step_branch, zero_step)
            && ir_instr_operand_block(step_branch, frame->header);
    if (!entered) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not guard the range step and enter its header");
        return false;
    }
    step_branch->span = span;
    if (trap) trap->span = span;
    if (trap && !n00bb_describe_failure(lower, trap, N00BB_FAILURE_ZERO_STEP,
            (core_string_t){0}, (core_string_t){0}))
        return false;
    frame->range_step_value = step;
    context->block = frame->header;

    bool signed_step = step_machine.class == IR_CLASS_SINT;
    bool emit_ascending = !step_known || step_value > 0;
    bool emit_descending = signed_step && (!step_known || step_value < 0);
    ir_value_t *current = n00bb_emit_local_read(context, frame->iterator, span);
    ir_value_t *ascending = current && emit_ascending
        ? n00bb_emit_binary_ir(context,
            frame->range_inclusive ? IR_OP_LE : IR_OP_LT,
            bool_type, current, end, span) : nullptr;
    ir_value_t *descending = current && emit_descending
        ? n00bb_emit_binary_ir(context,
            frame->range_inclusive ? IR_OP_GE : IR_OP_GT,
            bool_type, current, end, span) : nullptr;
    ir_value_t *negative = signed_step && zero_ok
        ? n00bb_emit_binary_ir(context, IR_OP_LT, bool_type, step, zero->dest,
                               span) : nullptr;
    ir_value_t *condition = step_known
        ? (step_value > 0 ? ascending : descending) : ascending;
    if (signed_step && !step_known) {
        ir_instr_t *selected = ir_lower_emit_instruction(context, IR_OP_SELECT,
                                                         span);
        if (selected && ir_lower_set_temp_dest(context, selected, CORE_STRING_STATIC("__n00bb_"),
                                               bool_type)
            && ir_instr_operand_value(selected, negative)
            && ir_instr_operand_value(selected, descending)
            && ir_instr_operand_value(selected, ascending)) {
            condition = selected->dest;
        }
        else condition = nullptr;
    }
    ir_instr_t *branch = condition
        ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
    if (!branch || !ir_instr_operand_value(branch, condition)
        || !ir_instr_operand_block(branch, frame->body_block)
        || !ir_instr_operand_block(branch, frame->exit)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not emit range loop condition");
        return false;
    }
    branch->span = span;

    context->block = frame->body_block;
    frame->binding_checkpoint = ir_lower_bindings_checkpoint(context);
    frame->binding_scope_active = true;
    if (!frame->range_binding || !frame->iterator_symbol
        || !ir_lower_bind_scoped_value(context, frame->iterator_symbol,
                                       current, frame->range_binding)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not initialize the range binding");
        return false;
    }

    context->block = frame->latch;
    current = n00bb_emit_local_read(context, frame->iterator, span);
    if (!current) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not read range loop variable at its latch");
        return false;
    }
    if (frame->range_inclusive) {
        ir_value_t *at_end = n00bb_emit_binary_ir(
            context, IR_OP_EQ, bool_type, current, end, span);
        ir_instr_t *last = at_end
            ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
        if (!last || !ir_instr_operand_value(last, at_end)
            || !ir_instr_operand_block(last, frame->exit)
            || !ir_instr_operand_block(last, frame->increment)) {
            lower_error(context, IR_DIAG_INTERNAL, span,
                        "could not emit inclusive range loop latch");
            return false;
        }
        last->span = span;
        context->block = frame->increment;
    }
    ir_value_t *incremented = n00bb_emit_binary_ir(
        context, IR_OP_ADD, frame->iterator->type, current, step, span);
    ir_instr_t *store = incremented
        ? ir_lower_emit_instruction(context, IR_OP_COPY, span) : nullptr;
    ir_instr_t *backedge = store
        ? ir_instr_add(context->block, IR_OP_JUMP) : nullptr;
    if (!store || !ir_instr_set_dest_local(store, frame->iterator)
        || !ir_instr_operand_value(store, incremented) || !backedge
        || !ir_instr_operand_block(backedge, frame->header)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not emit range loop increment");
        return false;
    }
    store->span = span;
    backedge->span = span;
    context->block = frame->body_block;
    context->reachable = true;
    return true;
}

ir_value_t *
n00bb_extract_step(ir_lower_context_t *context, ir_value_t *value,
                   n00bb_projection_step_t step, th_type *type, ir_span_t span)
{
    ir_instr_t *extract = value && type ? ir_lower_emit_instruction(context,
        step.positional ? IR_OP_EXTRACT_INDEX : IR_OP_EXTRACT, span)
        : nullptr;
    n00bb_lower_context_t *lower = context->user;
    n00bb_rewrite_later(context, extract);
    bool ok = extract && ir_lower_set_temp_dest(context, extract,
            CORE_STRING_STATIC("__n00bb_"), type)
        && ir_instr_operand_value(extract, value)
        && (step.positional ? ir_instr_operand_uint(extract, step.index)
                            : ir_instr_operand_name(extract, step.name))
        && n00bb_set_reference_access(lower->ir_context, extract->dest, type)
        && (!step.arity || ir_annot_set(lower->ir_context, &extract->annots,
               (ir_annot_t){.key = N00BB_PATTERN_ARITY_KEY,
                            .kind = IR_ANNOT_INT,
                            .i = {.mag = step.arity}}));
    return ok ? extract->dest : nullptr;
}

static ir_value_t *
n00bb_project_node(ir_lower_context_t *context, ir_value_t *value,
                   parse_tree_t *node, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    core_list_t(n00bb_projection_step_t) path
        = core_list_new(n00bb_projection_step_t);
    n00bb_pattern_path(lower->grammar, node, &path);
    for (size_t i = 0; value && i < path.len; i++)
        value = n00bb_extract_step(context, value, path.data[i],
            th_var(ir_module_types(context->module)), span);
    core_list_free(path);
    return value;
}

static void
report_outer_loop_name(ir_lower_context_t *context, parse_tree_t *node,
                       parse_sym_entry_t *symbol)
{
    parse_sym_entry_t *outer = symbol
        ? parse_sym_binding(symbol, N00BB_LOOP_OUTER_NAME_KEY) : nullptr;
    if (!outer) return;
    parse_tree_t *name = core_tree_num_children(node)
        ? core_tree_child(node, 0) : node;
    ir_span_t span = lower_span_for_tree(context, name);
    if (outer->mutability != PARSE_SYM_MUTABLE) {
        (void)lower_report(context, N00B_DIAG_LOOP_BINDS_OUTER_LET, span,
            N00B_TEXT_LOOP_BINDS_OUTER_LET,
            N00BB_ARGS(ARG_TEXT(symbol->name)));
        lower_declared_secondary(context, outer, N00B_TEXT_IS_DECLARED_HERE,
                                 N00BB_ARGS(ARG_TEXT(outer->name)));
        lower_help(context, N00B_TEXT_USE_A_NEW_NAME_OR_VAR,
                   N00BB_ARGS(ARG_TEXT(symbol->name)));
        return;
    }
    (void)lower_report(context, N00B_DIAG_LOOP_ASSIGNED_TYPE, span,
        N00B_TEXT_LOOP_ASSIGNED_TYPE, N00BB_ARGS(ARG_TEXT(symbol->name)));
    lower_help(context, N00B_TEXT_REMOVE_THE_LOOP_TYPE, N00BB_NO_ARGS);
}

bool
n00bb_lower_pattern(ir_lower_context_t *context, parse_visit_t *visit,
                    bool entering, void *state)
{
    (void)state;
    parse_grammar_t *grammar = visit->grammar;
    bool declaration = nt_is(grammar, visit->node,
                             CORE_STRING_STATIC("variable-decl"));
    parse_tree_t *first = core_tree_num_children(visit->node)
        ? core_tree_child(visit->node, 0) : nullptr;
    parse_tree_t *pattern = declaration
        ? child_nt(grammar, visit->node, CORE_STRING_STATIC("binding-pattern"))
        : nt_is(grammar, first, CORE_STRING_STATIC("assign-pattern"))
        ? first : nullptr;
    if (entering || !pattern) return true;
    parse_tree_t *source = declaration
        ? child_nt(grammar, visit->node, CORE_STRING_STATIC("expression"))
        : core_tree_child(visit->node, 2);
    ir_value_t *value = source ? ir_lower_node_value(context, source)
                               : nullptr;
    ir_span_t span = lower_span_for_tree(context, visit->node);
    core_list_t(parse_tree_ptr_t) targets = core_list_new(parse_tree_ptr_t);
    collect_for_bindings(grammar, pattern, &targets);
    bool internal = !value;
    for (size_t i = 0; !internal && i < targets.len; i++) {
        parse_tree_t *target = targets.data[i];
        parse_node_resolution_t resolution = parse_tree_resolution(target);
        parse_sym_entry_t *symbol = parse_tree_sym(target);
        if (!symbol && resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL)
            symbol = resolution.target.symbol;
        if (!symbol) {
            internal = true;
            break;
        }
        ir_value_t *element = n00bb_project_node(context, value, target,
                                                 span);
        internal = !element || ir_lower_assign_symbol(context, symbol, element,
            declaration, span).status == IR_LOWER_FAILED;
    }
    core_list_free(targets);
    if (internal)
        return lower_failed_after(context, span,
                CORE_STRING_STATIC("could not unpack the pattern"))
            .status != IR_LOWER_FAILED;
    return true;
}

static bool
n00bb_lower_container_body_enter(ir_lower_context_t *context,
                                 n00bb_lower_context_t *lower,
                                 n00bb_loop_frame_t *frame)
{
    ir_span_t span = lower_span_for_tree(context, frame->node);
    ir_value_t *source = frame->iteration_source
        ? ir_lower_node_value(context, frame->iteration_source) : nullptr;
    if (!source || !frame->iteration_plan) {
        lower_internal(context, span,
                    "container loop has incomplete iteration semantics");
        return false;
    }

    ir_value_t *cursor = n00bb_emit_iteration_call(context,
        CORE_STRING_STATIC("__iterator"), source, TH_IN,
        frame->iteration_plan->cursor_type, nullptr, frame->node);
    if (!cursor) {
        lower_internal(context, span,
                    "could not initialize the iteration cursor");
        return false;
    }
    ir_block_t *entry_block = context->block;
    frame->cleanup_active = true;
    ir_instr_t *enter = ir_instr_add(context->block, IR_OP_JUMP);
    if (!enter || !ir_instr_operand_block(enter, frame->header)) {
        lower_internal(context, span,
                    "could not enter the iteration header");
        return false;
    }
    enter->span = span;

    context->block = frame->header;
    context->reachable = true;
    frame->iteration_cursor_phi = ir_lower_emit_instruction(context, IR_OP_PHI,
                                                            span);
    ir_value_t *current = frame->iteration_cursor_phi
        && ir_lower_set_temp_dest(context, frame->iteration_cursor_phi,
                                  CORE_STRING_STATIC("__n00bb_cursor_"),
                                  frame->iteration_plan->cursor_type)
        && ir_instr_operand_phi_arm(frame->iteration_cursor_phi, entry_block,
                                    cursor)
            ? frame->iteration_cursor_phi->dest : nullptr;
    if (!current) {
        lower_internal(context, span,
                    "could not create the iteration cursor phi");
        return false;
    }
    frame->iteration_cursor_value = current;
    th_type *option_type = th_option(ir_module_types(context->module),
                                     frame->iteration_plan->item_type);
    ir_value_t *option = current && option_type
        ? n00bb_emit_iteration_call(context,
            CORE_STRING_STATIC("__next"), current, TH_INOUT,
            option_type, &frame->iteration_cursor_value, frame->node)
        : nullptr;
    th_type *bool_type = th_name(ir_module_types(context->module), CORE_STRING_STATIC("bool"));
    ir_block_t *cleanup = n00bb_branch_block(context, lower,
                                              "for_cleanup");
    ir_instr_t *test = option && bool_type && cleanup
        ? ir_lower_emit_instruction(context, IR_OP_OPTION_IS_NIL, span)
        : nullptr;
    ir_instr_t *branch = test ? ir_instr_add(context->block, IR_OP_BRANCH)
                              : nullptr;
    bool emitted = test
        && ir_lower_set_temp_dest(context, test, CORE_STRING_STATIC("__n00bb_iteration_done_"),
                                  bool_type)
        && ir_instr_operand_value(test, option)
        && branch && ir_instr_operand_value(branch, test->dest)
        && ir_instr_operand_block(branch, cleanup)
        && ir_instr_operand_block(branch, frame->body_block);
    if (!emitted) {
        lower_internal(context, span,
                    "could not test the next iteration item");
        return false;
    }
    branch->span = span;

    context->block = cleanup;
    context->reachable = true;
    if (!n00bb_emit_iteration_cleanup(context, frame, frame->node))
        return false;
    ir_instr_t *done = ir_instr_add(context->block, IR_OP_JUMP);
    if (!done || !ir_instr_operand_block(done, frame->exit)) {
        lower_internal(context, span,
                    "could not leave an exhausted iteration");
        return false;
    }
    done->span = span;

    context->block = frame->body_block;
    context->reachable = true;
    ir_instr_t *get = ir_lower_emit_instruction(context, IR_OP_OPTION_GET,
                                                span);
    if (!get || !ir_lower_set_temp_dest(context, get,
                                        CORE_STRING_STATIC("__n00bb_iteration_item_"),
                                        frame->iteration_plan->item_type)
        || !ir_instr_operand_value(get, option)) {
        lower_internal(context, span,
                    "could not extract the next iteration item");
        return false;
    }
    frame->binding_checkpoint = ir_lower_bindings_checkpoint(context);
    frame->binding_scope_active = true;
    for (size_t i = 0; i < frame->iteration_bindings.len; i++) {
        parse_sym_entry_t *binding = frame->iteration_bindings.data[i];
        ir_local_t *local = ir_lower_bound_local(context, binding);
        ir_value_t *value = n00bb_project_node(context, get->dest,
                                               binding->decl_node, span);
        if (!value) {
            lower_internal(context, span,
                           "could not destructure the iteration item");
            return false;
        }
        if (parse_sym_binding_origin(binding)) {
            if (ir_lower_assign_symbol(context, binding, value, false,
                                       span).status == IR_LOWER_FAILED) {
                lower_internal(context, span,
                               "could not assign the next iteration item");
                return false;
            }
            continue;
        }
        if (!local || !ir_lower_bind_scoped_value(context, binding, value,
                                                   local)) {
            lower_internal(context, span,
                        "could not bind the next iteration item");
            return false;
        }
    }
    return true;
}

bool
n00bb_emit_jump(ir_lower_context_t *context, ir_block_t *target,
                ir_span_t span)
{
    ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
    if (!jump || !target || !ir_instr_operand_block(jump, target))
        return false;
    jump->span = span;
    return true;
}

static bool
n00bb_store_try_value(ir_lower_context_t *context,
                      n00bb_try_frame_t *frame, parse_grammar_t *grammar,
                      parse_tree_t *body)
{
    parse_tree_t *expression = value_body_expression(grammar, body);
    ir_value_t *value = expression
        ? ir_lower_node_value(context, expression)
        : nullptr;
    ir_instr_t *store = value
        ? ir_lower_emit_instruction(context, IR_OP_COPY,
                                    lower_span_for_tree(context, body))
        : nullptr;
    if (!store || !frame->result_value
        || !ir_instr_set_dest_local(store, frame->result_value)
        || !ir_instr_operand_value(store, value)) {
        lower_internal(context, lower_span_for_tree(context, body),
            "try expression body produced no value");
        return false;
    }
    return true;
}

int
n00bb_route_try_error(ir_lower_context_t *context, parse_tree_t *source,
                      ir_value_t *error, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_try_frame_t *frame = nullptr;
    for (size_t i = lower->tries.len; i > 0; i--) {
        n00bb_try_frame_t *candidate = &lower->tries.data[i - 1];
        if (candidate->active && candidate->body
            && node_is_within(source, candidate->body)) {
            frame = candidate;
            break;
        }
    }
    if (!frame) return 0;
    if (frame->error_symbol
        && !n00bb_store_try_error(context, lower, frame, error, span))
        return -1;
    if (!n00bb_emit_jump(context, frame->handler, span)) {
        lower_internal(context, span,
                    "could not route failure to the try handler");
        return -1;
    }
    frame->error_reachable = true;
    context->reachable = false;
    return 1;
}

static bool
n00bb_store_try_error(ir_lower_context_t *context,
                      n00bb_lower_context_t *lower, n00bb_try_frame_t *frame,
                      ir_value_t *error, ir_span_t span)
{
    parse_sym_entry_t *symbol = frame->error_symbol;
    th_ctx *types = tu_state(lower->unit)->types;
    if (!error || !types) {
        lower_internal(context, span,
                       "try error handler has no bound error value");
        return false;
    }
    th_type *bound_type = frame->error_value ? frame->error_value->type
        : symbol->type ? symbol->type : error->type;
    symbol->type = bound_type;
    if (!frame->error_value) {
        char name_buffer[48];
        core_string_t name = n00bb_format_name(name_buffer,
            sizeof(name_buffer), "__n00bb_try_error_%llu",
            (unsigned long long)context->next_value_id++);
        frame->error_value = ir_local_add(context->function, name,
                                          bound_type);
        if (!frame->error_value) {
            lower_internal(context, span,
                        "could not allocate the try handler error value");
            return false;
        }
    }
    ir_instr_t *store = ir_lower_emit_instruction(context, IR_OP_COPY, span);
    if (!store || !ir_instr_set_dest_local(store, frame->error_value)
        || !ir_instr_operand_value(store, error)
        || !ir_annot_set(lower->ir_context, &store->annots, (ir_annot_t){
               .key = CORE_STRING_STATIC(N00BB_TRY_ERROR_ANNOTATION),
               .kind = IR_ANNOT_BOOL, .b = true})) {
        lower_internal(context, span,
                    "could not store the failure for the try handler");
        return false;
    }
    return true;
}

bool
n00bb_lower_try_scope(ir_lower_context_t *context, parse_visit_t *visit,
                      bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (!lower) return true;
    core_string_t name = parse_tree_nt_name(visit->grammar, visit->node);

    bool try_statement = core_string_eq(name,
                                        CORE_STRING_STATIC("try-stmt"));
    bool try_expression = core_string_eq(name,
                                        CORE_STRING_STATIC("try-value-expr"));
    if (try_statement || try_expression) {
        if (entering) {
            n00bb_try_frame_t frame = {
                .node = visit->node,
                .body = child_nt(visit->grammar, visit->node,
                    try_expression ? CORE_STRING_STATIC("value-body")
                                   : CORE_STRING_STATIC("try-body")),
                .error_clause = child_nt(visit->grammar, visit->node,
                    try_expression
                        ? CORE_STRING_STATIC("try-value-error-clause")
                        : CORE_STRING_STATIC("try-error-clause")),
                .error_symbol = nullptr,
                .expression = try_expression,
                .active = context->reachable,
            };
            frame.error_symbol = n00bb_try_clause_symbol(visit->grammar,
                                                         frame.error_clause);
            if (frame.active) {
                frame.handler = n00bb_branch_block(context, lower,
                                                   "try_handler");
                frame.join = n00bb_branch_block(context, lower, "try_join");
                if (!frame.handler || !frame.join) {
                    lower_internal(context,
                        lower_span_for_tree(context, visit->node),
                        "could not create try control-flow blocks");
                    return false;
                }
                if (frame.expression) {
                    th_type *result_type =
                        n00bb_node_result_type(context, visit->node);
                    char local_buffer[48];
                    core_string_t local_name = n00bb_format_name(
                        local_buffer, sizeof(local_buffer),
                        "__n00bb_try_value_%llu",
                        (unsigned long long)context->next_value_id++);
                    frame.result_value = result_type
                        ? ir_local_add(context->function, local_name,
                                       result_type) : nullptr;
                    if (!frame.result_value) {
                        lower_internal(context,
                            lower_span_for_tree(context, visit->node),
                            "try expression has no inferred result type");
                        return false;
                    }
                }
            }
            core_list_push(lower->tries, frame);
            return true;
        }
        if (!lower->tries.len
            || lower->tries.data[lower->tries.len - 1].node != visit->node)
            return false;
        n00bb_try_frame_t frame = lower->tries.data[--lower->tries.len];
        if (!frame.active) return true;
        if (frame.expression && frame.join_reachable) {
            ir_instr_t *read = ir_instr_add(frame.join, IR_OP_COPY);
            char result_buffer[48];
            core_string_t result_name = n00bb_format_name(result_buffer,
                sizeof(result_buffer), "__n00bb_try_result_%llu",
                (unsigned long long)context->next_value_id++);
            if (!read || !ir_instr_set_dest(read, result_name,
                                            frame.result_value->type)
                || !ir_instr_operand_local(read, frame.result_value)
                || !ir_lower_set_node_value(context, frame.node, read->dest)) {
                lower_internal(context,
                    lower_span_for_tree(context, frame.node),
                    "could not read the value from the try-expression join");
                return false;
            }
            read->span = lower_span_for_tree(context, frame.node);
        }
        if (!frame.join_reachable && context->block == frame.join) {
            context->block = frame.join;
            ir_instr_t *unreachable = ir_instr_add(context->block,
                                                   IR_OP_UNREACHABLE);
            if (!unreachable)
                return false;
            unreachable->span = lower_span_for_tree(context, visit->node);
            context->reachable = false;
        }
        return true;
    }

    if (core_string_eq(visit->annot->scope_tag,
                       CORE_STRING_STATIC("block"))) {
        if (entering || !lower->tries.len) return true;
        n00bb_try_frame_t *frame = &lower->tries.data[lower->tries.len - 1];
        if (frame->body != visit->node || !frame->active) return true;
        if (context->reachable) {
            if (frame->expression
                && !n00bb_store_try_value(context, frame, visit->grammar,
                                          visit->node))
                return false;
            if (!n00bb_emit_jump(context, frame->join,
                                 lower_span_for_tree(context, visit->node)))
                return false;
            frame->join_reachable = true;
        }
        if (!frame->error_reachable) {
            lower_report(context, N00B_DIAG_TRY_CANNOT_FAIL,
                lower_span_for_first_token(context, frame->node),
                N00B_TEXT_NOTHING_IN_THIS_TRY_CAN,
                N00BB_NO_ARGS);
            lower_help(context, N00B_TEXT_REMOVE_THE_TRY_AND_ITS,
                N00BB_NO_ARGS);
            return false;
        }
        context->block = frame->handler;
        context->reachable = frame->error_reachable;
        if (!frame->error_reachable) {
            ir_instr_t *unreachable = ir_instr_add(context->block,
                                                   IR_OP_UNREACHABLE);
            if (!unreachable) return false;
            unreachable->span = lower_span_for_tree(context, visit->node);
        }
        return true;
    }

    if (core_string_eq(visit->annot->scope_tag,
                       CORE_STRING_STATIC("try-error"))
        && (core_string_eq(name, CORE_STRING_STATIC("try-error-clause"))
            || core_string_eq(name,
                              CORE_STRING_STATIC("try-value-error-clause")))) {
        if (!lower->tries.len) return true;
        n00bb_try_frame_t *frame = &lower->tries.data[lower->tries.len - 1];
        if (frame->error_clause != visit->node || !frame->active)
            return true;
        if (entering && context->reachable) {
            parse_sym_entry_t *symbol = frame->error_symbol;
            if (!symbol) return true;
            if (!frame->error_value
                || !ir_lower_bind_local(context, symbol,
                                        frame->error_value)) {
                lower_internal(context,
                    lower_span_for_tree(context, visit->node),
                    "could not bind the try handler error value");
                return false;
            }
            return true;
        }
        if (entering) return true;
        if (context->reachable) {
            if (frame->expression) {
                parse_tree_t *body = child_nt(visit->grammar, visit->node,
                                               CORE_STRING_STATIC("value-body"));
                if (!n00bb_store_try_value(context, frame, visit->grammar,
                                           body))
                    return false;
            }
            if (!n00bb_emit_jump(context, frame->join,
                                 lower_span_for_tree(context, visit->node)))
                return false;
            frame->join_reachable = true;
        }
        context->block = frame->join;
        context->reachable = frame->join_reachable;
        return true;
    }
    return true;
}

bool
n00bb_lower_branch_body(ir_lower_context_t *context, parse_visit_t *visit,
                        bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    bool logical_rhs = nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("and-rhs"))
        || nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("or-rhs"));
    if (!lower || (!logical_rhs
        && (!visit->annot || !core_string_eq(visit->annot->scope_tag,
                                             CORE_STRING_STATIC("block")))))
        return true;
    if (!entering
        && !n00bb_finish_final_scope(context, visit->node, visit->node))
        return false;
    if (nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("block-value-expr"))) {
        if (entering) {
            n00bb_value_block_frame_t frame = {
                .node = visit->node,
                .order = ++lower->next_control_order,
                .active = context->reachable,
            };
            if (frame.active) {
                th_type *result_type = n00bb_node_result_type(context,
                                                              visit->node);
                char local_buffer[48];
                core_string_t local_name = n00bb_format_name(local_buffer,
                    sizeof(local_buffer), "__n00bb_block_value_%llu",
                    (unsigned long long)context->next_value_id++);
                frame.result = result_type
                    ? ir_local_add(context->function, local_name, result_type)
                    : nullptr;
                frame.continuation = n00bb_branch_block(
                    context, lower, "block_value_cont");
                if (!frame.result || !frame.continuation) {
                    lower_internal(context,
                        lower_span_for_tree(context, visit->node),
                        "block value has no result type or continuation");
                    return false;
                }
            }
            core_list_push(lower->value_blocks, frame);
            return true;
        }
        if (!lower->value_blocks.len
            || lower->value_blocks.data[lower->value_blocks.len - 1].node
                != visit->node) return false;
        n00bb_value_block_frame_t frame = lower->value_blocks.data[
            --lower->value_blocks.len];
        if (!frame.active) return true;
        if (!frame.value_written || !context->reachable) {
            lower_report(context, N00B_DIAG_VALUE_BLOCK_WITHOUT_VALUE,
                lower_span_for_tree(context, visit->node),
                N00B_TEXT_THIS_BLOCK_IS_USED_AS,
                N00BB_NO_ARGS);
            lower_help(context, N00B_TEXT_END_EVERY_PATH_THROUGH_THE,
                N00BB_NO_ARGS);
            return false;
        }
        ir_instr_t *read = ir_lower_emit_instruction(context, IR_OP_COPY,
            lower_span_for_tree(context, visit->node));
        if (!read || !ir_lower_set_temp_dest(context, read,
                                             CORE_STRING_STATIC("__n00bb_block_result_"),
                                             frame.result->type)
            || !ir_instr_operand_local(read, frame.result)
            || !ir_lower_set_node_value(context, visit->node, read->dest)) {
            lower_internal(context,
                lower_span_for_tree(context, visit->node),
                "could not read the value block result");
            return false;
        }
        return true;
    }
    for (size_t i = lower->branches.len; i > 0; i--) {
        n00bb_branch_frame_t *frame = &lower->branches.data[i - 1];
        if (!frame->active) continue;
        bool else_body = frame->else_body == visit->node;
        size_t arm_index = frame->arms.len;
        for (size_t j = 0; j < frame->arms.len; j++)
            if (frame->arms.data[j].body == visit->node) {
                arm_index = j;
                break;
            }
        if (arm_index == frame->arms.len && !else_body) continue;
        if (entering) {
            if (else_body) {
                context->block = frame->else_block;
                context->reachable = true;
                return true;
            }
            n00bb_branch_arm_t *arm = &frame->arms.data[arm_index];
            ir_span_t span = lower_span_for_tree(context, frame->node);
            bool nested = (!frame->short_circuit || frame->jumping
                           || frame->short_block)
                && jumping_condition(visit->grammar, arm->condition);
            if (frame->jumping || nested) {
                if (nested && !n00bb_close_condition_scratch(lower))
                    return false;
                bool is_or = nt_is(visit->grammar, frame->node,
                                   CORE_STRING_STATIC("or-expr"));
                if (frame->jumping && !nested) {
                    ir_value_t *operand = condition_value(context,
                                                          arm->condition);
                    th_type *bool_type = th_name(
                        tu_state(lower->unit)->types,
                        CORE_STRING_STATIC("bool"));
                    if (!operand || !bool_type) {
                        lower_condition_error(context, arm->condition,
                            operand, logical_construct(visit->grammar,
                                                       frame->node));
                        return false;
                    }
                    if (!n00bb_emit_branch(context, operand,
                            is_or ? frame->jump_true : arm->body_block,
                            is_or ? arm->body_block : frame->jump_false,
                            span)
                        || !n00bb_mark_condition(context, logical_construct(
                               visit->grammar, frame->node), arm->condition))
                        return false;
                }
                context->block = arm->body_block;
                context->reachable = true;
                return true;
            }
            ir_value_t *condition = condition_value(context, arm->condition);
            th_type *bool_type = th_name(tu_state(lower->unit)->types, CORE_STRING_STATIC("bool"));
            if (!condition || !bool_type) {
                lower_condition_error(context, arm->condition, condition,
                    logical_construct(visit->grammar, frame->node));
                return false;
            }
            ir_block_t *false_block = arm_index + 1 < frame->arms.len
                ? frame->arms.data[arm_index + 1].test_block
                : frame->else_block ? frame->else_block : frame->join;
            n00bb_text_t construct = logical_construct(visit->grammar,
                                                       frame->node);
            if (frame->short_circuit) {
                ir_instr_t *store = ir_lower_emit_instruction(context,
                    IR_OP_COPY, lower_span_for_tree(context, frame->node));
                if (!store || !ir_instr_set_dest_local(store,
                        frame->result_local)
                    || !ir_instr_operand_value(store, condition)
                    || !n00bb_mark_condition(context, construct,
                                             arm->condition)) return false;
            }
            bool shortcut_true = frame->short_circuit
                && nt_is(visit->grammar, frame->node, CORE_STRING_STATIC("or-expr"));
            ir_instr_t *branch = ir_instr_add(context->block, IR_OP_BRANCH);
            bool emitted = branch
                && ir_instr_operand_value(branch, condition)
                && ir_instr_operand_block(branch,
                    shortcut_true ? false_block : arm->body_block)
                && ir_instr_operand_block(branch,
                    shortcut_true ? arm->body_block : false_block);
            if (!emitted) {
                lower_internal(context,
                    lower_span_for_tree(context, frame->node),
                    "could not emit conditional branch");
                return false;
            }
            branch->span = lower_span_for_tree(context, frame->node);
            if (!n00bb_mark_condition(context, construct, arm->condition))
                return false;
            context->block = arm->body_block;
            context->reachable = true;
        }
        else if (arm_index < frame->arms.len && frame->jumping) {
            if (jumping_condition(visit->grammar, visit->node)) {
                if (!n00bb_close_condition_scratch(lower)) return false;
            }
            else if (context->reachable) {
                ir_span_t span = lower_span_for_tree(context, frame->node);
                parse_tree_t *operand_node =
                    condition_core(visit->grammar, visit->node);
                ir_value_t *operand = condition_value(context, operand_node);
                th_type *bool_type = th_name(tu_state(lower->unit)->types,
                                             CORE_STRING_STATIC("bool"));
                if (!operand || !bool_type) {
                    lower_condition_error(context, operand_node, operand,
                        logical_construct(visit->grammar, frame->node));
                    return false;
                }
                if (!n00bb_emit_branch(context, operand, frame->jump_true,
                                       frame->jump_false, span)
                    || !n00bb_mark_condition(context, logical_construct(
                           visit->grammar, frame->node), operand_node))
                    return false;
            }
            context->reachable = false;
        }
        else if (arm_index < frame->arms.len) {
            if (frame->expression && context->reachable) {
                parse_tree_t *expression = frame->short_circuit
                    ? core_tree_child(visit->node,
                        core_tree_num_children(visit->node) - 1)
                    : value_body_expression(
                        visit->grammar, visit->node);
                ir_value_t *value = expression
                    ? ir_lower_node_value(context, expression) : nullptr;
                if (frame->short_circuit && !value) {
                    lower_condition_error(context, expression, value,
                        logical_construct(visit->grammar, frame->node));
                    return false;
                }
                ir_instr_t *store = value
                    ? ir_lower_emit_instruction(context, IR_OP_COPY,
                        lower_span_for_tree(context, visit->node)) : nullptr;
                if (!store || !ir_instr_set_dest_local(
                                  store, frame->result_local)
                    || !ir_instr_operand_value(store, value)
                    || (frame->short_circuit && !n00bb_mark_condition(context,
                            logical_construct(visit->grammar, frame->node),
                            expression))) {
                    lower_internal(context,
                        lower_span_for_tree(context, visit->node),
                        "an if-expression arm produced no value");
                    return false;
                }
            }
            if (context->reachable) {
                ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
                if (!jump || !ir_instr_operand_block(jump, frame->join)) {
                    lower_internal(context,
                        lower_span_for_tree(context, frame->node),
                        "could not emit branch join");
                    return false;
                }
                jump->span = lower_span_for_tree(context, frame->node);
                frame->join_reachable = true;
            }
            context->block = arm_index + 1 < frame->arms.len
                ? frame->arms.data[arm_index + 1].test_block
                : frame->else_body ? frame->else_block : frame->join;
            context->reachable = true;
        }
        else {
            if (frame->expression && context->reachable) {
                parse_tree_t *expression = value_body_expression(
                    visit->grammar, visit->node);
                ir_value_t *value = expression
                    ? ir_lower_node_value(context, expression) : nullptr;
                ir_instr_t *store = value
                    ? ir_lower_emit_instruction(context, IR_OP_COPY,
                        lower_span_for_tree(context, visit->node)) : nullptr;
                if (!store || !ir_instr_set_dest_local(
                                  store, frame->result_local)
                    || !ir_instr_operand_value(store, value)) {
                    lower_internal(context,
                        lower_span_for_tree(context, visit->node),
                        "an if-expression else arm produced no value");
                    return false;
                }
            }
            if (context->reachable) {
                ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
                if (!jump || !ir_instr_operand_block(jump, frame->join)) {
                    lower_internal(context,
                        lower_span_for_tree(context, frame->node),
                        "could not emit branch join");
                    return false;
                }
                jump->span = lower_span_for_tree(context, frame->node);
                frame->join_reachable = true;
            }
            context->block = frame->join;
            context->reachable = frame->join_reachable;
        }
        return true;
    }
    for (size_t i = lower->loops.len; i > 0; i--) {
        n00bb_loop_frame_t *frame = &lower->loops.data[i - 1];
        if (!frame->active || frame->body != visit->node) continue;
        if (entering) {
            if (frame->is_range)
                return n00bb_lower_range_body_enter(context, lower, frame);
            if (frame->is_container)
                return n00bb_lower_container_body_enter(context, lower,
                                                        frame);
            if (jumping_condition(visit->grammar, frame->condition)) {
                if (!n00bb_close_condition_scratch(lower)) return false;
                context->block = frame->body_block;
                context->reachable = true;
                return true;
            }
            ir_value_t *condition = condition_value(context,
                                                    frame->condition);
            th_type *bool_type = th_name(tu_state(lower->unit)->types, CORE_STRING_STATIC("bool"));
            if (!condition || !bool_type) {
                lower_condition_error(context, frame->condition, condition,
                                      N00B_TEXT_PHRASE_A_WHILE_CONDITION);
                return false;
            }
            ir_instr_t *literal = condition->def;
            bool forever = literal && literal->op == IR_OP_CONST
                && literal->n_operands == 1
                && literal->operands[0].kind == IR_OPD_CONST
                && !literal->operands[0].lit.is_float
                && literal->operands[0].lit.i.mag != 0;
            ir_instr_t *branch = ir_instr_add(context->block,
                forever ? IR_OP_JUMP : IR_OP_BRANCH);
            if (!branch
                || (!forever && !ir_instr_operand_value(branch, condition))
                || !ir_instr_operand_block(branch, frame->body_block)
                || (!forever
                    && !ir_instr_operand_block(branch, frame->exit))) {
                lower_internal(context,
                    lower_span_for_tree(context, frame->node),
                    "could not emit while condition branch");
                return false;
            }
            branch->span = lower_span_for_tree(context, frame->node);
            if (!forever && !n00bb_mark_condition(context,
                    N00B_TEXT_PHRASE_A_WHILE_CONDITION, frame->condition))
                return false;
            context->block = frame->body_block;
            context->reachable = true;
        }
        else {
            if (context->reachable) {
                if (!n00bb_add_iteration_backedge(frame, context->block)) {
                    lower_internal(context,
                        lower_span_for_tree(context, frame->node),
                        "could not add the iteration cursor backedge");
                    return false;
                }
                ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
                if (!jump || !ir_instr_operand_block(
                                 jump, frame->continue_block)) {
                    lower_internal(context,
                        lower_span_for_tree(context, frame->node),
                        "could not emit while back edge");
                    return false;
                }
                jump->span = lower_span_for_tree(context, frame->node);
            }
            if (frame->binding_scope_active
                && !ir_lower_bindings_restore(
                    context, frame->binding_checkpoint)) {
                lower_internal(context,
                    lower_span_for_tree(context, frame->node),
                    "could not restore bindings after loop body");
                return false;
            }
            frame->binding_scope_active = false;
            context->block = frame->exit;
            context->reachable = true;
        }
        return true;
    }
    for (size_t i = lower->switches.len; i > 0; i--) {
        n00bb_switch_frame_t *frame = &lower->switches.data[i - 1];
        if (!frame->active) continue;
        bool matched = false;
        size_t case_index = frame->cases.len;
        for (size_t j = 0; j < frame->cases.len; j++)
            if (frame->cases.data[j].body == visit->node) {
                case_index = j;
                matched = true;
                break;
            }
        bool is_default = frame->else_body == visit->node;
        if (!matched && !is_default) continue;
        if (entering) {
            if (!n00bb_emit_case_tests(context, lower, frame,
                                       is_default ? frame->cases.len
                                                  : case_index))
                return false;
            context->block = is_default ? frame->default_block
                : frame->cases.data[case_index].block;
            context->reachable = true;
        }
        else {
            if (frame->expression && context->reachable) {
                parse_tree_t *expression = value_body_expression(
                    visit->grammar, visit->node);
                ir_value_t *value = expression
                    ? ir_lower_node_value(context, expression) : nullptr;
                ir_instr_t *store = value
                    ? ir_lower_emit_instruction(context, IR_OP_COPY,
                        lower_span_for_tree(context, visit->node)) : nullptr;
                if (!store || !ir_instr_set_dest_local(
                                  store, frame->result_local)
                    || !ir_instr_operand_value(store, value)) {
                    lower_internal(context,
                        lower_span_for_tree(context, visit->node),
                        "a switch-expression arm produced no value");
                    return false;
                }
            }
            if (context->reachable) {
                ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
                if (!jump || !ir_instr_operand_block(jump, frame->join)) {
                    lower_internal(context,
                        lower_span_for_tree(context, frame->node),
                        "could not emit switch join");
                    return false;
                }
                jump->span = lower_span_for_tree(context, frame->node);
                frame->join_reachable = true;
            }
            context->reachable = false;
        }
        return true;
    }
    return true;
}

bool
n00bb_resume_unreachable_statement(ir_lower_context_t *context,
                                   parse_visit_t *visit, bool entering,
                                   void *opaque)
{
    (void)opaque;
    (void)visit;
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    if (!entering || !lower || context->reachable) return true;
    ir_block_t *block = n00bb_branch_block(context, lower, "unreachable");
    if (!block) return false;
    context->block = block;
    context->reachable = true;
    return true;
}

static bool
n00bb_typecase_collect(parse_grammar_t *grammar, parse_tree_t *node,
                       n00bb_typecase_frame_t *frame,
                       ir_lower_context_t *context)
{
    if (nt_is(grammar, node, CORE_STRING_STATIC("typecase-case-block"))
        || nt_is(grammar, node, CORE_STRING_STATIC("typecase-value-case-block"))) {
        n00bb_lower_context_t *lower = context->user;
        n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
        n00bb_node_facts_t *facts = n00bb_node_facts(node);
        n00bb_typecase_arm_types_t *types = state && facts
            ? facts->typecase_arm_types : nullptr;
        th_type *type = types && types->count
            ? types->count == 1 ? types->types[0]
                : th_sum(state->types, TH_UNION, types->types, types->count)
            : nullptr;
        parse_tree_t *body = child_nt(grammar, node, CORE_STRING_STATIC("body"));
        if (!body) body = child_nt(grammar, node, CORE_STRING_STATIC("value-body"));
        ir_block_t *block = frame->active
            ? n00bb_branch_block(context, context->user, "typecase_arm")
            : nullptr;
        if (!type || !body || (frame->active && !block)) {
            lower_internal(context, lower_span_for_tree(context, node),
                "typecase arm has no resolved types or body");
            return false;
        }
        core_list_push(frame->arms, ((n00bb_typecase_arm_t){
            .node = node,
            .body = body,
            .type = type,
            .test = n00bb_case_test_type(state->types, lower->function_symbol,
                                         type),
            .subject_shadow = parse_tree_resolution(node).kind
                == PARSE_NODE_RESOLUTION_SYMBOL
                ? parse_tree_resolution(node).target.symbol : nullptr,
            .block = block,
        }));
        return true;
    }
    if (nt_is(grammar, node, CORE_STRING_STATIC("case-else"))
        || nt_is(grammar, node, CORE_STRING_STATIC("typecase-value-else"))) {
        frame->else_body = child_nt(grammar, node, CORE_STRING_STATIC("body"));
        if (!frame->else_body)
            frame->else_body = child_nt(grammar, node, CORE_STRING_STATIC("value-body"));
        return frame->else_body != nullptr;
    }
    if (!node || core_tree_is_leaf(node)) return true;
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!n00bb_typecase_collect(grammar, core_tree_child(node, i), frame,
                                    context)) return false;
    return true;
}

static th_type *
n00bb_case_test_type(th_ctx *types, parse_sym_entry_t *function,
                     th_type *type)
{
    th_type *resolved = type ? th_resolve(type) : nullptr;
    th_comb  comb     = TH_INTERSECTION;
    if (resolved && th_sum_comb(resolved, &comb) && comb == TH_UNION) {
        size_t    n    = th_sum_len(resolved);
        th_type **arms = core_alloc_array(th_type *, n);
        for (size_t i = 0; i < n; i++)
            arms[i] = n00bb_case_test_type(types, function,
                                           th_sum_arm(resolved, i));
        th_type *sum = th_sum(types, TH_UNION, arms, n);
        core_free(arms);
        return sum;
    }
    if (!resolved || th_kind_of(resolved) != TH_VAR) return type;
    core_list_t(th_trait_requirement) bounds = n00bb_declared_bounds(function);
    th_type *test = type;
    for (size_t i = 0; i < bounds.len; i++)
        if (th_resolve(bounds.data[i].subject) == resolved
            && bounds.data[i].kind == TH_CON_IMPLEMENTS)
            test = th_trait(types, bounds.data[i].iface, nullptr, 0,
                            nullptr, 0);
    core_list_free(bounds);
    return test;
}

static bool
n00bb_typecase_emit_dispatch(ir_lower_context_t *context,
                             n00bb_typecase_frame_t *frame)
{
    ir_value_t *subject = ir_lower_node_value(context, frame->subject_node);
    th_type *bool_type = th_name(tu_state(
        ((n00bb_lower_context_t *)context->user)->unit)->types, CORE_STRING_STATIC("bool"));
    if (!subject || !bool_type) return false;
    frame->subject_value = subject;
    ir_block_t *test_block = context->block;
    ir_span_t span = lower_span_for_tree(context, frame->node);
    for (size_t i = 0; i < frame->arms.len; i++) {
        n00bb_typecase_arm_t *arm = &frame->arms.data[i];
        ir_block_t *next = i + 1 < frame->arms.len
            ? n00bb_branch_block(context, context->user, "typecase_test")
            : frame->else_body ? frame->else_block : frame->unmatched_block;
        ir_instr_t *test = ir_instr_add(test_block, IR_OP_TYPE_TEST);
        ir_instr_t *branch = test
            ? ir_instr_add(test_block, IR_OP_BRANCH) : nullptr;
        if (!next || !test || !ir_lower_set_temp_dest(context, test,
            CORE_STRING_STATIC("__n00bb_type_test_"), bool_type)
            || !ir_instr_operand_value(test, subject)
            || !ir_instr_operand_type(test, arm->test)
            || !branch || !ir_instr_operand_value(branch, test->dest)
            || !ir_instr_operand_block(branch, arm->block)
            || !ir_instr_operand_block(branch, next)) return false;
        test->span = span;
        branch->span = span;
        test_block = next;
    }
    if (!frame->else_body) {
        ir_instr_t *unreachable = ir_instr_add(test_block, IR_OP_UNREACHABLE);
        if (!unreachable) return false;
        unreachable->span = span;
    }
    frame->emitted = true;
    return true;
}

bool
n00bb_lower_typecase_scope(ir_lower_context_t *context, parse_visit_t *visit,
                           bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !visit->annot
        || !core_string_eq(visit->annot->scope_tag,
                           CORE_STRING_STATIC("block"))) return true;
    for (size_t i = lower->typecases.len; i > 0; i--) {
        n00bb_typecase_frame_t *frame = &lower->typecases.data[i - 1];
        size_t arm_index = frame->arms.len;
        for (size_t a = 0; a < frame->arms.len; a++)
            if (frame->arms.data[a].body == visit->node) {
                arm_index = a;
                break;
            }
        bool is_else = frame->else_body == visit->node;
        if (arm_index == frame->arms.len && !is_else) continue;
        if (!frame->active) return true;
        if (entering) {
            if (!frame->emitted && !n00bb_typecase_emit_dispatch(context,
                                                                   frame)) {
                lower_internal(context,
                    lower_span_for_tree(context, frame->node),
                    "could not emit typecase dispatch");
                return false;
            }
            if (is_else) {
                context->block = frame->else_block;
                context->reachable = true;
                return true;
            }
            n00bb_typecase_arm_t *arm = &frame->arms.data[arm_index];
            context->block = arm->block;
            context->reachable = true;
            ir_instr_t *narrow = ir_lower_emit_instruction(context,
                IR_OP_TYPE_NARROW, lower_span_for_tree(context, visit->node));
            if (!narrow || !ir_lower_set_temp_dest(context, narrow,
                                                    CORE_STRING_STATIC("__n00bb_narrow_"),
                                                    arm->type)
                || !ir_instr_operand_value(narrow, frame->subject_value)
                || !ir_instr_operand_type(narrow, arm->type)) {
                lower_internal(context,
                    lower_span_for_tree(context, frame->node),
                    "could not narrow the typecase subject");
                return false;
            }
            frame->binding_checkpoint =
                ir_lower_bindings_checkpoint(context);
            ir_local_t *local = ir_lower_bound_local(context,
                                                     frame->subject_symbol);
            if (!arm->subject_shadow
                || !ir_lower_bind_scoped_value(context, arm->subject_shadow,
                                            narrow->dest, local)) {
                lower_internal(context,
                    lower_span_for_tree(context, frame->node),
                    "could not bind the narrowed typecase subject");
                return false;
            }
            return true;
        }
        if (frame->expression && context->reachable) {
            parse_tree_t *expression = value_body_expression(
                visit->grammar, visit->node);
            ir_value_t *value = expression && frame->result_local
                ? ir_lower_node_value(context, expression) : nullptr;
            ir_instr_t *store = value
                ? ir_lower_emit_instruction(context, IR_OP_COPY,
                    lower_span_for_tree(context, visit->node)) : nullptr;
            if (!store || !frame->result_local
                || !ir_instr_set_dest_local(store, frame->result_local)
                || !ir_instr_operand_value(store, value)) {
                lower_internal(context,
                    lower_span_for_tree(context, visit->node),
                    "could not store a typecase-expression arm value");
                return false;
            }
        }
        if (!is_else
            && !ir_lower_bindings_restore(context,
                                            frame->binding_checkpoint)) {
            lower_internal(context,
                lower_span_for_tree(context, frame->node),
                "could not restore the typecase subject binding");
            return false;
        }
        if (context->reachable) {
            ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
            if (!jump || !ir_instr_operand_block(jump, frame->join)) {
                lower_internal(context,
                    lower_span_for_tree(context, frame->node),
                    "could not emit typecase join");
                return false;
            }
            jump->span = lower_span_for_tree(context, frame->node);
            frame->join_reachable = true;
        }
        return true;
    }
    return true;
}

static bool
n00bb_lower_typecase_annotation(ir_lower_context_t *context,
                                parse_visit_t *visit, bool entering)
{
    n00bb_lower_context_t *lower = context->user;
    if (entering) {
        bool expression = nt_is(visit->grammar, visit->node,
                                CORE_STRING_STATIC("typecase-value-expr"));
        parse_cf_label_t *label = parse_cf_label_lookup(
            tu_state(lower->unit)->annotations, visit->node);
        parse_tree_t *subject_node = label
            ? n00bb_simple_lvalue(label->cond) : nullptr;
        parse_sym_entry_t *symbol = subject_node
            ? parse_tree_sym(subject_node) : nullptr;
        if (!label || !subject_node || !symbol
            || (symbol->kind != PARSE_SYM_VARIABLE
                && symbol->kind != PARSE_SYM_PARAM)) {
            parse_tree_t *subject = label ? label->cond : visit->node;
            lower_report(context, N00B_DIAG_SUBJECT_NOT_VARIABLE,
                lower_span_for_tree(context, subject),
                N00B_TEXT_A_TYPECASE_SUBJECT_MUST_BE,
                N00BB_NO_ARGS);
            lower_help(context, N00B_TEXT_STORE_THE_VALUE_IN_A, N00BB_NO_ARGS);
            return false;
        }
        n00bb_typecase_frame_t frame = {
            .node = visit->node,
            .label = n00bb_control_label(visit->grammar, visit->node,
                                         CORE_STRING_STATIC("labeled-typecase")),
            .subject_node = subject_node,
            .subject_symbol = symbol,
            .arms = core_list_new(n00bb_typecase_arm_t),
            .order = ++lower->next_control_order,
            .expression = expression,
            .active = context->reachable,
        };
        if (!n00bb_typecase_collect(visit->grammar, label->then_body,
                                    &frame, context)) {
            core_list_free(frame.arms);
            return false;
        }
        if (!frame.arms.len) {
            core_list_free(frame.arms);
            lower_report(context, N00B_DIAG_TYPECASE_WITHOUT_CASES,
                lower_span_for_tree(context, visit->node),
                N00B_TEXT_THIS_TYPECASE_ON_HAS_NO,
                N00BB_ARGS(ARG_TEXT(symbol->name)));
            return false;
        }
        if (frame.active) {
            frame.join = n00bb_branch_block(context, lower, "typecase_join");
            if (expression) {
                th_type *result_type = n00bb_node_result_type(context,
                                                              visit->node);
                char local_buffer[64];
                core_string_t local_name = n00bb_format_name(local_buffer,
                    sizeof(local_buffer), "__n00bb_typecase_result_%llu",
                    (unsigned long long)context->next_value_id++);
                frame.result_local = result_type
                    ? ir_local_add(context->function, local_name, result_type)
                    : nullptr;
                if (!frame.result_local) {
                    core_list_free(frame.arms);
                    lower_internal(context,
                        lower_span_for_tree(context, visit->node),
                        "typecase expression has no inferred result type");
                    return false;
                }
            }
            frame.else_block = frame.else_body
                ? n00bb_branch_block(context, lower, "typecase_else") : nullptr;
            frame.unmatched_block = frame.else_body ? frame.else_block
                : n00bb_branch_block(context, lower, "typecase_unmatched");
            if (!frame.join || (frame.else_body && !frame.else_block)
                || !frame.unmatched_block) {
                core_list_free(frame.arms);
                lower_internal(context,
                    lower_span_for_tree(context, visit->node),
                    "could not allocate typecase control-flow blocks");
                return false;
            }
        }
        core_list_push(lower->typecases, frame);
        return true;
    }
    if (!lower->typecases.len
        || lower->typecases.data[lower->typecases.len - 1].node != visit->node)
        return true;
    n00bb_typecase_frame_t frame =
        lower->typecases.data[--lower->typecases.len];
    core_list_free(frame.arms);
    if (!frame.active) return true;
    if (!frame.join_reachable) {
        ir_instr_t *unreachable = ir_instr_add(frame.join, IR_OP_UNREACHABLE);
        if (!unreachable) return false;
        unreachable->span = lower_span_for_tree(context, frame.node);
    }
    if (frame.expression && frame.join_reachable) {
        ir_instr_t *read = ir_instr_add(frame.join, IR_OP_COPY);
        if (!read || !ir_lower_set_temp_dest(context, read,
            CORE_STRING_STATIC("__n00bb_typecase_value_"), frame.result_local->type)
            || !ir_instr_operand_local(read, frame.result_local)
            || !ir_lower_set_node_value(context, frame.node, read->dest)) {
            lower_internal(context,
                lower_span_for_tree(context, frame.node),
                "could not read the typecase expression result");
            return false;
        }
        read->span = lower_span_for_tree(context, frame.node);
    }
    context->block = frame.join;
    context->reachable = frame.join_reachable;
    return true;
}

bool
n00bb_lower_loop_annotation(ir_lower_context_t *context, parse_visit_t *visit,
                            bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (!lower) return false;
    if (!entering) {
        if (!lower->loops.len
            || lower->loops.data[lower->loops.len - 1].node != visit->node)
            return true;
        n00bb_loop_frame_t frame =
            lower->loops.data[--lower->loops.len];
        core_list_free(frame.iteration_bindings);
        if (frame.active) {
            context->block = frame.exit;
            context->reachable = true;
        }
        return true;
    }

    n00bb_tu_state_t *tu = tu_state(lower->unit);
    parse_cf_label_t *label = tu
        ? parse_cf_label_lookup(tu->annotations, visit->node) : nullptr;
    if (!label || label->kind != PARSE_CF_LOOP
        || !label->cond || !label->then_body)
        return false;
    if (!context->reachable) {
        core_list_push(lower->loops, ((n00bb_loop_frame_t){
            .node = visit->node,
            .iteration_bindings = core_list_new(parse_sym_entry_ptr_t),
            .active = false,
        }));
        return true;
    }

    bool while_loop = nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("while-stmt"));
    bool range_loop = nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("for-stmt"))
        && nt_is(visit->grammar, label->cond, CORE_STRING_STATIC("range-loop"));
    bool container_loop = nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("for-stmt"))
        && child_nt(visit->grammar, visit->node, CORE_STRING_STATIC("for-in-target-list"));
    if (!while_loop && !range_loop && !container_loop) {
        lower_report(context, N00B_DIAG_NOT_SUPPORTED_YET,
            lower_span_for_tree(context, visit->node),
            N00B_TEXT_NOT_SUPPORTED_YET_THIS_FORM,
            N00BB_NO_ARGS);
        lower_help(context, N00B_TEXT_USE_FOR_I_FROM_A, N00BB_NO_ARGS);
        return false;
    }

    parse_tree_t *range_start = nullptr;
    parse_tree_t *range_end = nullptr;
    parse_tree_t *range_step = nullptr;
    parse_sym_entry_t *iterator_symbol = nullptr;
    ir_local_t *iterator = nullptr;
    ir_local_t *range_binding = nullptr;
    parse_tree_t *iteration_source = nullptr;
    n00bb_iteration_plan_t *iteration_plan = nullptr;
    core_list_t(parse_sym_entry_ptr_t) iteration_bindings =
        core_list_new(parse_sym_entry_ptr_t);
    bool inclusive = false;
    if (range_loop) {
        parse_tree_t *vars = child_nt(visit->grammar, visit->node,
                                      CORE_STRING_STATIC("for-range-var"));
        parse_tree_t *parts = child_nt(visit->grammar, label->cond,
                                       CORE_STRING_STATIC("range-parts"));
        parse_tree_t *step_node = child_nt(visit->grammar, label->cond,
                                           CORE_STRING_STATIC("range-step"));
        core_list_t(parse_tree_ptr_t) range_bindings =
            core_list_new(parse_tree_ptr_t);
        collect_for_bindings(visit->grammar, vars, &range_bindings);
        if (!vars || range_bindings.len != 1 || !parts
            || core_tree_num_children(parts) < 3) {
            core_list_free(range_bindings);
            lower_report(context, N00B_DIAG_NOT_SUPPORTED_YET,
                lower_span_for_tree(context, label->cond),
                N00B_TEXT_NOT_SUPPORTED_YET_A_RANGE,
                N00BB_NO_ARGS);
            return false;
        }
        for (size_t i = 0; i < core_tree_num_children(parts); i++) {
            parse_tree_t *child = core_tree_child(parts, i);
            if (nt_is(visit->grammar, child, CORE_STRING_STATIC("expression"))) {
                if (!range_start) range_start = child;
                else range_end = child;
            }
            if (nt_is(visit->grammar, child, CORE_STRING_STATIC("range-sep")))
                inclusive = core_string_eq(first_token_text(child),
                                             CORE_STRING_STATIC("to"));
        }
        range_step = step_node
            ? child_nt(visit->grammar, step_node, CORE_STRING_STATIC("expression")) : nullptr;
        n00bb_static_value_t step_constant = {0};
        if (range_step
            && n00bb_trivial_static_value(visit->grammar, tu_state(lower->unit),
                                          range_step, &step_constant)
            && step_constant.kind == N00BB_STATIC_VALUE_INT
            && step_constant.as.integer == 0) {
            core_list_free(range_bindings);
            lower_report(context, N00B_DIAG_ZERO_RANGE_STEP,
                lower_span_for_tree(context, range_step),
                N00B_TEXT_THIS_RANGE_STEPS_BY_ZERO,
                N00BB_NO_ARGS);
            lower_help(context, N00B_TEXT_STEP_BY_A_NONZERO_AMOUNT,
                N00BB_NO_ARGS);
            return false;
        }
        iterator_symbol = parse_tree_sym(range_bindings.data[0]);
        core_list_free(range_bindings);
        th_type *iterator_type = iterator_symbol
            ? iterator_symbol->type : nullptr;
        if (!range_start || !range_end || !iterator_symbol || !iterator_type) {
            lower_internal(context,
                lower_span_for_tree(context, visit->node),
                "range loop variable or endpoint types are unresolved");
            return false;
        }
        char local_buffer[48];
        core_string_t local_name = n00bb_format_name(local_buffer,
            sizeof(local_buffer), "__n00bb_for_%llu",
            (unsigned long long)context->next_value_id++);
        iterator = ir_local_add(context->function, local_name, iterator_type);
        local_name = n00bb_format_name(local_buffer, sizeof(local_buffer),
            "__n00bb_for_binding_%llu",
            (unsigned long long)context->next_value_id++);
        range_binding = iterator
            ? ir_local_add(context->function, local_name, iterator_type)
            : nullptr;
        if (!range_binding || !n00bb_bind_local(context, iterator_symbol,
                range_binding, lower_span_for_tree(context, visit->node))) {
            lower_internal(context,
                lower_span_for_tree(context, visit->node),
                "could not bind range loop variable");
            return false;
        }
    }
    if (container_loop) {
        parse_tree_t *target = child_nt(visit->grammar, visit->node,
                                         CORE_STRING_STATIC("for-in-target-list"));
        iteration_source = child_nt(visit->grammar, target, CORE_STRING_STATIC("expression"));
        n00bb_node_facts_t *facts = n00bb_node_facts(visit->node);
        iteration_plan = facts ? facts->iteration_plan : nullptr;
        if (!iteration_plan && iteration_source) {
            iteration_plan = core_arena_alloc(
                &tu_state(lower->unit)->semantic_values,
                sizeof(*iteration_plan));
            if (iteration_plan)
                *iteration_plan = (n00bb_iteration_plan_t){
                    .cursor_type = th_var(ir_module_types(context->module)),
                    .item_type = th_var(ir_module_types(context->module)),
                };
        }
        parse_tree_t *vars = child_nt(visit->grammar, visit->node,
                                      CORE_STRING_STATIC("for-var-list"));
        core_list_t(parse_tree_ptr_t) binding_nodes =
            core_list_new(parse_tree_ptr_t);
        collect_for_bindings(visit->grammar, vars, &binding_nodes);
        bool valid = iteration_source && iteration_plan && binding_nodes.len;
        for (size_t i = 0; valid && i < binding_nodes.len; i++) {
            parse_sym_entry_t *symbol = parse_tree_sym(binding_nodes.data[i]);
            if (symbol && parse_sym_binding_origin(symbol)) {
                core_list_push(iteration_bindings, symbol);
                continue;
            }
            report_outer_loop_name(context, binding_nodes.data[i], symbol);
            char local_buffer[64];
            core_string_t local_name = n00bb_format_name(local_buffer,
                sizeof(local_buffer), "__n00bb_for_item_%llu",
                (unsigned long long)context->next_value_id++);
            ir_local_t *local = symbol && symbol->type
                ? ir_local_add(context->function, local_name, symbol->type)
                : nullptr;
            valid = local && n00bb_bind_local(context, symbol, local,
                lower_span_for_tree(context, binding_nodes.data[i]));
            if (valid) core_list_push(iteration_bindings, symbol);
        }
        core_list_free(binding_nodes);
        if (!valid) {
            core_list_free(iteration_bindings);
            lower_internal(context,
                lower_span_for_tree(context, visit->node),
                "container loop bindings or iterator type are unresolved");
            return false;
        }
    }

    ir_block_t *header = n00bb_branch_block(context, lower, "while_header");
    ir_block_t *body = n00bb_branch_block(context, lower, "while_body");
    ir_block_t *latch = range_loop
        ? n00bb_branch_block(context, lower, "for_latch") : header;
    ir_block_t *increment = range_loop && inclusive
        ? n00bb_branch_block(context, lower, "for_increment") : nullptr;
    ir_block_t *exit = n00bb_branch_block(context, lower, "while_exit");
    ir_instr_t *jump = header && !range_loop && !container_loop
        ? ir_instr_add(context->block, IR_OP_JUMP) : nullptr;
    if (!header || !body || !latch || (range_loop && inclusive && !increment)
        || !exit || (while_loop && (!jump
            || !ir_instr_operand_block(jump, header)))) {
        core_list_free(iteration_bindings);
        lower_internal(context,
                    lower_span_for_tree(context, visit->node),
                    "could not create while control-flow blocks");
        return false;
    }
    if (jump) jump->span = lower_span_for_tree(context, visit->node);
    core_list_push(lower->loops, ((n00bb_loop_frame_t){
        .node = visit->node,
        .label = n00bb_control_label(visit->grammar, visit->node,
                                    while_loop
                                        ? CORE_STRING_STATIC("labeled-while")
                                        : CORE_STRING_STATIC("labeled-for")),
        .condition = label->cond,
        .body = label->then_body,
        .range_start = range_start,
        .range_end = range_end,
        .range_step = range_step,
        .header = header,
        .body_block = body,
        .latch = latch,
        .increment = increment,
        .exit = exit,
        .continue_block = range_loop ? latch : header,
        .iterator = iterator,
        .range_binding = range_binding,
        .iterator_symbol = iterator_symbol,
        .iteration_source = iteration_source,
        .iteration_plan = iteration_plan,
        .iteration_bindings = iteration_bindings,
        .range_inclusive = inclusive,
        .is_range = range_loop,
        .is_container = container_loop,
        .order = ++lower->next_control_order,
        .active = true,
    }));
    if (while_loop) context->block = header;
    context->reachable = true;
    return true;
}

bool
n00bb_lower_jump_annotation(ir_lower_context_t *context, parse_visit_t *visit,
                            bool entering, void *state)
{
    (void)state;
    core_string_t name = parse_tree_nt_name(visit->grammar, visit->node);
    if (core_string_eq(name, CORE_STRING_STATIC("return-stmt"))
        && !entering && context->reachable) {
        n00bb_lower_context_t *lower = context->user;
        parse_tree_t *expression = child_nt(visit->grammar, visit->node,
                                            CORE_STRING_STATIC("expression"));
        ir_shape_t *expected = lower && lower->bound_return_shape
            ? lower->bound_return_shape
            : lower && lower->function_symbol
                ? parse_sym_binding(lower->function_symbol,
                    CORE_STRING_STATIC("ir.return-shape")) : nullptr;
        if (expression && n00bb_shape_has_constraints(expected)) {
            ir_value_t *value = ir_lower_node_value(context, expression);
            if (!value || !n00bb_attach_expression_shape(context,
                    visit->grammar, expression, value)
                || !n00bb_emit_sequence_shape_check(context, value,
                    expected, lower_span_for_tree(context, visit->node)))
                return false;
        }
        if (!n00bb_move_source(context, visit->grammar, expression))
            return false;
        ir_value_t *result = expression
            ? ir_lower_node_value(context, expression) : nullptr;
        if (result && result->kind == IR_VAL_GLOBAL) {
            ir_value_t *read = n00bb_emit_temp(context, IR_OP_COPY,
                result->type, result,
                lower_span_for_tree(context, expression));
            if (!read || !ir_lower_set_node_value(context, expression, read))
                return false;
        }
        return n00bb_emit_with_success(context, visit->node);
    }
    if (core_string_eq(name, CORE_STRING_STATIC("value-stmt"))) {
        if (entering || !context->reachable) return true;
        n00bb_lower_context_t *lower = context->user;
        for (size_t i = lower->value_blocks.len; i > 0; i--) {
            n00bb_value_block_frame_t *frame =
                &lower->value_blocks.data[i - 1];
            if (!frame->active
                || !node_is_within(visit->node, frame->node)) continue;
            parse_tree_t *expression = child_nt(visit->grammar, visit->node,
                                                CORE_STRING_STATIC("expression"));
            ir_value_t *value = expression
                ? ir_lower_node_value(context, expression)
                : nullptr;
            if (value && !n00bb_move_source(context, visit->grammar,
                                            expression))
                return false;
            ir_instr_t *store = value
                ? ir_lower_emit_instruction(context, IR_OP_COPY,
                    lower_span_for_tree(context, visit->node)) : nullptr;
            bool stored = store && ir_instr_set_dest_local(store,
                                                           frame->result)
                && ir_instr_operand_value(store, value)
                && n00bb_emit_scoped_cleanups(context, visit->node,
                                              frame->order, nullptr, true,
                                              UINT64_MAX);
            ir_instr_t *jump = stored
                ? ir_instr_add(context->block, IR_OP_JUMP) : nullptr;
            if (!jump || !ir_instr_operand_block(jump, frame->continuation)) {
                lower_internal(context,
                    lower_span_for_tree(context, visit->node),
                    "could not store a block value and continue");
                return false;
            }
            jump->span = lower_span_for_tree(context, visit->node);
            frame->value_written = true;
            context->block = frame->continuation;
            context->reachable = true;
            return true;
        }
        bool handled_by_value_expression = false;
        for (size_t i = 0; i < lower->tries.len; i++) {
            n00bb_try_frame_t *frame = &lower->tries.data[i];
            if (frame->active && frame->expression
                && node_is_within(visit->node, frame->node)) {
                handled_by_value_expression = true;
                break;
            }
        }
        for (parse_tree_t *ancestor = visit->node;
             ancestor && !handled_by_value_expression;
             ancestor = parse_tree_parent(ancestor)) {
            for (size_t i = 0; i < lower->branches.len; i++) {
                n00bb_branch_frame_t *frame = &lower->branches.data[i];
                if (!frame->active || !frame->expression) continue;
                for (size_t arm = 0; arm < frame->arms.len; arm++)
                    if (frame->arms.data[arm].body == ancestor)
                        handled_by_value_expression = true;
                if (frame->else_body == ancestor)
                    handled_by_value_expression = true;
            }
            for (size_t i = 0; i < lower->switches.len; i++) {
                n00bb_switch_frame_t *frame = &lower->switches.data[i];
                if (!frame->active || !frame->expression) continue;
                for (size_t arm = 0; arm < frame->cases.len; arm++)
                    if (frame->cases.data[arm].body == ancestor)
                        handled_by_value_expression = true;
                if (frame->else_body == ancestor)
                    handled_by_value_expression = true;
            }
            for (size_t i = 0; i < lower->typecases.len; i++) {
                n00bb_typecase_frame_t *frame = &lower->typecases.data[i];
                if (!frame->active || !frame->expression) continue;
                for (size_t arm = 0; arm < frame->arms.len; arm++)
                    if (frame->arms.data[arm].body == ancestor)
                        handled_by_value_expression = true;
                if (frame->else_body == ancestor)
                    handled_by_value_expression = true;
            }
        }
        if (handled_by_value_expression) return true;

        bool function_body_value = false;
        bool nested_value_body = false;
        for (parse_tree_t *ancestor = parse_tree_parent(visit->node);
             ancestor; ancestor = parse_tree_parent(ancestor)) {
            if (nt_is(visit->grammar, ancestor, CORE_STRING_STATIC("value-body")))
                nested_value_body = true;
            if (nt_is(visit->grammar, ancestor, CORE_STRING_STATIC("body"))) {
                function_body_value = ancestor == lower->function_body;
                break;
            }
        }
        if (!function_body_value || nested_value_body) {
            lower_report(context, N00B_DIAG_NOT_SUPPORTED_YET,
                lower_span_for_tree(context, visit->node),
                N00B_TEXT_NOT_SUPPORTED_YET_A_VALUE,
                N00BB_NO_ARGS);
            return false;
        }
        parse_tree_t *expression = child_nt(visit->grammar, visit->node,
                                            CORE_STRING_STATIC("expression"));
        ir_value_t *value = expression
            ? ir_lower_node_value(context, expression) : nullptr;
        if (!value) {
            lower_internal(context,
                lower_span_for_tree(context, visit->node),
                "function value body has no value");
            return false;
        }
        if (!n00bb_emit_with_success(context, visit->node)) return false;
        if (!n00bb_emit_success_return(context, value,
                &lower->parameters_to_copy_out,
                lower_span_for_tree(context, visit->node))) {
            lower_internal(context,
                lower_span_for_tree(context, visit->node),
                "could not emit function value return");
            return false;
        }
        return true;
    }
    if (core_string_eq(name, CORE_STRING_STATIC("fail-stmt"))) {
        if (entering || !context->reachable) return true;
        n00bb_lower_context_t *lower = context->user;
        parse_tree_t *expression = child_nt(visit->grammar, visit->node,
                                            CORE_STRING_STATIC("expression"));
        ir_value_t *value = expression
            ? ir_lower_node_value(context, expression) : nullptr;
        ir_span_t span = lower_span_for_tree(context, visit->node);
        if (!value) {
            lower_internal(context, span,
                           "fail statement has no failure value");
            return false;
        }
        if (!n00bb_emit_with_failure(context, visit->node))
            return false;
        int routed = n00bb_route_try_error(context, visit->node, value, span);
        if (routed) return routed > 0;
        th_type *failure_type = lower->function_symbol
            ? th_func_error(lower->function_symbol->type) : nullptr;
        if (!failure_type || th_is_nil(failure_type)) {
            parse_sym_entry_t *function = lower_user_function(context);
            if (function)
                lower_report(context, N00B_DIAG_FAIL_WITHOUT_FAILURE_TYPE, span,
                    N00B_TEXT_FAIL_IS_USED_IN_WHICH,
                    N00BB_ARGS(ARG_TEXT(function->name)));
            else
                lower_report(context, N00B_DIAG_FAIL_WITHOUT_FAILURE_TYPE, span,
                    N00B_TEXT_FAIL_IS_USED_OUTSIDE_ANY,
                    N00BB_NO_ARGS);
            if (function)
                lower_help(context, N00B_TEXT_DECLARE_THE_FAILURE_TYPE_AS,
                    N00BB_ARGS(ARG_TEXT(function->name)));
            return false;
        }
        if (!value) {
            lower_internal(context, span, "could not lower the failure value");
            return false;
        }
        ir_instr_t *fail = ir_instr_add(context->block, IR_OP_FAIL);
        if (!fail || !ir_instr_operand_value(fail, value)) {
            lower_internal(context,
                span,
                "could not emit function failure exit");
            return false;
        }
        fail->span = span;
        context->reachable = false;
        return true;
    }
    if (!entering) return true;
    if (core_string_eq(name, CORE_STRING_STATIC("return-stmt"))) return true;
    if (!core_string_eq(name, CORE_STRING_STATIC("break-stmt"))
        && !core_string_eq(name, CORE_STRING_STATIC("continue-stmt"))) {
        lower_internal(context, lower_span_for_tree(context, visit->node),
                       "jump lowering reached a node that is not a jump");
        return false;
    }
    parse_cf_label_t *cf_label = parse_cf_label_lookup(
        tu_state(((n00bb_lower_context_t *)context->user)->unit)->annotations,
        visit->node);
    if (!cf_label || cf_label->kind != PARSE_CF_JUMP) return false;
    n00bb_lower_context_t *lower = context->user;
    core_string_t target_name = core_tree_num_children(visit->node) > 1
        ? first_token_text(core_tree_child(visit->node, 1))
        : (core_string_t){0};
    n00bb_loop_frame_t *target = nullptr;
    for (size_t i = lower->loops.len; i > 0; i--) {
        if (lower->loops.data[i - 1].active
            && (!target_name.data || core_string_eq(
                    lower->loops.data[i - 1].label, target_name))
            && (!target || lower->loops.data[i - 1].order > target->order)) {
            target = &lower->loops.data[i - 1];
        }
    }
    n00bb_switch_frame_t *switch_target = nullptr;
    n00bb_typecase_frame_t *typecase_target = nullptr;
    bool is_break = core_string_eq(name, CORE_STRING_STATIC("break-stmt"));
    if (is_break) {
        uint64_t best_order = target ? target->order : 0;
        for (size_t i = lower->switches.len; i > 0; i--) {
            n00bb_switch_frame_t *candidate = &lower->switches.data[i - 1];
            if (candidate->active && (!target_name.data || core_string_eq(
                    candidate->label, target_name)) && (!switch_target
                || candidate->order > best_order)) {
                switch_target = candidate;
                typecase_target = nullptr;
                target = nullptr;
                best_order = candidate->order;
            }
        }
        for (size_t i = lower->typecases.len; i > 0; i--) {
            n00bb_typecase_frame_t *candidate =
                &lower->typecases.data[i - 1];
            if (candidate->active && (!target_name.data || core_string_eq(
                    candidate->label, target_name))
                && candidate->order > best_order) {
                typecase_target = candidate;
                switch_target = nullptr;
                target = nullptr;
                best_order = candidate->order;
            }
        }
    }
    if (!target && !switch_target && !typecase_target) {
        ir_span_t span = lower_span_for_tree(context, visit->node);
        if (target_name.data)
            lower_report(context, N00B_DIAG_UNKNOWN_JUMP_LABEL, span,
                is_break ? N00B_TEXT_BREAK_NAMES_NO_LABEL
                         : N00B_TEXT_CONTINUE_NAMES_NO_LABEL,
                N00BB_ARGS(ARG_TEXT(target_name)));
        else
            lower_report(context, N00B_DIAG_JUMP_OUTSIDE_LOOP, span,
                is_break ? N00B_TEXT_BREAK_OUTSIDE
                         : N00B_TEXT_CONTINUE_OUTSIDE, N00BB_NO_ARGS);
        return false;
    }
    ir_block_t *destination = typecase_target ? typecase_target->join
        : switch_target ? switch_target->join
        : is_break ? target->exit : target->continue_block;
    if (target) {
        if (!n00bb_emit_scoped_success(context, visit->node, target)
            || (is_break && !n00bb_emit_iteration_cleanup(
                    context, target, visit->node)))
            return false;
        if (!is_break
            && !n00bb_add_iteration_backedge(target, context->block)) {
            lower_internal(context,
                lower_span_for_tree(context, visit->node),
                "could not add the continued iteration cursor backedge");
            return false;
        }
    }
    ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
    if (!jump || !ir_instr_operand_block(jump, destination)) {
        lower_internal(context,
                    lower_span_for_tree(context, visit->node),
                    "could not emit loop jump");
        return false;
    }
    jump->span = lower_span_for_tree(context, visit->node);
    if (switch_target) switch_target->join_reachable = true;
    if (typecase_target) typecase_target->join_reachable = true;
    context->reachable = false;
    return true;
}

ir_lower_result_t
n00bb_lower_return(ir_lower_context_t *context, parse_visit_t *visit,
                   ir_value_t *const *operands, size_t operand_count,
                   th_type *result_type, ir_span_t span, void *state)
{
    (void)visit;
    (void)result_type;
    (void)state;
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    if (!lower || operand_count > 1 || (operand_count && !operands[0]))
        return lower_internal(context, span,
            "return statement received an invalid value");
    ir_value_t *value = operand_count ? operands[0] : nullptr;
    if (!n00bb_emit_success_return(context, value,
            &lower->parameters_to_copy_out, span))
        return lower_internal(context, span,
            "could not emit the function success result and copy-out values");
    return (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr};
}

static bool
n00bb_switch_case_name(parse_grammar_t *grammar, n00bb_lower_context_t *lower,
                       parse_tree_t *item, core_string_t *name)
{
    parse_tree_t *expression = child_nt(grammar, item,
                                        CORE_STRING_STATIC("expression"));
    core_string_t text = {0};
    if (!lower || !expression
        || child_nt(grammar, item, CORE_STRING_STATIC("range-sep"))
        || !simple_identifier_expression(grammar, expression, &text))
        return false;
    parse_sym_entry_t *symbol = attached_symbol(grammar, lower->unit,
                                                expression);
    if (symbol && symbol->kind != PARSE_SYM_ENUM_CONST
        && !n00bb_variant_arm_symbol(grammar, symbol)) return false;
    if (name) *name = text;
    return true;
}

bool
n00bb_prune_case_name(ir_lower_context_t *context, parse_visit_t *visit,
                      bool entering, void *opaque)
{
    (void)opaque;
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    if (!entering || !lower || !lower->switches.len
        || !n00bb_switch_case_name(visit->grammar, lower, visit->node,
                                   nullptr)) return true;
    return ir_lower_prune_node(context, visit->node);
}

static bool
n00bb_switch_collect(parse_grammar_t *grammar, parse_tree_t *node,
                     n00bb_switch_frame_t *frame, ir_lower_context_t *context)
{
    if (!node || core_tree_is_leaf(node)) return true;
    if (nt_is(grammar, node, CORE_STRING_STATIC("switch-case-block"))
        || nt_is(grammar, node, CORE_STRING_STATIC("switch-value-case-block"))) {
        bool expression = nt_is(grammar, node,
            CORE_STRING_STATIC("switch-value-case-block"));
        n00bb_switch_case_t item = {
            .list = child_nt(grammar, node,
                             CORE_STRING_STATIC("case-expr-list")),
            .body = child_nt(grammar, node, expression
                ? CORE_STRING_STATIC("value-body") : CORE_STRING_STATIC("body")),
            .block = n00bb_branch_block(context, context->user, "switch_case"),
            .patterns = core_list_new(n00bb_switch_pattern_t),
        };
        parse_tree_t *list = child_nt(grammar, node,
                                      CORE_STRING_STATIC("case-expr-list"));
        if (!item.body || !item.block || !list) {
            core_list_free(item.patterns);
            return false;
        }
        core_list_t(parse_tree_ptr_t) stack = core_list_new(parse_tree_ptr_t);
        core_list_push(stack, list);
        while (stack.len) {
            parse_tree_t *current = stack.data[--stack.len];
            if (!nt_is(grammar, current, CORE_STRING_STATIC("case-expr-item"))) {
                for (size_t c = core_tree_num_children(current); c > 0; c--)
                    core_list_push(stack, core_tree_child(current, c - 1));
                continue;
            }
            n00bb_switch_pattern_t pattern = {0};
            parse_tree_t *separator = child_nt(grammar, current,
                CORE_STRING_STATIC("range-sep"));
            parse_tree_t *flag_test = child_nt(grammar, current,
                CORE_STRING_STATIC("case-flags-test"));
            if (flag_test) {
                pattern.flags = child_nt(grammar, flag_test,
                                         CORE_STRING_STATIC("case-flags"));
                pattern.node = pattern.flags ? flag_test : nullptr;
                pattern.exact = core_string_eq(first_token_text(flag_test),
                                               CORE_STRING_STATIC("is"));
            }
            else if (separator) {
                pattern.is_range = true;
                pattern.inclusive = core_string_eq(first_token_text(separator),
                                                   CORE_STRING_STATIC("to"));
                for (size_t i = 0; i < core_tree_num_children(current); i++) {
                    parse_tree_t *child = core_tree_child(current, i);
                    if (!nt_is(grammar, child, CORE_STRING_STATIC("expression")))
                        continue;
                    if (!pattern.node) pattern.node = child;
                    else if (!pattern.upper) pattern.upper = child;
                }
            }
            else {
                (void)n00bb_switch_case_name(grammar, context->user, current,
                                             &pattern.name);
                pattern.node = child_nt(grammar, current,
                                        CORE_STRING_STATIC("expression"));
            }
            if (!pattern.node || (pattern.is_range && !pattern.upper)) {
                lower_internal(context, lower_span_for_tree(context, current),
                               "switch case has no pattern");
                core_list_free(stack);
                core_list_free(item.patterns);
                return false;
            }
            core_list_push(item.patterns, pattern);
        }
        core_list_free(stack);
        core_list_push(frame->cases, item);
        return true;
    }
    if (nt_is(grammar, node, CORE_STRING_STATIC("case-else"))
        || nt_is(grammar, node, CORE_STRING_STATIC("switch-value-else"))) {
        frame->else_body = child_nt(grammar, node,
            nt_is(grammar, node, CORE_STRING_STATIC("switch-value-else"))
                ? CORE_STRING_STATIC("value-body") : CORE_STRING_STATIC("body"));
        return frame->else_body != nullptr;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!n00bb_switch_collect(grammar, core_tree_child(node, i), frame,
                                  context)) return false;
    return true;
}

static bool
n00bb_switch_branch(ir_lower_context_t *context, ir_value_t *condition,
                    ir_block_t *yes, ir_block_t *no, ir_span_t span)
{
    ir_instr_t *branch = condition
        ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
    if (!branch || !ir_instr_operand_value(branch, condition)
        || !ir_instr_operand_block(branch, yes)
        || !ir_instr_operand_block(branch, no)) return false;
    branch->span = span;
    return true;
}

static bool
n00bb_mark_case_test(n00bb_lower_context_t *lower, ir_value_t *test,
                     const n00bb_switch_frame_t *frame, size_t case_index,
                     size_t patterns)
{
    ir_annots_t *annots = test && test->def ? &test->def->annots : nullptr;
    return annots
        && ir_annot_set(lower->ir_context, annots, (ir_annot_t){
               .key = N00BB_SWITCH_KEY, .kind = IR_ANNOT_INT,
               .i = {.mag = frame->order}})
        && ir_annot_set(lower->ir_context, annots, (ir_annot_t){
               .key = N00BB_CASE_INDEX_KEY, .kind = IR_ANNOT_INT,
               .i = {.mag = case_index}})
        && ir_annot_set(lower->ir_context, annots, (ir_annot_t){
               .key = N00BB_CASE_PATTERNS_KEY, .kind = IR_ANNOT_INT,
               .i = {.mag = patterns}});
}

static bool
n00bb_switch_test(ir_lower_context_t *context, n00bb_lower_context_t *lower,
                  const n00bb_switch_frame_t *frame, size_t case_index,
                  const n00bb_switch_case_t *item,
                  const n00bb_switch_pattern_t *pattern,
                  ir_value_t *scrutinee, ir_block_t *next)
{
    ir_span_t span = lower_span_for_tree(context, pattern->node);
    th_type *truth = n00bb_ir_builtin_type(context->module,
                                           CORE_STRING_STATIC("bool"));
    if (pattern->is_range) {
        ir_value_t *low = ir_lower_node_value(context, pattern->node);
        ir_value_t *high = ir_lower_node_value(context, pattern->upper);
        ir_value_t *below = low && high
            ? n00bb_comparison_is(context, scrutinee, low,
                                  CORE_STRING_STATIC("less"), span)
            : nullptr;
        ir_block_t *upper = below
            ? n00bb_branch_block(context, lower, "switch_upper") : nullptr;
        if (!upper || !n00bb_mark_case_test(lower,
                ir_operand_value(&below->def->operands[0]), frame,
                case_index, item->patterns.len)
            || !n00bb_switch_branch(context, below, next, upper, span))
            return false;
        context->block = upper;
        ir_value_t *beyond = n00bb_comparison_is(context, scrutinee, high,
            pattern->inclusive ? CORE_STRING_STATIC("greater")
                               : CORE_STRING_STATIC("less"), span);
        if (!beyond || !n00bb_mark_case_test(lower,
                ir_operand_value(&beyond->def->operands[0]), frame,
                case_index, item->patterns.len))
            return false;
        return pattern->inclusive
            ? n00bb_switch_branch(context, beyond, next, item->block, span)
            : n00bb_switch_branch(context, beyond, item->block, next, span);
    }
    ir_value_t *test = nullptr;
    if (pattern->name.data) {
        test = n00bb_dispatch(context, N00BB_CASE_METHOD, &scrutinee, 1,
                              truth, span);
        if (!test || !ir_annot_set(lower->ir_context, &test->def->annots,
                (ir_annot_t){.key = N00BB_CASE_KEY, .kind = IR_ANNOT_STR,
                             .s = ir_strdup(lower->ir_context, pattern->name)})
            || !ir_annot_set(lower->ir_context, &test->def->annots,
                (ir_annot_t){.key = N00BB_SWITCH_VARIABLE_KEY,
                             .kind = IR_ANNOT_BOOL,
                             .b = frame->subject_variable}))
            return false;
    }
    else {
        ir_value_t *value = ir_lower_node_value(context, pattern->node);
        ir_value_t *operands[] = {scrutinee, value};
        core_string_t text = first_token_text(pattern->node);
        parse_token_info_t *token = parse_tree_find_first_terminal(
            pattern->node);
        core_string_t terminal = token
            ? parse_terminal_name(lower->grammar, parse_token_tid(token))
            : (core_string_t){0};
        ir_lit_t integer = {0};
        core_string_t kind =
            core_string_eq(terminal, CORE_STRING_STATIC("STRING_LIT"))
                ? CORE_STRING_STATIC("`string`")
            : core_string_eq(terminal, CORE_STRING_STATIC("FLOAT_LIT"))
                ? CORE_STRING_STATIC("`f64`")
            :
            core_string_eq(text, CORE_STRING_STATIC("true"))
                || core_string_eq(text, CORE_STRING_STATIC("false"))
                ? CORE_STRING_STATIC("a boolean")
            : n00bb_switch_case_is_char(lower->grammar, pattern->node)
                ? CORE_STRING_STATIC("a character")
            : n00bb_switch_case_integer(lower->grammar, pattern->node,
                                        &integer)
                ? CORE_STRING_STATIC("an integer")
                : (core_string_t){0};
        test = value ? n00bb_dispatch(context, CORE_STRING_STATIC("__equal"),
                                      operands, 2, truth, span)
                     : nullptr;
        if (test && kind.data
            && !ir_annot_set(lower->ir_context, &test->def->annots,
                   (ir_annot_t){.key = N00BB_CASE_KIND_KEY,
                                .kind = IR_ANNOT_STR, .s = kind}))
            return false;
    }
    return n00bb_mark_case_test(lower, test, frame, case_index,
                                item->patterns.len)
        && n00bb_switch_branch(context, test, item->block, next, span);
}

static bool
n00bb_switch_unmatched(ir_lower_context_t *context,
                       n00bb_lower_context_t *lower,
                       const n00bb_switch_frame_t *frame,
                       parse_tree_t *subject)
{
    ir_instr_t *jump = ir_instr_add(frame->default_block, IR_OP_JUMP);
    if (!jump || !ir_instr_operand_block(jump, frame->join)) return false;
    jump->span = lower_span_for_tree(context, subject);
    return ir_annot_set(lower->ir_context, &jump->annots, (ir_annot_t){
               .key = N00BB_SWITCH_KEY, .kind = IR_ANNOT_INT,
               .i = {.mag = frame->order}})
        && ir_annot_set(lower->ir_context, &jump->annots, (ir_annot_t){
               .key = N00BB_SWITCH_EXPRESSION_KEY, .kind = IR_ANNOT_BOOL,
               .b = frame->expression})
        && ir_annot_set(lower->ir_context, &jump->annots, (ir_annot_t){
               .key = N00BB_SWITCH_SUBJECT_KEY, .kind = IR_ANNOT_STR,
               .s = ir_strdup(lower->ir_context,
                              source_text_of(lower->unit, subject))});
}

static void
n00bb_free_switch_cases(n00bb_switch_frame_t *frame)
{
    for (size_t i = 0; i < frame->cases.len; i++)
        core_list_free(frame->cases.data[i].patterns);
    core_list_free(frame->cases);
}

bool
n00bb_lower_switch_annotation(ir_lower_context_t *context,
                              parse_visit_t *visit, bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (!lower) return false;
    if (nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("typecase-stmt"))
        || nt_is(visit->grammar, visit->node,
                 CORE_STRING_STATIC("typecase-value-expr")))
        return n00bb_lower_typecase_annotation(context, visit, entering);
    if (!entering) {
        if (!lower->switches.len
            || lower->switches.data[lower->switches.len - 1].node
                != visit->node) return true;
        n00bb_switch_frame_t frame =
            lower->switches.data[--lower->switches.len];
        n00bb_free_switch_cases(&frame);
        if (frame.active) {
            if (!frame.join_reachable) {
                ir_instr_t *unreachable =
                    ir_instr_add(frame.join, IR_OP_UNREACHABLE);
                if (!unreachable) {
                    lower_internal(context,
                        lower_span_for_tree(context, frame.node),
                        "could not terminate unreachable switch join");
                    return false;
                }
                unreachable->span = lower_span_for_tree(context, frame.node);
            }
            if (frame.expression && frame.join_reachable) {
                ir_instr_t *read = ir_instr_add(frame.join, IR_OP_COPY);
                if (!read || !ir_lower_set_temp_dest(context, read,
                        CORE_STRING_STATIC("__n00bb_switch_value_"),
                        frame.result_local->type)
                    || !ir_instr_operand_local(read, frame.result_local)
                    || !ir_lower_set_node_value(context, frame.node,
                                                read->dest)) {
                    lower_internal(context,
                        lower_span_for_tree(context, frame.node),
                        "could not read the value from the switch-expression join");
                    return false;
                }
                read->span = lower_span_for_tree(context, frame.node);
            }
            context->block = frame.join;
            context->reachable = frame.join_reachable;
        }
        return true;
    }
    bool expression = nt_is(visit->grammar, visit->node,
                            CORE_STRING_STATIC("switch-value-expr"));
    if (!expression && !nt_is(visit->grammar, visit->node,
                              CORE_STRING_STATIC("switch-stmt"))) {
        lower_internal(context, lower_span_for_tree(context, visit->node),
            "switch lowering reached a node that is not a switch");
        return false;
    }
    parse_cf_label_t *label = parse_cf_label_lookup(
        tu_state(lower->unit)->annotations, visit->node);
    if (!label || label->kind != PARSE_CF_SWITCH || !label->cond
        || !label->then_body) return false;
    parse_tree_t *subject_lvalue = n00bb_simple_lvalue(label->cond);
    parse_sym_entry_t *subject = subject_lvalue
        ? attached_symbol(visit->grammar, lower->unit, subject_lvalue)
        : nullptr;
    n00bb_switch_frame_t frame = {
        .node = visit->node,
        .label = n00bb_control_label(visit->grammar, visit->node,
                                    CORE_STRING_STATIC("labeled-switch")),
        .scrutinee = label->cond,
        .cases = core_list_new(n00bb_switch_case_t),
        .order = ++lower->next_control_order,
        .expression = expression,
        .subject_variable = subject && (subject->kind == PARSE_SYM_VARIABLE
                                        || subject->kind == PARSE_SYM_PARAM),
        .active = context->reachable,
    };
    if (!frame.active) {
        core_list_push(lower->switches, frame);
        return true;
    }
    frame.join = n00bb_branch_block(context, lower, "switch_join");
    if (expression) {
        th_type *result_type = n00bb_node_result_type(context, visit->node);
        char local_buffer[48];
        core_string_t local_name = n00bb_format_name(local_buffer,
            sizeof(local_buffer), "__n00bb_switch_result_%llu",
            (unsigned long long)context->next_value_id++);
        frame.result_local = result_type
            ? ir_local_add(context->function, local_name, result_type)
            : nullptr;
        if (!frame.result_local) {
            lower_internal(context, lower_span_for_tree(context, visit->node),
                "switch expression has no result");
            n00bb_free_switch_cases(&frame);
            return false;
        }
    }
    if (!frame.join || !n00bb_switch_collect(visit->grammar, label->then_body,
                                             &frame, context)) {
        n00bb_free_switch_cases(&frame);
        return false;
    }
    frame.default_block = n00bb_branch_block_named(context, lower,
        frame.else_body ? CORE_STRING_STATIC("switch_else")
                        : CORE_STRING_STATIC("switch_unmatched"));
    frame.join_reachable = frame.else_body == nullptr;
    for (size_t c = 0; c < frame.cases.len; c++)
        frame.total += frame.cases.data[c].patterns.len;
    if (!frame.default_block
        || (!frame.else_body
            && !n00bb_switch_unmatched(context, lower, &frame, label->cond))) {
        lower_internal(context, lower_span_for_tree(context, visit->node),
                       "could not emit the unmatched switch path");
        n00bb_free_switch_cases(&frame);
        return false;
    }
    core_list_push(lower->switches, frame);
    return true;
}

bool
n00bb_lower_variant_payload(ir_lower_context_t *context, parse_visit_t *visit,
                            bool entering, void *state)
{
    (void)state;
    if (entering || !n00bb_postfix_node(visit->grammar, visit->node)
        || core_tree_num_children(visit->node) < 3
        || !parse_tree_leaf_is(core_tree_child(visit->node, 1),
                               CORE_STRING_STATIC("."))) return true;
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    if (!lower) return false;
    parse_sym_entry_t *arm = attached_symbol(visit->grammar, lower->unit,
                                             visit->node);
    if (!n00bb_variant_arm_symbol(visit->grammar, arm)
        || n00bb_assignment_target(visit->grammar, visit->node, nullptr)
        || ir_lower_node_value(context, visit->node)) return true;
    parse_tree_t *receiver_node = core_tree_child(visit->node, 0);
    ir_value_t *value = ir_lower_node_value(context, receiver_node);
    parse_sym_entry_t *receiver = value ? nullptr
        : attached_symbol(visit->grammar, lower->unit, receiver_node);
    if (!value && receiver) value = ir_lower_bound_value(context, receiver);
    if (!value) return true;
    ir_span_t span = lower_span_for_tree(context, visit->node);
    ir_instr_t *extract = ir_lower_emit_instruction(context, IR_OP_EXTRACT,
                                                    span);
    if (!extract || !ir_lower_set_temp_dest(context, extract,
            CORE_STRING_STATIC("__n00bb_payload_"),
            n00bb_node_result_type(context, visit->node))
        || !ir_instr_operand_value(extract, value)
        || !ir_instr_operand_name(extract, arm->name)
        || !ir_lower_set_node_value(context, visit->node, extract->dest)) {
        lower_internal(context, span, "could not lower variant payload access");
        return false;
    }
    n00bb_rewrite_later(context, extract);
    return true;
}

static n00bb_switch_frame_t *
n00bb_open_switch(n00bb_lower_context_t *lower)
{
    if (!lower || !lower->switches.len) return nullptr;
    n00bb_switch_frame_t *frame =
        &lower->switches.data[lower->switches.len - 1];
    return frame->active ? frame : nullptr;
}

static bool
n00bb_start_switch_tests(ir_lower_context_t *context,
                         n00bb_switch_frame_t *frame)
{
    if (frame->test_block) return true;
    frame->subject = ir_lower_node_value(context, frame->scrutinee);
    if (frame->subject && !frame->subject->def) {
        ir_instr_t *copy = ir_lower_emit_instruction(context, IR_OP_COPY,
            lower_span_for_tree(context, frame->scrutinee));
        frame->subject = copy && ir_lower_set_temp_dest(context, copy,
                CORE_STRING_STATIC("__n00bb_switch_subject_"),
                frame->subject->type)
            && ir_instr_operand_value(copy, frame->subject)
            ? copy->dest : nullptr;
    }
    n00bb_lower_context_t *lower = context->user;
    if (frame->subject && frame->subject->def && lower
        && !ir_annot_set(lower->ir_context, &frame->subject->def->annots,
               (ir_annot_t){.key = N00BB_SWITCH_COPY_KEY,
                            .kind = IR_ANNOT_BOOL, .b = true}))
        return false;
    frame->test_block = context->block;
    return frame->subject != nullptr;
}

bool
n00bb_enter_case_patterns(ir_lower_context_t *context, parse_visit_t *visit,
                          bool entering, void *opaque)
{
    (void)opaque;
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    n00bb_switch_frame_t *frame = n00bb_open_switch(lower);
    bool owned = false;
    for (size_t c = 0; frame && c < frame->cases.len; c++)
        owned |= frame->cases.data[c].list == visit->node;
    if (!entering || !owned) return true;
    if (!n00bb_start_switch_tests(context, frame)) {
        lower_internal(context, lower_span_for_tree(context, frame->node),
                       "switch has no subject value");
        return false;
    }
    context->block = frame->test_block;
    context->reachable = true;
    return true;
}

enum { FLAGSET_TABLE_BITS = 8, FLAGSET_ANALYSIS_BITS = 16 };

typedef struct {
    size_t case_index;
    const n00bb_switch_pattern_t *pattern;
    bool live;
} n00bb_flag_test_t;

static bool
n00bb_case_flags_mask(ir_lower_context_t *context, parse_grammar_t *grammar,
                      parse_sym_entry_t *owner, parse_tree_t *node,
                      ir_uint_t *mask)
{
    if (!node || core_tree_is_leaf(node)) return false;
    if (nt_is(grammar, node, CORE_STRING_STATIC("nil-keyword"))) {
        *mask = 0;
        return true;
    }
    if (nt_is(grammar, node, CORE_STRING_STATIC("case-flag-name"))) {
        core_string_t name = first_token_text(node);
        parse_sym_entry_t *member = owner->member_scope
            ? parse_symtab_lookup_in(owner->member_scope, name) : nullptr;
        n00bb_flagset_value_t *value = n00bb_flagset_member(member)
            && member->has_comptime_value ? member->comptime_value : nullptr;
        if (!value) {
            lower_report(context, N00B_DIAG_FLAGSET_CASE_UNKNOWN_FLAG,
                lower_span_for_tree(context, node),
                N00B_TEXT_FLAGSET_CASE_NOT_A_FLAG,
                N00BB_ARGS(ARG_TEXT(name), ARG_TEXT(owner->name)));
            return false;
        }
        *mask = value->mask;
        return true;
    }
    size_t count = core_tree_num_children(node);
    if (count == 1)
        return n00bb_case_flags_mask(context, grammar, owner,
                                     core_tree_child(node, 0), mask);
    if (count == 2) {
        ir_uint_t inner = 0;
        if (!n00bb_case_flags_mask(context, grammar, owner,
                                   core_tree_child(node, 1), &inner))
            return false;
        *mask = ~inner;
        return true;
    }
    if (count != 3) return false;
    if (core_tree_is_leaf(core_tree_child(node, 0)))
        return n00bb_case_flags_mask(context, grammar, owner,
                                     core_tree_child(node, 1), mask);
    ir_uint_t left = 0, right = 0;
    bool left_ok = n00bb_case_flags_mask(context, grammar, owner,
                                         core_tree_child(node, 0), &left);
    bool right_ok = n00bb_case_flags_mask(context, grammar, owner,
                                          core_tree_child(node, 2), &right);
    if (!left_ok || !right_ok) return false;
    core_string_t op = first_token_text(core_tree_child(node, 1));
    if (core_string_eq(op, CORE_STRING_STATIC("+"))
        || core_string_eq(op, CORE_STRING_STATIC("|"))) *mask = left | right;
    else if (core_string_eq(op, CORE_STRING_STATIC("-")))
        *mask = left & ~right;
    else if (core_string_eq(op, CORE_STRING_STATIC("^"))) *mask = left ^ right;
    else if (core_string_eq(op, CORE_STRING_STATIC("&"))) *mask = left & right;
    else return false;
    return true;
}

static size_t
n00bb_flag_winner(const n00bb_flag_test_t *tests, size_t count,
                  ir_uint_t value, bool outside)
{
    for (size_t t = 0; t < count; t++) {
        ir_uint_t mask = tests[t].pattern->mask;
        bool hit = tests[t].pattern->exact ? !outside && value == mask
                                           : (value & mask) == mask;
        if (hit) return t;
    }
    return count;
}

static bool
n00bb_flag_covered(const n00bb_flag_test_t *tests, size_t at)
{
    const n00bb_switch_pattern_t *later = tests[at].pattern;
    for (size_t t = 0; t < at; t++) {
        const n00bb_switch_pattern_t *earlier = tests[t].pattern;
        if (earlier->exact ? later->exact && earlier->mask == later->mask
                           : (earlier->mask & later->mask) == earlier->mask)
            return true;
    }
    return false;
}

static ir_value_t *
n00bb_flag_constant(ir_lower_context_t *context, th_type *type,
                    ir_uint_t value, ir_span_t span)
{
    ir_instr_t *constant = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                     span);
    return constant && ir_lower_set_temp_dest(context, constant,
            CORE_STRING_STATIC("__n00bb_flags_"), type)
        && ir_instr_operand_uint(constant, value) ? constant->dest : nullptr;
}

static ir_value_t *
n00bb_flag_masked(ir_lower_context_t *context, th_type *type,
                  ir_value_t *bits, ir_uint_t mask, ir_span_t span)
{
    ir_value_t *constant = n00bb_flag_constant(context, type, mask, span);
    return constant ? n00bb_emit_binary_ir(context, IR_OP_AND, type, bits,
                                           constant, span) : nullptr;
}

static bool
n00bb_flag_table(ir_lower_context_t *context, n00bb_switch_frame_t *frame,
                 const n00bb_flag_test_t *tests, size_t count,
                 ir_value_t *index, ir_uint_t relevant, bool outside,
                 ir_span_t span)
{
    ir_instr_t *table = ir_instr_add(context->block, IR_OP_SWITCH);
    if (!table || !ir_instr_operand_value(table, index)
        || !ir_instr_operand_default(table, frame->default_block))
        return false;
    table->span = span;
    ir_uint_t value = 0;
    do {
        size_t winner = n00bb_flag_winner(tests, count, value, outside);
        if (winner < count && !ir_instr_operand_switch_arm(table,
                (ir_int_t)value,
                frame->cases.data[tests[winner].case_index].block))
            return false;
        value = (value - relevant) & relevant;
    } while (value);
    return true;
}

static ir_uint_t
n00bb_flag_subject_mask(const ir_value_t *subject)
{
    const ir_instr_t *def = subject ? subject->def : nullptr;
    while (def && def->op == IR_OP_COPY && def->n_operands
           && ir_operand_value(&def->operands[0]))
        def = ir_operand_value(&def->operands[0])->def;
    for (uint32_t o = 0; def && def->op == IR_OP_BITSET_INTERSECTION
                         && o < def->n_operands; o++) {
        const ir_value_t *operand = ir_operand_value(&def->operands[o]);
        const ir_instr_t *flag = operand ? operand->def : nullptr;
        if (flag && flag->op == IR_OP_BITSET_CONST && flag->n_operands == 2
            && flag->operands[1].kind == IR_OPD_CONST)
            return flag->operands[1].lit.i.mag;
    }
    return ~(ir_uint_t)0;
}

static bool
n00bb_flag_dispatch(ir_lower_context_t *context, n00bb_switch_frame_t *frame,
                    const n00bb_flag_test_t *tests, size_t count,
                    const n00bb_flagset_info_t *info, ir_uint_t possible,
                    ir_uint_t relevant, unsigned relevant_bits, ir_span_t span)
{
    unsigned width = info->atomic_count <= 8 ? 8
        : info->atomic_count <= 16 ? 16 : info->atomic_count <= 32 ? 32
        : info->atomic_count <= 64 ? 64 : 128;
    char type_name[8];
    core_string_t name = n00bb_format_name(type_name, sizeof(type_name),
                                           "u%u", width);
    th_type *bits_type = n00bb_ir_builtin_type(context->module, name);
    th_type *truth = n00bb_ir_builtin_type(context->module,
                                           CORE_STRING_STATIC("bool"));
    ir_instr_t *read = bits_type && truth
        ? ir_lower_emit_instruction(context, IR_OP_BITSET_VALUE, span)
        : nullptr;
    if (!read || !ir_lower_set_temp_dest(context, read,
            CORE_STRING_STATIC("__n00bb_flags_"), bits_type)
        || !ir_instr_operand_value(read, frame->subject)) return false;
    ir_value_t *bits = read->dest;
    if (relevant_bits <= FLAGSET_TABLE_BITS) {
        bool exact = false;
        for (size_t t = 0; t < count; t++) exact |= tests[t].pattern->exact;
        ir_uint_t others = possible & ~relevant;
        ir_value_t *index = n00bb_flag_masked(context, bits_type, bits,
                                              relevant, span);
        if (!index) return false;
        if (!exact || !others)
            return n00bb_flag_table(context, frame, tests, count, index,
                                    relevant, false, span);
        ir_value_t *rest = n00bb_flag_masked(context, bits_type, bits,
                                             others, span);
        ir_value_t *zero = rest
            ? n00bb_flag_constant(context, bits_type, 0, span) : nullptr;
        ir_value_t *extra = zero ? n00bb_emit_binary_ir(context, IR_OP_NE,
            truth, rest, zero, span) : nullptr;
        ir_block_t *with = extra
            ? n00bb_branch_block(context, context->user, "switch_flags_more")
            : nullptr;
        ir_block_t *without = with
            ? n00bb_branch_block(context, context->user, "switch_flags_only")
            : nullptr;
        if (!without
            || !n00bb_switch_branch(context, extra, with, without, span))
            return false;
        context->block = with;
        if (!n00bb_flag_table(context, frame, tests, count, index, relevant,
                              true, span)) return false;
        context->block = without;
        return n00bb_flag_table(context, frame, tests, count, index,
                                relevant, false, span);
    }
    for (size_t t = 0; t < count; t++) {
        ir_uint_t mask = tests[t].pattern->mask;
        ir_value_t *tested = tests[t].pattern->exact ? bits
            : n00bb_flag_masked(context, bits_type, bits, mask, span);
        ir_value_t *wanted = tested
            ? n00bb_flag_constant(context, bits_type, mask, span) : nullptr;
        ir_value_t *hit = wanted ? n00bb_emit_binary_ir(context, IR_OP_EQ,
            truth, tested, wanted, span) : nullptr;
        ir_block_t *next = t + 1 < count
            ? n00bb_branch_block(context, context->user, "switch_test")
            : frame->default_block;
        if (!hit || !next || !n00bb_switch_branch(context, hit,
                frame->cases.data[tests[t].case_index].block, next, span))
            return false;
        context->block = next;
    }
    return true;
}

static bool
n00bb_flagset_never_unmatched(n00bb_switch_frame_t *frame)
{
    ir_instr_t *jump = frame->else_body
        ? nullptr : ir_block_terminator(frame->default_block);
    if (!jump) return true;
    ir_span_t span = jump->span;
    ir_block_t *block = jump->block;
    ir_instr_t *never = ir_instr_erase(jump)
        ? ir_instr_add(block, IR_OP_UNREACHABLE) : nullptr;
    if (never) never->span = span;
    return never != nullptr;
}

static bool
n00bb_flagset_switch(ir_lower_context_t *context,
                     n00bb_lower_context_t *lower,
                     n00bb_switch_frame_t *frame, parse_sym_entry_t *owner)
{
    ir_span_t span = lower_span_for_tree(context, frame->node);
    const n00bb_flagset_info_t *info = owner ? parse_sym_binding(
        canonical_symbol(owner), CORE_STRING_STATIC("n00bb.flagset-info"))
        : nullptr;
    n00bb_flag_test_t *tests = core_alloc_array(n00bb_flag_test_t,
                                                frame->total ? frame->total
                                                             : 1);
    size_t count = 0;
    bool valid = owner && info;
    for (size_t c = 0; c < frame->cases.len; c++) {
        n00bb_switch_case_t *item = &frame->cases.data[c];
        for (size_t p = 0; p < item->patterns.len; p++) {
            n00bb_switch_pattern_t *pattern = &item->patterns.data[p];
            ir_span_t at = lower_span_for_tree(context, pattern->node);
            if (!owner) {
                if (!pattern->flags) continue;
                core_string_t subject = lower_type_text(context,
                                                        frame->subject->type);
                lower_report(context, N00B_DIAG_FLAGSET_CASE_NOT_FLAGSET, at,
                    N00B_TEXT_FLAGSET_CASE_ON_NON_FLAGSET,
                    N00BB_ARGS(ARG_TEXT(pattern->exact
                                   ? CORE_STRING_STATIC("is")
                                   : CORE_STRING_STATIC("has")),
                               ARG_TEXT(subject)));
                core_free(subject.data);
                continue;
            }
            if (!pattern->flags) {
                lower_report(context, N00B_DIAG_FLAGSET_CASE_NEEDS_TEST, at,
                    N00B_TEXT_FLAGSET_CASE_NEEDS_HAS_OR_IS,
                    N00BB_ARGS(ARG_TEXT(owner->name)));
                lower_help(context, N00B_TEXT_FLAGSET_CASE_HAS_OR_IS_HELP,
                           N00BB_NO_ARGS);
                valid = false;
                continue;
            }
            if (!info || !n00bb_case_flags_mask(context, lower->grammar,
                    owner, pattern->flags, &pattern->mask)) {
                valid = false;
                continue;
            }
            pattern->mask &= info->all_mask;
            if (!pattern->exact && !pattern->mask) {
                lower_report(context, N00B_DIAG_FLAGSET_CASE_HAS_NOTHING, at,
                    N00B_TEXT_FLAGSET_CASE_HAS_NO_FLAGS, N00BB_NO_ARGS);
                lower_help(context, N00B_TEXT_FLAGSET_CASE_HAS_NOTHING_HELP,
                           N00BB_NO_ARGS);
                valid = false;
                continue;
            }
            tests[count++] = (n00bb_flag_test_t){.case_index = c,
                                                 .pattern = pattern};
        }
    }
    bool ok = true;
    if (valid) {
        ir_uint_t possible = info->all_mask
            & n00bb_flag_subject_mask(frame->subject);
        ir_uint_t relevant = 0;
        unsigned relevant_bits = 0;
        for (size_t t = 0; t < count; t++) relevant |= tests[t].pattern->mask;
        relevant &= possible;
        for (ir_uint_t bits = relevant; bits; bits &= bits - 1)
            relevant_bits++;
        bool others = (possible & ~relevant) != 0;
        bool exhaustive = relevant_bits <= FLAGSET_ANALYSIS_BITS;
        if (relevant_bits <= FLAGSET_ANALYSIS_BITS) {
            for (int outside = 0; outside <= (others ? 1 : 0); outside++) {
                ir_uint_t value = 0;
                do {
                    size_t winner = n00bb_flag_winner(tests, count, value,
                                                      outside);
                    if (winner < count) tests[winner].live = true;
                    else exhaustive = false;
                    value = (value - relevant) & relevant;
                } while (value);
            }
        }
        else {
            for (size_t t = 0; t < count; t++)
                tests[t].live = !(tests[t].pattern->mask & ~possible)
                    && !n00bb_flag_covered(tests, t);
        }
        for (size_t t = 0; t < count; t++) {
            if (tests[t].live) continue;
            lower_report(context, N00B_DIAG_FLAGSET_CASE_UNREACHABLE,
                lower_span_for_tree(context, tests[t].pattern->node),
                N00B_TEXT_FLAGSET_CASE_NEVER_MATCHES, N00BB_NO_ARGS);
            lower_help(context, N00B_TEXT_FLAGSET_CASE_REORDER_HELP,
                       N00BB_NO_ARGS);
        }
        ok = n00bb_flag_dispatch(context, frame, tests, count, info,
                                 possible, relevant, relevant_bits, span)
            && (!exhaustive || n00bb_flagset_never_unmatched(frame));
    }
    else {
        ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
        ok = jump && ir_instr_operand_block(jump, frame->default_block);
    }
    core_free(tests);
    frame->flags_dispatched = true;
    frame->tested = frame->total;
    frame->test_block = frame->default_block;
    context->block = frame->default_block;
    if (!ok)
        lower_internal(context, span, "could not emit the flagset switch");
    return ok;
}

static ir_value_t *
n00bb_switch_flag_slice(ir_lower_context_t *context,
                        n00bb_lower_context_t *lower, ir_value_t *subject)
{
    const ir_instr_t *def = subject ? subject->def : nullptr;
    while (def && def->op == IR_OP_COPY && def->n_operands
           && ir_operand_value(&def->operands[0]))
        def = ir_operand_value(&def->operands[0])->def;
    ir_value_t *base = def && def->op == IR_OP_EXTRACT && def->n_operands == 2
        && def->operands[1].kind == IR_OPD_NAME
        ? ir_operand_value(&def->operands[0]) : nullptr;
    parse_sym_entry_t *owner = nullptr;
    if (!base || !n00bb_flagset_type(lower->unit, tu_state(lower->unit),
                                     base->type, &owner)
        || !owner->member_scope) return nullptr;
    parse_sym_entry_t *flag = parse_symtab_lookup_in(owner->member_scope,
                                                     def->operands[1].name);
    const n00bb_flagset_value_t *value = n00bb_flagset_member(flag)
        && flag->has_comptime_value ? flag->comptime_value : nullptr;
    if (!value) return nullptr;
    ir_instr_t *mask = ir_lower_emit_instruction(context, IR_OP_BITSET_CONST,
                                                 def->span);
    ir_instr_t *cut = mask && ir_lower_set_temp_dest(context, mask,
            CORE_STRING_STATIC("__n00bb_"), base->type)
        && ir_instr_operand_name(mask, flag->name)
        && ir_instr_operand_uint(mask, value->mask)
        ? ir_lower_emit_instruction(context, IR_OP_BITSET_INTERSECTION,
                                    def->span)
        : nullptr;
    return cut && ir_lower_set_temp_dest(context, cut,
            CORE_STRING_STATIC("__n00bb_"), base->type)
        && ir_instr_operand_value(cut, base)
        && ir_instr_operand_value(cut, mask->dest) ? cut->dest : nullptr;
}

static bool
n00bb_switch_uses_flags(const n00bb_switch_frame_t *frame)
{
    for (size_t c = 0; c < frame->cases.len; c++)
        for (size_t p = 0; p < frame->cases.data[c].patterns.len; p++)
            if (frame->cases.data[c].patterns.data[p].flags) return true;
    return false;
}

static bool
n00bb_emit_case_tests(ir_lower_context_t *context,
                      n00bb_lower_context_t *lower,
                      n00bb_switch_frame_t *frame, size_t case_index)
{
    if (!n00bb_start_switch_tests(context, frame)) {
        lower_internal(context, lower_span_for_tree(context, frame->node),
                       "switch has no subject value");
        return false;
    }
    context->block = frame->test_block;
    if (frame->flags_dispatched) return true;
    parse_sym_entry_t *owner = nullptr;
    ir_value_t *slice = n00bb_switch_flag_slice(context, lower,
                                                frame->subject);
    if (slice) frame->subject = slice;
    if (n00bb_flagset_type(lower->unit, tu_state(lower->unit),
                           frame->subject->type, &owner))
        return n00bb_flagset_switch(context, lower, frame, owner);
    if (n00bb_switch_uses_flags(frame))
        return n00bb_flagset_switch(context, lower, frame, nullptr);
    bool ok = true;
    if (case_index >= frame->cases.len) {
        if (!frame->total && frame->test_block != frame->default_block) {
            ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
            ok = jump && ir_instr_operand_block(jump, frame->default_block);
            frame->test_block = frame->default_block;
        }
        if (!ok)
            lower_internal(context, lower_span_for_tree(context, frame->node),
                           "could not reach the switch default");
        return ok;
    }
    n00bb_switch_case_t *item = &frame->cases.data[case_index];
    for (size_t p = 0; ok && p < item->patterns.len; p++) {
        ir_block_t *next = ++frame->tested < frame->total
            ? n00bb_branch_block(context, lower, "switch_test")
            : frame->default_block;
        ok = next && n00bb_switch_test(context, lower, frame, case_index,
                                       item, &item->patterns.data[p],
                                       frame->subject, next);
        context->block = next;
    }
    frame->test_block = context->block;
    if (!ok)
        lower_internal(context, lower_span_for_tree(context, frame->node),
                       "could not emit switch tests");
    return ok;
}
