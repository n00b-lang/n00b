#include "parse/infer.h"
#include "parse/symtab.h"
#include "parse/tree_util.h"
#include "parse/tree_visit.h"
#include "ir/typelang.h"
#include "private/parse/grammar.h"
#include "lib/alloc.h"
#include "lib/list.h"
#include "th/environment.h"
#include "th/typehash.h"

#include <string.h>

typedef struct {
    core_string_t name;
    th_type     *var;
} tvar_t;

core_list_decl(tvar_t);
core_list_decl(th_arg);
core_list_decl(th_param);
core_list_decl(th_kwparam);

typedef struct {
    th_ctx       *tc;
    parse_symtab_t *st;
    parse_visit_t  *v;
    core_list_t(tvar_t) tvars;
    bool          failed;
} ictx;

static th_type *eval(ictx *c, const parse_infer_expr_t *e);
static th_type *fresh(ictx *c);

static bool node_is(parse_grammar_t *grammar, parse_tree_t *node,
                    const char *name)
{
    return node && !core_tree_is_leaf(node) &&
        core_string_eq(parse_tree_nt_name(grammar, node),
                       core_string_from_cstr(name));
}

static parse_tree_t *child_named(parse_grammar_t *grammar,
                                 parse_tree_t *node, const char *name)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (node_is(grammar, child, name)) return child;
    }
    return nullptr;
}

static th_type *node_type(ictx *c, parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    parse_nt_node_t *pn = &core_tree_node_value(node);
    if (!pn->type) pn->type = fresh(c);
    return pn->type;
}

static th_type *transparent_expression_type(ictx *c, parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    parse_nt_node_t *pn = &core_tree_node_value(node);
    if (pn->sym && pn->sym->type) return pn->sym->type;

    th_type *type = pn->type;
    if (type && th_kind_of(th_resolve(type)) != TH_VAR) return type;

    parse_tree_t *only_child = nullptr;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (!child || core_tree_is_leaf(child)) continue;
        if (only_child) return type;
        only_child = child;
    }
    th_type *child_type = transparent_expression_type(c, only_child);
    if (child_type && type) {
        th_type *unified = th_unify_in_place(c->tc, type, child_type);
        return unified ? unified : type;
    }
    return child_type ? child_type : type;
}

static bool tree_contains(parse_tree_t *root, const parse_tree_t *target)
{
    if (root == target) return true;
    if (!root || core_tree_is_leaf(root)) return false;
    for (size_t i = 0; i < core_tree_num_children(root); i++)
        if (tree_contains(core_tree_child(root, i), target)) return true;
    return false;
}

static th_type *method_call_shape(ictx *c, th_type *result)
{
    parse_tree_t *parent = c->v->parent;
    parse_tree_t *node = c->v->node;
    if (!node_is(c->v->grammar, parent, "postfix-expr") ||
        core_tree_num_children(parent) < 3 ||
        core_tree_child(parent, 0) != node ||
        !core_tree_is_leaf(core_tree_child(parent, 1)) ||
        !parse_tree_leaf_is(core_tree_child(parent, 1),
                             CORE_STRING_STATIC("(")))
        return nullptr;

    parse_tree_t *call_args = core_tree_child(parent, 2);
    parse_tree_t *positional = child_named(c->v->grammar, call_args,
                                            "positional-args");
    parse_tree_t *keywords = child_named(c->v->grammar, call_args,
                                          "keyword-args");
    core_list_t(th_param) params = core_list_new(th_param);
    core_list_t(th_kwparam) kwparams = core_list_new(th_kwparam);
    if (positional) {
        for (size_t i = 0; i < core_tree_num_children(positional); i++) {
            parse_tree_t *arg = core_tree_child(positional, i);
            if (!node_is(c->v->grammar, arg, "expression")) continue;
            core_list_push(params, ((th_param){
                .mode = TH_IN,
                .type = node_type(c, arg),
            }));
        }
    }
    if (keywords) {
        for (size_t i = 0; i < core_tree_num_children(keywords); i++) {
            parse_tree_t *arg = core_tree_child(keywords, i);
            if (!node_is(c->v->grammar, arg, "call-kw-arg")) continue;
            parse_tree_t *value = child_named(c->v->grammar, arg, "expression");
            core_string_t name = parse_tree_extract_first_identifier(arg);
            if (!name.data || !value) {
                c->failed = true;
                continue;
            }
            core_list_push(kwparams, ((th_kwparam){
                .name = name.data,
                .mode = TH_IN,
                .type = node_type(c, value),
            }));
        }
    }
    th_type *shape = th_func(c->tc, result, params.data, params.len, nullptr,
                             kwparams.data, kwparams.len);
    core_list_free(params);
    core_list_free(kwparams);
    return shape;
}

