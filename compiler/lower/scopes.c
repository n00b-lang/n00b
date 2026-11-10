#include "lower_internal.h"

static bool
n00bb_mark_binding_reentry(ir_lower_context_t *context,
                           parse_sym_entry_t *symbol);
static ir_value_t *
n00bb_final_address(ir_lower_context_t *context, parse_sym_entry_t *symbol,
                    ir_span_t span);
static ir_value_t *
n00bb_fields_storage(ir_lower_context_t *context, ir_value_t *address,
                     ir_span_t span);
static ir_value_t *
n00bb_final_field(ir_lower_context_t *context, ir_value_t *address,
                  core_string_t name, ir_span_t span);
static ir_local_t *
n00bb_finalize_temporary(ir_lower_context_t *context, th_type *type);
static ir_value_t *
n00bb_storage_address(ir_lower_context_t *context, ir_local_t *storage,
                      ir_span_t span);
static bool
n00bb_finalize_copy(ir_lower_context_t *context, ir_value_t *value,
                    th_type *type, ir_span_t span, size_t depth);
static bool
n00bb_emit_finalizer_call(ir_lower_context_t *context,
                          const th_env_method_t *method,
                          ir_value_t *address, ir_span_t span);
static bool
n00bb_emit_open_finalize(ir_lower_context_t *context, ir_value_t *address,
                         th_type *type, ir_span_t span);

static bool
n00bb_binding_in_loop_scope(parse_sym_entry_t *symbol,
                            parse_tree_t *loop)
{
    for (parse_scope_t *scope = symbol ? symbol->scope : nullptr;
         scope; scope = scope->parent)
        if (scope->node == loop) return true;
    return false;
}

static bool
n00bb_mark_binding_reentry(ir_lower_context_t *context,
                           parse_sym_entry_t *symbol)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    if (!context || !lower || !symbol || !symbol->scope) return false;

    for (size_t i = lower->loops.len; i > 0; i--) {
        n00bb_loop_frame_t *frame = &lower->loops.data[i - 1];
        if (!frame->active || !frame->node || !frame->header) continue;
        if (!n00bb_binding_in_loop_scope(symbol, frame->node)) continue;

        ir_value_t *binding = ir_lower_bound_address(context, symbol);
        if (!binding) {
            ir_local_t *local = ir_lower_bound_local(context, symbol);
            binding = local ? ir_local_value(context->function, local)
                            : nullptr;
        }
        return binding && ir_value_set_binding_reentry(binding,
                                                       frame->header);
    }
    return true;
}

ir_mutability_t
local_binding_mutability(const parse_sym_entry_t *symbol)
{
    return symbol->kind == PARSE_SYM_VARIABLE
        && symbol->mutability == PARSE_SYM_IMMUTABLE
        ? IR_MUT_IMMUTABLE : IR_MUT_MUTABLE;
}

n00bb_final_frame_t *
n00bb_final_frame(n00bb_lower_context_t *lower,
                  const parse_sym_entry_t *symbol)
{
    for (size_t i = lower ? lower->finals.len : 0;
         symbol && i > lower->final_base; i--)
        if (lower->finals.data[i - 1].symbol == symbol)
            return &lower->finals.data[i - 1];
    return nullptr;
}

bool
n00bb_set_flag(ir_lower_context_t *context, ir_local_t *flag, bool value,
               ir_span_t span)
{
    th_type *bool_type = n00bb_ir_builtin_type(context->module,
                                               CORE_STRING_STATIC("bool"));
    ir_instr_t *constant = bool_type
        ? ir_lower_emit_instruction(context, IR_OP_CONST, span) : nullptr;
    if (!constant || !ir_lower_set_temp_dest(context, constant,
            CORE_STRING_STATIC("__n00bb_"), bool_type)
        || !ir_instr_operand_int(constant, value ? 1 : 0)) return false;
    ir_instr_t *copy = ir_lower_emit_instruction(context, IR_OP_COPY, span);
    return copy && ir_instr_set_dest_local(copy, flag)
        && ir_instr_operand_value(copy, constant->dest);
}

