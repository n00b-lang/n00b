#include <stdlib.h>
#include <string.h>

#include "private/th/internal.h"
#include "lib/dict.h"
#include "lib/list.h"
#include "lib/option.h"

typedef const char *iface_name_ptr;
core_list_decl(iface_name_ptr);

static bool nullary_name(const th_type *type, const char **name);

static void defer_environment_constraints(th_ctx *ctx, th_type *variable)
{
    if (!ctx || !variable) return;
    for (size_t i = 0; i < ctx->pending_environment_constraints.len; i++)
        if (ctx->pending_environment_constraints.data[i] == variable) return;
    core_list_push(ctx->pending_environment_constraints, variable);
}

typedef struct {
    const char *module;
    const char *package;
} constraint_scope_t;

static constraint_scope_t enter_constraint_scope(
    th_ctx *ctx, const th_constraint *constraint)
{
    constraint_scope_t previous = {ctx->module, ctx->package};
    if (constraint->has_scope) {
        ctx->module = constraint->scope_module;
        ctx->package = constraint->scope_package;
    }
    return previous;
}

static void leave_constraint_scope(th_ctx *ctx, constraint_scope_t previous)
{
    ctx->module = previous.module;
    ctx->package = previous.package;
}

static bool same_text(const char *a, const char *b)
{
    return !strcmp(a ? a : "", b ? b : "");
}

static bool environment_constraint_active(th_ctx *ctx, th_con_kind kind,
                                          const char *name,
                                          th_type *receiver)
{
    uint64_t receiver_hash = th_hash(ctx, receiver);
    for (size_t i = 0; i < ctx->active_environment_constraints.len; i++) {
        const th_active_constraint_t *active =
            &ctx->active_environment_constraints.data[i];
        if (active->kind == kind && active->receiver_hash == receiver_hash
            && same_text(active->name, name)
            && same_text(active->module, ctx->module)
            && same_text(active->package, ctx->package)) return true;
    }
    return false;
}

static void push_environment_constraint(th_ctx *ctx, th_con_kind kind,
                                        const char *name, th_type *receiver)
{
    core_list_push(ctx->active_environment_constraints,
        ((th_active_constraint_t){
            .kind = kind,
            .name = name,
            .module = ctx->module,
            .package = ctx->package,
            .receiver_hash = th_hash(ctx, receiver),
        }));
}

static bool iface_name_seen(core_list_t(iface_name_ptr) *names,
                            const char *name)
{
    for (size_t i = 0; i < names->len; i++) {
        if (!strcmp(names->data[i], name)) {
            return true;
        }
    }
    core_list_push(*names, name);
    return false;
}

static bool same_constraint_scope(const th_constraint *a,
                                  const th_constraint *b)
{
    if (a->has_scope != b->has_scope) return false;
    return !a->has_scope
        || (!strcmp(a->scope_module ? a->scope_module : "",
                    b->scope_module ? b->scope_module : "")
            && !strcmp(a->scope_package ? a->scope_package : "",
                       b->scope_package ? b->scope_package : ""));
}

static th_constraint *con_same_slot(th_type *var, th_constraint c)
{
    for (size_t i = 0; i < var->var.cons.len; i++) {
        th_constraint *e = &var->var.cons.data[i];

        if (e->kind != c.kind) {
            continue;
        }

        if (c.kind == TH_CON_HAS_FIELD
            && e->field.auto_deref == c.field.auto_deref
            && !strcmp(e->field.name, c.field.name)) {
            return e;
        }

        if (c.kind == TH_CON_HAS_PARAM && e->param.index == c.param.index) {
            return e;
        }

        if (c.kind == TH_CON_IMPLEMENTS && e->iface && c.iface &&
            !strcmp(e->iface, c.iface) && same_constraint_scope(e, &c)) {
            return e;
        }

        if (c.kind == TH_CON_METHOD && e->method.name && c.method.name &&
            !strcmp(e->method.name, c.method.name)
            && same_constraint_scope(e, &c)) {
            return e;
        }

    }

    return nullptr;
}

static bool same_constraint_type(th_ctx *ctx, th_type *a, th_type *b)
{
    a = th_resolve(a);
    b = th_resolve(b);
    if (a == b) return true;
    if (!ctx || !a || !b || !th_is_closed(ctx, a)
        || !th_is_closed(ctx, b)) return false;
    uint64_t left = th_hash(ctx, a);
    return left && left == th_hash(ctx, b);
}

static bool same_repeated_constraint(th_ctx *ctx, th_type *var,
                                     const th_constraint *candidate)
{
    for (size_t i = 0; i < var->var.cons.len; i++) {
        const th_constraint *have = &var->var.cons.data[i];
        if (have->kind != candidate->kind
            || !same_constraint_scope(have, candidate)) continue;
        switch (candidate->kind) {
        case TH_CON_UNIFIES:
        case TH_CON_PROMOTES:
        case TH_CON_EXCLUDES:
            if (same_constraint_type(ctx, have->target, candidate->target))
                return true;
            break;
        case TH_CON_KIND:
            if (have->type_kind == candidate->type_kind) return true;
            break;
        case TH_CON_ONE_OF:
            if (have->one_of.n != candidate->one_of.n) break;
            for (size_t j = 0; j < have->one_of.n; j++)
                if (!same_constraint_type(ctx, have->one_of.types[j],
                                          candidate->one_of.types[j]))
                    goto next_constraint;
            return true;
        default:
            break;
        }
next_constraint:;
    }
    return false;
}

static bool same_method_type_ref(th_ctx *ctx, th_type *a, th_type *a_owner,
                                 th_type *b, th_type *b_owner)
{
    th_type *resolved_a = th_resolve(a);
    th_type *resolved_b = th_resolve(b);
    return resolved_a == resolved_b
        || (resolved_a == th_resolve(a_owner)
            && resolved_b == th_resolve(b_owner))
        || (th_is_closed(ctx, resolved_a) && th_is_closed(ctx, resolved_b)
            && th_hash(ctx, resolved_a) == th_hash(ctx, resolved_b));
}

static bool same_method_output(th_ctx *ctx, th_type *a, th_type *a_owner,
                               th_type *b, th_type *b_owner)
{
    th_type *left = th_resolve(a);
    th_type *right = th_resolve(b);
    return same_method_type_ref(ctx, a, a_owner, b, b_owner)
        || (left && right
            && (left->kind == TH_VAR || right->kind == TH_VAR));
}

static bool same_method_obligation(th_ctx *ctx, const th_constraint *a,
                                   th_type *a_owner,
                                   const th_constraint *b, th_type *b_owner)
{
    if (!same_method_output(ctx, a->method.result, a_owner,
                              b->method.result, b_owner)) return false;
    th_type *a_call = th_resolve(a->method.call);
    th_type *b_call = th_resolve(b->method.call);
    if (!a_call || !b_call) return a_call == b_call;
    if (a_call->kind != TH_FUNCTION || b_call->kind != TH_FUNCTION
        || a_call->fn.np != b_call->fn.np
        || a_call->fn.nkw != b_call->fn.nkw
        || !!a_call->fn.rest != !!b_call->fn.rest
        || !same_method_output(ctx, a_call->fn.ret, a_owner,
                                 b_call->fn.ret, b_owner)) return false;
    if (a_call->fn.error && b_call->fn.error
        && !same_method_output(ctx, a_call->fn.error, a_owner,
                                 b_call->fn.error, b_owner)) return false;
    for (size_t i = 0; i < a_call->fn.np; i++)
        if (a_call->fn.params[i].mode != b_call->fn.params[i].mode
            || !same_method_type_ref(ctx, a_call->fn.params[i].type, a_owner,
                                     b_call->fn.params[i].type, b_owner))
            return false;
    if (a_call->fn.rest
        && (a_call->fn.rest->mode != b_call->fn.rest->mode
            || !same_method_type_ref(ctx, a_call->fn.rest->type, a_owner,
                                     b_call->fn.rest->type, b_owner)))
        return false;
    for (size_t i = 0; i < a_call->fn.nkw; i++)
        if (strcmp(a_call->fn.kw[i].name, b_call->fn.kw[i].name)
            || a_call->fn.kw[i].mode != b_call->fn.kw[i].mode
            || !same_method_type_ref(ctx, a_call->fn.kw[i].type, a_owner,
                                     b_call->fn.kw[i].type, b_owner))
            return false;
    return true;
}

static bool merge_method_types(th_ctx *ctx, th_type *a, th_type *b,
                               core_dict_t *seen);

static bool set_method_failure(th_ctx *ctx, th_constraint *constraint,
                               th_type *error, core_dict_t *seen)
{
    th_type *call = th_resolve(constraint->method.call);
    if (!call) {
        constraint->method.call = th_func_with_error(ctx,
            constraint->method.result, error, nullptr, 0,
            nullptr, nullptr, 0);
        return constraint->method.call != nullptr;
    }
    if (th_kind_of(call) != TH_FUNCTION) return false;
    th_type *existing = th_func_error(call);
    if (existing)
        return merge_method_types(ctx, existing, error, seen);
    constraint->method.call = th_func_with_error(ctx, th_func_ret(call),
        error, call->fn.params, call->fn.np, call->fn.rest,
        call->fn.kw, call->fn.nkw);
    return constraint->method.call != nullptr;
}

static bool complete_method_failure(th_ctx *ctx, th_type *owner,
                                    th_constraint *slot,
                                    const th_constraint *incoming,
                                    core_dict_t *seen)
{
    th_type *have = th_resolve(slot->method.call);
    th_type *want = th_resolve(incoming->method.call);
    if (!have || !want || have->fn.error || !want->fn.error) return true;
    bool completed = set_method_failure(ctx, slot, want->fn.error, seen);
    if (completed) owner->revision++;
    return completed;
}

static bool uni(th_ctx *ctx, th_type *a, th_type *b, core_dict_t *seen);
static void uni_seen_free(core_dict_t *seen);

static bool merge_method_types(th_ctx *ctx, th_type *a, th_type *b,
                               core_dict_t *seen)
{
    size_t before = th_error_count(ctx);
    if (!uni(ctx, a, b, seen)) {
        if (th_error_count(ctx) == before)
            th_err(ctx, TH_ERR_UNIFY, nullptr, b, a, "types do not unify");
        return false;
    }
    th_normalize(th_apply_subst(ctx, a));
    return true;
}

