#include "private/ir/internal.h"
#include "ir/analyses.h"
#include "ir/memssa.h"
#include "ir/typelang.h"
#include "ir/vn.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/hash.h"
#include "lib/list.h"

typedef struct {
    uint32_t len;
    uint64_t words[];
} vn_key_t;

typedef vn_key_t *vn_key_ptr_t;
core_list_decl(vn_key_ptr_t);

typedef struct {
    bool     known;
    ir_int_t value;
} vn_constant_t;
core_list_decl(vn_constant_t);

typedef uint64_t vn_word_t;
core_list_decl(vn_word_t);

struct ir_value_numbers_t {
    ir_func_t                    *f;
    ir_memssa_t                  *memssa;
    core_dict_t                   of_value;
    core_dict_t                   by_key;
    core_list_t(vn_key_ptr_t)     keys;
    core_list_t(vn_constant_t)    constants;
};

enum {
    KEY_INT_CONSTANT = 1u << 20,
    KEY_MEMORY,
    KEY_PHI,
};

static core_hash_value_t
key_hash(void *key)
{
    const vn_key_t *k = key;
    return core_xxh3_64bits_raw(k->words, k->len * sizeof(uint64_t));
}

static bool
key_eq(const void *a, const void *b)
{
    const vn_key_t *x = a;
    const vn_key_t *y = b;
    return x->len == y->len
        && !memcmp(x->words, y->words, x->len * sizeof(uint64_t));
}

static uint32_t
fresh(ir_value_numbers_t *n)
{
    core_list_push(n->constants, ((vn_constant_t){0}));
    return (uint32_t)n->constants.len;
}

static void
assign(ir_value_numbers_t *n, const ir_value_t *value, uint32_t number)
{
    core_dict_put(&n->of_value, (void *)value, (void *)(uintptr_t)number);
}

static uint32_t
known_number(const ir_value_numbers_t *n, const ir_value_t *value)
{
    bool found = false;
    uintptr_t number = (uintptr_t)core_dict_get(
        (core_dict_t *)&n->of_value, (void *)value, &found);
    return found ? (uint32_t)number : 0;
}

static uint32_t
operand_number(ir_value_numbers_t *n, const ir_value_t *value)
{
    if (!value) return 0;
    uint32_t number = known_number(n, value);
    if (number || value->kind == IR_VAL_INSTR) return number;
    number = fresh(n);
    assign(n, value, number);
    return number;
}

static uint32_t
keyed(ir_value_numbers_t *n, core_list_t(vn_word_t) *words,
      uint32_t constant_known, ir_int_t constant)
{
    vn_key_t *key = core_alloc_flex(vn_key_t, uint64_t, words->len);
    key->len = (uint32_t)words->len;
    memcpy(key->words, words->data, words->len * sizeof(uint64_t));
    bool found = false;
    uintptr_t number = (uintptr_t)core_dict_get(&n->by_key, key, &found);
    if (found) {
        core_free(key);
        return (uint32_t)number;
    }
    uint32_t result = fresh(n);
    if (constant_known)
        n->constants.data[result - 1] = (vn_constant_t){true, constant};
    core_dict_put(&n->by_key, key, (void *)(uintptr_t)result);
    core_list_push(n->keys, key);
    return result;
}

static bool
integer_type(ir_func_t *f, th_type *type, ir_interval_t *bounds)
{
    ir_machine_t machine = type ? ir_machine_of(f->owner, type)
                                : (ir_machine_t){0};
    if ((machine.class != IR_CLASS_SINT && machine.class != IR_CLASS_UINT)
        || !machine.width || machine.width > 128) return false;
    if (bounds)
        *bounds = ir_interval_of_width(machine.width,
                                       machine.class == IR_CLASS_SINT);
    return true;
}

static bool
literal_value(const ir_operand_t *operand, ir_int_t *value)
{
    if (operand->kind != IR_OPD_CONST || operand->lit.is_float) return false;
    ir_lit_t literal = operand->lit.i;
    if (literal.mag > (ir_uint_t)ir_int_max + (literal.neg ? 1 : 0))
        return false;
    *value = literal.neg ? -(ir_int_t)(literal.mag - 1) - 1
                         : (ir_int_t)literal.mag;
    return true;
}

