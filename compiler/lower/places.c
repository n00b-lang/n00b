#include "lower_internal.h"

static bool
n00bb_referent_written(parse_grammar_t *grammar, parse_tree_t *node);
static bool
n00bb_mark_reference_address(ir_lower_context_t *context, ir_value_t *address,
                             bool writable);
static ir_value_t *
n00bb_custom_reference_address(ir_lower_context_t *context,
                               parse_grammar_t *grammar,
                               parse_tree_t *referent, ir_value_t *reference,
                               ir_span_t span);
static ir_value_t *
n00bb_open_reference_address(ir_lower_context_t *context,
                             parse_grammar_t *grammar,
                             parse_tree_t *referent, ir_value_t *reference,
                             ir_span_t span);

ir_value_t *
n00bb_reference_view(ir_lower_context_t *context, ir_value_t *value,
                     ir_span_t span)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    th_type *definition = value && lower
        ? n00bb_reference_definition(ir_module_types(context->module),
                                     value->type) : nullptr;
    if (!definition) return value;
    ir_instr_t *conversion = ir_lower_emit_instruction(context,
        IR_OP_NOMINAL_CAST, span);
    if (!conversion || !ir_lower_set_temp_dest(context, conversion,
                                               CORE_STRING_STATIC("__n00bb_"), definition)
        || !ir_instr_operand_value(conversion, value)
        || !n00bb_set_reference_access(lower->ir_context, conversion->dest,
                                       definition)) return nullptr;
    return conversion->dest;
}

ir_value_t *
n00bb_emit_hook_call(ir_lower_context_t *context,
                     const th_env_method_t *method, ir_value_t *argument,
                     ir_span_t span)
{
    ir_module_t *owner = nullptr;
    ir_func_t *function = n00bb_record_field_method(context, method, &owner);
    ir_value_t *callee = owner && function
        ? ir_func_address(owner, function) : nullptr;
    th_type *signature = callee ? method->signature : nullptr;
    th_ctx *types = ir_module_types(context->module);
    if (signature && !th_is_closed(types, signature)) {
        th_type *instance = th_instantiate(types,
                                           th_generalize(types, signature));
        const th_param *self = instance
            && th_kind_of(instance) == TH_FUNCTION
            && th_func_nparams(instance) ? th_func_param(instance, 0)
                                         : nullptr;
        th_type *concrete = self && th_match(self->type, argument->type)
            ? th_apply_subst(types, instance) : nullptr;
        if (concrete) signature = concrete;
    }
    th_type *result = signature ? th_func_ret(signature) : nullptr;
    ir_instr_t *call = result
        ? ir_lower_emit_instruction(context, IR_OP_CALL, span) : nullptr;
    return call && ir_lower_set_temp_dest(context, call,
            CORE_STRING_STATIC("__n00bb_hook_"), result)
        && ir_instr_operand_value(call, callee)
        && ir_instr_operand_value(call, argument)
        && ir_instr_set_call_signature(call, signature) ? call->dest : nullptr;
}

static bool
n00bb_referent_written(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (n00bb_reference_target(grammar, node)) return true;
    parse_tree_t *at = node;
    for (parse_tree_t *parent = parse_tree_parent(at); parent;
         at = parent, parent = parse_tree_parent(at)) {
        if (nt_is(grammar, parent, CORE_STRING_STATIC("assign-stmt"))
            || nt_is(grammar, parent, CORE_STRING_STATIC("binop-assign-stmt")))
            return core_tree_child(parent, 0) == at;
        bool receiver = n00bb_postfix_node(grammar, parent)
            && core_tree_child(parent, 0) == at;
        if (!receiver && core_tree_num_children(parent) != 1) return false;
    }
    return false;
}

static bool
n00bb_mark_reference_address(ir_lower_context_t *context, ir_value_t *address,
                             bool writable)
{
    n00bb_lower_context_t *lower = context->user;
    return address && ir_value_set_indirect_access(lower->ir_context, address,
            writable ? IR_INDIRECT_ACCESS_READ_WRITE
                     : IR_INDIRECT_ACCESS_READ)
        && ir_annot_set(lower->ir_context, ir_value_annots(address),
            (ir_annot_t){.key = CORE_STRING_STATIC("n00bb.reference_address"),
                         .kind = IR_ANNOT_BOOL, .b = true});
}