static bool merge_method_obligation(th_ctx *ctx, th_type *owner,
                                    th_constraint *slot,
                                    const th_constraint *incoming,
                                    core_dict_t *seen)
{
    th_type *have_call = th_resolve(slot->method.call);
    th_type *want_call = th_resolve(incoming->method.call);
    return merge_method_types(ctx, incoming->method.result,
               slot->method.result, seen)
        && (!have_call || !want_call
            || (merge_method_types(ctx, want_call->fn.ret,
                    have_call->fn.ret, seen)
                && (!have_call->fn.error || !want_call->fn.error
                    || merge_method_types(ctx, want_call->fn.error,
                        have_call->fn.error, seen))))
        && complete_method_failure(ctx, owner, slot, incoming, seen);
}

static bool th_var_constrain_seen(th_ctx *ctx, th_type *var,
                                  th_constraint c, core_dict_t *seen)
{
    if (!var || var->kind != TH_VAR) {
        return false;
    }
    if (c.kind == TH_CON_KIND && c.type_kind > TH_TRAIT) return false;
    if (same_repeated_constraint(ctx, var, &c)) return true;

    if (c.kind == TH_CON_METHOD) {
        if (!ctx || !c.method.name || !*c.method.name || !c.method.result ||
            (c.method.call &&
             th_kind_of(th_resolve(c.method.call)) != TH_FUNCTION))
            return false;
        if (!c.has_scope && (ctx->module || ctx->package)) {
            const char *module = ctx->module ? ctx->module : "";
            const char *package = ctx->package ? ctx->package : "";
            c.scope_module = th_strdup(ctx, module, strlen(module));
            c.scope_package = th_strdup(ctx, package, strlen(package));
            c.has_scope = true;
        }
        c.method.name = th_strdup(ctx, c.method.name, strlen(c.method.name));
        for (size_t i = 0; i < var->var.cons.len; i++) {
            th_constraint *slot = &var->var.cons.data[i];
            if (slot->kind == TH_CON_METHOD
                && !strcmp(slot->method.name, c.method.name)
                && same_constraint_scope(slot, &c)
                && same_method_obligation(ctx, slot, var, &c, var)) {
                return merge_method_obligation(ctx, var, slot, &c, seen);
            }
        }
        core_list_push(var->var.cons, c);
        var->revision++;
        return true;
    }

    th_constraint *slot = con_same_slot(var, c);

    if (slot) {
        if (c.kind == TH_CON_IMPLEMENTS) return true;

        th_type *have = c.kind == TH_CON_HAS_FIELD ? slot->field.type
                                                   : slot->param.type;
        th_type *want = c.kind == TH_CON_HAS_FIELD ? c.field.type
                                                   : c.param.type;

        if (have && want && !th_unify(ctx, have, want)) {
            th_err(ctx, TH_ERR_CONSTRAINT,
                   c.kind == TH_CON_HAS_FIELD ? "has_field" : "has_param",
                   have, want,
                   "constraint conflicts with one already on the variable");

            return false;
        }

        return true;
    }

    if (c.kind == TH_CON_HAS_FIELD) {
        if (!ctx || !c.field.name || !*c.field.name) return false;
        c.field.name = th_strdup(ctx, c.field.name, strlen(c.field.name));
    }

    if (ctx && c.kind == TH_CON_IMPLEMENTS && !c.has_scope &&
        (ctx->module || ctx->package)) {
        const char *module = ctx->module ? ctx->module : "";
        const char *package = ctx->package ? ctx->package : "";
        c.scope_module = th_strdup(ctx, module, strlen(module));
        c.scope_package = th_strdup(ctx, package, strlen(package));
        c.has_scope = true;
    }

    if (c.kind == TH_CON_IMPLEMENTS && c.iface) {
        if (!ctx) return false;
        c.iface = th_strdup(ctx, c.iface, strlen(c.iface));
    }

    core_list_push(var->var.cons, c);
    var->revision++;

    return true;
}

bool th_var_constrain(th_ctx *ctx, th_type *var, th_constraint c)
{
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    bool result = th_var_constrain_seen(ctx, var, c, &seen);
    uni_seen_free(&seen);
    return result;
}

bool th_require_method(th_ctx *ctx, th_type *receiver, const char *name,
                       th_type *result, th_type *call)
{
    if (!ctx || !receiver || !name || !*name || !result ||
        (call && th_kind_of(th_resolve(call)) != TH_FUNCTION))
        return false;
    th_type *resolved = th_resolve(receiver);
    bool is_variable = th_kind_of(resolved) == TH_VAR;
    th_type *obligation = is_variable ? resolved : th_var(ctx);
    if (!th_var_constrain(ctx, obligation,
                          (th_constraint){.kind = TH_CON_METHOD,
                                          .method = {.name = name,
                                                     .result = result,
                                                     .call = call}}))
        return false;
    return is_variable ||
           th_unify_in_place(ctx, obligation, receiver) != nullptr;
}

bool th_method_obligation_set_error(th_ctx *ctx, th_type *receiver,
                                    const char *name, th_type *result,
                                    th_type *error)
{
    if (!ctx || !receiver || !name || !*name || !result || !error)
        return false;
    th_type *variable = th_resolve(receiver);
    if (th_kind_of(variable) != TH_VAR) return false;
    th_constraint *match = nullptr;
    size_t named_count = 0;
    for (size_t i = 0; i < variable->var.cons.len; i++) {
        th_constraint *constraint = &variable->var.cons.data[i];
        if (constraint->kind != TH_CON_METHOD
            || strcmp(constraint->method.name, name)) continue;
        named_count++;
        if (th_resolve(constraint->method.result) == th_resolve(result)) {
            match = constraint;
            break;
        }
    }
    if (!match && named_count == 1)
        for (size_t i = 0; i < variable->var.cons.len; i++) {
            th_constraint *constraint = &variable->var.cons.data[i];
            if (constraint->kind == TH_CON_METHOD
                && !strcmp(constraint->method.name, name)) {
                match = constraint;
                break;
            }
        }
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    bool updated = match && set_method_failure(ctx, match, error, &seen);
    uni_seen_free(&seen);
    if (updated) variable->revision++;
    return updated;
}

size_t th_var_constraint_count(const th_type *var)
{
    return var && var->kind == TH_VAR ? var->var.cons.len : 0;
}

bool th_var_constraint_at(const th_type *var, size_t i, th_constraint *out)
{
    if (!var || var->kind != TH_VAR || i >= var->var.cons.len) {
        return false;
    }

    if (out) {
        *out = var->var.cons.data[i];
    }

    return true;
}

void th_register_promotion(th_ctx *ctx, const char *from, const char *to)
{
    if (!ctx || !from || !to) {
        return;
    }

    th_promo p = {.from = th_strdup(ctx, from, strlen(from)),
                  .to   = th_strdup(ctx, to, strlen(to))};

    core_list_push(ctx->promos, p);

    if (!ctx->promo_index) {
        ctx->promo_index = core_alloc(core_dict_t);
        core_dict_init(ctx->promo_index, core_hash_cstring,
                       core_dict_cstr_eq);
    }

    bool  found = false;
    void *have  = _core_dict_get(ctx->promo_index, (void *)p.from, &found);

    core_list_t(core_const_charptr_t) *edges = have;

    if (!found) {
        edges  = core_alloc(core_list_t(core_const_charptr_t));
        *edges = core_list_new(core_const_charptr_t);

        _core_dict_put(ctx->promo_index, (void *)p.from, edges);
    }

    core_list_push(*edges, p.to);
}

bool th_promotes_to(th_ctx *ctx, const char *from, const char *to)
{
    if (!ctx || !from || !to) {
        return false;
    }

    if (!strcmp(from, to)) {
        return true;
    }

    core_dict_t seen;
    core_list_t(core_const_charptr_t) queue
        = core_list_new(core_const_charptr_t);

    core_dict_init(&seen, core_hash_cstring, core_dict_cstr_eq);
    core_dict_put(&seen, from, from);
    core_list_push(queue, from);

    bool hit = false;

    for (size_t at = 0; !hit && at < queue.len; at++) {
        const char *cur   = queue.data[at];
        bool        found = false;

        if (!ctx->promo_index) {
            break;
        }

        core_list_t(core_const_charptr_t) *edges
            = _core_dict_get(ctx->promo_index, (void *)cur, &found);

        for (size_t i = 0; found && i < edges->len; i++) {
            const char *next = edges->data[i];

            if (!strcmp(next, to)) {
                hit = true;
                break;
            }

            if (!core_dict_contains(&seen, (void *)next)) {
                core_dict_put(&seen, next, next);
                core_list_push(queue, next);
            }
        }
    }

    core_dict_free(&seen);
    core_list_free(queue);

    return hit;
}

bool th_type_promotes_to(th_ctx *ctx, const th_type *from, const th_type *to)
{
    from = th_resolve_c(from);
    to = th_resolve_c(to);
    if (!ctx || !from || !to)
        return false;
    if (from == to)
        return true;
    if (from->kind == TH_BITINT && to->kind == TH_BITINT) {
        if (from->bitint.width == to->bitint.width &&
            from->bitint.flavor == to->bitint.flavor)
            return true;
        if (from->bitint.flavor == TH_BITINT_BITS
            || to->bitint.flavor == TH_BITINT_BITS)
            return false;
        if (from->bitint.width >= to->bitint.width)
            return false;
        if (from->bitint.flavor == TH_BITINT_SIGNED)
            return to->bitint.flavor == TH_BITINT_SIGNED;
        return to->bitint.flavor == TH_BITINT_UNSIGNED ||
               from->bitint.width < to->bitint.width;
    }
    const char *from_name = nullptr, *to_name = nullptr;
    return nullary_name(from, &from_name) &&
           nullary_name(to, &to_name) &&
           th_promotes_to(ctx, from_name, to_name);
}

static bool uni_in_place(th_ctx *ctx, th_type *a, th_type *b);

