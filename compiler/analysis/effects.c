#include "lower/lower_internal.h"

static ir_func_t *
effects_subject(ir_instr_t *call);
static core_string_t
query_text(ir_instr_t *call);
static effects_query_t
effects_query(const ir_instr_t *in);
static bool
summary_has_effect(const ir_summary_t *summary, core_string_t name);
static uint64_t
effects_answer(n00bb_link_analysis_t *analysis, ir_instr_t *in,
               effects_query_t query);
static bool
handler_listed(n00bb_link_analysis_t *analysis, const ir_func_t *handler,
               const th_type *type);
static bool
callback_targets(n00bb_link_analysis_t *analysis, const ir_instr_t *call,
                 ir_summary_target_fn add, void *sink);
static bool
finalizer_function(const ir_func_t *function);
static bool
merge_entry(ir_summary_t *into, ir_summaries_t *summaries,
            const ir_func_t *function, bool callback);
static void
collect_escaped_globals(n00bb_link_analysis_t *analysis);
static void
dump_text(core_buffer_t *out);
static void
dump_param(const ir_func_t *function, uint32_t p);
static void
steps_text(core_buffer_t *out, const ir_spath_t *path);
static void
heap_text(core_buffer_t *out, const ir_summary_t *summary, uint32_t origin);
static void
spath_text(core_buffer_t *out, const ir_func_t *function,
           const ir_summary_t *summary, const ir_spath_t *path);
static void
dump_flows(const ir_func_t *function, const ir_summary_t *summary);
static void
dump_spath(const ir_func_t *function, const ir_summary_t *summary,
           const ir_spath_t *path);
static int
compare_texts(const void *left, const void *right);
static void
add_text(text_list_t *texts, const ir_func_t *function,
         const ir_summary_t *summary, const ir_spath_t *path);
static void
put_texts(core_buffer_t *line, core_string_t label, text_list_t *texts);
static void
collect_output(const ir_func_t *function, const ir_summary_t *summary,
               ir_sflow_table_t table, const ir_spath_t *to,
               text_list_t *data, text_list_t *control);
static core_string_t
output_name(const ir_func_t *function, const ir_summary_t *summary,
            ir_sflow_table_t table, const ir_spath_t *to);
static int
compare_spaths(const void *left, const void *right);
static int
compare_reader_dests(const void *left, const void *right);
static void
reader_table(const ir_func_t *function, const ir_summary_t *summary,
             ir_sflow_table_t table);
static void
dump_function_summary(ir_summaries_t *summaries, ir_func_t *function,
                      bool for_reader);
static int
compare_function_names(const void *left, const void *right);
static void
push_declared_functions(n00bb_link_analysis_t *analysis,
                        core_list_t(n00bb_ir_func_ptr_t) *found,
                        n00bb_tu_state_t *state, core_string_t name);
static void
push_exported_functions(n00bb_link_analysis_t *analysis,
                        core_list_t(n00bb_ir_func_ptr_t) *found,
                        n00bb_tu_state_t *state, core_string_t wanted);
static core_string_t
next_effects_of_name(core_string_t *rest);
static void
explain_named_chain(core_buffer_t *out, n00bb_link_analysis_t *analysis,
                    ir_func_t *function, core_string_t effect);
static void
explain_chain(core_buffer_t *out, n00bb_link_analysis_t *analysis,
              const ir_summary_t *summary, uint32_t flag);
static void
dump_witness_chains(n00bb_link_analysis_t *analysis, ir_func_t *function);
static void
json_bool(core_buffer_t *out, const char *key, bool value, bool comma);
static void
json_path(core_buffer_t *out, const ir_func_t *function,
          const ir_summary_t *summary, const ir_spath_t *path);
static void
json_why(core_buffer_t *out, n00bb_link_analysis_t *analysis,
         const ir_summary_t *summary);
static void
json_function(core_buffer_t *out, n00bb_link_analysis_t *analysis,
              ir_func_t *function);

static ir_func_t *
effects_subject(ir_instr_t *call)
{
    ir_annot_t *subject = ir_annot_get(&call->annots, effects_subject_key);
    ir_value_t *value = subject && subject->kind == IR_ANNOT_VALUE
        ? subject->v : nullptr;
    return value && value->kind == IR_VAL_FUNC && value->owner
        ? n00bb_cached_body(value->owner) : nullptr;
}

static core_string_t
query_text(ir_instr_t *call)
{
    ir_annot_t *name = ir_annot_get(&call->annots, effects_name_key);
    return name && name->kind == IR_ANNOT_STR ? name->s : (core_string_t){0};
}

static effects_query_t
effects_query(const ir_instr_t *in)
{
    static const struct {
        core_string_t   native;
        effects_query_t query;
    } natives[] = {
        {CORE_STRING_STATIC("n00bb_function_has_effect"),
         EFFECTS_QUERY_HAS_EFFECT},
        {CORE_STRING_STATIC("n00bb_function_reads"), EFFECTS_QUERY_READS},
        {CORE_STRING_STATIC("n00bb_function_writes"), EFFECTS_QUERY_WRITES},
        {CORE_STRING_STATIC("n00bb_effect_query"), EFFECTS_QUERY_USER},
    };
    const ir_func_t *target = ir_call_function(in);
    if (!target || target->n_blocks) return EFFECTS_QUERY_NONE;
    for (size_t k = 0; k < sizeof(natives) / sizeof(*natives); k++)
        if (core_string_eq(target->name, natives[k].native))
            return natives[k].query;
    return EFFECTS_QUERY_NONE;
}

static bool
summary_has_effect(const ir_summary_t *summary, core_string_t name)
{
    uint64_t bits = 0;
    return ir_summary_effect_lookup(name, &bits)
        ? (ir_summary_effect_bits(summary) & bits) != 0
        : ir_summary_has_effect(summary, name);
}

static uint64_t
effects_answer(n00bb_link_analysis_t *analysis, ir_instr_t *in,
               effects_query_t query)
{
    ir_func_t *subject = effects_subject(in);
    ir_summary_t *summary = ir_summary_for(analysis->summaries, subject);
    core_string_t text = query_text(in);
    switch (query) {
    case EFFECTS_QUERY_HAS_EFFECT:
        return !text.u8_bytes || summary_has_effect(summary, text);
    case EFFECTS_QUERY_READS:
    case EFFECTS_QUERY_WRITES:
        return !text.u8_bytes || ir_summary_may_access(summary, subject, text,
            query == EFFECTS_QUERY_WRITES);
    case EFFECTS_QUERY_USER: {
        ir_annot_t *word = ir_annot_get(&in->annots,
            CORE_STRING_STATIC("n00bb.effects.query"));
        return word && word->kind == IR_ANNOT_STR
            && answer_query(analysis, subject, n00bb_query_named(word->s),
                            text);
    }
    case EFFECTS_QUERY_NONE:
        break;
    }
    return 0;
}

void
push_effect_query_subjects(n00bb_link_analysis_t *analysis,
                           core_list_t(n00bb_ir_func_ptr_t) *roots)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        if (!state || !state->ir_module || state->interface_module) continue;
        for (size_t f = 0; f < ir_func_count(state->ir_module); f++) {
            ir_func_t *function = ir_func_at(state->ir_module, f);
            for (uint32_t b = 0; b < function->n_blocks; b++)
                for (uint32_t i = 0; i < function->blocks[b]->n_instrs;
                     i++) {
                    ir_instr_t *in = function->blocks[b]->instrs[i];
                    ir_func_t *subject = effects_query(in)
                        ? effects_subject(in) : nullptr;
                    if (subject && subject->n_blocks)
                        core_list_push(*roots, subject);
                }
        }
    }
}

bool
fold_function_effects(n00bb_link_analysis_t *analysis)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        if (!state || !state->ir_module || state->interface_module) continue;
        for (size_t f = 0; f < ir_func_count(state->ir_module); f++) {
            ir_func_t *function = ir_func_at(state->ir_module, f);
            for (uint32_t b = 0; b < function->n_blocks; b++)
                for (uint32_t i = 0; i < function->blocks[b]->n_instrs;
                     i++) {
                    ir_instr_t *in = function->blocks[b]->instrs[i];
                    effects_query_t query = effects_query(in);
                    if (!query) continue;
                    uint64_t bits = effects_answer(analysis, in, query);
                    in->op         = IR_OP_CONST;
                    in->n_operands = 0;
                    in->n_mods     = 0;
                    if (!ir_instr_operand_uint(in, bits)) return false;
                }
        }
    }
    return true;
}

ir_func_t *
callback_handler(const ir_instr_t *creation)
{
    const ir_value_t *reference = creation->op == IR_OP_CALLBACK_CREATE
            && creation->n_operands
            && creation->operands[0].kind == IR_OPD_VALUE
        ? creation->operands[0].value : nullptr;
    const ir_instr_t *retained = reference ? reference->def : nullptr;
    const ir_value_t *handler = retained && retained->op == IR_OP_RETAIN_REF
            && retained->n_operands == 1
            && retained->operands[0].kind == IR_OPD_VALUE
        ? retained->operands[0].value : nullptr;
    return handler && handler->kind == IR_VAL_FUNC && handler->owner
        ? n00bb_cached_body(handler->owner) : nullptr;
}

static bool
handler_listed(n00bb_link_analysis_t *analysis, const ir_func_t *handler,
               const th_type *type)
{
    for (size_t h = 0; h < analysis->callback_handlers.len; h++)
        if (analysis->callback_handlers.data[h] == handler
            && analysis->callback_handler_types.data[h] == type)
            return true;
    return false;
}

void
collect_callback_handlers(n00bb_link_analysis_t *analysis)
{
    analysis->callback_handlers.len = 0;
    analysis->callback_handler_types.len = 0;
    analysis->referenced_functions.len = 0;
    if (analysis->may_link_further) return;
    core_dict_t known, referenced;
    core_dict_init(&known, nullptr, nullptr);
    core_dict_init(&referenced, nullptr, nullptr);
    for (size_t f = 0; f < analysis->emitted_functions.len; f++) {
        ir_func_t *function = analysis->emitted_functions.data[f];
        for (uint32_t b = 0; b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                ir_instr_t *creation = function->blocks[b]->instrs[i];
                for (uint32_t o = creation->op == IR_OP_CALL ? 1 : 0;
                     o < creation->n_operands; o++) {
                    const ir_value_t *value = ir_operand_value(
                        &creation->operands[o]);
                    ir_func_t *taken = value && value->kind == IR_VAL_FUNC
                            && value->owner
                        ? n00bb_cached_body(value->owner) : nullptr;
                    if (taken && core_dict_add(&referenced, taken, taken))
                        core_list_push(analysis->referenced_functions, taken);
                }
                ir_func_t *handler = analysis->closed_world
                    ? callback_handler(creation) : nullptr;
                if (!handler) continue;
                th_type *type = creation->dest ? creation->dest->type
                                               : nullptr;
                bool found = false;
                th_type *first = core_dict_get(&known, handler, &found);
                if (found && (first == type
                              || handler_listed(analysis, handler, type)))
                    continue;
                if (!found) core_dict_put(&known, handler, type);
                core_list_push(analysis->callback_handlers, handler);
                core_list_push(analysis->callback_handler_types, type);
            }
    }
    core_dict_free(&known);
    core_dict_free(&referenced);
}

static bool
function_value_targets(n00bb_link_analysis_t *analysis, const ir_instr_t *call,
                       ir_summary_target_fn add, void *sink)
{
    const ir_value_t *callee = ir_call_callee(call);
    th_type *signature = callee ? ir_instr_call_signature(call) : nullptr;
    if (analysis->may_link_further || !signature
        || (callee->kind == IR_VAL_INSTR && callee->def
            && callee->def->op == IR_OP_TRAIT_METHOD)) return false;
    ir_module_t *module = call->block->owner->owner;
    bool known = false;
    for (size_t f = 0; f < analysis->referenced_functions.len; f++) {
        ir_func_t *candidate = analysis->referenced_functions.data[f];
        if (candidate->n_params + 1 != call->n_operands
            || !ir_type_eq(module, candidate->type, signature)) continue;
        add(sink, candidate);
        known = true;
    }
    return known;
}

static bool
callback_targets(n00bb_link_analysis_t *analysis, const ir_instr_t *call,
                 ir_summary_target_fn add, void *sink)
{
    if (!analysis->closed_world || analysis->may_link_further
        || !call->n_operands) return false;
    const ir_value_t *callback = ir_operand_value(&call->operands[0]);
    ir_module_t *module = call->block->owner->owner;
    core_list_t(n00bb_ir_func_ptr_t) matched =
        core_list_new(n00bb_ir_func_ptr_t);
    bool arity = true;
    for (size_t h = 0; callback && arity
         && h < analysis->callback_handlers.len; h++) {
        th_type *type = analysis->callback_handler_types.data[h];
        if (!type || !ir_type_eq(module, type, callback->type)) continue;
        ir_func_t *handler = analysis->callback_handlers.data[h];
        arity = handler->n_params + 1 == call->n_operands;
        core_list_push(matched, handler);
    }
    bool known = arity && matched.len;
    for (size_t h = 0; known && h < matched.len; h++)
        add(sink, matched.data[h]);
    core_list_free(matched);
    return known;
}