static uint32_t
constant_number(ir_value_numbers_t *n, ir_int_t value)
{
    core_list_t(vn_word_t) words = core_list_new(vn_word_t);
    core_list_push(words, (uint64_t)KEY_INT_CONSTANT);
    core_list_push(words, (uint64_t)(ir_uint_t)value);
    core_list_push(words, (uint64_t)((ir_uint_t)value >> 64));
    uint32_t number = keyed(n, &words, true, value);
    core_list_free(words);
    return number;
}

bool
ir_value_number_constant(const ir_value_numbers_t *n, uint32_t number,
                         ir_int_t *value)
{
    if (!n || !number || number > n->constants.len
        || !n->constants.data[number - 1].known) return false;
    if (value) *value = n->constants.data[number - 1].value;
    return true;
}

static bool
pure(ir_op_t op)
{
    switch (op) {
    case IR_OP_ENUM_CONST: case IR_OP_ENUM_VALUE: case IR_OP_ADDRESS_OF:
    case IR_OP_FIELD: case IR_OP_INDEX: case IR_OP_OFFSET: case IR_OP_DIFF:
    case IR_OP_EXTRACT: case IR_OP_EXTRACT_INDEX: case IR_OP_INSERT:
    case IR_OP_VARIANT_TAG: case IR_OP_OPTION_IS_NIL: case IR_OP_NIL:
    case IR_OP_ADD: case IR_OP_SUB: case IR_OP_MUL: case IR_OP_DIV:
    case IR_OP_REM: case IR_OP_NEG: case IR_OP_AND: case IR_OP_OR:
    case IR_OP_XOR: case IR_OP_NOT: case IR_OP_SHL: case IR_OP_SHR:
    case IR_OP_ROL: case IR_OP_ROR: case IR_OP_CLZ: case IR_OP_CTZ:
    case IR_OP_POPCOUNT: case IR_OP_BITREVERSE: case IR_OP_BYTESWAP:
    case IR_OP_EQ: case IR_OP_NE: case IR_OP_LT: case IR_OP_LE:
    case IR_OP_GT: case IR_OP_GE: case IR_OP_CAST: case IR_OP_BITCAST:
    case IR_OP_NOMINAL_CAST: case IR_OP_SELECT: case IR_OP_SIZEOF:
    case IR_OP_ALIGNOF: case IR_OP_OFFSETOF: case IR_OP_TYPEHASH:
    case IR_OP_TYPE_TEST: case IR_OP_CONST:
        return true;
    default:
        return false;
    }
}

static bool
commutative(ir_op_t op)
{
    return op == IR_OP_ADD || op == IR_OP_MUL || op == IR_OP_AND
        || op == IR_OP_OR || op == IR_OP_XOR || op == IR_OP_EQ
        || op == IR_OP_NE;
}

static bool
memory_read(ir_op_t op)
{
    return op == IR_OP_LOAD || op == IR_OP_SEQUENCE_LENGTH
        || op == IR_OP_SEQUENCE_GET;
}

static uint64_t
text_word(core_string_t text)
{
    return core_xxh3_64bits_raw(text.data, text.u8_bytes);
}

static bool
operand_words(ir_value_numbers_t *n, th_ctx *types, const ir_operand_t *od,
              core_list_t(vn_word_t) *words)
{
    switch (od->kind) {
    case IR_OPD_VALUE: {
        uint32_t number = operand_number(n, od->value);
        if (!number) return false;
        core_list_push(*words, (uint64_t)number);
        return true;
    }
    case IR_OPD_CONST: {
        core_list_push(*words, (uint64_t)od->lit.is_float);
        uint64_t bits = 0;
        memcpy(&bits, &od->lit.f, sizeof(bits));
        core_list_push(*words, od->lit.is_float ? bits
                                                : (uint64_t)od->lit.i.mag);
        core_list_push(*words, (uint64_t)(od->lit.i.mag >> 64));
        core_list_push(*words, (uint64_t)od->lit.i.neg);
        return true;
    }
    case IR_OPD_NAME:
        core_list_push(*words, text_word(od->name));
        return true;
    case IR_OPD_TYPE:
        core_list_push(*words, th_hash(types, od->type));
        return true;
    default:
        return false;
    }
}

static const ir_value_t *
stored_value(const ir_instr_t *store)
{
    uint32_t slot = store->op == IR_OP_STORE ? 1
        : store->op == IR_OP_SEQUENCE_SET ? 2 : UINT32_MAX;
    return slot < store->n_operands
        && store->operands[slot].kind == IR_OPD_VALUE
        ? store->operands[slot].value : nullptr;
}

