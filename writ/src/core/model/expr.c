#include <stdlib.h>

#include "model.h"

typedef struct {
    writ_model_t     *model;
    writ_expr_scope_t scope;
    uint32_t          owner;
    bool              impure;
} checker_t;

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

static void
owner_slots(const checker_t *c, uint32_t *first, uint32_t *count)
{
    *first = 0;
    *count = 0;
    if (c->scope == WRIT_EXPR_SCOPE_FN && c->owner != WRIT_NONE) {
        const writ_fn_rec_t *fn = &c->model->fns.data[c->owner];
        *first = fn->first_slot;
        *count = fn->slot_count;
    } else if (c->scope == WRIT_EXPR_SCOPE_RECORD) {
        const writ_record_rec_t *record = &c->model->records.data[c->owner];
        *first = record->first_field;
        *count = record->field_count;
    }
}

static bool check(checker_t *c, uint32_t expr);

static void
report_unknown(writ_model_t *model, const writ_node_t *node)
{
    writ_str_t name = writ_sym_str(model->session, node->name);
    writ_report(model->session, WRIT_D_UNKNOWN_NAME, node->loc, "`%.*s`", (int)name.length, name.data);
}

static bool
check_name(checker_t *c, uint32_t expr, const writ_node_t *node)
{
    writ_model_t *model = c->model;
    writ_ref_t *ref = ref_slot(model, expr);
    uint32_t first;
    uint32_t count;
    owner_slots(c, &first, &count);
    for (uint32_t i = 0; i < count; i++) {
        if (model->slots.data[first + i].name == node->name) {
            *ref = (writ_ref_t){WRIT_REF_SLOT, WRIT_REF_VALUE, first + i};
            return true;
        }
    }
    if (c->scope == WRIT_EXPR_SCOPE_FN && c->owner != WRIT_NONE && writ_is_text(model, node->name, "ret")
        && model->fns.data[c->owner].has_result) {
        *ref = (writ_ref_t){WRIT_REF_RESULT, WRIT_REF_VALUE, c->owner};
        return true;
    }
    if (c->scope == WRIT_EXPR_SCOPE_FN && c->owner != WRIT_NONE && writ_is_text(model, node->name, "none")) {
        const writ_type_rec_t *result = writ_type(model, model->fns.data[c->owner].result);
        if (result->key.kind == WRIT_TYPE_HANDLE) {
            uint32_t handle = writ_type(model, writ_type_named(model, result->key.name))->decl;
            *ref = model->handles.data[handle].has_none ? (writ_ref_t){WRIT_REF_NONE_OF, WRIT_REF_VALUE, handle}
                                                        : (writ_ref_t){WRIT_REF_NULL, WRIT_REF_VALUE, 0};
            return true;
        }
    }
    if (c->scope == WRIT_EXPR_SCOPE_CONDITION) {
        int64_t value;
        bool fact = writ_platform_fact(model->platform, node->name, &value);
        bool version = writ_is_text(model, node->name, "version");
        *ref = (writ_ref_t){fact ? WRIT_REF_FACT : version ? WRIT_REF_VERSION : WRIT_REF_ATOM, WRIT_REF_VALUE,
                            node->name};
        return true;
    }
    uint32_t index;
    if (writ_scope_lookup(model, node->name, WRIT_SCOPE_CONST, &index)) {
        *ref = (writ_ref_t){WRIT_REF_CONST, WRIT_REF_VALUE, index};
        return true;
    }
    if (c->scope != WRIT_EXPR_SCOPE_CONSTANT && writ_scope_lookup(model, node->name, WRIT_SCOPE_CHANNEL, &index)) {
        *ref = (writ_ref_t){WRIT_REF_CHANNEL, WRIT_REF_VALUE, index};
        return true;
    }
    if (writ_is_text(model, node->name, "null")) {
        *ref = (writ_ref_t){WRIT_REF_NULL, WRIT_REF_VALUE, 0};
        return true;
    }
    report_unknown(model, node);
    return false;
}

static bool
slot_is_integer_cell(const writ_model_t *model, uint32_t slot)
{
    const writ_type_rec_t *type = writ_type(model, writ_type_physical(model, model->slots.data[slot].type));
    return type->key.kind == WRIT_TYPE_POINTER && writ_type_is_integer(model, type->key.target);
}

