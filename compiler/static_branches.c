#include "static_branches.h"

#include "ir/doms.h"
#include "ir/pass.h"
#include "ir/typelang.h"
#include "lib/alloc.h"
#include "th/environment.h"

static th_type *
test_source_type(th_ctx *types, ir_value_t *value)
{
    for (size_t depth = 0; value && depth < 8; depth++) {
        if (th_is_closed(types, value->type)) return value->type;
        ir_instr_t *narrow = value->kind == IR_VAL_INSTR ? value->def
                                                         : nullptr;
        if (!narrow || narrow->op != IR_OP_TYPE_NARROW
            || narrow->n_operands != 2
            || narrow->operands[0].kind != IR_OPD_VALUE
            || narrow->operands[1].kind != IR_OPD_TYPE)
            break;
        ir_value_t *source = narrow->operands[0].value;
        th_type *concrete = source ? source->type : nullptr;
        if (concrete && th_is_closed(types, concrete)
            && !th_env_type_pattern_accepts(types,
                narrow->operands[1].type, concrete)) break;
        value = source;
    }
    return value ? value->type : nullptr;
}

int
n00bb_static_type_test(const ir_instr_t *test)
{
    if (!test || test->op != IR_OP_TYPE_TEST || test->n_operands != 2
        || test->operands[0].kind != IR_OPD_VALUE
        || test->operands[1].kind != IR_OPD_TYPE
        || !test->operands[0].value || !test->block) return -1;
    th_ctx *types = ir_module_types(test->block->owner->owner);
    th_type *source = test_source_type(types, test->operands[0].value);
    th_type *pattern = test->operands[1].type;
    th_type *resolved = source ? th_resolve(source) : nullptr;
    if (!resolved || !pattern || !th_is_closed(types, resolved)
        || th_kind_of(resolved) == TH_SUM || th_kind_of(resolved) == TH_TRAIT
        || th_builtin_kind_of(resolved) == TH_BUILTIN_ANY) return -1;
    core_string_t trait = {0};
    if (th_kind_of(th_resolve(pattern)) == TH_TRAIT
        && th_trait_name(th_resolve(pattern), &trait))
        return th_env_implements(types, resolved, trait);
    if (!th_is_closed(types, pattern))
        return th_env_type_pattern_accepts(types, pattern, resolved);
    return th_hash(types, resolved) == th_hash(types, th_resolve(pattern));
}

static bool *
reachable_blocks(ir_func_t *function)
{
    uint32_t count = function->n_blocks;
    ir_block_t *entry = ir_block_entry(function);
    if (!count || !entry) return nullptr;
    bool *seen = core_alloc_array(bool, count);
    ir_block_t **stack = core_alloc_array(ir_block_t *, count);
    size_t depth = 0;
    seen[entry->index] = true;
    stack[depth++] = entry;
    while (depth) {
        ir_instr_t *term = ir_block_terminator(stack[--depth]);
        for (uint32_t s = 0; term && s < ir_succ_count(term); s++) {
            ir_block_t *next = ir_succ_at(term, s);
            if (!next || seen[next->index]) continue;
            seen[next->index] = true;
            stack[depth++] = next;
        }
    }
    core_free(stack);
    return seen;
}

void
n00bb_drop_unreachable_blocks(ir_func_t *function)
{
    uint32_t count = function->n_blocks;
    bool *seen = reachable_blocks(function);
    if (!seen) return;
    for (uint32_t b = 0; b < count; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; seen[b] && i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];
            if (in->op != IR_OP_PHI) continue;
            uint32_t kept = 0;
            for (uint32_t o = 0; o < in->n_operands; o++) {
                ir_operand_t *arm = &in->operands[o];
                if (arm->kind == IR_OPD_PHI_ARM && arm->phi.block
                    && !seen[arm->phi.block->index]) continue;
                in->operands[kept++] = *arm;
            }
            if (kept != in->n_operands) {
                in->n_operands = kept;
                ir_instr_uses_changed(in);
            }
        }
    }
    for (uint32_t b = count; b-- > 0;) {
        ir_block_t *block = function->blocks[b];
        if (seen[b]) continue;
        while (block->n_instrs
               && ir_instr_erase(block->instrs[block->n_instrs - 1])) {}
        (void)ir_block_erase(block);
    }
    core_free(seen);
}