static th_type *index_call_shape(ictx *c, th_type *result)
{
    parse_tree_t *node = c->v->node;
    if (!node || core_tree_is_leaf(node) || core_tree_num_children(node) < 4 ||
        !core_tree_is_leaf(core_tree_child(node, 1)) ||
        !parse_tree_leaf_is(core_tree_child(node, 1),
                            CORE_STRING_STATIC("[")))
        return nullptr;
    parse_tree_t *index_contents = core_tree_child(node, 2);
    parse_tree_t *contents = nullptr;
    size_t expression_count = 0;
    for (size_t i = 0; index_contents && !core_tree_is_leaf(index_contents) &&
         i < core_tree_num_children(index_contents); i++) {
        parse_tree_t *child = core_tree_child(index_contents, i);
        if (node_is(c->v->grammar, child, "expression")) {
            contents = child;
            expression_count++;
        } else if (core_tree_is_leaf(child) &&
                   parse_tree_leaf_is(child, CORE_STRING_STATIC(":"))) {
            return nullptr;
        }
    }
    if (!contents || expression_count != 1) return nullptr;
    th_param parameter = {.mode = TH_IN, .type = node_type(c, contents)};
    return th_func(c->tc, result, &parameter, 1, nullptr, nullptr, 0);
}

static th_type *slice_call_shape(ictx *c, th_type *result)
{
    parse_tree_t *node = c->v->node;
    if (!node || core_tree_is_leaf(node) || core_tree_num_children(node) < 4 ||
        !core_tree_is_leaf(core_tree_child(node, 1)) ||
        !parse_tree_leaf_is(core_tree_child(node, 1),
                            CORE_STRING_STATIC("[")))
        return nullptr;

    parse_tree_t *contents = core_tree_child(node, 2);
    bool colon = false;
    size_t lower_count = 0;
    size_t upper_count = 0;
    th_type *integer = th_name(c->tc, "int");
    for (size_t i = 0; contents && !core_tree_is_leaf(contents) &&
         i < core_tree_num_children(contents); i++) {
        parse_tree_t *part = core_tree_child(contents, i);
        if (core_tree_is_leaf(part) &&
            parse_tree_leaf_is(part, CORE_STRING_STATIC(":"))) {
            if (colon) return nullptr;
            colon = true;
        } else if (node_is(c->v->grammar, part, "expression")) {
            size_t *count = colon ? &upper_count : &lower_count;
            if (++*count > 1 ||
                !th_unify_in_place(c->tc,
                    transparent_expression_type(c, part), integer)) {
                c->failed = true;
                return nullptr;
            }
        }
    }
    if (!colon) return nullptr;

    th_param bounds[] = {
        {.mode = TH_IN, .type = integer},
        {.mode = TH_IN, .type = integer},
    };
    return th_func(c->tc, result, bounds, 2, nullptr, nullptr, 0);
}

static bool is_index_postfix(parse_grammar_t *grammar, parse_tree_t *node)
{
    return node_is(grammar, node, "postfix-expr") &&
        core_tree_num_children(node) >= 4 &&
        core_tree_is_leaf(core_tree_child(node, 1)) &&
        parse_tree_leaf_is(core_tree_child(node, 1),
                           CORE_STRING_STATIC("["));
}

