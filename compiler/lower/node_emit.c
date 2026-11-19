#include "compiler_tu.h"

#define S(text) CORE_STRING_STATIC(text)

static void
emit_set(n00bb_emit_t *emit, core_string_t action, size_t count,
         uint32_t first, uint32_t second)
{
    emit->action = action;
    emit->operand_count = count;
    emit->operands[0] = first;
    emit->operands[1] = second;
}

static void
emit_literal(n00bb_emit_t *emit, core_string_t action, core_string_t kind)
{
    emit_set(emit, action, 0, 0, 0);
    emit->literal = kind;
}

static bool
child_is(parse_tree_t *node, size_t index, core_string_t text)
{
    parse_tree_t *child = index < core_tree_num_children(node)
        ? core_tree_child(node, index) : nullptr;
    return child && core_tree_is_leaf(child)
        && parse_tree_leaf_is(child, text);
}

static bool
child_named(parse_grammar_t *grammar, parse_tree_t *node, size_t index,
         core_string_t name)
{
    return index < core_tree_num_children(node)
        && nt_is(grammar, core_tree_child(node, index), name);
}

static void
emit_simple_literal(parse_grammar_t *grammar, parse_tree_t *node,
                    n00bb_emit_t *emit)
{
    if (child_named(grammar, node, 0, S("bool-lit"))) {
        emit_literal(emit, S("literal"), S("bool"));
        return;
    }
    if (child_named(grammar, node, 0, S("nil-keyword"))) {
        emit_literal(emit, S("nil_literal"), S("nil"));
        return;
    }
    parse_token_info_t *token = parse_tree_find_first_terminal(node);
    core_string_t terminal = token
        ? parse_terminal_name(grammar, parse_token_tid(token))
        : (core_string_t){0};
    if (core_string_eq(terminal, S("INT_LIT"))
        || core_string_eq(terminal, S("HEX_LIT")))
        emit_literal(emit, S("literal"), S("int"));
    else if (core_string_eq(terminal, S("FLOAT_LIT")))
        emit_literal(emit, S("literal"), S("f64"));
    else if (core_string_eq(terminal, S("STRING_LIT")))
        emit_set(emit, S("string_literal"), 0, 0, 0);
    else if (core_string_eq(terminal, S("CHAR_LIT")))
        emit_literal(emit, S("char_literal"), S("char"));
}

static void
emit_unary(parse_grammar_t *grammar, parse_tree_t *node, n00bb_emit_t *emit)
{
    if (child_named(grammar, node, 0, S("not-op"))
        || child_named(grammar, node, 0, S("not-word-op")))
        emit_set(emit, S("bool_not"), 1, 1, 0);
    else if (child_is(node, 0, S("~")))
        emit_set(emit, S("complement"), 1, 1, 0);
    else if (child_is(node, 0, S("-"))) emit_set(emit, S("negate"), 1, 1, 0);
    else if (child_is(node, 0, S("+")))
        emit_set(emit, S("identity"), 1, 1, 0);
    else if (child_is(node, 0, S("cast"))) emit_set(emit, S("cast"), 1, 4, 0);
    else if (child_is(node, 0, S("unsafe_cast")))
        emit_set(emit, S("unsafe_cast"), 1, 4, 0);
    else if (child_is(node, 1, S(":")))
        emit_set(emit, S("type_annotation"), 1, 0, 0);
    else if (core_tree_num_children(node) == 1
             && !core_tree_is_leaf(core_tree_child(node, 0)))
        emit_set(emit, S("identity"), 1, 0, 0);
}

static void
emit_postfix(parse_tree_t *node, bool place, n00bb_emit_t *emit)
{
    if (core_tree_num_children(node) == 1) {
        if (core_tree_is_leaf(core_tree_child(node, 0)))
            emit_set(emit, S("reference"), 0, 0, 0);
        else if (!place) emit_set(emit, S("identity"), 1, 0, 0);
        return;
    }
    if (place && child_is(node, 0, S("(")))
        emit_set(emit, S("identity"), 1, 1, 0);
    else if (child_is(node, 1, S(".")))
        emit_set(emit, S("module_member"), 0, 0, 0);
    else if (child_is(node, 1, S("[")) || child_is(node, 1, S("(")))
        emit_set(emit, S("call"), 0, 0, 0);
}

bool
n00bb_node_emit(parse_grammar_t *grammar, parse_tree_t *node,
                n00bb_emit_t *emit)
{
    *emit = (n00bb_emit_t){0};
    if (!grammar || !node || core_tree_is_leaf(node)) return false;
    size_t count = core_tree_num_children(node);
    switch (n00bb_rule_of(grammar, node)) {
    case RULE(EQUALITY_EXPR): case RULE(COMPARE_EXPR): case RULE(BITOR_EXPR):
    case RULE(BITXOR_EXPR): case RULE(BITAND_EXPR): case RULE(SHIFT_EXPR):
    case RULE(ADD_EXPR): case RULE(MUL_EXPR):
        if (count == 3) emit_set(emit, S("binary"), 2, 0, 2);
        else if (count == 1) emit_set(emit, S("identity"), 1, 0, 0);
        break;
    case RULE(OR_EXPR): case RULE(AND_EXPR):
        if (count == 1) emit_set(emit, S("identity"), 1, 0, 0);
        break;
    case RULE(EXPRESSION): case RULE(OR_RHS): case RULE(AND_RHS):
        emit_set(emit, S("identity"), 1, 0, 0);
        break;
    case RULE(UNARY_EXPR): emit_unary(grammar, node, emit); break;
    case RULE(POSTFIX_EXPR): emit_postfix(node, false, emit); break;
    case RULE(PLACE): emit_postfix(node, true, emit); break;
    case RULE(PRIMARY):
        if (count == 1 && core_tree_is_leaf(core_tree_child(node, 0)))
            emit_set(emit, S("reference"), 0, 0, 0);
        else if (count == 1) emit_set(emit, S("identity"), 1, 0, 0);
        break;
    case RULE(SIMPLE_LIT): emit_simple_literal(grammar, node, emit); break;
    case RULE(BOOL_LIT): emit_literal(emit, S("literal"), S("bool")); break;
    case RULE(LIST_LIT): emit_set(emit, S("sequence_literal"), 0, 0, 0); break;
    case RULE(DICT_OR_SET_LIT):
        emit_set(emit, S("brace_literal"), 0, 0, 0);
        break;
    case RULE(TYPEOF_EXPR): emit_set(emit, S("typeof"), 1, 2, 0); break;
    case RULE(TYPESPEC_EXPR):
        emit_set(emit, S("typespec_literal"), 0, 0, 0);
        break;
    case RULE(EFFECT_QUERY): emit_set(emit, S("effect_query"), 0, 0, 0); break;
    case RULE(NEW_EXPR): emit_set(emit, S("array_new"), 0, 0, 0); break;
    case RULE(CALLBACK_EXPR):
        emit_set(emit, S("callback_create"), 0, 0, 0);
        break;
    case RULE(RETURN_STMT):
        emit_set(emit, S("return"), count > 1 ? 1 : 0, 1, 0);
        break;
    case RULE(BINOP_ASSIGN_STMT):
        emit_set(emit, S("compound_assign"), 2, 0, 2);
        break;
    default:
        break;
    }
    return emit->action.data != nullptr;
}
