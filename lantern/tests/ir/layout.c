#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/layout.h"
#include "ir/trait.h"
#include "ir/rule.h"
#include "ir/text.h"
#include "ir/verify.h"
#include "harness.h"

static bool produced[IR_DIAG_COUNT];

static void test_trait_vtable_dispatch_call(void) {
    ir_ctx_t *ctx = ir_ctx_new();
    th_ctx *types = ir_ctx_types(ctx);
    ir_module_t *module = ir_module_new(ctx, "trait_dispatch",
                                        &ir_profile_bce, nullptr);
    th_type *object = ir_trait_value_type(types);
    th_type *data = th_record_lookup(object, IR_TRAIT_VALUE_DATA_FIELD);
    th_type *integer = th_name(types, "int");
    th_type *string = th_name(types, "string");
    th_param lowered_params[] = {
        {.mode = TH_IN, .type = data},
        {.mode = TH_IN, .type = integer},
    };
    th_param lowered_rest = {.mode = TH_IN, .type = integer};
    th_kwparam lowered_keywords[] = {
        {.name = "format", .mode = TH_IN, .type = string},
    };
    th_type *lowered_signature =
        th_func(types, string, lowered_params, 2, &lowered_rest,
                lowered_keywords, 1);
    const char *methods[] = {"render"};
    th_type *signatures[] = {lowered_signature};
    th_type *vtable_type =
        ir_trait_vtable_type(types, methods, signatures, 1);
    th_param caller_params[] = {
        {.mode = TH_IN, .type = object},
        {.mode = TH_IN, .type = integer},
    };
    th_param caller_rest = {.mode = TH_IN, .type = integer};
    th_kwparam caller_keywords[] = {
        {.name = "format", .mode = TH_IN, .type = string},
    };
    ir_func_t *function = ir_func_add(
        module, "invoke", th_func(types, string, caller_params, 2, &caller_rest,
                                  caller_keywords, 1), IR_LINK_INTERNAL);
    ir_value_t *value = ir_param_add(function, "value", object);
    ir_value_t *argument = ir_param_add(function, "argument", integer);
    ir_value_t *format = ir_kwparam_add(function, "format", string);
    ir_value_t *extras = ir_restparam_add(function, "extras", integer);
    bool rejects_duplicate_rest_binding = extras &&
        !ir_restparam_add(function, "other_extras", integer);
    ir_block_t *block = ir_block_add(function, "entry");
    ir_value_t *args[] = {argument};
    ir_trait_named_arg_t named[] = {{.name = "format", .value = format}};
    ir_value_t *result = ir_trait_method_call_with_rest(
        types, block, "result", value, vtable_type, "render", args, 1,
        extras, named, 1);
    ir_instr_t *call = result ? result->def : nullptr;
    th_param bad_params[] = {
        {.mode = TH_IN, .type = object},
        {.mode = TH_IN, .type = integer},
    };
    th_param bad_rest = {.mode = TH_IN, .type = string};
    ir_func_t *bad_function = ir_func_add(
        module, "bad_rest_call",
        th_func(types, string, bad_params, 2, &bad_rest, nullptr, 0),
        IR_LINK_INTERNAL);
    ir_value_t *bad_value = ir_param_add(bad_function, "value", object);
    ir_value_t *bad_argument = ir_param_add(bad_function, "argument", integer);
    ir_value_t *bad_extras = ir_restparam_add(bad_function, "extras", string);
    ir_block_t *bad_block = ir_block_add(bad_function, "entry");
    ir_value_t *bad_args[] = {bad_argument};
    bool rejects_wrong_rest_type = bad_block &&
        !ir_trait_method_call_with_rest(
            types, bad_block, "bad_result", bad_value, vtable_type,
            "render", bad_args, 1, bad_extras, named, 1);
    ir_rules_t *rules = ir_rules_from_profile(ctx, &ir_profile_bce);
    ir_diags_t *diagnostics = ir_diags_new(ctx, nullptr);
    bool typed = vtable_type && result && call &&
        th_kind_of(vtable_type) == TH_RECORD &&
        ir_type_eq(module, result->type, string) &&
        call->n_operands == 5 &&
        call->operands[0].value->def->op == IR_OP_LOAD &&
        call->operands[1].value->def->op == IR_OP_EXTRACT &&
        call->operands[2].value == argument &&
        call->operands[3].kind == IR_OPD_SPREAD &&
        call->operands[3].value == extras &&
        call->operands[4].kind == IR_OPD_NAMED_ARG &&
        !strcmp(call->operands[4].named.name, "format") &&
        call->operands[4].named.value == format;
    bool accepted = typed && rules && ir_rules_add_defaults(rules);
    for (uint32_t i = 0; accepted && i < block->n_instrs; i++)
        accepted = ir_rule_apply(module, rules, block->instrs[i], diagnostics)
                       .verdict == IR_RULE_ACCEPT;
    ir_instr_t *early_spread = accepted
        ? ir_instr_add(block, IR_OP_CALL) : nullptr;
    bool early_spread_built = early_spread &&
        ir_instr_set_dest(early_spread, "early_spread_result", string) &&
        ir_instr_operand_value(early_spread, call->operands[0].value) &&
        ir_instr_operand_value(early_spread, call->operands[1].value) &&
        ir_instr_operand_spread(early_spread, extras) &&
        ir_instr_operand_value(early_spread, argument);
    bool early_spread_rejected = early_spread_built &&
        ir_rule_apply(module, rules, early_spread, diagnostics).verdict ==
            IR_RULE_REJECT;
    ir_instr_t *late_positional = accepted
        ? ir_instr_add(block, IR_OP_CALL) : nullptr;
    bool late_positional_built = late_positional &&
        ir_instr_set_dest(late_positional, "late_positional_result", string) &&
        ir_instr_operand_value(late_positional, call->operands[0].value) &&
        ir_instr_operand_value(late_positional, call->operands[1].value) &&
        ir_instr_operand_value(late_positional, argument) &&
        ir_instr_operand_spread(late_positional, extras) &&
        ir_instr_operand_value(late_positional, argument);
    bool late_positional_rejected = late_positional_built &&
        ir_rule_apply(module, rules, late_positional, diagnostics).verdict ==
            IR_RULE_REJECT;
    check("trait vtable slots lower to typed indirect method calls",
          accepted);
    check("a rest spread follows all fixed positional arguments",
          early_spread_rejected);
    check("positional arguments cannot follow a rest spread",
          late_positional_rejected);
    check("trait method calls reject a rest argument with the wrong type",
          rejects_wrong_rest_type);
    check("a function binds its rest slot to only one IR value",
          rejects_duplicate_rest_binding);
    check("trait vtable rejects incompatible erased receiver signatures",
          !ir_trait_vtable_type(types, methods,
              (th_type *[]){th_func(types, string,
                  (th_param[]){{.mode = TH_IN, .type = integer}}, 1,
                  nullptr, nullptr, 0)}, 1));
    check("trait methods cannot collide with the vtable type-info field",
          !ir_trait_vtable_type(types,
              (const char *const[]){IR_TRAIT_VTABLE_TYPE_INFO_FIELD},
              signatures, 1));
    ir_ctx_free(ctx);
}