static core_list_t(n00bb_ir_func_ptr_t) *
implementations(n00bb_link_analysis_t *analysis, core_string_t trait,
                core_string_t method)
{
    th_ctx *types = ir_ctx_types(analysis->ir_context);
    core_link_unit_t *link = analysis->units.len
        ? tu_state(analysis->units.data[0])->link : nullptr;
    size_t count = link
        ? th_env_method_candidates(types, trait, method, nullptr, 0) : 0;
    th_env_method_t *methods = count
        ? core_alloc_array(th_env_method_t, count) : nullptr;
    count = methods
        ? th_env_method_candidates(types, trait, method, methods, count) : 0;
    core_list_t(n00bb_ir_func_ptr_t) *found =
        core_alloc(core_list_t(n00bb_ir_func_ptr_t));
    *found = core_list_new(n00bb_ir_func_ptr_t);
    bool known = count != 0;
    for (size_t i = 0; known && i < count; i++) {
        parse_sym_entry_t *symbol = initializer_declaration_symbol(
            linked_method_declaration(link, &methods[i]));
        ir_func_t *target = symbol ? n00bb_cached_body(
            link_ir_function_for_symbol(analysis, symbol)) : nullptr;
        known = target != nullptr;
        if (target) core_list_push(*found, target);
    }
    core_free(methods);
    if (known) return found;
    core_list_free(*found);
    core_free(found);
    return nullptr;
}

static bool
trait_targets(n00bb_link_analysis_t *analysis, const ir_instr_t *call,
              ir_summary_target_fn add, void *sink)
{
    const ir_value_t *callee = ir_call_callee(call);
    const ir_instr_t *selection = callee && callee->kind == IR_VAL_INSTR
        ? callee->def : nullptr;
    const ir_value_t *requirement = selection
        && selection->op == IR_OP_TRAIT_METHOD && selection->n_operands >= 4
        && selection->operands[0].kind == IR_OPD_NAME
        && selection->operands[1].kind == IR_OPD_NAME
        ? ir_operand_value(&selection->operands[3]) : nullptr;
    if (!requirement || requirement->kind != IR_VAL_FUNC
        || !requirement->owner) return false;
    if (!analysis->trait_targets_ready) {
        core_dict_init(&analysis->trait_targets, nullptr, nullptr);
        analysis->trait_targets_ready = true;
    }
    bool found = false;
    core_list_t(n00bb_ir_func_ptr_t) *targets = core_dict_get(
        &analysis->trait_targets, requirement->owner, &found);
    if (!found) {
        targets = implementations(analysis, selection->operands[0].name,
                                  selection->operands[1].name);
        core_dict_put(&analysis->trait_targets, requirement->owner, targets);
    }
    for (size_t i = 0; targets && i < targets->len; i++)
        add(sink, targets->data[i]);
    return targets != nullptr;
}

static bool
hook_targets(n00bb_link_analysis_t *analysis, ir_func_t *placeholder,
             core_string_t trait, core_string_t method,
             ir_summary_target_fn add, void *sink)
{
    if (!analysis->trait_targets_ready) {
        core_dict_init(&analysis->trait_targets, nullptr, nullptr);
        analysis->trait_targets_ready = true;
    }
    bool found = false;
    core_list_t(n00bb_ir_func_ptr_t) *targets = core_dict_get(
        &analysis->trait_targets, placeholder, &found);
    if (!found) {
        targets = implementations(analysis, trait, method);
        core_dict_put(&analysis->trait_targets, placeholder, targets);
    }
    for (size_t i = 0; targets && i < targets->len; i++)
        add(sink, targets->data[i]);
    return targets != nullptr;
}

static const struct {
    core_string_t root;
    core_string_t trait;
    core_string_t method;
} open_hooks[] = {
    {CORE_STRING_STATIC("__n00bb_finalize_open"),
     CORE_STRING_STATIC("Finalize"), CORE_STRING_STATIC("__finalize")},
    {CORE_STRING_STATIC("__n00bb_ref_address_open"),
     CORE_STRING_STATIC("Reference"), CORE_STRING_STATIC("__ref_address")},
    {CORE_STRING_STATIC("__n00bb_ref_address_mut_open"),
     CORE_STRING_STATIC("Mutable_Reference"),
     CORE_STRING_STATIC("__ref_address_mut")},
};

bool
n00bb_open_hook(const ir_instr_t *call, core_string_t *trait,
                core_string_t *method)
{
    for (size_t h = 0; h < sizeof(open_hooks) / sizeof(*open_hooks); h++)
        if (n00bb_open_call(call, open_hooks[h].root)) {
            *trait = open_hooks[h].trait;
            *method = open_hooks[h].method;
            return true;
        }
    return false;
}

void
n00bb_trait_targets_free(n00bb_link_analysis_t *analysis)
{
    if (!analysis->trait_targets_ready) return;
    core_dict_foreach(&analysis->trait_targets, item) {
        core_list_t(n00bb_ir_func_ptr_t) *targets = item->value;
        if (!targets) continue;
        core_list_free(*targets);
        core_free(targets);
    }
    core_dict_free(&analysis->trait_targets);
    analysis->trait_targets_ready = false;
}

bool
n00bb_summary_call_targets(void *context, const ir_instr_t *call,
                           ir_summary_target_fn add, void *sink)
{
    if (call->op == IR_OP_CALLBACK_INVOKE)
        return bounded_call_target(context, call, add, sink)
            || callback_targets(context, call, add, sink);
    ir_func_t *target = n00bb_direct_call_target(call);
    core_string_t trait = {0}, method = {0};
    if (target && n00bb_open_hook(call, &trait, &method))
        return hook_targets(context, target, trait, method, add, sink)
            || bounded_call_target(context, call, add, sink);
    if (!target)
        return trait_targets(context, call, add, sink)
            || bounded_call_target(context, call, add, sink)
            || function_value_targets(context, call, add, sink);
    add(sink, target);
    return true;
}

enum { FINALIZED_TYPE_LIMIT = 64 };

typedef struct {
    th_ctx   *types;
    th_type  *found[FINALIZED_TYPE_LIMIT];
    size_t    count;
} finalized_types_t;

static bool
note_finalized_type(void *context, th_type *variable, th_type *value)
{
    (void)variable;
    finalized_types_t *pending = context;
    th_type *type = value ? th_resolve(value) : nullptr;
    if (!type || !th_is_closed(pending->types, type)) return true;
    for (size_t i = 0; i < pending->count; i++)
        if (th_hash(pending->types, pending->found[i])
            == th_hash(pending->types, type)) return true;
    if (pending->count < FINALIZED_TYPE_LIMIT)
        pending->found[pending->count++] = type;
    return true;
}

static th_type *
finalized_receiver(const ir_func_t *finalizer)
{
    const th_param *self = th_func_param(finalizer->type, 0);
    th_type *type = self && self->type ? th_resolve(self->type) : nullptr;
    return type && th_kind_of(type) == TH_INDIRECTION
        ? th_resolve(th_ind_referent(type)) : type;
}

static const core_list_t(n00bb_ir_func_ptr_t) *
finalizer_implementations(n00bb_link_analysis_t *analysis)
{
    static const char key = 0;
    if (!analysis->trait_targets_ready) {
        core_dict_init(&analysis->trait_targets, nullptr, nullptr);
        analysis->trait_targets_ready = true;
    }
    bool found = false;
    core_list_t(n00bb_ir_func_ptr_t) *targets = core_dict_get(
        &analysis->trait_targets, (void *)&key, &found);
    if (!found) {
        targets = implementations(analysis, CORE_STRING_STATIC("Finalize"),
                                  CORE_STRING_STATIC("__finalize"));
        core_dict_put(&analysis->trait_targets, (void *)&key, targets);
    }
    return targets;
}

static bool
bind_from_operands(th_ctx *types, const ir_instr_t *call,
                   const ir_func_t *target, finalized_types_t *pending)
{
    size_t params = th_func_nparams(target->type);
    for (size_t p = 0; p < params && p + 1 < call->n_operands; p++) {
        const th_param *formal = th_func_param(target->type, p);
        const ir_value_t *actual = call->operands[p + 1].kind == IR_OPD_VALUE
            ? call->operands[p + 1].value : nullptr;
        th_type *type = actual && actual->type ? th_resolve(actual->type)
                                               : nullptr;
        th_type *expected = formal && formal->type ? th_resolve(formal->type)
                                                   : nullptr;
        if (type && expected && th_kind_of(type) == TH_INDIRECTION
            && th_kind_of(expected) != TH_INDIRECTION)
            type = th_resolve(th_ind_referent(type));
        if (!actual) continue;
        if (!expected || !type || !th_is_closed(types, type)
            || !th_match_each(expected, type, note_finalized_type, pending))
            return false;
    }
    th_type *result = th_func_ret(target->type);
    th_type *returned = call->dest && call->dest->type
        ? th_resolve(call->dest->type) : nullptr;
    if (result && returned && th_is_closed(types, returned))
        (void)th_match_each(result, returned, note_finalized_type, pending);
    return true;
}

bool
n00bb_summary_call_finalizers(void *context, const ir_instr_t *call,
                              const ir_func_t *target,
                              ir_summary_target_fn add, void *sink)
{
    n00bb_link_analysis_t *analysis = context;
    ir_module_t *module = call->block && call->block->owner
        ? call->block->owner->owner : nullptr;
    th_ctx *types = module ? ir_module_types(module) : nullptr;
    th_type *signature = ir_instr_call_signature(call);
    if (!types || !target || th_is_closed(types, target->type)) return false;
    finalized_types_t pending = {.types = types};
    if (signature && th_is_closed(types, signature)) {
        if (!th_match_each(target->type, signature, note_finalized_type,
                           &pending))
            return false;
    }
    else if (!bind_from_operands(types, call, target, &pending))
        return false;
    const core_list_t(n00bb_ir_func_ptr_t) *finalizers =
        finalizer_implementations(analysis);
    for (size_t t = 0; t < pending.count; t++) {
        th_type *type = pending.found[t];
        for (size_t f = 0; finalizers && f < finalizers->len; f++) {
            ir_func_t *finalizer = finalizers->data[f];
            th_type *receiver = finalized_receiver(finalizer);
            if (!receiver || th_kind_of(receiver) == TH_VAR
                || !th_matches(receiver, type)) continue;
            add(sink, finalizer);
            if (!th_is_closed(types, receiver))
                (void)th_match_each(receiver, type, note_finalized_type,
                                    &pending);
        }
        for (size_t a = 0; th_kind_of(type) == TH_NAMED
             && a < th_named_nargs(type); a++) {
            const th_arg *argument = th_named_arg(type, a);
            if (argument && argument->kind == TH_ARG_TYPE)
                (void)note_finalized_type(&pending, nullptr, argument->type);
        }
        th_type *definition = th_kind_of(type) == TH_NAMED
            ? ir_type_definition(module, type) : type;
        definition = definition ? th_resolve(definition) : nullptr;
        for (size_t m = 0; definition && th_kind_of(definition) == TH_RECORD
             && m < th_record_len(definition); m++)
            (void)note_finalized_type(&pending, nullptr,
                                      th_record_member(definition, m)->type);
    }
    return true;
}

uint32_t
n00bb_summary_call_flags(void *context, const ir_instr_t *call)
{
    (void)context;
    core_string_t trait = {0}, method = {0};
    return n00bb_direct_call_target(call)
            && n00bb_open_hook(call, &trait, &method)
            && core_string_eq(trait, CORE_STRING_STATIC("Finalize"))
        ? IR_SUMMARY_FINALIZES_PARAMS : 0;
}

ir_summary_t *
n00bb_summary_axiom(void *context, ir_summaries_t *summaries,
                    const ir_func_t *function)
{
    if (!function->n_blocks && core_string_starts_with(function->name,
            CORE_STRING_STATIC(N00BB_MARK_QUERY)))
        return ir_summary_new(summaries, function);
    if (n00bb_is_callback_bound(function)) {
        n00bb_link_analysis_t *analysis = context;
        ir_summary_t *summary = ir_summary_bounded(summaries, function,
            n00bb_bound_bits(function, N00BB_QUERY_FORBIDS_EFFECT));
        for (size_t e = 0; summary && e < analysis->user_effects.len; e++) {
            core_string_t name = analysis->user_effects.data[e].qualified;
            if (!analysis->user_effects.data[e].bits
                && !n00bb_bound_names(function, N00BB_QUERY_FORBIDS_EFFECT,
                                      name))
                (void)ir_summary_add_effect(summary, name);
        }
        return summary;
    }
    ir_summary_t *summary = n00bb_native_axiom(summaries, function);
    return summary ? summary
                   : n00bb_interface_summary(context, summaries, function);
}