static uint32_t
element_forwarded(ir_value_numbers_t *n, const ir_instr_t *in)
{
    uint32_t access = ir_memssa_access_of(n->memssa, in, IR_ACCESS_LOAD);
    if (access == UINT32_MAX
        || ir_memssa_element_link_count(n->memssa, access) != 1)
        return 0;
    uint32_t def = ir_memssa_element_link(n->memssa, access, 0);
    const ir_memdef_t *store = ir_memssa_def(n->memssa, def);
    const ir_value_t *value = store && store->instr
        ? stored_value(store->instr) : nullptr;
    if (!value || !in->dest
        || ir_memssa_element_relation(n->memssa, def, access)
            != IR_ELEMENT_SAME
        || !ir_type_eq(n->f->owner, in->dest->type, value->type))
        return 0;
    return operand_number(n, value);
}

static uint32_t
forwarded(ir_value_numbers_t *n, const ir_instr_t *in, uint32_t address)
{
    if (!n->memssa) return 0;
    const ir_value_t *base = nullptr, *index = nullptr;
    if (ir_memssa_element_of(in, &base, &index))
        return element_forwarded(n, in);
    uint32_t def = ir_memssa_def_before(n->memssa, in);
    const ir_memdef_t *reaching = ir_memssa_def(n->memssa, def);
    const ir_instr_t *store = reaching && reaching->kind == IR_MEMDEF_STORE
        ? reaching->instr : nullptr;
    if (!store) return 0;
    if (in->op == IR_OP_LOAD && store->op == IR_OP_STORE
        && store->n_operands == 2
        && store->operands[0].kind == IR_OPD_VALUE
        && store->operands[1].kind == IR_OPD_VALUE
        && operand_number(n, store->operands[0].value) == address
        && in->dest && store->operands[1].value
        && ir_type_eq(n->f->owner, in->dest->type,
                      store->operands[1].value->type))
        return operand_number(n, store->operands[1].value);
    if (in->op == IR_OP_SEQUENCE_GET && store->op == IR_OP_SEQUENCE_SET
        && store->n_operands == 3 && in->n_operands == 2
        && store->operands[0].kind == IR_OPD_VALUE
        && store->operands[1].kind == IR_OPD_VALUE
        && store->operands[2].kind == IR_OPD_VALUE
        && in->operands[1].kind == IR_OPD_VALUE
        && operand_number(n, store->operands[0].value) == address
        && operand_number(n, store->operands[1].value)
            == operand_number(n, in->operands[1].value))
        return operand_number(n, store->operands[2].value);
    return 0;
}

static uint32_t
cast_number(ir_value_numbers_t *n, const ir_instr_t *in)
{
    const ir_value_t *source = in->n_operands == 1
        && in->operands[0].kind == IR_OPD_VALUE ? in->operands[0].value
                                                : nullptr;
    ir_interval_t from = {0}, to = {0};
    if (!source || !in->dest
        || !integer_type(n->f, source->type, &from)
        || !integer_type(n->f, in->dest->type, &to)) return 0;
    uint32_t number = operand_number(n, source);
    ir_int_t value = 0;
    if (ir_value_number_constant(n, number, &value))
        return value >= to.lo && value <= to.hi ? number : 0;
    return ir_interval_contains(to, from) ? number : 0;
}

static uint32_t
phi_number(ir_value_numbers_t *n, const ir_instr_t *in)
{
    uint32_t common = 0;
    bool same = true;
    core_list_t(vn_word_t) words = core_list_new(vn_word_t);
    core_list_push(words, (uint64_t)KEY_PHI);
    core_list_push(words, (uint64_t)(uintptr_t)in->block);
    for (uint32_t i = 0; i < in->n_operands; i++) {
        const ir_operand_t *od = &in->operands[i];
        const ir_value_t *arm = od->kind == IR_OPD_PHI_ARM ? od->phi.value
                                                          : nullptr;
        if (arm == in->dest) continue;
        uint32_t number = arm ? known_number(n, arm) : 0;
        if (arm && !number && arm->kind != IR_VAL_INSTR)
            number = operand_number(n, arm);
        if (!number) {
            core_list_free(words);
            return fresh(n);
        }
        core_list_push(words, (uint64_t)(uintptr_t)od->phi.block);
        core_list_push(words, (uint64_t)number);
        if (!common) common = number;
        else if (common != number) same = false;
    }
    uint32_t result = same && common ? common : keyed(n, &words, 0, 0);
    core_list_free(words);
    return result;
}