static bool
check_base(checker_t *c, uint32_t expr, writ_ref_t *target)
{
    uint32_t base = writ_node_child(c->model->unit, expr, 0);
    if (!check(c, base)) {
        return false;
    }
    *target = writ_expr_ref(c->model, base);
    return true;
}

static bool
check_member(checker_t *c, uint32_t expr, const writ_node_t *node)
{
    writ_model_t *model = c->model;
    writ_ref_t target;
    if (!check_base(c, expr, &target)) {
        return false;
    }
    bool before = writ_is_text(model, node->name, "in");
    bool after = writ_is_text(model, node->name, "out");
    if (target.kind == WRIT_REF_SLOT && (before || after)) {
        const writ_slot_rec_t *slot = &model->slots.data[target.index];
        if (slot->mode != WRIT_MODE_INOUT || !slot_is_integer_cell(model, target.index)) {
            writ_report(model->session, WRIT_D_BAD_PHASE, node->loc, nullptr);
            return false;
        }
        *ref_slot(model, expr) = (writ_ref_t){WRIT_REF_SLOT, before ? WRIT_REF_BEFORE : WRIT_REF_AFTER, target.index};
        return true;
    }
    if (target.kind == WRIT_REF_SLOT || target.kind == WRIT_REF_FIELD) {
        writ_type_id type = writ_type_physical(model, model->slots.data[target.index].type);
        const writ_type_rec_t *record = writ_type(model, type);
        if (record->key.kind == WRIT_TYPE_POINTER) {
            record = writ_type(model, writ_type_physical(model, record->key.target));
        }
        if (record->key.kind == WRIT_TYPE_RECORD && record->decl != WRIT_NONE) {
            const writ_record_rec_t *decl = &model->records.data[record->decl];
            for (uint32_t i = 0; i < decl->field_count; i++) {
                if (model->slots.data[decl->first_field + i].name == node->name) {
                    *ref_slot(model, expr) = (writ_ref_t){WRIT_REF_FIELD, WRIT_REF_VALUE, decl->first_field + i};
                    return true;
                }
            }
        }
    }
    writ_report(model->session, WRIT_D_UNKNOWN_NAME, node->loc, "no field `%.*s`",
                (int)writ_sym_str(model->session, node->name).length, writ_sym_str(model->session, node->name).data);
    return false;
}

static bool
check_deref(checker_t *c, uint32_t expr, const writ_node_t *node)
{
    writ_model_t *model = c->model;
    writ_ref_t target;
    if (!check_base(c, expr, &target)) {
        return false;
    }
    if (target.kind != WRIT_REF_SLOT || !slot_is_integer_cell(model, target.index)) {
        writ_report(model->session, WRIT_D_BAD_PHASE, node->loc, "`*` reads an integer through a pointer parameter");
        return false;
    }
    writ_mode_t mode = model->slots.data[target.index].mode;
    *ref_slot(model, expr) = (writ_ref_t){WRIT_REF_SLOT, mode == WRIT_MODE_IN ? WRIT_REF_BEFORE : WRIT_REF_AFTER,
                                          target.index};
    return true;
}

static bool
check_builtin_call(checker_t *c, uint32_t expr, const writ_node_t *node, const writ_node_t *callee)
{
    writ_model_t *model = c->model;
    uint32_t callee_id = writ_node_child(model->unit, expr, 0);
    if (writ_is_text(model, callee->name, "sizeof")) {
        const writ_node_t *arg = writ_node(model->unit, writ_node_child(model->unit, expr, 1));
        writ_type_id type = node->child_count == 2 && arg->kind == WRIT_N_EXPR_NAME ? writ_type_named(model, arg->name) : 0;
        if (!type) {
            writ_report(model->session, WRIT_D_UNKNOWN_TYPE, node->loc, "`sizeof` takes one type name");
            return false;
        }
        *ref_slot(model, expr) = (writ_ref_t){WRIT_REF_SIZEOF, WRIT_REF_VALUE, type};
        return true;
    }
    bool is_min = writ_is_text(model, callee->name, "min");
    if (!is_min && !writ_is_text(model, callee->name, "max")) {
        return false;
    }
    if (node->child_count != 3) {
        writ_report(model->session, WRIT_D_BAD_ARITY, node->loc, "expected 2, found %u", node->child_count - 1);
        return false;
    }
    *ref_slot(model, callee_id) = (writ_ref_t){WRIT_REF_BUILTIN, WRIT_REF_VALUE, is_min ? WRIT_BUILTIN_MIN : WRIT_BUILTIN_MAX};
    bool first = check(c, writ_node_child(model->unit, expr, 1));
    bool second = check(c, writ_node_child(model->unit, expr, 2));
    return first && second;
}

