#include "lower_internal.h"

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
    (void)grammar;
    (void)referent;
    (void)lower;
    return n00bb_reference_hook_address(context, reference, false, span);
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
    bool writable = false;
    (void)grammar;
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


ir_value_t *
n00bb_referent_address(ir_lower_context_t *context, parse_grammar_t *grammar,
                       parse_tree_t *node, ir_span_t span)
{
    parse_tree_t *referent = referent_access_node(grammar, node);
    parse_tree_t *receiver = referent ? core_tree_child(referent, 0)
                                      : nullptr;
    ir_value_t *pointer = receiver ? ir_lower_node_value(context, receiver)
                                   : nullptr;
    if (pointer && pointer->kind == IR_VAL_GLOBAL) {
        ir_instr_t *read = ir_lower_emit_instruction(context, IR_OP_COPY,
                                                     span);
        pointer = read && ir_lower_set_temp_dest(context, read,
                CORE_STRING_STATIC("__n00bb_"), pointer->type)
            && ir_instr_operand_value(read, pointer) ? read->dest : nullptr;
    }
    th_type *held = pointer ? th_resolve(pointer->type) : nullptr;
    if (!held) return nullptr;
    if (th_kind_of(held) == TH_VAR || th_option_item(held))
        return n00bb_open_reference_address(context, grammar, referent,
                                            pointer, span);
    if (th_kind_of(held) == TH_NAMED)
        return n00bb_custom_reference_address(context, grammar, referent,
                                              pointer, span);
    return th_kind_of(held) == TH_INDIRECTION ? pointer : nullptr;
}

static ir_value_t *
emit_address(ir_lower_context_t *context, ir_op_t op, th_type *referent,
             ir_value_t *base, ir_local_t *local, core_string_t member,
             ir_span_t span)
{
    th_type *type = referent
        ? th_ind(ir_module_types(context->module), 0, referent) : nullptr;
    ir_instr_t *in = type ? ir_lower_emit_instruction(context, op, span)
                          : nullptr;
    bool ok = in && ir_lower_set_temp_dest(context, in,
                                           CORE_STRING_STATIC("__n00bb_place_"),
                                           type)
        && (local ? ir_instr_operand_local(in, local)
                  : ir_instr_operand_value(in, base))
        && (!member.data || ir_instr_operand_name(in, member));
    return ok ? in->dest : nullptr;
}

static bool
element_read(ir_lower_context_t *context, const ir_instr_t *read);

void
n00bb_note_field_read(ir_lower_context_t *context, ir_value_t *value,
                      parse_tree_t *node)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    if (!lower || !value || !node) return;
    if (!lower->field_reads_ready) {
        core_dict_init(&lower->field_reads, nullptr, nullptr);
        lower->field_reads_ready = true;
    }
    core_dict_put(&lower->field_reads, value, node);
}

static parse_tree_t *
field_read_node(n00bb_lower_context_t *lower, const ir_value_t *value)
{
    return lower && lower->field_reads_ready && value
        ? core_dict_get(&lower->field_reads, (void *)value, nullptr)
        : nullptr;
}

void
n00bb_note_place_written(ir_lower_context_t *context, ir_value_t *target,
                         bool reads)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    for (size_t depth = 0; lower && lower->field_reads_ready && target
         && depth < 64; depth++) {
        parse_tree_t *node = field_read_node(lower, target);
        parse_node_facts_t *facts = node
            ? parse_tree_facts_for_update(node) : nullptr;
            if (facts) {
            facts->field_written = true;
            facts->field_write_reads |= reads;
        }
        ir_instr_t *step = target->kind == IR_VAL_INSTR ? target->def
                                                        : nullptr;
        if (!step) return;
        uint32_t base = 0;
        switch (step->op) {
        case IR_OP_EXTRACT: case IR_OP_LOAD: case IR_OP_FIELD:
        case IR_OP_INDEX: case IR_OP_SEQUENCE_GET: case IR_OP_NOMINAL_CAST:
            break;
        case IR_OP_DISPATCH: {
            ir_value_t *source = step->n_operands > 1
                ? ir_operand_value(&step->operands[1]) : nullptr;
            th_type *held = source ? th_resolve(source->type) : nullptr;
            bool reference = step->operands[0].kind == IR_OPD_NAME
                && core_string_starts_with(step->operands[0].name,
                                           CORE_STRING_STATIC("__ref"));
            if (!reference && !(element_read(context, step) && held
                                && th_kind_of(held) == TH_SEQUENCE))
                return;
            base = 1;
            break;
        }
        default:
            return;
        }
        target = step->n_operands > base
            ? ir_operand_value(&step->operands[base]) : nullptr;
    }
}