void th_register_iface(th_ctx *ctx, const char *name, th_type *methods)
{
    if (!ctx || !name || !methods) {
        return;
    }

    th_iface i = {.name    = th_strdup(ctx, name, strlen(name)),
                  .methods = methods};

    core_list_push(ctx->ifaces, i);

    if (!ctx->iface_index) {
        ctx->iface_index = core_alloc(core_dict_t);
        core_dict_init(ctx->iface_index, core_hash_cstring,
                       core_dict_cstr_eq);
    }

    _core_dict_put(ctx->iface_index, (void *)i.name,
                   (void *)(uintptr_t)ctx->ifaces.len);
}

bool th_register_method(th_ctx *ctx, th_type *receiver, const char *name,
                        th_type *signature)
{
    if (!ctx || !receiver || !name || !*name || !signature) {
        return false;
    }

    receiver = (th_type *)(uintptr_t)th_resolve_c(receiver);

    for (size_t i = 0; i < ctx->methods.len; i++) {
        th_method *method = &ctx->methods.data[i];

        if (method->receiver == receiver && !strcmp(method->name, name)) {
            method->signature = signature;

            return true;
        }
    }

    core_list_push(ctx->methods,
                   ((th_method){.receiver = receiver,
                                .name = th_strdup(ctx, name, strlen(name)),
                                .signature = signature}));

    return true;
}

bool th_register_method_resolver(th_ctx *ctx,
                                 th_method_resolver_fn resolver,
                                 void *user)
{
    if (!ctx || !resolver) return false;
    for (size_t i = 0; i < ctx->method_resolvers.len; i++) {
        th_method_resolver *entry = &ctx->method_resolvers.data[i];
        if (entry->resolver == resolver && entry->user == user) return true;
    }
    core_list_push(ctx->method_resolvers,
                   ((th_method_resolver){resolver, user}));
    return true;
}

th_type *th_method_lookup(th_ctx *ctx, th_type *receiver, const char *name)
{
    if (!ctx || !receiver || !name || !*name) {
        return nullptr;
    }

    receiver = (th_type *)(uintptr_t)th_resolve_c(receiver);

    for (size_t i = 0; i < ctx->methods.len; i++) {
        th_method *method = &ctx->methods.data[i];

        if (method->receiver == receiver && !strcmp(method->name, name)) {
            return method->signature;
        }
    }

    uint64_t receiver_hash = th_hash(ctx, receiver);

    for (size_t i = 0; i < ctx->methods.len; i++) {
        th_method *method = &ctx->methods.data[i];

        if (!strcmp(method->name, name)
            && th_hash(ctx, method->receiver) == receiver_hash) {
            return method->signature;
        }
    }

    th_type *derived = nullptr;
    for (size_t i = 0; i < ctx->method_resolvers.len; i++) {
        th_method_resolver *entry = &ctx->method_resolvers.data[i];
        th_type *candidate = entry->resolver(ctx, receiver, name,
                                             entry->user);
        if (!candidate) continue;
        if (derived && !th_unify_probe(ctx, derived, candidate))
            return nullptr;
        if (!derived) derived = candidate;
    }
    if (derived) return derived;

    return nullptr;
}

static const th_iface *iface_named(th_ctx *ctx, const char *name)
{
    if (!ctx->iface_index) {
        return nullptr;
    }

    bool  found = false;
    void *slot  = _core_dict_get(ctx->iface_index, (void *)name, &found);

    return found ? &ctx->ifaces.data[(uintptr_t)slot - 1] : nullptr;
}

static bool satisfies(th_ctx *ctx, th_type *type, th_type *methods)
{
    size_t n = th_record_len(methods);

    for (size_t i = 0; i < n; i++) {
        const th_member *m = th_record_member(methods, i);
        th_type         *have;

        if (!m) {
            return false;
        }

        have = th_record_lookup_in(ctx, type, m->name);

        if (!have || !th_unify(ctx, have, m->type)) {
            return false;
        }
    }

    return true;
}

bool th_implements(th_ctx *ctx, th_type *type, const char *iface)
{
    if (!ctx || !type || !iface) {
        return false;
    }

    th_type *resolved = th_resolve(type);
    if (resolved && resolved->kind == TH_VAR) {
        for (size_t c = 0; c < resolved->var.cons.len; c++) {
            const th_constraint *constraint = &resolved->var.cons.data[c];
            if (constraint->kind == TH_CON_IMPLEMENTS && constraint->iface &&
                !strcmp(constraint->iface, iface)) {
                return true;
            }
        }
    }

    const th_iface *i = iface_named(ctx, iface);

    return (i && satisfies(ctx, type, i->methods))
        || th_env_implements(ctx, type, iface);
}

size_t th_infer_ifaces(th_ctx *ctx, th_type *type, const char **out,
                       size_t max)
{
    if (!ctx || !type) {
        return 0;
    }

    size_t n = 0;
    core_list_t(iface_name_ptr) emitted = core_list_new(iface_name_ptr);
    th_type *resolved = th_resolve(type);
    if (resolved && resolved->kind == TH_VAR) {
        for (size_t c = 0; c < resolved->var.cons.len; c++) {
            const th_constraint *constraint = &resolved->var.cons.data[c];
            if (constraint->kind != TH_CON_IMPLEMENTS || !constraint->iface ||
                iface_name_seen(&emitted, constraint->iface)) {
                continue;
            }
            if (out && n < max) {
                out[n] = constraint->iface;
            }
            n++;
        }
    }

    for (size_t i = 0; i < ctx->ifaces.len; i++) {
        const th_iface *f = &ctx->ifaces.data[i];

        if (!satisfies(ctx, type, f->methods)) {
            continue;
        }
        if (iface_name_seen(&emitted, f->name)) {
            continue;
        }

        if (out && n < max) {
            out[n] = f->name;
        }
        n++;
    }

    if (ctx->environment) {
        for (size_t i = 0; i < ctx->environment->traits.len; i++) {
            const th_env_trait_t *trait = &ctx->environment->traits.data[i];
            bool already_seen = false;
            for (size_t j = 0; j < ctx->ifaces.len; j++)
                if (!strcmp(ctx->ifaces.data[j].name, trait->name)) {
                    already_seen = true;
                    break;
                }
            for (size_t j = 0; !already_seen && j < i; j++)
                if (!strcmp(ctx->environment->traits.data[j].name,
                            trait->name)) {
                    already_seen = true;
                    break;
                }
            if (already_seen || !th_env_implements(ctx, type, trait->name))
                continue;
            if (iface_name_seen(&emitted, trait->name))
                continue;
            if (out && n < max) out[n] = trait->name;
            n++;
        }
    }

    core_list_free(emitted);
    return n;
}

static bool nullary_name(const th_type *t, const char **name);

static void record_coercion(th_ctx *ctx, th_coerce_flag kind, th_type *from,
                            th_type *to)
{
    th_coercion_t c = {
        .kind = kind,
        .from = from,
        .to   = to,
        .span = ctx->span,
    };

    core_list_push(ctx->coercions, c);
}

void th_register_coercion(th_ctx *ctx, th_coerce_fn fn, void *user)
{
    if (ctx) {
        ctx->custom      = fn;
        ctx->custom_user = user;
    }
}

size_t th_coercion_count(const th_ctx *ctx)
{
    return ctx ? core_list_len(ctx->coercions) : 0;
}

bool th_coercion_at(const th_ctx *ctx, size_t i, th_coercion_t *out)
{
    if (!ctx || i >= core_list_len(ctx->coercions)) {
        return false;
    }

    if (out) {
        *out = ctx->coercions.data[i];
    }

    return true;
}

void th_coercions_clear(th_ctx *ctx)
{
    if (ctx) {
        ctx->coercions.len = 0;
    }
}

static th_type *option_of(th_type *t)
{
    t = th_resolve(t);

    if (!t || t->kind != TH_NAMED || t->named.nargs != 1
        || !t->named.name || strcmp(t->named.name, "option") != 0) {
        return nullptr;
    }

    if (t->named.args[0].kind != TH_ARG_TYPE) {
        return nullptr;
    }

    return t->named.args[0].type;
}

void th_ctx_set_method_argument_coercions(th_ctx *ctx, uint8_t allowed)
{
    if (ctx) ctx->method_argument_coercions = allowed;
}

static bool method_argument_unifies(th_ctx *ctx, th_type *actual,
                                    th_type *formal, th_mode mode,
                                    core_dict_t *seen)
{
    if (!actual || !formal) return false;
    if (!(ctx->method_argument_coercions
          & (TH_COERCE_OPTION_WRAP | TH_COERCE_NIL_OPTION
             | TH_COERCE_RECORD_CONSTRUCT)))
        return uni(ctx, actual, formal, seen);
    if (th_unify_probe(ctx, actual, formal))
        return uni(ctx, actual, formal, seen);
    th_type *resolved_actual = th_resolve(actual);
    th_type *resolved_formal = th_resolve(formal);
    if (mode == TH_IN
        && (ctx->method_argument_coercions & TH_COERCE_RECORD_CONSTRUCT)
        && th_kind_of(resolved_actual) == TH_RECORD
        && th_kind_of(resolved_formal) == TH_NAMED) {
        th_type *definition = th_type_definition(ctx, resolved_formal);
        if (definition && th_kind_of(th_resolve(definition)) == TH_RECORD
            && th_unify_probe(ctx, resolved_actual, definition))
            return uni(ctx, resolved_actual, definition, seen);
    }
    th_type *inner = mode == TH_IN ? option_of(formal) : nullptr;
    if (!inner) return false;
    actual = th_resolve(actual);
    if ((ctx->method_argument_coercions & TH_COERCE_NIL_OPTION)
        && actual->kind == TH_NAMED && actual->named.nargs == 0
        && actual->named.name && !strcmp(actual->named.name, "nil"))
        return true;
    return (ctx->method_argument_coercions & TH_COERCE_OPTION_WRAP)
        && th_unify_probe(ctx, actual, inner)
        && uni(ctx, actual, inner, seen);
}