static th_type *index_store_call_shape(ictx *c, th_type *result)
{
    parse_tree_t *node = c->v->node;
    parse_tree_t *assignment = nullptr;
    for (parse_tree_t *parent = c->v->parent; parent;
         parent = core_tree_node_value(parent).parent) {
        if (node_is(c->v->grammar, parent, "assign-stmt")) {
            assignment = parent;
            break;
        }
        if (is_index_postfix(c->v->grammar, parent) &&
            tree_contains(core_tree_child(parent, 0), node))
            return nullptr;
    }
    if (!node_is(c->v->grammar, assignment, "assign-stmt") ||
        core_tree_num_children(assignment) < 3 ||
        !tree_contains(core_tree_child(assignment, 0), node) ||
        core_tree_num_children(node) < 4 ||
        !core_tree_is_leaf(core_tree_child(node, 1)) ||
        !parse_tree_leaf_is(core_tree_child(node, 1),
                            CORE_STRING_STATIC("[")))
        return nullptr;

    parse_tree_t *contents = nullptr;
    parse_tree_t *index_contents = core_tree_child(node, 2);
    size_t expression_count = 0;
    for (size_t i = 0; index_contents && !core_tree_is_leaf(index_contents) &&
         i < core_tree_num_children(index_contents); i++) {
        parse_tree_t *child = core_tree_child(index_contents, i);
        if (node_is(c->v->grammar, child, "expression")) {
            contents = child;
            expression_count++;
        } else if (core_tree_is_leaf(child) &&
                   parse_tree_leaf_is(child, CORE_STRING_STATIC(":"))) {
            return nullptr;
        }
    }
    parse_tree_t *value = core_tree_child(assignment, 2);
    if (!contents || expression_count != 1 ||
        !node_is(c->v->grammar, value, "expression"))
        return nullptr;

    th_param parameters[] = {
        {.mode = TH_IN, .type = node_type(c, contents)},
        {.mode = TH_IN, .type = node_type(c, value)},
    };
    return th_func(c->tc, result, parameters, 2, nullptr, nullptr, 0);
}

static bool index_is_assignment_target(parse_visit_t *visit,
                                       parse_tree_t *node)
{
    for (parse_tree_t *parent = core_tree_node_value(node).parent; parent;
         node = parent, parent = core_tree_node_value(parent).parent) {
        if (node_is(visit->grammar, parent, "assign-stmt"))
            return core_tree_num_children(parent) > 0 &&
                   tree_contains(core_tree_child(parent, 0), node);
        if (is_index_postfix(visit->grammar, parent) &&
            tree_contains(core_tree_child(parent, 0), node)) return false;
    }
    return false;
}

static th_type *infer_method_obligation(ictx *c, th_type *receiver,
                                        const char *method_name,
                                        th_type *result, th_type *call_shape)
{
    if (!th_require_method(c->tc, receiver, method_name, result, call_shape))
        c->failed = true;
    return result;
}

static bool unify_method_call_shape(ictx *c, th_type *signature,
                                    th_type *receiver, th_type *result,
                                    th_type *call_shape)
{
    const th_param *self = th_func_param(signature, 0);
    th_type *ret = th_func_ret(signature);
    if (!self || !ret || !th_unify_in_place(c->tc, self->type, receiver) ||
        !th_unify_in_place(c->tc, result, ret))
        return false;
    if (!call_shape) return true;

    size_t actual_count = th_func_nparams(call_shape);
    size_t formal_count = th_func_nparams(signature);
    if (actual_count > formal_count - 1 && !th_func_rest(signature))
        return false;
    for (size_t i = 0; i < actual_count; i++) {
        const th_param *actual = th_func_param(call_shape, i);
        const th_param *formal = i + 1 < formal_count
            ? th_func_param(signature, i + 1) : th_func_rest(signature);
        if (!actual || !formal ||
            !th_unify_in_place(c->tc, actual->type, formal->type))
            return false;
    }
    for (size_t i = 0; i < th_func_nkw(call_shape); i++) {
        const th_kwparam *actual = th_func_kw(call_shape, i);
        const th_kwparam *formal = actual
            ? th_func_kw_lookup(signature, actual->name) : nullptr;
        if (!actual || !formal ||
            !th_unify_in_place(c->tc, actual->type, formal->type))
            return false;
    }
    return true;
}

static th_type *
fresh(ictx *c)
{
    return th_var(c->tc);
}

static th_type *
method_requirement_return(ictx *c, th_type *receiver,
                          const th_env_method_requirement_t *requirement,
                          th_type *result, th_type *call_shape)
{
    if (!requirement || !requirement->signature ||
        !th_func_nparams(requirement->signature)) {
        c->failed = true;
        return nullptr;
    }
    th_type *self = th_func_param(requirement->signature, 0)->type;
    if (!th_var_constrain(
            c->tc, self,
            (th_constraint){.kind = TH_CON_IMPLEMENTS,
                            .iface = requirement->trait_name}) ||
        !unify_method_call_shape(c, requirement->signature, receiver,
                                 result, call_shape)) {
        c->failed = true;
        return nullptr;
    }
    return result;
}

