#include "internal.h"

static uint64_t
hash_bytes(const core_buffer_t *bytes)
{
    return core_xxh3_64bits_raw(bytes->data ? bytes->data : "",
                                bytes->byte_len) | 1;
}

static uint64_t
function_name_hash(const ir_func_t *function)
{
    const ir_func_t *body = function
        ? ir_func_definition((ir_func_t *)function) : nullptr;
    if (!body) return 0;
    core_buffer_t text = {0};
    if (body->owner)
        core_buffer_append_string(&text,
                                  ir_module_name((ir_module_t *)body->owner));
    core_buffer_putc(&text, 0);
    core_buffer_append_string(&text, body->name);
    uint64_t hash = hash_bytes(&text);
    core_buffer_release(&text);
    return hash;
}

static int
compare_hashes(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return a < b ? -1 : a > b;
}

static uint64_t
functions_answer(ir_func_t *const *functions, size_t count, uint64_t tag)
{
    uint64_t *hashes = core_alloc_array(uint64_t, count + 1);
    hashes[0] = tag;
    for (size_t i = 0; i < count; i++)
        hashes[i + 1] = function_name_hash(functions[i]);
    qsort(hashes + 1, count, sizeof(uint64_t), compare_hashes);
    uint64_t answer = core_xxh3_64bits_raw(hashes,
                                           (count + 1) * sizeof(uint64_t)) | 1;
    core_free(hashes);
    return answer;
}

static uint64_t
stable_content(const ir_summary_t *summary)
{
    if (!summary) return 0;
    if (summary->stable) return summary->stable;
    core_buffer_t bytes = {0};
    (void)ir_summary_encode(summary, &bytes);
    uint64_t hash = hash_bytes(&bytes);
    core_buffer_release(&bytes);
    ((ir_summary_t *)summary)->stable = hash;
    return hash;
}

static bool
same_input(const summary_input_t *a, const summary_input_t *b)
{
    return a->kind == b->kind && a->site == b->site && a->caller == b->caller
        && a->function == b->function && a->global == b->global
        && a->module == b->module && core_string_eq(a->effect, b->effect);
}

static bool
input_wanted(ir_summaries_t *summaries, const summary_input_t *probe)
{
    if (!summaries || !summaries->sink) return false;
    for (size_t i = 0; i < summaries->sink->len; i++)
        if (same_input(&summaries->sink->data[i], probe)) return false;
    return true;
}

static void
record_input(ir_summaries_t *summaries, summary_input_t input)
{
    if (!input_wanted(summaries, &input)) return;
    const ir_func_t *owner = input.caller ? input.caller : input.function;
    if (input.effect.u8_bytes && owner && owner->owner)
        input.effect = ir_strdup(owner->owner->ctx, input.effect);
    core_list_push(*summaries->sink, input);
}

static bool
solving_member(const ir_summaries_t *summaries, const ir_func_t *function)
{
    for (size_t i = 0; function && i < summaries->member_count; i++)
        if (summaries->members[i] == function) return true;
    return false;
}

static const ir_func_t *
call_owner(const ir_instr_t *call)
{
    return call && call->block ? call->block->owner : nullptr;
}

static uint32_t
call_site(const ir_instr_t *call)
{
    const ir_func_t *f = call_owner(call);
    uint32_t at = 0;
    for (uint32_t b = 0; f && b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++, at++)
            if (f->blocks[b]->instrs[i] == call) return at;
    return UINT32_MAX;
}

static const ir_instr_t *
site_instr(const ir_func_t *f, uint32_t site)
{
    uint32_t at = 0;
    for (uint32_t b = 0; f && b < f->n_blocks; b++) {
        if (site - at < f->blocks[b]->n_instrs)
            return f->blocks[b]->instrs[site - at];
        at += f->blocks[b]->n_instrs;
    }
    return nullptr;
}

static uint64_t
targets_answer(const call_targets_t *targets, bool hooked)
{
    return functions_answer(targets->targets.data, targets->targets.len,
                            (uint64_t)hooked << 1 | targets->unknown);
}

static bool
plain_call(const ir_instr_t *call, const call_targets_t *targets, bool hooked)
{
    ir_value_t *callee = call->op == IR_OP_CALL && call->n_operands
        ? ir_operand_value(&call->operands[0]) : nullptr;
    return hooked && !targets->unknown && targets->targets.len == 1
        && callee && callee->kind == IR_VAL_FUNC
        && callee->owner == targets->targets.data[0];
}