static void test_trait_receiver_adapters(void) {
    ir_ctx_t *ctx = ir_ctx_new();
    th_ctx *types = ir_ctx_types(ctx);
    ir_module_t *module = ir_module_new(ctx, "trait_adapters",
                                        &ir_profile_bce, nullptr);
    th_type *object = ir_trait_value_type(types);
    th_type *data = th_record_lookup(object, IR_TRAIT_VALUE_DATA_FIELD);
    th_type *meter = th_name(types, "Meter");
    th_type *integer = th_name(types, "int");
    th_type *string = th_name(types, "string");
    th_param implementation_params[] = {
        {.mode = TH_IN, .type = meter},
        {.mode = TH_IN, .type = integer},
    };
    ir_func_t *implementation = ir_func_add(module, "meter_render",
        th_func(types, th_name(types, "void"), implementation_params, 2,
                nullptr, nullptr, 0),
        IR_LINK_EXTERNAL);
    ir_instr_add(ir_block_add(implementation, "entry"), IR_OP_RETURN);
    th_param slot_params[] = {
        {.mode = TH_IN, .type = data},
        {.mode = TH_IN, .type = integer},
    };
    th_type *slot_signature =
        th_func(types, th_name(types, "void"), slot_params, 2,
                nullptr, nullptr, 0);
    ir_func_t *adapter = ir_trait_receiver_adapter_add(
        module, "meter_render_adapter", implementation, slot_signature);
    ir_block_t *entry = adapter ? ir_block_entry(adapter) : nullptr;
    ir_instr_t *call = entry && entry->n_instrs > 2
        ? entry->instrs[2] : nullptr;
    bool by_value = entry && entry->n_instrs == 4 &&
        entry->instrs[0]->op == IR_OP_BITCAST &&
        entry->instrs[1]->op == IR_OP_LOAD && call->op == IR_OP_CALL &&
        call->operands[0].value->kind == IR_VAL_FUNC &&
        call->operands[1].value == entry->instrs[1]->dest &&
        call->operands[2].value == adapter->params[1];

    th_param mutable_receiver = {.mode = TH_INOUT, .type = meter};
    ir_func_t *mutator = ir_func_add(module, "meter_update",
        th_func(types, th_name(types, "void"), &mutable_receiver, 1,
                nullptr, nullptr, 0), IR_LINK_EXTERNAL);
    ir_instr_add(ir_block_add(mutator, "entry"), IR_OP_RETURN);
    th_param mutable_slot = {.mode = TH_IN, .type = data};
    ir_func_t *mutable_adapter = ir_trait_receiver_adapter_add(
        module, "meter_update_adapter", mutator,
        th_func(types, th_name(types, "void"), &mutable_slot, 1,
                nullptr, nullptr, 0));
    ir_block_t *mutable_entry = mutable_adapter
        ? ir_block_entry(mutable_adapter) : nullptr;
    ir_instr_t *mutable_call = mutable_entry && mutable_entry->n_instrs > 1
        ? mutable_entry->instrs[1] : nullptr;
    bool mutable_receiver_is_storage = mutable_entry &&
        mutable_entry->n_instrs == 3 &&
        mutable_entry->instrs[0]->op == IR_OP_BITCAST &&
        mutable_call->op == IR_OP_CALL &&
        mutable_call->operands[1].value == mutable_entry->instrs[0]->dest &&
        mutable_entry->instrs[2]->op == IR_OP_RETURN;

    th_kwparam implementation_keyword = {
        .name = "format", .mode = TH_IN, .type = string,
    };
    ir_func_t *keyword_impl = ir_func_add(module, "meter_format",
        th_func(types, th_name(types, "void"), &mutable_receiver, 1,
                nullptr, &implementation_keyword, 1), IR_LINK_EXTERNAL);
    ir_instr_add(ir_block_add(keyword_impl, "entry"), IR_OP_RETURN);
    th_kwparam slot_keyword = {
        .name = "format", .mode = TH_IN, .type = string,
    };
    ir_func_t *keyword_adapter = ir_trait_receiver_adapter_add(
        module, "meter_format_adapter", keyword_impl,
        th_func(types, th_name(types, "void"), &mutable_slot, 1,
                nullptr, &slot_keyword, 1));
    ir_block_t *keyword_entry = keyword_adapter
        ? ir_block_entry(keyword_adapter) : nullptr;
    ir_instr_t *keyword_call = keyword_entry && keyword_entry->n_instrs > 1
        ? keyword_entry->instrs[1] : nullptr;
    bool keyword_forwarded = keyword_adapter && keyword_entry &&
        keyword_adapter->n_params == 2 &&
        keyword_adapter->params[1]->is_keyword && keyword_call &&
        keyword_call->operands[2].kind == IR_OPD_NAMED_ARG &&
        !strcmp(keyword_call->operands[2].named.name, "format") &&
        keyword_call->operands[2].named.value == keyword_adapter->params[1];

    th_param variadic_receiver = {.mode = TH_IN, .type = meter};
    th_param implementation_rest = {.mode = TH_IN, .type = integer};
    ir_func_t *variadic_impl = ir_func_add(module, "meter_sum",
        th_func(types, th_name(types, "void"), &variadic_receiver, 1,
                &implementation_rest, nullptr, 0), IR_LINK_EXTERNAL);
    ir_restparam_add(variadic_impl, "extras", integer);
    ir_instr_add(ir_block_add(variadic_impl, "entry"), IR_OP_RETURN);
    th_param erased_receiver = {.mode = TH_IN, .type = data};
    th_param slot_rest = {.mode = TH_IN, .type = integer};
    ir_func_t *variadic_adapter = ir_trait_receiver_adapter_add(
        module, "meter_sum_adapter", variadic_impl,
        th_func(types, th_name(types, "void"), &erased_receiver, 1,
                &slot_rest, nullptr, 0));
    ir_block_t *variadic_entry = variadic_adapter
        ? ir_block_entry(variadic_adapter) : nullptr;
    ir_instr_t *variadic_call = variadic_entry && variadic_entry->n_instrs > 2
        ? variadic_entry->instrs[2] : nullptr;
    bool rest_forwarded = variadic_adapter && variadic_entry &&
        variadic_adapter->n_params == 2 &&
        variadic_adapter->params[1]->is_rest && variadic_call &&
        variadic_call->operands[2].kind == IR_OPD_SPREAD &&
        variadic_call->operands[2].value == variadic_adapter->params[1];

    ir_rules_t *rules = ir_rules_from_profile(ctx, &ir_profile_bce);
    ir_diags_t *diagnostics = ir_diags_new(ctx, nullptr);
    bool valid = rules && keyword_entry && variadic_entry &&
                 ir_rules_add_defaults(rules);
    for (uint32_t i = 0; valid && i < entry->n_instrs; i++)
        valid = ir_rule_apply(module, rules, entry->instrs[i], diagnostics)
                    .verdict == IR_RULE_ACCEPT;
    for (uint32_t i = 0; valid && i < mutable_entry->n_instrs; i++)
        valid = ir_rule_apply(module, rules, mutable_entry->instrs[i],
                              diagnostics).verdict == IR_RULE_ACCEPT;
    for (uint32_t i = 0; valid && i < keyword_entry->n_instrs; i++)
        valid = ir_rule_apply(module, rules, keyword_entry->instrs[i],
                              diagnostics).verdict == IR_RULE_ACCEPT;
    for (uint32_t i = 0; valid && i < variadic_entry->n_instrs; i++)
        valid = ir_rule_apply(module, rules, variadic_entry->instrs[i],
                              diagnostics).verdict == IR_RULE_ACCEPT;
    ir_instr_t *duplicate_keyword_call = valid
        ? ir_instr_add(keyword_entry, IR_OP_CALL) : nullptr;
    bool duplicate_call_built = duplicate_keyword_call &&
        ir_instr_set_dest(duplicate_keyword_call, "duplicate_result", string) &&
        ir_instr_operand_value(duplicate_keyword_call,
                               keyword_call->operands[0].value) &&
        ir_instr_operand_value(duplicate_keyword_call,
                               keyword_call->operands[1].value) &&
        ir_instr_operand_named(duplicate_keyword_call, "format",
                               keyword_adapter->params[1]) &&
        ir_instr_operand_named(duplicate_keyword_call, "format",
                               keyword_adapter->params[1]);
    bool duplicate_keyword_rejected = duplicate_call_built &&
        ir_rule_apply(module, rules, duplicate_keyword_call, diagnostics).verdict ==
            IR_RULE_REJECT;

    check("trait adapters load by-value receivers from erased storage",
          by_value && valid);
    check("trait adapters pass out/inout receivers as typed storage",
          mutable_receiver_is_storage && valid);
    check("trait adapters bind and forward named keyword parameters",
          keyword_forwarded && valid);
    check("trait adapters bind and spread variadic rest parameters",
          rest_forwarded && valid);
    check("call validation rejects duplicate keyword arguments",
          duplicate_keyword_rejected);
    check("trait adapters reject incompatible keyword contracts",
          !ir_trait_receiver_adapter_add(module, "bad_keyword_adapter",
              keyword_impl,
              th_func(types, th_name(types, "void"), &mutable_slot, 1,
                  nullptr,
                  (th_kwparam[]){{.name = "format", .mode = TH_IN,
                                  .type = integer}}, 1)));
    check("trait adapters reject mismatched non-receiver arguments",
          !ir_trait_receiver_adapter_add(module, "bad_adapter", implementation,
              th_func(types, string,
                  (th_param[]){{.mode = TH_IN, .type = data},
                               {.mode = TH_IN, .type = string}},
                  2, nullptr, nullptr, 0)));

    ir_global_t *vtable = ir_trait_vtable_global_add(
        module, "meter_vtable", 17,
        (const char *const[]){"render", "update"},
        (ir_func_t *const[]){adapter, mutable_adapter}, 2);
    const th_member *type_info = vtable
        ? th_record_member(vtable->type, 0) : nullptr;
    const th_member *render_slot = vtable
        ? th_record_member(vtable->type, 1) : nullptr;
    const th_member *update_slot = vtable
        ? th_record_member(vtable->type, 2) : nullptr;
    bool assembled = vtable && vtable->has_static_initializer &&
        vtable->is_static && vtable->mutability == IR_MUT_CONST &&
        vtable->static_initializer->kind == IR_GLOBAL_INIT_RECORD &&
        vtable->static_initializer->aggregate.count == 3 && type_info && render_slot &&
        update_slot &&
        !strcmp(type_info->name, IR_TRAIT_VTABLE_TYPE_INFO_FIELD) &&
        !strcmp(render_slot->name, "render") &&
        !strcmp(update_slot->name, "update") &&
        vtable->static_initializer->aggregate.items[0].kind
            == IR_GLOBAL_INIT_INT &&
        ir_lit_signed(vtable->static_initializer->aggregate.items[0].integer)
            == 17 &&
        vtable->static_initializer->aggregate.items[1].kind
            == IR_GLOBAL_INIT_SYMBOL &&
        vtable->static_initializer->aggregate.items[1].symbol->owner
            == adapter &&
        vtable->static_initializer->aggregate.items[2].kind
            == IR_GLOBAL_INIT_SYMBOL &&
        vtable->static_initializer->aggregate.items[2].symbol->owner
            == mutable_adapter;
    check("trait vtable globals assemble type info and ordered adapter slots",
          assembled);
    check("record initializer rejects a member-count mismatch",
          !ir_global_set_record_initializer(module, vtable,
              (ir_global_init_value_t[]){{.kind = IR_GLOBAL_INIT_INT,
                                          .integer = ir_lit_of(0)}}, 1));
    ir_global_t *small = ir_global_add(module, "small_record",
        th_record(types, (th_member[]){{.name = "small",
                                       .type = th_name(types, "u8")}}, 1));
    check("record initializer enforces scalar field width",
          small &&
          !ir_global_set_record_initializer(module, small,
              (ir_global_init_value_t[]){{.kind = IR_GLOBAL_INIT_INT,
                                          .member = "small",
                                          .integer = ir_lit_of(256)}}, 1) &&
          ir_global_set_record_initializer(module, small,
              (ir_global_init_value_t[]){{.kind = IR_GLOBAL_INIT_INT,
                                          .member = "small",
                                          .integer = ir_lit_of(255)}}, 1));
    ir_global_t *byte_target = ir_global_add(
        module, "byte_target", th_name(types, "u8"));
    ir_global_t *pointer_global = ir_global_add(module, "pointer_global",
        th_record(types, (th_member[]){{
            .name = "payload",
            .type = th_ind(types, 0, th_name(types, "u8")),
        }}, 1));
    ir_value_t *target_address = ir_global_address(module, byte_target);
    check("record initializers accept typed global-symbol addresses",
          pointer_global && target_address &&
          ir_global_set_record_initializer(module, pointer_global,
              (ir_global_init_value_t[]){{
                  .member = "payload",
                  .kind = IR_GLOBAL_INIT_SYMBOL,
                  .symbol = target_address,
              }}, 1));
    char *printed = ir_module_print(module);
    ir_diags_t *parse_diagnostics = ir_diags_new(ctx, nullptr);
    const ir_profile_t *profiles[] = {&ir_profile_bce};
    ir_module_t *round_trip = printed
        ? ir_module_parse(ctx, printed, 0, profiles, 1, parse_diagnostics)
        : nullptr;
    char *printed_again = round_trip ? ir_module_print(round_trip) : nullptr;
    check("static trait vtable globals round-trip with forward function refs",
          printed && round_trip && printed_again &&
          !strcmp(printed, printed_again));
    if (!round_trip) {
        ir_render_t render = {0};
        char *message = ir_diags_render_text(parse_diagnostics, &render);
        fprintf(stderr, "vtable reparse diagnostics:\n%s\n%s",
                message ? message : "(none)\n", printed ? printed : "");
        free(message);
    } else if (!printed_again || strcmp(printed, printed_again)) {
        fprintf(stderr, "vtable text changed:\n%s----\n%s",
                printed ? printed : "", printed_again ? printed_again : "");
    }
    ir_ctx_free(ctx);
}