static parse_tree_t *
child(ictx *c, int32_t ix)
{
    if (ix < 0 || (size_t)ix >= core_tree_num_children(c->v->node)) {
        return nullptr;
    }

    return core_tree_child(c->v->node, (size_t)ix);
}

static th_type *
type_of(ictx *c, int32_t ix)
{
    parse_tree_t *k = child(c, ix);

    if (!k || core_tree_is_leaf(k)) {
        return nullptr;
    }

    parse_nt_node_t *pn = &core_tree_node_value(k);

    if (!pn->type) {
        pn->type = fresh(c);
    }

    return pn->type;
}

static th_type *
tvar(ictx *c, core_string_t name)
{
    for (size_t i = 0; i < c->tvars.len; i++) {
        if (core_string_eq(c->tvars.data[i].name, name)) {
            return c->tvars.data[i].var;
        }
    }

    th_type *t = th_var_named(c->tc, name.data ? name.data : "t");

    core_list_push(c->tvars, ((tvar_t){.name = name, .var = t}));

    return t;
}

static th_type *
instantiate_symbol_type(ictx *c, parse_sym_entry_t *sym)
{
    if (!sym || !sym->type) return nullptr;
    if (th_is_scheme(sym->type))
        return th_instantiate(c->tc, sym->type);
    if (sym->kind != PARSE_SYM_FUNCTION)
        return sym->type;

    size_t capacity = 0;
    for (parse_sym_entry_t *entry = sym->scope
            ? sym->scope->first_in_scope : nullptr;
         entry;
         entry = entry->next_in_scope) {
        if (entry->kind == PARSE_SYM_FUNCTION) continue;
        capacity += (entry->type != nullptr) + (entry->error_type != nullptr);
    }
    th_type **environment = capacity
        ? core_alloc_array(th_type *, capacity) : nullptr;
    size_t count = 0;
    for (parse_sym_entry_t *entry = sym->scope
            ? sym->scope->first_in_scope : nullptr;
         entry;
         entry = entry->next_in_scope) {
        if (entry->kind == PARSE_SYM_FUNCTION) continue;
        if (entry->type) environment[count++] = entry->type;
        if (entry->error_type) environment[count++] = entry->error_type;
    }
    th_type *scheme = th_generalize_excluding(c->tc, sym->type, environment,
                                               count);
    core_free(environment);
    return th_instantiate(c->tc, scheme);
}

static th_type *
symbol_type(ictx *c, int32_t ix)
{
    parse_tree_t *k = child(c, ix);
    core_string_t      nm = k ? parse_tree_extract_first_identifier(k)
                             : (core_string_t){0};

    if (!nm.u8_bytes || !c->st) {
        return nullptr;
    }

    parse_scope_t *scope = c->v->pn->scope
                           ? c->v->pn->scope
                           : parse_symtab_current_scope(c->st,
                                                      core_string_empty());
    parse_sym_entry_t *sym = parse_symtab_lookup_scoped(c->st, scope,
                                                    core_string_empty(), nm);

    if (!sym) {
        return nullptr;
    }

    if (!sym->type) {
        sym->type = fresh(c);
    }

    return instantiate_symbol_type(c, sym);
}

static th_type *
named_arg_last(const th_type *t)
{
    size_t n = th_named_nargs(t);

    if (!n) {
        return nullptr;
    }

    const th_arg *a = th_named_arg(t, n - 1);

    return a && a->kind == TH_ARG_TYPE ? a->type : nullptr;
}

static th_type *
named_arg_first(const th_type *t)
{
    if (!th_named_nargs(t)) {
        return nullptr;
    }

    const th_arg *a = th_named_arg(t, 0);

    return a && a->kind == TH_ARG_TYPE ? a->type : nullptr;
}

static bool
named_is(const th_type *t, const char *want)
{
    const char *nm    = nullptr;
    size_t      nlen  = 0;
    size_t      nargs = 0;
    bool        var   = false;

    if (!th_named_info(t, &nm, &nlen, &var, &nargs) || !nm) {
        return false;
    }

    return nlen == strlen(want) && !memcmp(nm, want, nlen);
}