static void
note_targets(ir_summaries_t *summaries, const ir_instr_t *call,
             const call_targets_t *targets, bool hooked)
{
    const ir_func_t *owner = call_owner(call);
    if (!summaries->sink || !solving_member(summaries, owner)
        || plain_call(call, targets, hooked))
        return;
    summary_input_t input = {.kind = SUMMARY_INPUT_TARGETS, .caller = owner,
                             .site = call_site(call)};
    if (!input_wanted(summaries, &input)) return;
    input.answer = targets_answer(targets, hooked);
    record_input(summaries, input);
}

static uint64_t
finalizers_answer(const core_list_t(summary_func_ptr_t) *finalizers,
                  bool bound)
{
    return functions_answer(finalizers->data, finalizers->len, bound);
}

static void
note_finalizers(ir_summaries_t *summaries, const ir_instr_t *call,
                const ir_func_t *target,
                const core_list_t(summary_func_ptr_t) *finalizers, bool bound)
{
    const ir_func_t *owner = call_owner(call);
    if (!summaries->sink || !solving_member(summaries, owner)) return;
    record_input(summaries, (summary_input_t){
        .kind = SUMMARY_INPUT_FINALIZERS, .caller = owner,
        .site = call_site(call), .function = target,
        .answer = finalizers_answer(finalizers, bound)});
}

static void
note_entries(ir_summaries_t *summaries, uint32_t flag,
             const ir_summary_t *entry)
{
    summary_input_t input = {.kind = SUMMARY_INPUT_ENTRIES, .site = flag};
    if (!input_wanted(summaries, &input)) return;
    input.answer = stable_content(entry);
    record_input(summaries, input);
}

static void
note_global(ir_summaries_t *summaries, const ir_global_t *global,
            bool reachable)
{
    record_input(summaries, (summary_input_t){
        .kind = SUMMARY_INPUT_GLOBAL, .global = global,
        .answer = reachable ? 2 : 1});
}

static bool
inherits_answer(ir_summaries_t *summaries, const ir_func_t *caller,
                const ir_func_t *callee, core_string_t effect)
{
    const ir_summary_hooks_t *hooks = summaries->hooks;
    return !hooks || !hooks->inherits_effect
        || hooks->inherits_effect(hooks->context, caller, callee, effect);
}

static void
note_inherits(ir_summaries_t *summaries, const ir_func_t *caller,
              const ir_func_t *callee, core_string_t effect, bool inherits)
{
    record_input(summaries, (summary_input_t){
        .kind = SUMMARY_INPUT_INHERITS, .caller = caller, .function = callee,
        .effect = effect, .answer = inherits ? 2 : 1});
}

static uint64_t
effects_answer(ir_summaries_t *summaries, const ir_func_t *caller,
               const ir_func_t *callee)
{
    const ir_summary_hooks_t *hooks = summaries->hooks;
    if (!hooks || !hooks->callee_effects) return 1;
    ir_summary_t *probe = ir_summary_new(nullptr, caller);
    hooks->callee_effects(hooks->context, callee, probe);
    uint64_t *hashes = core_alloc_array(uint64_t, probe->effects.len + 1);
    hashes[0] = probe->flags;
    for (size_t e = 0; e < probe->effects.len; e++)
        hashes[e + 1] = core_string_hash(probe->effects.data[e]);
    qsort(hashes + 1, probe->effects.len, sizeof(uint64_t), compare_hashes);
    uint64_t answer = probe->flags || probe->effects.len
        ? core_xxh3_64bits_raw(hashes,
              (probe->effects.len + 1) * sizeof(uint64_t)) | 1
        : 1;
    core_free(hashes);
    summary_free(probe);
    return answer;
}

static void
note_effects(ir_summaries_t *summaries, const ir_func_t *caller,
             const ir_func_t *callee)
{
    summary_input_t input = {.kind = SUMMARY_INPUT_EFFECTS, .caller = caller,
                             .function = callee};
    if (!input_wanted(summaries, &input)) return;
    input.answer = effects_answer(summaries, caller, callee);
    record_input(summaries, input);
}

static void
observe_view(void *context, const ir_module_t *viewer)
{
    ir_summaries_t *summaries = context;
    summary_input_t input = {.kind = SUMMARY_INPUT_VIEW, .module = viewer};
    if (!input_wanted(summaries, &input)) return;
    input.answer = ir_reinterpretations_fingerprint((ir_module_t *)viewer);
    record_input(summaries, input);
}