bool
n00bb_set_owned(ir_lower_context_t *context, n00bb_final_frame_t *frame,
                bool owned, ir_span_t span)
{
    return n00bb_set_flag(context, frame->owned, owned, span);
}

static ir_value_t *
n00bb_final_address(ir_lower_context_t *context, parse_sym_entry_t *symbol,
                    ir_span_t span)
{
    ir_value_t *address = ir_lower_bound_address(context, symbol);
    if (address)
        return n00bb_emit_temp(context, IR_OP_COPY, address->type, address,
                               span);
    ir_local_t *local = ir_lower_bound_local(context, symbol);
    if (local) address = ir_lower_local_address(context, local);
    if (address) return address;
    ir_value_t *binding = local ? nullptr
        : ir_lower_bound_value(context, symbol);
    th_type *referent = local ? local->type
        : binding ? binding->type : nullptr;
    th_type *pointer_type = referent
        ? th_ind(ir_module_types(context->module), 0, referent) : nullptr;
    ir_instr_t *take = pointer_type
        ? ir_lower_emit_instruction(context, IR_OP_ADDRESS_OF, span)
        : nullptr;
    if (!take || !ir_lower_set_temp_dest(context, take,
            CORE_STRING_STATIC("__n00bb_finalized_"), pointer_type))
        return nullptr;
    bool bound = local ? ir_instr_operand_local(take, local)
        : ir_instr_operand_value(take, binding);
    return bound ? take->dest : nullptr;
}

static ir_value_t *
n00bb_fields_storage(ir_lower_context_t *context, ir_value_t *address,
                     ir_span_t span)
{
    th_ctx *types = ir_module_types(context->module);
    th_type *referent = th_resolve(th_ind_referent(th_resolve(address->type)));
    th_type *definition = referent && th_kind_of(referent) == TH_NAMED
        ? th_type_definition(types, referent) : nullptr;
    if (!definition || th_kind_of(th_resolve(definition)) != TH_INDIRECTION)
        return address;
    ir_instr_t *load = ir_lower_emit_instruction(context, IR_OP_LOAD, span);
    return load && ir_lower_set_temp_dest(context, load,
            CORE_STRING_STATIC("__n00bb_finalizing_"), referent)
        && ir_instr_operand_value(load, address)
        ? n00bb_reference_view(context, load->dest, span) : nullptr;
}

static ir_value_t *
n00bb_final_field(ir_lower_context_t *context, ir_value_t *address,
                  core_string_t name, ir_span_t span)
{
    th_ctx *types = ir_module_types(context->module);
    th_type *record = th_ind_referent(th_resolve(address->type));
    th_type *field = record && !th_is_poison(record)
        ? th_record_lookup_in(types, th_resolve(record), name) : nullptr;
    th_type *pointer_type = field ? th_ind(types, 0, field) : nullptr;
    ir_instr_t *at = pointer_type
        ? ir_lower_emit_instruction(context, IR_OP_FIELD, span) : nullptr;
    return at && ir_lower_set_temp_dest(context, at,
            CORE_STRING_STATIC("__n00bb_finalized_field_"), pointer_type)
        && ir_instr_operand_value(at, address)
        && ir_instr_operand_name(at, name) ? at->dest : nullptr;
}

static ir_local_t *
n00bb_finalize_temporary(ir_lower_context_t *context, th_type *type)
{
    char buffer[48];
    core_string_t name = n00bb_format_name(buffer, sizeof(buffer),
        "__n00bb_finalizing_%llu",
        (unsigned long long)context->next_value_id++);
    return ir_local_add(context->function, name, type);
}

static ir_value_t *
n00bb_storage_address(ir_lower_context_t *context, ir_local_t *storage,
                      ir_span_t span)
{
    th_type *pointer_type = th_ind(ir_module_types(context->module), 0,
                                   storage->type);
    ir_instr_t *take = ir_lower_emit_instruction(context, IR_OP_ADDRESS_OF,
                                                 span);
    return take && ir_lower_set_temp_dest(context, take,
            CORE_STRING_STATIC("__n00bb_finalizing_"), pointer_type)
        && ir_instr_operand_local(take, storage) ? take->dest : nullptr;
}