static bool
builtin_comparable(const th_type *type)
{
    if (th_kind_of(type) == TH_BITINT) return true;
    const char *name = nullptr;
    size_t nlen = 0;
    if (!th_named_info(type, &name, &nlen, nullptr, nullptr)) return false;
    static const char *const names[] = {
        "int", "uint", "i8", "i16", "i32", "i64", "i128",
        "u8", "u16", "u32", "u64", "u128", "float", "f64",
        "string",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strlen(names[i]) == nlen && !memcmp(name, names[i], nlen))
            return true;
    return false;
}

static bool
leaf_has_text(parse_tree_t *leaf, void *arg)
{
    (void)arg;

    parse_token_info_t *tok = core_tree_leaf_value(leaf);

    return tok && core_option_is_set(tok->value)
        && core_option_get(tok->value).u8_bytes;
}

static core_string_t
last_text(parse_tree_t *node)
{
    parse_tree_t *leaf = parse_tree_scan_leaves(node, true, leaf_has_text,
                                                 nullptr);

    return leaf ? core_option_get(core_tree_leaf_value(leaf)->value)
                : (core_string_t){0};
}

static bool
text_is(core_string_t text, const char *want)
{
    size_t n = strlen(want);

    return text.u8_bytes == n && !memcmp(text.data, want, n);
}

static th_type *
project(ictx *c, parse_infer_proj_t op, th_type *t, core_string_t explicit_name)
{
    if (!t) {
        return fresh(c);
    }

    switch (op) {
    case PARSE_INFER_RETURN_OF: {
        th_type *r = th_func_ret(t);

        if (r) {
            return r;
        }

        th_kind kind = th_kind_of(t);

        return kind == TH_NAMED || kind == TH_RECORD ? t : fresh(c);
    }

    case PARSE_INFER_ELEMENT_OF: {
        parse_tree_t *result_node = c->v->node;
        th_type *result = node_type(c, result_node);
        if (index_is_assignment_target(c->v, result_node)) {
            th_type *call_shape = index_store_call_shape(c, result);
            if (call_shape)
                return infer_method_obligation(c, t, "store", result,
                                               call_shape);
            c->failed = true;
            return result;
        }
        th_type *call_shape = index_call_shape(c, result);
        if (call_shape)
            return infer_method_obligation(c, t, "load", result, call_shape);

        th_type *e = th_seq_elem(t);

        if (e) {
            return e;
        }

        if (named_is(t, "dict")) {
            e = named_arg_last(t);
        }
        else if (named_is(t, "string")) {
            e = t;
        }
        else {
            e = named_arg_first(t);
        }

        return e ? e : fresh(c);
    }

    case PARSE_INFER_SLICE_OF: {
        parse_tree_t *slice_node = c->v->node;
        th_type *result = node_type(c, slice_node);
        th_type *call_shape = slice_call_shape(c, result);
        if (!call_shape) {
            c->failed = true;
            return result;
        }
        return infer_method_obligation(c, t, "slice", result, call_shape);
    }

    case PARSE_INFER_UNWRAP_RESULT: {
        return t;
    }

    case PARSE_INFER_METHOD_RETURN: {
        core_string_t name = explicit_name.data
                               ? explicit_name
                               : last_text(c->v->node);

        if (!name.data) {
            return fresh(c);
        }

        if (named_is(t, "option") || named_is(t, "result")) {
            if (text_is(name, "is_set?") || text_is(name, "ok?")
                || text_is(name, "is_ok?")) {
                return th_name(c->tc, "bool");
            }
            if (text_is(name, "unwrap")) {
                th_type *inner = named_arg_first(t);

                return inner ? inner : fresh(c);
            }
        }

        char *method_name = core_alloc_array(char, name.u8_bytes + 1);

        memcpy(method_name, name.data, name.u8_bytes);
        method_name[name.u8_bytes] = 0;

        th_type *signature = th_method_lookup(c->tc, t, method_name);

        if (!signature) {
            signature = th_record_lookup(t, method_name);
        }

        if (!signature) {
            size_t requirement_count = th_env_method_requirements(
                c->tc, method_name, nullptr, 0);
            th_type *method_result = fresh(c);
            th_type *call_shape = method_call_shape(c, method_result);
            if (requirement_count == 1) {
                th_env_method_requirement_t requirement = {0};
                th_env_method_requirements(c->tc, method_name, &requirement,
                                           1);
                th_type *resolved = method_requirement_return(
                    c, t, &requirement, method_result, call_shape);
                if (resolved) {
                    core_free(method_name);
                    return resolved;
                }
            } else if (requirement_count > 1) {
                if (th_kind_of(th_resolve(t)) == TH_VAR) {
                    if (th_var_constrain(
                            c->tc, th_resolve(t),
                            (th_constraint){
                                .kind = TH_CON_METHOD,
                                .method = {.name = method_name,
                                           .result = method_result,
                                           .call = call_shape}})) {
                        core_free(method_name);
                        return method_result;
                    }
                    else
                        c->failed = true;
                } else {
                    th_env_method_requirement_t *requirements =
                        core_alloc_array(th_env_method_requirement_t,
                                         requirement_count);
                    th_env_method_requirements(c->tc, method_name,
                                               requirements,
                                               requirement_count);
                    th_env_method_t selected = {0};
                    const char *trait_name = nullptr;
                    th_env_method_status_t status =
                        th_env_method_resolve_by_name(
                            c->tc, t, method_name, &selected, &trait_name);
                    if (status == TH_ENV_METHOD_FOUND) {
                        for (size_t i = 0; i < requirement_count; i++) {
                            if (strcmp(requirements[i].trait_name,
                                       trait_name))
                                continue;
                            th_type *resolved = method_requirement_return(
                                c, t, &requirements[i], method_result,
                                call_shape);
                            core_free(requirements);
                            core_free(method_name);
                            return resolved ? resolved : fresh(c);
                        }
                        c->failed = true;
                    } else {
                        c->failed = true;
                    }
                    core_free(requirements);
                }
            } else {
                th_type *resolved = infer_method_obligation(
                    c, t, method_name, method_result, call_shape);
                core_free(method_name);
                return resolved;
            }
            core_free(method_name);
            return method_result;
        }

        th_type *ret = signature ? th_func_ret(signature) : nullptr;

        core_free(method_name);

        return ret ? ret : fresh(c);
    }
    }

    return fresh(c);
}

