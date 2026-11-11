#include "types/solver.h"
#include "lib/dict.h"
#include "lib/list.h"
#include "th/environment.h"

#define S(x) CORE_STRING_STATIC(x)

typedef struct {
    ty_pred_t pred;
    ty_why_t  why;
    bool      settled;
} pending_t;

typedef struct {
    uint32_t pred;
    uint32_t next;
} link_t;

typedef struct {
    const ir_instr_t *at;
    uint32_t          next;
} site_t;

typedef const ir_instr_t *ty_instr_cptr_t;

typedef struct {
    th_type *var;
    th_type *to;
    ty_why_t why;
} event_t;

typedef th_type *ty_type_ptr_t;

typedef struct {
    th_type *slot;
    th_type *item;
} nil_slot_t;

typedef struct {
    core_datalog_symbol_t trait;
    core_datalog_symbol_t method;
    th_type              *contract;
} contract_t;

core_list_decl(pending_t);
core_list_decl(contract_t);
core_list_decl(link_t);
core_list_decl(site_t);
core_list_decl(ty_instr_cptr_t);

core_list_decl(event_t);
core_list_decl(ty_failure_t);
core_list_decl(ty_type_ptr_t);
core_list_decl(nil_slot_t);

typedef struct {
    core_list_t(ty_type_ptr_t) vars;
    core_list_t(pending_t)     preds;
} scheme_t;

typedef enum : uint8_t {
    HOLDS,
    FAILS,
    OPEN,
} verdict_t;

typedef struct {
    th_type  *var;
    ty_pred_t pred;
    ty_why_t  why;
} bound_t;
core_list_decl(bound_t);

struct ty_solver {
    th_ctx                    *types;
    core_datalog_engine_t     *logic;
    core_datalog_relation_id_t requires;
    core_datalog_relation_id_t declared;
    core_datalog_relation_id_t method;
    core_datalog_relation_id_t impl;
    core_datalog_relation_id_t candidate;
    core_datalog_relation_id_t missing;
    core_datalog_relation_id_t implements;
    core_datalog_relation_id_t operators;
    bool                       dirty;
    core_dict_t                links_of;
    core_dict_t                origins;
    core_list_t(pending_t)     pending;
    core_list_t(uint32_t)      live;
    core_list_t(link_t)        links;
    core_list_t(event_t)       events;
    core_list_t(ty_failure_t)  failures;
    ty_why_t                   cause;
    core_list_t(ty_type_ptr_t) results;
    core_list_t(nil_slot_t)    nils;
    core_dict_t                schemes;
    core_dict_t                joined;
    core_dict_t                defaults;
    core_list_t(ty_type_ptr_t) open_vars;
    core_list_t(contract_t)    contracts;
    core_dict_t                rigid;
    core_list_t(bound_t)       bounds;
    core_dict_t                flow_roots;
    core_dict_t                instances;
    core_list_t(ty_instr_cptr_t) dispatches;
    core_dict_t                sites_of;
    core_list_t(site_t)        sites;
    core_dict_t                converting;
    core_list_t(ty_instr_cptr_t) conversions;
    ty_call_method_fn          methods;
    void                      *methods_context;
    ty_requirements_fn         requirements;
    void                      *requirements_context;
    ty_method_fn               fields;
    void                      *fields_context;
    ty_param_name_fn           param_names;
    void                      *param_names_context;
    ty_constant_index_fn       constant_index;
    void                      *constant_index_context;
    ty_coerce_fn               coerces;
    void                      *coerces_context;
    ty_omittable_fn            omittable;
    void                      *omittable_context;
};

static bool flow(ty_solver_t *s, th_type *slot, th_type *value, ty_why_t why);
static bool integral(ty_solver_t *s, th_type *t);
static th_type *renamed(ty_solver_t *s, th_type *t);
static bool same(ty_solver_t *s, th_type *a, th_type *b, ty_why_t why);
static void recheck(ty_solver_t *s, th_type *var);
static void recheck_because(ty_solver_t *s, th_type *var, ty_why_t why);

static void add_rule(core_datalog_engine_t *logic,
                     core_datalog_relation_id_t head,
                     const core_datalog_symbol_t *head_args,
                     const core_datalog_relation_id_t *body,
                     const core_datalog_symbol_t (*body_args)[2],
                     const bool *negated, size_t n)
{
    core_datalog_rule_builder_t *builder = core_datalog_rule_builder_new();

    core_datalog_rule_builder_head(builder, head, 2, head_args);
    for (size_t i = 0; i < n; i++) {
        int32_t arity = body_args[i][1] == CORE_DATALOG_SYMBOL_INVALID ? 1 : 2;

        core_datalog_rule_builder_add_literal(builder, body[i], arity,
                                              body_args[i], negated[i]);
    }
    core_datalog_add_rule(logic, core_datalog_rule_builder_finish(builder));
}

static void install_rules(ty_solver_t *s)
{
    core_datalog_engine_t *l  = s->logic;
    core_datalog_symbol_t  t  = core_datalog_variable(l, S("T"));
    core_datalog_symbol_t  tr = core_datalog_variable(l, S("Tr"));
    core_datalog_symbol_t  m  = core_datalog_variable(l, S("M"));
    core_datalog_symbol_t  no = CORE_DATALOG_SYMBOL_INVALID;
    core_datalog_symbol_t  pair[] = {t, tr};

    add_rule(l, s->missing, pair,
             (core_datalog_relation_id_t[]){s->candidate, s->requires,
                                            s->method},
             (const core_datalog_symbol_t[][2]){{t, no}, {tr, m}, {t, m}},
             (bool[]){false, false, true}, 3);
    add_rule(l, s->implements, pair,
             (core_datalog_relation_id_t[]){s->candidate, s->declared,
                                            s->missing},
             (const core_datalog_symbol_t[][2]){{t, no}, {tr, no}, {t, tr}},
             (bool[]){false, false, true}, 3);
    add_rule(l, s->implements, pair,
             (core_datalog_relation_id_t[]){s->impl},
             (const core_datalog_symbol_t[][2]){{t, tr}}, (bool[]){false}, 1);
}

ty_solver_t *ty_solver_new(th_ctx *types, core_datalog_engine_t *logic)
{
    core_datalog_engine_t *l = logic;

    if (!types || !l) {
        return nullptr;
    }

    ty_solver_t *s = core_alloc(ty_solver_t);

    s->types      = types;
    s->logic      = l;
    s->requires   = core_datalog_relation(l, S("ty.requires"), 2);
    s->declared   = core_datalog_relation(l, S("ty.declared"), 1);
    s->method     = core_datalog_relation(l, S("ty.method"), 2);
    s->impl       = core_datalog_relation(l, S("ty.impl"), 2);
    s->candidate  = core_datalog_relation(l, S("ty.candidate"), 1);
    s->missing    = core_datalog_relation(l, S("ty.missing"), 2);
    s->implements = core_datalog_relation(l, S("ty.implements"), 2);
    s->operators  = core_datalog_relation(l, S("ty.operator"), 4);
    s->pending    = core_list_new(pending_t);
    s->live       = core_list_new(uint32_t);
    s->links      = core_list_new(link_t);
    s->events     = core_list_new(event_t);
    s->failures   = core_list_new(ty_failure_t);
    s->results    = core_list_new(ty_type_ptr_t);
    s->nils       = core_list_new(nil_slot_t);
    core_dict_init(&s->links_of, nullptr, nullptr);
    core_dict_init(&s->origins, nullptr, nullptr);
    core_dict_init(&s->schemes, nullptr, nullptr);
    core_dict_init(&s->joined, nullptr, nullptr);
    core_dict_init(&s->defaults, nullptr, nullptr);
    s->open_vars = core_list_new(ty_type_ptr_t);
    s->contracts = core_list_new(contract_t);
    core_dict_init(&s->rigid, nullptr, nullptr);
    s->bounds = core_list_new(bound_t);
    core_dict_init(&s->flow_roots, nullptr, nullptr);
    core_dict_init(&s->instances, nullptr, nullptr);
    s->dispatches = core_list_new(ty_instr_cptr_t);
    core_dict_init(&s->sites_of, nullptr, nullptr);
    core_dict_init(&s->converting, nullptr, nullptr);
    s->sites       = core_list_new(site_t);
    s->conversions = core_list_new(ty_instr_cptr_t);
    install_rules(s);

    return s;
}