static void
merge_inputs(core_list_t(summary_input_t) *into,
             const core_list_t(summary_input_t) *from)
{
    for (size_t i = 0; i < from->len; i++) {
        bool known = false;
        for (size_t j = 0; !known && j < into->len; j++)
            known = same_input(&into->data[j], &from->data[i]);
        if (!known) core_list_push(*into, from->data[i]);
    }
}

static uint64_t
current_answer(ir_summaries_t *summaries, const summary_input_t *input)
{
    const ir_summary_hooks_t *hooks = summaries->hooks;
    switch (input->kind) {
    case SUMMARY_INPUT_TARGETS: {
        const ir_instr_t *call = site_instr(input->caller, input->site);
        if (!call) return 0;
        call_targets_t targets;
        bool hooked = resolve_targets_hooked(summaries, call, &targets);
        uint64_t answer = targets_answer(&targets, hooked);
        core_list_free(targets.targets);
        return answer;
    }
    case SUMMARY_INPUT_FINALIZERS: {
        const ir_instr_t *call = site_instr(input->caller, input->site);
        if (!call || !hooks || !hooks->call_finalizers) return 0;
        core_list_t(summary_func_ptr_t) finalizers =
            core_list_new(summary_func_ptr_t);
        bool bound = hooks->call_finalizers(hooks->context, call,
                                            input->function, add_finalizer,
                                            &finalizers);
        uint64_t answer = finalizers_answer(&finalizers, bound);
        core_list_free(finalizers);
        return answer;
    }
    case SUMMARY_INPUT_ENTRIES:
        return stable_content(ir_summary_entries(summaries, input->site));
    case SUMMARY_INPUT_GLOBAL:
        return !hooks || !hooks->global_reachable
            || hooks->global_reachable(hooks->context, input->global)
            ? 2 : 1;
    case SUMMARY_INPUT_INHERITS:
        return inherits_answer(summaries, input->caller, input->function,
                               input->effect) ? 2 : 1;
    case SUMMARY_INPUT_EFFECTS:
        return effects_answer(summaries, input->caller, input->function);
    case SUMMARY_INPUT_VIEW:
        return ir_reinterpretations_fingerprint((ir_module_t *)input->module);
    }
    return 0;
}

static bool
inputs_hold(ir_summaries_t *summaries,
            const core_list_t(summary_input_t) *inputs)
{
    core_list_t(summary_input_t) *sink = summaries->sink;
    ir_summary_t *recording = summaries->recording;
    summaries->sink      = nullptr;
    summaries->recording = nullptr;
    bool holds = true;
    for (size_t i = 0; holds && i < inputs->len; i++)
        holds = current_answer(summaries, &inputs->data[i])
            == inputs->data[i].answer;
    summaries->sink      = sink;
    summaries->recording = recording;
    return holds;
}

static bool
stored_deps_hold(ir_summaries_t *summaries, const ir_summary_t *summary,
                 ir_func_t *const *members, size_t count)
{
    ir_summary_t *recording = summaries->recording;
    summaries->recording = nullptr;
    bool holds = true;
    for (size_t d = 0; holds && d < summary->deps.len; d++) {
        const summary_dep_t *dep = &summary->deps.data[d];
        if (member_index(members, count, dep->function) < count) continue;
        holds = stable_content(target_summary(summaries,
                    (ir_func_t *)dep->function)) == dep->stable;
    }
    summaries->recording = recording;
    return holds;
}

static void
put_u64(core_buffer_t *out, uint64_t value)
{
    put_u32(out, (uint32_t)value);
    put_u32(out, (uint32_t)(value >> 32));
}

static uint64_t
get_u64(summary_reader_t *r)
{
    uint64_t low = get_u32(r);
    return low | (uint64_t)get_u32(r) << 32;
}

static void
put_function(core_buffer_t *out, const ir_func_t *function)
{
    const ir_func_t *body = function
        ? ir_func_definition((ir_func_t *)function) : nullptr;
    put_text(out, nullptr, body && body->owner
        ? ir_module_name((ir_module_t *)body->owner) : (core_string_t){0});
    put_text(out, nullptr, body ? body->name : (core_string_t){0});
}

static bool
get_function(summary_reader_t *r, const ir_func_t **out)
{
    core_string_t module = get_text(r);
    core_string_t name   = get_text(r);
    *out = nullptr;
    if (r->failed) return false;
    if (!name.u8_bytes) return true;
    ir_func_t *found = r->resolver && r->resolver->function
        ? r->resolver->function(r->resolver->context, module, name) : nullptr;
    *out = found ? ir_func_definition(found) : nullptr;
    return *out != nullptr;
}