static void
drop_read(ir_lower_context_t *context, ir_value_t *value, ir_value_t *place)
{
    for (size_t depth = 0; value && value != place
         && value->kind == IR_VAL_INSTR && value->def && depth < 64; depth++) {
        ir_instr_t *read = value->def;
        bool pure = read->op == IR_OP_LOAD || read->op == IR_OP_COPY
            || read->op == IR_OP_EXTRACT || read->op == IR_OP_NOMINAL_CAST
            || read->op == IR_OP_FIELD || read->op == IR_OP_SEQUENCE_GET
            || (depth == 0 && element_read(context, read));
        if (!pure || ir_value_has_uses(context->function, value)) return;
        uint32_t from = read->op == IR_OP_DISPATCH ? 1 : 0;
        ir_value_t *source = read->n_operands > from
            ? ir_operand_value(&read->operands[from]) : nullptr;
        if (!ir_instr_erase(read)) return;
        value = source;
    }
}

static bool
element_read(ir_lower_context_t *context, const ir_instr_t *read)
{
    (void)context;
    return read && read->op == IR_OP_DISPATCH && read->n_operands == 3
        && read->operands[0].kind == IR_OPD_NAME
        && core_string_eq(read->operands[0].name, CORE_STRING_STATIC("__read"))
        && read->operands[1].kind == IR_OPD_VALUE
        && read->operands[2].kind == IR_OPD_VALUE;
}

bool
n00bb_accessed_element(const ir_value_t *value, ir_value_t **container,
                       ir_value_t **index)
{
    const ir_instr_t *read = value && value->kind == IR_VAL_INSTR
        ? value->def : nullptr;
    if (!element_read(nullptr, read)) return false;
    *container = ir_operand_value(&read->operands[1]);
    *index = ir_operand_value(&read->operands[2]);
    return true;
}

bool
n00bb_place_writable(const ir_value_t *place)
{
    for (size_t depth = 0; place && depth < 64; depth++) {
        const ir_instr_t *at = place->kind == IR_VAL_INSTR ? place->def
                                                            : nullptr;
        if (!at) return true;
        if (at->op == IR_OP_ALLOC)
            return !ir_annot_get((ir_annots_t *)&at->annots,
                                 N00BB_IMMUTABLE_STORAGE_KEY);
        if (at->op == IR_OP_ADDRESS_OF && at->n_operands == 1) {
            const ir_local_t *local = ir_operand_local(&at->operands[0]);
            const ir_value_t *storage = ir_operand_value(&at->operands[0]);
            if (local) return local->mutability == IR_MUT_MUTABLE;
            if (storage && storage->kind == IR_VAL_PARAM) return false;
            return !storage || !storage->global
                || storage->global->mutability == IR_MUT_MUTABLE;
        }
        if ((at->op != IR_OP_FIELD && at->op != IR_OP_INDEX)
            || !at->n_operands)
            return true;
        place = ir_operand_value(&at->operands[0]);
    }
    return true;
}

bool
n00bb_place_root(ir_value_t *place, ir_local_t **local, ir_value_t **storage)
{
    const ir_instr_t *take = place && place->kind == IR_VAL_INSTR
        ? place->def : nullptr;
    *local = nullptr;
    *storage = nullptr;
    if (take && take->op == IR_OP_ALLOC) {
        *storage = place;
        return true;
    }
    if (!take || take->op != IR_OP_ADDRESS_OF || take->n_operands != 1)
        return false;
    *local = ir_operand_local(&take->operands[0]);
    *storage = *local ? nullptr : ir_operand_value(&take->operands[0]);
    return true;
}