ir_value_t *
n00bb_emit_temp(ir_lower_context_t *context, ir_op_t op, th_type *type,
                ir_value_t *operand, ir_span_t span)
{
    ir_instr_t *in = ir_lower_emit_instruction(context, op, span);
    return in && ir_lower_set_temp_dest(context, in,
            CORE_STRING_STATIC("__n00bb_finalizing_"), type)
        && ir_instr_operand_value(in, operand) ? in->dest : nullptr;
}

static bool
n00bb_finalize_copy(ir_lower_context_t *context, ir_value_t *value,
                    th_type *type, ir_span_t span, size_t depth)
{
    ir_local_t *local = n00bb_finalize_temporary(context, type);
    ir_instr_t *copy = local
        ? ir_lower_emit_instruction(context, IR_OP_COPY, span) : nullptr;
    ir_value_t *address = copy && ir_instr_set_dest_local(copy, local)
        && ir_instr_operand_value(copy, value)
        ? n00bb_storage_address(context, local, span) : nullptr;
    return address && n00bb_emit_finalize_value(context, address, type, span,
                                                depth + 1);
}

static bool
n00bb_emit_finalizer_call(ir_lower_context_t *context,
                          const th_env_method_t *method,
                          ir_value_t *address, ir_span_t span)
{
    ir_module_t *owner = nullptr;
    ir_func_t *function = n00bb_record_field_method(context, method, &owner);
    ir_value_t *callee = owner && function
        ? ir_func_address(owner, function) : nullptr;
    th_type *signature = callee ? method->signature : nullptr;
    th_type *nil_type = th_name(ir_module_types(context->module),
                                CORE_STRING_STATIC("nil"));
    ir_value_t *consumed = signature
        ? n00bb_emit_consume_value(context, address, nullptr, span)
        : nullptr;
    ir_instr_t *call = consumed
        ? ir_lower_emit_instruction(context, IR_OP_CALL, span) : nullptr;
    return call && ir_lower_set_temp_dest(context, call,
            CORE_STRING_STATIC("__n00bb_finalized_"), nil_type)
        && ir_instr_operand_value(call, callee)
        && ir_instr_operand_value(call, consumed)
        && ir_instr_set_call_signature(call, signature);
}

static bool
n00bb_emit_open_finalize(ir_lower_context_t *context, ir_value_t *address,
                         th_type *type, ir_span_t span)
{
    th_ctx *types = ir_module_types(context->module);
    th_type *nil_type = th_name(types, CORE_STRING_STATIC("nil"));
    th_param parameter = {.mode = TH_CONSUME, .type = th_ind(types, 0, type)};
    th_type *signature = parameter.type
        ? th_func_with_error(types, nil_type, nil_type, &parameter, 1,
                             nullptr, nullptr, 0) : nullptr;
    n00bb_lower_context_t *lower = context->user;
    char buffer[64];
    core_string_t name = n00bb_format_name(buffer, sizeof(buffer),
        lower && lower->solved ? "__n00bb_finalize_open#%016llx"
                               : "__n00bb_finalize_pending#%016llx",
        (unsigned long long)th_hash(types, type));
    ir_func_t *placeholder = ir_func_find(context->module, name);
    if (!placeholder && signature) {
        placeholder = ir_func_add(context->module, name, signature,
                                  IR_LINK_INTERNAL);
        ir_block_t *entry = placeholder
            && ir_param_add(placeholder, CORE_STRING_STATIC("self"),
                            parameter.type)
            ? ir_block_add(placeholder, CORE_STRING_STATIC("entry"))
            : nullptr;
        if (!entry || !ir_instr_add(entry, IR_OP_RETURN)) return false;
    }
    ir_value_t *callee = placeholder
        ? ir_func_address(context->module, placeholder) : nullptr;
    ir_value_t *consumed = callee
        ? n00bb_emit_consume_value(context, address, nullptr, span) : nullptr;
    ir_instr_t *call = consumed
        ? ir_lower_emit_instruction(context, IR_OP_CALL, span) : nullptr;
    n00bb_rewrite_later(context, call);
    return call && ir_lower_set_temp_dest(context, call,
            CORE_STRING_STATIC("__n00bb_finalized_"), nil_type)
        && ir_instr_operand_value(call, callee)
        && ir_instr_operand_value(call, consumed)
        && ir_instr_set_call_signature(call, signature);
}