ir_value_t *
n00bb_reference_hook_address(ir_lower_context_t *context,
                             ir_value_t *reference, bool writable,
                             ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
    th_type *type = reference ? th_resolve(reference->type) : nullptr;
    if (!state || !type || th_kind_of(type) != TH_NAMED) return nullptr;
    th_env_method_t method = {0};
    if (!n00bb_reference_method(state->types, type, writable, &method,
                                nullptr)) {
        if (!writable || !n00bb_reference_method(state->types, type, false,
                                                 &method, nullptr))
            return nullptr;
        core_string_t text = lower_type_text(context, type);
        lower_report(context, N00B_DIAG_NOT_WRITABLE, span,
            N00B_TEXT_READ_ONLY_REFERENCE, N00BB_ARGS(ARG_TEXT(text)));
        core_free(text.data);
        if (!lower_mark_failed(context)) return nullptr;
        writable = false;
    }
    ir_value_t *address = n00bb_emit_hook_call(context, &method, reference,
                                               span);
    return n00bb_mark_reference_address(context, address, writable)
        ? address : nullptr;
}

static ir_value_t *
n00bb_custom_reference_address(ir_lower_context_t *context,
                               parse_grammar_t *grammar,
                               parse_tree_t *referent, ir_value_t *reference,
                               ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    return n00bb_reference_hook_address(context, reference,
        lower->reference_write || n00bb_referent_written(grammar, referent),
        span);
}

ir_value_t *
n00bb_reference_placeholder(ir_lower_context_t *context,
                            ir_value_t *reference, th_type *address_type,
                            bool writable, ir_span_t span)
{
    th_ctx *types = ir_module_types(context->module);
    th_type *nil_type = th_name(types, CORE_STRING_STATIC("nil"));
    th_param parameter = {.mode = TH_IN, .type = reference->type};
    th_type *signature = address_type
        ? th_func_with_error(types, address_type, nil_type, &parameter, 1,
                             nullptr, nullptr, 0) : nullptr;
    char buffer[80];
    core_string_t name = n00bb_format_name(buffer, sizeof(buffer),
        writable ? "__n00bb_ref_address_mut_open#%016llx"
                 : "__n00bb_ref_address_open#%016llx",
        (unsigned long long)(signature ? th_hash(types, signature) : 0));
    ir_func_t *placeholder = signature
        ? ir_func_find(context->module, name) : nullptr;
    if (!placeholder && signature)
        placeholder = ir_func_add(context->module, name, signature,
                                  IR_LINK_EXTERN);
    ir_value_t *callee = placeholder
        ? ir_func_address(context->module, placeholder) : nullptr;
    ir_instr_t *call = callee
        ? ir_lower_emit_instruction(context, IR_OP_CALL, span) : nullptr;
    return call && ir_lower_set_temp_dest(context, call,
            CORE_STRING_STATIC("__n00bb_hook_"), address_type)
        && ir_instr_operand_value(call, callee)
        && ir_instr_operand_value(call, reference)
        && ir_instr_set_call_signature(call, signature)
        && n00bb_mark_reference_address(context, call->dest, writable)
        ? call->dest : nullptr;
}

static ir_value_t *
n00bb_open_reference_address(ir_lower_context_t *context,
                             parse_grammar_t *grammar,
                             parse_tree_t *referent, ir_value_t *reference,
                             ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    th_ctx *types = ir_module_types(context->module);
    th_type *target = n00bb_node_type(context, referent);
    th_type *address_type = th_ind(types, 0, target ? target : th_var(types));
    bool writable = lower->reference_write
        || n00bb_referent_written(grammar, referent);
    if (lower->solved)
        return n00bb_reference_placeholder(context, reference, address_type,
                                           writable, span);
    ir_value_t *address = n00bb_dispatch(context, writable
            ? CORE_STRING_STATIC("__ref_address_mut")
            : CORE_STRING_STATIC("__ref_address"),
        &reference, 1, address_type, span);
    return n00bb_mark_reference_address(context, address, writable)
        ? address : nullptr;
}