static bool
finalizer_function(const ir_func_t *function)
{
    return function->n_blocks && function->n_params == 1
        && (core_string_eq(function->name, CORE_STRING_STATIC("__finalize"))
            || core_string_starts_with(function->name,
                                       CORE_STRING_STATIC("__finalize#")));
}

static bool
merge_entry(ir_summary_t *into, ir_summaries_t *summaries,
            const ir_func_t *function, bool callback)
{
    const ir_summary_t *summary = ir_summary_of(summaries, function);
    if (!summary) summary = ir_summary_for(summaries, (ir_func_t *)function);
    if (!summary) return false;
    ir_summary_set_flags(into, ir_summary_flags(into)
                               | ir_summary_flags(summary));
    ir_summary_set_trap_kinds(into, ir_summary_trap_kinds(into)
                                    | ir_summary_trap_kinds(summary));
    for (uint32_t i = 0; i < ir_summary_path_count(summary); i++) {
        const ir_spath_t *path = ir_summary_path_at(summary, i);
        ir_splace_t       places[2];
        uint32_t          count = ir_summary_path_places(function, path,
                                                         places);
        for (uint32_t p = 0; p < count; p++) {
            ir_splace_t place = places[p];
            if (place.kind == IR_SPLACE_PARAM && callback)
                place = (ir_splace_t){.kind = IR_SPLACE_UNKNOWN};
            else if (place.kind == IR_SPLACE_PARAM) {
                th_type *type = place.param < function->n_params
                    ? th_resolve(function->params[place.param]->type)
                    : nullptr;
                if (!type || th_kind_of(type) != TH_INDIRECTION) continue;
                place = (ir_splace_t){.kind = IR_SPLACE_HEAP,
                                      .type = th_ind_referent(type)};
            }
            ir_spath_t merged = ir_spath_of_place(&place,
                                                  path->flags
                                                      & IR_SPATH_ACCESS);
            ir_summary_add_path(into, &merged);
        }
    }
    return true;
}

const ir_summary_t *
n00bb_summary_entries(void *context, ir_summaries_t *summaries,
                      uint32_t flag)
{
    n00bb_link_analysis_t *analysis = context;
    if (analysis->entry_owner != summaries) {
        analysis->entry_owner = summaries;
        for (size_t s = 0; s < 3; s++) analysis->entry_ready[s] = false;
    }
    size_t slot = flag == IR_SUMMARY_CALLS_FINALIZERS ? 0
        : flag == IR_SUMMARY_STARTS_THREADS ? 1 : 2;
    if (analysis->entry_ready[slot]) return analysis->entry_summaries[slot];
    analysis->entry_ready[slot] = true;
    bool complete = true;
    core_list_t(n00bb_ir_func_ptr_t) entries = slot == 1
        ? thread_entries(&analysis->emitted_functions, &complete, nullptr)
        : core_list_new(n00bb_ir_func_ptr_t);
    for (size_t f = 0; slot == 0 && f < analysis->emitted_functions.len; f++)
        if (finalizer_function(analysis->emitted_functions.data[f]))
            core_list_push(entries, analysis->emitted_functions.data[f]);
    for (size_t f = 0; slot == 2 && f < analysis->callback_handlers.len; f++)
        core_list_push(entries, analysis->callback_handlers.data[f]);
    if (!complete)
        add_address_taken_functions(&analysis->emitted_functions, &entries);
    ir_summary_t *merged = ir_summary_new(summaries, nullptr);
    bool merged_all = true;
    for (size_t f = 0; merged_all && f < entries.len; f++)
        merged_all = merge_entry(merged, summaries, entries.data[f],
                                 slot == 2);
    core_list_free(entries);
    if (merged_all && (!analysis->closed_world || analysis->may_link_further))
        ir_summary_add_path(merged, &(ir_spath_t){
            .root = IR_SPLACE_UNKNOWN,
            .flags = IR_SPATH_ACCESS | IR_SPATH_DEEP});
    analysis->entry_summaries[slot] = merged_all ? merged : nullptr;
    return analysis->entry_summaries[slot];
}

static void
collect_escaped_globals(n00bb_link_analysis_t *analysis)
{
    if (analysis->escaped_owner == analysis->escapes) return;
    if (analysis->escaped_owner) core_dict_free(&analysis->escaped_globals);
    core_dict_init(&analysis->escaped_globals, nullptr, nullptr);
    analysis->escaped_owner = analysis->escapes;
    for (size_t f = 0; f < analysis->emitted_functions.len; f++) {
        ir_func_t *function = analysis->emitted_functions.data[f];
        for (uint32_t b = 0; b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                ir_instr_t *in = function->blocks[b]->instrs[i];
                const ir_value_t *storage = in->op == IR_OP_ADDRESS_OF
                        && in->n_operands
                    ? ir_operand_value(&in->operands[0]) : nullptr;
                if (!storage || storage->kind != IR_VAL_GLOBAL
                    || !storage->global || !in->dest
                    || !ir_address_escapes(analysis->escapes, function,
                                           in->dest)) continue;
                const ir_global_t *global =
                    ir_global_identity(storage->global);
                core_dict_put(&analysis->escaped_globals, (void *)global,
                              (void *)global);
            }
    }
}

bool
n00bb_summary_global_reachable(void *context, const ir_global_t *global)
{
    n00bb_link_analysis_t *analysis = context;
    if (!analysis->escapes) return true;
    if (!analysis->global_symbols_ready) {
        core_dict_init(&analysis->global_symbols, nullptr, nullptr);
        index_module_globals(analysis, &analysis->global_symbols);
        analysis->global_symbols_ready = true;
    }
    bool found = false;
    parse_sym_entry_t *symbol = core_dict_get(&analysis->global_symbols,
                                              (void *)global, &found);
    if (!found || !symbol || symbol_exported_by_default(symbol)
        || parse_sym_binding(symbol,
               CORE_STRING_STATIC("n00bb.external-export")))
        return true;
    collect_escaped_globals(analysis);
    return core_dict_contains(&analysis->escaped_globals, (void *)global);
}

void
param_text(core_buffer_t *out, const ir_func_t *function, uint32_t p)
{
    core_string_t name = p < function->n_params && function->params[p]
        ? function->params[p]->name : (core_string_t){0};
    if (name.u8_bytes) core_buffer_append_string(out, name);
    else core_buffer_printf(out, "arg%u", p);
}

static void
dump_text(core_buffer_t *out)
{
    fprintf(stdout, "%.*s", (int)out->byte_len, out->data ? out->data : "");
    core_buffer_release(out);
}

static void
dump_param(const ir_func_t *function, uint32_t p)
{
    core_buffer_t out = {0};
    param_text(&out, function, p);
    dump_text(&out);
}

static void
steps_text(core_buffer_t *out, const ir_spath_t *path)
{
    for (uint32_t s = 0; s < path->depth; s++) {
        core_string_t step = path->steps[s];
        if (!core_string_eq(step, CORE_STRING_STATIC("[]")))
            core_buffer_putc(out, '.');
        core_buffer_append_string(out, step);
    }
    if (path->flags & IR_SPATH_DEEP) core_buffer_puts(out, "...");
}

static void
heap_text(core_buffer_t *out, const ir_summary_t *summary, uint32_t origin)
{
    const ir_sheap_t *heap = ir_summary_heap_for(summary, origin);
    if (!heap || !heap->name.u8_bytes) {
        core_buffer_printf(out, "(new#%04x)", origin & 0xffff);
        return;
    }
    ir_switness_t named = {.module = heap->module, .name = heap->name};
    core_buffer_puts(out, "(new in ");
    ir_switness_callee_text(&named, out);
    core_buffer_printf(out, ":%u:%u)", heap->line, heap->column);
}

static void
spath_text(core_buffer_t *out, const ir_func_t *function,
           const ir_summary_t *summary, const ir_spath_t *path)
{
    if (path->root == IR_SPLACE_PARAM)
        param_text(out, function, path->param);
    else if (path->root == IR_SPLACE_SOURCE) {
        core_buffer_puts(out, "(");
        core_buffer_append_string(out, ir_summary_flag_name(path->param));
        core_buffer_puts(out, ")");
    }
    else if (path->root == IR_SPLACE_HEAP && path->param)
        heap_text(out, summary, path->param);
    else if (path->root == IR_SPLACE_HEAP)
        core_buffer_puts(out, "(new)");
    else if (path->global)
        core_buffer_append_string(out, path->global->name);
    else if (path->root == IR_SPLACE_GLOBAL)
        core_buffer_puts(out, "(globals)");
    else if (path->root == IR_SPLACE_THREAD)
        core_buffer_puts(out, "(thread globals)");
    else
        core_buffer_puts(out, "?");
    steps_text(out, path);
}

static void
dump_flows(const ir_func_t *function, const ir_summary_t *summary)
{
    static const ir_sflow_kind_t kinds[] = {IR_SFLOW_ALIAS, IR_SFLOW_COPY,
                                            IR_SFLOW_DATA, IR_SFLOW_CONTROL};
    static const char *const labels[] = {"", "copy ", "data ", "control "};
    for (int t = 0; t < 2; t++) {
        ir_sflow_table_t table = t ? IR_SFLOW_STORES : IR_SFLOW_RESULTS;
        for (uint32_t f = 0; f < ir_summary_flow_count(summary, table); f++) {
            const ir_spath_t *to = ir_summary_flow_to(summary, table, f);
            for (int k = 0; k < 4; k++)
                for (uint32_t s = 0;
                     s < ir_summary_flow_source_count(summary, table); s++) {
                    if (!ir_summary_flow_has(summary, table, f, s, kinds[k]))
                        continue;
                    const ir_spath_t *from =
                        ir_summary_flow_source(summary, table, s);
                    if (table == IR_SFLOW_RESULTS)
                        fprintf(stdout, "  result %u ", to->output);
                    else {
                        fputs("  store ", stdout);
                        dump_spath(function, summary, to);
                        fputs(" <- ", stdout);
                    }
                    fputs(labels[k], stdout);
                    dump_spath(function, summary, from);
                    if (from->flags & IR_SPATH_ROOT) fputs(" root", stdout);
                    fputc('\n', stdout);
                }
        }
    }
}

static void
dump_spath(const ir_func_t *function, const ir_summary_t *summary,
           const ir_spath_t *path)
{
    core_buffer_t out = {0};
    spath_text(&out, function, summary, path);
    dump_text(&out);
}

static int
compare_texts(const void *left, const void *right)
{
    return core_string_cmp(*(const core_string_t *)left,
                           *(const core_string_t *)right);
}

static void
add_text(text_list_t *texts, const ir_func_t *function,
         const ir_summary_t *summary, const ir_spath_t *path)
{
    core_buffer_t *text = core_buffer_empty();
    spath_text(text, function, summary, path);
    core_string_t added = core_buffer_take_string(text);
    for (size_t i = 0; i < texts->len; i++)
        if (core_string_eq(texts->data[i], added)) {
            core_free((void *)added.data);
            return;
        }
    core_list_push(*texts, added);
}

static void
put_texts(core_buffer_t *line, core_string_t label, text_list_t *texts)
{
    qsort(texts->data, texts->len, sizeof(core_string_t), compare_texts);
    core_buffer_puts(line, " ");
    core_buffer_append_string(line, label);
    core_buffer_puts(line, "[");
    for (size_t i = 0; i < texts->len; i++) {
        if (i) core_buffer_putc(line, ' ');
        core_buffer_append_string(line, texts->data[i]);
        core_free((void *)texts->data[i].data);
    }
    core_buffer_puts(line, "]");
    texts->len = 0;
}

static void
collect_output(const ir_func_t *function, const ir_summary_t *summary,
               ir_sflow_table_t table, const ir_spath_t *to,
               text_list_t *data, text_list_t *control)
{
    bool results = table == IR_SFLOW_RESULTS;
    for (uint32_t f = 0; f < ir_summary_flow_count(summary, table); f++) {
        const ir_spath_t *dest = ir_summary_flow_to(summary, table, f);
        if (results ? dest->output != to->output
                    : ir_spath_compare(dest, to) != 0)
            continue;
        for (uint32_t s = 0; s < ir_summary_flow_source_count(summary, table);
             s++) {
            const ir_spath_t *from = ir_summary_flow_source(summary, table, s);
            if (ir_summary_flow_has(summary, table, f, s, IR_SFLOW_DATA)
                || ir_summary_flow_has(summary, table, f, s, IR_SFLOW_ALIAS)
                || ir_summary_flow_has(summary, table, f, s, IR_SFLOW_COPY))
                add_text(data, function, summary, from);
            if (ir_summary_flow_has(summary, table, f, s, IR_SFLOW_CONTROL))
                add_text(control, function, summary, from);
        }
    }
}

