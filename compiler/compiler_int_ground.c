#include "compiler_tu.h"
#include "compiler_link.h"

typedef struct {
    th_type *type;
    parse_tree_t *site;
    core_translation_unit_t *unit;
    n00bb_tu_state_t *state;
    ir_int_t lo;
    ir_int_t hi;
    bool wide;
    th_type *pin;
} integer_class_t;
core_list_decl(integer_class_t);

static const ir_int_t i128_max = (ir_int_t)(~(ir_uint_t)0 >> 1);

static th_integer_class
integer_class_of(th_ctx *ctx, th_type *type)
{
    (void)ctx;
    uint64_t width = 0;
    bool is_signed = false;
    if (n00bb_integer_representation(type, &width, &is_signed))
        return is_signed ? TH_INTEGER_SIGNED : TH_INTEGER_UNSIGNED_TYPE;
    return n00bb_scalar_role_is(type, IR_ROLE_BYTE)
        ? TH_INTEGER_UNSIGNED_TYPE : TH_INTEGER_NONE;
}

void
n00bb_register_integer_class(n00bb_tu_state_t *state)
{
    th_register_integer_class(state->types, integer_class_of);
}

bool
n00bb_open_integer(th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    return type && th_kind_of(type) == TH_VAR
        && th_var_is_integer(type, nullptr);
}

bool
n00bb_require_unsigned_integer(n00bb_tu_state_t *state, th_type *type)
{
    return th_var_constrain(state->types, th_resolve(type),
        (th_constraint){.kind = TH_CON_INTEGER,
            .integer = {.flags = TH_INTEGER_UNSIGNED}});
}

th_type *
n00bb_integer_literal_variable(n00bb_tu_state_t *state, parse_tree_t *site,
                               ir_lit_t literal)
{
    th_type *type = th_var(state->types);
    if (!type || !th_var_constrain(state->types, type,
            (th_constraint){.kind = TH_CON_INTEGER})) return nullptr;
    bool wide = !literal.neg && literal.mag > (ir_uint_t)i128_max;
    ir_int_t value = wide ? i128_max
        : literal.neg ? -(ir_int_t)(literal.mag - 1) - 1
        : (ir_int_t)literal.mag;
    core_list_push(state->integer_seeds, ((n00bb_integer_seed_t){
        .site = site, .type = type, .lo = value, .hi = value, .wide = wide,
    }));
    return type;
}

static bool
fits(ir_int_t lo, ir_int_t hi, uint64_t width, bool is_signed)
{
    if (width >= 128)
        return is_signed || lo >= 0;
    ir_int_t span = (ir_int_t)1 << (is_signed ? width - 1 : width);
    return is_signed ? lo >= -span && hi < span : lo >= 0 && hi < span;
}

static th_type *
best_fit(th_ctx *types, const integer_class_t *class, bool unsigned_only)
{
    const core_string_t unsigned_names[] = {
        CORE_STRING_STATIC("u8"), CORE_STRING_STATIC("u16"),
        CORE_STRING_STATIC("u32"), CORE_STRING_STATIC("u64"),
        CORE_STRING_STATIC("u128"),
    };
    const core_string_t signed_names[] = {
        CORE_STRING_STATIC("i8"), CORE_STRING_STATIC("i16"),
        CORE_STRING_STATIC("i32"), CORE_STRING_STATIC("i64"),
        CORE_STRING_STATIC("i128"),
    };
    bool is_signed = class->lo < 0 && !class->wide;
    if (is_signed && unsigned_only) return nullptr;
    for (size_t i = 0; i < 5; i++) {
        uint64_t width = (uint64_t)8 << i;
        if (!class->wide && !fits(class->lo, class->hi, width, is_signed))
            continue;
        if (class->wide && width < 128) continue;
        return th_name(types, canonical_builtin_type_name(
            is_signed ? signed_names[i] : unsigned_names[i]));
    }
    return nullptr;
}

static void
report(const integer_class_t *class, core_string_t message)
{
    core_diagnostics_report_at(core_translation_unit_diagnostics(class->unit),
        class->unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0047"),
        message, link_source_location(class->site));
}

static void
collect_interface_integers(n00bb_tu_state_t *state, th_type *type,
                           core_dict_t *interface)
{
    th_type **vars = nullptr;
    size_t count = type ? th_free_vars(state->types, type, &vars) : 0;
    for (size_t i = 0; i < count; i++)
        if (n00bb_open_integer(vars[i]))
            core_dict_put(interface, th_resolve(vars[i]), vars[i]);
}