bool
n00bb_from_reference_address(ir_value_t *value)
{
    for (size_t depth = 0; value && depth < 64; depth++) {
        if (ir_annot_get(ir_value_annots(value),
                CORE_STRING_STATIC("n00bb.reference_address"))) return true;
        ir_instr_t *def = value->kind == IR_VAL_INSTR ? value->def : nullptr;
        bool step = def && def->n_operands
            && (def->op == IR_OP_FIELD || def->op == IR_OP_INDEX
                || def->op == IR_OP_COPY || def->op == IR_OP_CAST
                || def->op == IR_OP_NOMINAL_CAST || def->op == IR_OP_OFFSET);
        value = step ? ir_operand_value(&def->operands[0]) : nullptr;
    }
    return false;
}

bool
n00bb_call_argument(parse_grammar_t *grammar, parse_tree_t *node)
{
    for (parse_tree_t *at = node; at; at = parse_tree_parent(at)) {
        parse_tree_t *parent = parse_tree_parent(at);
        if (!parent) return false;
        if (nt_is(grammar, at, CORE_STRING_STATIC("expression")))
            return nt_is(grammar, parent, CORE_STRING_STATIC("positional-args"))
                || nt_is(grammar, parent, CORE_STRING_STATIC("keyword-arg"));
        if (core_tree_num_children(parent) != 1) return false;
    }
    return false;
}

