#include "lower_internal.h"

static ir_func_t *
function_at(ir_module_t *module, core_diag_location_t at);
static bool
n00bb_record_assert_site(ir_lower_context_t *context,
                         n00bb_lower_context_t *lower, ir_span_t span);

typedef struct {
    size_t loops, requires, value_blocks, tries, withs, finals, branches;
    size_t switches, typecases, pending;
} frame_marks_t;

static frame_marks_t
frame_marks(const n00bb_lower_context_t *lower)
{
    return (frame_marks_t){
        lower->loops.len, lower->requires.len, lower->value_blocks.len,
        lower->tries.len, lower->withs.len, lower->finals.len,
        lower->branches.len, lower->switches.len, lower->typecases.len,
        lower->pending_call_copyouts.len,
    };
}

static void
restore_frames(n00bb_lower_context_t *lower, frame_marks_t marks)
{
    lower->loops.len = marks.loops;
    lower->requires.len = marks.requires;
    lower->value_blocks.len = marks.value_blocks;
    lower->tries.len = marks.tries;
    lower->withs.len = marks.withs;
    lower->finals.len = marks.finals;
    lower->branches.len = marks.branches;
    lower->switches.len = marks.switches;
    lower->typecases.len = marks.typecases;
    lower->pending_call_copyouts.len = marks.pending;
}

static ir_block_t *
statement_block(ir_lower_context_t *context, size_t index, ir_instr_t **jump)
{
    char buffer[48];
    core_string_t name = n00bb_format_name(buffer, sizeof(buffer),
        "__n00bb_statement_%zu", index);
    ir_block_t *block = ir_block_add(context->function, name);
    ir_instr_t *entry = block && context->reachable
        ? ir_instr_add(context->block, IR_OP_JUMP) : nullptr;
    if (!entry || !ir_instr_operand_block(entry, block)) return nullptr;
    if (jump) *jump = entry;
    context->block = block;
    return block;
}

static void
forget_detached_sites(n00bb_tu_state_t *state)
{
    size_t kept = 0;
    for (size_t i = 0; i < state->rewrite_sites.len; i++)
        if (state->rewrite_sites.data[i]->block)
            state->rewrite_sites.data[kept++] = state->rewrite_sites.data[i];
    state->rewrite_sites.len = kept;
    kept = 0;
    for (size_t i = 0; i < state->condition_sites.len; i++)
        if (state->condition_sites.data[i]->block)
            state->condition_sites.data[kept++] =
                state->condition_sites.data[i];
    state->condition_sites.len = kept;
    kept = 0;
    for (size_t i = 0; i < state->write_sites.len; i++)
        if (!state->write_sites.data[i].store
            || state->write_sites.data[i].store->block)
            state->write_sites.data[kept++] = state->write_sites.data[i];
    state->write_sites.len = kept;
}

static bool
erase_block(ir_block_t *block)
{
    while (block->n_instrs)
        if (!ir_instr_erase(block->instrs[block->n_instrs - 1])) return false;
    return ir_block_erase(block);
}

static bool
tag_statement(ir_ctx_t *context, ir_func_t *function, ir_block_t *start,
              size_t index)
{
    for (uint32_t b = start->index; b < function->n_blocks; b++)
        if (!ir_annot_set(context, &function->blocks[b]->annots,
                (ir_annot_t){.key = N00BB_STATEMENT_KEY, .kind = IR_ANNOT_INT,
                             .i = {.mag = index}}))
            return false;
    return true;
}

static bool
in_statement(const ir_block_t *block, uint64_t index)
{
    const ir_annot_t *tag = block
        ? ir_annot_get((ir_annots_t *)&block->annots, N00BB_STATEMENT_KEY)
        : nullptr;
    return tag && tag->kind == IR_ANNOT_INT && tag->i.mag == index;
}

static bool
retarget(ir_instr_t *in, ir_block_t *from, ir_block_t *to)
{
    bool found = false;
    for (uint32_t o = 0; o < in->n_operands; o++)
        if (in->operands[o].kind == IR_OPD_BLOCK
            && in->operands[o].block == from) {
            in->operands[o].block = to;
            found = true;
        }
    return found;
}

bool
n00bb_cut_tagged_statement(n00bb_tu_state_t *state, ir_func_t *function,
                           uint64_t index)
{
    ir_block_t *start = nullptr, *exit = nullptr;
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        if (!in_statement(block, index)) continue;
        if (!start) start = block;
        ir_instr_t *end = ir_block_terminator(block);
        for (uint32_t s = 0; !exit && end && s < ir_succ_count(end); s++)
            if (!in_statement(ir_succ_at(end, s), index))
                exit = ir_succ_at(end, s);
    }
    if (!start) return false;
    char buffer[48];
    ir_block_t *resume = ir_block_add(function, n00bb_format_name(buffer,
        sizeof(buffer), "__n00bb_resume_%llu", (unsigned long long)index));
    ir_instr_t *tail = resume
        ? ir_instr_add(resume, exit ? IR_OP_JUMP : IR_OP_UNREACHABLE)
        : nullptr;
    if (!tail || (exit && !ir_instr_operand_block(tail, exit))) return false;
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        ir_instr_t *end = in_statement(block, index) ? nullptr
                                                      : ir_block_terminator(block);
        if (end) (void)retarget(end, start, resume);
    }
    for (uint32_t b = function->n_blocks; b-- > 0;)
        if (in_statement(function->blocks[b], index)
            && !erase_block(function->blocks[b]))
            return false;
    forget_detached_sites(state);
    return true;
}