static void test_trait_value_abi_keeps_data_and_vtable_indirect(void) {
    ir_ctx_t *ctx = ir_ctx_new();
    th_ctx *types = ir_ctx_types(ctx);
    th_type *object = ir_trait_value_type(types);
    th_type *data = th_record_lookup(object, IR_TRAIT_VALUE_DATA_FIELD);
    th_type *vtable = th_record_lookup(object, IR_TRAIT_VALUE_VTABLE_FIELD);
    const char *name = nullptr;
    size_t name_len = 0;
    bool is_variant = false;
    bool ok = object && th_kind_of(object) == TH_RECORD &&
              th_record_len(object) == 2 && data && vtable &&
              th_kind_of(data) == TH_INDIRECTION &&
              th_kind_of(vtable) == TH_INDIRECTION &&
              th_named_info(th_ind_referent(data), &name, &name_len,
                            &is_variant, nullptr) &&
              name_len == strlen(TH_ERASED) &&
              !memcmp(name, TH_ERASED, name_len) && !is_variant;
    check("trait values lower to data and vtable pointers", ok);
    check("trait value pointers share an opaque target-independent type",
          ok && th_hash(types, data) == th_hash(types, vtable));

    ir_module_t *module = ir_module_new(ctx, "trait_object", &ir_profile_bce,
                                        nullptr);
    th_param params[] = {{.mode = TH_IN, .type = data},
                         {.mode = TH_IN, .type = vtable}};
    th_type *signature = th_func(types, object, params, 2, nullptr, nullptr, 0);
    ir_func_t *function = ir_func_add(module, "pack", signature,
                                      IR_LINK_INTERNAL);
    ir_value_t *data_param = ir_param_add(function, "data", data);
    ir_value_t *vtable_param = ir_param_add(function, "vtable", vtable);
    ir_block_t *block = ir_block_add(function, "entry");
    ir_value_t *packed = ir_trait_value_make(types, block, "value",
                                             data_param, vtable_param);
    ir_value_t *extracted_data =
        ir_trait_value_data(block, "raw", packed);
    ir_value_t *extracted_vtable =
        ir_trait_value_vtable(block, "dispatch", packed);
    check("trait object IR can pack and recover both runtime components",
          packed && extracted_data && extracted_vtable &&
              th_hash(types, extracted_data->type) == th_hash(types, data) &&
              th_hash(types, extracted_vtable->type) == th_hash(types, vtable));

    ir_ctx_free(ctx);
}