ir_value_t *
n00bb_record_field_address(ir_lower_context_t *context,
                           parse_grammar_t *grammar,
                           core_translation_unit_t *unit,
                           parse_tree_t *node, ir_span_t span)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    if (!context || !lower || !unit || !node) return nullptr;
    parse_tree_t *referent = referent_access_node(grammar, node);
    if (referent) {
        parse_tree_t *receiver = core_tree_child(referent, 0);
        ir_value_t *pointer = nullptr;
        for (parse_tree_t *at = receiver; at && !core_tree_is_leaf(at);) {
            pointer = ir_lower_node_value(context, at);
            if (pointer) break;
            if (nt_is(grammar, at, CORE_STRING_STATIC("tuple-or-paren"))
                && core_tree_num_children(at) == 3)
                at = core_tree_child(at, 1);
            else if (core_tree_num_children(at) == 1)
                at = core_tree_child(at, 0);
            else
                break;
        }
        if (pointer && (th_kind_of(th_resolve(pointer->type)) == TH_VAR
                        || th_option_item(th_resolve(pointer->type))))
            return n00bb_open_reference_address(context, grammar, referent,
                                                pointer, span);
        if (pointer && th_kind_of(th_resolve(pointer->type)) == TH_NAMED)
            return n00bb_custom_reference_address(context, grammar,
                                                  referent, pointer, span);
        parse_sym_entry_t *symbol = attached_symbol(grammar, unit, receiver);
        if ((!pointer || th_kind_of(th_resolve(pointer->type))
                != TH_INDIRECTION) && symbol)
            pointer = ir_lower_bound_value(context, symbol);
        if (pointer && pointer->kind == IR_VAL_GLOBAL) pointer = nullptr;
        if (pointer && th_option_item(th_resolve(pointer->type))) {
            ir_local_t *local = symbol
                ? ir_lower_bound_local(context, symbol) : nullptr;
            ir_value_t *read = local
                ? n00bb_emit_local_read(context, local, span) : pointer;
            return read ? n00bb_open_reference_address(context, grammar,
                                                       referent, read, span)
                        : nullptr;
        }
        if (pointer && th_kind_of(th_resolve(pointer->type)) == TH_INDIRECTION)
            return pointer;
        if (pointer && th_kind_of(th_resolve(pointer->type)) == TH_NAMED)
            return n00bb_custom_reference_address(context, grammar,
                                                  referent, pointer, span);
        ir_value_t *storage = n00bb_addressable_expression_address(context,
            grammar, unit, receiver, span);
        th_type *stored = storage
            ? th_resolve(th_ind_referent(th_resolve(storage->type))) : nullptr;
        if (stored && (th_kind_of(stored) == TH_NAMED
                       || th_kind_of(stored) == TH_VAR
                       || th_option_item(stored))) {
            ir_value_t *reference = n00bb_emit_indirect_read(context, storage,
                                                             stored, span);
            if (reference && (th_kind_of(stored) == TH_VAR
                              || th_option_item(stored)))
                return n00bb_open_reference_address(context, grammar,
                    referent, reference, span);
            return reference ? n00bb_custom_reference_address(context,
                grammar, referent, reference, span) : nullptr;
        }
        return stored && th_kind_of(stored) == TH_INDIRECTION
            ? n00bb_emit_indirect_read(context, storage, stored, span)
            : nullptr;
    }
    parse_tree_t *member = member_access_node(grammar, node);
    if (member) {
        parse_tree_t *base_node = core_tree_num_children(member)
            ? core_tree_child(member, 0) : nullptr;
        parse_sym_entry_t *field = attached_symbol(grammar, unit, member);
        ir_value_t *base = n00bb_reference_view(context,
            ir_lower_node_value(context, base_node), span);
        parse_sym_entry_t *base_symbol = attached_symbol(grammar, unit,
                                                         base_node);
        ir_value_t *binding = base_symbol
            ? ir_lower_bound_value(context, base_symbol) : nullptr;
        if (binding && binding->type
            && th_kind_of(th_resolve(binding->type)) == TH_INDIRECTION)
            base = binding;
        if (!base || th_kind_of(th_resolve(base->type)) != TH_INDIRECTION)
            base = n00bb_addressable_expression_address(
                context, grammar, unit, base_node, span);
        while (base && th_kind_of(th_resolve(base->type)) == TH_INDIRECTION) {
            th_type *referent = th_resolve(th_ind_referent(base->type));
            bool nominal = referent && n00bb_reference_definition(
                ir_module_types(context->module), referent);
            if (!referent || (th_kind_of(referent) != TH_INDIRECTION
                              && !nominal)) break;
            ir_instr_t *load = ir_lower_emit_instruction(context, IR_OP_LOAD,
                                                          span);
            if (!load || !ir_lower_set_temp_dest(context, load,
                CORE_STRING_STATIC("__n00bb_deref_"), referent)
                || !ir_instr_operand_value(load, base)
                || !n00bb_set_reference_access(lower->ir_context,
                    load->dest, referent)) return nullptr;
            base = nominal ? n00bb_reference_view(context, load->dest, span)
                           : load->dest;
        }
        if ((field && (!field->is_field || !field->type)) || !base
            || th_option_item(th_ind_referent(th_resolve(base->type))))
            return nullptr;
        core_string_t field_name = field ? field->name
            : first_token_text(core_tree_child(member,
                  core_tree_num_children(member) - 1));
        th_type *member_type = n00bb_node_type(context, member);
        if (!member_type)
            member_type = th_var(ir_module_types(context->module));
        th_type *pointer_type = th_ind(ir_module_types(context->module), 0,
                                       member_type);
        ir_instr_t *address = pointer_type
            ? ir_lower_emit_instruction(context, IR_OP_FIELD, span) : nullptr;
        if (!address || !ir_lower_set_temp_dest(context, address,
            CORE_STRING_STATIC("__n00bb_field_address_"), pointer_type)
            || !ir_instr_operand_value(address, base)
            || !ir_instr_operand_name(address, field_name))
            return nullptr;
        ir_shape_t *field_shape = field ? parse_sym_binding(field,
            CORE_STRING_STATIC("ir.array-shape")) : nullptr;
        if (field_shape && !ir_value_set_shape(address->dest, field_shape))
            return nullptr;
        if (n00bb_open_type(context, th_ind_referent(th_resolve(base->type))))
            n00bb_rewrite_later(context, address);
        return address->dest;
    }

    parse_sym_entry_t *symbol = attached_symbol(grammar, unit, node);
    ir_value_t *address = symbol
        ? ir_lower_bound_address(context, symbol) : nullptr;
    ir_local_t *local = symbol
        ? ir_lower_bound_local(context, symbol) : nullptr;
    if (!address && local) address = ir_lower_local_address(context, local);
    if (address) return address;
    if (local) {
        th_type *pointer_type = th_ind(
            ir_module_types(context->module), 0, local->type);
        ir_instr_t *take = pointer_type
            ? ir_lower_emit_instruction(context, IR_OP_ADDRESS_OF, span)
            : nullptr;
        return take && ir_lower_set_temp_dest(context, take,
            CORE_STRING_STATIC("__n00bb_local_address_"), pointer_type)
            && ir_instr_operand_local(take, local) ? take->dest : nullptr;
    }
    ir_value_t *binding = symbol
        ? ir_lower_bound_value(context, symbol) : nullptr;
    if (binding && binding->kind == IR_VAL_GLOBAL && binding->global) {
        th_type *pointer_type = th_ind(
            ir_module_types(context->module), 0, binding->type);
        ir_instr_t *take = pointer_type
            ? ir_lower_emit_instruction(context, IR_OP_ADDRESS_OF, span)
            : nullptr;
        return take && ir_lower_set_temp_dest(context, take,
            CORE_STRING_STATIC("__n00bb_global_address_"), pointer_type)
            && ir_instr_operand_value(take, binding) ? take->dest : nullptr;
    }
    if (binding && th_kind_of(th_resolve(binding->type)) == TH_INDIRECTION)
        return binding;
    if (binding && symbol && symbol->type
        && (binding->kind == IR_VAL_PARAM
            || binding->kind == IR_VAL_GLOBAL)) {
        th_type *pointer_type = th_ind(
            ir_module_types(context->module), 0, symbol->type);
        ir_instr_t *take = pointer_type
            ? ir_lower_emit_instruction(context, IR_OP_ADDRESS_OF, span)
            : nullptr;
        return take && ir_lower_set_temp_dest(context, take,
            CORE_STRING_STATIC("__n00bb_value_address_"), pointer_type)
            && ir_instr_operand_value(take, binding) ? take->dest : nullptr;
    }
    return nullptr;
}