bool
n00bb_emit_finalize_fields(ir_lower_context_t *context, ir_value_t *address,
                           ir_span_t span, size_t depth)
{
    n00bb_lower_context_t *lower = context->user;
    th_ctx *types = ir_module_types(context->module);
    address = n00bb_fields_storage(context, address, span);
    if (!address) return false;
    th_type *resolved = th_resolve(th_ind_referent(th_resolve(address->type)));
    th_type *shape = resolved && th_kind_of(resolved) == TH_NAMED
        ? th_type_definition(types, resolved) : resolved;
    shape = shape ? th_resolve(shape) : nullptr;
    for (size_t i = 0; shape && th_kind_of(shape) == TH_RECORD
         && i < th_record_len(shape); i++) {
        const th_member *member = th_record_member(shape, i);
        if (!member || !member->type
            || !n00bb_type_may_finalize(lower->unit, member->type)) continue;
        ir_value_t *field = n00bb_final_field(context, address, member->name,
                                              span);
        if (!field || !n00bb_emit_finalize_value(context, field, member->type,
                                                 span, depth + 1))
            return false;
    }
    return true;
}

bool
n00bb_emit_finalize_value(ir_lower_context_t *context, ir_value_t *address,
                          th_type *type, ir_span_t span, size_t depth)
{
    n00bb_lower_context_t *lower = context->user;
    th_type *resolved = type ? th_resolve(type) : nullptr;
    if (!resolved || depth >= N00BB_FINALIZE_DEPTH) return true;
    if (!th_is_closed(ir_module_types(context->module), resolved))
        return !n00bb_type_may_finalize(lower->unit, resolved)
            || n00bb_emit_open_finalize(context, address, resolved, span);
    if (!n00bb_type_finalized(lower->unit, resolved)) return true;
    th_type *item = th_option_item(resolved);
    size_t count = 0;
    if (item || th_tuple_element(resolved, 0, &count)) {
        ir_value_t *value = n00bb_emit_temp(context, IR_OP_LOAD, resolved,
                                            address, span);
        if (!value) return false;
        if (!item) {
            for (size_t i = 0; i < count; i++) {
                th_type *element = th_tuple_element(resolved, i,
                                                             nullptr);
                if (!n00bb_type_finalized(lower->unit, element)) continue;
                ir_instr_t *extract = ir_lower_emit_instruction(context,
                    IR_OP_EXTRACT_INDEX, span);
                if (!extract || !ir_lower_set_temp_dest(context, extract,
                        CORE_STRING_STATIC("__n00bb_finalizing_"), element)
                    || !ir_instr_operand_value(extract, value)
                    || !ir_instr_operand_uint(extract, i)
                    || !n00bb_finalize_copy(context, extract->dest, element,
                                            span, depth)) return false;
            }
            return true;
        }
        th_type *bool_type = n00bb_ir_builtin_type(context->module,
                                                   CORE_STRING_STATIC("bool"));
        ir_value_t *absent = bool_type
            ? n00bb_emit_temp(context, IR_OP_OPTION_IS_NIL, bool_type, value,
                              span) : nullptr;
        ir_block_t *present = n00bb_branch_block(context, lower, "present");
        ir_block_t *join = n00bb_branch_block(context, lower, "absent");
        if (!absent || !present || !join
            || !n00bb_emit_branch(context, absent, join, present, span))
            return false;
        context->block = present;
        ir_value_t *payload = n00bb_emit_temp(context, IR_OP_OPTION_GET, item,
                                              value, span);
        if (!payload || !n00bb_finalize_copy(context, payload, item, span,
                                             depth)
            || !n00bb_emit_jump(context, join, span)) return false;
        context->block = join;
        return true;
    }
    th_env_method_t method = {0};
    if (n00bb_type_finalizer(lower->unit, resolved, &method))
        return n00bb_emit_finalizer_call(context, &method, address, span);
    return n00bb_emit_finalize_fields(context, address, span, depth);
}