static core_string_t
output_name(const ir_func_t *function, const ir_summary_t *summary,
            ir_sflow_table_t table, const ir_spath_t *to)
{
    core_buffer_t *name = core_buffer_empty();
    if (table != IR_SFLOW_RESULTS)
        spath_text(name, function, summary, to);
    else if (to->output == IR_SOUTPUT_FAILURE)
        core_buffer_puts(name, "failure");
    else if (to->output)
        core_buffer_printf(name, "result %u", to->output);
    else
        core_buffer_puts(name, "result");
    return core_buffer_take_string(name);
}

static int
compare_spaths(const void *left, const void *right)
{
    const ir_spath_t *a = left, *b = right;
    if (a->output != b->output) return a->output < b->output ? -1 : 1;
    return ir_spath_compare(a, b);
}

static int
compare_reader_dests(const void *left, const void *right)
{
    const reader_dest_t *a = left, *b = right;
    if (a->to.output != b->to.output)
        return a->to.output < b->to.output ? -1 : 1;
    return core_string_cmp(a->name, b->name);
}

static void
reader_table(const ir_func_t *function, const ir_summary_t *summary,
             ir_sflow_table_t table)
{
    bool            results = table == IR_SFLOW_RESULTS;
    ir_spath_list_t dests   = core_list_new(ir_spath_t);
    if (results) core_list_push(dests, (ir_spath_t){0});
    for (uint32_t f = 0; f < ir_summary_flow_count(summary, table); f++)
        core_list_push(dests, *ir_summary_flow_to(summary, table, f));
    if (results)
        for (size_t i = 0; i < dests.len; i++)
            dests.data[i] = (ir_spath_t){.output = dests.data[i].output};
    qsort(dests.data, dests.len, sizeof(ir_spath_t), compare_spaths);
    core_list_t(reader_dest_t) named = core_list_new(reader_dest_t);
    for (size_t i = 0; i < dests.len; i++)
        if ((!i || compare_spaths(&dests.data[i - 1], &dests.data[i]))
            && !(results && dests.data[i].output == IR_SOUTPUT_STATUS))
            core_list_push(named, ((reader_dest_t){
                .name = output_name(function, summary, table,
                                    &dests.data[i]),
                .to   = dests.data[i]}));
    qsort(named.data, named.len, sizeof(reader_dest_t), compare_reader_dests);
    for (size_t i = 0; i < named.len;) {
        text_list_t data    = core_list_new(core_string_t);
        text_list_t control = core_list_new(core_string_t);
        size_t      next    = i;
        for (; next < named.len && !compare_reader_dests(&named.data[i],
                                                         &named.data[next]);
             next++)
            collect_output(function, summary, table, &named.data[next].to,
                           &data, &control);
        core_buffer_t line = {0};
        core_buffer_puts(&line, "  output ");
        core_buffer_append_string(&line, named.data[i].name);
        put_texts(&line, CORE_STRING_STATIC("data"), &data);
        put_texts(&line, CORE_STRING_STATIC("control"), &control);
        core_buffer_putc(&line, '\n');
        dump_text(&line);
        core_list_free(data);
        core_list_free(control);
        i = next;
    }
    for (size_t i = 0; i < named.len; i++)
        core_free((void *)named.data[i].name.data);
    core_list_free(named);
    core_list_free(dests);
}

static void
dump_function_summary(ir_summaries_t *summaries, ir_func_t *function,
                      bool for_reader)
{
    static const struct {
        uint8_t     flag;
        const char *name;
    } outcomes[] = {
        {IR_SPATH_ROOT, "root"},
        {IR_SPATH_MUST_SUCCESS, "must-success"},
        {IR_SPATH_MUST_FAILURE, "must-failure"},
        {IR_SPATH_READS_ENTRY, "reads-entry"},
    };
    ir_summary_t *summary = ir_summary_of(summaries, function);
    if (!summary) return;
    core_string_t owner = ir_module_name(function->owner);
    th_ctx *types = ir_module_types(function->owner);
    if (for_reader) {
        core_string_t name = n00bb_function_root(function->name);
        core_string_t type = n00bb_print_type(types, function->type);
        fprintf(stdout, "%.*s.%.*s: %.*s\n  effects",
                (int)owner.u8_bytes, owner.data,
                (int)name.u8_bytes, name.data,
                (int)type.u8_bytes, type.data);
    }
    else
        fprintf(stdout, "summary @%.*s.%.*s",
                (int)owner.u8_bytes, owner.data,
                (int)function->name.u8_bytes, function->name.data);
    for (uint32_t b = 0; b < IR_SEFFECT_FLAG_BITS; b++) {
        core_string_t flag = ir_summary_flag_name(b);
        if (ir_summary_flags(summary) & (1u << b))
            fprintf(stdout, " %.*s", (int)flag.u8_bytes, flag.data);
    }
    if (for_reader && !(ir_summary_flags(summary) & IR_SEFFECT_FLAG_MASK))
        fputs(" none", stdout);
    fputc('\n', stdout);
    for (uint32_t e = 0; e < ir_summary_effect_count(summary); e++) {
        core_string_t effect = ir_summary_effect_at(summary, e);
        fprintf(stdout, "  effect %.*s\n", (int)effect.u8_bytes,
                effect.data);
    }
    ir_spath_list_t merged = core_list_new(ir_spath_t);
    if (for_reader) ir_summary_merged_paths(summary, &merged);
    uint32_t paths = for_reader ? (uint32_t)merged.len
                                : ir_summary_path_count(summary);
    for (uint32_t i = 0; i < paths; i++) {
        const ir_spath_t *path = for_reader ? &merged.data[i]
                                            : ir_summary_path_at(summary, i);
        fprintf(stdout, "  %s%s ",
                path->flags & IR_SPATH_READ ? "reads" : "",
                path->flags & IR_SPATH_WRITE
                    ? path->flags & IR_SPATH_READ ? "+writes" : "writes"
                    : "");
        dump_spath(function, summary, path);
        for (size_t n = 0; !for_reader && n < sizeof(outcomes)
                                                  / sizeof(*outcomes); n++)
            if (path->flags & outcomes[n].flag)
                fprintf(stdout, " %s", outcomes[n].name);
        if (!for_reader && path->type) {
            core_string_t type = n00bb_print_type(types, path->type);
            fprintf(stdout, " : %.*s", (int)type.u8_bytes, type.data);
        }
        fputc('\n', stdout);
    }
    ir_spath_list_release(&merged);
    if (!for_reader) dump_flows(function, summary);
    for (uint32_t p = 0; p < function->n_params; p++) {
        uint8_t escape = ir_summary_param_escape(summary, p);
        if (!escape) continue;
        fputs("  escapes ", stdout);
        dump_param(function, p);
        fprintf(stdout, "%s%s\n",
                escape & IR_SESCAPE_ADDRESS ? " address" : "",
                escape & IR_SESCAPE_REFERENT ? " referent" : "");
    }
    uint8_t traps = ir_summary_trap_kinds(summary);
    if (traps) {
        fputs("  traps", stdout);
        for (uint32_t t = 0; t < IR_SEFFECT_TRAP_BITS; t++)
            if (traps & (1u << t)) {
                core_string_t name = ir_summary_trap_name(t);
                fprintf(stdout, " %.*s", (int)name.u8_bytes, name.data);
            }
        fputc('\n', stdout);
    }
    for (uint32_t s = 0; s < ir_summary_trap_site_count(summary); s++) {
        const ir_strap_site_t *site = ir_summary_trap_site_at(summary, s);
        for (uint32_t t = 0; t < IR_SEFFECT_TRAP_BITS; t++)
            if (site->kind == 1u << t) {
                core_string_t name = ir_summary_trap_name(t);
                fprintf(stdout, "  trap %.*s %u:%u\n", (int)name.u8_bytes,
                        name.data, site->line, site->column);
            }
    }
    ir_sreturn_t returned = ir_summary_return(summary);
    if (returned.kind == IR_SRETURN_PARAM) {
        fputs("  returns ", stdout);
        dump_param(function, returned.param);
        fputc('\n', stdout);
    }
    else if (returned.kind == IR_SRETURN_GLOBAL && returned.global)
        fprintf(stdout, "  returns global %.*s\n",
                (int)returned.global->name.u8_bytes,
                returned.global->name.data);
    else if (returned.kind == IR_SRETURN_FRESH_EXTERNAL)
        fputs("  returns fresh-external\n", stdout);
    else if (ir_summary_returns_fresh(summary))
        fputs("  returns fresh\n", stdout);
    else if (returned.kind == IR_SRETURN_UNKNOWN) {
        text_list_t sources = core_list_new(core_string_t);
        for (uint32_t f = 0;
             f < ir_summary_flow_count(summary, IR_SFLOW_RESULTS); f++) {
            const ir_spath_t *to = ir_summary_flow_to(summary,
                                                      IR_SFLOW_RESULTS, f);
            for (uint32_t s = 0; !to->output && !to->depth
                 && s < ir_summary_flow_source_count(summary,
                                                     IR_SFLOW_RESULTS); s++)
                if (ir_summary_flow_has(summary, IR_SFLOW_RESULTS, f, s,
                                        IR_SFLOW_ALIAS))
                    add_text(&sources, function, summary,
                             ir_summary_flow_source(summary,
                                                    IR_SFLOW_RESULTS, s));
        }
        qsort(sources.data, sources.len, sizeof(core_string_t),
              compare_texts);
        core_buffer_t line = {0};
        core_buffer_puts(&line, "  returns");
        for (size_t s = 0; s < sources.len; s++) {
            core_buffer_putc(&line, ' ');
            core_buffer_append_string(&line, sources.data[s]);
            core_free((void *)sources.data[s].data);
        }
        if (!sources.len) core_buffer_puts(&line, " ?");
        core_buffer_putc(&line, '\n');
        dump_text(&line);
        core_list_free(sources);
    }
    if (for_reader) {
        reader_table(function, summary, IR_SFLOW_RESULTS);
        reader_table(function, summary, IR_SFLOW_STORES);
    }
}

void
dump_function_summaries(n00bb_link_analysis_t *analysis)
{
    for (size_t f = 0; f < analysis->reachable_functions.len; f++)
        dump_function_summary(analysis->summaries,
            n00bb_cached_body(analysis->reachable_functions.data[f]), false);
}

static int
compare_function_names(const void *left, const void *right)
{
    const ir_func_t *a = *(const ir_func_t *const *)left;
    const ir_func_t *b = *(const ir_func_t *const *)right;
    int order = core_string_cmp(a->name, b->name);
    return order ? order : core_string_cmp(ir_module_name(a->owner),
                                           ir_module_name(b->owner));
}

static void
push_declared_functions(n00bb_link_analysis_t *analysis,
                        core_list_t(n00bb_ir_func_ptr_t) *found,
                        n00bb_tu_state_t *state, core_string_t name)
{
    parse_scope_t *scope = state ? module_scope(state) : nullptr;
    for (parse_sym_entry_t *symbol = n00bb_scope_named_next(scope, name,
                                                            nullptr);
         symbol; symbol = n00bb_scope_named_next(scope, name, symbol)) {
        ir_func_t *function = n00bb_cached_body(link_ir_function_for_symbol(
            analysis, canonical_symbol(symbol)));
        if (function && function->n_blocks
            && !core_option_is_set(core_list_find(*found, function)))
            core_list_push(*found, function);
    }
}

static void
push_exported_functions(n00bb_link_analysis_t *analysis,
                        core_list_t(n00bb_ir_func_ptr_t) *found,
                        n00bb_tu_state_t *state, core_string_t wanted)
{
    for (size_t d = 0; d < state->package_declarations.len; d++) {
        n00bb_package_declaration_t *package =
            &state->package_declarations.data[d];
        core_string_t member;
        if (!n00bb_name_member(wanted, package->name, &member)) continue;
        for (size_t e = 0; e < package->exports.len; e++) {
            n00bb_package_export_t *export = &package->exports.data[e];
            n00bb_tu_state_t *source = export->source_unit
                ? tu_state(export->source_unit) : nullptr;
            if (!source) continue;
            core_string_t source_name = n00bb_name_leaf(export->source_name);
            core_string_t exported = export->export_name.u8_bytes
                ? n00bb_name_leaf(export->export_name) : source_name;
            if (export->module_export)
                push_declared_functions(analysis, found, source, member);
            else if (core_string_eq(exported, member))
                push_declared_functions(analysis, found, source, source_name);
        }
    }
}