ir_shape_t *
n00bb_place_shape(ir_value_t *place)
{
    ir_local_t *local = nullptr;
    ir_value_t *storage = nullptr;
    ir_shape_t *shape = nullptr;
    if (n00bb_place_root(place, &local, &storage))
        shape = local ? ir_local_shape(local)
            : storage && storage->kind == IR_VAL_GLOBAL && storage->global
            ? ir_global_shape(storage->global) : nullptr;
    return shape ? shape : ir_value_shape(place);
}

static bool
writable_hook(ir_lower_context_t *context, ir_value_t *place, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    th_ctx *types = ir_module_types(context->module);
    ir_value_t *address = place;
    for (size_t depth = 0; address && address->kind == IR_VAL_INSTR
         && address->def && (address->def->op == IR_OP_FIELD
                             || address->def->op == IR_OP_INDEX)
         && address->def->n_operands && depth < 64; depth++)
        address = ir_operand_value(&address->def->operands[0]);
    ir_instr_t *hook = address && address->kind == IR_VAL_INSTR
        ? address->def : nullptr;
    if (!hook || hook->n_operands < 2
        || !ir_annot_get(ir_value_annots(address),
                         CORE_STRING_STATIC("n00bb.reference_address"))
        || ir_value_indirect_access(address) == IR_INDIRECT_ACCESS_READ_WRITE)
        return true;
    ir_value_t *reference = ir_operand_value(&hook->operands[1]);
    th_type *held = reference ? th_resolve(reference->type) : nullptr;
    if (hook->op == IR_OP_DISPATCH)
        hook->operands[0].name = CORE_STRING_STATIC("__ref_address_mut");
    else if (held && th_kind_of(held) == TH_NAMED) {
        th_env_method_t method = {0};
        if (!n00bb_reference_method(types, held, true, &method, nullptr)) {
            core_string_t text = lower_type_text(context, held);
            lower_report(context, N00B_DIAG_NOT_WRITABLE, span,
                N00B_TEXT_READ_ONLY_REFERENCE, N00BB_ARGS(ARG_TEXT(text)));
            core_free(text.data);
            return false;
        }
        ir_module_t *owner = nullptr;
        ir_func_t *function = n00bb_record_field_method(context, &method,
                                                        &owner);
        ir_value_t *callee = owner && function
            ? ir_func_address(owner, function) : nullptr;
        if (!callee) return false;
        hook->operands[0] = (ir_operand_t){.kind = IR_OPD_VALUE,
                                           .value = callee};
    }
    else {
        ir_value_t *writable = held
            ? n00bb_reference_placeholder(context, reference, address->type,
                                          true, span)
            : nullptr;
        ir_instr_t *made = writable ? writable->def : nullptr;
        if (!made) return false;
        hook->operands[0] = made->operands[0];
        (void)ir_instr_erase(made);
    }
    ir_instr_uses_changed(hook);
    (void)lower;
    return n00bb_mark_reference_address(context, address, true);
}

static ir_value_t *
place_in(ir_lower_context_t *context, ir_value_t *value, ir_span_t span,
         core_list_t(n00bb_element_writeback_t) *writebacks);

static ir_value_t *
element_copy(ir_lower_context_t *context, ir_value_t *element,
             ir_value_t *container, ir_value_t *index, ir_span_t span,
             core_list_t(n00bb_element_writeback_t) *writebacks)
{
    n00bb_element_writeback_t back = {
        .container = container,
        .container_place = place_in(context, container, span, writebacks),
        .index = index,
    };
    char buffer[48];
    back.element = ir_local_add(context->function,
        n00bb_format_name(buffer, sizeof(buffer), "__n00bb_element_%llu",
                          (unsigned long long)context->next_value_id++),
        element->type);
    if (!back.element) return nullptr;
    back.element->synthetic = true;
    if (!n00bb_emit_local_write(context, back.element, element, span))
        return nullptr;
    core_list_push(*writebacks, back);
    return emit_address(context, IR_OP_ADDRESS_OF, element->type, nullptr,
                        back.element, (core_string_t){0}, span);
}