static ir_instr_t *
branch_type_test(ir_block_t *block)
{
    ir_instr_t *term = block ? ir_block_terminator(block) : nullptr;
    ir_value_t *condition = term && term->op == IR_OP_BRANCH
            && term->n_operands == 3
            && term->operands[0].kind == IR_OPD_VALUE
        ? term->operands[0].value : nullptr;
    ir_instr_t *test = condition && condition->kind == IR_VAL_INSTR
        ? condition->def : nullptr;
    return test && test->op == IR_OP_TYPE_TEST ? test : nullptr;
}

static uint64_t
annotated_int(const ir_annots_t *annots, core_string_t key)
{
    ir_annot_t *seen = ir_annot_get((ir_annots_t *)annots, key);
    return seen && seen->kind == IR_ANNOT_INT ? (uint64_t)seen->i.mag : 0;
}

static void
note_outcome(ir_ctx_t *context, ir_func_t *origin, uint32_t block,
             uint64_t outcomes)
{
    ir_instr_t *test = block < origin->n_blocks
        ? branch_type_test(origin->blocks[block]) : nullptr;
    if (test)
        (void)ir_annot_set(context, &test->annots, (ir_annot_t){
            .key = CORE_STRING_STATIC(N00BB_TYPE_TEST_OUTCOMES_KEY),
            .kind = IR_ANNOT_INT,
            .i = {.mag = outcomes | annotated_int(&test->annots,
                CORE_STRING_STATIC(N00BB_TYPE_TEST_OUTCOMES_KEY))}});
}

uint64_t
n00bb_type_test_outcomes(const ir_instr_t *test)
{
    return annotated_int(&test->annots,
                         CORE_STRING_STATIC(N00BB_TYPE_TEST_OUTCOMES_KEY));
}

bool
n00bb_instantiated(const ir_func_t *function)
{
    return annotated_int(&function->annots,
                         CORE_STRING_STATIC(N00BB_TYPE_TEST_OUTCOMES_KEY))
        != 0;
}

bool
n00bb_fold_static_branches(ir_func_t *function, ir_func_t *origin,
                           ir_ctx_t *context)
{
    bool folded = false;
    uint32_t count = function->n_blocks;
    uint8_t *outcomes = core_alloc_array(uint8_t, count + 1);
    for (uint32_t b = 0; b < count; b++) {
        ir_instr_t *term = ir_block_terminator(function->blocks[b]);
        ir_instr_t *test = branch_type_test(function->blocks[b]);
        int decided = test ? n00bb_static_type_test(test) : -1;
        outcomes[b] = !test ? 0 : decided < 0 ? 3 : decided ? 2 : 1;
        ir_block_t *next = decided >= 0
            ? ir_succ_at(term, decided ? 0 : 1) : nullptr;
        if (!next) continue;
        term->op = IR_OP_JUMP;
        term->n_operands = 0;
        folded = ir_instr_operand_block(term, next) || folded;
    }
    bool *reached = origin ? reachable_blocks(function) : nullptr;
    for (uint32_t b = 0; reached && b < count; b++)
        if (reached[b] && outcomes[b])
            note_outcome(context, origin, b, outcomes[b]);
    if (origin)
        (void)ir_annot_set(context, &origin->annots, (ir_annot_t){
            .key = CORE_STRING_STATIC(N00BB_TYPE_TEST_OUTCOMES_KEY),
            .kind = IR_ANNOT_INT, .i = {.mag = 1}});
    core_free(reached);
    core_free(outcomes);
    if (folded) n00bb_drop_unreachable_blocks(function);
    th_ctx *types = ir_module_types(function->owner);
    core_dict_t replacements;
    core_dict_init(&replacements, nullptr, nullptr);
    bool replaced = false;
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            ir_instr_t *narrow = function->blocks[b]->instrs[i];
            ir_value_t *source = narrow->op == IR_OP_TYPE_NARROW
                    && narrow->dest && narrow->n_operands == 2
                    && narrow->operands[0].kind == IR_OPD_VALUE
                ? ir_value_replacement(&replacements,
                                       narrow->operands[0].value)
                : nullptr;
            if (!source || !th_is_closed(types, source->type)
                || th_is_closed(types, narrow->dest->type)) continue;
            core_dict_put(&replacements, narrow->dest, source);
            replaced = true;
        }
    if (replaced) ir_func_replace_values(function, &replacements);
    core_dict_free(&replacements);
    return folded || replaced;
}