static core_list_t(n00bb_ir_func_ptr_t)
effects_of_functions(n00bb_link_analysis_t *analysis, core_string_t wanted)
{
    core_list_t(n00bb_ir_func_ptr_t) found =
        core_list_new(n00bb_ir_func_ptr_t);
    for (size_t u = 0; wanted.data && u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        if (!state || !state->ir_module) continue;
        core_string_t member;
        if (n00bb_name_member(wanted, ir_module_name(state->ir_module),
                             &member))
            push_declared_functions(analysis, &found, state, member);
        push_exported_functions(analysis, &found, state, wanted);
    }
    core_list_sort(found, compare_function_names);
    return found;
}

static core_string_t
next_effects_of_name(core_string_t *rest)
{
    int64_t comma = core_string_find_byte(*rest, ',');
    size_t length = comma < 0 ? rest->u8_bytes : (size_t)comma;
    core_string_t name = core_string_slice(*rest, 0, length);
    *rest = comma < 0 ? (core_string_t){0}
        : core_string_slice(*rest, length + 1, rest->u8_bytes - length - 1);
    return name;
}

void
push_effects_of_roots(n00bb_link_analysis_t *analysis,
                      core_list_t(n00bb_ir_func_ptr_t) *roots)
{
    for (core_string_t rest = analysis->effects_of; rest.data;) {
        core_list_t(n00bb_ir_func_ptr_t) functions =
            effects_of_functions(analysis, next_effects_of_name(&rest));
        for (size_t i = 0; i < functions.len; i++)
            core_list_push(*roots, functions.data[i]);
        core_list_free(functions);
    }
}

uint32_t
witness_chain(n00bb_link_analysis_t *analysis, const ir_summary_t *summary,
              uint32_t flag, const ir_switness_t **chain)
{
    const ir_switness_t *witness = summary
            && (flag == IR_SWITNESS_UNKNOWN
                || ir_summary_flags(summary) & flag)
        ? ir_summary_witness_for(summary, flag) : nullptr;
    uint32_t depth = 0;
    while (witness && depth < WITNESS_DEPTH) {
        chain[depth++] = witness;
        ir_func_t *next = n00bb_cached_body(witness->callee);
        witness = next ? ir_summary_witness_for(
            ir_summary_of(analysis->summaries, next), flag) : nullptr;
    }
    return depth;
}

static bool
unknown_witness(const ir_switness_t *witness)
{
    return !witness->callee
        && core_string_eq(witness->name, CORE_STRING_STATIC("?"));
}

static bool
callback_witness(const ir_switness_t *witness)
{
    return !witness->callee && !witness->module.u8_bytes
        && witness->name.u8_bytes && !unknown_witness(witness);
}

void
witness_text(const ir_switness_t *witness, core_buffer_t *out)
{
    if (unknown_witness(witness)) {
        core_buffer_puts(out, "an unknown call");
        return;
    }
    if (callback_witness(witness)) {
        core_buffer_puts(out, "callback `");
        core_buffer_append_string(out, witness->name);
        core_buffer_putc(out, '`');
        return;
    }
    ir_annot_t *bound = n00bb_callback_bound(witness->callee);
    if (!bound || bound->kind != IR_ANNOT_STR) {
        ir_switness_callee_text(witness, out);
        return;
    }
    if (!bound->s.u8_bytes) {
        core_buffer_puts(out, "a checked callback");
        return;
    }
    core_buffer_puts(out, "a call to `");
    core_buffer_append_string(out, bound->s);
    core_buffer_putc(out, '`');
}

typedef struct {
    const ir_instr_t *call;
    ir_func_t        *caller;
    ir_func_t        *target;
} named_step_t;

static uint32_t
named_chain(n00bb_link_analysis_t *analysis, ir_func_t *subject,
            core_string_t effect, named_step_t *steps)
{
    n00bb_user_effect_t *user = user_effect(analysis, effect);
    uint32_t depth = 0;
    if (user && n00bb_function_listed(&user->sources, subject)) return 0;
    for (ir_func_t *function = subject;
         user && function && function->n_blocks && depth < WITNESS_DEPTH;) {
        const ir_instr_t *found = nullptr;
        ir_func_t *next = nullptr;
        bool source = false;
        for (uint32_t b = 0; !found && b < function->n_blocks; b++)
            for (uint32_t i = 0; !found && i < function->blocks[b]->n_instrs;
                 i++) {
                const ir_instr_t *in = function->blocks[b]->instrs[i];
                ir_func_t *target = n00bb_direct_call_target(in);
                ir_summary_t *summary = target
                    ? ir_summary_of(analysis->summaries, target) : nullptr;
                if (!target
                    || clause_applies(analysis, &user->handlers, function,
                                      target))
                    continue;
                source = clause_applies(analysis, &user->sources, function,
                                        target);
                bool carries = summary && (user->bits
                    ? (ir_summary_effect_bits(summary) & user->bits) != 0
                    : ir_summary_has_effect(summary, effect));
                bool repeated = false;
                for (uint32_t d = 0; d < depth; d++)
                    repeated = repeated || steps[d].caller == target;
                if (source || (carries && !repeated && target != function)) {
                    found = in;
                    next = target;
                }
            }
        if (!found) break;
        steps[depth++] = (named_step_t){
            .call = found, .caller = function, .target = next};
        function = source ? nullptr : n00bb_cached_body(next);
    }
    return depth;
}

static void
explain_named_chain(core_buffer_t *out, n00bb_link_analysis_t *analysis,
                    ir_func_t *function, core_string_t effect)
{
    named_step_t steps[WITNESS_DEPTH];
    uint32_t depth = named_chain(analysis, function, effect, steps);
    for (uint32_t d = 0; d < depth; d++) {
        if (d) core_buffer_puts(out, " -> ");
        else core_buffer_puts(out, "  ");
        function_name_text(out, steps[d].target);
        core_buffer_printf(out, " (%u:%u)", steps[d].call->span.start_line,
                           steps[d].call->span.start_col);
    }
}

bool
n00bb_effect_crosses(const n00bb_user_effect_t *user, const ir_func_t *caller,
                     const ir_func_t *callee)
{
    if (!user || user->scope == N00BB_EFFECT_SCOPE_THREAD) return true;
    if (user->scope == N00BB_EFFECT_SCOPE_CALLER || !user->analysis
        || !caller || !callee) return false;
    core_translation_unit_t *from = unit_for_module(user->analysis,
                                                    caller->owner);
    core_translation_unit_t *to = unit_for_module(user->analysis,
                                                  callee->owner);
    return from && to
        && core_string_eq(n00bb_package_id(from), n00bb_package_id(to));
}

static bool
leaves_package(n00bb_link_analysis_t *analysis, const ir_func_t *subject,
               core_string_t module)
{
    core_translation_unit_t *home = unit_for_module(analysis, subject->owner);
    core_translation_unit_t *there = module.u8_bytes
        ? n00bb_unit_named(analysis, module) : nullptr;
    return !home || !there
        || !core_string_eq(n00bb_package_id(home), n00bb_package_id(there));
}

typedef struct {
    core_translation_unit_t *unit;
    core_diag_location_t     at;
    core_string_t            text;
} chain_step_t;
core_list_decl(chain_step_t);

static void
chain_steps_release(core_list_t(chain_step_t) *steps)
{
    for (size_t i = 0; i < steps->len; i++)
        core_free(steps->data[i].text.data);
    core_list_free(*steps);
}

static bool
named_chain_steps(n00bb_link_analysis_t *analysis, ir_func_t *subject,
                  core_string_t effect, core_list_t(chain_step_t) *out)
{
    named_step_t steps[WITNESS_DEPTH];
    uint32_t depth = named_chain(analysis, subject, effect, steps);
    for (uint32_t d = 0; d < depth; d++) {
        core_buffer_t target = {0};
        function_name_text(&target, steps[d].target);
        bool last = d + 1 == depth || leaves_package(analysis, subject,
            ir_module_name(steps[d].target->owner));
        core_list_push(*out, ((chain_step_t){
            .unit = unit_for_module(analysis, steps[d].caller->owner),
            .at = n00bb_span_location(steps[d].call->span),
            .text = n00bb_text(
                last ? N00B_TEXT_QUERY_CALLS_MAY_HAVE : N00B_TEXT_QUERY_CALLS,
                N00BB_ARGS(
                    ARG_TEXT(n00bb_function_root(steps[d].caller->name)),
                    ARG_TEXT(core_buffer_string(&target)),
                    ARG_TEXT(effect))),
        }));
        core_buffer_release(&target);
        if (last) break;
    }
    return depth != 0;
}

static void
report_chain_steps(core_diagnostics_t *diagnostics,
                   core_list_t(chain_step_t) *steps)
{
    for (size_t i = 0; i < steps->len; i++)
        core_diag_secondary(diagnostics, steps->data[i].unit,
                            steps->data[i].at, steps->data[i].text);
}

bool
named_chain_notes(n00bb_link_analysis_t *analysis,
                  core_diagnostics_t *diagnostics, ir_func_t *subject,
                  core_string_t effect)
{
    core_list_t(chain_step_t) steps = core_list_new(chain_step_t);
    bool found = named_chain_steps(analysis, subject, effect, &steps);
    report_chain_steps(diagnostics, &steps);
    chain_steps_release(&steps);
    return found;
}

static void
explain_chain(core_buffer_t *out, n00bb_link_analysis_t *analysis,
              const ir_summary_t *summary, uint32_t flag)
{
    const ir_switness_t *chain[WITNESS_DEPTH];
    uint32_t depth = witness_chain(analysis, summary, flag, chain);
    for (uint32_t d = 0; d < depth; d++) {
        core_buffer_t callee = {0};
        witness_text(chain[d], &callee);
        if (d) core_buffer_puts(out, " -> ");
        if (callee.byte_len) core_buffer_append(out, callee.data,
                                                callee.byte_len);
        else core_buffer_puts(out, "here");
        core_buffer_printf(out, " (%u:%u)", chain[d]->line, chain[d]->column);
        core_buffer_release(&callee);
    }
}

static void
dump_witness_chains(n00bb_link_analysis_t *analysis, ir_func_t *function)
{
    ir_summary_t *summary = ir_summary_of(analysis->summaries, function);
    for (uint32_t b = 0; b <= IR_SEFFECT_FLAG_BITS; b++) {
        bool     unknown = b == IR_SEFFECT_FLAG_BITS;
        core_buffer_t chain = {0};
        explain_chain(&chain, analysis, summary,
                      unknown ? IR_SWITNESS_UNKNOWN : 1u << b);
        core_string_t name = unknown ? CORE_STRING_STATIC("unknown")
                                     : ir_summary_flag_name(b);
        if (chain.byte_len)
            fprintf(stdout, "  why %.*s: %.*s\n", (int)name.u8_bytes,
                    name.data, (int)chain.byte_len, chain.data);
        core_buffer_release(&chain);
    }
}

void
function_name_text(core_buffer_t *out, const ir_func_t *function)
{
    core_buffer_append_string(out, ir_module_name(function->owner));
    core_buffer_putc(out, '.');
    core_buffer_append_string(out, n00bb_function_root(function->name));
}

static void
json_bool(core_buffer_t *out, const char *key, bool value, bool comma)
{
    core_buffer_printf(out, "%s\"%s\":%s", comma ? "," : "", key,
                       value ? "true" : "false");
}

static void
json_path(core_buffer_t *out, const ir_func_t *function,
          const ir_summary_t *summary, const ir_spath_t *path)
{
    core_buffer_t text = {0};
    spath_text(&text, function, summary, path);
    core_buffer_puts(out, "\"path\":");
    core_buffer_append_quoted(out, core_buffer_string(&text));
    core_buffer_release(&text);
}

static void
json_why(core_buffer_t *out, n00bb_link_analysis_t *analysis,
         const ir_summary_t *summary)
{
    core_buffer_puts(out, ",\"why\":{");
    bool first = true;
    for (uint32_t b = 0; b <= IR_SEFFECT_FLAG_BITS; b++) {
        bool     unknown = b == IR_SEFFECT_FLAG_BITS;
        uint32_t flag    = unknown ? IR_SWITNESS_UNKNOWN : 1u << b;
        const ir_switness_t *chain[WITNESS_DEPTH];
        uint32_t depth = witness_chain(analysis, summary, flag, chain);
        if (!depth) continue;
        if (!first) core_buffer_putc(out, ',');
        first = false;
        core_buffer_append_quoted(out, unknown ? CORE_STRING_STATIC("unknown")
                                               : ir_summary_flag_name(b));
        core_buffer_puts(out, ":[");
        for (uint32_t d = 0; d < depth; d++) {
            if (d) core_buffer_putc(out, ',');
            core_buffer_puts(out, "{\"callee\":");
            core_buffer_t callee = {0};
            ir_switness_callee_text(chain[d], &callee);
            if (callee.byte_len)
                core_buffer_append_quoted(out, core_buffer_string(&callee));
            else
                core_buffer_puts(out, "null");
            core_buffer_release(&callee);
            core_buffer_printf(out, ",\"line\":%u,\"column\":%u}",
                               chain[d]->line, chain[d]->column);
        }
        core_buffer_putc(out, ']');
    }
    core_buffer_putc(out, '}');
}