typedef struct {
    ir_ctx_t    *ctx;
    ir_module_t *m;
    ir_diags_t  *d;
} env;

static env *open_env(const char *target)
{
    env *e = calloc(1, sizeof(env));

    e->ctx = ir_ctx_new();
    e->m   = ir_module_new(e->ctx, "m", &ir_profile_bce, target);
    e->d   = ir_diags_new(e->ctx, nullptr);

    ir_module_set_span(e->m, ir_span_make(0, 0, 6, 1, 1, 1, 7));
    ir_module_establish(e->m, IR_PROP_PARSED | IR_PROP_TYPED);

    return e;
}

static bool told(env *e, const char *text)
{
    for (uint32_t i = 0; i < ir_diags_count(e->d); i++) {
        const ir_diag_t *dg = ir_diags_at(e->d, i);

        if ((dg->primary.label && strstr(dg->primary.label, text))
            || (dg->message && strstr(dg->message, text))) {
            return true;
        }
    }

    return false;
}

static void harvest(env *e)
{
    for (uint32_t i = 0; i < ir_diags_count(e->d); i++) {
        produced[ir_diags_at(e->d, i)->code] = true;
    }
}

static void close_env(env *e)
{
    harvest(e);
    ir_ctx_free(e->ctx);
    free(e);
}

static th_type *ty(env *e, const char *t)
{
    return ir_type_parse(e->m, t);
}

static ir_func_t *touching(env *e, const char *const *types, size_t n)
{
    ir_func_t  *f = ir_func_add(e->m, "f", ty(e, "() -> void"),
                                IR_LINK_EXTERNAL);
    ir_block_t *b = ir_block_add(f, "entry");

    for (size_t i = 0; i < n; i++) {
        ir_instr_t *in = ir_instr_add(b, IR_OP_SIZEOF);

        ir_instr_operand_type(in, ty(e, types[i]));
    }
    ir_instr_add(b, IR_OP_UNREACHABLE);

    return f;
}

static void test_descriptors_are_keyed_and_scoped(void)
{
    env *e = open_env("x86_64");

    th_type *t = ty(e, "i32");

    ir_descriptor_t *a = ir_layout_ensure(e->m, t, "x86_64");
    ir_descriptor_t *b = ir_layout_ensure(e->m, t, "wasm32");

    check("one type can carry a descriptor per target", a && b && a != b);
    check("and they are found back by target",
          ir_layout_of(e->m, t, "x86_64") == a
              && ir_layout_of(e->m, t, "wasm32") == b);
    check("the module's own target is the default",
          ir_layout_of(e->m, t, nullptr) == a);
    check("two descriptors are counted as two", ir_layout_count(e->m, t) == 2);

    uint64_t before = th_hash(ir_ctx_types(e->ctx), t);

    a->size  = (ir_layout_val_t){.state = IR_LAYOUT_RESOLVED, .value = 4};
    b->size  = (ir_layout_val_t){.state = IR_LAYOUT_RESOLVED, .value = 99};

    check("changing a descriptor does not change the type's hash",
          th_hash(ir_ctx_types(e->ctx), t) == before);
    check("a second spelling of the same type finds the same descriptor",
          ir_layout_of(e->m, ty(e, "i32"), "x86_64") == a);

    check("a descriptor names the type it describes",
          a->type == t && b->type == t);
    check("a descriptor names the target it is scoped to",
          !strcmp(a->target, "x86_64") && !strcmp(b->target, "wasm32"));
    check("the pair round-trips from either end",
          ir_layout_of(e->m, a->type, a->target) == a);

    th_type         *u  = ty(e, "i64");
    ir_descriptor_t *ua = ir_layout_ensure(e->m, u, "x86_64");

    ua->size = (ir_layout_val_t){.state = IR_LAYOUT_RESOLVED, .value = 8};

    th_ctx *th = ir_ctx_types(e->ctx);

    check("same identity, different descriptors: an adaptor is called for",
          th_hash(th, a->type) == th_hash(th, b->type) && a->size.value
                                                              != b->size.value);
    ir_descriptor_t *c2 = ir_layout_ensure(e->m, t, "arm64");

    c2->size = a->size;

    check("same identity, same descriptor: a no-op",
          th_hash(th, a->type) == th_hash(th, c2->type)
              && a->size.value == c2->size.value);
    check("different identity: a type error, whatever the descriptors say",
          th_hash(th, a->type) != th_hash(th, ua->type));

    close_env(e);
}