ir_value_t *
n00bb_index_writeback_place(ir_lower_context_t *context,
                            parse_grammar_t *grammar,
                            core_translation_unit_t *unit, parse_tree_t *node,
                            n00bb_index_writeback_t *writeback, ir_span_t span)
{
    parse_tree_t *index = n00bb_index_expression_node(grammar, node);
    if (index) {
        ir_value_t *element = ir_lower_node_value(context, index);
        th_type *element_type = element ? element->type : nullptr;
        char name_buffer[48];
        core_string_t name = n00bb_format_name(name_buffer,
            sizeof(name_buffer), "__n00bb_index_element_%llu",
            (unsigned long long)context->next_value_id++);
        ir_local_t *local = element
            ? ir_local_add(context->function, name, element_type) : nullptr;
        if (!local || !n00bb_emit_local_write(context, local, element, span))
            return nullptr;
        writeback->index_node = index;
        writeback->element = local;
        th_type *pointer_type = th_ind(ir_module_types(context->module), 0,
                                       element_type);
        ir_instr_t *take = pointer_type
            ? ir_lower_emit_instruction(context, IR_OP_ADDRESS_OF, span)
            : nullptr;
        return take && ir_lower_set_temp_dest(context, take,
                CORE_STRING_STATIC("__n00bb_element_address_"), pointer_type)
            && ir_instr_operand_local(take, local) ? take->dest : nullptr;
    }
    parse_tree_t *member = member_access_node(grammar, node);
    size_t children = member ? core_tree_num_children(member) : 0;
    if (children < 3) return nullptr;
    ir_value_t *base = n00bb_index_writeback_place(context, grammar, unit,
        core_tree_child(member, 0), writeback, span);
    th_ctx *types = ir_module_types(context->module);
    ir_instr_t *address = base
        ? ir_lower_emit_instruction(context, IR_OP_FIELD, span) : nullptr;
    return address && ir_lower_set_temp_dest(context, address,
            CORE_STRING_STATIC("__n00bb_field_address_"),
            th_ind(types, 0, th_var(types)))
        && ir_instr_operand_value(address, base)
        && ir_instr_operand_name(address,
               first_token_text(core_tree_child(member, children - 1)))
        ? address->dest : nullptr;
}

bool
n00bb_index_writeback_finish(ir_lower_context_t *context,
                             parse_grammar_t *grammar, parse_tree_t *site,
                             const n00bb_index_writeback_t *writeback,
                             ir_span_t span)
{
    if (!writeback->index_node) return true;
    ir_value_t *updated = n00bb_emit_local_read(context, writeback->element,
                                                span);
    return updated && n00bb_lower_index_store_at(context, grammar, site,
        writeback->index_node, nullptr, updated);
}