th_type *th_coerce(th_ctx *ctx, th_type *from, th_coerce_policy policy)
{
    th_type *bottom = th_name(ctx, TH_BOTTOM);

    if (!ctx || !from || !policy.target) {
        return bottom;
    }

    if (th_unify(ctx, from, policy.target)) {
        return policy.target;
    }

    if (policy.allow & TH_COERCE_DEREF) {
        th_type *inner = th_ind_referent(from);

        if (inner) {
            th_coerce_policy inner_policy = policy;
            th_type         *r            = th_coerce(ctx, inner,
                                                      inner_policy);

            if (!th_is_bottom(r)) {
                record_coercion(ctx, TH_COERCE_DEREF, from, r);

                return r;
            }
        }
    }

    if (policy.allow & TH_COERCE_PROMOTE) {
        if (th_type_promotes_to(ctx, from, policy.target)) {
            record_coercion(ctx, TH_COERCE_PROMOTE, from, policy.target);

            return policy.target;
        }
    }

    if (policy.allow & TH_COERCE_OPTION_WRAP) {
        th_type *inner = option_of(policy.target);

        if (inner && th_unify(ctx, from, inner)) {
            record_coercion(ctx, TH_COERCE_OPTION_WRAP, from, policy.target);

            return policy.target;
        }
    }

    if ((policy.allow & TH_COERCE_CUSTOM) && ctx->custom
        && ctx->custom(ctx, from, policy.target, ctx->custom_user)) {
        record_coercion(ctx, TH_COERCE_CUSTOM, from, policy.target);

        return policy.target;
    }

    return bottom;
}

static bool nullary_name(const th_type *t, const char **name)
{
    t = th_resolve_c(t);

    if (!t || t->kind != TH_NAMED || t->named.nargs) {
        return false;
    }

    *name = t->named.name;

    return true;
}