static void test_the_pass_computes_around_pins(void)
{
    const char *types[] = {"i32", "u8", "f64", "bool", "(x: i32, y: u64)",
                           "array[i32; 4]", "void"};
    env        *e       = open_env("x86_64");

    touching(e, types, 7);
    check("the layout pass runs", ir_layout_run(e->m, e->d));

    struct {
        const char *type;
        uint64_t    size;
        uint64_t    align;
    } want[] = {
        {"i32", 4, 4}, {"u8", 1, 1}, {"f64", 8, 8}, {"bool", 1, 1},
        {"(x: i32, y: u64)", 16, 8}, {"array[i32; 4]", 16, 4},
        {"void", 0, 1},
    };

    for (size_t i = 0; i < 7; i++) {
        ir_descriptor_t *d = ir_layout_of(e->m, ty(e, want[i].type), nullptr);

        if (!d || d->size.value != want[i].size
            || d->align.value != want[i].align) {
            printf("FAIL  %s lays out as %llu/%llu, wanted %llu/%llu\n",
                   want[i].type,
                   (unsigned long long)(d ? d->size.value : 0),
                   (unsigned long long)(d ? d->align.value : 0),
                   (unsigned long long)want[i].size,
                   (unsigned long long)want[i].align);
            fails++;
        }
    }

    ir_descriptor_t *rec = ir_layout_of(e->m, ty(e, "(x: i32, y: u64)"),
                                        nullptr);

    check("a member's bit offset and width are both recorded",
          rec && rec->n_members == 2
              && rec->members[0].bit_offset.value == 0
              && rec->members[0].bit_width.value == 32
              && rec->members[1].bit_offset.value == 64
              && rec->members[1].bit_width.value == 64);
    check("padding shows as a gap between members, not as a member",
          rec && rec->n_members == 2);
    close_env(e);
}

static void test_pins_are_honored(void)
{
    const char *types[] = {"(x: i32, y: i32)"};
    env        *e       = open_env("x86_64");

    touching(e, types, 1);

    th_type *t = ty(e, "(x: i32, y: i32)");

    check("a size pin is accepted", ir_layout_pin_size(e->m, t, 16));
    check("an alignment pin is accepted", ir_layout_pin_align(e->m, t, 16));
    check("a member pin is accepted",
          ir_layout_pin_member(e->m, t, "y", 96, 32));
    check("an alignment that is not a power of two is refused",
          !ir_layout_pin_align(e->m, t, 6));
    check("a pin that contradicts one already set is refused",
          !ir_layout_pin_size(e->m, t, 8));

    check("the pass fills around the pins", ir_layout_run(e->m, e->d));

    ir_descriptor_t *d = ir_layout_of(e->m, t, nullptr);

    check("the pinned size survives",
          d && d->size.state == IR_LAYOUT_PINNED && d->size.value == 16);
    check("the pinned alignment survives",
          d && d->align.state == IR_LAYOUT_PINNED && d->align.value == 16);

    ir_member_layout_t *y = nullptr;
    ir_member_layout_t *x = nullptr;

    for (uint32_t i = 0; d && i < d->n_members; i++) {
        if (!strcmp(d->members[i].name, "y")) {
            y = &d->members[i];
        }
        else {
            x = &d->members[i];
        }
    }

    check("the pinned member stays where it was put",
          y && y->bit_offset.state == IR_LAYOUT_PINNED
              && y->bit_offset.value == 96);
    check("and the unpinned one is placed by the pass",
          x && x->bit_offset.state == IR_LAYOUT_RESOLVED);
    close_env(e);
}

static void test_explicitly_computes_unreachable_type(void)
{
    env *e = open_env("x86_64");
    th_type *record = ty(e, "(x: i32, y: u64)");
    check("an explicitly requested, otherwise unreachable type lays out",
          ir_layout_compute(e->m, record, e->d));
    ir_descriptor_t *descriptor = ir_layout_of(e->m, record, nullptr);
    check("explicit layout computation records the descriptor",
          descriptor && ir_layout_is_settled(&descriptor->size)
              && descriptor->size.value == 16
              && descriptor->members[1].bit_offset.value == 64);
    close_env(e);
}

static void test_representation_alias_inherits_layout(void)
{
    env *e = open_env("x86_64");
    th_type *alias = th_name(ir_ctx_types(e->ctx), "ScalarAlias");
    th_type *base = ty(e, "i32");
    check("an undefined representation alias is unresolved",
          alias && !ir_layout_compute(e->m, alias, e->d));
    check("a scalar representation alias is defined",
          alias && base && ir_type_define(e->m, alias, base));
    check("a scalar representation alias computes its layout",
          ir_layout_compute(e->m, alias, e->d));
    ir_descriptor_t *layout = ir_layout_of(e->m, alias, nullptr);
    ir_descriptor_t *base_layout = ir_layout_of(e->m, base, nullptr);
    check("a scalar alias has a layout descriptor", layout != nullptr);
    check("a scalar alias inherits its base size",
          layout && base_layout
              && layout->size.value == base_layout->size.value);
    check("a scalar alias inherits its base alignment",
          layout && base_layout
              && layout->align.value == base_layout->align.value);
    close_env(e);
}

static void test_an_unsatisfiable_pin_fails_loudly(void)
{
    const char *types[] = {"(x: i32, y: i32)"};
    env        *e       = open_env("x86_64");

    touching(e, types, 1);

    th_type *t = ty(e, "(x: i32, y: i32)");

    ir_layout_pin_size(e->m, t, 4);

    bool ran = ir_layout_run(e->m, e->d);

    harvest(e);

    check("a size too small for the members fails the pass", !ran);
    check("and says so rather than overriding the pin",
          told(e, "smaller than"));
    check("and the pin is left as the author set it",
          ir_layout_of(e->m, t, nullptr)->size.value == 4);
    check("so the module is not laid out",
          !(ir_module_props(e->m) & IR_PROP_LAID_OUT));
    close_env(e);
}

static void test_symbolic_survives_a_target_change(void)
{
    const char *types[] = {"ptr[i32]", "Widget"};
    env        *e       = open_env("x86_64");

    touching(e, types, 2);
    ir_layout_run(e->m, e->d);

    ir_descriptor_t *p = ir_layout_of(e->m, ty(e, "ptr[i32]"), nullptr);
    ir_descriptor_t *w = ir_layout_of(e->m, ty(e, "Widget"), nullptr);

    check("a pointer's size is symbolic, not a number the pass invented",
          p && p->size.state == IR_LAYOUT_SYMBOLIC
              && p->size.sym == IR_SYM_SIZEOF);
    check("and so is an opaque named type's",
          w && w->size.state == IR_LAYOUT_SYMBOLIC);
    check("a symbolic value names the type it is about",
          p && ir_type_eq(e->m, p->size.of, ty(e, "ptr[i32]")));
    check("a module with symbolic layout is not laid out for its target",
          !(ir_module_props(e->m) & IR_PROP_LAID_OUT));

    ir_descriptor_t *other = ir_layout_ensure(e->m, ty(e, "ptr[i32]"),
                                              "wasm32");

    check("the same symbolic expression is meaningful under another target",
          other && other->size.state == IR_LAYOUT_UNSET);
    close_env(e);
}

