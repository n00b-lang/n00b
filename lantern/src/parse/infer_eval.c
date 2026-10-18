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
#include "private/th/internal.h"

#include <string.h>

typedef struct {
    core_string_t name;
    th_type     *var;
} tvar_t;

core_list_decl(tvar_t);
core_list_decl(th_arg);
core_list_decl(th_param);
core_list_decl(th_kwparam);
core_list_decl(parse_tree_ptr_t);

typedef struct {
    th_ctx       *tc;
    parse_symtab_t *st;
    parse_visit_t  *v;
    parse_infer_type_node_resolver_fn type_node_resolver;
    void *type_node_resolver_context;
    core_list_t(tvar_t) tvars;
    bool          failed;
} ictx;

static th_type *eval(ictx *c, const parse_infer_expr_t *e);
static th_type *fresh(ictx *c);

static th_type *named_application(ictx *c, const char *name,
                                  const th_arg *args, size_t n)
{
    uint8_t attrs;
    if (n == 1 && args[0].kind == TH_ARG_TYPE
        && th_indirection_attrs(name, &attrs))
        return th_ind(c->tc, attrs, args[0].type);
    return th_apply(c->tc, name, args, n);
}

static bool node_is(parse_grammar_t *grammar, parse_tree_t *node,
                    const char *name)
{
    return node && !core_tree_is_leaf(node) &&
        core_string_eq(parse_tree_nt_name(grammar, node),
                       core_string_from_cstr(name));
}

static th_type *node_type(ictx *c, parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    if (!parse_tree_type(node)) parse_tree_set_type(node, fresh(c));
    return parse_tree_type(node);
}

static core_string_t
node_identifier_name(parse_tree_t *node)
{
    if (!node) return (core_string_t){0};
    if (core_tree_is_leaf(node)) return parse_tree_leaf_text(node);
    core_string_t unresolved = parse_tree_unresolved_name(node);
    if (unresolved.data) return unresolved;
    parse_sym_entry_t *sym = parse_tree_sym(node);
    if (sym && sym->name.data) return sym->name;
    if (core_tree_num_children(node) == 1) {
        parse_tree_t *child = core_tree_child(node, 0);
        if (core_tree_is_leaf(child)) return parse_tree_leaf_text(child);
    }
    return (core_string_t){0};
}