static bool
check_call(checker_t *c, uint32_t expr, const writ_node_t *node)
{
    writ_model_t *model = c->model;
    uint32_t callee = writ_node_child(model->unit, expr, 0);
    const writ_node_t *callee_node = writ_node(model->unit, callee);
    uint32_t target;
    bool known = callee_node->kind == WRIT_N_EXPR_NAME
              && writ_scope_lookup(model, callee_node->name, WRIT_SCOPE_FN, &target);
    if (!known && callee_node->kind == WRIT_N_EXPR_NAME) {
        size_t first_diag = model->session->diags.len;
        if (check_builtin_call(c, expr, node, callee_node) || model->session->diags.len != first_diag) {
            return model->session->diags.len == first_diag;
        }
    }
    if (!known) {
        writ_str_t text = writ_loc_text(model->session, callee_node->loc);
        writ_report(model->session, WRIT_D_UNKNOWN_NAME, callee_node->loc, "`%.*s` is not a function",
                    (int)text.length, text.data);
        return false;
    }
    const writ_fn_rec_t *record = &model->fns.data[target];
    if (c->scope == WRIT_EXPR_SCOPE_CHANNEL) {
        c->scope = WRIT_EXPR_SCOPE_FN;
        c->owner = target;
    } else if (!record->pure && !c->impure) {
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
        ok = check(c, writ_node_child(model->unit, expr, i)) && ok;
    }
    return ok;
}

static bool
check(checker_t *c, uint32_t expr)
{
    writ_model_t *model = c->model;
    const writ_node_t *node = writ_node(model->unit, expr);
    switch (node->kind) {
    case WRIT_N_EXPR_NAME:
        return check_name(c, expr, node);
    case WRIT_N_EXPR_MEMBER:
        return check_member(c, expr, node);
    case WRIT_N_EXPR_DEREF:
        return check_deref(c, expr, node);
    case WRIT_N_EXPR_CALL:
        return check_call(c, expr, node);
    case WRIT_N_EXPR_INT:
    case WRIT_N_EXPR_STRING:
        return true;
    case WRIT_N_EXPR_VERSION:
    case WRIT_N_EXPR_FEATURE:
    case WRIT_N_EXPR_IN:
        if (c->scope != WRIT_EXPR_SCOPE_CONDITION) {
            writ_report(model->session, WRIT_D_BAD_CONDITION, node->loc, "only `when` conditions test platforms");
            return false;
        }
        return node->kind != WRIT_N_EXPR_IN || check(c, writ_node_child(model->unit, expr, 0));
    case WRIT_N_EXPR_VIEW:
        if (!writ_type_named(model, node->name)) {
            writ_str_t text = writ_sym_str(model->session, node->name);
            writ_report(model->session, WRIT_D_UNKNOWN_TYPE, node->loc, "`%.*s`", (int)text.length, text.data);
            return false;
        }
        return check(c, writ_node_child(model->unit, expr, 0));
    default: {
        bool ok = true;
        for (uint32_t i = 0; i < node->child_count; i++) {
            ok = check(c, writ_node_child(model->unit, expr, i)) && ok;
        }
        return ok;
    }
    }
}

bool
writ_expr_check_in(writ_model_t *model, writ_expr_scope_t scope, uint32_t owner, uint32_t expr)
{
    checker_t c = {model, scope, owner, false};
    return check(&c, expr);
}

bool
writ_expr_check_detail(writ_model_t *model, uint32_t fn, uint32_t expr)
{
    checker_t c = {model, WRIT_EXPR_SCOPE_FN, fn, true};
    return check(&c, expr);
}