void ty_solver_free(ty_solver_t *s)
{
    if (!s) {
        return;
    }
    core_dict_foreach(&s->origins, entry) core_free(entry->value);
    core_dict_free(&s->origins);
    core_dict_foreach(&s->schemes, entry) {
        scheme_t *scheme = entry->value;

        core_list_free(scheme->vars);
        core_list_free(scheme->preds);
        core_free(scheme);
    }
    core_dict_free(&s->schemes);
    core_dict_free(&s->joined);
    core_dict_free(&s->defaults);
    core_list_free(s->open_vars);
    core_list_free(s->contracts);
    core_dict_free(&s->rigid);
    core_list_free(s->bounds);
    core_dict_free(&s->flow_roots);
    core_dict_free(&s->instances);
    core_list_free(s->dispatches);
    core_dict_free(&s->sites_of);
    core_dict_free(&s->converting);
    core_list_free(s->sites);
    core_list_free(s->conversions);
    core_list_free(s->nils);
    core_dict_free(&s->links_of);
    core_list_free(s->pending);
    core_list_free(s->live);
    core_list_free(s->links);
    core_list_free(s->events);
    core_list_free(s->failures);
    core_list_free(s->results);
    core_free(s);
}

th_ctx *ty_solver_types(ty_solver_t *s)
{
    return s ? s->types : nullptr;
}

static core_datalog_symbol_t key_of(ty_solver_t *s, th_type *t)
{
    core_string_t name = {0};

    switch (th_kind_of(t)) {
    case TH_NAMED:
        th_named_info(t, &name, nullptr, nullptr);
        break;
    case TH_BITINT:
        name = th_print(s->types, t);
        break;
    case TH_INDIRECTION:
        name = S("ref");
        break;
    case TH_SEQUENCE:
        name = S("array");
        break;
    case TH_RECORD:
        name = S("record");
        break;
    case TH_FUNCTION:
        name = S("function");
        break;
    case TH_SUM:
        name = S("union");
        break;
    default:
        name = th_print(s->types, t);
        break;
    }

    return core_datalog_constant(s->logic, name);
}

static core_datalog_symbol_t word(ty_solver_t *s, core_string_t text)
{
    return core_datalog_constant(s->logic, text);
}

bool ty_declare_trait(ty_solver_t *s, core_string_t trait,
                      const core_string_t *methods, size_t n)
{
    if (!s || !trait.data || (n && !methods)) {
        return false;
    }

    core_datalog_symbol_t tr = word(s, trait);
    bool                  ok = !n
                            || core_datalog_add_fact(s->logic, s->declared, 1,
                                                     &tr);

    for (size_t i = 0; ok && i < n; i++) {
        core_datalog_symbol_t row[] = {tr, word(s, methods[i])};

        ok = core_datalog_add_fact(s->logic, s->requires, 2, row);
    }
    s->dirty = true;

    return ok;
}

bool ty_declare_method(ty_solver_t *s, th_type *type, core_string_t method)
{
    if (!s || !type || !method.data) {
        return false;
    }

    core_datalog_symbol_t row[] = {key_of(s, th_resolve(type)),
                                   word(s, method)};

    s->dirty = true;

    return core_datalog_add_fact(s->logic, s->method, 2, row);
}

static bool exact_sequence(ty_solver_t *s, th_type *t)
{
    return th_kind_of(t) == TH_SEQUENCE
        && th_is_closed(s->types, th_resolve(th_seq_elem(t)));
}

bool ty_declare_impl(ty_solver_t *s, th_type *type, core_string_t trait)
{
    if (!s || !type || !trait.data) {
        return false;
    }

    type = th_resolve(type);

    core_datalog_symbol_t row[] = {exact_sequence(s, type)
                                       ? word(s, th_print(s->types, type))
                                       : key_of(s, type),
                                   word(s, trait)};

    s->dirty = true;

    return core_datalog_add_fact(s->logic, s->impl, 2, row);
}

bool ty_declare_operator(ty_solver_t *s, core_string_t op, th_type *left,
                         th_type *right, th_type *result)
{
    if (!s || !op.data || !left || !right || !result) {
        return false;
    }

    core_datalog_symbol_t row[] = {
        word(s, op), key_of(s, th_resolve(left)), key_of(s, th_resolve(right)),
        core_datalog_integer(s->logic, (int64_t)s->results.len)};

    core_list_push(s->results, result);
    s->dirty = true;

    return core_datalog_add_fact(s->logic, s->operators, 4, row);
}

th_type *ty_fresh(ty_solver_t *s, ty_origin_t origin)
{
    th_type *var = s ? th_var(s->types) : nullptr;

    if (var && (origin.value || origin.symbol)) {
        ty_origin_t *kept = core_alloc(ty_origin_t);

        *kept = origin;
        core_dict_put(&s->origins, var, kept);
    }

    return var;
}

static void fail(ty_solver_t *s, ty_fail_kind_t kind, ty_why_t why,
                 th_type *expected, th_type *actual, const ty_pred_t *pred)
{
    ty_failure_t failure = {.kind     = kind,
                            .why      = why,
                            .cause    = s->cause,
                            .expected = expected,
                            .actual   = actual};

    if (pred) {
        failure.pred = *pred;
    }
    core_list_push(s->failures, failure);
}

static bool rigid(ty_solver_t *s, th_type *t);

static th_type *root_var(ty_solver_t *s, th_type *t)
{
    th_type *last = nullptr;

    while (t && th_kind_of(t) == TH_VAR) {
        bool root = false;

        core_dict_get(&s->flow_roots, t, &root);
        if (root) {
            return t;
        }
        last = t;
        t    = th_var_ref(t);
    }

    return rigid(s, last) ? nullptr : last;
}

static bool unbound(th_type *t)
{
    return t && th_kind_of(t) == TH_VAR && !th_var_ref(t);
}

static bool rigid(ty_solver_t *s, th_type *t)
{
    bool found = false;

    if (unbound(t)) {
        core_dict_get(&s->rigid, t, &found);
    }

    return found;
}

static bool flexible(ty_solver_t *s, th_type *t)
{
    return unbound(t) && !rigid(s, t);
}


static uint32_t first_link(ty_solver_t *s, th_type *var)
{
    bool  found = false;
    void *value = core_dict_get(&s->links_of, var, &found);

    return found ? (uint32_t)((uintptr_t)value - 1) : UINT32_MAX;
}

static void link_pred(ty_solver_t *s, th_type *var, uint32_t pred)
{
    uint32_t head = first_link(s, var);

    core_list_push(s->links, ((link_t){.pred = pred, .next = head}));
    core_dict_put(&s->links_of, var, (void *)(uintptr_t)s->links.len);
}

static void move_links(ty_solver_t *s, th_type *from, th_type *to)
{
    for (uint32_t l = first_link(s, from); l != UINT32_MAX;
         l = s->links.data[l].next) {
        link_pred(s, to, s->links.data[l].pred);
    }
}

static void convert_at(ty_solver_t *s, const ir_instr_t *at)
{
    bool seen = false;

    if (!at) {
        return;
    }
    core_dict_get(&s->converting, (void *)at, &seen);
    if (!seen) {
        core_dict_put(&s->converting, (void *)at, (void *)(uintptr_t)1);
        core_list_push(s->conversions, at);
    }
}

static uint32_t first_site(ty_solver_t *s, th_type *root)
{
    bool  found = false;
    void *value = core_dict_get(&s->sites_of, root, &found);

    return found ? (uint32_t)((uintptr_t)value - 1) : UINT32_MAX;
}