static bool
cut_statement(ir_lower_context_t *context, ir_block_t *start,
              ir_instr_t *jump, size_t index)
{
    ir_func_t *function = context->function;
    for (uint32_t b = function->n_blocks; b-- > start->index;)
        if (!erase_block(function->blocks[b])) return false;
    char buffer[48];
    ir_block_t *resume = ir_block_add(function, n00bb_format_name(buffer,
        sizeof(buffer), "__n00bb_resume_%zu", index));
    if (!resume || !jump->n_operands) return false;
    jump->operands[0].block = resume;
    context->block = resume;
    context->reachable = true;
    n00bb_lower_context_t *lower = context->user;
    forget_detached_sites(tu_state(lower->unit));
    return true;
}

static bool
mark_declaration(ir_ctx_t *ir_context, ir_block_t *block)
{
    for (uint32_t i = block ? block->n_instrs : 0; i-- > 0;) {
        ir_instr_t *in = block->instrs[i];
        if (in->op == IR_OP_GLOBAL_STORE)
            return ir_annot_set(ir_context, &in->annots, (ir_annot_t){
                .key = N00BB_DECLARATION_KEY, .kind = IR_ANNOT_BOOL,
                .b = true});
    }
    return false;
}

bool
lower_ir_body(ir_lowerer_t *lowerer, parse_grammar_t *grammar,
              core_translation_unit_t *unit, parse_sym_entry_t *symbol,
              parse_tree_t *body, ir_func_t *function,
              core_list_t(parse_sym_entry_ptr_t) *parameters,
              core_list_t(n00bb_tree_ptr_t) *roots,
              parse_sym_entry_t *rest_parameter,
              bool expression_body,
              ir_ctx_t *ir_context,
              core_list_t(core_translation_unit_ptr_t) *units,
              n00bb_expression_result_hook_t expression_result_hook,
              void *expression_result_data)
{
    n00bb_tu_state_t *state = tu_state(unit);
    ir_module_t *module = unit_module(state);
    th_type *function_type = function ? th_resolve(function->type) : nullptr;
    if (!state || !module || !body || !function || !function_type)
        return false;

    ir_policy_t *policy = ir_policy_new(ir_context);
    if (!policy || !ir_policy_set(policy, CORE_STRING_STATIC("IR-W0039"),
                                  IR_POLICY_DENY))
        return false;
    ir_diags_t *diagnostics = attach_ir_diags(ir_context, policy, unit,
        "the rule that function bodies use every value they compute");
    if (!diagnostics) return false;
    if (parameters->len + (rest_parameter != nullptr) !=
        th_func_nparams(function_type)) {
        n00bb_internal_error(core_translation_unit_diagnostics(unit), unit,
            symbol ? link_source_location(symbol->decl_node)
                   : (core_diag_location_t){0},
            "generating code for a function",
            "parameter declarations do not match the callable type");
        return false;
    }

    ir_block_t *entry = ir_block_add(function, CORE_STRING_STATIC("entry"));
    if (!entry) {
        return false;
    }
    int32_t file_id = ir_file_count(module) - 1;
    n00bb_lower_context_t lower_data = {
        .grammar = grammar,
        .lowerer = lowerer,
        .bind_external = true,
        .track_writes = true,
        .ir_context = ir_context,
        .units = units,
        .unit = unit,
        .function_body = body,
        .function_symbol = symbol,
        .loops = core_list_new(n00bb_loop_frame_t),
        .requires = core_list_new(n00bb_require_frame_t),
        .value_blocks = core_list_new(n00bb_value_block_frame_t),
        .init_fields = core_list_new(n00bb_init_field_t),
        .tries = core_list_new(n00bb_try_frame_t),
        .withs = core_list_new(n00bb_with_frame_t),
        .finals = core_list_new(n00bb_final_frame_t),
        .parameters_to_copy_out = core_list_new(n00bb_copyout_t),
        .pending_call_copyouts = core_list_new(n00bb_pending_copyouts_t),
        .default_params = core_list_new(n00bb_default_param_t),
        .branches = core_list_new(n00bb_branch_frame_t),
        .switches = core_list_new(n00bb_switch_frame_t),
        .typecases = core_list_new(n00bb_typecase_frame_t),
    };
    ir_lower_context_t context = {
        .module = module,
        .function = function,
        .block = entry,
        .diagnostics = diagnostics,
        .file_id = file_id,
        .user = &lower_data,
        .reachable = true,
    };
    size_t failures = lower_failures(&context);
    size_t contained = 0;
    bool ok = false;
    for (size_t i = 0; i < parameters->len; i++) {
        parse_sym_entry_t *parameter = parameters->data[i];
        const th_param *type_param = th_func_param(function_type, i);
        th_mode mode = parameter && parameter->has_mode
            ? parameter->mode : TH_IN;
        if (!parameter || !type_param || !parameter->name.data
            || (parameter->is_optional && !th_mode_is_input_only(mode))
            || !th_mode_is_valid(mode)) {
            if (parameter && parameter->name.data && type_param
                && parameter->is_optional && th_mode_is_valid(mode))
                lower_report(&context, N00B_DIAG_OUTPUT_PARAMETER_DEFAULT,
                    lower_symbol_name_span(&context, parameter),
                    N00B_TEXT_PARAMETER_CANNOT_HAVE_A_DEFAULT,
                    N00BB_ARGS(ARG_TEXT(mode_keyword(mode)),
                               ARG_TEXT(parameter->name)));
            else
                lower_internal(&context, IR_SPAN_UNKNOWN,
                    "a parameter has no declaration or an invalid passing "
                    "mode");
            goto cleanup;
        }
        th_type *signature_parameter_type = type_param->type;
        th_type *parameter_type = signature_parameter_type;
        if (!parameter_type) {
            lower_internal(&context, lower_symbol_name_span(&context,
                                                             parameter),
                "parameter type does not match the callable signature");
            goto cleanup;
        }
        if (th_mode_is_input_only(mode)) {
            if (ir_func_parameter_is_optional(function, (uint32_t)i)) {
                char local_buffer[48];
                core_string_t local_name = n00bb_format_name(local_buffer,
                    sizeof(local_buffer), "__n00bb_default_local_%llu",
                    (unsigned long long)context.next_value_id++);
                ir_value_t *argument = ir_param_add(function,
                    parameter->name, parameter_type);
                bool optional_set = argument
                    && ir_param_set_optional(argument, true);
                ir_local_t *local = ir_local_add(function, local_name,
                                                 parameter_type);
                ir_value_t *local_value = local
                    ? ir_local_value(function, local) : nullptr;
                ir_span_t parameter_span = lower_span_for_tree(
                    &context, parameter->decl_node);
                if (!argument || !optional_set || !local
                    || !n00bb_set_reference_access(ir_context, local_value,
                                                   parameter_type)
                    || !n00bb_bind_local(&context, parameter, local,
                                          parameter_span)) {
                    lower_internal(&context,
                        lower_span_for_tree(&context, parameter->decl_node),
                        "defaulted parameter has no valid optional storage");
                    goto cleanup;
                }
                if (!n00bb_attach_symbol_array_shape(&context, grammar,
                        parameter, parameter_type, argument, local)) {
                    goto cleanup;
                }
                core_list_push(lower_data.default_params,
                    ((n00bb_default_param_t){
                        .symbol = parameter,
                        .expression = child_nt(grammar, parameter->decl_node,
                                               CORE_STRING_STATIC("expression")),
                        .local = local,
                        .argument = argument,
                    }));
                continue;
            }
            ir_value_t *value = ir_param_add(function, parameter->name,
                                             parameter_type);
            if (!value || !n00bb_set_reference_access(ir_context, value,
                                                       parameter_type)) {
                goto cleanup;
            }
            if (!n00bb_attach_symbol_array_shape(&context, grammar,
                    parameter, parameter_type, value, nullptr)) {
                goto cleanup;
            }
            if (parameter->storage_duration == PARSE_SYM_STORAGE_DYNAMIC
                || parameter->mutability == PARSE_SYM_MUTABLE
                || (parameter->selected_storage_class.data
                    && parameter->selected_storage_class.u8_bytes)) {
                char local_buffer[48];
                core_string_t local_name = n00bb_format_name(local_buffer,
                    sizeof(local_buffer), "__n00bb_param_local_%llu",
                    (unsigned long long)context.next_value_id++);
                ir_local_t *local = ir_local_add(function, local_name,
                                                 parameter_type);
                ir_value_t *local_value = local
                    ? ir_local_value(function, local) : nullptr;
                ir_span_t parameter_span = lower_span_for_tree(
                    &context, parameter->decl_node);
                if (!local
                    || !n00bb_set_reference_access(ir_context, local_value,
                                                   parameter_type)
                    || (ir_value_shape(value)
                        && !ir_local_set_shape(local,
                                               ir_value_shape(value)))
                    || !n00bb_bind_local(&context, parameter, local,
                                                 parameter_span)
                    || !n00bb_emit_local_write(&context, local, value,
                                               parameter_span)) {
                    lower_internal(&context, parameter_span,
                        "could not place the parameter in selected storage");
                    goto cleanup;
                }
            }
            else if (!ir_lower_bind_value(&context, parameter, value)
                     || !n00bb_begin_final_frame(&context, parameter,
                            lower_span_for_tree(&context,
                                                parameter->decl_node))) {
                goto cleanup;
            }
            continue;
        }

        bool mode_matches = parameter->has_mode
            && (mode == TH_OUT || mode == TH_INOUT);
        ir_value_t *input = mode_matches && mode == TH_INOUT
            ? ir_param_add(function, parameter->name, parameter_type)
            : nullptr;
        char local_buffer[48];
        core_string_t local_name = n00bb_format_name(local_buffer,
            sizeof(local_buffer), "__n00bb_param_%llu",
            (unsigned long long)context.next_value_id++);
        ir_local_t *local = mode_matches
            ? ir_local_add(function, local_name, parameter_type) : nullptr;
        if (local) {
            (void)ir_local_set_source_name(function, local, parameter->name);
            local->synthetic = true;
        }
        ir_value_t *local_value = local ? ir_local_value(function, local)
                                        : nullptr;
        ir_span_t parameter_span = lower_span_for_tree(
            &context, parameter->decl_node);
        if (!mode_matches || (mode == TH_INOUT && !input) || !local
            || (input && !n00bb_set_reference_access(ir_context, input,
                                                     parameter_type))
            || !n00bb_set_reference_access(ir_context, local_value,
                                           parameter_type)
            || !n00bb_attach_symbol_array_shape(&context, grammar,
                parameter, parameter_type, input, local)
            || !n00bb_bind_local(&context, parameter, local,
                                  parameter_span)) {
            lower_internal(&context, parameter_span,
                "output parameter has no matching passing metadata or "
                "local copy");
            goto cleanup;
        }
        if (mode == TH_INOUT) {
            if (!n00bb_emit_local_write(&context, local, input,
                                        parameter_span)) {
                lower_internal(&context,
                    lower_span_for_tree(&context, parameter->decl_node),
                    "could not copy a modify parameter into its local storage");
                goto cleanup;
            }
        }
        size_t output_index = 0;
        for (size_t previous = 0; previous < i; previous++) {
            parse_sym_entry_t *prior = parameters->data[previous];
            if (prior && prior->has_mode
                && th_mode_produces_output(prior->mode))
                output_index++;
        }
        core_list_push(lower_data.parameters_to_copy_out,
            ((n00bb_copyout_t){.symbol = parameter,
                               .local = local,
                               .output_index = output_index,
                               .required_shape = parse_sym_binding(parameter,
                                   CORE_STRING_STATIC("ir.array-shape"))}));
    }

    if (rest_parameter) {
        const th_param *collected = th_func_param(function_type,
            th_func_nparams(function_type) - 1);
        th_type *array_type = collected ? collected->type : nullptr;
        if (array_type) rest_parameter->type = array_type;
        ir_value_t *rest = array_type ? ir_param_add(function,
            rest_parameter->name, array_type) : nullptr;
        bool collected_rest = rest
            && ir_lower_bind_value(&context, rest_parameter, rest)
            && n00bb_attach_symbol_array_shape(&context, grammar,
                rest_parameter, rest_parameter->type, rest, nullptr);
        if (!collected_rest) {
            lower_internal(&context,
                lower_span_for_tree(&context, rest_parameter->decl_node),
                "variadic parameter does not have its collected array type");
            goto cleanup;
        }
    }

    if (!n00bb_initialize_default_params(lowerer, &context,
                                          &lower_data.default_params)) {
        goto cleanup;
    }
    for (size_t i = 0; i < lower_data.default_params.len; i++) {
        n00bb_default_param_t *parameter =
            &lower_data.default_params.data[i];
        char local_buffer[48];
        core_string_t local_name = n00bb_format_name(local_buffer,
            sizeof(local_buffer), "__n00bb_default_bound_%llu",
            (unsigned long long)context.next_value_id++);
        ir_local_t *bound = ir_local_add(function, local_name,
                                         parameter->local->type);
        ir_value_t *bound_value = bound
            ? ir_local_value(function, bound) : nullptr;
        ir_span_t span = lower_span_for_tree(&context,
                                             parameter->symbol->decl_node);
        ir_value_t *resolved = n00bb_emit_local_read(&context,
                                                     parameter->local, span);
        if (!bound || !bound_value
            || !n00bb_set_reference_access(ir_context, bound_value,
                                           parameter->local->type)
            || !ir_lower_rebind_local(&context, parameter->symbol, bound)
            || !n00bb_attach_symbol_array_shape(&context, grammar,
                parameter->symbol, parameter->local->type, nullptr, bound)
            || !resolved
            || !n00bb_emit_local_write(&context, bound, resolved, span)) {
            lower_internal(&context, span,
                "could not bind the initialized default parameter value");
            goto cleanup;
        }
    }

    for (size_t i = 0; i < parameters->len; i++) {
        parse_sym_entry_t *parameter = parameters->data[i];
        ir_shape_t *shape = parse_sym_binding(parameter,
            CORE_STRING_STATIC("ir.array-shape"));
        bool runtime_bound = false;
        for (uint32_t d = 0; shape && d < ir_shape_rank(shape); d++) {
            ir_shape_extent_t extent;
            if (!ir_shape_extent(shape, d, &extent)) goto cleanup;
            runtime_bound |= extent.kind == IR_SHAPE_EXTENT_RUNTIME;
        }
        if (!runtime_bound) continue;
        ir_span_t span = lower_span_for_tree(&context,
                                             parameter->decl_node);
        ir_shape_t *bound_shape = ir_shape_suffix(ir_context, shape, 0);
        if (!bound_shape || !n00bb_lower_signature_array_shape(lowerer,
                &context, grammar, bound_shape)) {
            lower_internal(&context, span,
                "could not lower a parameter's runtime array dimensions");
            goto cleanup;
        }
        th_mode mode = parameter->has_mode ? parameter->mode : TH_IN;
        ir_local_t *local = ir_lower_bound_local(&context, parameter);
        ir_value_t *parameter_value = ir_lower_bound_value(&context,
                                                           parameter);
        if ((local && !ir_local_set_shape(local, bound_shape))
            || (parameter_value
                && !ir_value_set_shape(parameter_value, bound_shape)))
            goto cleanup;
        for (size_t c = 0; c < lower_data.parameters_to_copy_out.len; c++)
            if (lower_data.parameters_to_copy_out.data[c].symbol == parameter)
                lower_data.parameters_to_copy_out.data[c].required_shape =
                    bound_shape;
        if (mode == TH_OUT) continue;
        ir_value_t *value = local
            ? n00bb_emit_local_read(&context, local, span)
            : parameter_value;
        if (!value || !n00bb_emit_sequence_shape_check_impl(&context,
                value, bound_shape, span, true)) goto cleanup;
    }
    ir_shape_t *return_shape = symbol ? parse_sym_binding(symbol,
        CORE_STRING_STATIC("ir.return-shape")) : nullptr;
    if (return_shape) {
        lower_data.bound_return_shape = ir_shape_suffix(ir_context,
                                                        return_shape, 0);
    }
    if (return_shape && (!lower_data.bound_return_shape
        || !n00bb_lower_signature_array_shape(lowerer, &context,
            grammar, lower_data.bound_return_shape))) {
        lower_internal(&context,
            lower_span_for_tree(&context, symbol->decl_node),
            "could not lower the return's runtime array dimensions");
        goto cleanup;
    }

    if (!n00bb_open_result_variable(&context, symbol)) {
        lower_internal(&context, lower_span_for_tree(&context, body),
            "could not open the function's result variable");
        goto cleanup;
    }
    ir_lower_result_t lowered = {IR_LOWER_NO_VALUE, nullptr};
    if (roots) {
        for (size_t i = 0; i < roots->len; i++) {
            if (!context.reachable) {
                lower_report(&context, N00B_DIAG_UNREACHABLE_TOP_LEVEL,
                    lower_span_for_tree(&context, roots->data[i]),
                    N00B_TEXT_THIS_TOP_LEVEL_STATEMENT_CAN,
                    N00BB_NO_ARGS);
                lower_help(&context, N00B_TEXT_REMOVE_THE_STATEMENT_OR_THE,
                    N00BB_NO_ARGS);
                lowered.status = IR_LOWER_FAILED;
                break;
            }
            lower_data.function_body = nt_is(grammar, roots->data[i], CORE_STRING_STATIC("body"))
                ? roots->data[i] : body;
            ir_func_t *deferred = state->deferred_consts
                ? core_dict_get(state->deferred_consts, roots->data[i],
                                nullptr)
                : nullptr;
            if (deferred) {
                if (!n00bb_emit_comptime_call(&context, deferred,
                        lower_span_for_tree(&context, roots->data[i]))) {
                    lowered.status = IR_LOWER_FAILED;
                    break;
                }
                continue;
            }
            ir_instr_t *entry = nullptr;
            ir_block_t *start = statement_block(&context, i, &entry);
            frame_marks_t marks = frame_marks(&lower_data);
            size_t before = lower_failures(&context);
            lowered = ir_lower_tree(lowerer, &context, grammar,
                                    roots->data[i]);
            parse_tree_t *statement = roots->data[i];
            while (statement && !nt_is(grammar, statement,
                                       CORE_STRING_STATIC("top-level-stmt")))
                statement = parse_tree_parent(statement);
            bool settled = lowered.status != IR_LOWER_FAILED
                && (!n00bb_static_declaration(grammar, state, roots->data[i])
                    || mark_declaration(ir_context, context.block))
                && (!statement || n00bb_finish_final_scope(&context,
                                                           statement,
                                                           roots->data[i]));
            size_t added = lower_failures(&context) - before;
            if (settled && !added) {
                if (start && !tag_statement(ir_context, function, start, i)) {
                    lowered.status = IR_LOWER_FAILED;
                    break;
                }
                continue;
            }
            if (!start || !added) {
                lowered.status = IR_LOWER_FAILED;
                break;
            }
            restore_frames(&lower_data, marks);
            if (!cut_statement(&context, start, entry, i)) {
                lowered.status = IR_LOWER_FAILED;
                break;
            }
            contained += added;
            lowered = (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr};
        }
    }
    else lowered = ir_lower_tree(lowerer, &context, grammar, body);
    if (lowered.status == IR_LOWER_FAILED) goto cleanup;
    if (expression_body) {
        if (lowered.status != IR_LOWER_VALUE || !lowered.value) {
            lower_report(&context, N00B_DIAG_VALUE_BLOCK_WITHOUT_VALUE,
                lower_span_for_tree(&context, body),
                N00B_TEXT_THIS_DEFAULT_VALUE_DOES_NOT,
                N00BB_NO_ARGS);
            goto cleanup;
        }
        if (expression_result_hook
            && !expression_result_hook(&context, &lowered.value,
                                       expression_result_data))
            goto cleanup;
        if (context.reachable) {
            if (!n00bb_finish_final_scope(&context, lower_data.finals.len
                        ? lower_data.finals.data[0].scope : nullptr, body)
                || !n00bb_emit_success_return(&context, lowered.value,
                    &lower_data.parameters_to_copy_out,
                    lower_span_for_tree(&context, body))) goto cleanup;
        }
    }
    else if (context.reachable) {
        if (!n00bb_emit_with_success(&context, body)
            || !n00bb_finish_final_scope(&context, lower_data.finals.len
                    ? lower_data.finals.data[0].scope : nullptr, body))
            goto cleanup;
        ir_span_t implicit_return = ir_module_span(module);
        implicit_return.implied = true;
        if (!n00bb_emit_success_return(&context, nullptr,
                &lower_data.parameters_to_copy_out, implicit_return))
            goto cleanup;
    }
    ok = lower_failures(&context) <= failures + contained
        || lower_mark_failed(&context);

cleanup:
    if (lower_data.node_types_ready) core_dict_free(&lower_data.node_types);
    for (size_t i = 0; i < lower_data.branches.len; i++)
        core_list_free(lower_data.branches.data[i].arms);
    core_list_free(lower_data.branches);
    core_list_free(lower_data.loops);
    core_list_free(lower_data.requires);
    for (size_t i = 0; i < lower_data.switches.len; i++) {
        for (size_t c = 0; c < lower_data.switches.data[i].cases.len; c++)
            core_list_free(lower_data.switches.data[i].cases.data[c].patterns);
        core_list_free(lower_data.switches.data[i].cases);
    }
    core_list_free(lower_data.switches);
    for (size_t i = 0; i < lower_data.typecases.len; i++)
        core_list_free(lower_data.typecases.data[i].arms);
    core_list_free(lower_data.typecases);
    core_list_free(lower_data.value_blocks);
    core_list_free(lower_data.init_fields);
    core_list_free(lower_data.tries);
    for (size_t w = 0; w < lower_data.withs.len; w++) {
        for (size_t i = 0; i < lower_data.withs.data[w].items.len; i++)
            core_list_free(lower_data.withs.data[w].items.data[i].captures);
        core_list_free(lower_data.withs.data[w].items);
    }
    core_list_free(lower_data.withs);
    core_list_free(lower_data.finals);
    for (size_t i = 0; i < lower_data.pending_call_copyouts.len; i++)
        core_list_free(lower_data.pending_call_copyouts.data[i].items);
    core_list_free(lower_data.pending_call_copyouts);
    core_list_free(lower_data.default_params);
    for (size_t i = 0; i < lower_data.tuple_parts.len; i++) {
        core_list_free(lower_data.tuple_parts.data[i].expressions);
        core_list_free(lower_data.tuple_parts.data[i].values);
    }
    core_list_free(lower_data.tuple_parts);
    core_list_free(lower_data.parameters_to_copy_out);
    ir_lower_context_release(&context);
    return ok;
}