bool
writ_expr_check(writ_model_t *model, uint32_t fn, uint32_t expr)
{
    return writ_expr_check_in(model, WRIT_EXPR_SCOPE_FN, fn, expr);
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

static const struct {
    const char *text;
    const char *c;
} operators[] = {
    {"==", "=="}, {"!=", "!="}, {"<", "<"},   {">", ">"},   {"<=", "<="},
    {">=", ">="}, {"+", "+"},   {"-", "-"},   {"*", "*"},   {"/", "/"},
    {"and", "&&"}, {"or", "||"},
};

static bool
apply_binary(writ_str_t op, int64_t a, int64_t b, int64_t *out)
{
    int code = -1;
    for (int i = 0; i < (int)(sizeof(operators) / sizeof(operators[0])); i++) {
        if (writ_str_eq(op, writ_cstr(operators[i].text))) {
            code = i;
        }
    }
    switch (code) {
    case 0: *out = a == b; return true;
    case 1: *out = a != b; return true;
    case 2: *out = a < b; return true;
    case 3: *out = a > b; return true;
    case 4: *out = a <= b; return true;
    case 5: *out = a >= b; return true;
    case 6: *out = a + b; return true;
    case 7: *out = a - b; return true;
    case 8: *out = a * b; return true;
    case 9: if (!b) return false; *out = a / b; return true;
    case 10: *out = a && b; return true;
    case 11: *out = a || b; return true;
    default: return false;
    }
}

static bool
eval_ref(const writ_model_t *model, uint32_t expr, writ_ref_t ref, writ_expr_env_t *env, int64_t *out)
{
    switch (ref.kind) {
    case WRIT_REF_CONST:
        *out = model->consts.data[ref.index].value;
        return !model->consts.data[ref.index].text;
    case WRIT_REF_NULL:
        *out = 0;
        return true;
    case WRIT_REF_NONE_OF:
        *out = model->handles.data[ref.index].none_value;
        return true;
    case WRIT_REF_SIZEOF:
        *out = writ_type(model, ref.index)->size;
        return true;
    case WRIT_REF_FACT:
        return writ_platform_fact(model->platform, ref.index, out);
    case WRIT_REF_ATOM:
        *out = ref.index;
        return true;
    case WRIT_REF_VERSION: {
        bool found = false;
        void *value = core_dict_get((core_dict_t *)&model->platform->versions, model->library, &found);
        *out = (int64_t)(intptr_t)value;
        return found;
    }
    default:
        return env && env->value && env->value(env, ref, expr, out);
    }
}

static bool
eval_in(const writ_model_t *model, uint32_t expr, writ_expr_env_t *env, int64_t *out)
{
    const writ_node_t *node = writ_node(model->unit, expr);
    int64_t subject = 0;
    if (!writ_expr_eval(model, writ_node_child(model->unit, expr, 0), env, &subject)) {
        return false;
    }
    writ_str_t text = writ_sym_str(model->session, (writ_sym_t)subject);
    *out = 0;
    for (uint32_t i = 1; i < node->child_count; i++) {
        writ_str_t pattern = writ_sym_str(model->session, writ_node(model->unit, writ_node_child(model->unit, expr, i))->name);
        *out = *out || writ_glob_match(pattern, text);
    }
    return true;
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
    case WRIT_N_EXPR_VERSION:
        *out = writ_version_value(writ_sym_str(model->session, node->name));
        return true;
    case WRIT_N_EXPR_FEATURE: {
        bool found = false;
        void *value = core_dict_get((core_dict_t *)&model->platform->features, node->name, &found);
        *out = found && value;
        return true;
    }
    case WRIT_N_EXPR_IN:
        return eval_in(model, expr, env, out);
    case WRIT_N_EXPR_NAME:
    case WRIT_N_EXPR_MEMBER:
    case WRIT_N_EXPR_DEREF:
        return eval_ref(model, expr, writ_expr_ref(model, expr), env, out);
    case WRIT_N_EXPR_VIEW:
        return writ_expr_eval(model, writ_node_child(model->unit, expr, 0), env, out);
    case WRIT_N_EXPR_CALL: {
        writ_ref_t self = writ_expr_ref(model, expr);
        if (self.kind == WRIT_REF_SIZEOF) {
            return eval_ref(model, expr, self, env, out);
        }
        writ_ref_t callee = writ_expr_ref(model, writ_node_child(model->unit, expr, 0));
        int64_t args[16];
        uint32_t count = node->child_count - 1;
        if (count > 16) {
            return false;
        }
        for (uint32_t i = 0; i < count; i++) {
            if (!writ_expr_eval(model, writ_node_child(model->unit, expr, i + 1), env, &args[i])) {
                return false;
            }
        }
        if (callee.kind == WRIT_REF_BUILTIN) {
            *out = (callee.index == WRIT_BUILTIN_MIN) == (args[0] < args[1]) ? args[0] : args[1];
            return true;
        }
        return env && env->call && env->call(env, callee.index, args, count, out);
    }
    case WRIT_N_EXPR_BINARY:
        return writ_expr_eval(model, writ_node_child(model->unit, expr, 0), env, &a)
            && writ_expr_eval(model, writ_node_child(model->unit, expr, 1), env, &b)
            && apply_binary(writ_sym_str(model->session, node->payload), a, b, out);
    case WRIT_N_EXPR_NEG:
    case WRIT_N_EXPR_NOT:
    case WRIT_N_EXPR_BITNOT:
        if (!writ_expr_eval(model, writ_node_child(model->unit, expr, 0), env, &a)) {
            return false;
        }
        *out = node->kind == WRIT_N_EXPR_NEG ? -a : node->kind == WRIT_N_EXPR_NOT ? !a : ~a;
        return true;
    default:
        return false;
    }
}

