#include "ir/interval.h"

const ir_int_t ir_int_max = (ir_int_t)(~(ir_uint_t)0 >> 1);
const ir_int_t ir_int_min = -(ir_int_t)(~(ir_uint_t)0 >> 1) - 1;

void
ir_uint_append(core_buffer_t *out, ir_uint_t value)
{
    char   digits[40];
    size_t at = sizeof(digits);
    do {
        digits[--at] = (char)('0' + (int)(value % 10));
        value /= 10;
    } while (value);
    core_buffer_append(out, digits + at, sizeof(digits) - at);
}

void
ir_int_append(core_buffer_t *out, ir_int_t value)
{
    if (value < 0) core_buffer_append(out, "-", 1);
    ir_uint_append(out, value < 0 ? (ir_uint_t)(-(value + 1)) + 1
                                  : (ir_uint_t)value);
}

static ir_int_t
add(ir_int_t a, ir_int_t b)
{
    ir_int_t sum;
    if (!__builtin_add_overflow(a, b, &sum)) return sum;
    return b > 0 ? ir_int_max : ir_int_min;
}

static ir_int_t
subtract(ir_int_t a, ir_int_t b)
{
    ir_int_t difference;
    if (!__builtin_sub_overflow(a, b, &difference)) return difference;
    return b < 0 ? ir_int_max : ir_int_min;
}

static ir_int_t
multiply(ir_int_t a, ir_int_t b)
{
    ir_int_t product;
    if (!__builtin_mul_overflow(a, b, &product)) return product;
    return (a < 0) == (b < 0) ? ir_int_max : ir_int_min;
}

static ir_int_t
shift_left(ir_int_t value, ir_int_t count)
{
    if (value == 0) return 0;
    if (count >= 127) return value > 0 ? ir_int_max : ir_int_min;
    ir_int_t limit = ir_int_max >> count;
    if (value > limit) return ir_int_max;
    if (value < -limit) return ir_int_min;
    return value * ((ir_int_t)1 << count);
}

static ir_int_t
magnitude(ir_interval_t range)
{
    ir_int_t low = range.lo == ir_int_min ? ir_int_max : -range.lo;
    return ir_int_greater(low, range.hi);
}

static ir_int_t
all_ones_above(ir_int_t value)
{
    ir_int_t mask = 0;
    while (mask < value && mask < ir_int_max) mask = (mask << 1) | 1;
    return mask;
}

ir_interval_t
ir_interval_point(ir_int_t value)
{
    return (ir_interval_t){value, value};
}

ir_interval_t
ir_interval_join(ir_interval_t a, ir_interval_t b)
{
    return (ir_interval_t){ir_int_lesser(a.lo, b.lo), ir_int_greater(a.hi, b.hi)};
}

bool
ir_interval_meet(ir_interval_t a, ir_interval_t b, ir_interval_t *out)
{
    ir_interval_t met = {ir_int_greater(a.lo, b.lo), ir_int_lesser(a.hi, b.hi)};
    if (met.lo > met.hi) return false;
    if (out) *out = met;
    return true;
}

bool
ir_interval_contains(ir_interval_t outer, ir_interval_t inner)
{
    return inner.lo >= outer.lo && inner.hi <= outer.hi;
}

ir_interval_t
ir_interval_of_width(uint64_t width, bool is_signed)
{
    if (width >= 128)
        return is_signed ? (ir_interval_t){ir_int_min, ir_int_max}
                         : (ir_interval_t){0, ir_int_max};
    if (!width) return (ir_interval_t){0, 0};
    uint64_t bits = is_signed ? width - 1 : width;
    ir_int_t high = bits >= 127 ? ir_int_max : ((ir_int_t)1 << bits) - 1;
    return is_signed ? (ir_interval_t){-high - 1, high}
                     : (ir_interval_t){0, high};
}

bool
ir_interval_of_integer_type(ir_module_t *module, th_type *type,
                            ir_interval_t *out)
{
    if (!module || !type) return false;
    ir_machine_t machine = ir_machine_of(module, type);
    if ((machine.class != IR_CLASS_SINT && machine.class != IR_CLASS_UINT)
        || !machine.width || machine.width > 128) return false;
    if (out)
        *out = ir_interval_of_width(machine.width,
                                    machine.class == IR_CLASS_SINT);
    return true;
}

bool
ir_interval_fits(ir_interval_t range, uint64_t width, bool is_signed)
{
    return ir_interval_contains(ir_interval_of_width(width, is_signed),
                                range);
}