static void
spread_args(ictx *c, int32_t ix, core_list_t(th_arg) * out)
{
    parse_tree_t *node = child(c, ix);

    if (!node) {
        return;
    }

    size_t nc = core_tree_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        parse_tree_t *k = core_tree_child(node, i);

        if (!k || core_tree_is_leaf(k)) {
            continue;
        }

        th_type *t = ir_type_from_node(c->tc, c->v->grammar, k);

        if (t) {
            core_list_push(*out,
                           ((th_arg){.kind = TH_ARG_TYPE, .type = t}));
        }
    }
}

static void
app_args(ictx *c, const parse_infer_expr_t *e, core_list_t(th_arg) * out)
{
    for (size_t i = 0; i < e->n_args; i++) {
        const parse_infer_expr_t *a = e->args[i];

        if (a->kind == PARSE_INFER_SPREAD) {
            spread_args(c, a->index, out);

            continue;
        }

        th_type *t = eval(c, a);

        if (!t) {
            c->failed = true;

            return;
        }

        core_list_push(*out, ((th_arg){.kind = TH_ARG_TYPE, .type = t}));
    }
}

static th_type *
eval(ictx *c, const parse_infer_expr_t *e)
{
    if (!e) {
        return nullptr;
    }

    switch (e->kind) {
    case PARSE_INFER_CHILD: {
        th_type *t = type_of(c, e->index);

        return t ? t : fresh(c);
    }

    case PARSE_INFER_SELF:
        if (!c->v->pn->type) {
            c->v->pn->type = fresh(c);
        }

        return c->v->pn->type;

    case PARSE_INFER_RETURN: {
        parse_sym_entry_t *sym
            = c->st ? parse_symtab_lookup(c->st, core_string_empty(),
                                        CORE_STRING_STATIC("$return"))
                    : nullptr;

        if (sym) {
            if (!sym->type) {
                sym->type = fresh(c);
            }

            return sym->type;
        }

        return fresh(c);
    }

    case PARSE_INFER_TVAR:
        return tvar(c, e->name);

    case PARSE_INFER_NAMED:
        return th_name(c->tc, e->name.data ? e->name.data : "");

    case PARSE_INFER_NAMED_APP: {
        core_list_t(th_arg) args = core_list_new(th_arg);

        app_args(c, e, &args);

        th_type *r = th_apply(c->tc, e->name.data ? e->name.data : "",
                              args.data, args.len);

        core_list_free(args);

        return r;
    }

    case PARSE_INFER_CHILD_APP: {
        parse_tree_t *k  = child(c, e->index);
        core_string_t      nm = k ? parse_tree_extract_first_identifier(k)
                                 : (core_string_t){0};

        if (!nm.u8_bytes) {
            c->failed = true;

            return nullptr;
        }

        core_list_t(th_arg) args = core_list_new(th_arg);

        app_args(c, e, &args);

        th_type *r = th_apply(c->tc, nm.data, args.data, args.len);

        core_list_free(args);

        return r;
    }

    case PARSE_INFER_SPREAD:
        c->failed = true;

        return nullptr;

    case PARSE_INFER_LOOKUP: {
        th_type *t = symbol_type(c, e->index);

        return t ? t : fresh(c);
    }

    case PARSE_INFER_PROJECT: {
        th_type *input = e->lhs ? eval(c, e->lhs) : type_of(c, e->index);
        return project(c, e->proj, input, e->name);
    }

    case PARSE_INFER_COMPARE: {
        th_type *left = eval(c, e->lhs);
        th_type *right = eval(c, e->rhs);
        if (!left || !right) {
            c->failed = true;
            return th_name(c->tc, "bool");
        }
        if (builtin_comparable(left)) {
            if (!th_unify_in_place(c->tc, left, right)) c->failed = true;
            return th_name(c->tc, "bool");
        }
        th_type *comparison = ir_type_from_text(c->tc,
            CORE_STRING_STATIC("i:2"));
        th_param parameter = {.mode = TH_IN, .type = right};
        th_type *call_shape = th_func(c->tc, comparison, &parameter, 1,
                                      nullptr, nullptr, 0);
        infer_method_obligation(c, left, "cmp", comparison, call_shape);
        return th_name(c->tc, "bool");
    }

    case PARSE_INFER_UNIFY: {
        th_type *l = eval(c, e->lhs);
        th_type *r = eval(c, e->rhs);

        if (!l || !r) {
            c->failed = true;

            return l;
        }

        return th_unify_in_place(c->tc, l, r);
    }

    case PARSE_INFER_SUM: {
        th_type *l = eval(c, e->lhs);
        th_type *r = eval(c, e->rhs);

        if (!l || !r) {
            c->failed = true;

            return l;
        }

        th_type *arms[2] = {l, r};

        return th_sum(c->tc, TH_UNION, arms, 2);
    }
    }

    return nullptr;
}