bool
ir_summary_encode_stored(ir_summaries_t *summaries,
                         const ir_func_t *function, uint64_t body,
                         core_buffer_t *out)
{
    ir_summary_t *summary = ir_summary_of(summaries, function);
    if (!summary) return false;
    core_buffer_t codec = {0};
    bool ok = ir_summary_encode(summary, &codec);
    put_u32(out, (uint32_t)codec.byte_len);
    core_buffer_append(out, codec.data, codec.byte_len);
    core_buffer_release(&codec);
    put_u8(out, (uint8_t)(summary->proof_free | summary->bookkept << 1));
    put_u64(out, body);
    put_u32(out, (uint32_t)summary->deps.len);
    for (size_t d = 0; d < summary->deps.len; d++) {
        const ir_func_t *dep = summary->deps.data[d].function;
        put_function(out, dep);
        put_u64(out, stable_content(target_summary(summaries,
                                                   (ir_func_t *)dep)));
    }
    put_u32(out, (uint32_t)summary->inputs.len);
    for (size_t i = 0; i < summary->inputs.len; i++) {
        const summary_input_t *input = &summary->inputs.data[i];
        put_u8(out, input->kind);
        put_u32(out, input->site);
        put_function(out, input->caller);
        put_function(out, input->function);
        put_global(out, nullptr, input->global);
        put_text(out, nullptr, input->module
            ? ir_module_name((ir_module_t *)input->module)
            : (core_string_t){0});
        put_text(out, nullptr, input->effect);
        put_u64(out, input->answer);
    }
    return ok;
}

ir_summary_t *
ir_summary_decode_stored(ir_summaries_t *summaries,
                         const ir_func_t *function, const uint8_t *data,
                         size_t size, const ir_summary_resolver_t *resolver,
                         ir_summary_module_fn module_named,
                         ir_summary_body_fn body_matches)
{
    summary_reader_t r = {.data = data, .size = size, .resolver = resolver};
    uint32_t codec = get_u32(&r);
    if (r.failed || codec > size - r.at) return nullptr;
    ir_summary_t *summary = decode_summary(nullptr, function, data + r.at,
                                           codec, resolver, nullptr);
    if (!summary) return nullptr;
    r.at += codec;
    uint8_t extras = get_u8(&r);
    summary->proof_free = extras & 1;
    summary->bookkept   = extras >> 1 & 1;
    uint64_t body = get_u64(&r);
    if (!r.failed
        && (!body_matches
            || !body_matches(resolver ? resolver->context : nullptr,
                             function, body)))
        r.failed = true;
    uint32_t deps = get_u32(&r);
    for (uint32_t d = 0; !r.failed && d < deps; d++) {
        summary_dep_t dep = {0};
        if (!get_function(&r, &dep.function) || !dep.function) {
            r.failed = true;
            break;
        }
        dep.stable = get_u64(&r);
        core_list_push(summary->deps, dep);
    }
    uint32_t inputs = get_u32(&r);
    for (uint32_t i = 0; !r.failed && i < inputs; i++) {
        summary_input_t input = {.kind = get_u8(&r)};
        input.site = get_u32(&r);
        if (!get_function(&r, &input.caller)
            || !get_function(&r, &input.function)) {
            r.failed = true;
            break;
        }
        core_string_t global_module = get_text(&r);
        core_string_t global_name   = get_text(&r);
        if (global_name.u8_bytes) {
            input.global = resolver && resolver->global
                ? resolver->global(resolver->context, global_module,
                                   global_name)
                : nullptr;
            if (!input.global) r.failed = true;
        }
        core_string_t module = get_text(&r);
        if (module.u8_bytes) {
            input.module = module_named
                ? module_named(resolver ? resolver->context : nullptr,
                               module)
                : nullptr;
            if (!input.module) r.failed = true;
        }
        core_string_t effect = get_text(&r);
        input.effect = effect.u8_bytes && function && function->owner
            ? ir_strdup(function->owner->ctx, effect) : (core_string_t){0};
        input.answer = get_u64(&r);
        if (input.kind > SUMMARY_INPUT_VIEW) r.failed = true;
        if (!r.failed) core_list_push(summary->inputs, input);
    }
    if (r.failed || r.at != r.size) {
        summary_free(summary);
        return nullptr;
    }
    if (summaries) core_list_push(summaries->owned, summary);
    return summary;
}