bool
span_contains(ir_span_t span, core_diag_location_t at)
{
    if (!span.start_line || !at.has_location) return false;
    bool after_start = at.line > span.start_line
        || (at.line == span.start_line && at.column >= span.start_col);
    bool before_end = at.line < span.end_line
        || (at.line == span.end_line && at.column <= span.end_col);
    return after_start && before_end;
}

static ir_func_t *
function_at(ir_module_t *module, core_diag_location_t at)
{
    ir_func_t *found = nullptr;
    for (size_t f = 0; module && f < ir_func_count(module); f++) {
        ir_func_t *function = ir_func_at(module, f);
        if (!function->n_blocks || ir_func_is_initializer(function)
            || !span_contains(function->span, at))
            continue;
        if (!found || function->span.end_line - function->span.start_line
                < found->span.end_line - found->span.start_line)
            found = function;
    }
    return found;
}

bool
exclude_function(n00bb_link_analysis_t *analysis,
                 core_translation_unit_t *unit, ir_func_t *function)
{
    if (!function) return false;
    core_list_push(analysis->failed_regions, ((n00bb_failed_region_t){
        .module = core_string_dup(unit->module.identifier),
        .span = function->span,
    }));
    return ir_func_discard_body(function);
}

void
exclude_failed_functions(n00bb_link_analysis_t *analysis,
                         core_list_t(core_translation_unit_ptr_t) *units,
                         size_t *marks)
{
    for (size_t u = 0; marks && u < units->len; u++) {
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(units->data[u]);
        ir_module_t *module = tu_state(units->data[u])->ir_module;
        size_t count = core_diagnostics_count(diagnostics);
        for (size_t i = marks[u]; i < count; i++) {
            const core_diagnostic_t *item = core_diagnostics_at(diagnostics, i);
            if (item->severity != CORE_DIAG_ERROR
                && item->severity != CORE_DIAG_FATAL) continue;
            ir_func_t *function = function_at(module, item->location);
            if (function) (void)exclude_function(analysis, units->data[u],
                                                 function);
        }
        marks[u] = count;
    }
}