bool
n00bb_emit_scope_finalize(ir_lower_context_t *context,
                          n00bb_final_frame_t *frame, parse_tree_t *source)
{
    n00bb_lower_context_t *lower = context->user;
    ir_span_t span = lower_span_for_tree(context, source);
    ir_value_t *owned = ir_local_value(context->function, frame->owned);
    ir_block_t *finalize = n00bb_branch_block(context, lower, "finalize");
    ir_block_t *join = n00bb_branch_block(context, lower, "finalized");
    ir_instr_t *branch = finalize && join && owned
        ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
    if (!branch || !ir_instr_operand_value(branch, owned)
        || !ir_instr_operand_block(branch, finalize)
        || !ir_instr_operand_block(branch, join)) {
        lower_internal(context, span, "could not test a finalized local");
        return false;
    }
    branch->span = span;
    if (n00bb_open_type(context, frame->type))
        n00bb_rewrite_later(context, branch);
    context->block = finalize;
    bool ok = n00bb_set_owned(context, frame, false, span);
    if (ok && frame->fields) {
        ir_local_t *local = ir_lower_bound_local(context, frame->symbol);
        ir_value_t *self = local ? ir_local_value(context->function, local)
            : ir_lower_bound_value(context, frame->symbol);
        ok = self && n00bb_emit_finalize_fields(context, self, span, 0);
    }
    else if (ok) {
        ir_value_t *address = frame->storage
            ? n00bb_storage_address(context, frame->storage, span)
            : n00bb_final_address(context, frame->symbol, span);
        ok = address && n00bb_emit_finalize_value(context, address,
                                                  frame->type, span, 0);
    }
    if (!ok || !n00bb_emit_jump(context, join, span)) {
        lower_internal(context, span, "could not finalize a local");
        return false;
    }
    context->block = join;
    return true;
}

bool
n00bb_begin_final_frame(ir_lower_context_t *context,
                        parse_sym_entry_t *symbol, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !context->reachable
        || (symbol->selected_storage_class.data
            && symbol->selected_storage_class.u8_bytes)) return true;
    bool parameter = symbol->kind == PARSE_SYM_PARAM;
    bool consumed = parameter && core_string_eq(symbol->direction,
                                                CORE_STRING_STATIC("consume"));
    bool finalizer_self = consumed && lower->function_symbol
        && core_string_eq(lower->function_symbol->name,
                          CORE_STRING_STATIC("__finalize"))
        && th_kind_of(th_resolve(symbol->type)) == TH_INDIRECTION;
    th_type *type = finalizer_self
        ? th_ind_referent(th_resolve(symbol->type)) : symbol->type;
    if ((parameter && !consumed) || !type
        || symbol->kind == PARSE_SYM_FUNCTION) return true;
    if (finalizer_self) {
        th_type *shape = th_fields_record(ir_module_types(context->module),
                                          type);
        bool any = false;
        for (size_t i = 0; shape && th_kind_of(shape) == TH_RECORD
             && i < th_record_len(shape); i++) {
            const th_member *member = th_record_member(shape, i);
            any |= member && member->type
                && n00bb_type_may_finalize(lower->unit, member->type);
        }
        if (!any) return true;
    }
    else if (!n00bb_type_may_finalize(lower->unit, type)) return true;
    parse_tree_t *scope = symbol->scope ? symbol->scope->node : nullptr;
    if (!scope || (!parameter
        && !nt_is(lower->grammar, scope, CORE_STRING_STATIC("body"))
        && !nt_is(lower->grammar, scope, CORE_STRING_STATIC("try-body"))
        && !nt_is(lower->grammar, scope,
                  CORE_STRING_STATIC("block-value-expr"))))
        return true;
    th_type *bool_type = n00bb_ir_builtin_type(context->module,
                                               CORE_STRING_STATIC("bool"));
    char buffer[48];
    core_string_t name = n00bb_format_name(buffer, sizeof(buffer),
        "__n00bb_owned_%llu", (unsigned long long)context->next_value_id++);
    ir_local_t *owned = bool_type
        ? ir_local_add(context->function, name, bool_type) : nullptr;
    n00bb_final_frame_t frame = {
        .scope = scope,
        .symbol = symbol,
        .owned = owned,
        .type = type,
        .fields = finalizer_self,
        .order = ++lower->next_control_order,
    };
    ir_value_t *binding = ir_lower_bound_address(context, symbol);
    ir_local_t *local = binding ? nullptr
        : ir_lower_bound_local(context, symbol);
    if (!binding) binding = local ? ir_local_value(context->function, local)
                                  : ir_lower_bound_value(context, symbol);
    ir_annots_t *annots = binding ? ir_value_annots(binding) : nullptr;
    if (!owned || !annots || !core_list_try_push(lower->finals, frame)
        || !ir_annot_set(lower->ir_context, annots, (ir_annot_t){
                .key = CORE_STRING_STATIC("n00bb.finalize_owned"),
                .kind = IR_ANNOT_VALUE,
                .v = ir_local_value(context->function, owned),
            })
        || !n00bb_set_owned(context,
            &lower->finals.data[lower->finals.len - 1], parameter, span)) {
        lower_internal(context, span, "could not track a finalized local");
        return false;
    }
    return true;
}