static bool con_check(th_ctx *ctx, const th_type *var, th_type *bound_to,
                      core_dict_t *seen)
{
    th_type *bt = th_resolve(bound_to);

    if (bt && bt->kind == TH_VAR) {
        for (size_t i = 0; i < var->var.cons.len; i++) {
            th_constraint constraint = var->var.cons.data[i];
            bool matched = false;
            if (constraint.kind == TH_CON_METHOD) {
                for (size_t j = 0; j < bt->var.cons.len; j++) {
                    th_constraint *slot = &bt->var.cons.data[j];
                    if (slot->kind != TH_CON_METHOD
                        || strcmp(slot->method.name,
                                  constraint.method.name)
                        || !same_constraint_scope(slot, &constraint)
                        || !same_method_obligation(ctx, slot, bt,
                            &constraint, (th_type *)var)) continue;
                    if (!merge_method_obligation(ctx, bt, slot, &constraint,
                                                 seen))
                        return false;
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
            if (!th_var_constrain_seen(ctx, bt, constraint, seen)) {
                return false;
            }
        }

        return true;
    }

    for (size_t i = 0; i < var->var.cons.len; i++) {
        const th_constraint *c = &var->var.cons.data[i];

        switch (c->kind) {
        case TH_CON_KIND:
            if (th_kind_of(th_resolve(bound_to)) != c->type_kind) {
                th_err(ctx, TH_ERR_CONSTRAINT, "kind", nullptr, bound_to,
                       "constraint kind does not match the type family");
                return false;
            }
            break;
        case TH_CON_UNIFIES:
            if (!uni(ctx, bound_to, c->target, seen)) {
                th_err(ctx, TH_ERR_CONSTRAINT, "unifies", c->target,
                       bound_to, "constraint unifies failed");

                return false;
            }
            break;

        case TH_CON_ONE_OF: {
            bool matched = false;

            for (size_t j = 0; !matched && j < c->one_of.n; j++) {
                const th_type *cand = th_resolve_c(c->one_of.types[j]);
                const th_type *bt   = th_resolve_c(bound_to);
                const char    *a, *b;

                if (nullary_name(bt, &a) && nullary_name(cand, &b)) {
                    matched = !strcmp(a, b);
                }
                else {
                    matched = bt == cand;
                    if (!matched) {
                        size_t error_count = ctx->errors.len;
                        size_t pending_count =
                            ctx->pending_environment_constraints.len;
                        const char *previous_error = ctx->err;
                        matched = th_unify(ctx, (th_type *)bt,
                                            (th_type *)cand) != nullptr;
                        ctx->errors.len = error_count;
                        if (!matched)
                            ctx->pending_environment_constraints.len = pending_count;
                        ctx->err = previous_error;
                    }
                }
            }

            if (!matched) {
                th_err(ctx, TH_ERR_CONSTRAINT, "one_of", nullptr, bound_to,
                       "constraint one_of matched no alternative");

                return false;
            }
            break;
        }

        case TH_CON_PROMOTES: {
            if (!th_type_promotes_to(ctx, bound_to, c->target)) {
                th_err(ctx, TH_ERR_CONSTRAINT, "promotes", c->target,
                       bound_to,
                       "constraint promotes has no promotion path");

                return false;
            }
            break;
        }

        case TH_CON_EXCLUDES: {
            const th_type *ex = th_resolve_c(c->target);
            const th_type *bt = th_resolve_c(bound_to);
            const char    *a, *b;
            bool           same;

            if (nullary_name(bt, &a) && nullary_name(ex, &b)) {
                same = !strcmp(a, b);
            }
            else {
                same = bt == ex;
            }

            if (same) {
                th_err(ctx, TH_ERR_CONSTRAINT, "excludes", c->target,
                       bound_to,
                       "constraint excludes matched the excluded type");

                return false;
            }
            break;
        }

        case TH_CON_HAS_FIELD: {
            th_type *field_owner = c->field.auto_deref
                ? th_auto_deref(ctx, bound_to) : th_resolve(bound_to);
            th_type *m = th_record_lookup_in(ctx, field_owner,
                                            c->field.name);

            if (!m) {
                th_err(ctx, TH_ERR_NO_SUCH_FIELD, "has_field",
                       c->field.type, bound_to,
                       "constraint has_field: no such field");

                return false;
            }

            size_t field_errors = ctx->errors.len;
            if (c->field.type && !uni(ctx, m, c->field.type, seen)) {
                if (ctx->errors.len > field_errors) return false;
                th_err(ctx, TH_ERR_CONSTRAINT, "has_field", c->field.type,
                       m, "constraint has_field: field type does not unify");

                return false;
            }
            break;
        }

        case TH_CON_HAS_PARAM: {
            size_t at = (size_t)c->param.index;

            if (at >= th_named_nargs(bound_to)) {
                th_err(ctx, TH_ERR_PARAM_MISMATCH, "has_param",
                       c->param.type, bound_to,
                       "constraint has_param: no parameter at that index");

                return false;
            }

            const th_arg *a = th_named_arg(bound_to, at);

            if (!a || a->kind != TH_ARG_TYPE) {
                th_err(ctx, TH_ERR_PARAM_MISMATCH, "has_param",
                       c->param.type, bound_to,
                       "constraint has_param: parameter is not a type");

                return false;
            }

            size_t param_errors = ctx->errors.len;
            if (c->param.type && !uni(ctx, a->type, c->param.type, seen)) {
                if (ctx->errors.len > param_errors) return false;
                th_err(ctx, TH_ERR_CONSTRAINT, "has_param", c->param.type,
                       a->type,
                       "constraint has_param: parameter does not unify");

                return false;
            }
            break;
        }

        case TH_CON_IMPLEMENTS:
            if (!ctx->environment && !iface_named(ctx, c->iface)) {
                defer_environment_constraints(ctx, (th_type *)var);
                break;
            }
            constraint_scope_t previous = enter_constraint_scope(ctx, c);
            if (environment_constraint_active(ctx, TH_CON_IMPLEMENTS,
                                              c->iface, bound_to)) {
                leave_constraint_scope(ctx, previous);
                break;
            }
            push_environment_constraint(ctx, TH_CON_IMPLEMENTS, c->iface,
                                        bound_to);
            bool implements = th_implements(ctx, bound_to, c->iface);
            ctx->active_environment_constraints.len--;
            leave_constraint_scope(ctx, previous);
            if (!implements) {
                th_err(ctx, TH_ERR_CONSTRAINT, c->iface, nullptr, bound_to,
                       "constraint implements: interface not satisfied");

                return false;
            }
            break;

        case TH_CON_METHOD: {
            if (!ctx->environment) {
                defer_environment_constraints(ctx, (th_type *)var);
                break;
            }
            constraint_scope_t previous = enter_constraint_scope(ctx, c);
            if (environment_constraint_active(ctx, TH_CON_METHOD,
                                              c->method.name, bound_to)) {
                leave_constraint_scope(ctx, previous);
                break;
            }
            push_environment_constraint(ctx, TH_CON_METHOD, c->method.name,
                                        bound_to);
            th_env_method_status_t status = TH_ENV_METHOD_NOT_FOUND;
            th_type *signature = th_method_lookup(ctx, bound_to,
                                                  c->method.name);
            if (!signature) {
                th_env_method_t method = {0};
                status = th_env_method_resolve_by_name(
                    ctx, bound_to, c->method.name, &method, nullptr);
                signature = status == TH_ENV_METHOD_FOUND
                    ? method.signature : nullptr;
            }
            if (signature) signature = th_type_clone(ctx, signature);
            const th_param *receiver =
                signature ? th_func_param(signature, 0) : nullptr;
            th_type *method_result = signature ? th_func_ret(signature)
                                               : nullptr;
            bool matches = receiver && method_result;
            size_t receiver_errors = ctx->errors.len;
            bool receiver_matches = false;
            if (matches) {
                matches = uni(ctx, receiver->type, bound_to, seen);
                receiver_matches = matches;
                receiver_errors = ctx->errors.len;
                bool self_result = c->method.result == var
                    && method_result == receiver->type;
                if (matches && !self_result)
                    matches = uni(ctx, c->method.result, method_result, seen);
            }
            th_type *call = c->method.call;
            th_type *call_error = call ? th_func_error(call) : nullptr;
            if (matches && call_error) {
                th_type *target_error = th_func_error(signature);
                if (!target_error) target_error = th_name(ctx, "nil");
                matches = uni(ctx, call_error, target_error, seen);
            }
            size_t call_nparams = call ? th_func_nparams(call) : 0;
            size_t target_nparams = signature
                ? th_func_nparams(signature) : 0;
            if (matches && call && call_nparams > target_nparams - 1 &&
                !th_func_rest(signature))
                matches = false;
            for (size_t i = 0; matches && call && i < call_nparams; i++) {
                const th_param *actual = th_func_param(call, i);
                const th_param *formal = i + 1 < target_nparams
                    ? th_func_param(signature, i + 1)
                    : th_func_rest(signature);
                matches = actual && formal && method_argument_unifies(ctx,
                    actual->type, formal->type, formal->mode, seen);
            }
            for (size_t i = 0; matches && call && i < th_func_nkw(call); i++) {
                const th_kwparam *actual = th_func_kw(call, i);
                const th_kwparam *formal = actual
                    ? th_func_kw_lookup(signature, actual->name) : nullptr;
                matches = actual && (!th_func_nkw(signature) || formal) &&
                    (!formal || method_argument_unifies(ctx, actual->type,
                        formal->type, formal->mode, seen));
            }
            ctx->active_environment_constraints.len--;
            leave_constraint_scope(ctx, previous);
            if (!matches && receiver_matches
                && ctx->errors.len > receiver_errors)
                return false;
            if (!matches) {
                th_err(ctx, TH_ERR_CONSTRAINT, c->method.name,
                       c->method.result, bound_to,
                       status == TH_ENV_METHOD_AMBIGUOUS
                           ? "method dispatch is ambiguous"
                           : "no matching method implementation");
                return false;
            }
            break;
        }
        }
    }

    return true;
}

bool th_ctx_validate_constraints(th_ctx *ctx)
{
    if (!ctx || !ctx->environment) return false;
    size_t pending = 0;
    for (size_t i = 0; i < ctx->pending_environment_constraints.len; i++) {
        th_type *variable = ctx->pending_environment_constraints.data[i];
        th_type *bound_to = th_resolve(variable);
        if (!bound_to || bound_to->kind == TH_VAR) {
            ctx->pending_environment_constraints.data[pending++] = variable;
            continue;
        }
        core_dict_t seen;
        core_dict_init(&seen, nullptr, nullptr);
        bool valid = con_check(ctx, variable, bound_to, &seen);
        uni_seen_free(&seen);
        if (!valid) return false;
    }
    ctx->pending_environment_constraints.len = pending;
    return true;
}

th_type *th_resolve(th_type *t)
{
    while (t && t->kind == TH_VAR && t->var.ref) {
        th_type *next = t->var.ref;

        if (next->kind == TH_VAR && next->var.ref) {
            t->var.ref = next->var.ref;
        }

        t = t->var.ref;
    }

    return t;
}

typedef struct {
    th_type  *t;
    th_type **kids;
    size_t    n;
    size_t    next;
    bool      entered;
} fvframe;

core_option_decl(fvframe);
core_list_decl(fvframe);

typedef th_type *th_occ_ptr_t;

core_list_decl(th_occ_ptr_t);

static th_type **fv_children(th_ctx *ctx, th_type *t, core_dict_t *acc,
                             size_t *out)
{
    th_type **kids = nullptr;
    size_t    n    = 0;

    *out = 0;

    switch (t->kind) {
    case TH_VAR: {
        bool found;

        core_dict_get(acc, t, &found);

        if (!found) {
            core_dict_put(acc, t, core_dict_len(acc) + 1);
        }
        for (size_t i = 0; i < t->var.cons.len; i++) {
            const th_constraint *c = &t->var.cons.data[i];
            switch (c->kind) {
            case TH_CON_UNIFIES:
            case TH_CON_PROMOTES:
            case TH_CON_EXCLUDES:
                n += c->target != nullptr;
                break;
            case TH_CON_ONE_OF:
                n += c->one_of.n;
                break;
            case TH_CON_HAS_FIELD:
                n += c->field.type != nullptr;
                break;
            case TH_CON_HAS_PARAM:
                n += c->param.type != nullptr;
                break;
            case TH_CON_METHOD:
                n += c->method.result != nullptr;
                n += c->method.call != nullptr;
                break;
            case TH_CON_IMPLEMENTS:
            case TH_CON_KIND:
                break;
            }
        }
        size_t count = n;
        n = 0;
        kids = core_alloc_array(th_type *, count);
        for (size_t i = 0; i < t->var.cons.len; i++) {
            const th_constraint *c = &t->var.cons.data[i];
            switch (c->kind) {
            case TH_CON_UNIFIES:
            case TH_CON_PROMOTES:
            case TH_CON_EXCLUDES:
                if (c->target) kids[n++] = c->target;
                break;
            case TH_CON_ONE_OF:
                for (size_t j = 0; j < c->one_of.n; j++)
                    kids[n++] = c->one_of.types[j];
                break;
            case TH_CON_HAS_FIELD:
                if (c->field.type) kids[n++] = c->field.type;
                break;
            case TH_CON_HAS_PARAM:
                if (c->param.type) kids[n++] = c->param.type;
                break;
            case TH_CON_METHOD:
                if (c->method.result) kids[n++] = c->method.result;
                if (c->method.call) kids[n++] = c->method.call;
                break;
            case TH_CON_IMPLEMENTS:
            case TH_CON_KIND:
                break;
            }
        }
        break;
    }

    case TH_NAMED:
        kids = core_alloc_array(th_type *, t->named.nargs);

        for (size_t i = 0; i < t->named.nargs; i++) {
            if (t->named.args[i].kind == TH_ARG_TYPE) {
                kids[n++] = t->named.args[i].type;
            }
        }
        break;

    case TH_INDIRECTION:
        kids      = core_alloc_array(th_type *, 1);
        kids[n++] = t->ind.referent;
        break;

    case TH_SEQUENCE:
        kids      = core_alloc_array(th_type *, 1);
        kids[n++] = t->seq.elem;
        break;

    case TH_RECORD: {
        size_t     rn = t->record.n;
        th_member *m  = core_alloc_array(th_member, rn);

        memcpy(m, t->record.members, rn * sizeof(th_member));
        qsort(m, rn, sizeof(th_member), th_cmp_member);

        kids = core_alloc_array(th_type *, rn);

        for (size_t i = 0; i < rn; i++) {
            kids[n++] = m[i].type;
        }
        core_free(m);
        break;
    }

    case TH_TRAIT:
        kids = core_alloc_array(th_type *,
            t->trait.n + 2 * t->trait.nr);
        for (size_t i = 0; i < t->trait.n; i++)
            kids[n++] = t->trait.methods[i].type;
        for (size_t i = 0; i < t->trait.nr; i++) {
            kids[n++] = t->trait.requirements[i].subject;
            if (t->trait.requirements[i].target)
                kids[n++] = t->trait.requirements[i].target;
        }
        break;

    case TH_FUNCTION: {
        kids      = core_alloc_array(th_type *, t->fn.np + t->fn.nkw + 3);
        kids[n++] = t->fn.ret;

        for (size_t i = 0; i < t->fn.np; i++) {
            kids[n++] = t->fn.params[i].type;
        }

        if (t->fn.rest) {
            kids[n++] = t->fn.rest->type;
        }

        if (t->fn.nkw) {
            th_kwparam *k = core_alloc_array(th_kwparam, t->fn.nkw);

            memcpy(k, t->fn.kw, t->fn.nkw * sizeof(th_kwparam));
            qsort(k, t->fn.nkw, sizeof(th_kwparam), th_cmp_kw);

            for (size_t i = 0; i < t->fn.nkw; i++) {
                kids[n++] = k[i].type;
            }
            core_free(k);
        }
        if (t->fn.error) kids[n++] = t->fn.error;
        break;
    }

    case TH_BINDER:
        kids      = core_alloc_array(th_type *, 1);
        kids[n++] = t->binder.body;
        break;

    case TH_SUM:
        *out = t->sum.n;

        return th_sort_arms(ctx, t->sum.arms, t->sum.n);

    default:
        return nullptr;
    }

    *out = n;

    return kids;
}

static void fv_walk(th_ctx *ctx, th_type *root, core_dict_t *acc,
                    core_dict_t *stack)
{
    core_list_t(fvframe) st = core_list_new_cap(fvframe, 16);

    core_list_push(st, ((fvframe){.t = root}));

    while (st.len) {
        fvframe *f = &st.data[st.len - 1];

        if (!f->entered) {
            f->entered = true;
            f->t       = th_resolve(f->t);

            if (!f->t || core_dict_contains(stack, f->t)) {
                f->t = nullptr;
                core_list_pop(fvframe, st);

                continue;
            }
            core_dict_put(stack, f->t, f->t);
            f->kids = fv_children(ctx, f->t, acc, &f->n);
            f       = &st.data[st.len - 1];
        }

        if (f->next < f->n) {
            th_type *kid = f->kids[f->next++];

            core_list_push(st, ((fvframe){.t = kid}));

            continue;
        }

        core_dict_remove(stack, f->t);
        core_free(f->kids);
        core_list_pop(fvframe, st);
    }

    core_list_free(st);
}

uint64_t th_type_graph_revision(th_type *const *roots, size_t count)
{
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    core_list_t(th_occ_ptr_t) pending = {0};
    for (size_t i = 0; i < count; i++)
        if (roots[i]) core_list_push(pending, roots[i]);
    uint64_t revision = 0;
#define VISIT(child) do { if (child) core_list_push(pending, (child)); } while (0)
    while (pending.len) {
        th_type *type = pending.data[--pending.len];
        if (!type || core_dict_contains(&seen, type)) continue;
        core_dict_put(&seen, type, type);
        revision += type->revision;
        switch (type->kind) {
        case TH_VAR:
            VISIT(type->var.ref);
            for (size_t i = 0; i < type->var.cons.len; i++) {
                const th_constraint *c = &type->var.cons.data[i];
                switch (c->kind) {
                case TH_CON_UNIFIES:
                case TH_CON_PROMOTES:
                case TH_CON_EXCLUDES: VISIT(c->target); break;
                case TH_CON_ONE_OF:
                    for (size_t j = 0; j < c->one_of.n; j++)
                        VISIT(c->one_of.types[j]);
                    break;
                case TH_CON_HAS_FIELD: VISIT(c->field.type); break;
                case TH_CON_HAS_PARAM: VISIT(c->param.type); break;
                case TH_CON_METHOD:
                    VISIT(c->method.result);
                    VISIT(c->method.call);
                    break;
                default: break;
                }
            }
            break;
        case TH_NAMED:
            for (size_t i = 0; i < type->named.nargs; i++)
                if (type->named.args[i].kind == TH_ARG_TYPE)
                    VISIT(type->named.args[i].type);
            break;
        case TH_INDIRECTION: VISIT(type->ind.referent); break;
        case TH_SEQUENCE: VISIT(type->seq.elem); break;
        case TH_RECORD:
            for (size_t i = 0; i < type->record.n; i++)
                VISIT(type->record.members[i].type);
            break;
        case TH_TRAIT:
            for (size_t i = 0; i < type->trait.n; i++)
                VISIT(type->trait.methods[i].type);
            for (size_t i = 0; i < type->trait.nr; i++) {
                VISIT(type->trait.requirements[i].subject);
                VISIT(type->trait.requirements[i].target);
            }
            break;
        case TH_FUNCTION:
            VISIT(type->fn.ret);
            VISIT(type->fn.error);
            for (size_t i = 0; i < type->fn.np; i++)
                VISIT(type->fn.params[i].type);
            if (type->fn.rest) VISIT(type->fn.rest->type);
            for (size_t i = 0; i < type->fn.nkw; i++)
                VISIT(type->fn.kw[i].type);
            break;
        case TH_BINDER:
            VISIT(type->binder.body);
            for (size_t i = 0; i < type->binder.arity; i++)
                VISIT(type->binder.qvars[i]);
            break;
        case TH_SUM:
            for (size_t i = 0; i < type->sum.n; i++)
                VISIT(type->sum.arms[i]);
            break;
        default: break;
        }
    }
#undef VISIT
    core_list_free(pending);
    core_dict_free(&seen);
    return revision;
}

size_t th_free_vars(th_ctx *ctx, th_type *t, th_type ***out)
{
    core_dict_t acc, stack;

    core_dict_init(&acc, nullptr, nullptr);
    core_dict_init(&stack, nullptr, nullptr);

    fv_walk(ctx, t, &acc, &stack);
    core_dict_free(&stack);

    size_t n = core_dict_len(&acc);

    if (out) {
        th_type **r = th_alloc(ctx, (n ? n : 1) * sizeof(th_type *));

        core_dict_foreach (&acc, b) {
            r[(size_t)(uintptr_t)b->value - 1] = (th_type *)b->key;
        }
        *out = r;
    }
    core_dict_free(&acc);

    return n;
}

bool th_is_closed(th_ctx *ctx, th_type *t)
{
    return th_free_vars(ctx, t, nullptr) == 0;
}

bool th_occurs(th_ctx *ctx, th_type *var, th_type *t)
{
    (void)ctx;

    core_dict_t seen;
    core_list_t(th_occ_ptr_t) work = core_list_new_cap(th_occ_ptr_t, 16);
    bool                      hit  = false;

    core_dict_init(&seen, nullptr, nullptr);

    var = th_resolve(var);

    core_list_push(work, t);

    while (!hit && work.len) {
        th_type *cur = th_resolve(work.data[--work.len]);

        if (!cur || core_dict_contains(&seen, cur)) {
            continue;
        }

        if (cur == var) {
            hit = true;

            break;
        }

        core_dict_put(&seen, cur, cur);

        switch (cur->kind) {
        case TH_NAMED:
            for (size_t i = 0; i < cur->named.nargs; i++) {
                if (cur->named.args[i].kind == TH_ARG_TYPE) {
                    core_list_push(work, cur->named.args[i].type);
                }
            }
            break;

        case TH_INDIRECTION:
            core_list_push(work, cur->ind.referent);
            break;

        case TH_SEQUENCE:
            core_list_push(work, cur->seq.elem);
            break;

        case TH_RECORD:
            for (size_t i = 0; i < cur->record.n; i++) {
                core_list_push(work, cur->record.members[i].type);
            }
            break;

        case TH_TRAIT:
            for (size_t i = 0; i < cur->trait.n; i++)
                core_list_push(work, cur->trait.methods[i].type);
            for (size_t i = 0; i < cur->trait.nr; i++) {
                core_list_push(work, cur->trait.requirements[i].subject);
                if (cur->trait.requirements[i].target)
                    core_list_push(work, cur->trait.requirements[i].target);
            }
            break;

        case TH_FUNCTION:
            core_list_push(work, cur->fn.ret);
            if (cur->fn.error) core_list_push(work, cur->fn.error);

            for (size_t i = 0; i < cur->fn.np; i++) {
                core_list_push(work, cur->fn.params[i].type);
            }

            if (cur->fn.rest) {
                core_list_push(work, cur->fn.rest->type);
            }

            for (size_t i = 0; i < cur->fn.nkw; i++) {
                core_list_push(work, cur->fn.kw[i].type);
            }
            break;

        case TH_BINDER:
            core_list_push(work, cur->binder.body);
            break;

        case TH_SUM:
            for (size_t i = 0; i < cur->sum.n; i++) {
                core_list_push(work, cur->sum.arms[i]);
            }
            break;

        default:
            break;
        }
    }

    core_dict_free(&seen);
    core_list_free(work);

    return hit;
}

static th_type *dup_shallow(th_ctx *ctx, const th_type *t)
{
    th_type *o = th_alloc(ctx, sizeof(th_type));

    *o = *t;

    return o;
}

typedef struct {
    th_type **slot;
    th_type  *src;
    uint64_t  depth;
} rjob;

core_option_decl(rjob);
core_list_decl(rjob);

typedef bool (*subst_fn)(th_ctx *ctx, th_type *t, uint64_t depth,
                         core_dict_t *memo, void *st, th_type **out);

static void rpush(core_list_t(rjob) *w, th_type **slot, th_type *src,
                  uint64_t depth)
{
    core_list_push(*w, ((rjob){.slot = slot, .src = src, .depth = depth}));
}

static void plan_kids(th_ctx *ctx, th_type *out, const th_type *t,
                      uint64_t depth, core_list_t(rjob) *w)
{
    switch (t->kind) {
    case TH_NAMED:
        if (t->named.nargs) {
            out->named.args = th_alloc(ctx, t->named.nargs * sizeof(th_arg));
            memcpy(out->named.args, t->named.args,
                   t->named.nargs * sizeof(th_arg));

            for (size_t i = 0; i < t->named.nargs; i++) {
                if (out->named.args[i].kind == TH_ARG_TYPE) {
                    rpush(w, &out->named.args[i].type,
                          out->named.args[i].type, depth);
                }
            }
        }
        break;

    case TH_INDIRECTION:
        rpush(w, &out->ind.referent, t->ind.referent, depth);
        break;

    case TH_SEQUENCE:
        rpush(w, &out->seq.elem, t->seq.elem, depth);
        break;

    case TH_RECORD:
        if (t->record.n) {
            out->record.members = th_alloc(ctx,
                                           t->record.n * sizeof(th_member));
            memcpy(out->record.members, t->record.members,
                   t->record.n * sizeof(th_member));

            for (size_t i = 0; i < t->record.n; i++) {
                rpush(w, &out->record.members[i].type,
                      out->record.members[i].type, depth);
            }
        }
        break;

    case TH_TRAIT:
        if (t->trait.n) {
            out->trait.methods = th_alloc(ctx,
                t->trait.n * sizeof(th_member));
            memcpy(out->trait.methods, t->trait.methods,
                t->trait.n * sizeof(th_member));
            for (size_t i = 0; i < t->trait.n; i++)
                rpush(w, &out->trait.methods[i].type,
                    t->trait.methods[i].type, depth);
        }
        if (t->trait.nr) {
            out->trait.requirements = th_alloc(ctx,
                t->trait.nr * sizeof(th_trait_requirement));
            memcpy(out->trait.requirements, t->trait.requirements,
                t->trait.nr * sizeof(th_trait_requirement));
            for (size_t i = 0; i < t->trait.nr; i++) {
                rpush(w, &out->trait.requirements[i].subject,
                    t->trait.requirements[i].subject, depth);
                if (t->trait.requirements[i].target)
                    rpush(w, &out->trait.requirements[i].target,
                        t->trait.requirements[i].target, depth);
            }
        }
        break;

    case TH_FUNCTION:
        rpush(w, &out->fn.ret, t->fn.ret, depth);
        if (t->fn.error) rpush(w, &out->fn.error, t->fn.error, depth);

        if (t->fn.np) {
            out->fn.params = th_alloc(ctx, t->fn.np * sizeof(th_param));
            memcpy(out->fn.params, t->fn.params, t->fn.np * sizeof(th_param));

            for (size_t i = 0; i < t->fn.np; i++) {
                rpush(w, &out->fn.params[i].type, out->fn.params[i].type,
                      depth);
            }
        }

        if (t->fn.rest) {
            out->fn.rest  = th_alloc(ctx, sizeof(th_param));
            *out->fn.rest = *t->fn.rest;
            rpush(w, &out->fn.rest->type, t->fn.rest->type, depth);
        }

        if (t->fn.nkw) {
            out->fn.kw = th_alloc(ctx, t->fn.nkw * sizeof(th_kwparam));
            memcpy(out->fn.kw, t->fn.kw, t->fn.nkw * sizeof(th_kwparam));

            for (size_t i = 0; i < t->fn.nkw; i++) {
                rpush(w, &out->fn.kw[i].type, out->fn.kw[i].type, depth);
            }
        }
        break;

    case TH_BINDER:
        rpush(w, &out->binder.body, t->binder.body, depth + 1);
        break;

    case TH_SUM:
        if (t->sum.n) {
            out->sum.arms = th_alloc(ctx, t->sum.n * sizeof(th_type *));

            for (size_t i = 0; i < t->sum.n; i++) {
                out->sum.arms[i] = t->sum.arms[i];
                rpush(w, &out->sum.arms[i], t->sum.arms[i], depth);
            }
        }
        break;

    default:
        break;
    }
}

static th_type *rebuild(th_ctx *ctx, th_type *root, core_dict_t *memo,
                        bool resolve, subst_fn subst, void *st)
{
    th_type          *out_root = nullptr;
    core_list_t(rjob)  w        = core_list_new(rjob);

    rpush(&w, &out_root, root, 0);

    while (w.len) {
        rjob     j = core_option_get(core_list_pop(rjob, w));
        th_type *t = resolve ? th_resolve(j.src) : j.src;

        if (!t) {
            *j.slot = nullptr;

            continue;
        }

        bool  found;
        void *hit = core_dict_get(memo, t, &found);

        if (found) {
            *j.slot = hit;

            continue;
        }

        th_type *fixed = nullptr;

        if (subst(ctx, t, j.depth, memo, st, &fixed)) {
            *j.slot = fixed;

            continue;
        }

        th_type *out = dup_shallow(ctx, t);

        core_dict_put(memo, t, out);
        *j.slot = out;

        plan_kids(ctx, out, t, j.depth, &w);
    }

    core_list_free(w);

    return out_root;
}


static bool subst_subst(th_ctx *ctx, th_type *t, uint64_t depth,
                        core_dict_t *memo, void *st, th_type **out)
{
    (void)ctx;
    (void)depth;
    (void)memo;
    (void)st;

    if (t->kind != TH_VAR) {
        return false;
    }
    *out = t;

    return true;
}

th_type *th_apply_subst(th_ctx *ctx, th_type *t)
{
    core_dict_t memo;

    core_dict_init(&memo, nullptr, nullptr);

    th_type *r = rebuild(ctx, t, &memo, true, subst_subst, nullptr);

    core_dict_free(&memo);

    return r;
}

static bool arg_eq(const th_arg *x, const th_arg *y)
{
    if (x->kind != y->kind) {
        return false;
    }

    switch (x->kind) {
    case TH_ARG_UINT:
        return x->u == y->u;
    case TH_ARG_SINT:
        return x->s == y->s;
    case TH_ARG_STR:
        return x->str.n == y->str.n
            && !memcmp(x->str.p, y->str.p, x->str.n);
    default:
        return true;
    }
}

typedef struct {
    th_type *a;
    th_type *b;
} tpair;

core_option_decl(tpair);
core_list_decl(tpair);

static void upush(core_list_t(tpair) *w, th_type *a, th_type *b)
{
    core_list_push(*w, ((tpair){.a = a, .b = b}));
}

static bool uni_named(th_ctx *ctx, th_type *a, th_type *b,
                      core_list_t(tpair) *w)
{
    if (a->named.nlen != b->named.nlen
        || memcmp(a->named.name, b->named.name, a->named.nlen)
        || a->named.is_variant != b->named.is_variant) {
        th_err(ctx, TH_ERR_UNIFY, nullptr, b, a,
               "these are different named types");

        return false;
    }

    if (a->named.nargs != b->named.nargs) {
        th_err(ctx, TH_ERR_ARITY_MISMATCH, nullptr, b, a,
               "the two applications have different argument counts");

        return false;
    }

    for (size_t i = a->named.nargs; i-- > 0;) {
        const th_arg *x = &a->named.args[i], *y = &b->named.args[i];

        if (x->kind != y->kind) {
            th_err(ctx, TH_ERR_ARITY_MISMATCH, nullptr, b, a,
                   "an argument differs in kind");

            return false;
        }

        if (x->kind == TH_ARG_TYPE) {
            upush(w, x->type, y->type);
        }
        else if (!arg_eq(x, y)) {
            th_err(ctx, TH_ERR_UNIFY, nullptr, b, a,
                   "a non-type argument differs");

            return false;
        }
    }

    return true;
}

static bool uni_record(th_ctx *ctx, th_type *a, th_type *b,
                       core_list_t(tpair) *w)
{
    if (a->record.n != b->record.n) {
        th_err(ctx, TH_ERR_ARITY_MISMATCH, nullptr, b, a,
               "the two records have different member counts");

        return false;
    }

    core_dict_t by_name;

    core_dict_init(&by_name, core_hash_cstring, core_dict_cstr_eq);

    for (size_t i = 0; i < b->record.n; i++) {
        core_dict_put(&by_name, b->record.members[i].name,
                      b->record.members[i].type);
    }

    bool ok = true;

    for (size_t i = a->record.n; ok && i-- > 0;) {
        bool     found = false;
        th_type *m     = core_dict_get(&by_name,
                                       a->record.members[i].name, &found);

        if (!found) {
            th_err(ctx, TH_ERR_NO_SUCH_FIELD, a->record.members[i].name,
                   b, a, "the other record has no such member");
            ok = false;

            break;
        }

        upush(w, a->record.members[i].type, m);
    }

    core_dict_free(&by_name);

    return ok;
}

static bool uni_func(th_ctx *ctx, th_type *a, th_type *b,
                     core_list_t(tpair) *w)
{
    if (!a->fn.rest != !b->fn.rest || a->fn.np != b->fn.np
        || a->fn.nkw != b->fn.nkw || !a->fn.error != !b->fn.error) {
        th_err(ctx, TH_ERR_ARITY_MISMATCH, nullptr, b, a,
               "the two signatures differ in arity");

        return false;
    }

    core_dict_t kw_by_name;

    core_dict_init(&kw_by_name, core_hash_cstring, core_dict_cstr_eq);

    for (size_t i = 0; i < b->fn.nkw; i++) {
        core_dict_put(&kw_by_name, b->fn.kw[i].name, &b->fn.kw[i]);
    }

    bool kw_ok = true;

    for (size_t i = a->fn.nkw; kw_ok && i-- > 0;) {
        bool              found = false;
        const th_kwparam *k     = core_dict_get(&kw_by_name,
                                                a->fn.kw[i].name, &found);

        if (!found || k->mode != a->fn.kw[i].mode) {
            th_err(ctx, TH_ERR_MISSING_KEYWORD, a->fn.kw[i].name, b, a,
                   "the other signature lacks that keyword parameter");
            kw_ok = false;

            break;
        }

        upush(w, a->fn.kw[i].type, k->type);
    }

    core_dict_free(&kw_by_name);

    if (!kw_ok) {
        return false;
    }

    if (a->fn.rest) {
        if (a->fn.rest->mode != b->fn.rest->mode) {
            th_err(ctx, TH_ERR_UNIFY, nullptr, b, a,
                   "the rest parameters differ in mode");

            return false;
        }

        upush(w, a->fn.rest->type, b->fn.rest->type);
    }

    for (size_t i = a->fn.np; i-- > 0;) {
        if (a->fn.params[i].mode != b->fn.params[i].mode) {
            th_err(ctx, TH_ERR_UNIFY, nullptr, b, a,
                   "a parameter differs in mode");

            return false;
        }

        upush(w, a->fn.params[i].type, b->fn.params[i].type);
    }

    upush(w, a->fn.ret, b->fn.ret);
    if (a->fn.error) upush(w, a->fn.error, b->fn.error);

    return true;
}

static bool uni_sum(th_ctx *ctx, th_type *a, th_type *b, core_list_t(tpair) *w)
{
    if (a->sum.comb != b->sum.comb || a->sum.n != b->sum.n) {
        th_err(ctx, TH_ERR_ARITY_MISMATCH, nullptr, b, a,
               "the two composites differ in shape");

        return false;
    }

    if (th_is_closed(ctx, a) && th_is_closed(ctx, b)) {
        return th_hash(ctx, a) == th_hash(ctx, b);
    }

    size_t    n  = a->sum.n;
    th_type **xa = th_sort_arms(ctx, a->sum.arms, n);
    th_type **xb = th_sort_arms(ctx, b->sum.arms, n);

    for (size_t i = n; i-- > 0;) {
        upush(w, xa[i], xb[i]);
    }
    core_free(xa);
    core_free(xb);

    return true;
}

static bool uni_step(th_ctx *ctx, th_type *a, th_type *b, core_list_t(tpair) *w)
{
    switch (a->kind) {
    case TH_NAMED:
        return uni_named(ctx, a, b, w);

    case TH_BITINT:
        return a->bitint.width == b->bitint.width
            && a->bitint.flavor == b->bitint.flavor;

    case TH_INDIRECTION:
        if (a->ind.attrs != b->ind.attrs) {
            return false;
        }
        upush(w, a->ind.referent, b->ind.referent);

        return true;

    case TH_SEQUENCE:
        if (a->seq.has_extent != b->seq.has_extent) {
            return false;
        }

        if (a->seq.has_extent && a->seq.extent != b->seq.extent) {
            return false;
        }

        upush(w, a->seq.elem, b->seq.elem);

        return true;

    case TH_RECORD:
        return uni_record(ctx, a, b, w);

    case TH_TRAIT:
        if (a->trait.nlen != b->trait.nlen
            || memcmp(a->trait.name, b->trait.name, a->trait.nlen)
            || a->trait.n != b->trait.n
            || a->trait.nr != b->trait.nr) return false;
        for (size_t i = a->trait.n; i-- > 0;) {
            if (strcmp(a->trait.methods[i].name,
                       b->trait.methods[i].name)) return false;
            upush(w, a->trait.methods[i].type,
                b->trait.methods[i].type);
        }
        for (size_t i = a->trait.nr; i-- > 0;) {
            const th_trait_requirement *left = &a->trait.requirements[i];
            const th_trait_requirement *right = &b->trait.requirements[i];
            if (left->kind != right->kind
                || (left->iface && !right->iface)
                || (!left->iface && right->iface)
                || (left->iface && strcmp(left->iface, right->iface)))
                return false;
            upush(w, left->subject, right->subject);
            if (left->target) upush(w, left->target, right->target);
        }
        return true;

    case TH_FUNCTION:
        return uni_func(ctx, a, b, w);

    case TH_BINDER:
        if (a->binder.arity != b->binder.arity) {
            return false;
        }
        upush(w, a->binder.body, b->binder.body);

        return true;

    case TH_BOUND:
        return a->bound.distance == b->bound.distance
            && a->bound.position == b->bound.position;

    case TH_SUM:
        return uni_sum(ctx, a, b, w);

    default:
        return false;
    }
}

static bool uni(th_ctx *ctx, th_type *a0, th_type *b0, core_dict_t *seen)
{
    core_list_t(tpair) w  = core_list_new(tpair);
    bool              ok = true;

    upush(&w, a0, b0);

    while (ok && w.len) {
        tpair    p = core_option_get(core_list_pop(tpair, w));
        th_type *a = th_resolve(p.a);
        th_type *b = th_resolve(p.b);

        if (a == b) {
            continue;
        }

        if (!a || !b) {
            ok = false;
            break;
        }

        bool        found;
        core_dict_t *rhs = core_dict_get(seen, a, &found);

        if (!found) {
            rhs = core_alloc(core_dict_t);

            core_dict_init(rhs, nullptr, nullptr);
            core_dict_put(seen, a, rhs);
        }

        if (core_dict_contains(rhs, b)) {
            continue;
        }
        core_dict_put(rhs, b, b);

        if (a->kind != TH_VAR && b->kind == TH_VAR) {
            th_type *t = a;

            a = b;
            b = t;
        }

        if (a->kind == TH_VAR) {
            if (th_occurs(ctx, a, b)) {
                th_err(ctx, TH_ERR_OCCURS, nullptr, a, b,
                       "the variable occurs in the type it would be bound to");
                ok = false;
                break;
            }

            if (!con_check(ctx, a, b, seen)) {
                ok = false;
                break;
            }
            a->var.ref = b;
            a->revision++;

            continue;
        }

        if (a->kind != b->kind) {
            th_err(ctx, TH_ERR_UNIFY, nullptr, b, a,
                   "the two types are of different kinds");
            ok = false;
            break;
        }
        ok = uni_step(ctx, a, b, &w);
    }

    core_list_free(w);

    return ok;
}

static bool uni_in_place(th_ctx *ctx, th_type *a, th_type *b)
{
    core_dict_t seen;

    core_dict_init(&seen, nullptr, nullptr);

    bool ok = uni(ctx, a, b, &seen);

    uni_seen_free(&seen);

    return ok;
}

static void uni_seen_free(core_dict_t *seen)
{
    core_dict_foreach (seen, e) {
        core_dict_free((core_dict_t *)e->value);
        core_free(e->value);
    }
    core_dict_free(seen);
}

th_type *th_unify(th_ctx *ctx, th_type *a, th_type *b)
{
    if (!ctx || !a || !b) return nullptr;
    const th_type *sources[] = {a, b};
    th_type *copies[2];
    if (!th_type_clone_many(ctx, sources, copies, 2)) return nullptr;
    th_type *ia = copies[0];
    th_type *ib = copies[1];
    size_t   before = th_error_count(ctx);

    if (!ia || !ib) return nullptr;

    if (!uni_in_place(ctx, ia, ib)) {
        if (th_error_count(ctx) == before) {
            th_err(ctx, TH_ERR_UNIFY, nullptr, ib, ia,
                   "types do not unify");
        }

        return nullptr;
    }

    th_type *r = th_apply_subst(ctx, ia);

    th_normalize(r);

    return r;
}

th_type *th_unify_probe(th_ctx *ctx, th_type *a, th_type *b)
{
    if (!ctx || !a || !b) return nullptr;
    size_t before = th_error_count(ctx);
    th_type *result = th_unify(ctx, a, b);
    if (!result) ctx->errors.len = before;
    return result;
}

th_type *th_unify_in_place(th_ctx *ctx, th_type *a, th_type *b)
{
    if (!ctx || !a || !b) return nullptr;

    size_t before = th_error_count(ctx);
    if (!uni_in_place(ctx, a, b)) {
        if (th_error_count(ctx) == before) {
            th_err(ctx, TH_ERR_UNIFY, nullptr, b, a,
                   "types do not unify");
        }
        return nullptr;
    }

    th_type *r = th_apply_subst(ctx, a);
    th_normalize(r);
    return r;
}

typedef struct {
    core_dict_t memo;
    th_type **fresh;
    th_type **qvars;
    uint64_t  arity;
    uint64_t  depth;
} th_inst;

static bool inst_subst(th_ctx *ctx, th_type *t, uint64_t depth, core_dict_t *memo,
                       void *st, th_type **out)
{
    th_inst *s = st;

    (void)ctx;
    (void)memo;

    if (t->kind == TH_BOUND) {
        *out = t->bound.distance == depth + 1 && t->bound.position < s->arity
                 ? s->fresh[t->bound.position]
                 : t;

        return true;
    }

    if (t->kind == TH_VAR) {
        *out = t;

        return true;
    }

    return false;
}

static bool inst_constraint_subst(th_ctx *ctx, th_type *t, uint64_t depth,
                                  core_dict_t *memo, void *st,
                                  th_type **out)
{
    th_inst *s = st;
    (void)ctx;
    (void)depth;
    (void)memo;
    if (t->kind != TH_VAR) return false;
    for (uint64_t i = 0; i < s->arity; i++) {
        if (s->qvars && (s->qvars[i] == t
            || th_resolve(s->qvars[i]) == t)) {
            *out = s->fresh[i];
            return true;
        }
    }
    *out = t;
    return true;
}

static th_constraint instantiate_constraint(th_ctx *ctx, th_constraint c,
                                             th_inst *s)
{
    switch (c.kind) {
    case TH_CON_UNIFIES:
    case TH_CON_PROMOTES:
    case TH_CON_EXCLUDES:
        c.target = rebuild(ctx, c.target, &s->memo, true,
                           inst_constraint_subst, s);
        break;
    case TH_CON_ONE_OF: {
        th_type **types = th_alloc(ctx, c.one_of.n * sizeof(th_type *));
        for (size_t i = 0; i < c.one_of.n; i++)
            types[i] = rebuild(ctx, c.one_of.types[i], &s->memo, true,
                               inst_constraint_subst, s);
        c.one_of.types = types;
        break;
    }
    case TH_CON_HAS_FIELD:
        c.field.type = rebuild(ctx, c.field.type, &s->memo, true,
                               inst_constraint_subst, s);
        break;
    case TH_CON_HAS_PARAM:
        c.param.type = rebuild(ctx, c.param.type, &s->memo, true,
                               inst_constraint_subst, s);
        break;
    case TH_CON_METHOD:
        c.method.result = rebuild(ctx, c.method.result, &s->memo, true,
                                  inst_constraint_subst, s);
        c.method.call = rebuild(ctx, c.method.call, &s->memo, true,
                                inst_constraint_subst, s);
        break;
    case TH_CON_IMPLEMENTS:
    case TH_CON_KIND:
        break;
    }
    return c;
}

th_type *th_instantiate(th_ctx *ctx, th_type *t)
{
    t = th_resolve(t);

    if (!t || t->kind != TH_BINDER) {
        return t;
    }

    uint64_t n = t->binder.arity;
    th_inst  s = {.arity = n, .depth = 0, .qvars = t->binder.qvars};

    core_dict_init(&s.memo, nullptr, nullptr);
    s.fresh = core_alloc_array(th_type *, n);

    for (uint64_t i = 0; i < n; i++) {
        s.fresh[i] = th_var(ctx);

        th_type *q = t->binder.qvars ? t->binder.qvars[i] : nullptr;

        if (q) {
            s.fresh[i]->var.name = q->var.name;
        }
    }

    for (uint64_t i = 0; i < n; i++) {
        th_type *q = t->binder.qvars ? t->binder.qvars[i] : nullptr;
        if (!q) continue;
        for (size_t j = 0; j < q->var.cons.len; j++)
            th_var_constrain(ctx, s.fresh[i],
                             instantiate_constraint(ctx,
                                 q->var.cons.data[j], &s));
    }

    th_type *r = rebuild(ctx, t->binder.body, &s.memo, true, inst_subst, &s);

    core_dict_free(&s.memo);
    core_free(s.fresh);
    th_normalize(r);

    return r;
}

typedef struct {
    core_dict_t memo;
    core_dict_t index;
    th_type  **vars;
    size_t     nvars;
    uint64_t   depth;
} th_gen;

static bool gen_subst(th_ctx *ctx, th_type *t, uint64_t depth, core_dict_t *memo,
                      void *st, th_type **out)
{
    th_gen *s = st;

    (void)memo;

    if (t->kind != TH_VAR) {
        return false;
    }

    bool   found;
    size_t at = (size_t)(uintptr_t)core_dict_get(&s->index, t, &found);

    *out = found ? th_bound(ctx, depth + 1, at - 1) : t;

    return true;
}

static th_type *generalize_vars(th_ctx *ctx, th_type *t, th_type **vars,
                                size_t nvars)
{
    if (!nvars) {
        th_normalize(t);
        return t;
    }

    th_gen s = {.vars = vars, .nvars = nvars};
    core_dict_init(&s.memo, nullptr, nullptr);
    core_dict_init(&s.index, nullptr, nullptr);

    for (size_t i = 0; i < s.nvars; i++) {
        core_dict_put(&s.index, s.vars[i], i + 1);
    }

    th_type *body = rebuild(ctx, t, &s.memo, true, gen_subst, &s);

    core_dict_free(&s.memo);
    core_dict_free(&s.index);

    th_type *r = th_binder(ctx, s.nvars, body);
    r->binder.qvars = s.vars;
    th_normalize(r);
    return r;
}

th_type *th_generalize(th_ctx *ctx, th_type *t)
{
    th_type **vars = nullptr;
    size_t nvars = th_free_vars(ctx, t, &vars);
    return generalize_vars(ctx, t, vars, nvars);
}

th_type *th_generalize_excluding(th_ctx *ctx, th_type *t,
                                 th_type *const *environment,
                                 size_t environment_count)
{
    if (!environment || !environment_count)
        return th_generalize(ctx, t);

    core_dict_t fixed;
    core_dict_init(&fixed, nullptr, nullptr);
    for (size_t i = 0; i < environment_count; i++) {
        th_type **vars = nullptr;
        size_t nvars = th_free_vars(ctx, environment[i], &vars);
        for (size_t j = 0; j < nvars; j++)
            core_dict_put(&fixed, vars[j], 1);
    }

    th_type **candidates = nullptr;
    size_t count = th_free_vars(ctx, t, &candidates);
    th_type **generalizable = th_alloc(ctx,
        (count ? count : 1) * sizeof(th_type *));
    size_t n_generalizable = 0;
    for (size_t i = 0; i < count; i++) {
        bool is_fixed = false;
        core_dict_get(&fixed, candidates[i], &is_fixed);
        if (!is_fixed)
            generalizable[n_generalizable++] = candidates[i];
    }
    core_dict_free(&fixed);
    return generalize_vars(ctx, t, generalizable, n_generalizable);
}