bool
explained_by_errors(core_list_t(core_translation_unit_ptr_t) *units)
{
    bool reported = false;
    for (size_t u = 0; u < units->len; u++) {
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(units->data[u]);
        for (size_t i = 0; i < core_diagnostics_count(diagnostics); i++) {
            core_diag_severity_t severity =
                core_diagnostics_at(diagnostics, i)->severity;
            if (severity == CORE_DIAG_INTERNAL) return false;
            reported = reported || severity == CORE_DIAG_ERROR
                || severity == CORE_DIAG_FATAL;
        }
    }
    return reported;
}

bool
lower_function_body(ir_lowerer_t *lowerer, parse_grammar_t *grammar,
                    core_translation_unit_t *unit,
                    parse_sym_entry_t *symbol,
                    ir_ctx_t *ir_context,
                    core_list_t(core_translation_unit_ptr_t) *units)
{
    n00bb_tu_state_t *state = tu_state(unit);
    ir_module_t *module = unit_module(state);
    parse_tree_t *declaration = symbol ? symbol->decl_node : nullptr;
    parse_tree_t *body = child_nt(grammar, declaration, CORE_STRING_STATIC("body"));
    ir_func_t *function = state && module && symbol
        ? link_ir_function_for_symbol(state->link_analysis, symbol)
        : nullptr;
    core_list_t(parse_sym_entry_ptr_t) parameters =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *rest = nullptr;
    parse_tree_t *signature = child_nt(grammar, declaration,
                                       CORE_STRING_STATIC("func-type-signature"));
    collect_ir_parameters(grammar, signature, &parameters, &rest);
    bool ok = lower_ir_body(lowerer, grammar, unit, symbol, body,
        function, &parameters, nullptr, rest, false, ir_context, units,
        nullptr, nullptr);
    core_list_free(parameters);
    return ok;
}

