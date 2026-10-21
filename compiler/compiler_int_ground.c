#include "compiler_tu.h"
#include "compiler_link.h"

enum { GROWTH_LIMIT = 8, ROUND_LIMIT = 256 };

typedef struct {
    th_type *type;
    parse_tree_t *site;
    core_translation_unit_t *unit;
    n00bb_tu_state_t *state;
    ir_int_t lo;
    ir_int_t hi;
    bool known;
    bool wide;
    bool frozen;
    uint32_t growth;
    th_type *pin;
} integer_class_t;
core_list_decl(integer_class_t);

typedef struct {
    ir_int_t lo;
    ir_int_t hi;
    bool known;
} interval_t;

static const ir_int_t i128_max = (ir_int_t)(~(ir_uint_t)0 >> 1);
static const ir_int_t i128_min = -(ir_int_t)(~(ir_uint_t)0 >> 1) - 1;

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

th_type *
n00bb_integer_result_variable(n00bb_tu_state_t *state, parse_tree_t *site,
                              ir_op_t op, th_type *left, th_type *right)
{
    uint8_t flags = 0, other = 0;
    bool bitwise = op == IR_OP_AND || op == IR_OP_OR || op == IR_OP_XOR;
    if (bitwise) {
        (void)th_var_is_integer(th_resolve(left), &flags);
        if (right) (void)th_var_is_integer(th_resolve(right), &other);
    }
    th_type *type = th_var(state->types);
    if (!type || !th_var_constrain(state->types, type,
            (th_constraint){.kind = TH_CON_INTEGER,
                .integer = {.flags = (uint8_t)(flags | other)}}))
        return nullptr;
    core_list_push(state->integer_edges, ((n00bb_integer_edge_t){
        .site = site, .result = type, .left = left, .right = right, .op = op,
    }));
    core_dict_put(&state->integer_edge_sites, site, site);
    return type;
}

void
n00bb_integer_growth(n00bb_tu_state_t *state, parse_tree_t *site, ir_op_t op,
                     th_type *target, th_type *value)
{
    core_list_push(state->integer_edges, ((n00bb_integer_edge_t){
        .site = site, .result = target, .left = target, .right = value,
        .op = op,
    }));
}

static bool
fits(ir_int_t lo, ir_int_t hi, uint64_t width, bool is_signed)
{
    if (width >= 128)
        return is_signed || lo >= 0;
    ir_int_t span = (ir_int_t)1 << (is_signed ? width - 1 : width);
    return is_signed ? lo >= -span && hi < span : lo >= 0 && hi < span;
}

static bool
type_interval(th_type *type, interval_t *out)
{
    uint64_t width = 0;
    bool is_signed = false;
    if (!n00bb_integer_representation(type, &width, &is_signed)) {
        if (!n00bb_scalar_role_is(type, IR_ROLE_BYTE)) return false;
        width = 8;
    }
    if (!width) return false;
    if (width >= 128) {
        *out = (interval_t){is_signed ? i128_min : 0, i128_max, true};
        return true;
    }
    ir_int_t span = (ir_int_t)1 << (is_signed ? width - 1 : width);
    *out = is_signed ? (interval_t){-span, span - 1, true}
                     : (interval_t){0, span - 1, true};
    return true;
}

static ir_int_t
saturate_add(ir_int_t a, ir_int_t b)
{
    ir_int_t sum;
    if (!__builtin_add_overflow(a, b, &sum)) return sum;
    return b > 0 ? i128_max : i128_min;
}

static ir_int_t
saturate_sub(ir_int_t a, ir_int_t b)
{
    ir_int_t difference;
    if (!__builtin_sub_overflow(a, b, &difference)) return difference;
    return b < 0 ? i128_max : i128_min;
}

static ir_int_t
saturate_mul(ir_int_t a, ir_int_t b)
{
    ir_int_t product;
    if (!__builtin_mul_overflow(a, b, &product)) return product;
    return (a < 0) == (b < 0) ? i128_max : i128_min;
}

static ir_int_t
magnitude(interval_t a)
{
    ir_int_t low = a.lo == i128_min ? i128_max : -a.lo;
    return low > a.hi ? low : a.hi;
}