static ir_value_t *
storage_of(ir_lower_context_t *context, ir_value_t *value, ir_span_t span,
           core_list_t(n00bb_element_writeback_t) *writebacks, bool writing)
{
    th_ctx *types = ir_module_types(context->module);
    th_type *held = value ? th_resolve(value->type) : nullptr;
    ir_value_t *from = held && th_kind_of(held) == TH_VAR
            && value->kind == IR_VAL_INSTR && value->def
            && value->def->op == IR_OP_LOAD && value->def->n_operands == 1
        ? ir_operand_value(&value->def->operands[0]) : nullptr;
    th_type *address = from ? th_resolve(from->type) : nullptr;
    if (address && th_kind_of(address) == TH_INDIRECTION) {
        held = th_resolve(th_ind_referent(address));
        value->type = th_ind_referent(address);
    }
    th_type *referent = held && th_kind_of(held) == TH_INDIRECTION
        ? th_resolve(th_ind_referent(held)) : nullptr;
    if (referent && n00bb_reference_definition(types, referent)) {
        ir_value_t *handle = n00bb_emit_indirect_read(context, value,
                                                      referent, span);
        return handle ? n00bb_reference_view(context, handle, span) : nullptr;
    }
    if (referent) return value;
    if (held && n00bb_reference_definition(types, held))
        return n00bb_reference_view(context, value, span);
    if (writing) return place_in(context, value, span, writebacks);
    ir_value_t *loaded = value && value->kind == IR_VAL_INSTR && value->def
            && value->def->op == IR_OP_LOAD && value->def->n_operands == 1
        ? ir_operand_value(&value->def->operands[0]) : nullptr;
    ir_instr_t *source = loaded && loaded->kind == IR_VAL_INSTR
        ? loaded->def : nullptr;
    return loaded && loaded->kind != IR_VAL_GLOBAL
            && (!source || (source->op != IR_OP_ALLOC
                            && source->op != IR_OP_ADDRESS_OF))
        ? loaded : nullptr;
}

ir_value_t *
n00bb_field_storage(ir_lower_context_t *context, ir_value_t *base,
                    core_string_t name, th_type *type, ir_span_t span)
{
    ir_value_t *storage = base
        ? storage_of(context, base, span, nullptr, false) : nullptr;
    th_type *held = storage ? th_resolve(storage->type) : nullptr;
    if (!held || th_kind_of(held) != TH_INDIRECTION
        || th_option_item(th_resolve(th_ind_referent(held))))
        return nullptr;
    ir_value_t *field = emit_address(context, IR_OP_FIELD, type, storage,
                                     nullptr, name, span);
    if (field && n00bb_open_type(context, th_ind_referent(held)))
        n00bb_rewrite_later(context, field->def);
    if (field && base->kind == IR_VAL_INSTR && base->def
        && !ir_value_has_uses(context->function, base))
        drop_read(context, base, storage);
    return field;
}