static bool
n00bb_record_assert_site(ir_lower_context_t *context,
                         n00bb_lower_context_t *lower, ir_span_t span)
{
    n00bb_tu_state_t *state = tu_state(lower->unit);
    n00bb_link_analysis_t *analysis = state ? state->link_analysis : nullptr;
    if (!analysis || !context->function) return false;
    core_list_push(analysis->assert_sites, ((n00bb_assert_site_t){
        .function = context->function, .span = span, .unit = lower->unit,
    }));
    return true;
}

bool
n00bb_lower_assert_statement(ir_lower_context_t *context,
                             parse_visit_t *visit, bool entering,
                             void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    parse_tree_t *node = visit->node;
    ir_span_t span = lower_span_for_tree(context, node);
    bool debug_only = core_string_eq(first_token_text(node),
                                     CORE_STRING_STATIC("assert"));
    parse_tree_t *expression = child_nt(visit->grammar, node, CORE_STRING_STATIC("expression"));
    bool jumping = expression
        && jumping_condition(visit->grammar, expression);
    if (entering) {
        if (!jumping || !context->reachable) return true;
        ir_block_t *join = n00bb_branch_block(context, lower,
                                              "require_join");
        ir_block_t *failure = n00bb_branch_block(context, lower,
                                                  "condition_failed");
        if (!join || !failure) return false;
        core_list_push(lower->requires, ((n00bb_require_frame_t){
            .node = node, .condition = expression,
            .join = join, .failure = failure}));
        return true;
    }
    n00bb_require_frame_t frame = {0};
    if (lower->requires.len
        && lower->requires.data[lower->requires.len - 1].node == node)
        frame = lower->requires.data[--lower->requires.len];
    if (!context->reachable && !frame.node) return true;
    if (debug_only && !n00bb_record_assert_site(context, lower, span))
        return false;
    n00bb_failure_kind_t failure_kind = debug_only ? N00BB_FAILURE_ASSERT
                                                   : N00BB_FAILURE_REQUIRE;
    core_string_t condition_text = expression
        ? n00bb_node_source_text(lower->unit, expression, (core_string_t){0})
        : (core_string_t){0};
    ir_span_t condition_span = expression
        ? lower_span_for_tree(context, expression) : span;
    if (frame.node) {
        if (!n00bb_close_condition_scratch(lower)) return false;
        core_string_t frame_texts = {0};
        ir_value_t *frame_operands = n00bb_condition_operands(context, lower,
            visit->grammar, expression, frame.failure, condition_span,
            &frame_texts);
        ir_instr_t *trap = ir_instr_add(frame.failure, IR_OP_TRAP);
        if (!trap || !n00bb_describe_failure(lower, trap, failure_kind,
                condition_text, (core_string_t){0}))
            return false;
        if (frame_operands && (!ir_instr_operand_value(trap, frame_operands)
                || !ir_annot_set(lower->ir_context, &trap->annots,
                    (ir_annot_t){
                        .key = CORE_STRING_STATIC(
                            N00BB_FAILURE_OPERANDS_ANNOTATION),
                        .kind = IR_ANNOT_STR, .s = frame_texts,
                    })))
            return false;
        trap->span = condition_span;
        context->block = frame.join;
        context->reachable = true;
        return true;
    }

    ir_value_t *condition = expression
        ? condition_value(context, expression) : nullptr;
    th_type *bool_type = n00bb_ir_builtin_type(context->module, CORE_STRING_STATIC("bool"));
    if (!condition || !bool_type) {
        lower_condition_error(context, expression, condition,
                              N00B_TEXT_PHRASE_AN_ASSERT_OR_REQUIRE_CONDITION);
        return false;
    }
    ir_instr_t *constant = condition->def;
    if (constant && constant->op == IR_OP_CONST
        && constant->n_operands == 1
        && constant->operands[0].kind == IR_OPD_CONST
        && !constant->operands[0].lit.is_float
        && !constant->operands[0].lit.i.mag) {
        ir_instr_t *trap = ir_instr_add(context->block, IR_OP_TRAP);
        if (!trap || !n00bb_describe_failure(lower, trap,
                N00BB_FAILURE_UNREACHABLE,
                n00bb_node_source_text(lower->unit, node, (core_string_t){0}),
                (core_string_t){0}))
            return false;
        trap->span = span;
        context->reachable = false;
        return true;
    }
    ir_block_t *join = n00bb_branch_block(context, lower, "require_join");
    ir_block_t *failure = n00bb_branch_block(context, lower,
                                              "condition_failed");
    ir_instr_t *branch = ir_instr_add(context->block, IR_OP_BRANCH);
    if (!join || !failure || !branch
        || !ir_instr_operand_value(branch, condition)
        || !ir_instr_operand_block(branch, join)
        || !ir_instr_operand_block(branch, failure)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
            "could not lower assertion condition");
        return false;
    }
    branch->span = span;
    if (!n00bb_mark_condition(context,
            N00B_TEXT_PHRASE_AN_ASSERT_OR_REQUIRE_CONDITION, expression))
        return false;
    core_string_t operand_texts = {0};
    ir_value_t *operands = n00bb_condition_operands(context, lower,
        visit->grammar, expression, failure, condition_span, &operand_texts);
    ir_instr_t *trap = ir_instr_add(failure, IR_OP_TRAP);
    if (!trap || !n00bb_describe_failure(lower, trap, failure_kind,
            condition_text, (core_string_t){0}))
        return false;
    if (operands && (!ir_instr_operand_value(trap, operands)
            || !ir_annot_set(lower->ir_context, &trap->annots, (ir_annot_t){
                .key = CORE_STRING_STATIC(N00BB_FAILURE_OPERANDS_ANNOTATION),
                .kind = IR_ANNOT_STR, .s = operand_texts,
            })))
        return false;
    trap->span = condition_span;
    context->block = join;
    context->reachable = true;
    return true;
}