static ir_int_t
all_ones_above(ir_int_t value)
{
    ir_int_t mask = 0;
    while (mask < value && mask < i128_max) mask = (mask << 1) | 1;
    return mask;
}

static interval_t
apply(ir_op_t op, interval_t a, interval_t b)
{
    switch (op) {
    case IR_OP_ADD:
        return (interval_t){saturate_add(a.lo, b.lo),
                            saturate_add(a.hi, b.hi), true};
    case IR_OP_SUB:
        return (interval_t){saturate_sub(a.lo, b.hi),
                            saturate_sub(a.hi, b.lo), true};
    case IR_OP_MUL: {
        ir_int_t products[] = {
            saturate_mul(a.lo, b.lo), saturate_mul(a.lo, b.hi),
            saturate_mul(a.hi, b.lo), saturate_mul(a.hi, b.hi),
        };
        interval_t out = {products[0], products[0], true};
        for (size_t i = 1; i < 4; i++) {
            if (products[i] < out.lo) out.lo = products[i];
            if (products[i] > out.hi) out.hi = products[i];
        }
        return out;
    }
    case IR_OP_DIV:
        if (a.lo >= 0 && b.lo >= 0) return (interval_t){0, a.hi, true};
        return (interval_t){-magnitude(a), magnitude(a), true};
    case IR_OP_REM: {
        ir_int_t bound = magnitude(b) > 0 ? magnitude(b) - 1 : 0;
        if (a.lo >= 0 && b.lo >= 0)
            return (interval_t){0, a.hi < bound ? a.hi : bound, true};
        return (interval_t){-bound, bound, true};
    }
    case IR_OP_AND:
        if (a.lo >= 0 && b.lo >= 0)
            return (interval_t){0, a.hi < b.hi ? a.hi : b.hi, true};
        break;
    case IR_OP_OR:
    case IR_OP_XOR:
        if (a.lo >= 0 && b.lo >= 0)
            return (interval_t){0,
                all_ones_above(a.hi > b.hi ? a.hi : b.hi), true};
        break;
    case IR_OP_NEG:
        return (interval_t){saturate_sub(0, a.hi), saturate_sub(0, a.lo),
                            true};
    default:
        break;
    }
    ir_int_t bound = magnitude(a) > magnitude(b) ? magnitude(a) : magnitude(b);
    return (interval_t){-bound, bound, true};
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
append_integer(core_buffer_t *buffer, ir_int_t value, bool wide)
{
    char digits[48];
    size_t length = 0;
    ir_uint_t magnitude = wide ? ~(ir_uint_t)0
        : value < 0 ? (ir_uint_t)(-(value + 1)) + 1 : (ir_uint_t)value;
    do {
        digits[length++] = (char)('0' + (int)(magnitude % 10));
        magnitude /= 10;
    } while (magnitude);
    if (!wide && value < 0) core_buffer_append(buffer, "-", 1);
    while (length) core_buffer_append(buffer, &digits[--length], 1);
}

static bool
check_seed_fits(n00bb_tu_state_t *state, core_translation_unit_t *unit,
                const n00bb_integer_seed_t *seed, th_type *type)
{
    uint64_t width = 0;
    bool is_signed = false;
    if (!n00bb_integer_representation(type, &width, &is_signed)) {
        if (!n00bb_scalar_role_is(type, IR_ROLE_BYTE)) return true;
        width = 8;
    }
    if (!width || (seed->wide ? width >= 128 && !is_signed
                              : fits(seed->lo, seed->hi, width, is_signed)))
        return true;
    core_string_t name = th_print(state->types, type);
    core_buffer_t message = {0};
    core_buffer_append(&message, "integer constant ", 17);
    append_integer(&message, seed->lo, seed->wide);
    core_buffer_printf(&message, " does not fit %.*s",
                       (int)name.u8_bytes, name.data);
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0041"),
        core_buffer_string(&message), link_source_location(seed->site));
    core_buffer_release(&message);
    return false;
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

typedef struct {
    core_dict_t index;
    core_list_t(integer_class_t) classes;
} class_table_t;

static integer_class_t *
class_for(class_table_t *table, th_type *type, core_translation_unit_t *unit,
          n00bb_tu_state_t *state, parse_tree_t *site)
{
    type = type ? th_resolve(type) : nullptr;
    if (!n00bb_open_integer(type)) return nullptr;
    bool found = false;
    uintptr_t slot = (uintptr_t)core_dict_get(&table->index, type, &found);
    if (found) return &table->classes.data[slot - 1];
    if (!unit) return nullptr;
    core_list_push(table->classes, ((integer_class_t){
        .type = type, .site = site, .unit = unit, .state = state,
    }));
    core_dict_put(&table->index, type,
                  (void *)(uintptr_t)table->classes.len);
    return &table->classes.data[table->classes.len - 1];
}

static th_type *
seed_type(const n00bb_integer_seed_t *seed)
{
    th_type *current = seed->site ? parse_tree_type(seed->site) : nullptr;
    if (current && th_kind_of(th_resolve(current)) == TH_VAR
        && !n00bb_open_integer(current))
        current = nullptr;
    return current ? current : seed->type;
}

static interval_t
operand_interval(class_table_t *table, th_type *type)
{
    interval_t out = {0};
    integer_class_t *class = class_for(table, type, nullptr, nullptr,
                                       nullptr);
    if (class && class->pin) (void)type_interval(class->pin, &out);
    else if (class)
        out = (interval_t){class->lo, class->hi, class->known};
    else if (type && !type_interval(th_resolve(type), &out))
        out = (interval_t){INT64_MIN, INT64_MAX, true};
    return out;
}

static bool
widen(integer_class_t *class, interval_t range)
{
    if (class->frozen || class->pin || !range.known) return false;
    if (!class->known) {
        class->lo = range.lo;
        class->hi = range.hi;
        class->known = true;
        return true;
    }
    if (range.lo >= class->lo && range.hi <= class->hi) return false;
    if (++class->growth > GROWTH_LIMIT) {
        bool negative = class->lo < 0 || range.lo < 0;
        class->lo = negative ? INT64_MIN : 0;
        class->hi = negative ? INT64_MAX : (ir_int_t)UINT64_MAX;
        class->frozen = true;
        return true;
    }
    if (range.lo < class->lo) class->lo = range.lo;
    if (range.hi > class->hi) class->hi = range.hi;
    return true;
}

static void
solve(class_table_t *table, core_translation_unit_t *const *units,
      size_t unit_count)
{
    for (int pass = 0; pass < 2; pass++) {
        bool changed = true;
        for (size_t round = 0; changed && round < ROUND_LIMIT; round++) {
            changed = false;
            for (size_t u = 0; u < unit_count; u++) {
                n00bb_tu_state_t *state = tu_state(units[u]);
                for (size_t e = 0; state && e < state->integer_edges.len;
                     e++) {
                    n00bb_integer_edge_t *edge = &state->integer_edges.data[e];
                    integer_class_t *result = class_for(table, edge->result,
                        nullptr, nullptr, nullptr);
                    if (!result) continue;
                    interval_t left = operand_interval(table, edge->left);
                    interval_t right = edge->right
                        ? operand_interval(table, edge->right) : left;
                    if (!left.known || !right.known) continue;
                    interval_t out = apply(edge->op, left, right);
                    if (left.lo < out.lo) out.lo = left.lo;
                    if (left.hi > out.hi) out.hi = left.hi;
                    if (edge->right && right.lo < out.lo) out.lo = right.lo;
                    if (edge->right && right.hi > out.hi) out.hi = right.hi;
                    changed = widen(result, out) || changed;
                }
            }
        }
        for (size_t i = 0; pass == 0 && i < table->classes.len; i++) {
            integer_class_t *class = &table->classes.data[i];
            if (class->known || class->pin) continue;
            class->lo = INT64_MIN;
            class->hi = INT64_MAX;
            class->known = true;
        }
    }
}

static void
collect_classes(class_table_t *table, core_translation_unit_t *const *units,
                size_t unit_count)
{
    for (size_t u = 0; u < unit_count; u++) {
        n00bb_tu_state_t *state = tu_state(units[u]);
        if (!state) continue;
        for (size_t i = 0; i < state->integer_seeds.len; i++) {
            n00bb_integer_seed_t *seed = &state->integer_seeds.data[i];
            th_type *current = seed_type(seed);
            if (current != seed->type && n00bb_open_integer(seed->type)
                && th_resolve(current) != th_resolve(seed->type))
                (void)th_unify_in_place(state->types, seed->type, current);
            integer_class_t *class = class_for(table, current, units[u],
                                               state, seed->site);
            if (!class) continue;
            if (!class->known) {
                class->lo = seed->lo;
                class->hi = seed->hi;
                class->known = true;
            }
            if (seed->lo < class->lo) class->lo = seed->lo;
            if (seed->hi > class->hi) class->hi = seed->hi;
            class->wide = class->wide || seed->wide;
        }
        for (size_t e = 0; e < state->integer_edges.len; e++) {
            n00bb_integer_edge_t *edge = &state->integer_edges.data[e];
            (void)class_for(table, edge->result, units[u], state,
                            edge->site);
            (void)class_for(table, edge->left, units[u], state, edge->site);
            (void)class_for(table, edge->right, units[u], state, edge->site);
        }
    }
}

static bool
collect_pins(class_table_t *table, core_translation_unit_t *const *units,
             size_t unit_count)
{
    bool ok = true;
    for (size_t u = 0; u < unit_count; u++) {
        n00bb_tu_state_t *state = tu_state(units[u]);
        for (size_t i = 0; state && i < state->deferred_conversions.len; i++) {
            n00bb_deferred_conversion_t *pending =
                &state->deferred_conversions.data[i];
            th_type *target = th_resolve(pending->target);
            integer_class_t *class = th_is_closed(state->types, target)
                && integer_class_of(state->types, target) != TH_INTEGER_NONE
                ? class_for(table, pending->value, units[u], state,
                            pending->site)
                : nullptr;
            if (!class) continue;
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
    return ok;
}

static void
retire_resolved(core_translation_unit_t *const *units, size_t unit_count)
{
    for (size_t u = 0; u < unit_count; u++) {
        n00bb_tu_state_t *state = tu_state(units[u]);
        if (!state) continue;
        size_t kept = 0;
        for (size_t i = 0; i < state->integer_seeds.len; i++) {
            n00bb_integer_seed_t seed = state->integer_seeds.data[i];
            th_type *type = th_resolve(seed_type(&seed));
            if (th_kind_of(type) == TH_VAR)
                state->integer_seeds.data[kept++] = seed;
            else if (th_is_closed(state->types, type))
                (void)check_seed_fits(state, units[u], &seed, type);
        }
        state->integer_seeds.len = kept;
        kept = 0;
        for (size_t e = 0; e < state->integer_edges.len; e++)
            if (n00bb_open_integer(state->integer_edges.data[e].result))
                state->integer_edges.data[kept++] =
                    state->integer_edges.data[e];
        state->integer_edges.len = kept;
    }
}

bool
n00bb_ground_integers(core_translation_unit_t *const *units,
                      size_t unit_count, bool interface_only,
                      bool *grounded)
{
    *grounded = false;
    bool pending = false;
    for (size_t u = 0; !pending && u < unit_count; u++) {
        n00bb_tu_state_t *state = tu_state(units[u]);
        pending = state && (state->integer_seeds.len
                            || state->integer_edges.len);
    }
    if (!pending) return true;
    core_dict_t interface;
    core_dict_init(&interface, nullptr, nullptr);
    if (interface_only) collect_interfaces(units, unit_count, &interface);
    class_table_t table = {.classes = core_list_new(integer_class_t)};
    core_dict_init(&table.index, nullptr, nullptr);
    collect_classes(&table, units, unit_count);
    bool ok = collect_pins(&table, units, unit_count);
    solve(&table, units, unit_count);
    for (size_t i = 0; i < table.classes.len; i++) {
        integer_class_t *class = &table.classes.data[i];
        uint8_t flags = 0;
        th_type *type = th_resolve(class->type);
        if (!class->state || th_kind_of(type) != TH_VAR
            || !th_var_is_integer(type, &flags)
            || (interface_only && !core_dict_contains(&interface, type)))
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
    retire_resolved(units, unit_count);
    core_list_free(table.classes);
    core_dict_free(&table.index);
    core_dict_free(&interface);
    return ok;
}