static void
json_function(core_buffer_t *out, n00bb_link_analysis_t *analysis,
              ir_func_t *function)
{
    ir_summary_t *summary = ir_summary_of(analysis->summaries, function);
    core_buffer_t name = {0};
    function_name_text(&name, function);
    core_buffer_puts(out, "{\"function\":");
    core_buffer_append_quoted(out, core_buffer_string(&name));
    core_buffer_release(&name);
    core_buffer_puts(out, ",\"type\":");
    core_buffer_append_quoted(out, n00bb_print_type(
        ir_module_types(function->owner), function->type));
    core_buffer_puts(out, ",\"effects\":[");
    for (uint32_t b = 0, n = 0; b < IR_SEFFECT_FLAG_BITS; b++)
        if (ir_summary_flags(summary) & (1u << b)) {
            if (n++) core_buffer_putc(out, ',');
            core_buffer_append_quoted(out, ir_summary_flag_name(b));
        }
    core_buffer_puts(out, "],\"named_effects\":[");
    for (uint32_t e = 0; e < ir_summary_effect_count(summary); e++) {
        if (e) core_buffer_putc(out, ',');
        core_buffer_append_quoted(out, ir_summary_effect_at(summary, e));
    }
    core_buffer_puts(out, "],\"accesses\":[");
    ir_spath_list_t merged = core_list_new(ir_spath_t);
    ir_summary_merged_paths(summary, &merged);
    for (size_t i = 0; i < merged.len; i++) {
        const ir_spath_t *path = &merged.data[i];
        core_buffer_printf(out, "%s{", i ? "," : "");
        json_path(out, function, summary, path);
        json_bool(out, "reads", path->flags & IR_SPATH_READ, true);
        json_bool(out, "writes", path->flags & IR_SPATH_WRITE, true);
        core_buffer_putc(out, '}');
    }
    ir_spath_list_release(&merged);
    core_buffer_puts(out, "],\"escapes\":[");
    for (uint32_t p = 0, n = 0; p < function->n_params; p++) {
        uint8_t escape = ir_summary_param_escape(summary, p);
        if (!escape) continue;
        core_buffer_t name = {0};
        param_text(&name, function, p);
        core_buffer_printf(out, "%s{\"param\":", n++ ? "," : "");
        core_buffer_append_quoted(out, core_buffer_string(&name));
        core_buffer_release(&name);
        json_bool(out, "address", escape & IR_SESCAPE_ADDRESS, true);
        json_bool(out, "referent", escape & IR_SESCAPE_REFERENT, true);
        core_buffer_putc(out, '}');
    }
    core_buffer_puts(out, "],\"traps\":[");
    for (uint32_t t = 0, n = 0; t < IR_SEFFECT_TRAP_BITS; t++)
        if (ir_summary_trap_kinds(summary) & (1u << t)) {
            if (n++) core_buffer_putc(out, ',');
            core_buffer_append_quoted(out, ir_summary_trap_name(t));
        }
    core_buffer_putc(out, ']');
    json_bool(out, "returns_fresh", ir_summary_returns_fresh(summary), true);
    core_buffer_puts(out, ",\"returns\":[");
    bool first = true;
    for (uint32_t f = 0; f < ir_summary_flow_count(summary, IR_SFLOW_RESULTS);
         f++) {
        const ir_spath_t *to = ir_summary_flow_to(summary, IR_SFLOW_RESULTS, f);
        for (uint32_t s = 0;
             s < ir_summary_flow_source_count(summary, IR_SFLOW_RESULTS);
             s++) {
            if (!ir_summary_flow_has(summary, IR_SFLOW_RESULTS, f, s,
                                     IR_SFLOW_ALIAS)) continue;
            core_buffer_t at = {0};
            steps_text(&at, to);
            core_buffer_printf(out, "%s{\"output\":%u,\"at\":",
                               first ? "" : ",", (unsigned)to->output);
            first = false;
            core_buffer_append_quoted(out, core_buffer_string(&at));
            core_buffer_release(&at);
            core_buffer_putc(out, ',');
            json_path(out, function, summary,
                      ir_summary_flow_source(summary, IR_SFLOW_RESULTS, s));
            core_buffer_putc(out, '}');
        }
    }
    core_buffer_putc(out, ']');
    json_why(out, analysis, summary);
    core_buffer_putc(out, '}');
}

static void
explain_line(core_buffer_t *out, core_string_t label, core_buffer_t *detail)
{
    core_buffer_printf(out, "  %-16.*s", (int)label.u8_bytes, label.data);
    if (detail->byte_len) core_buffer_append(out, detail->data,
                                             detail->byte_len);
    core_buffer_putc(out, '\n');
    core_buffer_release(detail);
}

static void
explain_globals(core_buffer_t *detail, const ir_func_t *function,
                const ir_summary_t *summary, uint8_t access)
{
    bool first = true;
    for (uint32_t i = 0; i < ir_summary_path_count(summary); i++) {
        const ir_spath_t *path = ir_summary_path_at(summary, i);
        if ((path->root != IR_SPLACE_GLOBAL && path->root != IR_SPLACE_THREAD)
            || !(path->flags & access)) continue;
        if (!first) core_buffer_puts(detail, ", ");
        first = false;
        spath_text(detail, function, summary, path);
    }
}

static uint32_t
effect_witness_flag(uint64_t bits)
{
    for (uint32_t b = 0; b < IR_SEFFECT_FLAG_BITS; b++)
        if (bits == (1u << b)) return 1u << b;
    return 0;
}

static void
explain_function(n00bb_link_analysis_t *analysis, ir_func_t *function)
{
    ir_summary_t *summary = ir_summary_of(analysis->summaries, function);
    if (!summary) return;
    core_buffer_t out = {0};
    function_name_text(&out, function);
    core_buffer_puts(&out, ": ");
    core_buffer_append_string(&out, n00bb_print_type(
        ir_module_types(function->owner), function->type));
    core_buffer_putc(&out, '\n');
    uint64_t bits = ir_summary_effect_bits(summary);
    uint32_t flags = ir_summary_flags(summary);
    bool any = false;
    for (size_t i = 0; i < n00bb_effect_name_count(); i++) {
        const n00bb_effect_name_t *effect = n00bb_effect_name_at(i);
        if (effect->group || !(bits & effect->bits)) continue;
        any = true;
        core_buffer_t detail = {0};
        uint32_t flag = effect_witness_flag(effect->bits);
        if (flag == IR_SUMMARY_MAY_DIVERGE
            && (flags & IR_SUMMARY_DIVERGES)) {
            core_buffer_puts(&detail, "always");
        }
        bool reads = effect->bits == IR_SEFFECT_READS_NONLOCAL;
        if (reads || effect->bits == IR_SEFFECT_WRITES_NONLOCAL) {
            explain_globals(&detail, function, summary,
                            reads ? IR_SPATH_READ : IR_SPATH_WRITE);
            if (!detail.byte_len)
                explain_chain(&detail, analysis, summary,
                              IR_SWITNESS_UNKNOWN);
        }
        else if (flag) {
            core_buffer_t chain = {0};
            explain_chain(&chain, analysis, summary, flag);
            if (chain.byte_len && detail.byte_len)
                core_buffer_puts(&detail, ": ");
            core_buffer_append(&detail, chain.data, chain.byte_len);
            core_buffer_release(&chain);
        }
        explain_line(&out, effect->name, &detail);
    }
    for (size_t e = 0; e < analysis->user_effects.len; e++) {
        n00bb_user_effect_t *user = &analysis->user_effects.data[e];
        if (!user->bits || (bits & user->bits)
            || !n00bb_function_listed(&user->sources, function)) continue;
        any = true;
        core_buffer_t detail = {0};
        core_buffer_puts(&detail, "a source");
        explain_line(&out, user->qualified, &detail);
    }
    if (flags & IR_SUMMARY_CALLS_UNKNOWN) {
        any = true;
        core_buffer_t detail = {0};
        explain_chain(&detail, analysis, summary, IR_SWITNESS_UNKNOWN);
        explain_line(&out, CORE_STRING_STATIC("may do anything"), &detail);
    }
    if (!any) {
        core_buffer_t none = {0};
        explain_line(&out, CORE_STRING_STATIC("no effects"), &none);
    }
    for (uint32_t e = 0; e < ir_summary_effect_count(summary); e++) {
        core_string_t effect = ir_summary_effect_at(summary, e);
        n00bb_bound_t bound;
        core_buffer_t detail = {0};
        bool bounded = n00bb_bound_parse(effect, &bound);
        if (bounded) {
            core_buffer_append_string(&detail, n00bb_query_word(bound.query));
            core_buffer_putc(&detail, '(');
            param_text(&detail, function, bound.param);
            core_buffer_puts(&detail, ", ");
            core_buffer_append_string(&detail, bound.effect);
            core_buffer_putc(&detail, ')');
        }
        else {
            core_buffer_append_string(&detail, effect);
            n00bb_user_effect_t *user = user_effect(analysis, effect);
            if (user && n00bb_function_listed(&user->sources, function))
                core_buffer_puts(&detail, ", a source");
            if (user && n00bb_function_listed(&user->handlers, function))
                core_buffer_puts(&detail, ", handled here");
            explain_named_chain(&detail, analysis, function, effect);
        }
        explain_line(&out, bounded ? CORE_STRING_STATIC("checks")
                                   : CORE_STRING_STATIC("named"), &detail);
    }
    for (size_t e = 0; e < analysis->user_effects.len; e++) {
        n00bb_user_effect_t *user = &analysis->user_effects.data[e];
        if (user->bits || !n00bb_function_listed(&user->sources, function)
            || ir_summary_has_effect(summary, user->qualified)) continue;
        core_buffer_t detail = {0};
        core_buffer_append_string(&detail, user->qualified);
        core_buffer_puts(&detail, ", a source");
        explain_line(&out, CORE_STRING_STATIC("named"), &detail);
    }
    for (uint32_t p = 0; p < function->n_params; p++) {
        uint8_t role = n00bb_parameter_role(function, summary, p);
        core_buffer_t detail = {0};
        core_buffer_printf(&detail, "%s", role & N00BB_ROLE_WRITES ? "writer"
                                        : role & N00BB_ROLE_READS ? "reader"
                                        : "untouched");
        if (role & N00BB_ROLE_LENDS) core_buffer_puts(&detail, ", lends");
        core_buffer_t label = {0};
        core_buffer_puts(&label, "param ");
        param_text(&label, function, p);
        explain_line(&out, core_buffer_string(&label), &detail);
        core_buffer_release(&label);
    }
    core_buffer_t hidden = {0};
    uint64_t named = n00bb_named_effect_bits() | IR_SUMMARY_DIVERGES
        | IR_SUMMARY_NO_RETURN | IR_SUMMARY_MAY_FAIL
        | IR_SUMMARY_CALLS_UNKNOWN;
    for (uint32_t b = 0; b < IR_SEFFECT_FLAG_BITS; b++) {
        if (!(flags & (1u << b)) || (named & (1u << b))) continue;
        if (hidden.byte_len) core_buffer_putc(&hidden, ' ');
        core_buffer_append_string(&hidden, ir_summary_flag_name(b));
    }
    if (hidden.byte_len)
        explain_line(&out, CORE_STRING_STATIC("detail"), &hidden);
    else core_buffer_release(&hidden);
    dump_text(&out);
}

core_translation_unit_t *
unit_for_module(n00bb_link_analysis_t *analysis, const ir_module_t *module)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        if (state && state->ir_module
            && core_string_eq(ir_module_name(state->ir_module),
                              ir_module_name(module)))
            return analysis->units.data[u];
    }
    return nullptr;
}