bool
writ_condition_holds(writ_model_t *model, uint32_t expr)
{
    int64_t value = 0;
    if (!writ_expr_check_in(model, WRIT_EXPR_SCOPE_CONDITION, WRIT_NONE, expr)) {
        return false;
    }
    if (!writ_expr_eval(model, expr, nullptr, &value)) {
        writ_report(model->session, WRIT_D_BAD_CONDITION, writ_node(model->unit, expr)->loc, nullptr);
        return false;
    }
    return value != 0;
}

static void
print_child(const writ_model_t *model, uint32_t expr, uint32_t index, writ_language_t language, writ_expr_env_t *env,
            core_buffer_t *out)
{
    writ_expr_print(model, writ_node_child(model->unit, expr, index), language, env, out);
}

static void
print_ref(const writ_model_t *model, uint32_t expr, writ_ref_t ref, writ_language_t language, writ_expr_env_t *env,
          core_buffer_t *out)
{
    switch (ref.kind) {
    case WRIT_REF_CONST: {
        const writ_const_rec_t *constant = &model->consts.data[ref.index];
        writ_str_t name = writ_sym_str(model->session, constant->name);
        if (model->headers.len) {
            core_buffer_printf(out, "%.*s", (int)name.length, name.data);
        } else if (constant->text) {
            writ_str_t text = writ_sym_str(model->session, constant->text);
            core_buffer_printf(out, "\"%.*s\"", (int)text.length, text.data);
        } else {
            core_buffer_printf(out, "%lld", (long long)constant->value);
        }
        return;
    }
    case WRIT_REF_NULL:
        core_buffer_puts(out, "NULL");
        return;
    case WRIT_REF_NONE_OF:
        core_buffer_printf(out, "(%lld)", (long long)model->handles.data[ref.index].none_value);
        return;
    case WRIT_REF_SIZEOF: {
        writ_str_t spelling = writ_sym_str(model->session, writ_type(model, ref.index)->c_spelling);
        core_buffer_printf(out, "sizeof(%.*s)", (int)spelling.length, spelling.data);
        return;
    }
    case WRIT_REF_FIELD: {
        writ_str_t name = writ_sym_str(model->session, model->slots.data[ref.index].name);
        print_child(model, expr, 0, language, env, out);
        core_buffer_printf(out, "->%.*s", (int)name.length, name.data);
        return;
    }
    default:
        if (env && env->print_ref) {
            env->print_ref(env, ref, expr, out);
        }
        return;
    }
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
    case WRIT_N_EXPR_STRING: {
        writ_str_t text = writ_sym_str(model->session, node->name);
        core_buffer_printf(out, "\"%.*s\"", (int)text.length, text.data);
        return;
    }
    case WRIT_N_EXPR_NAME:
    case WRIT_N_EXPR_MEMBER:
    case WRIT_N_EXPR_DEREF:
        print_ref(model, expr, writ_expr_ref(model, expr), language, env, out);
        return;
    case WRIT_N_EXPR_VIEW:
        print_child(model, expr, 0, language, env, out);
        return;
    case WRIT_N_EXPR_CALL: {
        writ_ref_t self = writ_expr_ref(model, expr);
        if (self.kind == WRIT_REF_SIZEOF) {
            print_ref(model, expr, self, language, env, out);
            return;
        }
        writ_ref_t callee = writ_expr_ref(model, writ_node_child(model->unit, expr, 0));
        if (callee.kind == WRIT_REF_BUILTIN) {
            core_buffer_puts(out, "((");
            print_child(model, expr, 1, language, env, out);
            core_buffer_puts(out, callee.index == WRIT_BUILTIN_MIN ? ") < (" : ") > (");
            print_child(model, expr, 2, language, env, out);
            core_buffer_puts(out, ") ? (");
            print_child(model, expr, 1, language, env, out);
            core_buffer_puts(out, ") : (");
            print_child(model, expr, 2, language, env, out);
            core_buffer_puts(out, "))");
            return;
        }
        writ_str_t name = writ_sym_str(model->session, model->fns.data[callee.index].name);
        core_buffer_printf(out, "%.*s(", (int)name.length, name.data);
        for (uint32_t i = 1; i < node->child_count; i++) {
            if (i > 1) {
                core_buffer_puts(out, ", ");
            }
            print_child(model, expr, i, language, env, out);
        }
        core_buffer_putc(out, ')');
        return;
    }
    case WRIT_N_EXPR_BINARY: {
        writ_str_t op = writ_sym_str(model->session, node->payload);
        const char *spelled = op.data;
        int length = (int)op.length;
        for (size_t i = 0; i < sizeof(operators) / sizeof(operators[0]); i++) {
            if (writ_str_eq(op, writ_cstr(operators[i].text))) {
                spelled = operators[i].c;
                length = (int)strlen(spelled);
            }
        }
        core_buffer_putc(out, '(');
        print_child(model, expr, 0, language, env, out);
        core_buffer_printf(out, " %.*s ", length, spelled);
        print_child(model, expr, 1, language, env, out);
        core_buffer_putc(out, ')');
        return;
    }
    case WRIT_N_EXPR_NEG:
    case WRIT_N_EXPR_NOT:
    case WRIT_N_EXPR_BITNOT:
        core_buffer_puts(out, node->kind == WRIT_N_EXPR_NEG ? "(-" : node->kind == WRIT_N_EXPR_NOT ? "(!" : "(~");
        print_child(model, expr, 0, language, env, out);
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

writ_type_id
writ_expr_type(const writ_model_t *model, uint32_t expr)
{
    const writ_node_t *node = writ_node(model->unit, expr);
    if (node->kind == WRIT_N_EXPR_VIEW) {
        return writ_type_named(model, node->name);
    }
    writ_ref_t ref = writ_expr_ref(model, expr);
    switch (ref.kind) {
    case WRIT_REF_SLOT:
    case WRIT_REF_FIELD: {
        writ_type_id type = model->slots.data[ref.index].type;
        if (ref.phase != WRIT_REF_VALUE) {
            type = writ_type(model, writ_type_physical(model, type))->key.target;
        }
        return type;
    }
    case WRIT_REF_RESULT:
        return model->fns.data[ref.index].result;
    case WRIT_REF_CONST:
        return model->consts.data[ref.index].type;
    case WRIT_REF_CHANNEL: {
        uint32_t source = model->channels.data[ref.index].source_expr;
        writ_ref_t callee = writ_expr_ref(model, writ_node_child(model->unit, source, 0));
        writ_type_id result = callee.kind == WRIT_REF_FN ? model->fns.data[callee.index].result : 0;
        return result ? writ_type(model, writ_type_physical(model, result))->key.target : 0;
    }
    default:
        return 0;
    }
}