bool
n00bb_finish_final_scope(ir_lower_context_t *context,
                         parse_tree_t *scope, parse_tree_t *source)
{
    n00bb_lower_context_t *lower = context->user;
    for (size_t i = lower ? lower->finals.len : 0;
         scope && i > lower->final_base; i--) {
        if (lower->finals.data[i - 1].scope != scope) continue;
        if (context->reachable && !lower->finals.data[i - 1].moved
            && !n00bb_emit_scope_finalize(context, &lower->finals.data[i - 1],
                                          source))
            return false;
        for (size_t j = i; j < lower->finals.len; j++)
            lower->finals.data[j - 1] = lower->finals.data[j];
        lower->finals.len--;
    }
    return true;
}

bool
n00bb_lower_statement_scope(ir_lower_context_t *context, parse_visit_t *visit,
                            bool entering, void *state)
{
    (void)state;
    return entering
        || n00bb_finish_final_scope(context, visit->node, visit->node);
}

bool
n00bb_move_source(ir_lower_context_t *context, parse_grammar_t *grammar,
                  parse_tree_t *source)
{
    n00bb_lower_context_t *lower = context->user;
    if (!source || !lower || !context->reachable) return true;
    parse_tree_t *lvalue = n00bb_simple_lvalue(source);
    n00bb_final_frame_t *frame = lvalue
        ? n00bb_final_frame(lower, attached_symbol(grammar, lower->unit,
                                                   lvalue))
        : nullptr;
    ir_span_t span = lower_span_for_tree(context, source);
    if (!frame) {
        th_type *type = n00bb_node_type(context, source);
        parse_tree_t *member = member_access_node(grammar, source);
        parse_tree_t *index = n00bb_index_expression_node(grammar, source);
        core_string_t whole = source_text_of(lower->unit, source);
        bool call = whole.u8_bytes
            && whole.data[whole.u8_bytes - 1] == ')';
        bool place = lvalue
            || (member && core_string_eq(source_text_of(lower->unit, member),
                                         whole))
            || (index && core_string_eq(source_text_of(lower->unit, index),
                                        whole));
        if (!place || call) return true;
        if (!n00bb_type_finalized(lower->unit, type)) {
            ir_value_t *copied = n00bb_type_may_finalize(lower->unit, type)
                ? ir_lower_node_value(context, source) : nullptr;
            ir_instr_t *def = copied && copied->kind == IR_VAL_INSTR
                ? copied->def : nullptr;
            return !def || ir_annot_set(lower->ir_context, &def->annots,
                (ir_annot_t){
                    .key = CORE_STRING_STATIC("n00bb.place_copy"),
                    .kind = IR_ANNOT_STR,
                    .s = source_text_of(lower->unit, source)});
        }
        core_string_t type_text = lower_type_text(context, type);
        lower_report(context, N00B_DIAG_FINALIZED_COPY, span,
            N00B_TEXT_FINALIZED_VALUE_COPIED,
            N00BB_ARGS(ARG_TEXT(type_text),
                       ARG_TEXT(source_text_of(lower->unit, source))));
        lower_help(context, N00B_TEXT_MOVE_THE_WHOLE_VARIABLE,
                   N00BB_NO_ARGS);
        core_free(type_text.data);
        return false;
    }
    if (ir_lower_bound_address(context, frame->symbol))
        return n00bb_set_owned(context, frame, false, span);
    ir_local_t *local = ir_lower_bound_local(context, frame->symbol);
    ir_value_t *binding = local
        ? ir_local_value(context->function, local)
        : ir_lower_bound_value(context, frame->symbol);
    ir_instr_t *consume = binding
        ? ir_lower_emit_instruction(context, IR_OP_CONSUME, span) : nullptr;
    if (!consume || !ir_lower_set_temp_dest(context, consume,
            CORE_STRING_STATIC("__n00bb_moved_"), binding->type)
        || !ir_instr_operand_value(consume, binding)
        || !ir_annot_set(lower->ir_context, &consume->annots, (ir_annot_t){
                .key = CORE_STRING_STATIC("n00bb.move"),
                .kind = IR_ANNOT_BOOL, .b = true})
        || !n00bb_set_owned(context, frame, false, span)) {
        lower_internal(context, span, "could not move a finalized local");
        return false;
    }
    return true;
}