static void
witness_chain_steps(n00bb_link_analysis_t *analysis, ir_func_t *subject,
                    uint32_t flag, core_string_t effect,
                    core_list_t(chain_step_t) *out)
{
    for (size_t e = 0; flag != IR_SWITNESS_UNKNOWN
         && e < analysis->user_effects.len; e++)
        if ((analysis->user_effects.data[e].bits & flag)
            && named_chain_steps(analysis, subject,
                                 analysis->user_effects.data[e].qualified,
                                 out))
            return;
    ir_summary_t *summary = ir_summary_of(analysis->summaries, subject);
    const ir_switness_t *chain[WITNESS_DEPTH];
    bool unknown = flag == IR_SWITNESS_UNKNOWN;
    uint32_t depth = witness_chain(analysis, summary, flag, chain);
    if (!depth && unknown)
        depth = witness_chain(analysis, summary, IR_SUMMARY_CALLS_UNKNOWN,
                              chain);
    ir_func_t *caller = subject;
    for (uint32_t d = 0; d < depth && caller; d++) {
        core_translation_unit_t *unit = unit_for_module(analysis,
                                                        caller->owner);
        core_diag_location_t at = {
            .line = chain[d]->line, .column = chain[d]->column,
            .end_line = chain[d]->line, .end_column = chain[d]->column,
            .has_location = unit && chain[d]->line != 0,
        };
        core_string_t name = n00bb_function_root(caller->name);
        core_buffer_t callee = {0};
        bool callback = callback_witness(chain[d]);
        uint32_t declared = 0;
        bool foreign = chain[d]->callee
            && n00bb_axiom_foreign_native(chain[d]->callee, &declared);
        core_string_t root = foreign
            ? n00bb_axiom_foreign_name(chain[d]->callee) : (core_string_t){0};
        if (foreign && core_string_starts_with(root, CORE_STRING_STATIC("__")))
            root = core_string_slice(root, 2, root.u8_bytes - 2);
        if (callback) core_buffer_append_string(&callee, chain[d]->name);
        else if (foreign) core_buffer_append_string(&callee, root);
        else witness_text(chain[d], &callee);
        bool anything = (unknown && d + 1 == depth)
            || unknown_witness(chain[d]);
        bool last = d + 1 == depth
            || (chain[d]->callee
                && leaves_package(analysis, subject,
                                  ir_module_name(chain[d]->callee->owner)));
        n00bb_text_t text = callback ? N00B_TEXT_QUERY_CALLS_CALLBACK
            : anything && foreign ? N00B_TEXT_QUERY_CALLS_UNDECLARED_FOREIGN
            : anything ? N00B_TEXT_QUERY_MAY_DO_ANYTHING
            : !callee.byte_len ? N00B_TEXT_QUERY_DOES_HERE
            : last ? N00B_TEXT_QUERY_CALLS_MAY_HAVE
            : N00B_TEXT_QUERY_CALLS;
        n00bb_args_t args = callee.byte_len
            ? N00BB_ARGS(ARG_TEXT(name),
                         ARG_TEXT(core_buffer_string(&callee)),
                         ARG_TEXT(effect))
            : N00BB_ARGS(ARG_TEXT(name), ARG_TEXT(effect));
        core_list_push(*out, ((chain_step_t){
            .unit = unit, .at = at, .text = n00bb_text(text, args)}));
        core_buffer_release(&callee);
        if (last) break;
        caller = chain[d]->callee ? n00bb_cached_body(chain[d]->callee)
                                  : nullptr;
    }
}

void
explain_chain_notes(n00bb_link_analysis_t *analysis,
                    core_diagnostics_t *diagnostics, ir_func_t *subject,
                    uint32_t flag, core_string_t effect)
{
    core_list_t(chain_step_t) steps = core_list_new(chain_step_t);
    witness_chain_steps(analysis, subject, flag, effect, &steps);
    report_chain_steps(diagnostics, &steps);
    chain_steps_release(&steps);
}

uint32_t
bound_witness_flag(const ir_summary_t *summary, core_string_t effect_name)
{
    const n00bb_effect_name_t *effect = n00bb_effect_named(effect_name);
    uint32_t flags = summary ? ir_summary_flags(summary) : 0;
    uint32_t flag = effect ? (uint32_t)(effect->bits & flags
                                        & IR_SEFFECT_FLAG_MASK) : 0;
    return flag ? flag & -flag
        : flags & IR_SUMMARY_CALLS_UNKNOWN ? IR_SWITNESS_UNKNOWN : 0;
}

bool
dump_effects_of(n00bb_link_analysis_t *analysis, core_translation_unit_t *unit)
{
    bool found = true;
    core_buffer_t json = {0};
    size_t listed = 0;
    for (core_string_t rest = analysis->effects_of; rest.data;) {
        core_string_t name = next_effects_of_name(&rest);
        core_list_t(n00bb_ir_func_ptr_t) functions =
            effects_of_functions(analysis, name);
        if (!functions.len) {
            n00bb_report(core_translation_unit_diagnostics(unit), nullptr,
                N00B_DIAG_EFFECTS_OF_UNKNOWN_FUNCTION,
                (core_diag_location_t){}, N00B_TEXT_NO_FUNCTION_NAMED,
                N00BB_ARGS(ARG_TEXT(name)));
            found = false;
        }
        for (size_t i = 0; i < functions.len; i++) {
            if (analysis->effects_json) {
                core_buffer_printf(&json, "%s", listed++ ? ",\n " : "[");
                json_function(&json, analysis, functions.data[i]);
                continue;
            }
            if (analysis->effects_explain) {
                explain_function(analysis, functions.data[i]);
                continue;
            }
            dump_function_summary(analysis->summaries, functions.data[i],
                                  true);
            dump_witness_chains(analysis, functions.data[i]);
        }
        core_list_free(functions);
    }
    if (analysis->effects_json) {
        core_buffer_printf(&json, "%s", listed ? "]\n" : "[]\n");
        dump_text(&json);
    }
    return found;
}

typedef enum {
    QUERY_LEAF,
    QUERY_AND,
    QUERY_OR,
    QUERY_NOT,
} query_kind_t;

typedef struct query_node_t {
    query_kind_t               kind;
    n00bb_query_t              query;
    core_string_t              name;
    const n00bb_user_effect_t *user;
    struct query_node_t       *left;
    struct query_node_t       *right;
} query_node_t;

typedef struct {
    n00bb_link_analysis_t *analysis;
    core_diagnostics_t    *diagnostics;
    core_string_t          text;
    size_t                 at;
    bool                   failed;
} query_parser_t;

static void
query_free(query_node_t *node)
{
    if (!node) return;
    query_free(node->left);
    query_free(node->right);
    core_free(node->name.data);
    core_free(node);
}

static query_node_t *
query_node(query_kind_t kind, query_node_t *left, query_node_t *right)
{
    query_node_t *node = core_alloc(query_node_t);
    if (!node) {
        query_free(left);
        query_free(right);
        return nullptr;
    }
    *node = (query_node_t){.kind = kind, .left = left, .right = right};
    return node;
}

static bool
query_word_byte(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_' || c == '.';
}

static core_string_t
query_peek(const query_parser_t *parser)
{
    core_string_t text = parser->text;
    size_t at = parser->at;
    while (at < text.u8_bytes && (text.data[at] == ' '
                                  || text.data[at] == '\t'
                                  || text.data[at] == '\n'))
        at++;
    if (at >= text.u8_bytes) return (core_string_t){0};
    char c = text.data[at];
    size_t end = at + 1;
    if (query_word_byte(c))
        while (end < text.u8_bytes && query_word_byte(text.data[end])) end++;
    else if ((c == '&' || c == '|') && end < text.u8_bytes
             && text.data[end] == c)
        end++;
    return core_string_slice(text, at, end - at);
}

static void
query_advance(query_parser_t *parser, core_string_t token)
{
    parser->at = (size_t)(token.data - parser->text.data) + token.u8_bytes;
}

static bool
query_is(core_string_t token, core_string_t word)
{
    return token.data && core_string_eq(token, word);
}

static bool
query_keyword(core_string_t token)
{
    return query_is(token, CORE_STRING_STATIC("and"))
        || query_is(token, CORE_STRING_STATIC("or"))
        || query_is(token, CORE_STRING_STATIC("not"));
}

static void
query_report(query_parser_t *parser, n00bb_text_t text, n00bb_args_t args)
{
    if (parser->failed) return;
    parser->failed = true;
    n00bb_report(parser->diagnostics, nullptr, N00B_DIAG_EFFECTS_QUERY_INVALID,
                 (core_diag_location_t){}, text, args);
}

static void
query_expected(query_parser_t *parser, n00bb_text_t expected,
               core_string_t found)
{
    if (!found.data) {
        query_report(parser, N00B_TEXT_QUERY_EXPECTED,
                     N00BB_ARGS(ARG_PHRASE_ID(expected),
                                ARG_PHRASE(QUERY_THE_END)));
        return;
    }
    core_buffer_t quoted = {0};
    core_buffer_putc(&quoted, '`');
    core_buffer_append_string(&quoted, found);
    core_buffer_putc(&quoted, '`');
    query_report(parser, N00B_TEXT_QUERY_EXPECTED,
                 N00BB_ARGS(ARG_PHRASE_ID(expected),
                            ARG_TEXT(core_buffer_string(&quoted))));
    core_buffer_release(&quoted);
}

static bool
query_resolve_effect(query_parser_t *parser, core_string_t word,
                     query_node_t *node)
{
    n00bb_link_analysis_t *analysis = parser->analysis;
    const n00bb_effect_name_t *builtin = n00bb_effect_named(word);
    if (builtin) {
        if (builtin->group && node->query == N00BB_QUERY_HAS_EFFECT) {
            query_report(parser, N00B_TEXT_HAS_EFFECT_TAKES_ONE,
                         N00BB_ARGS(ARG_TEXT(word)));
            return false;
        }
        node->name = core_string_dup(word);
        node->user = user_effect(analysis, word);
        return true;
    }
    const n00bb_user_effect_t *found = user_effect(analysis, word);
    const n00bb_user_effect_t *other = nullptr;
    for (size_t e = 0; !found && e < analysis->user_effects.len; e++) {
        const n00bb_user_effect_t *effect = &analysis->user_effects.data[e];
        core_string_t qualified = effect->qualified;
        size_t prefix = qualified.u8_bytes > word.u8_bytes
            ? qualified.u8_bytes - word.u8_bytes : 0;
        if (effect->bits || !prefix || qualified.data[prefix - 1] != '.'
            || !core_string_eq(core_string_slice(qualified, prefix,
                                                 word.u8_bytes), word))
            continue;
        if (other) continue;
        for (size_t f = e + 1; !other
             && f < analysis->user_effects.len; f++) {
            const n00bb_user_effect_t *next = &analysis->user_effects.data[f];
            size_t at = next->qualified.u8_bytes > word.u8_bytes
                ? next->qualified.u8_bytes - word.u8_bytes : 0;
            if (!next->bits && at && next->qualified.data[at - 1] == '.'
                && core_string_eq(core_string_slice(next->qualified, at,
                                                    word.u8_bytes), word))
                other = next;
        }
        if (other) {
            query_report(parser, N00B_TEXT_NAMED_IN_TWO_MODULES,
                N00BB_ARGS(ARG_TEXT(word), ARG_TEXT(effect->qualified),
                           ARG_TEXT(other->qualified)));
            return false;
        }
        found = effect;
    }
    int64_t dot = core_string_rfind_byte(word, '.');
    for (size_t e = 0; !found && dot > 0 && e < analysis->user_effects.len;
         e++) {
        const n00bb_user_effect_t *effect = &analysis->user_effects.data[e];
        core_string_t package = core_string_slice(word, 0, (size_t)dot);
        core_string_t leaf = core_string_slice(word, (size_t)dot + 1,
            word.u8_bytes - (size_t)dot - 1);
        int64_t split = core_string_rfind_byte(effect->qualified, '.');
        if (effect->bits || split <= 0
            || !core_string_eq(core_string_slice(effect->qualified,
                                   (size_t)split + 1,
                                   effect->qualified.u8_bytes
                                       - (size_t)split - 1), leaf))
            continue;
        core_translation_unit_t *owner = n00bb_unit_named(analysis,
            core_string_slice(effect->qualified, 0, (size_t)split));
        if (owner && core_string_eq(n00bb_package_namespace(owner), package))
            found = effect;
    }
    if (!found) {
        query_report(parser, N00B_TEXT_NOT_AN_EFFECT,
                     N00BB_ARGS(ARG_TEXT(word)));
        return false;
    }
    node->name = core_string_dup(found->qualified);
    node->user = found;
    return true;
}

static query_node_t *query_or(query_parser_t *parser);

static query_node_t *
query_question(query_parser_t *parser)
{
    core_string_t word = query_peek(parser);
    if (!word.data || !query_word_byte(word.data[0]) || query_keyword(word)) {
        query_expected(parser, N00B_TEXT_QUERY_A_QUESTION, word);
        return nullptr;
    }
    n00bb_query_t query = n00bb_query_named(word);
    if (query != N00BB_QUERY_MAY_EFFECT && query != N00BB_QUERY_FORBIDS_EFFECT
        && query != N00BB_QUERY_HAS_EFFECT) {
        query_report(parser, N00B_TEXT_QUERY_NOT_A_QUESTION,
                     N00BB_ARGS(ARG_TEXT(word)));
        return nullptr;
    }
    query_advance(parser, word);
    core_string_t open = query_peek(parser);
    bool parenthesized = query_is(open, CORE_STRING_STATIC("("));
    if (parenthesized) query_advance(parser, open);
    core_string_t name = query_peek(parser);
    if (!name.data || !query_word_byte(name.data[0]) || query_keyword(name)) {
        query_expected(parser, N00B_TEXT_QUERY_AN_EFFECT, name);
        return nullptr;
    }
    query_advance(parser, name);
    if (parenthesized) {
        core_string_t close = query_peek(parser);
        if (!query_is(close, CORE_STRING_STATIC(")"))) {
            query_expected(parser, N00B_TEXT_QUERY_CLOSING_PAREN, close);
            return nullptr;
        }
        query_advance(parser, close);
    }
    query_node_t *node = query_node(QUERY_LEAF, nullptr, nullptr);
    if (!node) return nullptr;
    node->query = query;
    if (!query_resolve_effect(parser, name, node)) {
        query_free(node);
        return nullptr;
    }
    return node;
}