static ir_value_t *
place_in(ir_lower_context_t *context, ir_value_t *value, ir_span_t span,
         core_list_t(n00bb_element_writeback_t) *writebacks)
{
    if (!context || !value) return nullptr;
    if (value->kind == IR_VAL_PARAM || value->kind == IR_VAL_GLOBAL)
        return emit_address(context, IR_OP_ADDRESS_OF, value->type, value,
                            nullptr, (core_string_t){0}, span);
    ir_instr_t *read = value->kind == IR_VAL_INSTR ? value->def : nullptr;
    ir_value_t *place = nullptr;
    n00bb_lower_context_t *noting = context->user;
    parse_tree_t *field_use = field_read_node(noting, value);
    if (read && read->op == IR_OP_LOAD && read->n_operands == 1) {
        place = ir_operand_value(&read->operands[0]);
        if (field_use && !field_read_node(noting, place))
            n00bb_note_field_read(context, place, field_use);
        drop_read(context, value, place);
        return place;
    }
    else if (read && read->op == IR_OP_COPY && read->n_operands == 1) {
        ir_local_t *local = ir_operand_local(&read->operands[0]);
        ir_value_t *source = ir_operand_value(&read->operands[0]);
        place = local
            ? emit_address(context, IR_OP_ADDRESS_OF, local->type, nullptr,
                           local, (core_string_t){0}, span)
            : place_in(context, source, span, writebacks);
    }
    else if (read && read->op == IR_OP_EXTRACT && read->n_operands == 2
             && read->operands[1].kind == IR_OPD_NAME) {
        ir_value_t *base = storage_of(context,
            ir_operand_value(&read->operands[0]), span, writebacks, true);
        place = base ? emit_address(context, IR_OP_FIELD, value->type, base,
                                    nullptr, read->operands[1].name, span)
                     : nullptr;
        ir_shape_t *shape = place ? ir_value_shape(value) : nullptr;
        if (shape && !ir_value_set_shape(place, shape)) place = nullptr;
        if (place && n00bb_open_type(context,
                th_ind_referent(th_resolve(base->type))))
            n00bb_rewrite_later(context, place->def);
    }
    else if ((read && read->op == IR_OP_SEQUENCE_GET && read->n_operands == 2)
             || element_read(context, read)) {
        uint32_t first = read->op == IR_OP_DISPATCH ? 1 : 0;
        ir_value_t *source = ir_operand_value(&read->operands[first]);
        ir_value_t *offset = ir_operand_value(&read->operands[first + 1]);
        n00bb_lower_context_t *lower = context->user;
        th_type *held = source ? th_resolve(source->type) : nullptr;
        uint8_t attrs = 0;
        bool through = held && th_ind_attrs(held, &attrs)
            && ((attrs & TH_INDEXABLE)
                || th_kind_of(th_resolve(th_ind_referent(held)))
                    == TH_SEQUENCE);
        bool stored = through || (held && (th_kind_of(held) == TH_SEQUENCE
            || (th_kind_of(held) == TH_VAR && !lower->write_back_open_index)));
        if (!stored && writebacks && element_read(context, read))
            return element_copy(context, value, source, offset, span,
                                writebacks);
        ir_value_t *base = !stored ? nullptr : through ? source
            : place_in(context, source, span, writebacks);
        place = base && offset
            ? emit_address(context, IR_OP_INDEX, value->type, base, nullptr,
                           (core_string_t){0}, span)
            : nullptr;
        ir_shape_t *shape = place ? ir_value_shape(base) : nullptr;
        if (place && !ir_instr_operand_value(place->def, offset)) place = nullptr;
        if (place && shape
            && th_kind_of(th_resolve(value->type)) == TH_SEQUENCE) {
            ir_shape_t *suffix = ir_shape_suffix(lower->ir_context, shape, 1);
            if (!suffix || !ir_value_set_shape(place, suffix)) place = nullptr;
        }
    }
    if (place && field_use && !field_read_node(noting, place))
        n00bb_note_field_read(context, place, field_use);
    if (place) drop_read(context, value, place);
    return place;
}

ir_value_t *
n00bb_place_of(ir_lower_context_t *context, ir_value_t *value, bool writing,
               ir_span_t span)
{
    ir_value_t *place = place_in(context, value, span, nullptr);
    return place && (!writing || writable_hook(context, place, span))
        ? place : nullptr;
}

ir_value_t *
n00bb_place_for_write(ir_lower_context_t *context, ir_value_t *value,
                      ir_span_t span,
                      core_list_t(n00bb_element_writeback_t) *writebacks)
{
    ir_value_t *place = place_in(context, value, span, writebacks);
    return place && writable_hook(context, place, span) ? place : nullptr;
}

bool
n00bb_emit_writebacks(ir_lower_context_t *context,
                      core_list_t(n00bb_element_writeback_t) *writebacks,
                      ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    th_type *nil = th_name(ir_module_types(context->module),
                           CORE_STRING_STATIC("nil"));
    for (size_t i = writebacks->len; i > 0; i--) {
        n00bb_element_writeback_t *back = &writebacks->data[i - 1];
        ir_value_t *container = back->container_place
            ? n00bb_emit_indirect_read(context, back->container_place,
                th_ind_referent(th_resolve(back->container_place->type)),
                span)
            : back->container;
        ir_value_t *element = n00bb_emit_local_read(context, back->element,
                                                    span);
        ir_value_t *operands[] = {container, back->index, element};
        ir_value_t *written = container && element
            ? n00bb_dispatch(context, CORE_STRING_STATIC("__write"), operands,
                             3, nil, span)
            : nullptr;
        if (!written || !ir_annot_set(lower->ir_context, &written->def->annots,
                (ir_annot_t){.key = N00BB_WRITE_BACK_KEY,
                             .kind = IR_ANNOT_BOOL, .b = true}))
            return false;
    }
    return true;
}

