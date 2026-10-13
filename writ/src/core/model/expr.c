#include <stdlib.h>

#include "model.h"

static writ_ref_t *
ref_slot(writ_model_t *model, uint32_t expr)
{
    while (model->refs.len <= expr) {
        core_list_push(model->refs, ((writ_ref_t){0}));
    }
    return &model->refs.data[expr];
}

writ_ref_t
writ_expr_ref(const writ_model_t *model, uint32_t expr)
{
    return expr < model->refs.len ? model->refs.data[expr] : (writ_ref_t){0};
}

static bool
is_text(const writ_model_t *model, writ_sym_t sym, const char *text)
{
    return writ_str_eq(writ_sym_str(model->session, sym), writ_cstr(text));
}

static bool
lookup_scope(const writ_model_t *model, writ_sym_t name, uint32_t want, uint32_t *index)
{
    bool found = false;
    uintptr_t entry = (uintptr_t)core_dict_get((core_dict_t *)&model->scope, name, &found);
    if (!found || (entry >> 32) != want) {
        return false;
    }
    *index = (uint32_t)entry;
    return true;
}

static bool
check_name(writ_model_t *model, uint32_t fn, uint32_t expr, const writ_node_t *node)
{
    writ_ref_t *ref = ref_slot(model, expr);
    if (fn != UINT32_MAX) {
        const writ_fn_rec_t *record = &model->fns.data[fn];
        for (uint32_t i = 0; i < record->slot_count; i++) {
            if (model->slots.data[record->first_slot + i].name == node->name) {
                *ref = (writ_ref_t){WRIT_REF_SLOT, WRIT_REF_VALUE, record->first_slot + i};
                return true;
            }
        }
        if (is_text(model, node->name, "ret") && record->result
            && writ_type(model, record->result)->key.kind != WRIT_TYPE_VOID) {
            *ref = (writ_ref_t){WRIT_REF_RESULT, WRIT_REF_VALUE, fn};
            return true;
        }
    }
    uint32_t index;
    if (lookup_scope(model, node->name, WRIT_SCOPE_CONST, &index)) {
        *ref = (writ_ref_t){WRIT_REF_CONST, WRIT_REF_VALUE, index};
        return true;
    }
    writ_str_t name = writ_sym_str(model->session, node->name);
    writ_report(model->session, WRIT_D_UNKNOWN_NAME, node->loc, "`%.*s`", (int)name.length, name.data);
    return false;
}

static bool
check_member(writ_model_t *model, uint32_t fn, uint32_t expr, const writ_node_t *node)
{
    uint32_t base = writ_node_child(model->unit, expr, 0);
    if (!writ_expr_check(model, fn, base)) {
        return false;
    }
    writ_ref_t target = writ_expr_ref(model, base);
    bool before = is_text(model, node->name, "in");
    bool after = is_text(model, node->name, "out");
    bool valid = target.kind == WRIT_REF_SLOT && (before || after);
    if (valid) {
        const writ_slot_rec_t *slot = &model->slots.data[target.index];
        const writ_type_rec_t *type = writ_type(model, slot->type);
        valid = slot->mode == WRIT_MODE_INOUT && type->key.kind == WRIT_TYPE_POINTER
             && writ_type_is_integer(model, type->key.target);
    }
    if (!valid) {
        writ_report(model->session, WRIT_D_BAD_PHASE, node->loc, nullptr);
        return false;
    }
    *ref_slot(model, expr) = (writ_ref_t){WRIT_REF_SLOT, before ? WRIT_REF_BEFORE : WRIT_REF_AFTER, target.index};
    return true;
}