static void flow_site(ty_solver_t *s, th_type *root, ty_why_t why)
{
    if (!root || !why.at) {
        return;
    }
    core_list_push(s->sites, ((site_t){.at = why.at,
                                       .next = first_site(s, root)}));
    core_dict_put(&s->sites_of, root, (void *)(uintptr_t)s->sites.len);
}

static void convert_sites(ty_solver_t *s, th_type *root)
{
    for (uint32_t i = first_site(s, root); i != UINT32_MAX;
         i = s->sites.data[i].next) {
        convert_at(s, s->sites.data[i].at);
    }
}

size_t ty_conversion_count(const ty_solver_t *s)
{
    return s ? s->conversions.len : 0;
}

const ir_instr_t *ty_conversion_at(const ty_solver_t *s, size_t i)
{
    return s && i < s->conversions.len ? s->conversions.data[i] : nullptr;
}

static void note(ty_solver_t *s, th_type *var, th_type *to, ty_why_t why)
{
    core_list_push(s->events, ((event_t){.var = var, .to = to, .why = why}));
}

static bool bind(ty_solver_t *s, th_type *var, th_type *to, ty_why_t why)
{
    if (th_occurs(s->types, var, to)) {
        fail(s, TY_FAIL_OCCURS, why, var, to, nullptr);
        return false;
    }
    if (!th_var_bind(var, to)) {
        return false;
    }
    note(s, var, to, why);

    th_type *target = th_resolve(to);

    if (unbound(target)) {
        move_links(s, var, target);
        core_dict_put(&s->joined, target, (void *)(uintptr_t)1);
    }
    else {
        recheck_because(s, var, why);
    }

    return true;
}

static bool widen(ty_solver_t *s, th_type *var, th_type *to, ty_why_t why)
{
    if (!th_var_rebind(var, to)) {
        return false;
    }
    convert_sites(s, var);
    convert_at(s, why.at);
    note(s, var, to, why);
    recheck_because(s, var, why);

    return true;
}

static bool same(ty_solver_t *s, th_type *a, th_type *b, ty_why_t why)
{
    a = th_resolve(a);
    b = th_resolve(b);

    if (a == b) {
        return true;
    }
    if (flexible(s, a)) {
        return bind(s, a, b, why);
    }
    if (flexible(s, b)) {
        return bind(s, b, a, why);
    }
    if (unbound(a) || unbound(b) || !th_same_shape(a, b)) {
        fail(s, TY_FAIL_MISMATCH, why, a, b, nullptr);
        return false;
    }
    for (size_t i = 0; i < th_child_count(a); i++) {
        th_type *x = th_child(a, i);
        th_type *y = th_child(b, i);

        if ((x || y) && (!x || !y || !same(s, x, y, why))) {
            if (!x || !y) {
                fail(s, TY_FAIL_MISMATCH, why, a, b, nullptr);
            }
            return false;
        }
    }

    return true;
}

static bool flow_nil(ty_solver_t *s, th_type *slot, th_type *target,
                     ty_why_t why)
{
    th_type *root = root_var(s, slot);

    if (!th_is_nil(target)) {
        convert_at(s, why.at);
    }
    if (flexible(s, target) && root && root != target) {
        return widen(s, root, th_option(s->types, target), why);
    }
    if (flexible(s, target)) {
        th_type *item = th_var(s->types);

        core_list_push(s->nils, ((nil_slot_t){.slot = target, .item = item}));

        return bind(s, target, th_option(s->types, item), why);
    }
    if (th_option_item(target) || th_is_nil(target)
        || (s->coerces
            && s->coerces(s->coerces_context, target, th_nil(s->types)))) {
        return true;
    }
    if (root) {
        return widen(s, root, th_option(s->types, target), why);
    }
    fail(s, TY_FAIL_NIL, why, target, th_nil(s->types), nullptr);

    return false;
}

static bool settle_nil(ty_solver_t *s, th_type *given)
{
    for (size_t i = 0; i < s->nils.len; i++) {
        nil_slot_t *n = &s->nils.data[i];

        if (n->slot && !th_var_ref(n->item) && th_resolve(n->slot) == given) {
            th_var_rebind(n->slot, th_nil(s->types));
            note(s, n->slot, th_nil(s->types), (ty_why_t){.rule = 6});
            n->slot = nullptr;

            return true;
        }
    }

    return false;
}

static th_type *record_shape(ty_solver_t *s, th_type *type)
{
    return th_fields_record(s->types, type);
}

static bool flow_record(ty_solver_t *s, th_type *target, th_type *given,
                        ty_why_t why)
{
    th_type *shape   = record_shape(s, target);
    bool     variant = false;

    th_named_info(th_resolve(target), nullptr, &variant, nullptr);
    if (variant && shape && th_record_len(given) == 1) {
        const th_member *arm     = th_record_member(given, 0);
        th_type         *payload = th_record_lookup(shape, arm->name);

        if (!payload) {
            fail(s, TY_FAIL_MISMATCH, why, target, given, nullptr);
            return false;
        }

        return flow(s, payload, arm->type, why);
    }
    size_t omitted = 0;

    for (size_t i = 0; shape && i < th_record_len(shape); i++) {
        core_string_t name = th_record_member(shape, i)->name;

        if (!th_record_lookup(given, name)) {
            omitted += s->omittable
                    && s->omittable(s->omittable_context, target, name)
                         ? 1
                         : th_record_len(shape) + 1;
        }
    }
    if (!shape
        || th_record_len(shape) != th_record_len(given) + omitted) {
        fail(s, TY_FAIL_MISMATCH, why, target, given, nullptr);
        return false;
    }
    for (size_t i = 0; i < th_record_len(given); i++) {
        const th_member *member = th_record_member(given, i);
        th_type         *field  = th_record_lookup(shape, member->name);

        if (!field) {
            fail(s, TY_FAIL_MISMATCH, why, target, given, nullptr);
            return false;
        }
        if (!flow(s, field, member->type, why)) {
            return false;
        }
    }

    return true;
}

static bool flow(ty_solver_t *s, th_type *slot, th_type *value, ty_why_t why)
{
    th_type *target = th_resolve(slot);
    th_type *given  = th_resolve(value);
    th_type *root   = root_var(s, slot);

    flow_site(s, root, why);
    if (target == given) {
        return true;
    }

    th_type *atomic = target && given
                          && th_builtin_kind_of(given) != TH_BUILTIN_ATOMIC
                        ? th_builtin_item(s->types, target, TH_BUILTIN_ATOMIC, 0)
                        : nullptr;

    if (atomic) {
        convert_at(s, why.at);
        return flow(s, atomic, value, why);
    }
    if (given && th_is_nil(given)) {
        return flow_nil(s, slot, target, why);
    }
    if (target && given && th_builtin_kind_of(target) == TH_BUILTIN_ANY) {
        convert_at(s, why.at);
        return true;
    }
    if (flexible(s, given) && th_option_item(target)) {
        return ty_require(s,
                          (ty_pred_t){.kind    = TY_PRED_FLOW,
                                      .name    = CORE_STRING_STATIC("flow"),
                                      .subject = given,
                                      .other   = target},
                          why);
    }
    if (flexible(s, target) && !flexible(s, given)) {
        th_type *slot_root = root_var(s, slot);

        if (slot_root && slot_root != target) {
            return widen(s, slot_root, given, why)
                && flow(s, slot_root, target, why);
        }
    }
    if (flexible(s, target)) {
        if (!bind(s, target, given, why)) {
            return false;
        }
        core_dict_put(&s->flow_roots, target, (void *)(uintptr_t)1);
        flow_site(s, target, why);
        return true;
    }
    if (flexible(s, target) || flexible(s, given)) {
        return same(s, target, given, why);
    }
    if (root && th_kind_of(target) == TH_RECORD
        && th_kind_of(given) == TH_NAMED && record_shape(s, given)) {
        return flow_record(s, given, target, why)
            && widen(s, root, given, why);
    }
    if (th_kind_of(given) == TH_RECORD && th_kind_of(target) != TH_RECORD
        && !th_option_item(target)) {
        convert_at(s, why.at);
        return flow_record(s, target, given, why);
    }
    if (th_kind_of(given) == TH_RECORD && th_kind_of(target) == TH_RECORD) {
        return flow_record(s, target, given, why);
    }

    th_type *inner = th_option_item(target);
    th_type *item  = th_option_item(given);

    if (inner) {
        if (!item) {
            convert_at(s, why.at);
        }
        return flow(s, inner, item ? item : given, why);
    }
    if (th_is_nil(target) && settle_nil(s, given)) {
        return true;
    }
    if (th_is_nil(target) || item) {
        if (!root) {
            fail(s, TY_FAIL_NIL, why, target, given, nullptr);
            return false;
        }
        if (th_is_nil(target)) {
            return widen(s, root,
                         item ? given : th_option(s->types, given), why);
        }

        return widen(s, root, th_option(s->types, target), why)
            && same(s, target, item, why);
    }
    if (integral(s, target) && integral(s, given)) {
        convert_at(s, why.at);
        return true;
    }
    if (s->coerces && s->coerces(s->coerces_context, target, given)) {
        convert_at(s, why.at);
        return true;
    }
    return same(s, target, given, why);
}