th_type *
parse_infer_eval(const parse_infer_expr_t *e,
               parse_visit_t            *v,
               th_ctx                 *tc,
               parse_symtab_t           *st,
               bool                   *ok)
{
    if (ok) {
        *ok = false;
    }

    if (!e || !v || !tc) {
        return nullptr;
    }

    parse_tree_t *first_leaf = parse_tree_scan_leaves(v->node, false, nullptr,
                                                       nullptr);
    parse_tree_t *last_leaf  = parse_tree_scan_leaves(v->node, true, nullptr,
                                                       nullptr);
    parse_token_info_t *first = first_leaf ? core_tree_leaf_value(first_leaf)
                                           : nullptr;
    parse_token_info_t *last  = last_leaf ? core_tree_leaf_value(last_leaf)
                                          : first;

    if (first) {
        const char *file = core_option_is_set(first->file)
                               ? core_option_get(first->file).data
                               : nullptr;

        th_ctx_set_span(tc,
                        (th_span_t){.file = file,
                                    .start_line = first->line,
                                    .start_col = first->column,
                                    .end_line = last && last->endline
                                                    ? last->endline
                                                    : last ? last->line
                                                           : first->line,
                                    .end_col = last && last->endcol
                                                   ? last->endcol
                                                   : last ? last->column + 1
                                                          : first->column + 1});
    }

    ictx c = {.tc = tc, .st = st, .v = v, .tvars = core_list_new(tvar_t)};

    th_type *t = eval(&c, e);

    if (ok) {
        *ok = !c.failed && t != nullptr;
    }

    core_list_free(c.tvars);

    return t;
}