static void
collect_interfaces(core_translation_unit_t *const *units, size_t unit_count,
                   core_dict_t *interface)
{
    for (size_t u = 0; u < unit_count; u++) {
        n00bb_tu_state_t *state = tu_state(units[u]);
        if (!state || !state->annotations || !state->annotations->symtab)
            continue;
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
        for (size_t s = 0; s < scopes.len; s++)
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                collect_interface_integers(state, symbol->type, interface);
                collect_interface_integers(state, parse_sym_binding(symbol,
                    CORE_STRING_STATIC("n00bb.body-function-signature")),
                    interface);
            }
        core_list_free(scopes);
    }
}

bool
n00bb_ground_integers(core_translation_unit_t *const *units,
                      size_t unit_count, bool interface_only,
                      bool *grounded)
{
    *grounded = false;
    bool seeded = false;
    for (size_t u = 0; !seeded && u < unit_count; u++) {
        n00bb_tu_state_t *state = tu_state(units[u]);
        seeded = state && state->integer_seeds.len;
    }
    if (!seeded) return true;
    core_dict_t interface;
    core_dict_init(&interface, nullptr, nullptr);
    if (interface_only) collect_interfaces(units, unit_count, &interface);
    core_dict_t index;
    core_dict_init(&index, nullptr, nullptr);
    core_list_t(integer_class_t) classes = core_list_new(integer_class_t);
    for (size_t u = 0; u < unit_count; u++) {
        n00bb_tu_state_t *state = tu_state(units[u]);
        if (!state) continue;
        size_t kept = 0;
        for (size_t i = 0; i < state->integer_seeds.len; i++) {
            n00bb_integer_seed_t seed = state->integer_seeds.data[i];
            th_type *current = seed.site ? parse_tree_type(seed.site)
                                         : nullptr;
            if (current && th_resolve(current) != th_resolve(seed.type)
                && n00bb_open_integer(seed.type))
                (void)th_unify_in_place(state->types, seed.type, current);
            th_type *type = th_resolve(current ? current : seed.type);
            if (th_kind_of(type) != TH_VAR) continue;
            state->integer_seeds.data[kept++] = seed;
            if (interface_only && !core_dict_contains(&interface, type))
                continue;
            bool found = false;
            uintptr_t slot = (uintptr_t)core_dict_get(&index, type, &found);
            if (!found) {
                core_list_push(classes, ((integer_class_t){
                    .type = type, .site = seed.site, .unit = units[u],
                    .state = state, .lo = seed.lo, .hi = seed.hi,
                    .wide = seed.wide,
                }));
                core_dict_put(&index, type, (void *)(uintptr_t)classes.len);
                continue;
            }
            integer_class_t *class = &classes.data[slot - 1];
            if (seed.lo < class->lo) class->lo = seed.lo;
            if (seed.hi > class->hi) class->hi = seed.hi;
            class->wide = class->wide || seed.wide;
        }
        state->integer_seeds.len = kept;
    }
    bool ok = true;
    for (size_t u = 0; u < unit_count; u++) {
        n00bb_tu_state_t *state = tu_state(units[u]);
        for (size_t i = 0; state && i < state->deferred_conversions.len; i++) {
            n00bb_deferred_conversion_t *pending =
                &state->deferred_conversions.data[i];
            bool found = false;
            uintptr_t slot = (uintptr_t)core_dict_get(&index,
                th_resolve(pending->value), &found);
            th_type *target = th_resolve(pending->target);
            if (!found || !th_is_closed(state->types, target)
                || integer_class_of(state->types, target) == TH_INTEGER_NONE)
                continue;
            integer_class_t *class = &classes.data[slot - 1];
            if (!class->pin
                || n00bb_integer_type_promotes_to(target, class->pin))
                class->pin = target;
            else if (!n00bb_integer_type_promotes_to(class->pin, target)) {
                class->site = pending->site;
                class->unit = units[u];
                report(class, CORE_STRING_STATIC(
                    "this integer is used as two types that neither converts to"));
                ok = false;
            }
        }
    }
    for (size_t i = 0; i < classes.len; i++) {
        integer_class_t *class = &classes.data[i];
        uint8_t flags = 0;
        if (th_kind_of(th_resolve(class->type)) != TH_VAR
            || !th_var_is_integer(th_resolve(class->type), &flags))
            continue;
        th_type *chosen = class->pin ? class->pin
            : best_fit(class->state->types, class,
                       (flags & TH_INTEGER_UNSIGNED) != 0);
        if (!chosen) {
            report(class, (flags & TH_INTEGER_UNSIGNED)
                ? CORE_STRING_STATIC(
                      "a bitwise integer value can be negative; give it an unsigned type")
                : CORE_STRING_STATIC("no integer type holds this value"));
            ok = false;
            continue;
        }
        if (!th_unify_in_place(class->state->types, class->type, chosen)) {
            report(class, th_last_error(class->state->types));
            ok = false;
        }
        else *grounded = true;
    }
    core_list_free(classes);
    core_dict_free(&index);
    core_dict_free(&interface);
    return ok;
}