bool ty_equate(ty_solver_t *s, th_type *a, th_type *b, ty_why_t why)
{
    return s && a && b && same(s, a, b, why);
}

bool ty_unify(ty_solver_t *s, th_type *slot, th_type *value, ty_why_t why)
{
    return s && slot && value && flow(s, slot, value, why);
}

static th_type *with_nil(ty_solver_t *s, th_type *t)
{
    return th_is_nil(t) || th_option_item(t) ? t : th_option(s->types, t);
}

th_type *ty_join(ty_solver_t *s, th_type *a, th_type *b, ty_why_t why)
{
    if (!s || !a || !b) {
        return nullptr;
    }

    th_type *x = th_resolve(a);
    th_type *y = th_resolve(b);

    if (th_is_nil(x)) {
        return with_nil(s, y);
    }
    if (th_is_nil(y)) {
        return with_nil(s, x);
    }

    th_type *joined = th_var(s->types);

    if (!flow(s, joined, x, why) || !flow(s, joined, y, why)) {
        return nullptr;
    }

    return joined;
}

static void run_logic(ty_solver_t *s)
{
    if (s->dirty) {
        core_datalog_run(s->logic);
        s->dirty = false;
    }
}

static bool candidate(ty_solver_t *s, core_datalog_symbol_t key)
{
    if (!core_datalog_contains(s->logic, s->candidate, &key)) {
        core_datalog_add_fact(s->logic, s->candidate, 1, &key);
        s->dirty = true;
    }
    run_logic(s);

    return true;
}

static bool implements(ty_solver_t *s, core_datalog_symbol_t key,
                       core_datalog_symbol_t trait)
{
    core_datalog_symbol_t row[] = {key, trait};

    candidate(s, key);

    return core_datalog_contains(s->logic, s->implements, row);
}

static bool integral(ty_solver_t *s, th_type *t)
{
    t = th_resolve(t);
    if (!t || th_kind_of(t) == TH_VAR) {
        return false;
    }
    run_logic(s);

    return implements(s, key_of(s, t), word(s, CORE_STRING_STATIC("Some_Integer")));
}

typedef struct {
    ty_solver_t          *s;
    core_datalog_symbol_t key;
    core_datalog_symbol_t want;
    core_datalog_symbol_t left;
    core_datalog_symbol_t right;
    int64_t               found;
} scan_t;

static bool operator_row(const core_datalog_symbol_t *tuple, int32_t arity,
                         void *context)
{
    scan_t *scan = context;

    (void)arity;
    if (tuple[0] == scan->want && tuple[1] == scan->left
        && tuple[2] == scan->right) {
        core_datalog_int_result_t index =
            core_datalog_symbol_integer(scan->s->logic, tuple[3]);

        scan->found = index.is_ok ? index.result.value : -1;
        return false;
    }

    return true;
}

static th_type *bound_contract(ty_solver_t *s, th_type *var,
                               core_string_t method)
{
    core_datalog_symbol_t want = word(s, method);

    for (uint32_t l = first_link(s, var); l != UINT32_MAX;
         l = s->links.data[l].next) {
        const pending_t *bound = &s->pending.data[s->links.data[l].pred];

        if (bound->pred.kind != TY_PRED_IMPLEMENTS) {
            continue;
        }

        core_datalog_symbol_t trait = word(s, bound->pred.name);

        for (size_t i = 0; i < s->contracts.len; i++) {
            if (s->contracts.data[i].trait == trait
                && s->contracts.data[i].method == want) {
                return s->contracts.data[i].contract;
            }
        }
    }

    return nullptr;
}

static th_type *field_of(ty_solver_t *s, th_type *type, core_string_t name)
{
    th_type *shape = record_shape(s, type);
    th_type *found = shape ? th_record_lookup(shape, name) : nullptr;

    return found || !s->fields ? found
                               : s->fields(s->fields_context, type, name);
}


static th_type *element_of(ty_solver_t *s, th_type *type)
{
    if (th_kind_of(type) == TH_SEQUENCE) {
        return th_seq_elem(type);
    }

    th_env_method_t method = {0};
    core_string_t   trait  = {0};
    th_type        *found  = s->methods
                               ? s->methods(s->methods_context, type,
                                            CORE_STRING_STATIC("__init"),
                                            nullptr)
                               : nullptr;

    if (!found
        && th_env_method_resolve_by_name(s->types, type,
                                         CORE_STRING_STATIC("__init"),
                                         &method, &trait)
               == TH_ENV_METHOD_FOUND) {
        found = method.signature;
    }
    if (!found) {
        return nullptr;
    }
    if (th_kind_of(th_resolve(found)) == TH_BINDER) {
        found = th_binder_body(th_resolve(found));
    }

    th_type *signature = renamed(s, found);

    if (th_kind_of(signature) != TH_FUNCTION
        || th_func_nparams(signature) != 2
        || !same(s, th_func_param(signature, 0)->type, type,
                 (ty_why_t){.rule = 29})) {
        return nullptr;
    }

    th_type *seed = th_resolve(th_func_param(signature, 1)->type);

    return seed && th_kind_of(seed) == TH_SEQUENCE ? th_seq_elem(seed)
                                                   : nullptr;
}

static bool flow_call(ty_solver_t *s, th_type *shape, th_type *signature,
                      ty_why_t why)
{
    shape     = th_resolve(shape);
    signature = th_resolve(signature);
    if (!shape || !signature || th_kind_of(shape) != TH_FUNCTION
        || th_kind_of(signature) != TH_FUNCTION) {
        return false;
    }
    for (size_t i = 0; i < th_func_nparams(shape); i++) {
        const th_param *formal = i < th_func_nparams(signature)
                                   ? th_func_param(signature, i)
                                   : th_func_rest(signature);

        if (!formal
            || !flow(s, formal->type, th_func_param(shape, i)->type,
                     (ty_why_t){.at = why.at, .rule = 40})) {
            return false;
        }
    }

    th_type *failure = th_func_error(shape);

    if (failure && !th_is_nil(th_resolve(failure))
        && !flow(s, failure,
                 th_func_error(signature) ? th_func_error(signature)
                                          : th_nil(s->types),
                 why)) {
        return false;
    }

    return flow(s, th_func_ret(shape), th_func_ret(signature), why);
}

static const th_param *named_param(ty_solver_t *s, pending_t *p,
                                   th_type *signature, core_string_t name)
{
    th_type *receiver = th_resolve(p->pred.subject);

    for (size_t i = 0; s->param_names && i < th_func_nparams(signature); i++) {
        if (core_string_eq(s->param_names(s->param_names_context, receiver,
                                          p->pred.name, i),
                           name)) {
            return th_func_param(signature, i);
        }
    }

    return nullptr;
}