bool
lower_context_hook(ir_lowerer_t *lowerer, parse_grammar_t *grammar,
                   core_translation_unit_t *unit,
                   parse_sym_entry_t *context_symbol,
                   parse_tree_t *hook, ir_ctx_t *ir_context,
                   core_list_t(core_translation_unit_ptr_t) *units)
{
    n00bb_tu_state_t *state = tu_state(unit);
    core_string_t role = context_hook_binding_role(grammar, hook);
    core_link_declaration_t *declaration = state
        ? link_declaration_for_symbol(state->link_analysis, context_symbol)
        : nullptr;
    ir_func_t *function = declaration && role.data
        ? core_link_declaration_binding(declaration, role)
        : nullptr;
    parse_tree_t *body = child_nt(grammar, hook, CORE_STRING_STATIC("body"));
    th_type *type = parse_tree_type(hook);
    if (!function || !body || !type) return false;

    core_list_t(parse_sym_entry_ptr_t) parameters =
        core_list_new(parse_sym_entry_ptr_t);
    parse_tree_t *context_params = child_nt(grammar, context_symbol->decl_node,
                                             CORE_STRING_STATIC("param-decl"));
    parse_tree_t *hook_params = child_nt(grammar, hook,
                                          CORE_STRING_STATIC("context-hook-params"));
    parse_sym_entry_t *rest = nullptr;
    collect_ir_parameters(grammar, context_params, &parameters, &rest);
    collect_ir_parameters(grammar, hook_params, &parameters, &rest);
    parse_sym_entry_t function_symbol = {
        .name = context_symbol->name,
        .kind = PARSE_SYM_FUNCTION,
        .type = type,
        .decl_node = hook,
    };
    bool ok = lower_ir_body(lowerer, grammar, unit,
        &function_symbol, body, function, &parameters, nullptr, rest,
        false, ir_context, units, nullptr, nullptr);
    core_list_free(parameters);
    return ok;
}