static th_type *transparent_expression_type(ictx *c, parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    parse_sym_entry_t *sym = parse_tree_sym(node);
    if (sym && sym->type) return sym->type;

    th_type *type = parse_tree_type(node);
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

static parse_tree_t *method_call_parent(ictx *c)
{
    parse_tree_t *parent = c->v->parent;
    return node_is(c->v->grammar, parent, "postfix-expr") &&
           core_tree_num_children(parent) >= 3 &&
           core_tree_child(parent, 0) == c->v->node &&
           core_tree_is_leaf(core_tree_child(parent, 1)) &&
           parse_tree_leaf_is(core_tree_child(parent, 1),
                              CORE_STRING_STATIC("("))
        ? parent : nullptr;
}

static th_type *call_shape(ictx *c, parse_tree_t *call, th_type *result)
{
    if (!node_is(c->v->grammar, call, "postfix-expr") ||
        core_tree_num_children(call) < 3 ||
        !core_tree_is_leaf(core_tree_child(call, 1)) ||
        !parse_tree_leaf_is(core_tree_child(call, 1),
                            CORE_STRING_STATIC("("))) return nullptr;

    parse_tree_t *call_args = core_tree_child(call, 2);
    parse_tree_t *positional = parse_tree_find_direct_child_by_nt_name(
        c->v->grammar, call_args, CORE_STRING_STATIC("positional-args"));
    parse_tree_t *keywords = parse_tree_find_direct_child_by_nt_name(
        c->v->grammar, call_args, CORE_STRING_STATIC("keyword-args"));
    core_list_t(th_param) params = core_list_new(th_param);
    core_list_t(th_kwparam) kwparams = core_list_new(th_kwparam);
    if (positional) {
        for (size_t i = 0; i < core_tree_num_children(positional); i++) {
            parse_tree_t *arg = core_tree_child(positional, i);
            if (!node_is(c->v->grammar, arg, "expression")) continue;
            core_list_push(params, ((th_param){
                .mode = TH_IN,
                .type = parse_tree_argument_type(arg)
                    ? parse_tree_argument_type(arg)
                    : node_type(c, arg),
            }));
        }
    }
    core_list_t(parse_tree_ptr_t) keyword_nodes =
        core_list_new(parse_tree_ptr_t);
    for (size_t i = 0; keywords && i < core_tree_num_children(keywords);
         i++) {
        parse_tree_t *arg = core_tree_child(keywords, i);
        if (node_is(c->v->grammar, arg, "call-kw-arg")) {
            core_list_push(keyword_nodes, arg);
            continue;
        }
        if (core_tree_is_leaf(arg) || !core_tree_node_value(arg).group_top)
            continue;
        for (size_t j = 0; j < core_tree_num_children(arg); j++) {
            parse_tree_t *item = core_tree_child(arg, j);
            if (node_is(c->v->grammar, item, "call-kw-arg"))
                core_list_push(keyword_nodes, item);
        }
    }
    for (size_t i = 0; i < keyword_nodes.len; i++) {
        parse_tree_t *arg = keyword_nodes.data[i];
        parse_tree_t *value = parse_tree_find_direct_child_by_nt_name(
            c->v->grammar, arg, CORE_STRING_STATIC("expression"));
        parse_tree_t *name_node = core_tree_num_children(arg)
            ? core_tree_child(arg, 0) : nullptr;
        core_string_t name = node_identifier_name(name_node);
        if (!name.data || !value) {
            c->failed = true;
            continue;
        }
        core_list_push(kwparams, ((th_kwparam){
            .name = th_strdup(c->tc, name.data, name.u8_bytes),
            .mode = TH_IN,
            .type = parse_tree_argument_type(value)
                ? parse_tree_argument_type(value)
                : node_type(c, value),
        }));
    }
    core_list_free(keyword_nodes);
    th_type *shape = th_func(c->tc, result, params.data, params.len, nullptr,
                             kwparams.data, kwparams.len);
    core_list_free(params);
    core_list_free(kwparams);
    return shape;
}

static bool
call_has_member_callee(ictx *c, parse_tree_t *call)
{
    if (!node_is(c->v->grammar, call, "postfix-expr")
        || !core_tree_num_children(call)) return false;
    parse_tree_t *callee = core_tree_child(call, 0);
    return node_is(c->v->grammar, callee, "postfix-expr")
        && core_tree_num_children(callee) >= 3
        && core_tree_is_leaf(core_tree_child(callee, 1))
        && parse_tree_leaf_is(core_tree_child(callee, 1),
                              CORE_STRING_STATIC("."));
}

static th_type *method_call_shape(ictx *c, th_type *result)
{
    parse_tree_t *parent = method_call_parent(c);
    if (!parent) return nullptr;
    return call_shape(c, parent, result);
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
        if (node_is(c->v->grammar, part, "range-sep")
            || (core_tree_is_leaf(part) &&
                parse_tree_leaf_is(part, CORE_STRING_STATIC(":")))) {
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

static parse_tree_t *assignment_target(parse_visit_t *visit)
{
    for (size_t distance = 1; ; distance++) {
        parse_tree_t *parent = parse_visit_ancestor(visit, distance);
        if (!parent) return nullptr;
        parse_tree_t *child = parse_visit_ancestor(visit, distance - 1);
        if (node_is(visit->grammar, parent, "assign-stmt"))
            return core_tree_child(parent, 0) == child ? parent : nullptr;
        if (core_tree_num_children(parent) != 1) return nullptr;
    }
}

static th_type *index_store_call_shape(ictx *c, th_type *result,
                                       parse_tree_t *assignment)
{
    parse_tree_t *node = c->v->node;
    if (!node_is(c->v->grammar, assignment, "assign-stmt") ||
        core_tree_num_children(assignment) < 3 ||
        core_tree_num_children(node) < 4 ||
        !core_tree_is_leaf(core_tree_child(node, 1)) ||
        !parse_tree_leaf_is(core_tree_child(node, 1),
                            CORE_STRING_STATIC("[")))
        return nullptr;

    parse_tree_t *contents = nullptr;
    parse_tree_t *index_contents = core_tree_child(node, 2);
    if (!node_is(c->v->grammar, index_contents, "index-contents"))
        return nullptr;
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
        {.mode = TH_IN, .type = parse_tree_argument_type(value)
            ? parse_tree_argument_type(value)
            : node_type(c, value)},
    };
    return th_func(c->tc, result, parameters, 2, nullptr, nullptr, 0);
}

static th_type *slice_store_call_shape(ictx *c, th_type *result,
                                       parse_tree_t *assignment)
{
    parse_tree_t *node = c->v->node;
    if (!node_is(c->v->grammar, assignment, "assign-stmt") ||
        !node_is(c->v->grammar, core_tree_child(node, 2), "slice-contents") ||
        !slice_call_shape(c, result))
        return nullptr;
    parse_tree_t *value = core_tree_child(assignment, 2);
    if (!node_is(c->v->grammar, value, "expression")) return nullptr;
    th_type *bound = th_option(c->tc, th_name(c->tc, "int"));
    th_param parameters[] = {
        {.mode = TH_IN, .type = bound},
        {.mode = TH_IN, .type = bound},
        {.mode = TH_IN, .type = th_name(c->tc, "bool")},
        {.mode = TH_IN, .type = node_type(c, value)},
    };
    return th_func(c->tc, result, parameters, 4, nullptr, nullptr, 0);
}

static th_type *infer_method_obligation(ictx *c, th_type *receiver,
                                        const char *method_name,
                                        th_type *result, th_type *call_shape)
{
    if (!th_require_method_at(c->tc, receiver, method_name, nullptr, result,
                              call_shape, c->v->node))
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
    size_t keyword_count = th_func_nkw(call_shape);
    size_t formal_count = th_func_nparams(signature);
    if (actual_count > formal_count - 1 && !th_func_rest(signature))
        return false;
    size_t argument_count = actual_count + keyword_count;
    const th_type **sources = argument_count
        ? core_alloc_array(const th_type *, argument_count) : nullptr;
    th_type **copies = argument_count
        ? core_alloc_array(th_type *, argument_count) : nullptr;
    if (argument_count && (!sources || !copies)) {
        core_free(sources);
        core_free(copies);
        return false;
    }
    for (size_t i = 0; i < actual_count; i++) {
        const th_param *actual = th_func_param(call_shape, i);
        sources[i] = actual ? actual->type : nullptr;
    }
    for (size_t i = 0; i < keyword_count; i++) {
        const th_kwparam *actual = th_func_kw(call_shape, i);
        sources[actual_count + i] = actual ? actual->type : nullptr;
    }
    bool matched = th_type_clone_many(c->tc, sources, copies,
                                      argument_count);
    for (size_t i = 0; i < actual_count; i++) {
        const th_param *formal = i + 1 < formal_count
            ? th_func_param(signature, i + 1) : th_func_rest(signature);
        if (!matched || !copies[i] || !formal ||
            !th_unify_in_place(c->tc, formal->type, copies[i])) {
            matched = false;
            break;
        }
    }
    for (size_t i = 0; matched && i < keyword_count; i++) {
        const th_kwparam *actual = th_func_kw(call_shape, i);
        const th_kwparam *formal = actual
            ? th_func_kw_lookup(signature, actual->name) : nullptr;
        if (!actual || !copies[actual_count + i]
            || (th_func_nkw(signature) && !formal)
            || (formal && !th_unify_in_place(c->tc, formal->type,
                                             copies[actual_count + i])))
            matched = false;
    }
    core_free(sources);
    core_free(copies);
    return matched;
}

static bool
unify_function_call_shape(ictx *c, th_type *signature, th_type *call_shape)
{
    size_t actual_count = th_func_nparams(call_shape);
    size_t formal_count = th_func_nparams(signature);
    const th_param *rest = th_func_rest(signature);
    if (actual_count > formal_count && !rest) return false;
    for (size_t i = 0; i < actual_count; i++) {
        const th_param *actual = th_func_param(call_shape, i);
        const th_param *formal = i < formal_count
            ? th_func_param(signature, i) : rest;
        if (!actual || !formal || !th_unify_probe(
                c->tc, formal->type, actual->type)
            || !th_unify_in_place(c->tc, formal->type, actual->type))
            return false;
    }
    for (size_t i = 0; i < th_func_nkw(call_shape); i++) {
        const th_kwparam *actual = th_func_kw(call_shape, i);
        const th_kwparam *formal = actual
            ? th_func_kw_lookup(signature, actual->name) : nullptr;
        if (!actual || (th_func_nkw(signature) && !formal) ||
            (formal && (!th_unify_probe(c->tc, formal->type, actual->type)
                || !th_unify_in_place(c->tc, formal->type, actual->type))))
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
    th_type *signature = th_instantiate(
        c->tc, th_generalize(c->tc, requirement->signature));
    if (!signature || !th_func_nparams(signature)) {
        c->failed = true;
        return nullptr;
    }
    th_type *self = th_func_param(signature, 0)->type;
    if (!th_var_constrain(
            c->tc, self,
            (th_constraint){.kind = TH_CON_IMPLEMENTS,
                            .iface = requirement->trait_name}) ||
        !unify_method_call_shape(c, signature, receiver,
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

    if (!parse_tree_type(k)) {
        parse_tree_set_type(k, fresh(c));
    }

    return parse_tree_type(k);
}

static bool
unify_children(ictx *c, parse_tree_t *node, th_type **joined)
{
    size_t nc = core_tree_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        parse_tree_t *k = core_tree_child(node, i);

        if (!k || core_tree_is_leaf(k)) {
            continue;
        }

        parse_nt_node_t *pn = &core_tree_node_value(k);

        if (pn->group_top) {
            if (!unify_children(c, k, joined)) {
                return false;
            }

            continue;
        }

        if (!parse_tree_type(k)) {
            parse_tree_set_type(k, fresh(c));
        }

        *joined = *joined
            ? th_unify_in_place(c->tc, *joined, parse_tree_type(k))
            : parse_tree_type(k);

        if (!*joined) {
            return false;
        }
    }

    return true;
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
    core_string_t nm = node_identifier_name(k);

    if (!nm.u8_bytes || !c->st) {
        return nullptr;
    }

    parse_scope_t *scope = parse_tree_scope(c->v->node)
                           ? parse_tree_scope(c->v->node)
                           : parse_symtab_current_scope(c->st,
                                                      core_string_empty());
    parse_sym_entry_t *sym = parse_symtab_lookup_visible(c->st, scope,
        core_string_empty(), nm, c->v->node);
    if (!sym && core_tree_num_children(c->v->node) == 1
        && core_tree_child(c->v->node, 0) == k) {
        parse_node_resolution_t resolution = parse_tree_resolution(c->v->node);
        if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL)
            sym = resolution.target.symbol;
    }

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
        "u8", "u16", "u32", "u64", "u128", "float", "f64", "f32",
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

    if (!tok) {
        return false;
    }

    core_option_t(core_string_t) value = parse_token_value(tok);

    return core_option_is_set(value) && core_option_get(value).u8_bytes;
}

static core_string_t
last_text(parse_tree_t *node)
{
    parse_tree_t *leaf = parse_tree_scan_leaves(node, true, leaf_has_text,
                                                 nullptr);

    return leaf ? core_option_get(parse_token_value(core_tree_leaf_value(leaf)))
                : (core_string_t){0};
}

static th_type *
field_projection(ictx *c, th_type *receiver, core_string_t name,
                 bool auto_deref)
{
    if (!name.data) return fresh(c);
    char *field_name = core_alloc_array(char, name.u8_bytes + 1);
    memcpy(field_name, name.data, name.u8_bytes);
    field_name[name.u8_bytes] = 0;
    th_type *result = fresh(c);
    th_type *field = th_record_lookup_in(c->tc, receiver, field_name);
    if (field) {
        core_free(field_name);
        return field;
    }
    th_type *resolved = th_resolve(receiver);
    bool constrained = resolved && th_kind_of(resolved) == TH_VAR
        && th_var_constrain(c->tc, resolved,
            (th_constraint){
                .kind = TH_CON_HAS_FIELD,
                .field = {.name = field_name, .type = result,
                          .auto_deref = auto_deref}});
    core_free(field_name);
    if (!constrained) {
        c->failed = true;
        th_err(c->tc, TH_ERR_NO_SUCH_FIELD, "has_field", nullptr, receiver,
               resolved && th_option_item(resolved)
                   ? "an option has no fields; unwrap its value before reading a field"
                   : "this type has no field with that name");
    }
    return result;
}

static bool
text_is(core_string_t text, const char *want)
{
    size_t n = strlen(want);

    return text.u8_bytes == n && !memcmp(text.data, want, n);
}

static th_type *
project(ictx *c, parse_infer_proj_t op, th_type *t,
        core_string_t explicit_name, core_string_t other_name)
{
    if (!t) {
        return fresh(c);
    }

    switch (op) {
    case PARSE_INFER_RETURN_OF: {
        th_type *result = fresh(c);
        th_type *shape = call_shape(c, c->v->node, result);
        th_type *resolved = th_resolve(t);
        if (resolved && th_kind_of(resolved) == TH_FUNCTION
            && call_has_member_callee(c, c->v->node)) {
            th_type *ret = th_func_ret(resolved);
            return ret ? ret : result;
        }
        th_type *instance = resolved && th_kind_of(resolved) == TH_BINDER
            ? th_instantiate(c->tc, resolved)
            : resolved && th_kind_of(resolved) == TH_FUNCTION
                ? th_type_clone(c->tc, resolved) : nullptr;
        if (instance && th_kind_of(instance) == TH_FUNCTION) {
            if (shape)
                (void)unify_function_call_shape(c, instance, shape);
            th_type *ret = th_func_ret(instance);
            return ret ? ret : result;
        }
        if (!shape) {
            th_type *r = th_func_ret(t);
            if (r) return r;
            th_kind kind = th_kind_of(t);
            return kind == TH_NAMED || kind == TH_RECORD ? t : fresh(c);
        }

        if (!resolved || th_kind_of(resolved) != TH_VAR ||
            !th_unify_in_place(c->tc, t, shape)) {
            c->failed = true;
            return result;
        }
        th_type *ret = th_func_ret(t);
        return ret ? ret : result;
    }

    case PARSE_INFER_ELEMENT_OF: {
        parse_tree_t *result_node = c->v->node;
        th_type *result = node_type(c, result_node);
        if (parse_tree_is_index_address_access(result_node)) {
            th_type *container = th_resolve(t);
            if (container && th_kind_of(container) == TH_INDIRECTION)
                container = th_resolve(th_ind_referent(container));
            th_type *element = th_seq_elem(container);
            if (!element) {
                c->failed = true;
                th_err(c->tc, TH_ERR_CONSTRAINT, "element_address", nullptr, t,
                       "only array elements have addresses; this container cannot be indexed by reference");
                return result;
            }
            return element;
        }
        parse_tree_t *assignment = assignment_target(c->v);
        if (assignment) {
            th_type *call_shape = index_store_call_shape(c, result,
                                                         assignment);
            if (call_shape && other_name.data)
                return infer_method_obligation(c, t, other_name.data, result,
                                               call_shape);
            c->failed = true;
            return result;
        }
        th_type *call_shape = index_call_shape(c, result);
        if (call_shape && explicit_name.data)
            return infer_method_obligation(c, t, explicit_name.data, result,
                                           call_shape);
        if (call_shape) {
            c->failed = true;
            return result;
        }

        th_type *e = th_seq_elem(t);

        if (e) {
            return e;
        }

        th_builtin_kind_t builtin = th_builtin_kind(c->tc, t);

        if (builtin == TH_BUILTIN_DICT) {
            e = named_arg_last(t);
        }
        else if (builtin == TH_BUILTIN_STRING) {
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
        parse_tree_t *assignment = assignment_target(c->v);
        if (assignment) {
            th_type *call_shape = slice_store_call_shape(c, result,
                                                         assignment);
            if (call_shape && other_name.data)
                return infer_method_obligation(c, t, other_name.data,
                                               result, call_shape);
            c->failed = true;
            return result;
        }
        th_type *call_shape = slice_call_shape(c, result);
        if (!call_shape || !explicit_name.data) {
            c->failed = true;
            return result;
        }
        return infer_method_obligation(c, t, explicit_name.data, result,
                                       call_shape);
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

        th_type *option_item = th_option_item(t);

        if (option_item || named_is(t, "result")) {
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

        th_type *call_result = option_item
            && (text_is(name, "get") || text_is(name, "get_or_else"))
                ? option_item : fresh(c);
        th_type *call = method_call_shape(c, call_result);
        th_type *signature = th_method_lookup(c->tc, t, method_name);
        if (signature && th_kind_of(th_resolve(signature)) == TH_FUNCTION) {
            th_type *instance = th_instantiate(
                c->tc, th_generalize(c->tc, signature));
            bool matched = call
                ? unify_method_call_shape(c, instance, t, call_result, call)
                : true;
            core_free(method_name);
            if (!matched) {
                c->failed = true;
                return call_result;
            }
            return call ? call : th_func_ret(instance);
        }

        signature = th_record_lookup_in(c->tc, t, method_name);
        if (signature && th_kind_of(th_resolve(signature)) == TH_FUNCTION) {
            th_type *instance = th_instantiate(
                c->tc, th_generalize(c->tc, signature));
            bool matched = !call || th_unify_in_place(c->tc, instance, call);
            core_free(method_name);
            if (!matched) {
                c->failed = true;
                return call_result;
            }
            th_type *ret = th_func_ret(instance);
            return call ? call : ret;
        }

        if (!signature) {
            size_t requirement_count = th_env_method_requirements(
                c->tc, method_name, nullptr, 0);
            if (requirement_count == 1) {
                th_env_method_requirement_t requirement = {0};
                th_env_method_requirements(c->tc, method_name, &requirement,
                                           1);
                th_type *resolved = method_requirement_return(
                    c, t, &requirement, call_result, call);
                if (resolved) {
                    core_free(method_name);
                    return call ? call : resolved;
                }
            } else if (requirement_count > 1) {
                if (th_kind_of(th_resolve(t)) == TH_VAR) {
                    if (th_var_constrain(
                            c->tc, th_resolve(t),
                            (th_constraint){
                                .kind = TH_CON_METHOD,
                                .method = {.name = method_name,
                                           .result = call_result,
                                           .call = call}})) {
                        core_free(method_name);
                        return call ? call : call_result;
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
                                c, t, &requirements[i], call_result, call);
                            core_free(requirements);
                            core_free(method_name);
                            return resolved
                                ? (call ? call : resolved) : fresh(c);
                        }
                        c->failed = true;
                    } else {
                        c->failed = true;
                    }
                    core_free(requirements);
                }
            } else {
                th_type *resolved = infer_method_obligation(
                    c, t, method_name, call_result, call);
                core_free(method_name);
                return call ? call : resolved;
            }
            core_free(method_name);
            return call ? call : call_result;
        }

        th_type *ret = signature ? th_func_ret(signature) : nullptr;

        core_free(method_name);

        return ret ? ret : fresh(c);
    }

    case PARSE_INFER_FIELD_RETURN:
    case PARSE_INFER_FIELD_RETURN_AUTO_DEREF:
        if (method_call_parent(c))
            return project(c, PARSE_INFER_METHOD_RETURN, t,
                explicit_name.data ? explicit_name : last_text(c->v->node),
                (core_string_t){0});
        if (op == PARSE_INFER_FIELD_RETURN_AUTO_DEREF)
            t = th_auto_deref(c->tc, t);
        return field_projection(c, t,
            explicit_name.data ? explicit_name : last_text(c->v->node),
            op == PARSE_INFER_FIELD_RETURN_AUTO_DEREF);
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

        th_type *t = c->type_node_resolver
            ? c->type_node_resolver(c->tc, c->v->grammar, k,
                                    c->type_node_resolver_context)
            : nullptr;
        if (!t) t = parse_tree_type(k);

        if (!t) {
            c->failed = true;
            return;
        }
        core_list_push(*out,
                       ((th_arg){.kind = TH_ARG_TYPE, .type = t}));
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

    case PARSE_INFER_CHILDREN: {
        th_type *joined = nullptr;

        if (!unify_children(c, c->v->node, &joined) || !joined) {
            return fresh(c);
        }

        return joined;
    }

    case PARSE_INFER_SELF:
        if (!parse_tree_type(c->v->node)) {
            parse_tree_set_type(c->v->node, fresh(c));
        }

        return parse_tree_type(c->v->node);

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

        th_type *r = named_application(c,
            e->name.data ? e->name.data : "", args.data, args.len);

        core_list_free(args);

        return r;
    }

    case PARSE_INFER_CHILD_APP: {
        parse_tree_t *k  = child(c, e->index);
        core_string_t nm = node_identifier_name(k);

        if (!nm.u8_bytes) {
            c->failed = true;

            return nullptr;
        }

        core_list_t(th_arg) args = core_list_new(th_arg);

        app_args(c, e, &args);

        th_type *r = named_application(c, nm.data, args.data, args.len);

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
        return project(c, e->proj, input, e->name, e->other_name);
    }

    case PARSE_INFER_COMPARE: {
        th_type *left = eval(c, e->lhs);
        th_type *right = eval(c, e->rhs);
        if (!left || !right) {
            c->failed = true;
            return th_name(c->tc, "bool");
        }
        if (builtin_comparable(left)) {
            if (!th_custom_coerces(c->tc, right, left)
                && !th_unify_in_place(c->tc, left, right)) c->failed = true;
            return th_name(c->tc, "bool");
        }
        th_type *comparison = nullptr;
        if (e->other_name.data) {
            parse_scope_t *scope = parse_tree_scope(c->v->node)
                ? parse_tree_scope(c->v->node)
                : c->st
                    ? parse_symtab_current_scope(c->st, core_string_empty())
                    : nullptr;
            parse_sym_entry_t *type = c->st
                ? parse_symtab_lookup_scoped(c->st, scope,
                    CORE_STRING_STATIC("type"), e->other_name)
                : nullptr;
            comparison = type && type->type
                ? type->type : th_name(c->tc, e->other_name.data);
        }
        else comparison = th_bitint(c->tc, 2, true);
        th_param parameter = {.mode = TH_IN, .type = right};
        th_type *call_shape = th_func(c->tc, comparison, &parameter, 1,
                                      nullptr, nullptr, 0);
        infer_method_obligation(c, left,
            e->name.data ? e->name.data : "cmp", comparison, call_shape);
        return th_name(c->tc, "bool");
    }

    case PARSE_INFER_METHOD_RESULT: {
        th_type *receiver = eval(c, e->lhs);
        th_type *argument = eval(c, e->rhs);
        th_type *result = fresh(c);
        if (!receiver || !argument || !e->name.data) {
            c->failed = true;
            return result;
        }
        th_param parameter = {.mode = TH_IN, .type = argument};
        th_type *call = th_func(c->tc, result, &parameter, 1,
                                nullptr, nullptr, 0);
        if (!call) {
            c->failed = true;
            return result;
        }
        if (!th_require_method_at(c->tc, receiver, e->name.data,
                                  e->other_name.data, result, call,
                                  c->v->node))
            c->failed = true;
        return result;
    }

    case PARSE_INFER_UNIFY: {
        th_type *l = eval(c, e->lhs);
        th_type *r = eval(c, e->rhs);

        if (!l || !r) {
            c->failed = true;

            return l;
        }

        if (th_custom_coerces(c->tc, l, r)) return r;
        if (th_custom_coerces(c->tc, r, l)) return l;
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
parse_infer_eval_with_type_node_resolver(
    const parse_infer_expr_t *e, parse_visit_t *v, th_ctx *tc,
    parse_symtab_t *st, parse_infer_type_node_resolver_fn resolver,
    void *resolver_context, bool *ok)
{
    if (ok) {
        *ok = false;
    }

    if (!e || !v || !tc) {
        return nullptr;
    }

    th_span_t outer_span = th_ctx_span(tc);
    parse_tree_t *first_leaf = parse_tree_scan_leaves(v->node, false, nullptr,
                                                       nullptr);
    parse_tree_t *last_leaf  = parse_tree_scan_leaves(v->node, true, nullptr,
                                                       nullptr);
    parse_token_info_t *first = first_leaf ? core_tree_leaf_value(first_leaf)
                                           : nullptr;
    parse_token_info_t *last  = last_leaf ? core_tree_leaf_value(last_leaf)
                                          : first;

    if (first) {
        core_option_t(core_string_t) first_file = parse_token_file(first);
        const char *file = core_option_is_set(first_file)
                               ? core_option_get(first_file).data
                               : nullptr;
        uint32_t first_line   = parse_token_line(first);
        uint32_t first_column = parse_token_column(first);
        uint32_t last_line    = parse_token_line(last);
        uint32_t last_column  = parse_token_column(last);
        uint32_t last_endline = parse_token_endline(last);
        uint32_t last_endcol  = parse_token_endcol(last);

        th_ctx_set_span(tc,
                        (th_span_t){.file = file,
                                    .start_line = first_line,
                                    .start_col = first_column,
                                    .end_line = last_endline ? last_endline
                                                             : last_line,
                                    .end_col = last_endcol ? last_endcol
                                                           : last_column + 1});
    }

    ictx c = {
        .tc = tc,
        .st = st,
        .v = v,
        .type_node_resolver = resolver,
        .type_node_resolver_context = resolver_context,
        .tvars = core_list_new(tvar_t),
    };

    th_type *t = eval(&c, e);
    th_ctx_set_span(tc, outer_span);

    if (ok) {
        *ok = !c.failed && t != nullptr;
    }

    core_list_free(c.tvars);

    return t;
}

th_type *
parse_infer_eval(const parse_infer_expr_t *e, parse_visit_t *v, th_ctx *tc,
                 parse_symtab_t *st, bool *ok)
{
    return parse_infer_eval_with_type_node_resolver(e, v, tc, st,
        nullptr, nullptr, ok);
}