static bool flow_keywords(ty_solver_t *s, pending_t *p, th_type *shape,
                          th_type *signature)
{
    shape     = th_resolve(shape);
    signature = th_resolve(signature);
    for (size_t i = 0; i < th_func_nkw(shape); i++) {
        const th_kwparam *given = th_func_kw(shape, i);
        const th_kwparam *k     = th_func_kw_lookup(signature, given->name);
        const th_param   *param = k ? nullptr
                                    : named_param(s, p, signature, given->name);
        th_type          *want  = k ? k->type : param ? param->type : nullptr;

        if (want && !flow(s, want, given->type, p->why)) {
            return false;
        }
    }

    return true;
}

static bool dispatch_as(ty_solver_t *s, pending_t *p, th_type *instance)
{
    bool seen = false;
    bool site = p->why.at && !p->pred.bound;

    core_dict_get(&s->instances, (void *)p->why.at, &seen);
    if (site && !seen) {
        core_list_push(s->dispatches, p->why.at);
    }
    if (site) {
        core_dict_put(&s->instances, (void *)p->why.at, instance);
    }

    return !p->pred.other
        || (flow_call(s, p->pred.other, instance, p->why)
            && flow_keywords(s, p, p->pred.other, instance));
}

static th_type *tuple_member(th_type *tuple, size_t index)
{
    const th_arg *arg = tuple ? th_named_arg(th_resolve(tuple), index)
                              : nullptr;

    return arg && arg->kind == TH_ARG_TYPE ? arg->type : nullptr;
}

static bool dispatch_call(ty_solver_t *s, pending_t *p, th_type *signature)
{
    th_type *receiver = th_resolve(p->pred.subject);
    size_t   count    = s->requirements
        ? s->requirements(s->requirements_context, receiver, p->pred.name,
                          p->pred.other, signature, nullptr, 0)
        : 0;

    if (!count) {
        return dispatch_as(s, p, renamed(s, signature));
    }

    th_trait_requirement *requirements =
        core_alloc_array(th_trait_requirement, count);
    th_type **items = core_alloc_array(th_type *, count + 1);

    count    = s->requirements(s->requirements_context, receiver,
                               p->pred.name, p->pred.other, signature,
                               requirements, count);
    items[0] = signature;
    for (size_t i = 0; i < count; i++) {
        th_type *pair[] = {
            requirements[i].subject,
            requirements[i].target ? requirements[i].target
                                   : th_nil(s->types),
        };

        items[i + 1] = th_tuple(s->types, pair, 2);
    }

    th_type *fresh = renamed(s, th_tuple(s->types, items, count + 1));
    bool     ok    = dispatch_as(s, p, tuple_member(fresh, 0));

    for (size_t i = 0; ok && i < count; i++) {
        th_type *pair = tuple_member(fresh, i + 1);

        if (requirements[i].kind == TH_CON_IMPLEMENTS) {
            ty_require(s,
                       (ty_pred_t){.kind    = TY_PRED_IMPLEMENTS,
                                   .name    = requirements[i].iface,
                                   .subject = tuple_member(pair, 0)},
                       p->why);
        }
        else if (requirements[i].kind == TH_CON_ONE_OF) {
            ty_require(s,
                       (ty_pred_t){.kind    = TY_PRED_ONE_OF,
                                   .subject = tuple_member(pair, 0),
                                   .other   = tuple_member(pair, 1)},
                       p->why);
        }
    }
    core_free(requirements);
    core_free(items);

    return ok;
}

static th_type *bound_method(ty_solver_t *s, th_type *var, core_string_t name)
{
    for (size_t b = 0; b < s->bounds.len; b++) {
        bound_t *bound = &s->bounds.data[b];

        if (th_resolve(bound->var) == var
            && bound->pred.kind == TY_PRED_DISPATCH
            && core_string_eq(bound->pred.name, name)) {
            return bound->pred.other;
        }
    }

    return nullptr;
}

size_t ty_dispatch_count(const ty_solver_t *s)
{
    return s ? s->dispatches.len : 0;
}

const ir_instr_t *ty_dispatch_at(const ty_solver_t *s, size_t i)
{
    return s && i < s->dispatches.len ? s->dispatches.data[i] : nullptr;
}

th_type *ty_dispatch_instance(ty_solver_t *s, const ir_instr_t *at)
{
    bool     found    = false;
    th_type *instance = s && at ? core_dict_get(&s->instances, (void *)at,
                                                &found)
                                : nullptr;

    return found ? instance : nullptr;
}

static verdict_t judge(ty_solver_t *s, pending_t *p)
{
    th_type *subject = th_resolve(p->pred.subject);

    if (unbound(subject)) {
        th_type *method = p->pred.kind == TY_PRED_DISPATCH && !p->pred.bound
                            ? bound_method(s, subject, p->pred.name)
                            : nullptr;

        if (p->pred.bound) {
            return OPEN;
        }
        if (method) {
            return dispatch_as(s, p, method) ? HOLDS : FAILS;
        }

        th_type *contract = p->pred.kind == TY_PRED_DISPATCH && p->pred.other
                              ? bound_contract(s, subject, p->pred.name)
                              : nullptr;

        if (!contract && p->pred.kind == TY_PRED_DISPATCH && s->methods
            && rigid(s, subject)) {
            contract = s->methods(s->methods_context, subject, p->pred.name,
                                  p->pred.other);
        }
        if (!contract) {
            return OPEN;
        }

        return dispatch_call(s, p, contract) ? HOLDS : FAILS;
    }

    scan_t scan = {.s = s, .key = key_of(s, subject),
                   .want = p->pred.name.data ? word(s, p->pred.name) : 0,
                   .found = -1};

    switch (p->pred.kind) {
    case TY_PRED_IMPLEMENTS:
        if (th_kind_of(subject) == TH_SEQUENCE
            && !implements(s, scan.key, scan.want)) {
            if (!exact_sequence(s, subject)) {
                return OPEN;
            }

            scan.key = word(s, th_print(s->types, subject));
        }

        return implements(s, scan.key, scan.want)
                    || (th_ctx_environment(s->types)
                        && th_env_implements(s->types, subject,
                                             p->pred.name))
                 ? HOLDS
                 : FAILS;
    case TY_PRED_DISPATCH: {
        uint64_t position = p->pred.index;
        bool     tuple    = th_builtin_kind_of(subject) == TH_BUILTIN_TUPLE;
        bool     indexed  = tuple
            && (p->pred.positional
                || (core_string_eq(p->pred.name,
                                   CORE_STRING_STATIC("__read"))
                    && s->constant_index && p->why.at
                    && s->constant_index(s->constant_index_context,
                                         p->why.at, &position)));

        if (indexed) {
            return position < th_named_nargs(subject)
                        && flow(s, th_named_arg(subject, (size_t)position)->type,
                                th_func_ret(th_resolve(p->pred.other)),
                                p->why)
                     ? HOLDS
                     : FAILS;
        }

        th_type *signature = s->methods
                               ? s->methods(s->methods_context, subject,
                                            p->pred.name, p->pred.other)
                               : nullptr;

        return signature && dispatch_call(s, p, signature) ? HOLDS : FAILS;
    }
    case TY_PRED_CONDITION:
        return same(s, th_name(s->types, CORE_STRING_STATIC("bool")), subject,
                    p->why)
                 ? HOLDS
                 : FAILS;
    case TY_PRED_ONE_OF: {
        th_type *members = p->pred.other ? th_resolve(p->pred.other) : nullptr;

        for (size_t i = 0; members && i < th_sum_len(members); i++) {
            if (th_same_type(subject, th_sum_arm(members, i))) {
                return HOLDS;
            }
        }

        return FAILS;
    }
    case TY_PRED_ELEMENT: {
        if (p->pred.positional) {
            return th_kind_of(subject) == TH_SEQUENCE
                        && same(s, th_seq_elem(subject), p->pred.result,
                                p->why)
                     ? HOLDS
                     : FAILS;
        }

        th_type *element = element_of(s, subject);

        if (element && p->pred.other) {
            th_type *pair = th_resolve(element);

            if (th_builtin_kind_of(pair) == TH_BUILTIN_TUPLE
                && th_named_nargs(pair) == 2) {
                return flow(s, th_named_arg(pair, 0)->type, p->pred.result,
                            p->why)
                            && flow(s, th_named_arg(pair, 1)->type,
                                    p->pred.other, p->why)
                         ? HOLDS
                         : FAILS;
            }

            return flow(s, element, p->pred.result, p->why)
                        && same(s, p->pred.other, th_nil(s->types), p->why)
                     ? HOLDS
                     : FAILS;
        }

        return element && flow(s, element, p->pred.result, p->why) ? HOLDS
                                                                   : FAILS;
    }
    case TY_PRED_FLOW:
        if (p->pred.positional && unbound(th_resolve(p->pred.other))) {
            return OPEN;
        }

        return flow(s, p->pred.other, subject, p->why) ? HOLDS : FAILS;
    case TY_PRED_REFERENT: {
        if (th_kind_of(subject) != TH_INDIRECTION) {
            return FAILS;
        }

        th_type *held = th_resolve(th_ind_referent(subject));

        if (p->pred.other && unbound(held)) {
            return OPEN;
        }

        th_type *item = th_builtin_item(s->types, held, TH_BUILTIN_ATOMIC, 0);

        if (item) {
            convert_at(s, p->why.at);
        }

        return same(s, p->pred.result, item ? item : held, p->why) ? HOLDS
                                                                   : FAILS;
    }
    case TY_PRED_FIELD: {
        th_type *field = p->pred.positional
                           ? th_tuple_element(subject, p->pred.index, nullptr)
                           : field_of(s, subject, p->pred.name);
        th_type *item  = field && p->why.at
                             && p->why.at->op == IR_OP_EXTRACT
                           ? th_builtin_item(s->types, th_resolve(field),
                                             TH_BUILTIN_ATOMIC, 0)
                           : nullptr;

        if (item) {
            convert_at(s, p->why.at);
            field = item;
        }

        return field && flow(s, p->pred.result, field, p->why) ? HOLDS
                                                               : FAILS;
    }
    case TY_PRED_OPERATOR: {
        th_type *other = th_resolve(p->pred.other);

        if (unbound(other)) {
            return OPEN;
        }
        scan.left  = scan.key;
        scan.right = key_of(s, other);
        run_logic(s);
        core_datalog_query(s->logic, s->operators, operator_row, &scan);
        if (scan.found < 0 || (size_t)scan.found >= s->results.len) {
            return FAILS;
        }

        return flow(s, p->pred.result, s->results.data[scan.found], p->why)
                 ? HOLDS
                 : FAILS;
    }
    }

    return OPEN;
}