bool
lower_context_default(ir_lowerer_t *lowerer, parse_grammar_t *grammar,
                      core_translation_unit_t *unit,
                      parse_sym_entry_t *context_symbol, size_t parameter_index,
                      ir_ctx_t *ir_context,
                      core_list_t(core_translation_unit_ptr_t) *units)
{
    n00bb_tu_state_t *state = tu_state(unit);
    core_link_declaration_t *declaration = state
        ? link_declaration_for_symbol(state->link_analysis, context_symbol)
        : nullptr;
    char role_buffer[64];
    core_string_t role = {};
    core_buffer_t name = {0};
    if (!state || !declaration
        || !context_default_role(parameter_index, role_buffer,
                                 sizeof(role_buffer), &role)
        || !context_default_name(context_symbol, parameter_index, &name)) {
        core_buffer_release(&name);
        return false;
    }
    ir_func_t *function = core_link_declaration_binding(declaration, role);
    parse_tree_t *param_decl = child_nt(grammar, context_symbol->decl_node,
                                         CORE_STRING_STATIC("param-decl"));
    core_list_t(parse_sym_entry_ptr_t) parameters =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *rest = nullptr;
    collect_ir_parameters(grammar, param_decl, &parameters, &rest);
    if (rest) core_list_push(parameters, rest);
    parse_sym_entry_t *target = parameter_index < parameters.len
        ? parameters.data[parameter_index] : nullptr;
    parse_tree_t *expression = target
        ? child_nt(grammar, target->decl_node, CORE_STRING_STATIC("expression")) : nullptr;
    core_list_t(parse_sym_entry_ptr_t) dependencies =
        core_list_new(parse_sym_entry_ptr_t);
    context_default_dependencies(expression, &parameters,
                                 &dependencies);
    if (!function || !target || !expression) {
        core_buffer_release(&name);
        core_list_free(dependencies);
        core_list_free(parameters);
        return false;
    }
    parse_sym_entry_t function_symbol = {
        .name = core_buffer_string(&name),
        .kind = PARSE_SYM_FUNCTION,
        .type = function->type,
        .decl_node = target->decl_node,
    };
    bool ok = lower_ir_body(lowerer, grammar, unit, &function_symbol,
        expression, function, &dependencies, nullptr, nullptr,
        true, ir_context, units, nullptr, nullptr);
    core_buffer_release(&name);
    core_list_free(dependencies);
    core_list_free(parameters);
    return ok;
}