static bool
check_call(writ_model_t *model, uint32_t fn, uint32_t expr, const writ_node_t *node)
{
    uint32_t callee = writ_node_child(model->unit, expr, 0);
    const writ_node_t *callee_node = writ_node(model->unit, callee);
    uint32_t target;
    if (callee_node->kind != WRIT_N_EXPR_NAME || !lookup_scope(model, callee_node->name, WRIT_SCOPE_FN, &target)) {
        writ_str_t text = writ_loc_text(model->session, callee_node->loc);
        writ_report(model->session, WRIT_D_UNKNOWN_NAME, callee_node->loc, "`%.*s` is not a function",
                    (int)text.length, text.data);
        return false;
    }
    const writ_fn_rec_t *record = &model->fns.data[target];
    if (!record->pure) {
        writ_str_t name = writ_sym_str(model->session, record->name);
        writ_report(model->session, WRIT_D_CALL_NOT_PURE, node->loc, "`%.*s`", (int)name.length, name.data);
        return false;
    }
    if (node->child_count - 1 != record->slot_count) {
        writ_report(model->session, WRIT_D_BAD_ARITY, node->loc, "expected %u, found %u", record->slot_count,
                    node->child_count - 1);
        return false;
    }
    *ref_slot(model, callee) = (writ_ref_t){WRIT_REF_FN, WRIT_REF_VALUE, target};
    bool ok = true;
    for (uint32_t i = 1; i < node->child_count; i++) {
        ok = writ_expr_check(model, fn, writ_node_child(model->unit, expr, i)) && ok;
    }
    return ok;
}

bool
writ_expr_check(writ_model_t *model, uint32_t fn, uint32_t expr)
{
    const writ_node_t *node = writ_node(model->unit, expr);
    switch (node->kind) {
    case WRIT_N_EXPR_NAME:
        return check_name(model, fn, expr, node);
    case WRIT_N_EXPR_MEMBER:
        return check_member(model, fn, expr, node);
    case WRIT_N_EXPR_CALL:
        return check_call(model, fn, expr, node);
    case WRIT_N_EXPR_INT:
    case WRIT_N_EXPR_STRING:
        return true;
    default: {
        bool ok = true;
        for (uint32_t i = 0; i < node->child_count; i++) {
            ok = writ_expr_check(model, fn, writ_node_child(model->unit, expr, i)) && ok;
        }
        return ok;
    }
    }
}

static bool
parse_int(writ_str_t text, int64_t *out)
{
    char digits[64];
    size_t used = 0;
    for (size_t i = 0; i < text.length && used + 1 < sizeof(digits); i++) {
        if (text.data[i] != '_') {
            digits[used++] = text.data[i];
        }
    }
    digits[used] = 0;
    char *end = nullptr;
    *out = (int64_t)strtoull(digits, &end, 0);
    return used && end && !*end;
}

static bool
apply_binary(writ_str_t op, int64_t a, int64_t b, int64_t *out)
{
    static const struct {
        const char *text;
        int         code;
    } ops[] = {{"==", 1}, {"!=", 2}, {"<", 3}, {">", 4}, {"<=", 5}, {">=", 6}, {"+", 7},
               {"-", 8},  {"*", 9},  {"/", 10}, {"and", 11}, {"or", 12}};
    int code = 0;
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        if (writ_str_eq(op, writ_cstr(ops[i].text))) {
            code = ops[i].code;
        }
    }
    switch (code) {
    case 1: *out = a == b; return true;
    case 2: *out = a != b; return true;
    case 3: *out = a < b; return true;
    case 4: *out = a > b; return true;
    case 5: *out = a <= b; return true;
    case 6: *out = a >= b; return true;
    case 7: *out = a + b; return true;
    case 8: *out = a - b; return true;
    case 9: *out = a * b; return true;
    case 10: if (!b) return false; *out = a / b; return true;
    case 11: *out = a && b; return true;
    case 12: *out = a || b; return true;
    default: return false;
    }
}

bool
writ_expr_eval(const writ_model_t *model, uint32_t expr, writ_expr_env_t *env, int64_t *out)
{
    const writ_node_t *node = writ_node(model->unit, expr);
    int64_t a = 0;
    int64_t b = 0;
    switch (node->kind) {
    case WRIT_N_EXPR_INT:
        return parse_int(writ_sym_str(model->session, node->name), out);
    case WRIT_N_EXPR_NAME:
    case WRIT_N_EXPR_MEMBER: {
        writ_ref_t ref = writ_expr_ref(model, expr);
        if (ref.kind == WRIT_REF_CONST) {
            *out = model->consts.data[ref.index].value;
            return true;
        }
        return env && env->value && env->value(env, ref, out);
    }
    case WRIT_N_EXPR_CALL: {
        int64_t args[16];
        uint32_t count = node->child_count - 1;
        if (count > 16 || !env || !env->call) {
            return false;
        }
        for (uint32_t i = 0; i < count; i++) {
            if (!writ_expr_eval(model, writ_node_child(model->unit, expr, i + 1), env, &args[i])) {
                return false;
            }
        }
        return env->call(env, writ_expr_ref(model, writ_node_child(model->unit, expr, 0)).index, args, count, out);
    }
    case WRIT_N_EXPR_BINARY:
        return writ_expr_eval(model, writ_node_child(model->unit, expr, 0), env, &a)
            && writ_expr_eval(model, writ_node_child(model->unit, expr, 1), env, &b)
            && apply_binary(writ_sym_str(model->session, node->payload), a, b, out);
    case WRIT_N_EXPR_NEG:
        if (!writ_expr_eval(model, writ_node_child(model->unit, expr, 0), env, &a)) {
            return false;
        }
        *out = -a;
        return true;
    case WRIT_N_EXPR_NOT:
        if (!writ_expr_eval(model, writ_node_child(model->unit, expr, 0), env, &a)) {
            return false;
        }
        *out = !a;
        return true;
    default:
        return false;
    }
}