static void settle(ty_solver_t *s, uint32_t index)
{
    pending_t *p = &s->pending.data[index];

    if (p->settled) {
        return;
    }

    verdict_t verdict = judge(s, p);

    p = &s->pending.data[index];
    if (verdict == OPEN) {
        return;
    }
    p->settled = true;
    if (verdict == FAILS) {
        fail(s, TY_FAIL_PREDICATE, p->why, nullptr,
             th_resolve(p->pred.subject), &p->pred);
    }
}

static void recheck_because(ty_solver_t *s, th_type *var, ty_why_t why)
{
    ty_why_t outer = s->cause;

    s->cause = why;
    recheck(s, var);
    s->cause = outer;
}

static void recheck(ty_solver_t *s, th_type *var)
{
    for (uint32_t l = first_link(s, var); l != UINT32_MAX;
         l = s->links.data[l].next) {
        settle(s, s->links.data[l].pred);
    }
}

bool ty_require(ty_solver_t *s, ty_pred_t pred, ty_why_t why)
{
    if (!s || !pred.subject
        || (!pred.name.data
            && !(pred.kind == TY_PRED_FIELD && pred.positional)
            && !(pred.kind == TY_PRED_ONE_OF && pred.other)
            && pred.kind != TY_PRED_CONDITION)
        || (pred.kind == TY_PRED_OPERATOR && (!pred.other || !pred.result))
        || ((pred.kind == TY_PRED_FIELD || pred.kind == TY_PRED_ELEMENT
             || pred.kind == TY_PRED_REFERENT)
            && !pred.result)) {
        return false;
    }

    uint32_t index = (uint32_t)s->pending.len;
    size_t   before = s->failures.len;

    core_list_push(s->pending, ((pending_t){.pred = pred, .why = why}));
    core_list_push(s->live, index);

    th_type *subject = th_resolve(pred.subject);
    th_type *other   = pred.other ? th_resolve(pred.other) : nullptr;

    if (unbound(subject)) {
        link_pred(s, subject, index);
    }
    if (unbound(other)) {
        link_pred(s, other, index);
    }
    settle(s, index);

    return s->failures.len == before;
}

th_type *ty_refine(ty_solver_t *s, th_type *t, th_type *to, ty_why_t why)
{
    if (!s || !t || !to) {
        return nullptr;
    }

    th_type *from = th_resolve(t);
    th_type *item = th_option_item(from);
    th_type *want = th_resolve(to);

    if (item && th_is_nil(want)) {
        return want;
    }
    if (item && same(s, item, want, why)) {
        return item;
    }
    if (!item && same(s, from, want, why)) {
        return from;
    }
    fail(s, TY_FAIL_REFINE, why, from, want, nullptr);

    return nullptr;
}

static void default_nils(ty_solver_t *s)
{
    for (size_t i = 0; i < s->nils.len; i++) {
        nil_slot_t *n = &s->nils.data[i];

        bool joined = false;

        core_dict_get(&s->joined, n->item, &joined);
        if (n->slot && !th_var_ref(n->item) && !joined) {
            th_var_rebind(n->slot, th_nil(s->types));
            note(s, n->slot, th_nil(s->types), (ty_why_t){.rule = 6});
        }
        n->slot = nullptr;
    }
}

static void free_vars(th_type *t, core_list_t(ty_type_ptr_t) *out);

bool ty_declare_bound(ty_solver_t *s, th_type *var, ty_pred_t bound,
                      ty_why_t why)
{
    if (!s || !var) {
        return false;
    }
    bound.subject = var;
    bound.bound   = true;
    core_list_push(s->bounds, ((bound_t){.var = var, .pred = bound,
                                         .why = why}));
    ty_require(s, bound, why);

    return true;
}

void ty_declare_rigid(ty_solver_t *s, th_type *t)
{
    core_list_t(ty_type_ptr_t) vars = core_list_new(ty_type_ptr_t);

    free_vars(t, &vars);
    for (size_t i = 0; i < vars.len; i++) {
        if (th_var_name(vars.data[i]).u8_bytes) {
            core_dict_put(&s->rigid, vars.data[i], (void *)(uintptr_t)1);
        }
    }
    core_list_free(vars);
}

void ty_solver_set_methods(ty_solver_t *s, ty_call_method_fn methods,
                           void *context)
{
    if (s) {
        s->methods         = methods;
        s->methods_context = context;
    }
}

void ty_solver_set_requirements(ty_solver_t *s,
                                ty_requirements_fn requirements,
                                void *context)
{
    if (s) {
        s->requirements         = requirements;
        s->requirements_context = context;
    }
}

void ty_solver_set_fields(ty_solver_t *s, ty_method_fn fields, void *context)
{
    if (s) {
        s->fields         = fields;
        s->fields_context = context;
    }
}

void ty_solver_set_coercion(ty_solver_t *s, ty_coerce_fn coerces,
                            void *context)
{
    if (s) {
        s->coerces         = coerces;
        s->coerces_context = context;
    }
}