ir_value_t *
n00bb_addressable_expression_address(ir_lower_context_t *context,
                                     parse_grammar_t *grammar,
                                     core_translation_unit_t *unit,
                                     parse_tree_t *node, ir_span_t span)
{
    if (!context || !grammar || !unit || !node) return nullptr;
    n00bb_lower_context_t *lower = context->user;
    parse_tree_t *member = member_access_node(grammar, node);
    parse_sym_entry_t *resolved = attached_symbol(grammar, unit, node);
    if (member && resolved && !resolved->is_field) {
        ir_value_t *binding = ir_lower_bound_value(context, resolved);
        ir_value_t *address = ir_lower_bound_address(context, resolved);
        ir_local_t *local = ir_lower_bound_local(context, resolved);
        th_type *type = local ? local->type
            : binding ? binding->type : resolved->type;
        if (address) return address;
        if (!local && (!binding
            || (binding->kind != IR_VAL_PARAM
                && binding->kind != IR_VAL_GLOBAL))) return nullptr;
        th_type *address_type = type
            ? th_ind(ir_module_types(context->module), 0, type) : nullptr;
        ir_instr_t *take = address_type
            ? ir_lower_emit_instruction(context, IR_OP_ADDRESS_OF, span)
            : nullptr;
        return take && ir_lower_set_temp_dest(context, take,
            CORE_STRING_STATIC("__n00bb_member_address_"), address_type)
            && (local ? ir_instr_operand_local(take, local)
                      : binding && ir_instr_operand_value(take, binding))
            ? take->dest : nullptr;
    }
    if (member)
        return n00bb_record_field_address(context, grammar, unit, member,
                                           span);

    parse_tree_t *index = n00bb_index_expression_node(grammar, node);
    if (index) {
        parse_tree_t *receiver = core_tree_num_children(index)
            ? core_tree_child(index, 0) : nullptr;
        parse_tree_t *contents = core_tree_num_children(index) > 2
            ? core_tree_child(index, 2) : nullptr;
        parse_tree_t *subscript = child_nt(grammar, contents, CORE_STRING_STATIC("expression"));
        ir_value_t *base = n00bb_addressable_expression_address(
            context, grammar, unit, receiver, span);
        ir_value_t *offset = subscript
            ? ir_lower_node_value(context, subscript) : nullptr;
        th_type *container = base
            ? th_ind_referent(th_resolve(base->type)) : nullptr;
        container = container ? th_resolve(container) : nullptr;
        uint8_t address_attrs = 0;
        bool open = container && th_kind_of(container) == TH_VAR;
        if (open && lower && lower->write_back_open_index) return nullptr;
        if (!container || (!open && th_kind_of(container) != TH_SEQUENCE
                && (!th_ind_attrs(base->type, &address_attrs)
                    || !(address_attrs & TH_INDEXABLE))))
            return nullptr;
        th_type *element = open ? th_var(ir_module_types(context->module))
            : th_kind_of(container) == TH_SEQUENCE ? th_seq_elem(container)
            : n00bb_node_type(context, index);
        th_type *address_type = element
            ? th_ind(ir_module_types(context->module), 0, element) : nullptr;
        ir_instr_t *address = base && offset && address_type
            ? ir_lower_emit_instruction(context, IR_OP_INDEX, span) : nullptr;
        if (!address || !ir_lower_set_temp_dest(context, address,
            CORE_STRING_STATIC("__n00bb_index_address_"), address_type)
            || !ir_instr_operand_value(address, base)
            || !ir_instr_operand_value(address, offset)) return nullptr;
        if (lower && element
            && th_kind_of(th_resolve(element)) == TH_SEQUENCE) {
            ir_shape_t *base_shape = ir_value_shape(base);
            ir_shape_t *suffix = base_shape
                ? ir_shape_suffix(lower->ir_context, base_shape, 1) : nullptr;
            if (base_shape && (!suffix
                    || !ir_value_set_shape(address->dest, suffix)))
                return nullptr;
        }
        return address->dest;
    }
    return n00bb_record_field_address(context, grammar, unit, node, span);
}