ir_interval_t
ir_interval_apply(ir_op_t op, ir_interval_t a, ir_interval_t b)
{
    if (a.lo == a.hi && b.lo == b.hi) {
        bool divisible = b.lo != 0 && !(a.lo == ir_int_min && b.lo == -1);
        switch (op) {
        case IR_OP_AND: return ir_interval_point(a.lo & b.lo);
        case IR_OP_OR: return ir_interval_point(a.lo | b.lo);
        case IR_OP_XOR: return ir_interval_point(a.lo ^ b.lo);
        case IR_OP_DIV:
            if (divisible) return ir_interval_point(a.lo / b.lo);
            break;
        case IR_OP_REM:
            if (divisible) return ir_interval_point(a.lo % b.lo);
            break;
        default:
            break;
        }
    }
    switch (op) {
    case IR_OP_ADD:
        return (ir_interval_t){add(a.lo, b.lo), add(a.hi, b.hi)};
    case IR_OP_SUB:
        return (ir_interval_t){subtract(a.lo, b.hi), subtract(a.hi, b.lo)};
    case IR_OP_MUL: {
        ir_int_t products[] = {
            multiply(a.lo, b.lo), multiply(a.lo, b.hi),
            multiply(a.hi, b.lo), multiply(a.hi, b.hi),
        };
        ir_interval_t out = ir_interval_point(products[0]);
        for (size_t i = 1; i < 4; i++)
            out = ir_interval_join(out, ir_interval_point(products[i]));
        return out;
    }
    case IR_OP_DIV:
        if (a.lo >= 0 && b.lo > 0)
            return (ir_interval_t){a.lo / b.hi, a.hi / b.lo};
        if (a.lo >= 0 && b.lo >= 0) return (ir_interval_t){0, a.hi};
        return (ir_interval_t){-magnitude(a), magnitude(a)};
    case IR_OP_REM: {
        ir_int_t bound = magnitude(b) > 0 ? magnitude(b) - 1 : 0;
        if (a.lo >= 0 && b.lo >= 0)
            return (ir_interval_t){0, ir_int_lesser(a.hi, bound)};
        return (ir_interval_t){-bound, bound};
    }
    case IR_OP_AND:
        if (a.lo >= 0 && b.lo >= 0)
            return (ir_interval_t){0, ir_int_lesser(a.hi, b.hi)};
        if (a.lo >= 0) return (ir_interval_t){0, a.hi};
        if (b.lo >= 0) return (ir_interval_t){0, b.hi};
        break;
    case IR_OP_OR:
    case IR_OP_XOR:
        if (a.lo >= 0 && b.lo >= 0)
            return (ir_interval_t){0, all_ones_above(ir_int_greater(a.hi, b.hi))};
        break;
    case IR_OP_SHL:
        if (a.lo >= 0 && b.lo >= 0)
            return (ir_interval_t){shift_left(a.lo, b.lo),
                                   shift_left(a.hi, b.hi)};
        break;
    case IR_OP_SHR:
        if (a.lo >= 0 && b.lo >= 0)
            return (ir_interval_t){b.hi >= 127 ? 0 : a.lo >> b.hi,
                                   b.lo >= 127 ? 0 : a.hi >> b.lo};
        break;
    case IR_OP_NEG:
        return (ir_interval_t){subtract(0, a.hi), subtract(0, a.lo)};
    default:
        break;
    }
    ir_int_t bound = ir_int_greater(magnitude(a), magnitude(b));
    return (ir_interval_t){-bound, bound};
}

ir_interval_t
ir_interval_constrain(ir_interval_t range, ir_op_t comparison,
                      ir_interval_t other)
{
    switch (comparison) {
    case IR_OP_LT:
        range.hi = ir_int_lesser(range.hi, subtract(other.hi, 1));
        break;
    case IR_OP_LE:
        range.hi = ir_int_lesser(range.hi, other.hi);
        break;
    case IR_OP_GT:
        range.lo = ir_int_greater(range.lo, add(other.lo, 1));
        break;
    case IR_OP_GE:
        range.lo = ir_int_greater(range.lo, other.lo);
        break;
    case IR_OP_EQ:
        range.lo = ir_int_greater(range.lo, other.lo);
        range.hi = ir_int_lesser(range.hi, other.hi);
        break;
    case IR_OP_NE:
        if (other.lo == other.hi && range.lo == other.lo) range.lo++;
        else if (other.lo == other.hi && range.hi == other.lo) range.hi--;
        break;
    default:
        break;
    }
    return range;
}