void ty_solver_set_param_names(ty_solver_t *s, ty_param_name_fn names,
                               void *context)
{
    if (s) {
        s->param_names         = names;
        s->param_names_context = context;
    }
}

void ty_solver_set_constant_index(ty_solver_t *s, ty_constant_index_fn index,
                                  void *context)
{
    if (s) {
        s->constant_index         = index;
        s->constant_index_context = context;
    }
}

void ty_solver_set_omittable(ty_solver_t *s, ty_omittable_fn omittable,
                             void *context)
{
    if (s) {
        s->omittable         = omittable;
        s->omittable_context = context;
    }
}

bool ty_declare_trait_method(ty_solver_t *s, core_string_t trait,
                             core_string_t method, th_type *contract)
{
    if (!s || !trait.data || !method.data || !contract) {
        return false;
    }
    core_list_push(s->contracts, ((contract_t){.trait    = word(s, trait),
                                               .method   = word(s, method),
                                               .contract = contract}));

    return true;
}

bool ty_declare_default(ty_solver_t *s, core_string_t trait, th_type *type)
{
    if (!s || !trait.data || !type) {
        return false;
    }
    core_dict_put(&s->defaults, (void *)((uintptr_t)word(s, trait) + 1), type);

    return true;
}

static void apply_flow_defaults(ty_solver_t *s)
{
    for (uint32_t k = 0; k < s->live.len; k++) {
        uint32_t i = s->live.data[k];
        pending_t *p = &s->pending.data[i];

        if (p->settled || p->pred.kind != TY_PRED_FLOW
            || !flexible(s, th_resolve(p->pred.subject))) {
            continue;
        }

        th_type *item = th_option_item(th_resolve(p->pred.other));

        if (item) {
            same(s, item, p->pred.subject, p->why);
        }
    }
}

static bool member_fits(ty_solver_t *s, th_type *subject, th_type *member)
{
    for (uint32_t k = 0; k < s->live.len; k++) {
        uint32_t i = s->live.data[k];
        pending_t copy = s->pending.data[i];

        if (copy.settled || copy.pred.kind != TY_PRED_IMPLEMENTS
            || th_resolve(copy.pred.subject) != subject) {
            continue;
        }
        copy.pred.subject = member;
        if (judge(s, &copy) != HOLDS) {
            return false;
        }
    }

    return true;
}

static void apply_member_defaults(ty_solver_t *s)
{
    for (uint32_t k = 0; k < s->live.len; k++) {
        uint32_t i = s->live.data[k];
        pending_t *p       = &s->pending.data[i];
        th_type   *subject = th_resolve(p->pred.subject);
        th_type   *members = p->pred.other ? th_resolve(p->pred.other)
                                           : nullptr;
        th_type   *chosen  = nullptr;
        size_t     fits    = 0;

        if (p->settled || p->pred.kind != TY_PRED_ONE_OF
            || !flexible(s, subject) || !members) {
            continue;
        }
        for (size_t m = 0; m < th_sum_len(members); m++) {
            if (member_fits(s, subject, th_sum_arm(members, m))) {
                chosen = th_sum_arm(members, m);
                fits++;
            }
        }
        if (fits == 1) {
            flow(s, subject, chosen, s->pending.data[i].why);
        }
    }
}

static void apply_defaults(ty_solver_t *s)
{
    apply_flow_defaults(s);
    apply_member_defaults(s);
    for (uint32_t k = 0; k < s->live.len; k++) {
        uint32_t i = s->live.data[k];
        pending_t *p = &s->pending.data[i];

        if (!p->settled && p->pred.kind == TY_PRED_REFERENT && p->pred.other
            && unbound(th_resolve(p->pred.other))) {
            p->settled = true;
            same(s, p->pred.result, p->pred.other, p->why);
            continue;
        }
        if (!p->settled && p->pred.kind == TY_PRED_FLOW
            && p->pred.positional && unbound(th_resolve(p->pred.other))) {
            p->settled = true;
            flow(s, p->pred.other, p->pred.subject, p->why);
            continue;
        }
        if (p->settled
            || (p->pred.kind != TY_PRED_IMPLEMENTS
                && p->pred.kind != TY_PRED_ELEMENT)
            || !flexible(s, th_resolve(p->pred.subject))) {
            continue;
        }

        uintptr_t trait = (uintptr_t)word(s, p->pred.name) + 1;
        bool      found = false;
        th_type  *type  = core_dict_get(&s->defaults, (void *)trait, &found);

        if (found) {
            flow(s, p->pred.subject, renamed(s, type), p->why);
        }
    }
    th_type *boolean = th_name(s->types, CORE_STRING_STATIC("bool"));

    for (uint32_t k = 0; k < s->live.len; k++) {
        uint32_t i = s->live.data[k];
        pending_t *p       = &s->pending.data[i];
        th_type   *subject = th_resolve(p->pred.subject);

        if (!p->settled && p->pred.kind == TY_PRED_CONDITION
            && flexible(s, subject) && member_fits(s, subject, boolean)) {
            flow(s, p->pred.subject, boolean, p->why);
        }
    }
}

static bool bounded(ty_solver_t *s, th_type *var)
{
    for (size_t i = 0; i < s->bounds.len; i++) {
        if (th_resolve(s->bounds.data[i].var) == var) {
            return true;
        }
    }

    return false;
}

static void fail_unbounded_requirements(ty_solver_t *s)
{
    for (uint32_t k = 0; k < s->live.len; k++) {
        uint32_t i = s->live.data[k];
        pending_t *p       = &s->pending.data[i];
        th_type   *subject = th_resolve(p->pred.subject);

        if (p->settled || p->pred.bound || p->pred.kind != TY_PRED_IMPLEMENTS
            || !rigid(s, subject) || bounded(s, subject)) {
            continue;
        }
        p->settled = true;
        fail(s, TY_FAIL_PREDICATE, p->why, nullptr, subject, &p->pred);
    }
}

static void drop_settled(ty_solver_t *s)
{
    uint32_t kept = 0;

    for (uint32_t k = 0; k < s->live.len; k++) {
        uint32_t i = s->live.data[k];

        if (!s->pending.data[i].settled) {
            s->live.data[kept++] = i;
        }
    }
    s->live.len = kept;
}

bool ty_solve(ty_solver_t *s)
{
    if (!s) {
        return false;
    }
    for (uint32_t k = 0; k < s->live.len; k++) {
        settle(s, s->live.data[k]);
    }
    apply_defaults(s);
    for (uint32_t k = 0; k < s->live.len; k++) {
        settle(s, s->live.data[k]);
    }
    fail_unbounded_requirements(s);
    default_nils(s);
    drop_settled(s);

    return s->failures.len == 0;
}

static void free_vars(th_type *t, core_list_t(ty_type_ptr_t) *out)
{
    t = th_resolve(t);
    if (!t) {
        return;
    }
    if (unbound(t)) {
        for (size_t i = 0; i < out->len; i++) {
            if (out->data[i] == t) {
                return;
            }
        }
        core_list_push(*out, t);
        return;
    }
    for (size_t i = 0; i < th_child_count(t); i++) {
        free_vars(th_child(t, i), out);
    }
}

static bool mentions(const pending_t *p, const core_list_t(ty_type_ptr_t) *vars)
{
    core_list_t(ty_type_ptr_t) seen = core_list_new(ty_type_ptr_t);
    bool                       hit  = false;

    free_vars(p->pred.subject, &seen);
    if (p->pred.other) {
        free_vars(p->pred.other, &seen);
    }
    if (p->pred.result) {
        free_vars(p->pred.result, &seen);
    }
    for (size_t i = 0; !hit && i < seen.len; i++) {
        for (size_t k = 0; !hit && k < vars->len; k++) {
            hit = seen.data[i] == vars->data[k];
        }
    }
    core_list_free(seen);

    return hit;
}