static query_node_t *
query_unary(query_parser_t *parser)
{
    core_string_t token = query_peek(parser);
    if (query_is(token, CORE_STRING_STATIC("not"))
        || query_is(token, CORE_STRING_STATIC("!"))) {
        query_advance(parser, token);
        query_node_t *operand = query_unary(parser);
        return operand ? query_node(QUERY_NOT, operand, nullptr) : nullptr;
    }
    if (!query_is(token, CORE_STRING_STATIC("(")))
        return query_question(parser);
    query_advance(parser, token);
    query_node_t *inner = query_or(parser);
    if (!inner) return nullptr;
    core_string_t close = query_peek(parser);
    if (!query_is(close, CORE_STRING_STATIC(")"))) {
        query_expected(parser, N00B_TEXT_QUERY_CLOSING_PAREN, close);
        query_free(inner);
        return nullptr;
    }
    query_advance(parser, close);
    return inner;
}

static query_node_t *
query_and(query_parser_t *parser)
{
    query_node_t *left = query_unary(parser);
    while (left) {
        core_string_t token = query_peek(parser);
        if (!query_is(token, CORE_STRING_STATIC("and"))
            && !query_is(token, CORE_STRING_STATIC("&&")))
            break;
        query_advance(parser, token);
        query_node_t *right = query_unary(parser);
        if (!right) {
            query_free(left);
            return nullptr;
        }
        left = query_node(QUERY_AND, left, right);
    }
    return left;
}

static query_node_t *
query_or(query_parser_t *parser)
{
    query_node_t *left = query_and(parser);
    while (left) {
        core_string_t token = query_peek(parser);
        if (!query_is(token, CORE_STRING_STATIC("or"))
            && !query_is(token, CORE_STRING_STATIC("||")))
            break;
        query_advance(parser, token);
        query_node_t *right = query_and(parser);
        if (!right) {
            query_free(left);
            return nullptr;
        }
        left = query_node(QUERY_OR, left, right);
    }
    return left;
}

static query_node_t *
query_parse(n00bb_link_analysis_t *analysis, core_diagnostics_t *diagnostics,
            core_string_t text)
{
    query_parser_t parser = {
        .analysis = analysis, .diagnostics = diagnostics, .text = text,
    };
    query_node_t *tree = query_or(&parser);
    core_string_t rest = query_peek(&parser);
    if (tree && rest.data) {
        query_expected(&parser, N00B_TEXT_QUERY_THE_END, rest);
        query_free(tree);
        return nullptr;
    }
    return tree;
}

static bool
query_value(n00bb_link_analysis_t *analysis, const query_node_t *node,
            ir_func_t *function)
{
    switch (node->kind) {
    case QUERY_LEAF:
        return n00bb_query_answer(analysis->summaries, function, node->query,
                                  node->name, node->user);
    case QUERY_AND:
        return query_value(analysis, node->left, function)
            && query_value(analysis, node->right, function);
    case QUERY_OR:
        return query_value(analysis, node->left, function)
            || query_value(analysis, node->right, function);
    case QUERY_NOT:
        return !query_value(analysis, node->left, function);
    }
    return false;
}

static core_string_t
display_path(const core_translation_unit_t *unit)
{
    core_string_t path = core_source_file_path(&unit->source);
    core_string_t here = core_os_current_directory();
    size_t length = here.u8_bytes;
    bool inside = length && path.u8_bytes > length + 1
        && core_string_starts_with(path, here) && path.data[length] == '/';
    core_free(here.data);
    return inside ? core_string_slice(path, length + 1,
                                      path.u8_bytes - length - 1)
                  : path;
}

static void
append_location(core_buffer_t *out, const core_translation_unit_t *unit,
                uint32_t line, uint32_t column)
{
    core_buffer_append_string(out, display_path(unit));
    if (line) core_buffer_printf(out, ":%u:%u", line, column);
}

static void
query_why(n00bb_link_analysis_t *analysis, const query_node_t *node,
          ir_func_t *function, bool positive, core_buffer_t *out)
{
    if (node->kind == QUERY_NOT) {
        query_why(analysis, node->left, function, !positive, out);
        return;
    }
    if (node->kind != QUERY_LEAF) {
        query_why(analysis, node->left, function, positive, out);
        query_why(analysis, node->right, function, positive, out);
        return;
    }
    bool asks_may = node->query == N00BB_QUERY_FORBIDS_EFFECT
        ? !positive : positive;
    if (!asks_may || !n00bb_query_answer(analysis->summaries, function,
                                         N00BB_QUERY_MAY_EFFECT, node->name,
                                         node->user))
        return;
    core_list_t(chain_step_t) steps = core_list_new(chain_step_t);
    if (n00bb_effect_named(node->name))
        witness_chain_steps(analysis, function,
            bound_witness_flag(ir_summary_of(analysis->summaries, function),
                               node->name),
            node->name, &steps);
    else (void)named_chain_steps(analysis, function, node->name, &steps);
    if (!steps.len && node->user
        && n00bb_function_listed(&node->user->sources, function)) {
        core_string_t text = n00bb_text(N00B_TEXT_QUERY_IS_SOURCE,
            N00BB_ARGS(ARG_TEXT(n00bb_function_root(function->name)),
                       ARG_TEXT(node->name)));
        core_buffer_puts(out, "    ");
        core_buffer_append_string(out, text);
        core_buffer_putc(out, '\n');
        core_free(text.data);
    }
    for (size_t i = 0; i < steps.len; i++) {
        core_buffer_puts(out, "    ");
        if (steps.data[i].unit && steps.data[i].at.has_location) {
            append_location(out, steps.data[i].unit, steps.data[i].at.line,
                            steps.data[i].at.column);
            core_buffer_puts(out, "  ");
        }
        core_buffer_append_string(out, steps.data[i].text);
        core_buffer_putc(out, '\n');
    }
    chain_steps_release(&steps);
}

typedef struct {
    ir_func_t               *function;
    core_translation_unit_t *unit;
    core_string_t            qualified;
} query_candidate_t;
core_list_decl(query_candidate_t);

static bool
query_covers(core_string_t thing, core_string_t name)
{
    return thing.u8_bytes && core_string_starts_with(thing, name)
        && (thing.u8_bytes == name.u8_bytes
            || thing.data[name.u8_bytes] == '.');
}

static bool
candidate_covered(const query_candidate_t *candidate, core_string_t name)
{
    if (core_string_eq(name, CORE_STRING_STATIC("all"))) return true;
    return query_covers(candidate->qualified, name)
        || query_covers(n00bb_package_namespace(candidate->unit), name);
}

static bool
candidate_listed(const query_candidate_t *candidate, core_string_t list)
{
    for (core_string_t rest = list; rest.data;) {
        core_string_t name = next_effects_of_name(&rest);
        if (name.u8_bytes && candidate_covered(candidate, name)) return true;
    }
    return false;
}

static int
compare_candidates(const void *left, const void *right)
{
    const query_candidate_t *a = left;
    const query_candidate_t *b = right;
    int order = core_string_cmp(core_source_file_path(&a->unit->source),
                                core_source_file_path(&b->unit->source));
    if (order) return order;
    if (a->function->span.start_line != b->function->span.start_line)
        return a->function->span.start_line < b->function->span.start_line
            ? -1 : 1;
    return core_string_cmp(a->qualified, b->qualified);
}

static core_list_t(query_candidate_t)
query_candidates(n00bb_link_analysis_t *analysis)
{
    core_list_t(query_candidate_t) found = core_list_new(query_candidate_t);
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_scope_t *scope = state ? module_scope(state) : nullptr;
        for (parse_sym_entry_t *symbol = scope ? scope->first_in_scope
                                               : nullptr;
             symbol; symbol = symbol->next_in_scope) {
            if (symbol->kind != PARSE_SYM_FUNCTION) continue;
            ir_func_t *function = n00bb_cached_body(
                link_ir_function_for_symbol(analysis,
                                            canonical_symbol(symbol)));
            if (!function || !function->n_blocks
                || !core_string_eq(ir_module_name(function->owner),
                                   unit->module.identifier))
                continue;
            bool seen = false;
            for (size_t i = 0; !seen && i < found.len; i++)
                seen = found.data[i].function == function;
            if (seen) continue;
            core_buffer_t name = {0};
            function_name_text(&name, function);
            core_list_push(found, ((query_candidate_t){
                .function = function, .unit = unit,
                .qualified = core_string_dup(core_buffer_string(&name)),
            }));
            core_buffer_release(&name);
        }
    }
    if (found.len)
        qsort(found.data, found.len, sizeof(*found.data), compare_candidates);
    return found;
}

static bool
query_scope_known(n00bb_link_analysis_t *analysis, core_diagnostics_t *d,
                  const core_list_t(query_candidate_t) *candidates,
                  core_string_t list)
{
    bool known = true;
    for (core_string_t rest = list; rest.data;) {
        core_string_t name = next_effects_of_name(&rest);
        if (!name.u8_bytes) continue;
        bool covered = false;
        for (size_t i = 0; !covered && i < candidates->len; i++)
            covered = candidate_covered(&candidates->data[i], name);
        if (covered) continue;
        n00bb_report(d, nullptr, N00B_DIAG_EFFECTS_SCOPE_UNKNOWN,
                     (core_diag_location_t){}, N00B_TEXT_SCOPE_NAMES_NOTHING,
                     N00BB_ARGS(ARG_TEXT(name)));
        known = false;
    }
    (void)analysis;
    return known;
}

static bool
candidate_in_scope(n00bb_link_analysis_t *analysis,
                   const query_candidate_t *candidate,
                   const core_translation_unit_t *root)
{
    if (analysis->effects_in.data)
        return candidate_listed(candidate, analysis->effects_in);
    n00bb_tu_state_t *state = tu_state(candidate->unit);
    core_string_t package = n00bb_package_id(candidate->unit);
    return state && !state->interface_module && !toolchain_package(package)
        && core_string_eq(package, n00bb_package_id(root));
}

bool
dump_effects_query(n00bb_link_analysis_t *analysis,
                   core_translation_unit_t *unit)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    query_node_t *query = query_parse(analysis, diagnostics,
                                      analysis->effects_query);
    if (!query) return false;
    core_list_t(query_candidate_t) candidates = query_candidates(analysis);
    bool ok = query_scope_known(analysis, diagnostics, &candidates,
                                analysis->effects_in)
        && query_scope_known(analysis, diagnostics, &candidates,
                             analysis->effects_not_in);
    core_buffer_t out = {0};
    size_t matched = 0;
    for (size_t i = 0; ok && i < candidates.len; i++) {
        query_candidate_t *candidate = &candidates.data[i];
        if (!candidate_in_scope(analysis, candidate, unit)
            || (analysis->effects_not_in.data
                && candidate_listed(candidate, analysis->effects_not_in))
            || !query_value(analysis, query, candidate->function))
            continue;
        if (analysis->effects_json) {
            core_buffer_printf(&out, "%s", matched ? ",\n {" : "[{");
            core_buffer_puts(&out, "\"function\":");
            core_buffer_append_quoted(&out, candidate->qualified);
            core_buffer_puts(&out, ",\"path\":");
            core_buffer_append_quoted(&out, display_path(candidate->unit));
            core_buffer_printf(&out, ",\"line\":%u,\"column\":%u}",
                               candidate->function->span.start_line,
                               candidate->function->span.start_col);
        }
        else {
            append_location(&out, candidate->unit,
                            candidate->function->span.start_line,
                            candidate->function->span.start_col);
            core_buffer_puts(&out, "  ");
            core_buffer_append_string(&out, candidate->qualified);
            core_buffer_putc(&out, '\n');
            if (analysis->effects_why)
                query_why(analysis, query, candidate->function, true, &out);
        }
        matched++;
    }
    if (ok && analysis->effects_json && matched)
        core_buffer_puts(&out, "]\n");
    else if (ok && matched) {
        core_string_t summary = n00bb_text(N00B_TEXT_FUNCTIONS_MATCH,
            N00BB_ARGS(ARG_NUMBER(matched)));
        core_buffer_append_string(&out, summary);
        core_buffer_putc(&out, '\n');
        core_free(summary.data);
    }
    dump_text(&out);
    for (size_t i = 0; i < candidates.len; i++)
        core_free(candidates.data[i].qualified.data);
    core_list_free(candidates);
    query_free(query);
    return ok;
}