static void test_laid_out_is_verified(void)
{
    const char *types[] = {"i32", "(x: i32, y: u64)"};
    env        *e       = open_env("x86_64");

    touching(e, types, 2);
    ir_layout_run(e->m, e->d);
    ir_module_establish(e->m, IR_PROP_LAID_OUT);

    check("a fully resolved module verifies as laid out",
          (ir_verify(e->m, e->d) & IR_PROP_LAID_OUT) != 0);
    close_env(e);

    const char *unset[] = {"ptr[i32]"};
    env        *u       = open_env("x86_64");

    touching(u, unset, 1);
    ir_layout_run(u->m, u->d);
    ir_module_establish(u->m, IR_PROP_LAID_OUT);

    ir_props_t held = ir_verify(u->m, u->d);

    harvest(u);

    check("a module with an unresolved value is not laid out",
          !(held & IR_PROP_LAID_OUT));
    check("and the verifier names the rule",
          told(u, "laid-out.descriptor"));
    close_env(u);
}

static void test_overlapping_members_are_caught(void)
{
    const char *types[] = {"(x: i32, y: i32)"};
    env        *e       = open_env("x86_64");

    touching(e, types, 1);

    th_type *t = ty(e, "(x: i32, y: i32)");

    ir_layout_pin_member(e->m, t, "x", 0, 32);
    ir_layout_pin_member(e->m, t, "y", 16, 32);
    ir_layout_run(e->m, e->d);
    ir_module_establish(e->m, IR_PROP_LAID_OUT);

    ir_props_t held = ir_verify(e->m, e->d);

    harvest(e);

    check("members whose bits overlap are caught",
          !(held & IR_PROP_LAID_OUT) && told(e, "laid-out.overlap"));
    close_env(e);
}

static void test_bitcast_sizes_agree(void)
{
    env        *e = open_env("x86_64");
    ir_func_t  *f = ir_func_add(e->m, "f", ty(e, "(u64) -> void"),
                                IR_LINK_EXTERNAL);
    ir_value_t *a = ir_param_add(f, "a", ty(e, "u64"));
    ir_block_t *b = ir_block_add(f, "entry");
    ir_instr_t *c = ir_instr_add(b, IR_OP_BITCAST);

    ir_instr_set_dest(c, "s", ty(e, "u32"));
    ir_instr_operand_value(c, a);
    ir_instr_add(b, IR_OP_UNREACHABLE);

    ir_layout_run(e->m, e->d);
    ir_module_establish(e->m, IR_PROP_LAID_OUT);

    ir_props_t held = ir_verify(e->m, e->d);

    harvest(e);

    check("a bitcast between differently sized types is caught once laid out",
          !(held & IR_PROP_LAID_OUT) && told(e, "laid-out.bitcast"));
    close_env(e);
}

static void test_the_pass_declares_what_the_spec_says(void)
{
    check("the layout pass declares requires typed, establishes laid-out",
          ir_pass_layout.requires == IR_PROP_TYPED
              && ir_pass_layout.establishes == IR_PROP_LAID_OUT
              && ir_pass_declares(&ir_pass_layout));

    const char *types[] = {"i32"};
    env        *e       = open_env("x86_64");

    touching(e, types, 1);

    ir_props_t unmet = 0;

    check("it runs through the pass manager",
          ir_pass_run(&ir_pass_layout, e->m, nullptr, &unmet, e->d)
              == IR_PASS_OK);
    check("and establishes the property it declared",
          (ir_module_props(e->m) & IR_PROP_LAID_OUT) != 0);
    close_env(e);
}

static void test_comptime_is_symbolic_until_laid_out(void)
{
    const char *types[] = {"i32"};
    env        *e       = open_env("x86_64");
    ir_func_t  *f       = touching(e, types, 1);
    ir_instr_t *sz      = ir_block_entry(f)->instrs[0];
    ir_rules_t *rs      = ir_module_rules(e->m);

    ir_rule_apply(e->m, rs, sz, e->d);
    check("sizeof has no value before the layout pass runs",
          ir_annot_get(&sz->annots, "ir.const") == nullptr);

    ir_layout_run(e->m, e->d);
    ir_module_invalidate(e->m, IR_PROP_TYPED);
    ir_rule_apply(e->m, rs, sz, e->d);

    ir_annot_t *k = ir_annot_get(&sz->annots, "ir.const");

    check("and takes one once the type is laid out",
          k && k->kind == IR_ANNOT_INT && k->i.mag == 4);

    ir_instr_t *al = ir_instr_add(ir_block_entry(f), IR_OP_ALIGNOF);

    ir_instr_operand_type(al, ty(e, "i32"));
    ir_rule_apply(e->m, rs, al, e->d);

    ir_annot_t *a = ir_annot_get(&al->annots, "ir.const");

    check("alignof resolves the same way", a && a->i.mag == 4);

    ir_instr_t *of = ir_instr_add(ir_block_entry(f), IR_OP_OFFSETOF);

    ir_instr_operand_type(of, ty(e, "(x: i32, y: u64)"));
    ir_instr_operand_name(of, "y");
    ir_layout_run(e->m, e->d);
    ir_rule_apply(e->m, rs, of, e->d);

    ir_annot_t *o = ir_annot_get(&of->annots, "ir.const");

    check("and so does offsetof, in bits", o && o->i.mag == 64);

    ir_instr_t *sp = ir_instr_add(ir_block_entry(f), IR_OP_SIZEOF);

    ir_instr_operand_type(sp, ty(e, "ptr[i32]"));
    ir_layout_run(e->m, e->d);
    ir_rule_apply(e->m, rs, sp, e->d);
    check("a type whose size stays symbolic gets no value",
          ir_annot_get(&sp->annots, "ir.const") == nullptr);
    close_env(e);
}