bool
n00bb_write_binding(ir_lower_context_t *context, parse_sym_entry_t *symbol,
                    bool written)
{
    n00bb_final_frame_t *frame = n00bb_final_frame(context->user, symbol);
    if (!frame || !context->reachable) return true;
    if (!written)
        return n00bb_emit_scope_finalize(context, frame, symbol->decl_node);
    return n00bb_set_owned(context, frame, true,
                           lower_span_for_tree(context, symbol->decl_node));
}

static bool
n00bb_mark_open_storage(ir_lower_context_t *context, ir_instr_t *allocation,
                        ir_local_t *local, core_string_t storage_class)
{
    if ((storage_class.data && storage_class.u8_bytes)
        || !n00bb_open_type(context, local->type)) return true;
    n00bb_lower_context_t *lower = context->user;
    if (!lower->unit) return true;
    core_dict_put(&tu_state(lower->unit)->open_storage, allocation, local);
    n00bb_rewrite_later(context, allocation);
    return true;
}

bool
n00bb_bind_local(ir_lower_context_t *context, parse_sym_entry_t *symbol,
                 ir_local_t *local, ir_span_t span)
{
    if (!context || !symbol || !local) return false;
    if (!ir_span_known(local->span))
        local->span = lower_symbol_name_span(context, symbol);
    core_string_t storage_class = symbol->selected_storage_class;
    bool dynamic = symbol->storage_duration == PARSE_SYM_STORAGE_DYNAMIC;
    if ((!storage_class.data || !storage_class.u8_bytes) && !dynamic) {
        return ir_lower_bind_local(context, symbol, local)
            && n00bb_mark_binding_reentry(context, symbol)
            && n00bb_begin_final_frame(context, symbol, span);
    }
    parse_sym_set_storage_duration(symbol, PARSE_SYM_STORAGE_DYNAMIC);
    th_type *address_type = th_ind(ir_module_types(context->module), 0,
                                   local->type);
    ir_instr_t *allocation = address_type
        ? ir_lower_emit_instruction(context, IR_OP_ALLOC, span) : nullptr;
    return allocation
        && ir_lower_set_temp_dest(context, allocation, CORE_STRING_STATIC("__n00bb_storage_"),
                                  address_type)
        && ir_instr_operand_type(allocation, local->type)
        && ((!storage_class.data || !storage_class.u8_bytes)
            || ir_instr_set_storage_class(allocation, storage_class,
                                          symbol->storage_class_required))
        && ir_lower_bind_local_storage(context, symbol, local,
                                       allocation->dest)
        && n00bb_mark_open_storage(context, allocation, local, storage_class)
        && n00bb_mark_binding_reentry(context, symbol)
        && n00bb_begin_final_frame(context, symbol, span);
}