static th_type *swap(ty_solver_t *s, th_type *t,
                     const core_list_t(ty_type_ptr_t) *from, th_type **to)
{
    return t ? th_type_clone_replacing(s->types, t,
                                       (const th_type *const *)from->data, to,
                                       from->len)
             : nullptr;
}

static pending_t swap_pred(ty_solver_t *s, const pending_t *p,
                           const core_list_t(ty_type_ptr_t) *from,
                           th_type **to)
{
    pending_t copy = *p;

    copy.settled      = false;
    copy.pred.subject = swap(s, p->pred.subject, from, to);
    copy.pred.other   = swap(s, p->pred.other, from, to);
    copy.pred.result  = swap(s, p->pred.result, from, to);

    return copy;
}

static th_type *renamed(ty_solver_t *s, th_type *t)
{
    core_list_t(ty_type_ptr_t) vars = core_list_new(ty_type_ptr_t);

    free_vars(t, &vars);

    th_type **fresh = core_alloc_array(th_type *, vars.len ? vars.len : 1);

    for (size_t i = 0; i < vars.len; i++) {
        fresh[i] = th_var(s->types);
    }

    th_type *copy = vars.len ? swap(s, t, &vars, fresh) : th_resolve(t);

    core_free(fresh);
    core_list_free(vars);

    return copy;
}

th_type *ty_instantiate_declared(ty_solver_t *s, th_type *t, ty_why_t why)
{
    if (!s || !t) {
        return t;
    }

    core_list_t(ty_type_ptr_t) vars = core_list_new(ty_type_ptr_t);
    core_list_t(ty_type_ptr_t) named = core_list_new(ty_type_ptr_t);

    free_vars(t, &vars);
    for (size_t i = 0; i < vars.len; i++) {
        if (rigid(s, vars.data[i])) {
            core_list_push(named, vars.data[i]);
        }
    }

    th_type **fresh = core_alloc_array(th_type *, named.len ? named.len : 1);

    for (size_t i = 0; i < named.len; i++) {
        fresh[i] = th_var(s->types);
    }

    th_type *copy = named.len ? swap(s, t, &named, fresh) : th_resolve(t);

    for (size_t b = 0; b < s->bounds.len; b++) {
        bound_t *bound = &s->bounds.data[b];

        for (size_t i = 0; i < named.len; i++) {
            if (named.data[i] != bound->var) {
                continue;
            }

            ty_pred_t pred = bound->pred;

            pred.subject = fresh[i];
            pred.other   = pred.other ? swap(s, pred.other, &named, fresh)
                                      : nullptr;
            ty_require(s, pred, why.at ? why : bound->why);
        }
    }
    core_free(fresh);
    core_list_free(named);
    core_list_free(vars);

    return copy;
}

th_type *ty_generalize(ty_solver_t *s, th_type *t)
{
    if (!s || !t) {
        return nullptr;
    }
    default_nils(s);

    core_list_t(ty_type_ptr_t) vars = core_list_new(ty_type_ptr_t);

    free_vars(t, &vars);
    if (!vars.len) {
        core_list_free(vars);
        return th_resolve(t);
    }

    scheme_t *scheme = core_alloc(scheme_t);

    scheme->vars  = core_list_new(ty_type_ptr_t);
    scheme->preds = core_list_new(pending_t);
    for (size_t i = 0; i < vars.len; i++) {
        core_list_push(scheme->vars, th_var(s->types));
    }

    th_type *body = swap(s, t, &vars, scheme->vars.data);

    for (size_t k = 0; k < s->live.len; k++) {
        pending_t *p = &s->pending.data[s->live.data[k]];

        if (!p->settled && mentions(p, &vars)) {
            core_list_push(scheme->preds,
                           swap_pred(s, p, &vars, scheme->vars.data));
        }
    }
    core_list_free(vars);
    core_dict_put(&s->schemes, body, scheme);

    return body;
}

th_type *ty_instantiate(ty_solver_t *s, th_type *t, ty_why_t why)
{
    bool      found  = false;
    scheme_t *scheme = s && t ? core_dict_get(&s->schemes, t, &found)
                              : nullptr;

    if (!found) {
        return s && t ? renamed(s, t) : t;
    }

    th_type **fresh = core_alloc_array(th_type *, scheme->vars.len);

    for (size_t i = 0; i < scheme->vars.len; i++) {
        fresh[i] = th_var(s->types);
    }

    th_type *copy = swap(s, t, &scheme->vars, fresh);

    for (size_t i = 0; i < scheme->preds.len; i++) {
        pending_t p = swap_pred(s, &scheme->preds.data[i], &scheme->vars,
                                fresh);

        ty_require(s, p.pred, p.why.at ? p.why : why);
    }
    core_free(fresh);

    return copy;
}

th_type *ty_resolve(ty_solver_t *s, th_type *t)
{
    (void)s;

    return th_resolve(t);
}

void ty_explain(ty_solver_t *s, th_type *t, core_buffer_t *out)
{
    if (!s || !t || !out) {
        return;
    }

    th_type *final = th_resolve(t);

    for (size_t i = 0; i < s->events.len; i++) {
        event_t *e = &s->events.data[i];

        if (th_resolve(e->var) != final && e->var != t) {
            continue;
        }

        core_string_t to = th_print(s->types, e->to);

        core_buffer_printf(out, "R%u", (unsigned)e->why.rule);
        if (e->why.at && e->why.at->block) {
            core_buffer_puts(out, " at ");
            core_buffer_append_string(out, e->why.at->block->name);
            core_buffer_printf(out, "#%u", e->why.at->index);
        }
        core_buffer_puts(out, ": ");
        core_buffer_append_string(out, to);
        core_buffer_puts(out, "\n");
    }
}

size_t ty_failure_count(const ty_solver_t *s)
{
    return s ? s->failures.len : 0;
}

const ty_failure_t *ty_failure_at(const ty_solver_t *s, size_t i)
{
    return s && i < s->failures.len ? &s->failures.data[i] : nullptr;
}

size_t ty_pending_count(const ty_solver_t *s)
{
    size_t open = 0;

    for (size_t k = 0; s && k < s->live.len; k++) {
        open += !s->pending.data[s->live.data[k]].settled;
    }

    return open;
}

static bool open_pending(ty_solver_t *s, const pending_t *p,
                         core_datalog_symbol_t trait)
{
    return !p->settled && p->pred.kind == TY_PRED_IMPLEMENTS
        && word(s, p->pred.name) == trait
        && flexible(s, th_resolve(p->pred.subject));
}

size_t ty_open_count(ty_solver_t *s, core_string_t trait)
{
    if (!s || !trait.data) {
        return 0;
    }

    core_datalog_symbol_t name = word(s, trait);

    s->open_vars.len = 0;
    for (uint32_t k = 0; k < s->live.len; k++) {
        uint32_t i = s->live.data[k];
        pending_t *p = &s->pending.data[i];

        if (!open_pending(s, p, name)) {
            continue;
        }

        th_type *var  = th_resolve(p->pred.subject);
        bool     seen = false;

        for (size_t j = 0; !seen && j < s->open_vars.len; j++) {
            seen = s->open_vars.data[j] == var;
        }
        if (!seen) {
            core_list_push(s->open_vars, var);
        }
    }

    return s->open_vars.len;
}

th_type *ty_open_at(const ty_solver_t *s, size_t i)
{
    return s && i < s->open_vars.len ? s->open_vars.data[i] : nullptr;
}

bool ty_settle_open(ty_solver_t *s, core_string_t trait, th_type *var,
                    th_type *type)
{
    var = var ? th_resolve(var) : nullptr;
    if (!s || !trait.data || !var || !type) {
        return false;
    }

    core_datalog_symbol_t name = word(s, trait);

    for (uint32_t k = 0; k < s->live.len; k++) {
        uint32_t i = s->live.data[k];
        pending_t *p = &s->pending.data[i];

        if (open_pending(s, p, name) && th_resolve(p->pred.subject) == var) {
            return flow(s, p->pred.subject, renamed(s, type), p->why);
        }
    }

    return false;
}