static void test_a_module_with_no_target(void)
{
    const ir_profile_t *const profiles[] = {&ir_profile_bce};
    ir_ctx_t                 *ctx        = ir_ctx_new();
    ir_diags_t               *d          = ir_diags_new(ctx, nullptr);
    const char               *src        = "module m {\n  profile bce\n\n"
                                           "  func @f(%a: i32) -> i32 {\n"
                                           "    block entry:\n"
                                           "      %r: i32 = add wrap %a, %a\n"
                                           "      return %r\n"
                                           "  }\n}\n";
    ir_module_t              *m = ir_module_parse(ctx, src, 0, profiles, 1, d);

    check("a module may name no target", m != nullptr);

    if (!m) {
        ir_ctx_free(ctx);

        return;
    }
    ir_module_establish(m, IR_PROP_PARSED | IR_PROP_TYPED);
    check("and laying it out does not crash", ir_layout_run(m, d));

    th_type *i32 = ir_type_parse(m, "i32");

    check("its descriptors are keyed to having no target",
          ir_layout_of(m, i32, nullptr) != nullptr);
    check("and are not confused with a named target's",
          ir_layout_of(m, i32, "host") == nullptr);

    char *text = ir_module_print(m);

    check("and printing it does not crash", text != nullptr);
    check("and it still says no target",
          text && !strstr(text, "target"));
    free(text);
    ir_ctx_free(ctx);
}

static void test_a_member_may_be_named_for_a_layout_keyword(void)
{
    const ir_profile_t *const profiles[] = {&ir_profile_bce};
    ir_ctx_t                 *ctx        = ir_ctx_new();
    ir_diags_t               *d          = ir_diags_new(ctx, nullptr);
    const char               *src        = "module m {\n  profile bce\n"
                                           "  type Rec = (a: i32, b: i32)\n"
                                           "  layout Rec {\n"
                                           "    size = 8\n"
                                           "    size @ bits 0..32\n"
                                           "  }\n"
                                           "  func @f() -> void {\n"
                                           "    block e:\n      return\n"
                                           "  }\n}\n";
    ir_module_t              *m = ir_module_parse(ctx, src, 0, profiles, 1, d);

    check("a layout item may pin a member named for a layout keyword",
          m != nullptr);

    if (!m) {
        ir_ctx_free(ctx);

        return;
    }

    char *text = ir_module_print(m);

    check("the size pin is the size, not the member's low bit",
          text && strstr(text, "size = 8"));
    check("and the member pin survives",
          text && strstr(text, "size @ bits 0..32"));
    free(text);
    ir_ctx_free(ctx);
}

static void test_variant_and_indirection_are_recorded(void)
{
    const char *types[] = {"variant R[i32]", "ptr[i32]"};
    env        *e       = open_env("x86_64");

    ir_type_define(e->m, ty(e, "variant R[i32]"), ty(e, "(ok: i32, err: u8)"));
    touching(e, types, 2);
    ir_layout_run(e->m, e->d);

    ir_descriptor_t *v = ir_layout_of(e->m, ty(e, "variant R[i32]"), nullptr);

    check("a variant records where its tag lives",
          v && ir_layout_is_settled(&v->variant.tag_bit_offset)
              && ir_layout_is_settled(&v->variant.tag_bit_width));
    check("and is not using a niche unless told to",
          v && !v->variant.niche);
    check("its payload is sized by the widest arm",
          v && v->size.value >= 12);

    ir_variant_repr_t niche = {.niche = true, .tag_bit_width = {.value = 1}};

    check("variant representation is settable",
          ir_layout_pin_variant(e->m, ty(e, "variant R[i32]"), niche));
    check("and the niche choice is recorded",
          ir_layout_of(e->m, ty(e, "variant R[i32]"), nullptr)->variant.niche);

    ir_descriptor_t *p = ir_layout_of(e->m, ty(e, "ptr[i32]"), nullptr);

    check("an indirection records its representation",
          p && !p->indirect.fat
              && ir_layout_is_settled(&p->indirect.words)
              && p->indirect.words.value == 1);
    close_env(e);
}

static void test_all_authored_layout_state_round_trips(void)
{
    const ir_profile_t *const profiles[] = {&ir_profile_bce};
    const char *src = "module m {\n"
                      "  profile bce\n"
                      "  target host\n"
                      "  type Choice = variant Choice[i32]\n"
                      "  layout ptr[i32] {\n"
                      "    indirect = fat\n"
                      "    words = 2\n"
                      "  }\n"
                      "  layout Choice {\n"
                      "    variant tag @ bits 8..16\n"
                      "    niche = true\n"
                      "  }\n"
                      "  layout i32 for \"wasm32\" {\n"
                      "    size = 4\n"
                      "  }\n"
                      "  layout i32 without target {\n"
                      "    align = 4\n"
                      "  }\n"
                      "}\n";
    ir_ctx_t *ctx = ir_ctx_new();
    ir_diags_t *d = ir_diags_new(ctx, nullptr);
    ir_module_t *m = ir_module_parse(ctx, src, 0, profiles, 1, d);

    check("the complete layout syntax parses", m != nullptr);

    char *printed = m ? ir_module_print(m) : nullptr;

    check("fat indirections print with their word count",
          printed && strstr(printed, "indirect = fat")
              && strstr(printed, "words = 2"));
    check("variant representation prints its tag and niche",
          printed && strstr(printed, "variant tag @ bits 8..16")
              && strstr(printed, "niche = true"));
    check("non-module targets print explicitly",
          printed && strstr(printed, "layout i32 for \"wasm32\""));
    check("a targetless descriptor stays distinguishable",
          printed && strstr(printed, "layout i32 without target"));

    ir_diags_t *again_diags = ir_diags_new(ctx, nullptr);
    ir_module_t *again = printed
                             ? ir_module_parse(ctx, printed, 0, profiles, 1,
                                               again_diags)
                             : nullptr;

    check("printed layout state parses again", again != nullptr);

    if (again) {
        th_type *ptr_type = ir_type_parse(again, "ptr[i32]");
        th_type *variant_type = ir_type_parse(again, "Choice");
        ir_descriptor_t *ptr = ir_layout_of_targetless(again, ptr_type);
        ir_descriptor_t *variant = ir_layout_of_targetless(again, variant_type);
        th_type *i32 = ir_type_parse(again, "i32");
        ir_descriptor_t *wasm = ir_layout_of(again, i32, "wasm32");
        ir_descriptor_t *targetless = ir_layout_of_targetless(again, i32);


        check("indirection representation survives the round trip",
              ptr && ptr->indirect.authored && ptr->indirect.fat
                  && ptr->indirect.words.value == 2);
        check("variant representation survives the round trip",
              variant && variant->variant.authored && variant->variant.niche
                  && variant->variant.tag_bit_offset.value == 8
                  && variant->variant.tag_bit_width.value == 8);
        check("a named alternate target survives the round trip",
              wasm && wasm->size.authored && wasm->size.value == 4);
        check("a targetless descriptor survives the round trip",
              targetless && targetless->align.authored
                  && targetless->align.value == 4);
    }

    free(printed);
    ir_ctx_free(ctx);
}