static bool
moves_operand(const ir_instr_t *in, uint32_t operand)
{
    switch (in->op) {
    case IR_OP_COPY:
    case IR_OP_STORE:
    case IR_OP_GLOBAL_STORE:
    case IR_OP_RETURN:
    case IR_OP_AGGREGATE:
    case IR_OP_PHI:
    case IR_OP_SELECT:
    case IR_OP_CONSUME:
    case IR_OP_BOX:
    case IR_OP_OPTION_SOME:
        return true;
    case IR_OP_CALL:
    case IR_OP_CALLBACK_INVOKE: {
        th_type *signature = operand ? ir_instr_call_signature(in) : nullptr;
        signature = signature ? th_resolve(signature) : nullptr;
        const th_param *formal = signature
                && th_kind_of(signature) == TH_FUNCTION
                && operand - 1 < th_func_nparams(signature)
            ? th_func_param(signature, operand - 1) : nullptr;
        return operand && (!formal || formal->mode == TH_CONSUME
                           || th_mode_produces_output(formal->mode));
    }
    default:
        return false;
    }
}

static ir_instr_t *
temporary_end(ir_func_t *function, ir_instr_t *producer)
{
    ir_instr_t *last = producer;
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = function->blocks[b]->instrs[i];
            for (uint32_t o = 0; o < in->n_operands; o++) {
                if (ir_operand_value(&in->operands[o]) != producer->dest)
                    continue;
                if (in->block != producer->block || moves_operand(in, o))
                    return nullptr;
                if (in->index > last->index) last = in;
            }
        }
    return ir_op_is_terminator(last->op) ? nullptr : last;
}

bool
n00bb_finalize_temporaries(n00bb_link_analysis_t *analysis,
                           parse_grammar_t *grammar,
                           core_translation_unit_t *unit, ir_func_t *function)
{
    th_ctx *types = ir_module_types(function->owner);
    core_list_t(n00bb_ir_instr_ptr_t) producers =
        core_list_new(n00bb_ir_instr_ptr_t);
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = function->blocks[b]->instrs[i];
            th_type *type = (in->op == IR_OP_CALL
                             || in->op == IR_OP_AGGREGATE)
                    && in->dest_kind == IR_DEST_VALUE && in->dest
                ? th_resolve(in->dest->type) : nullptr;
            if (!type || !th_is_closed(types, type) || th_option_item(type)
                || th_tuple_element(type, 0, nullptr)
                || !n00bb_type_finalized(unit, type)) continue;
            core_list_push(producers, in);
        }
    if (!producers.len) {
        core_list_free(producers);
        return true;
    }
    n00bb_lower_context_t *lower = n00bb_solved_lower_new(analysis, grammar,
                                                          unit);
    ir_block_t *scratch = lower ? ir_block_add(function,
        CORE_STRING_STATIC("__n00bb_temporaries")) : nullptr;
    ir_lower_context_t context = {
        .module = function->owner, .function = function, .block = scratch,
        .reachable = true, .user = lower,
    };
    bool ok = scratch != nullptr;
    for (size_t p = 0; ok && p < producers.len; p++) {
        ir_instr_t *producer = producers.data[p];
        ir_instr_t *last = temporary_end(function, producer);
        if (!last) continue;
        context.block = scratch;
        ok = n00bb_finalize_copy(&context, producer->dest,
                                 producer->dest->type, producer->span, 0);
        for (uint32_t at = last->index + 1; ok && scratch->n_instrs; at++)
            ok = ir_instr_move(scratch->instrs[0], last->block, at);
    }
    ok = (!scratch || ir_block_erase(scratch)) && ok;
    core_free(lower);
    core_list_free(producers);
    return ok;
}