bool
n00bb_store_to(ir_lower_context_t *context, ir_value_t *target,
               ir_value_t *value, bool checked, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_note_place_written(context, target,
        target && ir_value_has_uses(context->function, target));
    ir_instr_t *read = target && target->kind == IR_VAL_INSTR ? target->def
                                                              : nullptr;
    ir_local_t *local = read && read->op == IR_OP_COPY && read->n_operands == 1
        ? ir_operand_local(&read->operands[0]) : nullptr;
    ir_value_t *global = target && target->kind == IR_VAL_GLOBAL ? target
        : read && read->op == IR_OP_COPY && read->n_operands == 1
            && ir_operand_value(&read->operands[0])
            && ir_operand_value(&read->operands[0])->kind == IR_VAL_GLOBAL
        ? ir_operand_value(&read->operands[0]) : nullptr;
    if (local || global) {
        if (read && !ir_value_has_uses(context->function, target))
            (void)ir_instr_erase(read);
        if (local)
            return (!checked || local->mutability == IR_MUT_MUTABLE)
                && n00bb_emit_local_write(context, local, value, span);
        ir_instr_t *store = ir_lower_emit_instruction(context,
                                                      IR_OP_GLOBAL_STORE, span);
        return store && ir_instr_operand_value(store, global)
            && ir_instr_operand_value(store, value);
    }
    if (read && read->op == IR_OP_EXTRACT && read->n_operands == 2
        && read->operands[1].kind == IR_OPD_NAME
        && ir_annot_get(&read->annots, N00BB_VARIANT_ARM_KEY)) {
        ir_value_t *holder = ir_operand_value(&read->operands[0]);
        core_string_t arm = read->operands[1].name;
        th_type *variant = holder ? th_resolve(holder->type) : nullptr;
        parse_sym_entry_t *named = variant && th_kind_of(variant) == TH_VAR
            ? ir_lower_read_variable(context, holder) : nullptr;
        if (named && named->type)
            variant = th_apply_subst(ir_module_types(context->module),
                                     named->type);
        if (!ir_value_has_uses(context->function, target))
            (void)ir_instr_erase(read);
        ir_instr_t *make = holder && variant
            ? ir_lower_emit_instruction(context, IR_OP_VARIANT_MAKE, span)
            : nullptr;
        return make && ir_lower_set_temp_dest(context, make,
                CORE_STRING_STATIC("__n00bb_variant_"), variant)
            && ir_instr_operand_name(make, arm)
            && ir_instr_operand_value(make, value)
            && n00bb_store_to(context, holder, make->dest, checked, span);
    }
    ir_value_t *mask = read && read->op == IR_OP_BITSET_INTERSECTION
            && read->n_operands == 2
        ? ir_operand_value(&read->operands[1]) : nullptr;
    if (mask && mask->kind == IR_VAL_INSTR && mask->def
        && mask->def->op == IR_OP_BITSET_CONST && mask->def->n_operands
        && mask->def->operands[0].kind == IR_OPD_NAME) {
        core_string_t flag = mask->def->operands[0].name;
        core_string_t owner = lower_type_text(context, mask->type);
        lower_report(context, N00B_DIAG_FLAGSET_FIELD_READ_ONLY, span,
                     N00B_TEXT_FLAGSET_FIELD_IS_READ_ONLY,
                     N00BB_ARGS(ARG_TEXT(flag), ARG_TEXT(owner)));
        lower_help(context, N00B_TEXT_FLAGSET_FIELD_ASSIGN_WHOLE_HELP,
                   N00BB_ARGS(ARG_TEXT(flag), ARG_TEXT(owner)));
        core_free(owner.data);
        return false;
    }
    ir_annot_t *slice = read && read->op == IR_OP_DISPATCH
            && read->n_operands == 5
        ? ir_annot_get(&read->annots, N00BB_SLICE_TARGET_KEY) : nullptr;
    if (slice) {
        th_ctx *types = ir_module_types(context->module);
        ir_value_t *container = ir_operand_value(&read->operands[1]);
        ir_value_t *bounds[] = {ir_operand_value(&read->operands[2]),
                                ir_operand_value(&read->operands[3]),
                                ir_operand_value(&read->operands[4])};
        if (!ir_value_has_uses(context->function, target))
            (void)ir_instr_erase(read);
        core_list_t(n00bb_element_writeback_t) writebacks =
            core_list_new(n00bb_element_writeback_t);
        ir_value_t *holder = place_in(context, container, span, &writebacks);
        ir_value_t *operands[] = {
            holder ? n00bb_emit_indirect_read(context, holder,
                         th_ind_referent(th_resolve(holder->type)), span)
                   : container,
            bounds[0], bounds[1], bounds[2], value};
        bool complete = true;
        for (size_t i = 0; i < 5; i++) complete &= operands[i] != nullptr;
        th_type *nil = th_name(types, CORE_STRING_STATIC("nil"));
        ir_value_t *stored = complete
            ? n00bb_dispatch(context, CORE_STRING_STATIC("__write_slice"),
                             operands, 5, nil, span)
            : nullptr;
        bool ok = stored && ir_annot_set(lower->ir_context,
                &stored->def->annots,
                (ir_annot_t){.key = N00BB_WRITE_BACK_KEY,
                             .kind = IR_ANNOT_BOOL, .b = true})
            && n00bb_emit_writebacks(context, &writebacks, span);
        core_list_free(writebacks);
        return ok;
    }
    ir_value_t *container = nullptr, *index = nullptr;
    if (n00bb_accessed_element(target, &container, &index)) {
        th_type *held = container ? th_resolve(container->type) : nullptr;
        uint8_t attrs = 0;
        bool stored = held && (th_kind_of(held) == TH_SEQUENCE
            || (th_ind_attrs(held, &attrs) && ((attrs & TH_INDEXABLE)
                || th_kind_of(th_resolve(th_ind_referent(held)))
                    == TH_SEQUENCE)));
        if (!stored) {
            core_list_t(n00bb_element_writeback_t) writebacks =
                core_list_new(n00bb_element_writeback_t);
            bool erase = !ir_value_has_uses(context->function, target);
            if (erase) (void)ir_instr_erase(target->def);
            ir_value_t *holder = place_in(context, container, span,
                                          &writebacks);
            ir_value_t *operands[] = {
                holder ? n00bb_emit_indirect_read(context, holder,
                             th_ind_referent(th_resolve(holder->type)), span)
                       : container,
                index, value};
            th_type *nil = th_name(ir_module_types(context->module),
                                   CORE_STRING_STATIC("nil"));
            ir_value_t *written = operands[0]
                ? n00bb_dispatch(context, CORE_STRING_STATIC("__write"),
                                 operands, 3, nil, span)
                : nullptr;
            bool ok = written && ir_annot_set(lower->ir_context,
                    &written->def->annots,
                    (ir_annot_t){.key = N00BB_WRITE_BACK_KEY,
                                 .kind = IR_ANNOT_BOOL, .b = true})
                && n00bb_emit_writebacks(context, &writebacks, span);
            core_list_free(writebacks);
            return ok;
        }
    }
    core_list_t(n00bb_element_writeback_t) writebacks =
        core_list_new(n00bb_element_writeback_t);
    bool saved = lower->write_back_open_index;
    lower->write_back_open_index = true;
    ir_value_t *place = place_in(context, target, span, &writebacks);
    lower->write_back_open_index = saved;
    for (ir_value_t *at = place; at && at->kind == IR_VAL_INSTR && at->def
         && (at->def->op == IR_OP_FIELD || at->def->op == IR_OP_INDEX)
         && at->def->n_operands;) {
        ir_value_t *base = ir_operand_value(&at->def->operands[0]);
        th_type *held = base ? th_resolve(base->type) : nullptr;
        if (held && th_kind_of(held) == TH_INDIRECTION
            && n00bb_atomic_element_type(th_ind_referent(held))) {
            lower_report(context, N00B_DIAG_ATOMIC_PART_UPDATE, span,
                         N00B_TEXT_AN_ATOMIC_VALUE_CANNOT_BE, N00BB_NO_ARGS);
            lower_help(context, N00B_TEXT_BUILD_THE_NEW_VALUE_AND,
                       N00BB_NO_ARGS);
            core_list_free(writebacks);
            return false;
        }
        at = base;
    }
    bool ok = place && writable_hook(context, place, span)
        && (!checked || n00bb_place_writable(place))
        && n00bb_emit_indirect_write(context, place, value, span)
        && n00bb_emit_writebacks(context, &writebacks, span);
    core_list_free(writebacks);
    return ok;
}