static uint32_t
instruction_number(ir_value_numbers_t *n, const ir_instr_t *in, th_ctx *types)
{
    if (in->op == IR_OP_COPY && in->n_operands == 1
        && in->operands[0].kind == IR_OPD_VALUE) {
        uint32_t number = operand_number(n, in->operands[0].value);
        return number ? number : fresh(n);
    }
    if (in->op == IR_OP_PHI) return phi_number(n, in);
    ir_int_t constant = 0;
    if (in->op == IR_OP_CONST && in->n_operands == 1 && in->dest
        && integer_type(n->f, in->dest->type, nullptr)
        && literal_value(&in->operands[0], &constant))
        return constant_number(n, constant);
    if (in->op == IR_OP_CAST) {
        uint32_t number = cast_number(n, in);
        if (number) return number;
    }
    bool memory = memory_read(in->op);
    if (!pure(in->op) && !(memory && n->memssa)) return fresh(n);
    core_list_t(vn_word_t) words = core_list_new(vn_word_t);
    core_list_push(words, (uint64_t)in->op);
    core_list_push(words, in->dest ? th_hash(types, in->dest->type) : 0);
    for (uint32_t i = 0; i < in->n_mods; i++)
        core_list_push(words, text_word(in->mods[i]));
    size_t first = words.len;
    for (uint32_t i = 0; i < in->n_operands; i++)
        if (!operand_words(n, types, &in->operands[i], &words)) {
            core_list_free(words);
            return fresh(n);
        }
    if (commutative(in->op) && in->n_operands == 2
        && words.len == first + 2 && words.data[first] > words.data[first + 1]) {
        uint64_t swap = words.data[first];
        words.data[first] = words.data[first + 1];
        words.data[first + 1] = swap;
    }
    if (memory) {
        uint32_t forward = forwarded(n, in, (uint32_t)words.data[first]);
        if (forward) {
            core_list_free(words);
            return forward;
        }
        core_list_push(words, (uint64_t)KEY_MEMORY);
        core_list_push(words, (uint64_t)ir_memssa_def_before(n->memssa, in));
    }
    uint32_t number = keyed(n, &words, 0, 0);
    core_list_free(words);
    return number;
}

ir_value_numbers_t *
ir_value_numbers_build(ir_func_t *f, ir_memssa_t *memssa)
{
    ir_doms_t *doms = f ? ir_func_doms(f) : nullptr;
    if (!doms) return nullptr;
    ir_value_numbers_t *n = core_alloc(ir_value_numbers_t);
    n->f = f;
    n->memssa = memssa;
    core_dict_init(&n->of_value, nullptr, nullptr);
    core_dict_init(&n->by_key, key_hash, key_eq);
    n->keys = core_list_new(vn_key_ptr_t);
    n->constants = core_list_new(vn_constant_t);
    th_ctx *types = ir_module_types(f->owner);
    uint32_t count = f->n_blocks;
    ir_block_t **order = core_alloc_array(ir_block_t *, count ? count : 1);
    for (uint32_t b = 0; b < count; b++) {
        ir_block_t *block = f->blocks[b];
        if (!ir_block_reachable(doms, block)) continue;
        uint32_t at = ir_rpo_index(doms, block);
        if (at < count) order[at] = block;
    }
    for (uint32_t b = 0; b < count; b++) {
        ir_block_t *block = order[b];
        for (uint32_t i = 0; block && i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];
            if (!in->dest) continue;
            assign(n, in->dest, instruction_number(n, in, types));
        }
    }
    core_free(order);
    return n;
}

void
ir_value_numbers_free(ir_value_numbers_t *n)
{
    if (!n) return;
    for (size_t i = 0; i < n->keys.len; i++) core_free(n->keys.data[i]);
    core_list_free(n->keys);
    core_list_free(n->constants);
    core_dict_free(&n->of_value);
    core_dict_free(&n->by_key);
    core_free(n);
}

uint32_t
ir_value_number(const ir_value_numbers_t *n, const ir_value_t *value)
{
    return n && value ? known_number(n, value) : 0;
}

bool
ir_value_numbers_same(const ir_value_numbers_t *n, const ir_value_t *a,
                      const ir_value_t *b)
{
    if (a == b) return a != nullptr;
    uint32_t x = ir_value_number(n, a);
    return x && x == ir_value_number(n, b);
}

uint32_t
ir_value_number_count(const ir_value_numbers_t *n)
{
    return n ? (uint32_t)n->constants.len : 0;
}