static const char *
c_operator(writ_str_t op)
{
    if (writ_str_eq(op, writ_cstr("and"))) {
        return "&&";
    }
    if (writ_str_eq(op, writ_cstr("or"))) {
        return "||";
    }
    return nullptr;
}

void
writ_expr_print(const writ_model_t *model, uint32_t expr, writ_language_t language, writ_expr_env_t *env,
                core_buffer_t *out)
{
    const writ_node_t *node = writ_node(model->unit, expr);
    switch (node->kind) {
    case WRIT_N_EXPR_INT: {
        int64_t value = 0;
        parse_int(writ_sym_str(model->session, node->name), &value);
        core_buffer_printf(out, "%lld", (long long)value);
        return;
    }
    case WRIT_N_EXPR_NAME:
    case WRIT_N_EXPR_MEMBER: {
        writ_ref_t ref = writ_expr_ref(model, expr);
        if (ref.kind == WRIT_REF_CONST) {
            core_buffer_printf(out, "%lld", (long long)model->consts.data[ref.index].value);
        } else if (env && env->print_ref) {
            env->print_ref(env, ref, out);
        }
        return;
    }
    case WRIT_N_EXPR_CALL: {
        writ_ref_t callee = writ_expr_ref(model, writ_node_child(model->unit, expr, 0));
        writ_str_t name = writ_sym_str(model->session, model->fns.data[callee.index].name);
        core_buffer_printf(out, "%.*s(", (int)name.length, name.data);
        for (uint32_t i = 1; i < node->child_count; i++) {
            if (i > 1) {
                core_buffer_puts(out, ", ");
            }
            writ_expr_print(model, writ_node_child(model->unit, expr, i), language, env, out);
        }
        core_buffer_putc(out, ')');
        return;
    }
    case WRIT_N_EXPR_BINARY: {
        writ_str_t op = writ_sym_str(model->session, node->payload);
        const char *spelled = language == WRIT_LANGUAGE_C ? c_operator(op) : nullptr;
        core_buffer_putc(out, '(');
        writ_expr_print(model, writ_node_child(model->unit, expr, 0), language, env, out);
        if (spelled) {
            core_buffer_printf(out, " %s ", spelled);
        } else {
            core_buffer_printf(out, " %.*s ", (int)op.length, op.data);
        }
        writ_expr_print(model, writ_node_child(model->unit, expr, 1), language, env, out);
        core_buffer_putc(out, ')');
        return;
    }
    case WRIT_N_EXPR_NEG:
    case WRIT_N_EXPR_NOT:
        core_buffer_puts(out, node->kind == WRIT_N_EXPR_NEG ? "(-" : "(!");
        writ_expr_print(model, writ_node_child(model->unit, expr, 0), language, env, out);
        core_buffer_putc(out, ')');
        return;
    default:
        return;
    }
}

bool
writ_expr_mentions(const writ_model_t *model, uint32_t expr, writ_ref_kind_t kind, uint32_t index)
{
    if (!expr) {
        return false;
    }
    writ_ref_t ref = writ_expr_ref(model, expr);
    if (ref.kind == kind && ref.index == index) {
        return true;
    }
    const writ_node_t *node = writ_node(model->unit, expr);
    for (uint32_t i = 0; i < node->child_count; i++) {
        if (writ_expr_mentions(model, writ_node_child(model->unit, expr, i), kind, index)) {
            return true;
        }
    }
    return false;
}