static void test_recursive_global_static_initializer(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    th_ctx *types = ir_ctx_types(ctx);
    ir_module_t *module = ir_module_new(ctx, "static_values",
                                        &ir_profile_bce, nullptr);
    th_type *integer = th_name(types, "i64");
    th_type *string = th_name(types, "string");
    th_arg list_arg = {.kind = TH_ARG_TYPE, .type = integer};
    th_arg dict_args[] = {
        {.kind = TH_ARG_TYPE, .type = string},
        {.kind = TH_ARG_TYPE, .type = th_name(types, "bool")},
    };
    th_type *list_type = th_apply(types, "list", &list_arg, 1);
    th_type *dict_type = th_apply(types, "dict", dict_args, 2);
    th_type *tuple_type = th_tuple(types,
        (th_type *[]){integer, string}, 2);
    th_type *record_type = th_record(types, (th_member[]){
        {.name = "enabled", .type = th_name(types, "bool")},
        {.name = "pair", .type = tuple_type},
        {.name = "values", .type = list_type},
        {.name = "labels", .type = dict_type},
    }, 4);
    ir_global_t *global = ir_global_add(module, "config", record_type);
    char pair_text[] = "ok";
    ir_global_init_value_t pair_items[] = {
        {.kind = IR_GLOBAL_INIT_INT, .integer = ir_lit_of(7)},
        {.kind = IR_GLOBAL_INIT_STRING,
         .string = {.bytes = pair_text, .length = 2}},
    };
    ir_global_init_value_t list_items[] = {
        {.kind = IR_GLOBAL_INIT_INT, .integer = ir_lit_of(1)},
        {.kind = IR_GLOBAL_INIT_INT, .integer = ir_lit_of(2)},
    };
    ir_global_init_value_t dict_items[] = {
        {.kind = IR_GLOBAL_INIT_STRING,
         .string = {.bytes = "ready", .length = 5}},
        {.kind = IR_GLOBAL_INIT_BOOL, .boolean = true},
    };
    ir_global_init_value_t fields[] = {
        {.member = "enabled", .kind = IR_GLOBAL_INIT_BOOL,
         .boolean = true},
        {.member = "pair", .kind = IR_GLOBAL_INIT_TUPLE,
         .aggregate = {.items = pair_items, .count = 2}},
        {.member = "values", .kind = IR_GLOBAL_INIT_SEQUENCE,
         .aggregate = {.items = list_items, .count = 2}},
        {.member = "labels", .kind = IR_GLOBAL_INIT_DICTIONARY,
         .aggregate = {.items = dict_items, .count = 2}},
    };
    ir_global_init_value_t initializer = {
        .kind = IR_GLOBAL_INIT_RECORD,
        .aggregate = {.items = fields, .count = 4},
    };
    ir_global_t *list_global = ir_global_add(module, "list_value", list_type);
    th_type *fixed_array_type = ir_type_parse(module, "array[i64; 2]");
    ir_global_t *fixed_array_global = fixed_array_type
        ? ir_global_add(module, "fixed_array", fixed_array_type) : nullptr;
    ir_global_t *tuple_global = ir_global_add(module, "tuple_value", tuple_type);
    ir_global_t *dict_global = ir_global_add(module, "dict_value", dict_type);
    bool list_set = list_global && ir_global_set_static_initializer(
            module, list_global,
            &(ir_global_init_value_t){.kind = IR_GLOBAL_INIT_SEQUENCE,
                .aggregate = {.items = list_items, .count = 2}});
    bool tuple_set = tuple_global && ir_global_set_static_initializer(
            module, tuple_global,
            &(ir_global_init_value_t){.kind = IR_GLOBAL_INIT_TUPLE,
                .aggregate = {.items = pair_items, .count = 2}});
    bool dict_set = dict_global && ir_global_set_static_initializer(
            module, dict_global,
            &(ir_global_init_value_t){.kind = IR_GLOBAL_INIT_DICTIONARY,
                .aggregate = {.items = dict_items, .count = 2}});
    check("sequence initializer node matches its type", list_set);
    check("fixed array initializer validates declared extent",
          fixed_array_global
              && ir_global_set_static_initializer(module, fixed_array_global,
                  &(ir_global_init_value_t){.kind = IR_GLOBAL_INIT_SEQUENCE,
                      .aggregate = {.items = list_items, .count = 2}}));
    ir_global_t *bad_fixed_array = ir_global_add(module, "bad_fixed_array",
                                                 fixed_array_type);
    check("fixed array initializer rejects payload extent mismatch",
          bad_fixed_array
              && !ir_global_set_static_initializer(module, bad_fixed_array,
                  &(ir_global_init_value_t){.kind = IR_GLOBAL_INIT_SEQUENCE,
                      .aggregate = {.items = list_items, .count = 1}}));
    check("tuple initializer node matches its type", tuple_set);
    check("dictionary initializer node matches its type", dict_set);
    bool set = global && ir_global_set_static_initializer(
        module, global, &initializer);
    pair_text[0] = 'X';
    pair_items[0].integer = ir_lit_of(99);
    bool retained = set && global->has_static_initializer
        && ir_global_static_initializer_valid(module, global)
        && global->static_initializer->aggregate.items[1]
            .aggregate.items[0].integer.mag == 7
        && global->static_initializer->aggregate.items[1]
            .aggregate.items[1].string.bytes[0] == 'o';
    check("global static initializers retain typed recursive aggregate data",
          retained);

    ir_global_t *bad = ir_global_add(module, "bad_config", record_type);
    ir_global_init_value_t bad_fields[] = {
        {.member = "enabled", .kind = IR_GLOBAL_INIT_INT,
         .integer = ir_lit_of(1)},
        {.member = "pair", .kind = IR_GLOBAL_INIT_TUPLE,
         .aggregate = {.items = pair_items, .count = 2}},
        {.member = "values", .kind = IR_GLOBAL_INIT_SEQUENCE,
         .aggregate = {.items = list_items, .count = 2}},
        {.member = "labels", .kind = IR_GLOBAL_INIT_DICTIONARY,
         .aggregate = {.items = dict_items, .count = 2}},
    };
    ir_global_init_value_t bad_initializer = {
        .kind = IR_GLOBAL_INIT_RECORD,
        .aggregate = {.items = bad_fields, .count = 4},
    };
    check("global static initializer rejects recursively mismatched types",
          bad && !ir_global_set_static_initializer(module, bad,
                                                    &bad_initializer));
    ir_ctx_free(ctx);
}


int main(void)
{
    test_trait_vtable_dispatch_call();
    test_trait_receiver_adapters();
    test_trait_value_abi_keeps_data_and_vtable_indirect();
    test_descriptors_are_keyed_and_scoped();
    test_the_pass_computes_around_pins();
    test_pins_are_honored();
    test_explicitly_computes_unreachable_type();
    test_representation_alias_inherits_layout();
    test_an_unsatisfiable_pin_fails_loudly();
    test_symbolic_survives_a_target_change();
    test_laid_out_is_verified();
    test_overlapping_members_are_caught();
    test_bitcast_sizes_agree();
    test_the_pass_declares_what_the_spec_says();
    test_comptime_is_symbolic_until_laid_out();
    test_a_module_with_no_target();
    test_a_member_may_be_named_for_a_layout_keyword();
    test_variant_and_indirection_are_recorded();
    test_all_authored_layout_state_round_trips();
    test_recursive_global_static_initializer();

    dump_produced_codes("produced-codes-layout.txt", produced);

    return report("ir layout");
}
