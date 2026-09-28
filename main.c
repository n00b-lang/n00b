#include <stdio.h>
#include <pthread.h>
#include <string.h>

#include "driver.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/dict.h"
#include "lib/list.h"
#include "lib/work_queue.h"
#include "lib/diagnostic.h"
#include "parse/commander.h"
#include "parse/bnf.h"
#include "parse/grammar.h"
#include "parse/pwz.h"
#include "parse/tree_dump.h"
#include "parse/tree.h"
#include "parse/tree_util.h"
#include "parse/annot_walk.h"
#include "ir/libir.h"
#include "ir/ir.h"
#include "ir/typelang.h"
#include "th/environment.h"
#include "scanner/token_stream.h"
#include "scanner/tokenizer.h"
#include "scanner/lex.h"
#include "preprocessor.h"

enum { N00BB_JOB_LOCATE_TU = 1, N00BB_JOB_PREPROCESS = 2 };

typedef ir_value_t *n00bb_ir_value_ptr_t;
core_list_decl(n00bb_ir_value_ptr_t);
typedef parse_tree_t *n00bb_tree_ptr_t;
core_list_decl(n00bb_tree_ptr_t);
typedef struct {
    parse_tree_t *node;
    size_t parameter_index;
} n00bb_call_actual_t;
core_list_decl(n00bb_call_actual_t);

static const ir_scalar_t N00BB_SCALARS[] = {
    {"int", IR_CLASS_SINT, 64, IR_ROLE_NONE},
    {"uint", IR_CLASS_UINT, 64, IR_ROLE_NONE},
    {"bool", IR_CLASS_BOOL, 1, IR_ROLE_NONE},
    {"f64", IR_CLASS_FLOAT, 64, IR_ROLE_NONE},
    {"nil", IR_CLASS_VOID, 0, IR_ROLE_NONE},
};

static const ir_profile_t N00BB_PROFILE = {
    .name = "n00bb",
    .n_scalars = sizeof(N00BB_SCALARS) / sizeof(N00BB_SCALARS[0]),
    .scalars = N00BB_SCALARS,
};

typedef struct {
    core_work_job_t *job;
    char *operand;
    bool root_input;
    core_module_resolve_status_t resolve_status;
    bool resolve_attempted;
} locate_request_t;
core_list_decl(locate_request_t);

typedef core_work_job_t *core_work_job_ptr_t;
core_list_decl(core_work_job_ptr_t);
core_list_decl(size_t);
core_list_decl(th_param);
core_list_decl(th_kwparam);
core_list_decl(th_member);
core_list_decl(th_arg);
typedef th_type *th_type_ptr_t;
core_list_decl(th_type_ptr_t);
typedef core_translation_unit_t *core_translation_unit_ptr_t;
core_list_decl(core_translation_unit_ptr_t);
typedef parse_scope_t *parse_scope_ptr_t;
core_list_decl(parse_scope_ptr_t);

static bool n00bb_nil_type(th_ctx *types, th_type *type);
static bool context_symbol(parse_grammar_t *grammar,
                           parse_sym_entry_t *symbol);
static bool context_hook_ir_name(parse_grammar_t *grammar,
                                 parse_sym_entry_t *symbol,
                                 parse_tree_t *hook,
                                 char *buffer, size_t buffer_size);
static parse_tree_t *n00bb_context_hook(parse_grammar_t *grammar,
                                        parse_sym_entry_t *symbol,
                                        const char *name);
static bool n00bb_emit_with_success(ir_lower_context_t *context,
                                    parse_tree_t *exit_node);
static bool n00bb_emit_with_failure(ir_lower_context_t *context,
                                    parse_tree_t *exit_node);
static parse_tree_t *n00bb_simple_lvalue(parse_tree_t *node);
static ir_value_t *n00bb_emit_outcome_payload(ir_lower_context_t *context,
                                               ir_value_t *outcome,
                                               const char *arm,
                                               th_type *type,
                                               ir_span_t span);
static parse_sym_entry_t *formal_symbol_at(parse_grammar_t *grammar,
                                            parse_tree_t *node,
                                            size_t target, size_t *index);
static parse_sym_entry_t *formal_symbol_named(parse_grammar_t *grammar,
                                               parse_tree_t *formals,
                                               core_string_t name,
                                               size_t *index);
static void collect_keyword_nodes(parse_grammar_t *grammar,
                                  parse_tree_t *node,
                                  core_list_t(n00bb_tree_ptr_t) *arguments);
static bool n00bb_default_references(parse_grammar_t *grammar,
                                    parse_tree_t *node,
                                    parse_sym_entry_t *symbol);

typedef struct {
    parse_token_stream_t *tokens;
    parse_pwz_parser_t *parser;
} n00bb_lex_result_t;

typedef struct {
    n00bb_lex_result_t *lex;
    parse_annot_result_t *annotations;
    parse_tree_walk_t *annotation_walk;
    parse_tree_walk_t *reference_walk;
    parse_tree_t *annotation_root;
    th_ctx *types;
    ir_module_t *ir_module;
    bool inference_finalized;
} n00bb_tu_state_t;

typedef struct {
    parse_tree_t *condition;
    parse_tree_t *body;
    ir_block_t *test_block;
    ir_block_t *body_block;
} n00bb_branch_arm_t;
core_list_decl(n00bb_branch_arm_t);

typedef struct {
    parse_tree_t *node;
    core_list_t(n00bb_branch_arm_t) arms;
    parse_tree_t *else_body;
    ir_block_t *join;
    ir_block_t *else_block;
    ir_local_t *result_local;
    bool expression;
    bool join_reachable;
    bool active;
} n00bb_branch_frame_t;
core_list_decl(n00bb_branch_frame_t);

typedef struct {
    parse_tree_t *node;
    core_string_t label;
    parse_tree_t *condition;
    parse_tree_t *body;
    parse_tree_t *range_start;
    parse_tree_t *range_end;
    ir_block_t *header;
    ir_block_t *body_block;
    ir_block_t *latch;
    ir_block_t *increment;
    ir_block_t *exit;
    ir_block_t *continue_block;
    ir_local_t *iterator;
    parse_sym_entry_t *iterator_symbol;
    bool range_inclusive;
    bool is_range;
    uint64_t order;
    bool active;
} n00bb_loop_frame_t;
core_list_decl(n00bb_loop_frame_t);

core_list_decl(ir_lit_t);

typedef struct {
    ir_lit_t lower;
    ir_lit_t upper;
    parse_tree_t *expression;
    parse_sym_entry_t *variant_arm;
    core_string_t variant_arm_name;
    bool is_range;
    bool inclusive;
    bool is_variant_tag;
} n00bb_switch_pattern_t;
core_list_decl(n00bb_switch_pattern_t);

typedef struct {
    parse_tree_t *body;
    ir_block_t *block;
    core_list_t(n00bb_switch_pattern_t) patterns;
} n00bb_switch_case_t;
core_list_decl(n00bb_switch_case_t);

typedef struct {
    parse_tree_t *node;
    core_string_t label;
    parse_tree_t *scrutinee;
    parse_sym_entry_t *variant_subject;
    parse_scope_t *variant_scope;
    core_list_t(n00bb_switch_case_t) cases;
    parse_tree_t *else_body;
    ir_block_t *default_block;
    ir_block_t *join;
    uint64_t order;
    ir_local_t *result_local;
    bool expression;
    bool variant_switch;
    bool emitted;
    bool join_reachable;
    bool active;
} n00bb_switch_frame_t;
core_list_decl(n00bb_switch_frame_t);

typedef struct {
    parse_tree_t *body;
    th_type *type;
    ir_block_t *block;
} n00bb_typecase_arm_t;
core_list_decl(n00bb_typecase_arm_t);

typedef struct {
    parse_tree_t *node;
    core_string_t label;
    parse_tree_t *subject_node;
    parse_sym_entry_t *subject_symbol;
    ir_value_t *subject_value;
    core_list_t(n00bb_typecase_arm_t) arms;
    parse_tree_t *else_body;
    ir_block_t *else_block;
    ir_block_t *unmatched_block;
    ir_block_t *join;
    void *binding_checkpoint;
    uint64_t order;
    bool emitted;
    bool join_reachable;
    bool active;
} n00bb_typecase_frame_t;
core_list_decl(n00bb_typecase_frame_t);

typedef struct {
    parse_tree_t *node;
    ir_local_t *result;
    ir_block_t *continuation;
    bool active;
    bool value_written;
} n00bb_value_block_frame_t;
core_list_decl(n00bb_value_block_frame_t);

typedef struct {
    parse_tree_t *node;
    parse_tree_t *body;
    parse_tree_t *error_clause;
    parse_sym_entry_t *error_symbol;
    ir_block_t *handler;
    ir_block_t *join;
    ir_local_t *error_value;
    ir_local_t *result_value;
    bool expression;
    bool error_reachable;
    bool join_reachable;
    bool active;
} n00bb_try_frame_t;
core_list_decl(n00bb_try_frame_t);

typedef struct {
    parse_sym_entry_t *context_symbol;
    core_translation_unit_t *context_unit;
    parse_tree_t *call_node;
    core_list_t(n00bb_ir_value_ptr_t) captures;
    ir_value_t *provided_value;
    bool active;
} n00bb_with_item_t;
core_list_decl(n00bb_with_item_t);

typedef struct {
    parse_tree_t *node;
    parse_tree_t *body;
    core_list_t(n00bb_with_item_t) items;
    bool active;
} n00bb_with_frame_t;
core_list_decl(n00bb_with_frame_t);

typedef struct {
    parse_sym_entry_t *symbol;
    ir_local_t *local;
    ir_value_t *address;
} n00bb_copyout_t;
core_list_decl(n00bb_copyout_t);

typedef struct {
    parse_sym_entry_t *symbol;
    parse_tree_t *expression;
    ir_local_t *local;
    ir_value_t *argument;
    bool initialized;
} n00bb_default_param_t;
core_list_decl(n00bb_default_param_t);

typedef struct {
    ir_value_t *outcome;
    core_list_t(n00bb_copyout_t) items;
} n00bb_pending_copyouts_t;
core_list_decl(n00bb_pending_copyouts_t);

typedef struct {
    parse_grammar_t *grammar;
    ir_lowerer_t *lowerer;
    core_list_t(core_translation_unit_ptr_t) *units;
    core_translation_unit_t *unit;
    parse_tree_t *function_body;
    parse_sym_entry_t *function_symbol;
    core_list_t(n00bb_branch_frame_t) branches;
    core_list_t(n00bb_loop_frame_t) loops;
    core_list_t(n00bb_switch_frame_t) switches;
    core_list_t(n00bb_typecase_frame_t) typecases;
    core_list_t(n00bb_value_block_frame_t) value_blocks;
    core_list_t(n00bb_try_frame_t) tries;
    core_list_t(n00bb_with_frame_t) withs;
    core_list_t(n00bb_copyout_t) parameters_to_copy_out;
    core_list_t(n00bb_pending_copyouts_t) pending_call_copyouts;
    core_list_t(n00bb_default_param_t) default_params;
    uint64_t next_block_id;
    uint64_t next_control_order;
} n00bb_lower_context_t;

static bool
n00bb_tree_contains(parse_tree_t *ancestor, parse_tree_t *node)
{
    for (parse_tree_t *current = node; current;
         current = core_tree_node_value(current).parent)
        if (current == ancestor) return true;
    return false;
}

typedef struct {
    core_work_queue_t *queue;
    n00bb_resolver_t *resolver;
    core_list_t(locate_request_t) *requests;
    pthread_mutex_t *requests_lock;
    parse_grammar_t *grammar;
    th_ctx *types;
    bool lex;
    bool parse;
} locate_worker_context_t;

typedef struct {
    core_work_queue_t *queue;
    n00bb_resolver_t *resolver;
    core_list_t(locate_request_t) *requests;
    pthread_mutex_t *requests_lock;
    locate_worker_context_t *loader;
    size_t worker_count;
    parse_grammar_t *grammar;
    n00bb_preprocessor_t *preprocessor;
    th_ctx *types;
} import_driver_t;

typedef struct {
    parse_grammar_t *grammar;
    th_ctx *types;
} annotation_context_t;

typedef struct {
    parse_grammar_t *grammar;
    core_translation_unit_t *const *units;
    size_t unit_count;
} module_inference_context_t;

static void
free_tu_state(void *data)
{
    n00bb_tu_state_t *state = data;
    if (state) {
        parse_tree_walk_free(state->annotation_walk);
        parse_tree_walk_free(state->reference_walk);
        parse_annot_result_free(state->annotations);
        if (state->lex) {
            parse_pwz_free(state->lex->parser);
            scan_token_stream_free(state->lex->tokens);
            core_free(state->lex);
        }
        core_free(state);
    }
}

static n00bb_tu_state_t *
tu_state(core_translation_unit_t *unit)
{
    return unit ? core_translation_unit_user_data(unit) : nullptr;
}

static bool
nt_is(parse_grammar_t *grammar, parse_tree_t *node, const char *name)
{
    return node && !core_tree_is_leaf(node)
        && core_string_eq(parse_tree_nt_name(grammar, node),
                          core_string_from_cstr(name));
}

static parse_tree_t *
child_nt(parse_grammar_t *grammar, parse_tree_t *node, const char *name)
{
    return parse_tree_find_child_by_nt_name(grammar, node,
                                             core_string_from_cstr(name));
}

static parse_tree_t *
descendant_nt(parse_grammar_t *grammar, parse_tree_t *node, const char *name)
{
    parse_tree_t *found = child_nt(grammar, node, name);
    if (found) return found;
    if (!node || core_tree_is_leaf(node)) return nullptr;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        found = descendant_nt(grammar, core_tree_child(node, i), name);
        if (found) return found;
    }
    return nullptr;
}

static core_string_t
first_token_text(parse_tree_t *node)
{
    parse_token_info_t *token = parse_tree_find_first_terminal(node);
    return token && core_option_is_set(token->value)
        ? core_option_get(token->value) : (core_string_t){0};
}

static core_string_t
n00bb_control_label(parse_grammar_t *grammar, parse_tree_t *node,
                    const char *labeled_form)
{
    parse_tree_t *parent = node
        ? core_tree_node_value(node).parent : nullptr;
    if (!nt_is(grammar, parent, labeled_form)) return (core_string_t){0};
    return first_token_text(child_nt(grammar, parent, "label"));
}

static void
apply_decl_binding(parse_grammar_t *grammar, parse_tree_t *node,
                   parse_sym_mutability_t mutability, bool threaded,
                   parse_annot_result_t *annotations)
{
    if (!node || core_tree_is_leaf(node)) return;
    if (nt_is(grammar, node, "sym-decl")) {
        parse_sym_entry_t *symbol = core_tree_node_value(node).sym;
        if (symbol) {
            symbol->mutability = mutability;
            symbol->is_thread_local = threaded;
            if (!child_nt(grammar, node, "type-spec"))
                parse_annot_result_mark_inferred(annotations, symbol);
        }
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        apply_decl_binding(grammar, core_tree_child(node, i), mutability,
                           threaded, annotations);
}

static size_t
terminal_count(parse_tree_t *node)
{
    if (!node) return 0;
    if (core_tree_is_leaf(node)) return 1;
    size_t count = 0;
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        count += terminal_count(core_tree_child(node, i));
    return count;
}

static parse_scope_t *
inferred_binding_scope(parse_scope_t *scope)
{
    for (parse_scope_t *s = scope; s; s = s->parent) {
        if (core_string_eq(s->scope_tag, CORE_STRING_STATIC("function"))
            || core_string_eq(s->scope_tag, CORE_STRING_STATIC("module")))
            return s;
    }
    return scope;
}

static void
initial_symbol_node(parse_visit_t *visit, bool entering, void *context)
{
    if (entering || !visit->pn) return;
    parse_annot_result_t *annotations = context;

    if (nt_is(visit->grammar, visit->node, "variable-decl")) {
        parse_tree_t *prefix = child_nt(visit->grammar, visit->node,
                                        "declaration-prefix");
        parse_tree_t *kind = child_nt(visit->grammar, prefix, "binding-kind");
        parse_tree_t *storage = child_nt(visit->grammar, prefix,
                                         "storage-class");
        core_string_t spelling = first_token_text(kind);
        parse_sym_mutability_t mutability =
            core_string_eq(spelling, CORE_STRING_STATIC("const"))
                ? PARSE_SYM_CONST
                : core_string_eq(spelling, CORE_STRING_STATIC("let"))
                    ? PARSE_SYM_IMMUTABLE : PARSE_SYM_MUTABLE;
        bool threaded = core_string_eq(first_token_text(storage),
                                       CORE_STRING_STATIC("thread"));
        for (size_t i = 0; i < core_tree_num_children(visit->node); i++)
            apply_decl_binding(visit->grammar,
                core_tree_child(visit->node, i), mutability, threaded,
                annotations);
    }
    else if (nt_is(visit->grammar, visit->node, "pos-param")
             || nt_is(visit->grammar, visit->node, "default-param")
             || nt_is(visit->grammar, visit->node, "vargs-param")) {
        if (visit->pn->sym) {
            visit->pn->sym->is_optional = nt_is(visit->grammar, visit->node,
                                                "default-param");
            visit->pn->sym->is_variadic = nt_is(visit->grammar, visit->node,
                                                "vargs-param");
            if (visit->pn->sym->has_mode
                && visit->pn->sym->mode != TH_IN)
                visit->pn->sym->mutability = PARSE_SYM_MUTABLE;
        }
        if (visit->pn->sym
            && !child_nt(visit->grammar, visit->node, "type-spec"))
            parse_annot_result_mark_inferred(annotations, visit->pn->sym);
    }
    else if (nt_is(visit->grammar, visit->node, "func-def")) {
        if (visit->pn->sym)
            parse_annot_result_mark_inferred(annotations, visit->pn->sym);
    }
    else if (nt_is(visit->grammar, visit->node, "assign-stmt")) {
        parse_tree_t *target = core_tree_child(visit->node, 0);
        parse_token_info_t *token = parse_tree_find_first_terminal(target);
        if (!target || terminal_count(target) != 1 || !token
            || token->tid != PARSE_TOK_IDENTIFIER) return;
        core_string_t name = parse_tree_extract_first_identifier(target);
        parse_namespace_t *ns = parse_symtab_ns(annotations->symtab,
                                                 CORE_STRING_STATIC(""));
        parse_scope_t *current = ns ? ns->current : nullptr;
        if (!name.data || !current
            || parse_symtab_lookup_scoped(annotations->symtab, current,
                CORE_STRING_STATIC(""), name)) return;

        parse_scope_t *destination = inferred_binding_scope(current);
        parse_symtab_enter_scope(annotations->symtab, CORE_STRING_STATIC(""),
                                 destination);
        parse_sym_entry_t *symbol = parse_symtab_add(annotations->symtab,
            CORE_STRING_STATIC(""), name, PARSE_SYM_VARIABLE, visit->node);
        parse_symtab_enter_scope(annotations->symtab, CORE_STRING_STATIC(""),
                                 current);
        if (symbol) {
            symbol->mutability = PARSE_SYM_MUTABLE;
            parse_tree_t *value = core_tree_child(visit->node, 2);
            if (value && !core_tree_is_leaf(value))
                symbol->type = core_tree_node_value(value).type;
            parse_annot_result_mark_inferred(annotations, symbol);
            visit->pn->sym = symbol;
        }
    }
}

static void
collect_function_params(parse_grammar_t *grammar, parse_tree_t *node,
                        th_ctx *types, core_list_t(th_param) *params,
                        th_param *rest, bool *has_rest)
{
    if (!node || core_tree_is_leaf(node)) return;
    bool positional = nt_is(grammar, node, "pos-param")
        || nt_is(grammar, node, "default-param");
    bool variadic = nt_is(grammar, node, "vargs-param");
    if (positional || variadic) {
        parse_sym_entry_t *symbol = core_tree_node_value(node).sym;
        parse_nt_node_t *parameter = &core_tree_node_value(node);
        th_type *type = symbol && symbol->type ? symbol->type
            : parameter->type ? parameter->type : th_var(types);
        if (variadic && symbol && symbol->type) {
            th_type *resolved = th_resolve(symbol->type);
            if (th_kind_of(resolved) == TH_SEQUENCE)
                type = th_seq_elem(resolved);
        }
        if (!variadic && parameter->type && parameter->type != type) {
            th_type *unified = th_unify_in_place(types, type,
                                                 parameter->type);
            if (unified) type = unified;
        }
        if (variadic) {
            th_type *binding = symbol ? th_resolve(symbol->type) : nullptr;
            if (symbol && th_kind_of(binding) != TH_SEQUENCE)
                symbol->type = th_array(types, type);
            parameter->type = symbol ? symbol->type : th_array(types, type);
            *rest = (th_param){.mode = TH_IN, .type = type};
            *has_rest = true;
        }
        else {
            parameter->type = type;
            if (symbol) symbol->type = type;
            core_list_push(*params, ((th_param){
                .mode = symbol && symbol->has_mode ? symbol->mode : TH_IN,
                .type = type,
            }));
        }
        return;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        collect_function_params(grammar, core_tree_child(node, i), types,
                                params, rest, has_rest);
}

static void
collect_function_exits(parse_grammar_t *grammar, parse_tree_t *node,
                       parse_tree_t *function, th_ctx *types,
                       th_type **success, th_type **failure)
{
    if (!node || core_tree_is_leaf(node)) return;
    if (node != function && nt_is(grammar, node, "func-def")) return;
    parse_tree_t *value = nullptr;
    th_type **channel = nullptr;
    if (nt_is(grammar, node, "return-stmt")) {
        value = child_nt(grammar, node, "expression");
        channel = success;
    }
    else if (nt_is(grammar, node, "fail-stmt")) {
        value = child_nt(grammar, node, "expression");
        channel = failure;
    }
    if (value && channel) {
        th_type *observed = core_tree_node_value(value).type;
        if (observed) {
            if (!*channel) *channel = observed;
            else {
                th_type *unified = th_unify_in_place(types, *channel,
                                                    observed);
                if (unified) *channel = unified;
            }
        }
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        collect_function_exits(grammar, core_tree_child(node, i), function,
                               types, success, failure);
}

static bool
type_word_byte(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}

static bool
append_node_tokens(parse_tree_t *node, core_buffer_t *buffer)
{
    if (!node) return false;
    if (core_tree_is_leaf(node)) {
        core_string_t text = parse_tree_leaf_text(node);
        if (!text.data || !text.u8_bytes) return false;
        if (buffer->byte_len && type_word_byte(
                (unsigned char)buffer->data[buffer->byte_len - 1])
            && type_word_byte((unsigned char)text.data[0]))
            core_buffer_putc(buffer, ' ');
        core_buffer_append(buffer, text.data, text.u8_bytes);
        return true;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!append_node_tokens(core_tree_child(node, i), buffer))
            return false;
    return true;
}

static parse_scope_t *
unit_variant_scope(core_translation_unit_t *unit, core_string_t name,
                   core_translation_unit_t **owner)
{
    core_translation_unit_t *target = unit;
    const char *lookup = name.data;
    size_t lookup_len = name.u8_bytes;
    size_t dot = lookup_len;
    while (dot && lookup[dot - 1] != '.') dot--;
    if (dot) {
        core_string_t module = core_string_borrow(lookup, dot - 1);
        lookup += dot;
        lookup_len -= dot;
        if (!core_string_eq(module, unit->module.identifier)) {
            target = nullptr;
            for (size_t i = 0; i < core_translation_unit_import_count(unit);
                 i++) {
                core_translation_unit_t *imported =
                    core_translation_unit_import(unit, i);
                if (core_string_eq(module, imported->module.identifier)) {
                    target = imported;
                    break;
                }
            }
        }
    }
    if (!target) return nullptr;
    n00bb_tu_state_t *state = tu_state(target);
    parse_symtab_t *symtab = state && state->annotations
        ? state->annotations->symtab : nullptr;
    if (!symtab) return nullptr;
    for (parse_scope_t *scope = symtab->all_scopes; scope;
         scope = scope->all_next)
        if (core_string_eq(scope->scope_tag,
                            CORE_STRING_STATIC("variant"))
            && scope->name.u8_bytes == lookup_len
            && !memcmp(scope->name.data, lookup, lookup_len)) {
            if (owner) *owner = target;
            return scope;
        }
    return nullptr;
}

static parse_scope_t *
unit_named_type_scope(parse_grammar_t *grammar,
                      core_translation_unit_t *unit, core_string_t name)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_symtab_t *symtab = state && state->annotations
        ? state->annotations->symtab : nullptr;
    if (!symtab) return nullptr;
    const char *lookup = name.data;
    size_t lookup_len = name.u8_bytes;
    size_t dot = lookup_len;
    while (dot && lookup[dot - 1] != '.') dot--;
    if (dot) {
        core_string_t module = core_string_borrow(lookup, dot - 1);
        if (!core_string_eq(module, unit->module.identifier))
            return nullptr;
        lookup += dot;
        lookup_len -= dot;
    }
    for (parse_scope_t *scope = symtab->all_scopes; scope;
         scope = scope->all_next) {
        if (!core_string_eq(scope->scope_tag, CORE_STRING_STATIC("type"))
            || !scope->node || !nt_is(grammar, scope->node,
                                      "type-decl")) continue;
        if (scope->name.u8_bytes == lookup_len
            && !memcmp(scope->name.data, lookup, lookup_len)) return scope;
    }
    return nullptr;
}

static void
append_module_type_name(core_translation_unit_t *unit, core_string_t name,
                        core_buffer_t *buffer)
{
    core_buffer_printf(buffer, "%.*s.%.*s",
        (int)unit->module.identifier.u8_bytes, unit->module.identifier.data,
        (int)name.u8_bytes, name.data);
}

static void
append_module_symbol_name(core_translation_unit_t *unit,
                          parse_sym_entry_t *symbol, core_buffer_t *buffer)
{
    core_buffer_printf(buffer, "%.*s.%.*s",
        (int)unit->module.identifier.u8_bytes, unit->module.identifier.data,
        (int)symbol->name.u8_bytes, symbol->name.data);
}

static bool
append_typelang_type(parse_grammar_t *grammar, core_translation_unit_t *unit,
                     parse_tree_t *node,
                     core_buffer_t *buffer)
{
    if (!node) return false;
    if (nt_is(grammar, node, "member-chain")) {
        core_buffer_t path = {0};
        bool ok = append_node_tokens(node, &path);
        core_string_t name = core_string_borrow(path.data, path.byte_len);
        parse_scope_t *record = ok
            ? unit_named_type_scope(grammar, unit, name) : nullptr;
        core_translation_unit_t *variant_owner = nullptr;
        parse_scope_t *variant = ok
            ? unit_variant_scope(unit, name, &variant_owner) : nullptr;
        if (record) append_module_type_name(unit, record->name, buffer);
        else if (variant)
            append_module_type_name(variant_owner, variant->name, buffer);
        else if (ok) core_buffer_append(buffer, path.data, path.byte_len);
        core_buffer_release(&path);
        return ok;
    }
    if (core_tree_is_leaf(node)) {
        core_string_t text = parse_tree_leaf_text(node);
        core_buffer_append(buffer, text.data, text.u8_bytes);
        return true;
    }
    if (nt_is(grammar, node, "one-tspec")) {
        parse_tree_t *base = child_nt(grammar, node, "base-tspec");
        bool optional = false;
        for (size_t i = 0; i < core_tree_num_children(node); i++) {
            parse_tree_t *child = core_tree_child(node, i);
            if (core_tree_is_leaf(child)
                && core_string_eq(parse_tree_leaf_text(child),
                                  CORE_STRING_STATIC("?")))
                optional = true;
        }
        if (optional) {
            core_buffer_puts(buffer, "option[");
            if (!append_typelang_type(grammar, unit, base, buffer))
                return false;
            core_buffer_putc(buffer, ']');
            return true;
        }
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!append_typelang_type(grammar, unit, core_tree_child(node, i),
                                  buffer))
            return false;
    return true;
}

static bool
append_type_span(parse_grammar_t *grammar, core_translation_unit_t *unit,
                 parse_tree_t *node, const char *type_nonterminal,
                 core_buffer_t *buffer)
{
    parse_tree_t *spec = descendant_nt(grammar, node, type_nonterminal);
    core_buffer_t text = {0};
    bool ok = append_typelang_type(grammar, unit, spec, &text);
    core_translation_unit_t *variant_owner = nullptr;
    parse_scope_t *variant = ok
        ? unit_variant_scope(unit,
            core_string_borrow(text.data, text.byte_len), &variant_owner)
        : nullptr;
    if (variant) {
        n00bb_tu_state_t *state = tu_state(unit);
        core_buffer_puts(buffer, "variant ");
        append_module_type_name(variant_owner, variant->name, buffer);
        bool first = true;
        for (parse_sym_entry_t *arm = variant->first_in_scope; arm;
             arm = arm->next_in_scope) {
            if (!arm->decl_node || !state || !state->types
                || !nt_is(grammar, arm->decl_node, "variant-decl-one"))
                continue;
            const char *payload_type = arm->type
                ? th_print(state->types, arm->type) : nullptr;
            if (!payload_type) { ok = false; break; }
            core_buffer_putc(buffer, first ? '[' : ',');
            if (!first) core_buffer_putc(buffer, ' ');
            core_buffer_puts(buffer, payload_type);
            first = false;
        }
        if (!first) core_buffer_putc(buffer, ']');
    }
    else if (ok) core_buffer_append(buffer, text.data, text.byte_len);
    core_buffer_release(&text);
    return ok;
}

static th_type *
declared_type_from_node(parse_grammar_t *grammar,
                        core_translation_unit_t *unit, parse_tree_t *node)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->types || !node) return nullptr;
    core_buffer_t text = {0};
    if (!append_type_span(grammar, unit, node, "one-tspec", &text)) {
        core_buffer_release(&text);
        return nullptr;
    }
    char *error = nullptr;
    th_type *type = ir_typelang_parse(state->types, text.data, &error);
    core_free(error);
    core_buffer_release(&text);
    return type;
}

static th_type *
function_with_error_channel(th_ctx *types, th_type *function,
                            th_type *error_type)
{
    if (!types || !function || th_kind_of(function) != TH_FUNCTION
        ) return nullptr;
    core_list_t(th_param) params = core_list_new(th_param);
    core_list_t(th_kwparam) keywords = core_list_new(th_kwparam);
    for (size_t i = 0; i < th_func_nparams(function); i++)
        core_list_push(params, *th_func_param(function, i));
    for (size_t i = 0; i < th_func_nkw(function); i++)
        core_list_push(keywords, *th_func_kw(function, i));
    th_type *result = th_func_with_error(types, th_func_ret(function),
        error_type, params.data, params.len, th_func_rest(function),
        keywords.data, keywords.len);
    core_list_free(params);
    core_list_free(keywords);
    return result;
}

static parse_tree_t *
first_identifier_leaf(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (!node) return nullptr;
    if (core_tree_is_leaf(node)) {
        parse_token_info_t *token = parse_node_token(node);
        return token && core_string_eq(parse_terminal_name(grammar, token->tid),
            CORE_STRING_STATIC("IDENTIFIER")) ? node : nullptr;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *found = first_identifier_leaf(
            grammar, core_tree_child(node, i));
        if (found) return found;
    }
    return nullptr;
}

static size_t
tree_token_leaf_count(parse_tree_t *node)
{
    if (!node) return 0;
    if (core_tree_is_leaf(node)) return 1;
    size_t count = 0;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        count += tree_token_leaf_count(core_tree_child(node, i));
        if (count > 1) return count;
    }
    return count;
}

static bool
simple_identifier_expression(parse_grammar_t *grammar, parse_tree_t *node,
                             core_string_t *name)
{
    if (!grammar || !node || tree_token_leaf_count(node) != 1) return false;
    parse_tree_t *identifier = first_identifier_leaf(grammar, node);
    if (!identifier) return false;
    if (name) *name = parse_tree_leaf_text(identifier);
    return true;
}

static bool
unit_declares_trait(core_translation_unit_t *unit, core_string_t name,
                    bool imported)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_symtab_t *symtab = state && state->annotations
        ? state->annotations->symtab : nullptr;
    if (!symtab) return false;
    for (parse_scope_t *scope = symtab->all_scopes; scope;
         scope = scope->all_next) {
        if (!core_string_eq(scope->scope_tag, CORE_STRING_STATIC("trait"))
            || !core_string_eq(scope->name, name)) continue;
        if (!imported) return true;
        parse_sym_entry_t *symbol = scope->node
            ? core_tree_node_value(scope->node).sym : nullptr;
        return symbol &&
            (core_string_eq(symbol->visibility, CORE_STRING_STATIC("public"))
             || core_string_eq(symbol->visibility,
                               CORE_STRING_STATIC("package")));
    }
    return false;
}

static bool
module_name_matches(core_string_t path, core_string_t module)
{
    return path.data && module.data && path.u8_bytes == module.u8_bytes &&
           !memcmp(path.data, module.data, path.u8_bytes);
}

static core_translation_unit_t *
find_direct_import(core_translation_unit_t *unit, core_string_t module)
{
    for (size_t i = 0; i < core_translation_unit_import_count(unit); i++) {
        core_translation_unit_t *imported =
            core_translation_unit_import(unit, i);
        if (module_name_matches(module, imported->module.identifier))
            return imported;
    }
    return nullptr;
}

static bool
append_resolved_trait_name(core_translation_unit_t *unit,
                           parse_tree_t *node, core_buffer_t *buffer)
{
    core_buffer_t path = {0};
    if (!append_node_tokens(node, &path)) {
        core_buffer_release(&path);
        return false;
    }
    char *last_dot = strrchr(path.data, '.');
    core_translation_unit_t *target = nullptr;
    core_string_t trait = {0};
    if (last_dot) {
        core_string_t module = core_string_borrow(path.data,
            (size_t)(last_dot - path.data));
        trait = core_string_borrow(last_dot + 1,
            path.byte_len - (size_t)(last_dot + 1 - path.data));
        if (module_name_matches(module, unit->module.identifier))
            target = unit;
        else
            target = find_direct_import(unit, module);
        if (!target || !unit_declares_trait(target, trait, target != unit))
            target = nullptr;
    }
    else {
        trait = core_string_borrow(path.data, path.byte_len);
        if (unit_declares_trait(unit, trait, false)) {
            target = unit;
        }
        else {
            for (size_t i = 0; i < core_translation_unit_import_count(unit);
                 i++) {
                core_translation_unit_t *imported =
                    core_translation_unit_import(unit, i);
                if (!unit_declares_trait(imported, trait, true)) continue;
                if (target) {
                    target = nullptr;
                    break;
                }
                target = imported;
            }
        }
    }

    if (target) {
        core_buffer_printf(buffer, "%.*s.%.*s",
            (int)target->module.identifier.u8_bytes,
            target->module.identifier.data,
            (int)trait.u8_bytes, trait.data);
    }
    else {
        core_buffer_append(buffer, path.data, path.byte_len);
    }
    core_buffer_release(&path);
    return true;
}

static bool
tree_has_leaf_text(parse_tree_t *node, core_string_t text)
{
    if (!node) return false;
    if (core_tree_is_leaf(node))
        return core_string_eq(parse_tree_leaf_text(node), text);
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (tree_has_leaf_text(core_tree_child(node, i), text)) return true;
    return false;
}

static bool
append_resolved_where_source(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             parse_tree_t *function, parse_tree_t *node,
                             core_buffer_t *buffer)
{
    if (!node) return false;
    if (nt_is(grammar, node, "one-constraint")) {
        parse_tree_t *identifier = first_identifier_leaf(grammar, node);
        core_string_t name = identifier
            ? parse_tree_leaf_text(identifier) : (core_string_t){0};
        (void)function;
        if (name.data && !tree_has_leaf_text(node,
                CORE_STRING_STATIC("!=")))
            return append_resolved_trait_name(unit, node, buffer);
        return append_node_tokens(node, buffer);
    }
    if (core_tree_is_leaf(node)) return append_node_tokens(node, buffer);
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!append_resolved_where_source(grammar, unit, function,
                core_tree_child(node, i), buffer)) return false;
    return true;
}

static bool
append_param_mode(parse_tree_t *node, core_buffer_t *buffer)
{
    if (!node) return true;
    th_mode mode = TH_IN;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (!core_tree_is_leaf(child)) continue;
        core_string_t text = parse_tree_leaf_text(child);
        if (core_string_eq(text, CORE_STRING_STATIC("out"))) {
            mode = TH_OUT;
            break;
        }
        if (core_string_eq(text, CORE_STRING_STATIC("modify"))) {
            mode = TH_INOUT;
            break;
        }
    }
    if (mode == TH_OUT) core_buffer_puts(buffer, "out ");
    else if (mode == TH_INOUT) core_buffer_puts(buffer, "inout ");
    else core_buffer_puts(buffer, "in ");
    return true;
}

static bool
append_explicit_param_types(parse_grammar_t *grammar,
                            core_translation_unit_t *unit,
                            parse_tree_t *node, core_buffer_t *buffer,
                            size_t *count)
{
    if (!node || core_tree_is_leaf(node)) return true;
    bool variadic = nt_is(grammar, node, "vargs-param");
    if (nt_is(grammar, node, "pos-param")
        || nt_is(grammar, node, "default-param") || variadic) {
        if (*count) core_buffer_putc(buffer, ',');
        if (variadic) core_buffer_putc(buffer, '*');
        if (!variadic && !append_param_mode(node, buffer)) return false;
        if (child_nt(grammar, node, "type-spec")) {
            if (!append_type_span(grammar, unit, node, "one-tspec", buffer))
                return false;
        }
        else core_buffer_printf(buffer, "`__n00bb_param_%zu", *count);
        (*count)++;
        return true;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!append_explicit_param_types(grammar, unit,
                core_tree_child(node, i), buffer, count)) return false;
    return true;
}

static th_type *
explicit_function_signature(parse_grammar_t *grammar,
                            core_translation_unit_t *unit,
                            parse_tree_t *func, th_ctx *types,
                            th_type **failure)
{
    parse_tree_t *signature = descendant_nt(grammar, func,
                                             "func-type-signature");
    parse_tree_t *params = descendant_nt(grammar, signature, "param-decl");
    parse_tree_t *result = descendant_nt(grammar, signature, "function-result");
    parse_tree_t *where = descendant_nt(grammar, signature, "where-clause");
    parse_tree_t *success = descendant_nt(grammar, result, "return-type");
    parse_tree_t *error = descendant_nt(grammar, result, "error-type");
    if (!success) return nullptr;

    core_buffer_t text = {0};
    core_buffer_putc(&text, '(');
    size_t count = 0;
    if (!append_explicit_param_types(grammar, unit, params, &text, &count)) {
        core_buffer_release(&text);
        return nullptr;
    }
    core_buffer_puts(&text, ")->");
    if (!append_type_span(grammar, unit, success, "union-tspec", &text)) {
        core_buffer_release(&text);
        return nullptr;
    }
    if (error) {
        core_buffer_puts(&text, " fail:");
        if (!append_type_span(grammar, unit, error, "one-tspec", &text)) {
            core_buffer_release(&text);
            return nullptr;
        }
    }
    if (where) {
        core_buffer_putc(&text, ' ');
        if (!append_resolved_where_source(grammar, unit, func, where, &text)) {
            core_buffer_release(&text);
            return nullptr;
        }
    }
    char *parse_error = nullptr;
    th_type *parsed_failure = nullptr;
    th_type *parsed = ir_typelang_parse_function(types, text.data,
                                                  &parsed_failure,
                                                  &parse_error);
    core_free(parse_error);
    core_buffer_release(&text);
    if (!parsed) return nullptr;
    if (!parsed_failure) parsed_failure = th_name(types, "nil");
    if (!th_func_error(parsed))
        parsed = function_with_error_channel(types, parsed, parsed_failure);
    if (failure) *failure = parsed_failure;
    return parsed;
}

static th_type *
provisional_function_signature(parse_grammar_t *grammar,
                               core_translation_unit_t *unit,
                               parse_tree_t *func, th_ctx *types)
{
    parse_tree_t *signature = descendant_nt(grammar, func,
                                             "func-type-signature");
    parse_tree_t *params = descendant_nt(grammar, signature, "param-decl");
    if (!params) return nullptr;

    core_buffer_t text = {0};
    core_buffer_putc(&text, '(');
    size_t count = 0;
    if (!append_explicit_param_types(grammar, unit, params, &text, &count)) {
        core_buffer_release(&text);
        return nullptr;
    }
    core_buffer_puts(&text, ")->nil");
    parse_tree_t *where = descendant_nt(grammar, signature, "where-clause");
    if (where) {
        core_buffer_putc(&text, ' ');
        if (!append_resolved_where_source(grammar, unit, func, where, &text)) {
            core_buffer_release(&text);
            return nullptr;
        }
    }

    char *parse_error = nullptr;
    th_type *parsed = ir_typelang_parse_function(types, text.data, nullptr,
                                                  &parse_error);
    core_free(parse_error);
    core_buffer_release(&text);
    if (!parsed) return nullptr;

    core_list_t(th_param) parameters = core_list_new(th_param);
    for (size_t i = 0; i < th_func_nparams(parsed); i++) {
        const th_param *parameter = th_func_param(parsed, i);
        if (parameter) core_list_push(parameters, *parameter);
    }
    th_param rest = {0};
    th_param *rest_ptr = nullptr;
    const th_param *parsed_rest = th_func_rest(parsed);
    if (parsed_rest) {
        rest = *parsed_rest;
        rest_ptr = &rest;
    }
    th_type *provisional = th_func_with_error(types, th_var(types),
        th_name(types, "nil"), parameters.data, parameters.len, rest_ptr,
        nullptr, 0);
    core_list_free(parameters);
    return provisional;
}

static th_type *
explicit_trait_method_signature(parse_grammar_t *grammar,
                                core_translation_unit_t *unit,
                                parse_tree_t *method, th_ctx *types,
                                th_type **failure)
{
    parse_tree_t *signature = descendant_nt(grammar, method,
                                             "trait-func-type-signature");
    parse_tree_t *params = descendant_nt(grammar, signature,
                                          "trait-param-decl");
    parse_tree_t *receiver = descendant_nt(grammar, params, "trait-obj-tref");
    parse_tree_t *result = descendant_nt(grammar, signature,
                                          "function-result");
    parse_tree_t *success = descendant_nt(grammar, result, "return-type");
    parse_tree_t *error = descendant_nt(grammar, result, "error-type");
    if (!params || !receiver) return nullptr;

    core_buffer_t text = {0};
    core_buffer_putc(&text, '(');
    parse_sym_entry_t *receiver_symbol = core_tree_node_value(receiver).sym;
    if (receiver_symbol && receiver_symbol->has_mode) {
        if (receiver_symbol->mode == TH_OUT)
            core_buffer_puts(&text, "out __n00bb_receiver:");
        else if (receiver_symbol->mode == TH_INOUT)
            core_buffer_puts(&text, "inout __n00bb_receiver:");
    }
    if (!append_type_span(grammar, unit, receiver, "tspec-tvar", &text)) {
        core_buffer_release(&text);
        return nullptr;
    }
    size_t count = 1;
    parse_tree_t *formals = descendant_nt(grammar, params, "formals");
    if (formals) {
        if (!append_explicit_param_types(grammar, unit, formals, &text,
                                         &count)) {
            core_buffer_release(&text);
            return nullptr;
        }
    }
    core_buffer_puts(&text, ")->");
    if (success) {
        if (!append_type_span(grammar, unit, success, "union-tspec", &text)) {
            core_buffer_release(&text);
            return nullptr;
        }
    }
    else core_buffer_puts(&text, "nil");
    if (error) {
        core_buffer_puts(&text, " fail:");
        if (!append_type_span(grammar, unit, error, "one-tspec", &text)) {
            core_buffer_release(&text);
            return nullptr;
        }
    }
    char *parse_error = nullptr;
    th_type *parsed_failure = nullptr;
    th_type *parsed = ir_typelang_parse_function(types, text.data,
                                                  &parsed_failure,
                                                  &parse_error);
    if (!parsed && parse_error) {
        core_buffer_t message = {0};
        core_buffer_puts(&message, "invalid trait method signature `");
        core_buffer_puts(&message, text.data);
        core_buffer_puts(&message, "`: ");
        core_buffer_puts(&message, parse_error);
        core_diagnostics_report(core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
            core_string_borrow(message.data, message.byte_len));
        core_buffer_release(&message);
    }
    core_free(parse_error);
    core_buffer_release(&text);
    if (!parsed) return nullptr;
    if (!parsed_failure) parsed_failure = th_name(types, "nil");
    if (!th_func_error(parsed))
        parsed = function_with_error_channel(types, parsed, parsed_failure);
    if (failure) *failure = parsed_failure;
    return parsed;
}

static bool
parameter_index_in(parse_grammar_t *grammar, parse_tree_t *node,
                  parse_tree_t *target, size_t *index)
{
    if (!node || core_tree_is_leaf(node)) return false;
    if (nt_is(grammar, node, "pos-param")
        || nt_is(grammar, node, "default-param")
        || nt_is(grammar, node, "vargs-param")) {
        if (node == target) return true;
        (*index)++;
        return false;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (parameter_index_in(grammar, core_tree_child(node, i), target,
                               index)) return true;
    return false;
}

static bool is_scheme(th_type *type);

static bool
function_has_inferred_parameter(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return false;
    if (nt_is(grammar, node, "pos-param")
        || nt_is(grammar, node, "default-param")
        || nt_is(grammar, node, "vargs-param"))
        return child_nt(grammar, node, "type-spec") == nullptr;
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (function_has_inferred_parameter(grammar,
                                            core_tree_child(node, i)))
            return true;
    return false;
}

static void
bind_declared_parameter(parse_grammar_t *grammar,
                        core_translation_unit_t *unit, parse_tree_t *node,
                        n00bb_tu_state_t *state)
{
    if (!state || !state->types || !node || core_tree_is_leaf(node)) return;
    parse_tree_t *callable = core_tree_node_value(node).parent;
    while (callable && !nt_is(grammar, callable, "func-def")
           && !nt_is(grammar, callable, "context-def"))
        callable = core_tree_node_value(callable).parent;
    if (!callable) return;
    parse_sym_entry_t *callable_symbol = core_tree_node_value(callable).sym;
    if (!callable_symbol || !callable_symbol->type) return;
    th_type *function_type = is_scheme(callable_symbol->type)
        ? th_binder_body(callable_symbol->type) : callable_symbol->type;
    if (!function_type || th_kind_of(function_type) != TH_FUNCTION) return;
    size_t index = 0;
    parse_tree_t *params = nt_is(grammar, callable, "func-def")
        ? descendant_nt(grammar, callable, "param-decl")
        : child_nt(grammar, callable, "param-decl");
    if (!parameter_index_in(grammar, params, node, &index)) return;
    bool variadic = nt_is(grammar, node, "vargs-param");
    const th_param *declared = variadic
        ? th_func_rest(function_type)
        : th_func_param(function_type, index);
    parse_sym_entry_t *parameter = core_tree_node_value(node).sym;
    if (!declared || !parameter) return;
    th_type *binding_type = variadic
        ? th_array(state->types, declared->type) : declared->type;
    if (!binding_type) return;
    parse_tree_t *declared_type = child_nt(grammar, node, "type-spec");
    if (declared_type) {
        parameter->type = binding_type;
        core_tree_node_value(node).type = binding_type;
        return;
    }
    if (parameter->type) {
        th_type *unified = th_unify_in_place(state->types, parameter->type,
                                             binding_type);
        if (!unified) {
            core_diagnostics_report(core_translation_unit_diagnostics(unit),
                unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
                CORE_STRING_STATIC("parameter declaration type does not match function signature"));
            return;
        }
        parameter->type = unified;
    }
    else parameter->type = binding_type;
    core_tree_node_value(node).type = parameter->type;
}

static void
record_function_signature(parse_grammar_t *grammar,
                           core_translation_unit_t *unit, parse_tree_t *func,
                           n00bb_tu_state_t *state, bool check_body)
{
    parse_sym_entry_t *symbol = core_tree_node_value(func).sym;
    if (!symbol || !state->types) return;

    parse_tree_t *signature = child_nt(grammar, func, "func-type-signature");
    parse_tree_t *params_node = child_nt(grammar, signature, "param-decl");
    core_list_t(th_param) params = core_list_new(th_param);
    th_param rest = {0};
    bool has_rest = false;
    collect_function_params(grammar, params_node, state->types, &params,
                            &rest, &has_rest);

    th_type *success = nullptr;
    th_type *failure = nullptr;
    collect_function_exits(grammar, func, func, state->types, &success,
                           &failure);
    bool has_success_value = success != nullptr;
    bool has_failure_value = failure != nullptr;
    if (!success) success = th_name(state->types, "nil");
    if (!failure) failure = th_name(state->types, "nil");

    th_type *declared_failure = nullptr;
    th_type *parsed_declared = explicit_function_signature(
        grammar, unit, func, state->types, &declared_failure);
    bool refine_symbol_type = parsed_declared
        && function_has_inferred_parameter(grammar, params_node)
        && symbol->type
        && !is_scheme(symbol->type)
        && th_kind_of(symbol->type) == TH_FUNCTION;
    th_type *declared = refine_symbol_type ? symbol->type : parsed_declared;
    th_type *inferred = th_func_with_error(state->types, success, failure,
        params.data, params.len, has_rest ? &rest : nullptr, nullptr, 0);
    if (declared) {
        th_type *check_type = th_instantiate(state->types,
            th_generalize(state->types, declared));
        th_type *check_inferred = inferred;
        if (check_type && !has_failure_value) {
            check_type = function_with_error_channel(state->types,
                                                      check_type, nullptr);
            check_inferred = function_with_error_channel(state->types,
                                                          inferred, nullptr);
        }
        th_type *unified = nullptr;
        if (!check_body || !has_success_value) unified = check_type;
        else unified = refine_symbol_type
            ? th_unify_in_place(state->types, check_type, check_inferred)
            : th_unify(state->types, check_type, check_inferred);
        bool matches = unified != nullptr;
        th_type *declared_error = declared_failure
            ? declared_failure : th_name(state->types, "nil");
        th_type *check_error = th_instantiate(state->types,
            th_generalize(state->types, declared_error));
        th_type *unified_error = nullptr;
        if (!check_body || !has_failure_value) unified_error = check_error;
        else unified_error = refine_symbol_type
            ? th_unify_in_place(state->types, check_error, failure)
            : th_unify(state->types, check_error, failure);
        matches = matches && unified_error != nullptr;
        if (check_body && !matches) {
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "function body does not match declared signature '%s'",
                th_print(state->types, declared));
            parse_token_info_t *site = parse_tree_find_first_terminal(func);
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit),
                unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0001"),
                core_string_from_cstr(message.data),
                site ? (core_diag_location_t){
                    .line = site->line, .column = site->column,
                    .end_line = site->endline, .end_column = site->endcol,
                    .has_location = true,
                } : (core_diag_location_t){0});
            core_buffer_release(&message);
        }
        if (refine_symbol_type) {
            symbol->type = has_failure_value ? unified
                : function_with_error_channel(state->types, unified,
                                              declared_error);
            symbol->error_type = has_failure_value ? unified_error
                                                   : declared_error;
        }
        else {
            symbol->type = declared;
            symbol->error_type = declared_error;
        }
    }
    else {
        if (symbol->type) {
            th_type *unified = is_scheme(symbol->type)
                ? th_unify(state->types, symbol->type, inferred)
                : th_unify_in_place(state->types, symbol->type, inferred);
            if (unified) symbol->type = unified;
        }
        else symbol->type = inferred;

        if (symbol->error_type) {
            th_type *unified = th_unify_in_place(state->types,
                symbol->error_type, failure);
            if (unified) symbol->error_type = unified;
        }
        else symbol->error_type = failure;
    }
    core_list_free(params);
    (void)unit;
}

static void
record_context_signature(parse_grammar_t *grammar,
                         core_translation_unit_t *unit,
                         parse_tree_t *context, n00bb_tu_state_t *state)
{
    parse_sym_entry_t *symbol = core_tree_node_value(context).sym;
    if (!symbol || !state || !state->types) return;
    parse_tree_t *params = child_nt(grammar, context, "param-decl");
    parse_tree_t *spec = child_nt(grammar, context, "context-spec");
    parse_tree_t *provides = descendant_nt(grammar, spec,
                                            "provides-clause");
    parse_tree_t *error = descendant_nt(grammar, spec, "error-type");
    parse_tree_t *error_hook = descendant_nt(grammar, context,
                                              "context-error");
    if (error_hook && !error) {
        core_diagnostics_report(core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0005"),
            CORE_STRING_STATIC("a context error hook requires a declared context failure type"));
        return;
    }

    core_list_t(th_param) constructor_params = core_list_new(th_param);
    th_param constructor_rest = {0};
    bool constructor_has_rest = false;
    collect_function_params(grammar, params, state->types,
        &constructor_params, &constructor_rest, &constructor_has_rest);
    core_list_free(constructor_params);

    core_buffer_t text = {0};
    core_buffer_putc(&text, '(');
    size_t count = 0;
    if (!append_explicit_param_types(grammar, unit, params, &text, &count)) {
        core_buffer_release(&text);
        return;
    }
    core_buffer_puts(&text, ")->");
    if (provides) {
        if (!append_type_span(grammar, unit, provides, "type-spec", &text)) {
            core_buffer_release(&text);
            return;
        }
    }
    else core_buffer_puts(&text, "nil");
    if (error) {
        core_buffer_puts(&text, " fail:");
        if (!append_type_span(grammar, unit, error, "one-tspec", &text)) {
            core_buffer_release(&text);
            return;
        }
    }

    char *parse_error = nullptr;
    th_type *signature = ir_typelang_parse_function(
        state->types, text.data, nullptr, &parse_error);
    core_free(parse_error);
    core_buffer_release(&text);
    if (!signature) {
        core_diagnostics_report(core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0003"),
            CORE_STRING_STATIC("could not form the context constructor signature"));
        return;
    }
    symbol->type = signature;
    symbol->error_type = error
        ? th_func_error(signature) : th_name(state->types, "nil");

    static const char *hook_names[] = {
        "context-enter", "context-exit", "context-error",
        "context-ensure",
    };
    for (size_t i = 0; i < sizeof(hook_names) / sizeof(hook_names[0]); i++) {
        parse_tree_t *hook = descendant_nt(grammar, context, hook_names[i]);
        if (!hook) continue;
        core_buffer_t hook_text = {0};
        core_buffer_putc(&hook_text, '(');
        size_t hook_param_count = 0;
        if (!append_explicit_param_types(grammar, unit, params, &hook_text,
                                         &hook_param_count)) {
            core_buffer_release(&hook_text);
            continue;
        }
        parse_tree_t *hook_params = child_nt(grammar, hook,
                                              "context-hook-params");
        parse_tree_t *hook_param = descendant_nt(grammar, hook_params,
                                                   "pos-param");
        if (hook_param) {
            if (hook_param_count) core_buffer_putc(&hook_text, ',');
            if (!append_type_span(grammar, unit, hook_param, "one-tspec",
                                  &hook_text)) {
                parse_tree_t *inferred = i == 2
                    ? error
                    : descendant_nt(grammar, provides, "type-spec");
                if (inferred) {
                    if (!append_type_span(grammar, unit, inferred,
                                          "one-tspec", &hook_text)) {
                        core_buffer_release(&hook_text);
                        continue;
                    }
                }
                else core_buffer_puts(&hook_text, "nil");
            }
        }
        core_buffer_puts(&hook_text, ")->");
        if (i == 0 && provides) {
            if (!append_type_span(grammar, unit, provides, "type-spec",
                                  &hook_text)) {
                core_buffer_release(&hook_text);
                continue;
            }
        }
        else core_buffer_puts(&hook_text, "nil");
        if (error && i != 3) {
            core_buffer_puts(&hook_text, " fail:");
            if (!append_type_span(grammar, unit, error, "one-tspec",
                                  &hook_text)) {
                core_buffer_release(&hook_text);
                continue;
            }
        }
        char *hook_error = nullptr;
        th_type *hook_type = ir_typelang_parse_function(
            state->types, hook_text.data, nullptr, &hook_error);
        core_free(hook_error);
        core_buffer_release(&hook_text);
        if (!hook_type) {
            core_diagnostics_report(core_translation_unit_diagnostics(unit),
                unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0004"),
                CORE_STRING_STATIC("could not form a context hook signature"));
            continue;
        }
        core_tree_node_value(hook).type = hook_type;
    }
}

static void
validate_refined_function_bodies(parse_grammar_t *grammar,
                                 core_translation_unit_t *unit,
                                 parse_tree_t *node, n00bb_tu_state_t *state)
{
    if (!node || core_tree_is_leaf(node)) return;
    if (nt_is(grammar, node, "func-def")) {
        th_type *failure = nullptr;
        if (explicit_function_signature(grammar, unit, node, state->types,
                                        &failure))
            record_function_signature(grammar, unit, node, state, true);
        return;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        validate_refined_function_bodies(grammar, unit,
            core_tree_child(node, i), state);
}

static bool
tu_list_contains(core_list_t(core_translation_unit_ptr_t) *units,
                 core_translation_unit_t *unit)
{
    for (size_t i = 0; i < units->len; i++)
        if (units->data[i] == unit) return true;
    return false;
}

static void
collect_import_closure(core_translation_unit_t *unit,
                       core_list_t(core_translation_unit_ptr_t) *units)
{
    if (!unit || tu_list_contains(units, unit)) return;
    core_list_push(*units, unit);
    for (size_t i = 0; i < core_translation_unit_import_count(unit); i++)
        collect_import_closure(core_translation_unit_import(unit, i), units);
}

static bool
is_scheme(th_type *type)
{
    uint64_t arity;
    return type && th_binder_arity(type, &arity);
}

static void
finalize_module_inference(core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || state->inference_finalized || !state->annotation_root
        || !state->annotations || !state->types) return;

    parse_annot_resolve_undefined(state->annotations);
    state->inference_finalized = true;
}

static void
report_call_type_error(core_translation_unit_t *unit, parse_tree_t *call,
                       th_ctx *types)
{
    core_buffer_t message = {0};
    const char *detail = th_last_error(types);
    core_buffer_printf(&message, "call type does not match its function%s%s",
                       detail && *detail ? ": " : "",
                       detail && *detail ? detail : "");
    parse_token_info_t *site = parse_tree_find_first_terminal(call);
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
        core_string_from_cstr(message.data),
        site ? (core_diag_location_t){
            .line = site->line, .column = site->column,
            .end_line = site->endline, .end_column = site->endcol,
            .has_location = true,
        } : (core_diag_location_t){0});
    core_buffer_release(&message);
}

static bool
unify_call_type(core_translation_unit_t *unit, n00bb_tu_state_t *state,
                parse_tree_t *call, th_type *actual, th_type *expected)
{
    if (!actual || !expected) return true;
    if (th_unify_in_place(state->types, actual, expected)) return true;
    report_call_type_error(unit, call, state->types);
    return false;
}

static core_list_t(parse_scope_ptr_t)
module_scopes(n00bb_tu_state_t *state)
{
    core_list_t(parse_scope_ptr_t) scopes = core_list_new(parse_scope_ptr_t);
    if (!state || !state->annotations || !state->annotations->symtab)
        return scopes;
    parse_symtab_t *symtab = state->annotations->symtab;
    parse_namespace_t *ns = parse_symtab_ns(symtab,
                                             CORE_STRING_STATIC(""));
    if (!ns || !ns->root) return scopes;
    core_list_push(scopes, ns->root);
    for (parse_scope_t *scope = symtab->all_scopes; scope;
         scope = scope->all_next) {
        if (scope->parent == ns->root
            && core_string_eq(scope->scope_tag,
                              CORE_STRING_STATIC("module")))
            core_list_push(scopes, scope);
    }
    return scopes;
}

static parse_scope_t *
module_scope(n00bb_tu_state_t *state)
{
    core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
    parse_scope_t *found = nullptr;
    for (size_t i = 0; !found && i < scopes.len; i++)
        if (core_string_eq(scopes.data[i]->scope_tag,
                           CORE_STRING_STATIC("module")))
            found = scopes.data[i];
    core_list_free(scopes);
    return found;
}

static parse_sym_entry_t *
attached_symbol(parse_grammar_t *grammar, core_translation_unit_t *unit,
               parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    parse_nt_node_t *pn = &core_tree_node_value(node);
    if (pn->sym) return pn->sym;
    if (nt_is(grammar, node, "primary")) {
        n00bb_tu_state_t *state = tu_state(unit);
        if (state && state->annotations && state->annotations->symtab)
            return parse_symtab_resolve_node(state->annotations->symtab,
                node, CORE_STRING_STATIC(""));
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_sym_entry_t *symbol = attached_symbol(grammar, unit,
            core_tree_child(node, i));
        if (symbol) return symbol;
    }
    return nullptr;
}

static void
resolve_module_member_nodes(parse_grammar_t *grammar,
                            core_translation_unit_t *unit,
                            parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return;
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        resolve_module_member_nodes(grammar, unit, core_tree_child(node, i));

    if (!nt_is(grammar, node, "postfix-expr")
        || core_tree_num_children(node) < 3
        || !core_tree_is_leaf(core_tree_child(node, 1))
        || !parse_tree_leaf_is(core_tree_child(node, 1),
                               CORE_STRING_STATIC(".")))
        return;

    parse_sym_entry_t *prefix = attached_symbol(
        grammar, unit, core_tree_child(node, 0));
    if (!prefix || prefix->kind != PARSE_SYM_MODULE
        || !prefix->exposed_scope) return;

    core_string_t member = first_token_text(core_tree_child(node, 2));
    parse_sym_entry_t *symbol = parse_symtab_lookup_in(prefix->exposed_scope,
                                                        member);
    if (symbol) core_tree_node_value(node).sym = symbol;
}

static void
resolve_imported_module_symbols(parse_grammar_t *grammar,
    core_list_t(core_translation_unit_ptr_t) *units)
{
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
        for (size_t s = 0; s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_MODULE) continue;
                core_translation_unit_t *imported =
                    find_direct_import(unit, symbol->name);
                if (imported)
                    symbol->exposed_scope = module_scope(tu_state(imported));
            }
        }
        core_list_free(scopes);
    }

    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->annotations) continue;
        core_dict_foreach(state->annotations->cf_labels, entry) {
            parse_cf_label_t *label = entry->value;
            if (label && label->kind == PARSE_CF_CALL && label->cond)
                resolve_module_member_nodes(grammar, unit, label->cond);
        }
    }
}

static parse_sym_entry_t *
bound_function_symbol(parse_grammar_t *grammar, parse_symtab_t *symtab,
                      parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    parse_sym_entry_t *symbol = core_tree_node_value(node).sym;
    if (symbol && symbol->kind == PARSE_SYM_FUNCTION) return symbol;
    if (nt_is(grammar, node, "primary")) {
        symbol = parse_symtab_resolve_node(symtab, node,
                                            CORE_STRING_STATIC(""));
        if (symbol && symbol->kind == PARSE_SYM_FUNCTION) return symbol;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        symbol = bound_function_symbol(grammar, symtab,
                                       core_tree_child(node, i));
        if (symbol) return symbol;
    }
    return nullptr;
}

static parse_tree_t *member_access_node(parse_grammar_t *grammar,
                                        parse_tree_t *node);

static parse_sym_entry_t *
call_function_symbol(parse_grammar_t *grammar, core_translation_unit_t *unit,
                    parse_tree_t *callee, bool member_call)
{
    (void)member_call;
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->annotations || !state->annotations->symtab)
        return nullptr;
    return bound_function_symbol(grammar, state->annotations->symtab,
                                  callee);
}

static core_translation_unit_t *
unit_for_symbol(core_list_t(core_translation_unit_ptr_t) *units,
               parse_sym_entry_t *symbol)
{
    for (size_t i = 0; units && i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(tu_state(unit));
        bool found = false;
        for (size_t s = 0; !found && s < scopes.len; s++)
            for (parse_sym_entry_t *candidate = scopes.data[s]->first_in_scope;
                 candidate; candidate = candidate->next_in_scope)
                if (candidate == symbol) { found = true; break; }
        core_list_free(scopes);
        if (found) return unit;
    }
    return nullptr;
}

static parse_sym_entry_t *
symbol_for_method(core_translation_unit_t *const *units, size_t unit_count,
                const th_env_method_t *method)
{
    if (!units || !method || !method->module || !method->root_name)
        return nullptr;
    size_t module_len = strlen(method->module);
    for (size_t i = 0; i < unit_count; i++) {
        core_translation_unit_t *unit = units[i];
        if (unit->module.identifier.u8_bytes != module_len
            || memcmp(unit->module.identifier.data, method->module,
                      module_len))
            continue;
        n00bb_tu_state_t *state = tu_state(unit);
        parse_scope_t *scope = module_scope(state);
        if (!scope || !state || !state->annotations
            || !state->annotations->symtab)
            return nullptr;
        parse_sym_entry_t *symbol = parse_symtab_lookup_in(scope,
            core_string_borrow(method->root_name, strlen(method->root_name)));
        return symbol && symbol->kind == PARSE_SYM_FUNCTION ? symbol : nullptr;
    }
    return nullptr;
}

static parse_sym_entry_t *
enclosing_function_symbol(parse_grammar_t *grammar, parse_tree_t *node)
{
    for (parse_tree_t *parent = node; parent;
         parent = core_tree_node_value(parent).parent) {
        if (nt_is(grammar, parent, "func-def"))
            return core_tree_node_value(parent).sym;
    }
    return nullptr;
}

static bool
function_reaches(parse_grammar_t *grammar,
                 core_translation_unit_t *const *members, size_t member_count,
                 parse_sym_entry_t *from, parse_sym_entry_t *target,
                 core_list_t(parse_sym_entry_ptr_t) *visited)
{
    if (from == target) return true;
    for (size_t i = 0; i < visited->len; i++)
        if (visited->data[i] == from) return false;
    core_list_push(*visited, from);

    for (size_t i = 0; i < member_count; i++) {
        n00bb_tu_state_t *state = tu_state(members[i]);
        if (!state || !state->annotations) continue;
        core_dict_foreach(state->annotations->cf_labels, entry) {
            parse_cf_label_t *label = entry->value;
            if (!label || label->kind != PARSE_CF_CALL || !label->cond
                || core_tree_is_leaf(label->cond)) continue;
            if (enclosing_function_symbol(grammar, label->self) != from)
                continue;
            parse_sym_entry_t *callee = call_function_symbol(grammar,
                members[i], label->cond,
                member_access_node(grammar, label->cond) != nullptr);
            if (!callee) continue;
            if (function_reaches(grammar, members, member_count, callee,
                                  target, visited)) return true;
        }
    }
    return false;
}

static bool
call_is_recursive(parse_grammar_t *grammar,
                  core_translation_unit_t *const *members,
                  size_t member_count, parse_sym_entry_t *caller,
                  parse_sym_entry_t *callee)
{
    if (!caller || !callee) return false;
    core_list_t(parse_sym_entry_ptr_t) visited =
        core_list_new(parse_sym_entry_ptr_t);
    bool recursive = function_reaches(grammar, members, member_count,
                                      callee, caller, &visited);
    core_list_free(visited);
    return recursive;
}

static core_list_t(th_type_ptr_t)
value_type_environment(core_translation_unit_t *const *units,
                       size_t unit_count)
{
    core_list_t(th_type_ptr_t) environment = core_list_new(th_type_ptr_t);
    for (size_t i = 0; i < unit_count; i++) {
        n00bb_tu_state_t *state = tu_state(units[i]);
        if (!state || !state->annotations || !state->annotations->symtab)
            continue;
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
        for (size_t s = 0; s < scopes.len; s++) {
            for (parse_sym_entry_t *entry = scopes.data[s]->first_in_scope;
                 entry; entry = entry->next_in_scope) {
                if (entry->kind == PARSE_SYM_FUNCTION) continue;
                if (entry->type) core_list_push(environment, entry->type);
                if (entry->error_type)
                    core_list_push(environment, entry->error_type);
            }
        }
        core_list_free(scopes);
    }
    return environment;
}

static parse_tree_t *
member_access_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    if (nt_is(grammar, node, "postfix-expr")
        && core_tree_num_children(node) >= 3
        && core_tree_is_leaf(core_tree_child(node, 1))
        && parse_tree_leaf_is(core_tree_child(node, 1),
                              CORE_STRING_STATIC(".")))
        return node;
    for (size_t i = core_tree_num_children(node); i > 0; i--) {
        parse_tree_t *found = member_access_node(
            grammar, core_tree_child(node, i - 1));
        if (found) return found;
    }
    return nullptr;
}

static void
collect_symbol_expression_type(parse_tree_t *node, th_type **found,
                               bool *ambiguous)
{
    if (!node || core_tree_is_leaf(node) || *ambiguous) return;
    parse_sym_entry_t *symbol = core_tree_node_value(node).sym;
    if (symbol && symbol->kind != PARSE_SYM_FUNCTION && symbol->type) {
        th_type *type = th_resolve(symbol->type);
        if (*found && th_resolve(*found) != type) *ambiguous = true;
        else *found = type;
        return;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        collect_symbol_expression_type(core_tree_child(node, i), found,
                                       ambiguous);
}

static void
collect_concrete_expression_type(parse_grammar_t *grammar,
                                parse_tree_t *node,
                                th_type **found, bool *ambiguous)
{
    if (!node || core_tree_is_leaf(node) || *ambiguous) return;
    th_type *type = core_tree_node_value(node).type;
    parse_sym_entry_t *symbol = core_tree_node_value(node).sym;
    if (symbol && symbol->kind != PARSE_SYM_FUNCTION && symbol->type
        && th_kind_of(th_resolve(symbol->type)) != TH_VAR) {
        type = symbol->type;
    }
    if (type && th_kind_of(th_resolve(type)) != TH_VAR) {
        type = th_resolve(type);
        if (*found && th_resolve(*found) != type) *ambiguous = true;
        else *found = type;
        return;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        collect_concrete_expression_type(grammar, core_tree_child(node, i),
                                         found, ambiguous);
    (void)grammar;
}

static th_type *
call_argument_type(parse_grammar_t *grammar, parse_tree_t *argument)
{
    if (!argument || core_tree_is_leaf(argument)) return nullptr;
    th_type *symbol_type = nullptr;
    bool symbol_ambiguous = false;
    collect_symbol_expression_type(argument, &symbol_type,
                                   &symbol_ambiguous);
    if (symbol_type && !symbol_ambiguous) return symbol_type;
    th_type *type = core_tree_node_value(argument).type;
    if (type && th_kind_of(th_resolve(type)) != TH_VAR) return type;
    th_type *found = nullptr;
    bool ambiguous = false;
    collect_concrete_expression_type(grammar, argument, &found, &ambiguous);
    return ambiguous ? type : found ? found : type;
}

static void
unify_call_results(parse_grammar_t *grammar, core_translation_unit_t *unit,
                   core_translation_unit_t *const *members,
                   size_t member_count, th_type *const *environment,
                   size_t environment_count, bool resolve_member_calls)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->annotations || !state->types) return;

    core_dict_foreach(state->annotations->cf_labels, item) {
        parse_cf_label_t *label = item->value;
        if (!label) continue;
        if (label->kind != PARSE_CF_CALL || !label->cond ||
            core_tree_is_leaf(label->cond)) continue;

        parse_nt_node_t *callee_node = &core_tree_node_value(label->cond);
        parse_tree_t *member = member_access_node(grammar, label->cond);
        if ((member != nullptr) != resolve_member_calls) continue;
        parse_sym_entry_t *callee_symbol = call_function_symbol(
            grammar, unit, label->cond, member != nullptr);
        parse_sym_entry_t *caller_symbol = enclosing_function_symbol(
            grammar, label->self);
        bool member_call = member && !callee_symbol;
        th_type *signature = callee_symbol ? callee_symbol->type
                                           : callee_node->type;
        bool signature_instantiated = false;
        if (callee_symbol && callee_symbol->callable_contract) {
            th_type *failure_type = nullptr;
            if (th_env_signature_channels(state->types,
                    callee_symbol->callable_contract, &signature,
                    &failure_type))
                signature_instantiated = true;
        }
        if (member_call) {
            parse_tree_t *receiver_node = core_tree_child(member, 0);
            core_string_t method_text = first_token_text(
                core_tree_child(member, 2));
            th_type *receiver_type = receiver_node
                && !core_tree_is_leaf(receiver_node)
                ? core_tree_node_value(receiver_node).type : nullptr;
            if (receiver_type && method_text.data) {
                char *method_name = core_strndup(method_text.data,
                                                 method_text.u8_bytes);
                th_env_method_t method = {0};
                th_env_method_status_t method_status =
                    th_env_method_resolve_by_name(state->types,
                        receiver_type, method_name, &method, nullptr);
                if (method_status == TH_ENV_METHOD_FOUND) {
                    signature = method.signature;
                    parse_sym_entry_t *implementation = symbol_for_method(
                        members, member_count, &method);
                    if (implementation) {
                        core_tree_node_value(member).sym = implementation;
                        core_tree_node_value(label->cond).sym = implementation;
                        core_tree_node_value(label->self).sym = implementation;
                        callee_symbol = implementation;
                    }
                }
                else if (receiver_type &&
                         th_kind_of(th_resolve(receiver_type)) != TH_VAR) {
                    core_diagnostics_report(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0003"),
                        core_string_from_cstr(
                            "no matching trait method for this receiver type"));
                }
                core_free(method_name);
            }
        }
        th_type *result = label->self && !core_tree_is_leaf(label->self)
            ? core_tree_node_value(label->self).type : nullptr;
        if (!signature || !result) continue;
        bool recursive = call_is_recursive(grammar, members, member_count,
                                           caller_symbol, callee_symbol);
        if (!signature_instantiated) {
            if (is_scheme(signature))
                signature = th_instantiate(state->types, signature);
            else if (callee_symbol && !recursive)
                signature = th_instantiate(state->types,
                    th_generalize_excluding(state->types, signature,
                        environment, environment_count));
        }

        th_type *return_type = th_func_ret(signature);
        if (return_type)
            (void)unify_call_type(unit, state, label->self, result,
                                  return_type);

        parse_tree_t *positional = child_nt(grammar, label->then_body,
                                            "positional-args");
        size_t parameter_count = th_func_nparams(signature);
        size_t argument_index = 0;
        size_t parameter_offset = member_call ? 1 : 0;
        if (member_call) {
            const th_param *receiver = th_func_param(signature, 0);
            parse_tree_t *receiver_node = core_tree_child(member, 0);
            th_type *receiver_type = receiver_node
                && !core_tree_is_leaf(receiver_node)
                ? core_tree_node_value(receiver_node).type : nullptr;
            if (!receiver || !unify_call_type(unit, state, label->self,
                                              receiver_type,
                                              receiver->type))
                continue;
        }
        if (!positional) continue;
        for (size_t i = 0; i < core_tree_num_children(positional); i++) {
            parse_tree_t *argument = core_tree_child(positional, i);
            if (!nt_is(grammar, argument, "expression")) continue;
            size_t parameter_index = argument_index + parameter_offset;
            const th_param *parameter = th_func_param(signature,
                                                       parameter_index);
            if (!parameter && parameter_index >= parameter_count)
                parameter = th_func_rest(signature);
            if (parameter && !core_tree_is_leaf(argument))
                (void)unify_call_type(unit, state, label->self,
                    call_argument_type(grammar, argument),
                    parameter->type);
            argument_index++;
        }
    }
}

static bool
generalize_module_group(void *context,
                        core_translation_unit_t *const *members,
                        size_t member_count)
{
    module_inference_context_t *inference = context;
    parse_grammar_t *grammar = inference->grammar;
    core_list_t(th_type_ptr_t) environment =
        value_type_environment(inference->units, inference->unit_count);
    for (size_t i = 0; i < member_count; i++)
        unify_call_results(grammar, members[i], members, member_count,
                           environment.data, environment.len, false);

    for (size_t i = 0; i < member_count; i++) {
        n00bb_tu_state_t *state = tu_state(members[i]);
        if (!state || !state->types || !state->annotations
            || !state->annotations->symtab) continue;
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
        for (size_t s = 0; s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_FUNCTION) continue;
                if (symbol->type && symbol->error_type
                    && !symbol->callable_contract) {
                    th_type *contract = th_env_signature_contract(
                        state->types, symbol->type, symbol->error_type);
                    symbol->callable_contract = th_generalize_excluding(
                        state->types, contract, environment.data,
                        environment.len);
                }
                if (symbol->type && !is_scheme(symbol->type))
                    symbol->type = th_generalize_excluding(state->types,
                        symbol->type, environment.data, environment.len);
                if (symbol->error_type && !is_scheme(symbol->error_type))
                    symbol->error_type = th_generalize_excluding(state->types,
                        symbol->error_type, environment.data, environment.len);
            }
        }
        core_list_free(scopes);
    }
    core_list_free(environment);
    return true;
}

static bool
module_type_universe_is_shared(
    core_list_t(core_translation_unit_ptr_t) *units)
{
    th_ctx *universe = nullptr;
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->types) continue;
        if (!universe) universe = state->types;
        else if (state->types != universe) {
            core_diagnostics_report(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0004"),
                CORE_STRING_STATIC(
                    "module import closure spans multiple type-inference contexts"));
            return false;
        }
    }
    return true;
}

static bool
generalize_import_closure(parse_grammar_t *grammar,
                          core_translation_unit_t *root)
{
    core_list_t(core_translation_unit_ptr_t) units =
        core_list_new(core_translation_unit_ptr_t);
    collect_import_closure(root, &units);
    if (!module_type_universe_is_shared(&units)) {
        core_list_free(units);
        return false;
    }
    resolve_imported_module_symbols(grammar, &units);
    for (size_t i = 0; i < units.len; i++)
        finalize_module_inference(units.data[i]);
    module_inference_context_t inference = {
        .grammar = grammar,
        .units = units.data,
        .unit_count = units.len,
    };
    bool ok = core_translation_unit_visit_sccs(root, generalize_module_group,
                                                &inference);
    core_list_free(units);
    return ok;
}

static th_env_visibility_t
symbol_visibility(parse_sym_entry_t *symbol)
{
    if (!symbol) return TH_ENV_PRIVATE;
    if (core_string_eq(symbol->visibility, CORE_STRING_STATIC("public")))
        return TH_ENV_PUBLIC;
    if (core_string_eq(symbol->visibility, CORE_STRING_STATIC("package")))
        return TH_ENV_PACKAGE;
    return TH_ENV_PRIVATE;
}

static ir_lower_result_t
lower_error(ir_lower_context_t *context, ir_diagcode_t code, ir_span_t span,
            const char *message)
{
    if (context && context->diagnostics) {
        ir_diag_t *diagnostic = ir_diag_begin(context->diagnostics, code,
                                              span, nullptr);
        ir_diag_message(diagnostic, context->diagnostics, message);
    }
    return (ir_lower_result_t){.status = IR_LOWER_FAILED};
}

static parse_token_info_t *
last_tree_terminal(parse_tree_t *node)
{
    if (!node) return nullptr;
    if (core_tree_is_leaf(node)) return parse_node_token(node);
    for (size_t i = core_tree_num_children(node); i > 0; i--) {
        parse_token_info_t *token =
            last_tree_terminal(core_tree_child(node, i - 1));
        if (token) return token;
    }
    return nullptr;
}

static ir_span_t
lower_span_for_tree(ir_lower_context_t *context, parse_tree_t *node)
{
    parse_token_info_t *first = parse_tree_find_first_terminal(node);
    parse_token_info_t *last = last_tree_terminal(node);
    if (!context || context->file_id < 0 || !first || !last
        || first->start_byte > UINT32_MAX || last->end_byte > UINT32_MAX)
        return IR_SPAN_UNKNOWN;
    return ir_span_make(context->file_id, (uint32_t)first->start_byte,
                        (uint32_t)last->end_byte, first->line, first->column,
                        last->endline, last->endcol);
}

static ir_block_t *
n00bb_branch_block(ir_lower_context_t *context, n00bb_lower_context_t *lower,
                   const char *role)
{
    char name[64];
    snprintf(name, sizeof(name), "__n00bb_%s_%llu", role,
             (unsigned long long)lower->next_block_id++);
    return ir_block_add(context->function, name);
}

static ir_value_t *
n00bb_emit_local_read(ir_lower_context_t *context, ir_local_t *local,
                     ir_span_t span)
{
    char name[48];
    snprintf(name, sizeof(name), "__n00bb_%llu",
             (unsigned long long)context->next_value_id++);
    ir_instr_t *instruction = ir_instr_add(context->block, IR_OP_COPY);
    if (!instruction || !ir_instr_set_dest(instruction, name, local->type)
        || !ir_instr_operand_local(instruction, local)) return nullptr;
    instruction->span = span;
    return instruction->dest;
}

static ir_value_t *
n00bb_emit_indirect_read(ir_lower_context_t *context, ir_value_t *address,
                        th_type *type, ir_span_t span)
{
    if (!address || !type) return nullptr;
    char name[48];
    snprintf(name, sizeof(name), "__n00bb_%llu",
             (unsigned long long)context->next_value_id++);
    ir_instr_t *instruction = ir_instr_add(context->block, IR_OP_LOAD);
    if (!instruction || !ir_instr_set_dest(instruction, name, type)
        || !ir_instr_operand_value(instruction, address)) return nullptr;
    instruction->span = span;
    return instruction->dest;
}

static bool
n00bb_emit_indirect_write(ir_lower_context_t *context, ir_value_t *address,
                          ir_value_t *value, ir_span_t span)
{
    ir_instr_t *instruction = address && value
        ? ir_instr_add(context->block, IR_OP_STORE) : nullptr;
    if (!instruction || !ir_instr_operand_value(instruction, address)
        || !ir_instr_operand_value(instruction, value)) return false;
    instruction->span = span;
    return true;
}

static bool
n00bb_emit_copyouts(ir_lower_context_t *context,
                    const core_list_t(n00bb_copyout_t) *copyouts,
                    ir_span_t span)
{
    for (size_t i = 0; i < copyouts->len; i++) {
        n00bb_copyout_t *copyout = &copyouts->data[i];
        ir_value_t *value = n00bb_emit_local_read(context, copyout->local,
                                                  span);
        if (!value || !n00bb_emit_indirect_write(context, copyout->address,
                                                  value, span))
            return false;
    }
    return true;
}

static th_type *
n00bb_indirect_type(th_ctx *types, th_type *referent)
{
    return th_ind(types, TH_NULLABLE | TH_RESEATABLE | TH_INDEXABLE,
                  referent);
}

static th_type *
n00bb_default_abi_type(th_ctx *types, th_type *value_type)
{
    th_member members[] = {
        {.name = "present", .type = th_name(types, "bool")},
        {.name = "value", .type = value_type},
    };
    return members[0].type && value_type
        ? th_record(types, members, sizeof(members) / sizeof(members[0]))
        : nullptr;
}

static ir_value_t *
n00bb_make_default_argument(ir_lower_context_t *context,
                            th_type *value_type, ir_value_t *value,
                            bool present, ir_span_t span)
{
    th_type *abi_type = n00bb_default_abi_type(
        ir_module_types(context->module), value_type);
    th_type *bool_type = ir_type_parse(context->module, "bool");
    uint64_t id = context->next_value_id++;
    char present_name[48], value_name[48], aggregate_name[48];
    snprintf(present_name, sizeof(present_name), "__n00bb_default_present_%llu",
             (unsigned long long)id);
    snprintf(value_name, sizeof(value_name), "__n00bb_default_value_%llu",
             (unsigned long long)id);
    snprintf(aggregate_name, sizeof(aggregate_name), "__n00bb_default_arg_%llu",
             (unsigned long long)id);
    ir_instr_t *present_instr = bool_type
        ? ir_instr_add(context->block, IR_OP_CONST) : nullptr;
    bool ok = abi_type && present_instr
        && ir_instr_set_dest(present_instr, present_name, bool_type)
        && ir_instr_operand_uint(present_instr, present ? 1 : 0);
    if (ok) present_instr->span = span;
    ir_value_t *payload = value;
    if (ok && !present) {
        ir_instr_t *undef = ir_instr_add(context->block, IR_OP_UNDEF);
        ok = undef && ir_instr_set_dest(undef, value_name, value_type);
        if (ok) {
            undef->span = span;
            payload = undef->dest;
        }
    }
    ir_instr_t *aggregate = ok
        ? ir_instr_add(context->block, IR_OP_AGGREGATE) : nullptr;
    ok = ok && payload && aggregate
        && ir_instr_set_dest(aggregate, aggregate_name, abi_type)
        && ir_instr_operand_value(aggregate, present_instr->dest)
        && ir_instr_operand_value(aggregate, payload);
    if (!ok) return nullptr;
    aggregate->span = span;
    return aggregate->dest;
}

static ir_value_t *
n00bb_emit_binary_ir(ir_lower_context_t *context, ir_op_t op,
                     th_type *result_type, ir_value_t *left,
                     ir_value_t *right, ir_span_t span)
{
    char name[48];
    snprintf(name, sizeof(name), "__n00bb_%llu",
             (unsigned long long)context->next_value_id++);
    ir_instr_t *instruction = ir_instr_add(context->block, op);
    if (!instruction || !ir_instr_set_dest(instruction, name, result_type)
        || !ir_instr_operand_value(instruction, left)
        || !ir_instr_operand_value(instruction, right)) return nullptr;
    ir_class_t result_class = ir_machine_of(context->module, result_type).class;
    if ((result_class == IR_CLASS_SINT || result_class == IR_CLASS_UINT)
        && ir_op_accepts_mod(op, "trap")) {
        if (!ir_instr_add_mod(instruction, "trap")) return nullptr;
        if ((op == IR_OP_DIV || op == IR_OP_REM)
            && result_class == IR_CLASS_SINT
            && !ir_instr_add_mod(instruction, "trap")) return nullptr;
    }
    instruction->span = span;
    return instruction->dest;
}

static bool
collect_elif_arms(parse_grammar_t *grammar, parse_annot_result_t *annotations,
                  parse_tree_t *node, const char *clause_kind,
                  core_list_t(n00bb_branch_arm_t) *arms)
{
    if (!node || core_tree_is_leaf(node)) return true;
    if (nt_is(grammar, node, clause_kind)) {
        parse_cf_label_t *label = parse_cf_label_lookup(annotations, node);
        if (!label || label->kind != PARSE_CF_BRANCH
            || !label->cond || !label->then_body)
            return false;
        core_list_push(*arms, ((n00bb_branch_arm_t){
            .condition = label->cond,
            .body = label->then_body,
        }));
        return true;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!collect_elif_arms(grammar, annotations,
                               core_tree_child(node, i), clause_kind, arms))
            return false;
    return true;
}

static bool
n00bb_lower_branch_annotation(ir_lower_context_t *context,
                              parse_visit_t *visit, bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (!lower) return false;
    if (!entering) {
        if (lower->branches.len
            && lower->branches.data[lower->branches.len - 1].node == visit->node) {
            n00bb_branch_frame_t frame =
                lower->branches.data[--lower->branches.len];
            if (frame.active && !frame.join_reachable) {
                ir_instr_t *unreachable =
                    ir_instr_add(frame.join, IR_OP_UNREACHABLE);
                if (!unreachable) return false;
                unreachable->span = lower_span_for_tree(context, frame.node);
            }
            if (frame.active) {
                if (frame.expression && frame.join_reachable) {
                    ir_instr_t *read = ir_instr_add(frame.join, IR_OP_COPY);
                    char name[48];
                    snprintf(name, sizeof(name), "__n00bb_if_value_%llu",
                             (unsigned long long)context->next_value_id++);
                    if (!read || !ir_instr_set_dest(read, name,
                                                    frame.result_local->type)
                        || !ir_instr_operand_local(read, frame.result_local)
                        || !ir_lower_set_node_value(context, frame.node,
                                                    read->dest)) {
                        core_list_free(frame.arms);
                        lower_error(context, IR_DIAG_INTERNAL,
                            lower_span_for_tree(context, frame.node),
                            "could not read the value from the if-expression join");
                        return false;
                    }
                    read->span = lower_span_for_tree(context, frame.node);
                }
                context->block = frame.join;
                context->reachable = frame.join_reachable;
            }
            core_list_free(frame.arms);
        }
        return true;
    }

    if (nt_is(visit->grammar, visit->node, "elif-clause")
        || nt_is(visit->grammar, visit->node, "value-elif-clause")) {
        for (parse_tree_t *parent = core_tree_node_value(visit->node).parent;
             parent; parent = core_tree_node_value(parent).parent) {
            if (nt_is(visit->grammar, visit->node, "elif-clause")
                && nt_is(visit->grammar, parent, "if-stmt")) return true;
            if (nt_is(visit->grammar, visit->node, "value-elif-clause")
                && nt_is(visit->grammar, parent, "if-value-expr")) return true;
        }
    }
    bool expression_form = nt_is(visit->grammar, visit->node,
                                 "if-value-expr");
    if (!expression_form
        && !nt_is(visit->grammar, visit->node, "if-stmt")) {
        lower_error(context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(context, visit->node),
                    "this branch form is not lowered yet");
        return false;
    }

    n00bb_tu_state_t *tu = tu_state(lower->unit);
    parse_cf_label_t *label = tu
        ? parse_cf_label_lookup(tu->annotations, visit->node) : nullptr;
    if (!label || label->kind != PARSE_CF_BRANCH || !label->cond
        || !label->then_body)
        return false;
    core_list_t(n00bb_branch_arm_t) arms =
        core_list_new(n00bb_branch_arm_t);
    core_list_push(arms, ((n00bb_branch_arm_t){
        .condition = label->cond,
        .body = label->then_body,
    }));
    if (!collect_elif_arms(visit->grammar, tu->annotations,
                           label->else_body,
                           expression_form ? "value-elif-clause"
                                           : "elif-clause",
                           &arms)) {
        core_list_free(arms);
        return false;
    }
    parse_tree_t *else_clause = descendant_nt(visit->grammar,
                                               label->else_body,
                         expression_form ? "value-else-clause"
                                         : "else-clause");
    parse_tree_t *else_body = else_clause
        ? child_nt(visit->grammar, else_clause,
                   expression_form ? "value-body" : "body") : nullptr;
    if (else_clause && !else_body) {
        core_list_free(arms);
        lower_error(context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(context, visit->node),
                    "this branch form is not lowered yet");
        return false;
    }
    if (expression_form && !else_body) {
        core_list_free(arms);
        lower_error(context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(context, visit->node),
                    "a value if-expression requires an else body");
        return false;
    }
    if (!context->reachable) {
        core_list_push(lower->branches, ((n00bb_branch_frame_t){
            .node = visit->node,
            .arms = arms,
            .else_body = else_body,
            .expression = expression_form,
            .active = false,
        }));
        return true;
    }
    ir_block_t *join = n00bb_branch_block(context, lower, "if_join");
    ir_block_t *else_block = else_body
        ? n00bb_branch_block(context, lower, "if_else") : nullptr;
    th_type *result_type = expression_form
        ? core_tree_node_value(visit->node).type : nullptr;
    ir_local_t *result_local = nullptr;
    if (expression_form && result_type) {
        char name[48];
        snprintf(name, sizeof(name), "__n00bb_if_result_%llu",
                 (unsigned long long)context->next_value_id++);
        result_local = ir_local_add(context->function, name, result_type);
    }
    bool blocks_ready = join && (!else_body || else_block)
        && (!expression_form || result_local);
    for (size_t i = 0; blocks_ready && i < arms.len; i++) {
        arms.data[i].body_block = n00bb_branch_block(context, lower,
                                                     "if_then");
        if (i) arms.data[i].test_block = n00bb_branch_block(
            context, lower, "if_test");
        blocks_ready = arms.data[i].body_block
            && (!i || arms.data[i].test_block);
    }
    if (!blocks_ready) {
        core_list_free(arms);
        return false;
    }
    core_list_push(lower->branches, ((n00bb_branch_frame_t){
        .node = visit->node,
        .arms = arms,
        .else_body = else_body,
        .join = join,
        .else_block = else_block,
        .result_local = result_local,
        .expression = expression_form,
        .join_reachable = !else_body,
        .active = true,
    }));
    return true;
}

static bool
n00bb_lower_range_body_enter(ir_lower_context_t *context,
                             n00bb_lower_context_t *lower,
                             n00bb_loop_frame_t *frame)
{
    ir_span_t span = lower_span_for_tree(context, frame->node);
    ir_value_t *start = ir_lower_node_value(context, frame->range_start);
    ir_value_t *end = ir_lower_node_value(context, frame->range_end);
    th_type *bool_type = th_name(tu_state(lower->unit)->types, "bool");
    if (!start || !end || !bool_type) {
        lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                    "range loop endpoints did not lower to typed values");
        return false;
    }
    ir_instr_t *initialize = ir_instr_add(context->block, IR_OP_COPY);
    if (!initialize || !ir_instr_set_dest_local(initialize, frame->iterator)
        || !ir_instr_operand_value(initialize, start)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not initialize range loop variable");
        return false;
    }
    initialize->span = span;

    ir_instr_t *to_header = ir_instr_add(context->block, IR_OP_JUMP);
    if (!to_header || !ir_instr_operand_block(to_header, frame->header)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not enter range loop header");
        return false;
    }
    to_header->span = span;
    context->block = frame->header;

    ir_value_t *current = n00bb_emit_local_read(context, frame->iterator, span);
    ir_value_t *condition = current
        ? n00bb_emit_binary_ir(context,
            frame->range_inclusive ? IR_OP_LE : IR_OP_LT,
            bool_type, current, end, span) : nullptr;
    ir_instr_t *branch = condition
        ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
    if (!branch || !ir_instr_operand_value(branch, condition)
        || !ir_instr_operand_block(branch, frame->body_block)
        || !ir_instr_operand_block(branch, frame->exit)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not emit range loop condition");
        return false;
    }
    branch->span = span;

    context->block = frame->latch;
    current = n00bb_emit_local_read(context, frame->iterator, span);
    if (!current) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not read range loop variable at its latch");
        return false;
    }
    if (frame->range_inclusive) {
        ir_value_t *at_end = n00bb_emit_binary_ir(
            context, IR_OP_EQ, bool_type, current, end, span);
        ir_instr_t *last = at_end
            ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
        if (!last || !ir_instr_operand_value(last, at_end)
            || !ir_instr_operand_block(last, frame->exit)
            || !ir_instr_operand_block(last, frame->increment)) {
            lower_error(context, IR_DIAG_INTERNAL, span,
                        "could not emit inclusive range loop latch");
            return false;
        }
        last->span = span;
        context->block = frame->increment;
    }
    ir_instr_t *one = ir_instr_add(context->block, IR_OP_CONST);
    char one_name[48];
    snprintf(one_name, sizeof(one_name), "__n00bb_%llu",
             (unsigned long long)context->next_value_id++);
    if (!one || !ir_instr_set_dest(one, one_name, frame->iterator->type)
        || !ir_instr_operand_int_lit(one, ir_lit_of(1))) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not create range increment value");
        return false;
    }
    one->span = span;
    ir_value_t *incremented = n00bb_emit_binary_ir(
        context, IR_OP_ADD, frame->iterator->type, current, one->dest, span);
    ir_instr_t *store = incremented
        ? ir_instr_add(context->block, IR_OP_COPY) : nullptr;
    ir_instr_t *backedge = store
        ? ir_instr_add(context->block, IR_OP_JUMP) : nullptr;
    if (!store || !ir_instr_set_dest_local(store, frame->iterator)
        || !ir_instr_operand_value(store, incremented) || !backedge
        || !ir_instr_operand_block(backedge, frame->header)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not emit range loop increment");
        return false;
    }
    store->span = span;
    backedge->span = span;
    context->block = frame->body_block;
    context->reachable = true;
    return true;
}

static bool
n00bb_emit_jump(ir_lower_context_t *context, ir_block_t *target,
                ir_span_t span)
{
    ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
    if (!jump || !target || !ir_instr_operand_block(jump, target))
        return false;
    jump->span = span;
    return true;
}

static bool
n00bb_store_try_value(ir_lower_context_t *context,
                      n00bb_try_frame_t *frame, parse_grammar_t *grammar,
                      parse_tree_t *body)
{
    parse_tree_t *value_stmt = descendant_nt(grammar, body, "value-stmt");
    parse_tree_t *expression = child_nt(grammar, value_stmt, "expression");
    ir_value_t *value = expression
        ? ir_lower_node_value(context, expression) : nullptr;
    ir_instr_t *store = value
        ? ir_instr_add(context->block, IR_OP_COPY) : nullptr;
    if (!store || !frame->result_value
        || !ir_instr_set_dest_local(store, frame->result_value)
        || !ir_instr_operand_value(store, value)) {
        lower_error(context, IR_DIAG_VERIFY_PARSED,
            lower_span_for_tree(context, body),
            "try expression body does not produce a lowered value");
        return false;
    }
    store->span = lower_span_for_tree(context, body);
    return true;
}

static int
n00bb_route_try_error(ir_lower_context_t *context, parse_tree_t *source,
                      ir_value_t *error, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_try_frame_t *frame = nullptr;
    for (size_t i = lower->tries.len; i > 0; i--) {
        n00bb_try_frame_t *candidate = &lower->tries.data[i - 1];
        if (candidate->active && candidate->body
            && n00bb_tree_contains(candidate->body, source)) {
            frame = candidate;
            break;
        }
    }
    if (!frame) return 0;

    parse_sym_entry_t *symbol = frame->error_symbol;
    th_ctx *types = tu_state(lower->unit)->types;
    if (!error || !symbol || !types) {
        lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                    "try error handler has no bound error value");
        return -1;
    }
    th_type *bound_type = symbol->type
        ? th_unify_in_place(types, symbol->type, error->type) : error->type;
    if (!bound_type) {
        lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                    "failure value does not match the try handler error type");
        return -1;
    }
    symbol->type = bound_type;
    if (!frame->error_value) {
        char name[48];
        snprintf(name, sizeof(name), "__n00bb_try_error_%llu",
                 (unsigned long long)context->next_value_id++);
        frame->error_value = ir_local_add(context->function, name,
                                          bound_type);
        if (!frame->error_value) {
            lower_error(context, IR_DIAG_INTERNAL, span,
                        "could not allocate the try handler error value");
            return -1;
        }
    }
    if (!ir_type_eq(context->module, frame->error_value->type, bound_type)) {
        lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                    "failure paths reaching this handler have incompatible types");
        return -1;
    }
    ir_instr_t *store = ir_instr_add(context->block, IR_OP_COPY);
    if (!store || !ir_instr_set_dest_local(store, frame->error_value)
        || !ir_instr_operand_value(store, error)
        || !n00bb_emit_jump(context, frame->handler, span)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not route failure to the try handler");
        return -1;
    }
    store->span = span;
    frame->error_reachable = true;
    context->reachable = false;
    return 1;
}

static bool
n00bb_lower_try_scope(ir_lower_context_t *context, parse_visit_t *visit,
                      bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (!lower) return true;
    core_string_t name = parse_tree_nt_name(visit->grammar, visit->node);

    bool try_statement = core_string_eq(name,
                                        CORE_STRING_STATIC("try-stmt"));
    bool try_expression = core_string_eq(name,
                                        CORE_STRING_STATIC("try-value-expr"));
    if (try_statement || try_expression) {
        if (entering) {
            n00bb_try_frame_t frame = {
                .node = visit->node,
                .body = child_nt(visit->grammar, visit->node,
                    try_expression ? "value-body" : "try-body"),
                .error_clause = child_nt(visit->grammar, visit->node,
                    try_expression ? "try-value-error-clause"
                                   : "try-error-clause"),
                .error_symbol = nullptr,
                .expression = try_expression,
                .active = context->reachable,
            };
            frame.error_symbol = frame.error_clause
                ? core_tree_node_value(frame.error_clause).sym : nullptr;
            if (frame.active) {
                frame.handler = n00bb_branch_block(context, lower,
                                                   "try_handler");
                frame.join = n00bb_branch_block(context, lower, "try_join");
                if (!frame.handler || !frame.join) {
                    lower_error(context, IR_DIAG_INTERNAL,
                        lower_span_for_tree(context, visit->node),
                        "could not create try control-flow blocks");
                    return false;
                }
                if (frame.expression) {
                    th_type *result_type =
                        core_tree_node_value(visit->node).type;
                    char local_name[48];
                    snprintf(local_name, sizeof(local_name),
                             "__n00bb_try_value_%llu",
                             (unsigned long long)context->next_value_id++);
                    frame.result_value = result_type
                        ? ir_local_add(context->function, local_name,
                                       result_type) : nullptr;
                    if (!frame.result_value) {
                        lower_error(context, IR_DIAG_VERIFY_TYPED,
                            lower_span_for_tree(context, visit->node),
                            "try expression has no inferred result type");
                        return false;
                    }
                }
            }
            core_list_push(lower->tries, frame);
            return true;
        }
        if (!lower->tries.len
            || lower->tries.data[lower->tries.len - 1].node != visit->node)
            return false;
        n00bb_try_frame_t frame = lower->tries.data[--lower->tries.len];
        if (!frame.active) return true;
        if (frame.expression && frame.join_reachable) {
            ir_instr_t *read = ir_instr_add(frame.join, IR_OP_COPY);
            char result_name[48];
            snprintf(result_name, sizeof(result_name),
                     "__n00bb_try_result_%llu",
                     (unsigned long long)context->next_value_id++);
            if (!read || !ir_instr_set_dest(read, result_name,
                                            frame.result_value->type)
                || !ir_instr_operand_local(read, frame.result_value)
                || !ir_lower_set_node_value(context, frame.node, read->dest)) {
                lower_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, frame.node),
                    "could not read the value from the try-expression join");
                return false;
            }
            read->span = lower_span_for_tree(context, frame.node);
        }
        if (!frame.join_reachable && context->block == frame.join) {
            context->block = frame.join;
            ir_instr_t *unreachable = ir_instr_add(context->block,
                                                   IR_OP_UNREACHABLE);
            if (!unreachable)
                return false;
            unreachable->span = lower_span_for_tree(context, visit->node);
            context->reachable = false;
        }
        return true;
    }

    if (core_string_eq(visit->annot->scope_tag,
                       CORE_STRING_STATIC("block"))) {
        if (entering || !lower->tries.len) return true;
        n00bb_try_frame_t *frame = &lower->tries.data[lower->tries.len - 1];
        if (frame->body != visit->node || !frame->active) return true;
        if (context->reachable) {
            if (frame->expression
                && !n00bb_store_try_value(context, frame, visit->grammar,
                                          visit->node))
                return false;
            if (!n00bb_emit_jump(context, frame->join,
                                 lower_span_for_tree(context, visit->node)))
                return false;
            frame->join_reachable = true;
        }
        context->block = frame->handler;
        context->reachable = frame->error_reachable;
        if (!frame->error_reachable) {
            ir_instr_t *unreachable = ir_instr_add(context->block,
                                                   IR_OP_UNREACHABLE);
            if (!unreachable) return false;
            unreachable->span = lower_span_for_tree(context, visit->node);
        }
        return true;
    }

    if (core_string_eq(visit->annot->scope_tag,
                       CORE_STRING_STATIC("try-error"))
        && (core_string_eq(name, CORE_STRING_STATIC("try-error-clause"))
            || core_string_eq(name,
                              CORE_STRING_STATIC("try-value-error-clause")))) {
        if (!lower->tries.len) return true;
        n00bb_try_frame_t *frame = &lower->tries.data[lower->tries.len - 1];
        if (frame->error_clause != visit->node || !frame->active)
            return true;
        if (entering && context->reachable) {
            parse_sym_entry_t *symbol = frame->error_symbol;
            if (!frame->error_value || !symbol
                || !ir_lower_bind_local(context, symbol,
                                        frame->error_value)) {
                lower_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, visit->node),
                    "could not bind the try handler error value");
                return false;
            }
            return true;
        }
        if (entering) return true;
        if (context->reachable) {
            if (frame->expression) {
                parse_tree_t *body = child_nt(visit->grammar, visit->node,
                                               "value-body");
                if (!n00bb_store_try_value(context, frame, visit->grammar,
                                           body))
                    return false;
            }
            if (!n00bb_emit_jump(context, frame->join,
                                 lower_span_for_tree(context, visit->node)))
                return false;
            frame->join_reachable = true;
        }
        context->block = frame->join;
        context->reachable = frame->join_reachable;
        return true;
    }
    return true;
}

static bool
n00bb_lower_branch_body(ir_lower_context_t *context, parse_visit_t *visit,
                        bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !core_string_eq(visit->annot->scope_tag,
                                  CORE_STRING_STATIC("block")))
        return true;
    if (nt_is(visit->grammar, visit->node, "block-value-expr")) {
        if (entering) {
            n00bb_value_block_frame_t frame = {
                .node = visit->node,
                .active = context->reachable,
            };
            if (frame.active) {
                th_type *result_type = core_tree_node_value(visit->node).type;
                char local_name[48];
                snprintf(local_name, sizeof(local_name),
                         "__n00bb_block_value_%llu",
                         (unsigned long long)context->next_value_id++);
                frame.result = result_type
                    ? ir_local_add(context->function, local_name, result_type)
                    : nullptr;
                frame.continuation = n00bb_branch_block(
                    context, lower, "block_value_cont");
                if (!frame.result || !frame.continuation) {
                    lower_error(context, IR_DIAG_VERIFY_TYPED,
                        lower_span_for_tree(context, visit->node),
                        "block value has no result type or continuation");
                    return false;
                }
            }
            core_list_push(lower->value_blocks, frame);
            return true;
        }
        if (!lower->value_blocks.len
            || lower->value_blocks.data[lower->value_blocks.len - 1].node
                != visit->node) return false;
        n00bb_value_block_frame_t frame = lower->value_blocks.data[
            --lower->value_blocks.len];
        if (!frame.active) return true;
        if (!frame.value_written || !context->reachable) {
            lower_error(context, IR_DIAG_VERIFY_PARSED,
                lower_span_for_tree(context, visit->node),
                "value block has no reachable value statement");
            return false;
        }
        ir_instr_t *read = ir_instr_add(context->block, IR_OP_COPY);
        char value_name[48];
        snprintf(value_name, sizeof(value_name), "__n00bb_block_result_%llu",
                 (unsigned long long)context->next_value_id++);
        if (!read || !ir_instr_set_dest(read, value_name, frame.result->type)
            || !ir_instr_operand_local(read, frame.result)
            || !ir_lower_set_node_value(context, visit->node, read->dest)) {
            lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, visit->node),
                "could not read the value block result");
            return false;
        }
        read->span = lower_span_for_tree(context, visit->node);
        return true;
    }
    for (size_t i = lower->branches.len; i > 0; i--) {
        n00bb_branch_frame_t *frame = &lower->branches.data[i - 1];
        if (!frame->active) continue;
        bool else_body = frame->else_body == visit->node;
        size_t arm_index = frame->arms.len;
        for (size_t j = 0; j < frame->arms.len; j++)
            if (frame->arms.data[j].body == visit->node) {
                arm_index = j;
                break;
            }
        if (arm_index == frame->arms.len && !else_body) continue;
        if (entering) {
            if (else_body) {
                context->block = frame->else_block;
                context->reachable = true;
                return true;
            }
            n00bb_branch_arm_t *arm = &frame->arms.data[arm_index];
            ir_value_t *condition =
                ir_lower_node_value(context, arm->condition);
            th_type *bool_type = th_name(tu_state(lower->unit)->types, "bool");
            if (!condition || !bool_type
                || !ir_type_eq(context->module, condition->type, bool_type)) {
                lower_error(context, IR_DIAG_TYPE_MISMATCH,
                    lower_span_for_tree(context, frame->node),
                    "if condition must lower to bool");
                return false;
            }
            ir_block_t *false_block = arm_index + 1 < frame->arms.len
                ? frame->arms.data[arm_index + 1].test_block
                : frame->else_block ? frame->else_block : frame->join;
            ir_instr_t *branch = ir_instr_add(context->block, IR_OP_BRANCH);
            bool emitted = branch
                && ir_instr_operand_value(branch, condition)
                && ir_instr_operand_block(branch, arm->body_block)
                && ir_instr_operand_block(branch, false_block);
            if (!emitted) {
                lower_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, frame->node),
                    "could not emit conditional branch");
                return false;
            }
            branch->span = lower_span_for_tree(context, frame->node);
            context->block = arm->body_block;
            context->reachable = true;
        }
        else if (arm_index < frame->arms.len) {
            if (frame->expression && context->reachable) {
                parse_tree_t *value_stmt = descendant_nt(
                    visit->grammar, visit->node, "value-stmt");
                parse_tree_t *expression = child_nt(
                    visit->grammar, value_stmt, "expression");
                ir_value_t *value = expression
                    ? ir_lower_node_value(context, expression) : nullptr;
                ir_instr_t *store = value
                    ? ir_instr_add(context->block, IR_OP_COPY) : nullptr;
                if (!store || !ir_instr_set_dest_local(
                                  store, frame->result_local)
                    || !ir_instr_operand_value(store, value)) {
                    lower_error(context, IR_DIAG_VERIFY_PARSED,
                        lower_span_for_tree(context, visit->node),
                        "value if arm does not produce a lowered value");
                    return false;
                }
                store->span = lower_span_for_tree(context, visit->node);
            }
            if (context->reachable) {
                ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
                if (!jump || !ir_instr_operand_block(jump, frame->join)) {
                    lower_error(context, IR_DIAG_INTERNAL,
                        lower_span_for_tree(context, frame->node),
                        "could not emit branch join");
                    return false;
                }
                jump->span = lower_span_for_tree(context, frame->node);
                frame->join_reachable = true;
            }
            context->block = arm_index + 1 < frame->arms.len
                ? frame->arms.data[arm_index + 1].test_block
                : frame->else_body ? frame->else_block : frame->join;
            context->reachable = true;
        }
        else {
            if (frame->expression && context->reachable) {
                parse_tree_t *value_stmt = descendant_nt(
                    visit->grammar, visit->node, "value-stmt");
                parse_tree_t *expression = child_nt(
                    visit->grammar, value_stmt, "expression");
                ir_value_t *value = expression
                    ? ir_lower_node_value(context, expression) : nullptr;
                ir_instr_t *store = value
                    ? ir_instr_add(context->block, IR_OP_COPY) : nullptr;
                if (!store || !ir_instr_set_dest_local(
                                  store, frame->result_local)
                    || !ir_instr_operand_value(store, value)) {
                    lower_error(context, IR_DIAG_VERIFY_PARSED,
                        lower_span_for_tree(context, visit->node),
                        "value if else arm does not produce a lowered value");
                    return false;
                }
                store->span = lower_span_for_tree(context, visit->node);
            }
            if (context->reachable) {
                ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
                if (!jump || !ir_instr_operand_block(jump, frame->join)) {
                    lower_error(context, IR_DIAG_INTERNAL,
                        lower_span_for_tree(context, frame->node),
                        "could not emit branch join");
                    return false;
                }
                jump->span = lower_span_for_tree(context, frame->node);
                frame->join_reachable = true;
            }
            context->block = frame->join;
            context->reachable = frame->join_reachable;
        }
        return true;
    }
    for (size_t i = lower->loops.len; i > 0; i--) {
        n00bb_loop_frame_t *frame = &lower->loops.data[i - 1];
        if (!frame->active || frame->body != visit->node) continue;
        if (entering) {
            if (frame->is_range)
                return n00bb_lower_range_body_enter(context, lower, frame);
            ir_value_t *condition = ir_lower_node_value(
                context, frame->condition);
            th_type *bool_type = th_name(tu_state(lower->unit)->types, "bool");
            if (!condition || !bool_type
                || !ir_type_eq(context->module, condition->type, bool_type)) {
                lower_error(context, IR_DIAG_TYPE_MISMATCH,
                    lower_span_for_tree(context, frame->node),
                    "while condition must lower to bool");
                return false;
            }
            ir_instr_t *branch = ir_instr_add(context->block, IR_OP_BRANCH);
            if (!branch || !ir_instr_operand_value(branch, condition)
                || !ir_instr_operand_block(branch, frame->body_block)
                || !ir_instr_operand_block(branch, frame->exit)) {
                lower_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, frame->node),
                    "could not emit while condition branch");
                return false;
            }
            branch->span = lower_span_for_tree(context, frame->node);
            context->block = frame->body_block;
            context->reachable = true;
        }
        else {
            if (context->reachable) {
                ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
                if (!jump || !ir_instr_operand_block(
                                 jump, frame->continue_block)) {
                    lower_error(context, IR_DIAG_INTERNAL,
                        lower_span_for_tree(context, frame->node),
                        "could not emit while back edge");
                    return false;
                }
                jump->span = lower_span_for_tree(context, frame->node);
            }
            context->block = frame->exit;
            context->reachable = true;
        }
        return true;
    }
    for (size_t i = lower->switches.len; i > 0; i--) {
        n00bb_switch_frame_t *frame = &lower->switches.data[i - 1];
        if (!frame->active) continue;
        bool matched = false;
        size_t case_index = frame->cases.len;
        for (size_t j = 0; j < frame->cases.len; j++)
            if (frame->cases.data[j].body == visit->node) {
                case_index = j;
                matched = true;
                break;
            }
        bool is_default = frame->else_body == visit->node;
        if (!matched && !is_default) continue;
        if (entering) {
            if (!frame->emitted) {
                ir_value_t *scrutinee = ir_lower_node_value(
                    context, frame->scrutinee);
                bool has_chain = false;
                for (size_t c = 0; c < frame->cases.len; c++)
                    for (size_t p = 0;
                         p < frame->cases.data[c].patterns.len; p++)
                        has_chain |= frame->cases.data[c].patterns.data[p].is_range
                            || (frame->cases.data[c].patterns.data[p].expression
                                && !frame->cases.data[c].patterns.data[p].is_variant_tag);
                bool emitted = scrutinee != nullptr;
                ir_instr_t *instruction = nullptr;
                if (has_chain) {
                    size_t total = 0;
                    for (size_t c = 0; c < frame->cases.len; c++)
                        total += frame->cases.data[c].patterns.len;
                    size_t ordinal = 0;
                    ir_block_t *test_block = context->block;
                    th_type *scrutinee_type = scrutinee->type;
                    th_type *bool_type = th_name(
                        tu_state(lower->unit)->types, "bool");
                    for (size_t c = 0; emitted && c < frame->cases.len; c++) {
                        n00bb_switch_case_t *item = &frame->cases.data[c];
                        for (size_t p = 0; emitted
                                          && p < item->patterns.len; p++) {
                            n00bb_switch_pattern_t *pattern =
                                &item->patterns.data[p];
                            bool has_next = ++ordinal < total;
                            char next_name[64];
                            snprintf(next_name, sizeof(next_name),
                                "__n00bb_switch_test_%llu",
                                (unsigned long long)context->next_value_id++);
                            ir_block_t *next = has_next
                                ? ir_block_add(context->function, next_name)
                                : frame->default_block;
                            if (!next || !bool_type || !scrutinee_type) {
                                emitted = false;
                                break;
                            }
                            uint64_t id = context->next_value_id++;
                            char first_name[48], first_cmp_name[48];
                            snprintf(first_name, sizeof(first_name),
                                "__n00bb_case_bound_%llu",
                                (unsigned long long)id);
                            snprintf(first_cmp_name, sizeof(first_cmp_name),
                                "__n00bb_case_test_%llu",
                                (unsigned long long)id);
                            ir_instr_t *first_const = nullptr;
                            ir_value_t *pattern_value = nullptr;
                            if (pattern->expression)
                                pattern_value = ir_lower_node_value(
                                    context, pattern->expression);
                            else {
                                first_const = ir_instr_add(test_block,
                                                           IR_OP_CONST);
                                emitted = first_const
                                    && ir_instr_set_dest(first_const,
                                        first_name, scrutinee_type)
                                    && ir_instr_operand_int_lit(first_const,
                                        pattern->lower);
                                pattern_value = first_const
                                    ? first_const->dest : nullptr;
                            }
                            ir_instr_t *first_cmp = emitted
                                ? ir_instr_add(test_block,
                                    pattern->is_range ? IR_OP_GE : IR_OP_EQ)
                                : nullptr;
                            emitted = emitted && pattern_value && first_cmp
                                && ir_instr_set_dest(first_cmp,
                                    first_cmp_name, bool_type)
                                && ir_instr_operand_value(first_cmp,
                                                           scrutinee)
                                && ir_instr_operand_value(first_cmp,
                                                           pattern->is_range
                                                               ? pattern_value
                                                               : (pattern->expression
                                                                   ? pattern_value
                                                                   : first_const->dest));
                            ir_value_t *condition = emitted
                                ? first_cmp->dest : nullptr;
                            ir_instr_t *last_const = nullptr;
                            ir_instr_t *last_cmp = nullptr;
                            ir_block_t *upper_test = nullptr;
                            ir_instr_t *lower_branch = nullptr;
                            if (emitted && pattern->is_range) {
                                char upper_name[64];
                                snprintf(upper_name, sizeof(upper_name),
                                    "__n00bb_switch_upper_%llu",
                                    (unsigned long long)
                                        context->next_value_id++);
                                upper_test = ir_block_add(context->function,
                                                          upper_name);
                                last_const = upper_test
                                    ? ir_instr_add(upper_test, IR_OP_CONST)
                                    : nullptr;
                                last_cmp = upper_test
                                    ? ir_instr_add(upper_test,
                                        pattern->inclusive
                                            ? IR_OP_LE : IR_OP_LT)
                                    : nullptr;
                                char last_name[48], last_cmp_name[48];
                                snprintf(last_name, sizeof(last_name),
                                    "__n00bb_case_bound_%llu",
                                    (unsigned long long)
                                        context->next_value_id++);
                                snprintf(last_cmp_name,
                                    sizeof(last_cmp_name),
                                    "__n00bb_case_test_%llu",
                                    (unsigned long long)
                                        context->next_value_id++);
                                emitted = upper_test && last_const && last_cmp
                                    && ir_instr_set_dest(last_const,
                                        last_name, scrutinee_type)
                                    && ir_instr_operand_int_lit(last_const,
                                        pattern->upper)
                                    && ir_instr_set_dest(last_cmp,
                                        last_cmp_name, bool_type)
                                    && ir_instr_operand_value(last_cmp,
                                        scrutinee)
                                    && ir_instr_operand_value(last_cmp,
                                        last_const->dest);
                                condition = emitted ? last_cmp->dest : nullptr;
                            }
                            ir_instr_t *branch = emitted && !pattern->is_range
                                ? ir_instr_add(test_block, IR_OP_BRANCH)
                                : nullptr;
                            if (pattern->is_range) {
                                lower_branch = emitted
                                    ? ir_instr_add(test_block, IR_OP_BRANCH)
                                    : nullptr;
                                branch = emitted
                                    ? ir_instr_add(upper_test, IR_OP_BRANCH)
                                    : nullptr;
                                emitted = emitted && lower_branch && branch
                                    && ir_instr_operand_value(lower_branch,
                                        first_cmp->dest)
                                    && ir_instr_operand_block(lower_branch,
                                        upper_test)
                                    && ir_instr_operand_block(lower_branch,
                                        next)
                                    && ir_instr_operand_value(branch,
                                        condition)
                                    && ir_instr_operand_block(branch,
                                        item->block)
                                    && ir_instr_operand_block(branch, next);
                            }
                            else {
                                emitted = emitted && branch
                                    && ir_instr_operand_value(branch,
                                        condition)
                                    && ir_instr_operand_block(branch,
                                        item->block)
                                    && ir_instr_operand_block(branch, next);
                            }
                            ir_span_t span = lower_span_for_tree(
                                context, frame->node);
                            if (first_const) first_const->span = span;
                            if (first_cmp) first_cmp->span = span;
                            if (branch) branch->span = span;
                            if (lower_branch) lower_branch->span = span;
                            if (emitted && pattern->is_range) {
                                last_const->span = span;
                                last_cmp->span = span;
                            }
                            test_block = next;
                        }
                    }
                }
                else if (frame->variant_switch) {
                    th_type *tag_type = ir_type_parse(context->module, "uint");
                    char tag_name[48];
                    snprintf(tag_name, sizeof(tag_name),
                        "__n00bb_variant_tag_%llu",
                        (unsigned long long)context->next_value_id++);
                    ir_instr_t *tag = tag_type
                        ? ir_instr_add(context->block, IR_OP_VARIANT_TAG)
                        : nullptr;
                    instruction = tag
                        ? ir_instr_add(context->block, IR_OP_SWITCH) : nullptr;
                    emitted = tag && instruction
                        && ir_instr_set_dest(tag, tag_name, tag_type)
                        && ir_instr_operand_value(tag, scrutinee)
                        && ir_instr_operand_value(instruction, tag->dest)
                        && ir_instr_operand_default(instruction,
                                                    frame->default_block);
                    for (size_t c = 0; emitted && c < frame->cases.len; c++)
                        for (size_t p = 0; emitted
                                && p < frame->cases.data[c].patterns.len; p++) {
                            n00bb_switch_pattern_t *pattern =
                                &frame->cases.data[c].patterns.data[p];
                            emitted = pattern->is_variant_tag
                                && ir_instr_operand_switch_arm_lit(
                                    instruction, pattern->lower,
                                    frame->cases.data[c].block);
                        }
                    if (tag)
                        tag->span = lower_span_for_tree(context, frame->node);
                }
                else {
                    instruction = scrutinee
                        ? ir_instr_add(context->block, IR_OP_SWITCH) : nullptr;
                    emitted = instruction
                        && ir_instr_operand_value(instruction, scrutinee)
                        && ir_instr_operand_default(instruction,
                                                    frame->default_block);
                    for (size_t c = 0; emitted && c < frame->cases.len; c++)
                        for (size_t p = 0; emitted
                                          && p < frame->cases.data[c].patterns.len;
                             p++)
                            emitted = ir_instr_operand_switch_arm_lit(
                                instruction,
                                frame->cases.data[c].patterns.data[p].lower,
                                frame->cases.data[c].block);
                }
                if (!emitted) {
                    lower_error(context, IR_DIAG_VERIFY_PARSED,
                        lower_span_for_tree(context, frame->node),
                        "could not emit switch dispatch");
                    return false;
                }
                if (instruction)
                    instruction->span = lower_span_for_tree(context,
                                                            frame->node);
                frame->emitted = true;
            }
            context->block = is_default ? frame->default_block
                : frame->cases.data[case_index].block;
            context->reachable = true;
        }
        else {
            if (frame->expression && context->reachable) {
                parse_tree_t *value_stmt = descendant_nt(
                    visit->grammar, visit->node, "value-stmt");
                parse_tree_t *expression = child_nt(
                    visit->grammar, value_stmt, "expression");
                ir_value_t *value = expression
                    ? ir_lower_node_value(context, expression) : nullptr;
                ir_instr_t *store = value
                    ? ir_instr_add(context->block, IR_OP_COPY) : nullptr;
                if (!store || !ir_instr_set_dest_local(
                                  store, frame->result_local)
                    || !ir_instr_operand_value(store, value)) {
                    lower_error(context, IR_DIAG_VERIFY_PARSED,
                        lower_span_for_tree(context, visit->node),
                        "switch value arm does not produce a lowered value");
                    return false;
                }
                store->span = lower_span_for_tree(context, visit->node);
            }
            if (context->reachable) {
                ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
                if (!jump || !ir_instr_operand_block(jump, frame->join)) {
                    lower_error(context, IR_DIAG_INTERNAL,
                        lower_span_for_tree(context, frame->node),
                        "could not emit switch join");
                    return false;
                }
                jump->span = lower_span_for_tree(context, frame->node);
                frame->join_reachable = true;
            }
            context->reachable = false;
        }
        return true;
    }
    return true;
}

static bool
n00bb_parse_switch_integer(core_string_t text, ir_lit_t *literal)
{
    unsigned base = 10;
    bool hex = false;
    size_t i = 0;
    if (text.u8_bytes >= 2 && text.data[0] == '0') {
        if (text.data[1] == 'x' || text.data[1] == 'X') {
            base = 16;
            hex = true;
            i = 2;
        }
        else if (text.data[1] == 'b' || text.data[1] == 'B') {
            base = 2;
            i = 2;
        }
        else if (text.data[1] == 'o' || text.data[1] == 'O') {
            base = 8;
            i = 2;
        }
    }
    ir_uint_t value = 0;
    bool digit_seen = false;
    for (; i < text.u8_bytes; i++) {
        unsigned char c = (unsigned char)text.data[i];
        if (c == '_') continue;
        unsigned digit = c >= '0' && c <= '9' ? c - '0'
            : c >= 'a' && c <= 'f' ? c - 'a' + 10
            : c >= 'A' && c <= 'F' ? c - 'A' + 10 : base;
        if (digit >= base || value > (~(ir_uint_t)0 - digit) / base)
            return false;
        value = value * base + digit;
        digit_seen = true;
    }
    if (!digit_seen) return false;
    *literal = (ir_lit_t){.mag = value, .hex = hex};
    return true;
}

static parse_tree_t *
n00bb_simple_switch_literal(parse_grammar_t *grammar, parse_tree_t *node,
                            bool *negative, bool *saw_sign)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    if (nt_is(grammar, node, "simple-lit")) return node;
    parse_tree_t *literal = nullptr;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (core_tree_is_leaf(child)) {
            core_string_t spelling = parse_tree_leaf_text(child);
            if (*saw_sign || (!core_string_eq(spelling,
                                              CORE_STRING_STATIC("-"))
                && !core_string_eq(spelling, CORE_STRING_STATIC("+"))))
                return nullptr;
            *saw_sign = true;
            *negative = core_string_eq(spelling,
                                       CORE_STRING_STATIC("-"));
            continue;
        }
        parse_tree_t *found = n00bb_simple_switch_literal(
            grammar, child, negative, saw_sign);
        if (!found || literal) return nullptr;
        literal = found;
    }
    return literal;
}

static bool
n00bb_switch_case_integer(parse_grammar_t *grammar, parse_tree_t *node,
                         ir_lit_t *value)
{
    bool negative = false;
    bool saw_sign = false;
    parse_tree_t *literal = n00bb_simple_switch_literal(
        grammar, node, &negative, &saw_sign);
    if (!literal || !n00bb_parse_switch_integer(
            first_token_text(literal), value)) return false;
    if (negative) value->neg = true;
    return true;
}

static parse_scope_t *
n00bb_variant_scope_for_type(core_translation_unit_t *unit,
                             n00bb_tu_state_t *state, th_type *type)
{
    if (!state || !state->annotations || !state->annotations->symtab || !type)
        return nullptr;
    const char *name = nullptr;
    size_t name_len = 0;
    if (!th_named_info(th_resolve(type), &name, &name_len, nullptr, nullptr))
        return nullptr;
    return unit_variant_scope(unit, core_string_borrow(name, name_len), nullptr);
}

static bool
n00bb_switch_collect(parse_grammar_t *grammar, parse_tree_t *node,
                     n00bb_switch_frame_t *frame, ir_lower_context_t *context)
{
    if (!node || core_tree_is_leaf(node)) return true;
    if (nt_is(grammar, node, "switch-case-block")
        || nt_is(grammar, node, "switch-value-case-block")) {
        bool expression = nt_is(grammar, node, "switch-value-case-block");
        n00bb_switch_case_t item = {
            .body = child_nt(grammar, node,
                             expression ? "value-body" : "body"),
            .block = n00bb_branch_block(context, context->user, "switch_case"),
            .patterns = core_list_new(n00bb_switch_pattern_t),
        };
        parse_tree_t *list = child_nt(grammar, node, "case-expr-list");
        if (!item.body || !item.block || !list) {
            core_list_free(item.patterns);
            return false;
        }
        core_list_t(parse_tree_ptr_t) stack = core_list_new(parse_tree_ptr_t);
        core_list_push(stack, list);
        while (stack.len) {
            parse_tree_t *current = stack.data[--stack.len];
            if (nt_is(grammar, current, "case-expr-item")) {
                parse_tree_t *separator = descendant_nt(
                    grammar, current, "range-sep");
                if (separator) {
                    parse_tree_t *bounds[2] = {0};
                    size_t bound_count = 0;
                    for (size_t i = 0; i < core_tree_num_children(current);
                         i++) {
                        parse_tree_t *child = core_tree_child(current, i);
                        if (nt_is(grammar, child, "expression")
                            && bound_count < 2)
                            bounds[bound_count++] = child;
                    }
                    n00bb_switch_pattern_t pattern = {
                        .is_range = true,
                        .inclusive = core_string_eq(first_token_text(separator),
                            CORE_STRING_STATIC("to")),
                    };
                    if (bound_count != 2
                        || !n00bb_switch_case_integer(grammar, bounds[0],
                                                       &pattern.lower)
                        || !n00bb_switch_case_integer(grammar, bounds[1],
                                                       &pattern.upper)) {
                        lower_error(context, IR_DIAG_VERIFY_PARSED,
                            lower_span_for_tree(context, current),
                            "switch ranges require integer literal bounds");
                        core_list_free(stack);
                        core_list_free(item.patterns);
                        return false;
                    }
                    core_list_push(item.patterns, pattern);
                }
                else {
                    ir_lit_t value = {0};
                    parse_tree_t *literal = n00bb_simple_switch_literal(
                        grammar, current, &(bool){false}, &(bool){false});
                    core_string_t text = first_token_text(literal);
                    parse_tree_t *expression = child_nt(
                        grammar, current, "expression");
                    bool literal_pattern = false;
                    if (core_string_eq(text, CORE_STRING_STATIC("true"))) {
                        value.mag = 1;
                        literal_pattern = true;
                    }
                    else if (core_string_eq(text,
                                            CORE_STRING_STATIC("false"))) {
                        value.mag = 0;
                        literal_pattern = true;
                    }
                    else literal_pattern = n00bb_switch_case_integer(
                        grammar, current, &value);
                    parse_sym_entry_t *variant_arm = nullptr;
                    core_string_t variant_arm_name = {0};
                    if (!literal_pattern && frame->variant_scope
                        && expression
                        && simple_identifier_expression(grammar, expression,
                                                        &variant_arm_name)) {
                        variant_arm = parse_symtab_lookup_in(
                            frame->variant_scope, variant_arm_name);
                        if (variant_arm && variant_arm->decl_node
                            && nt_is(grammar, variant_arm->decl_node,
                                     "variant-decl-one")) {
                            value.mag = th_arm_tag(variant_arm_name.data);
                            literal_pattern = true;
                        }
                    }
                    if (!literal_pattern && !expression) {
                        lower_error(context, IR_DIAG_VERIFY_PARSED,
                            lower_span_for_tree(context, current),
                            "switch cases must be integer or boolean literals");
                        core_list_free(stack);
                        core_list_free(item.patterns);
                        return false;
                    }
                    core_list_push(item.patterns, ((n00bb_switch_pattern_t){
                        .lower = value,
                        .expression = variant_arm ? expression
                            : literal_pattern ? nullptr : expression,
                        .variant_arm = variant_arm,
                        .variant_arm_name = variant_arm_name,
                        .is_variant_tag = variant_arm != nullptr,
                    }));
                }
                continue;
            }
            for (size_t c = core_tree_num_children(current); c > 0; c--)
                core_list_push(stack, core_tree_child(current, c - 1));
        }
        core_list_free(stack);
        if (frame->variant_scope && item.patterns.len != 1) {
            lower_error(context, IR_DIAG_VERIFY_PARSED,
                lower_span_for_tree(context, node),
                "variant switch cases must name exactly one arm");
            core_list_free(item.patterns);
            return false;
        }
        core_list_push(frame->cases, item);
        return true;
    }
    if (nt_is(grammar, node, "case-else")
        || nt_is(grammar, node, "switch-value-else")) {
        frame->else_body = child_nt(grammar, node,
            nt_is(grammar, node, "switch-value-else")
                ? "value-body" : "body");
        return frame->else_body != nullptr;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!n00bb_switch_collect(grammar, core_tree_child(node, i), frame,
                                  context)) return false;
    return true;
}

static bool
n00bb_variant_switch_exhaustive(parse_grammar_t *grammar,
                                parse_scope_t *variant_scope,
                                core_list_t(n00bb_switch_case_t) cases)
{
    if (!variant_scope) return false;
    for (size_t c = 0; c < cases.len; c++) {
        if (cases.data[c].patterns.len != 1) return false;
        parse_sym_entry_t *arm = cases.data[c].patterns.data[0].variant_arm;
        if (!arm) return false;
        for (size_t previous = 0; previous < c; previous++)
            if (cases.data[previous].patterns.len == 1
                && cases.data[previous].patterns.data[0].variant_arm == arm)
                return false;
    }
    for (parse_sym_entry_t *arm = variant_scope->first_in_scope; arm;
         arm = arm->next_in_scope) {
        if (!arm->decl_node
            || !nt_is(grammar, arm->decl_node, "variant-decl-one"))
            continue;
        bool covered = false;
        for (size_t c = 0; c < cases.len; c++)
            covered |= cases.data[c].patterns.len == 1
                && cases.data[c].patterns.data[0].variant_arm == arm;
        if (!covered) return false;
    }
    return true;
}

static bool
n00bb_typecase_collect(parse_grammar_t *grammar, parse_tree_t *node,
                       n00bb_typecase_frame_t *frame,
                       ir_lower_context_t *context)
{
    if (nt_is(grammar, node, "typecase-case-block")) {
        th_type *type = core_tree_node_value(node).type;
        parse_tree_t *body = child_nt(grammar, node, "body");
        ir_block_t *block = frame->active
            ? n00bb_branch_block(context, context->user, "typecase_arm")
            : nullptr;
        if (!type || !body || (frame->active && !block)) {
            lower_error(context, IR_DIAG_VERIFY_TYPED,
                lower_span_for_tree(context, node),
                "typecase arm needs resolved types and a body");
            return false;
        }
        core_list_push(frame->arms, ((n00bb_typecase_arm_t){
            .body = body,
            .type = type,
            .block = block,
        }));
        return true;
    }
    if (nt_is(grammar, node, "case-else")) {
        frame->else_body = child_nt(grammar, node, "body");
        return frame->else_body != nullptr;
    }
    if (!node || core_tree_is_leaf(node)) return true;
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!n00bb_typecase_collect(grammar, core_tree_child(node, i), frame,
                                    context)) return false;
    return true;
}

static bool
n00bb_typecase_case_contains(ir_module_t *module, th_type *case_type,
                             th_type *wanted)
{
    if (ir_type_eq(module, case_type, wanted)) return true;
    th_comb comb;
    if (!th_sum_comb(case_type, &comb) || comb != TH_UNION) return false;
    for (size_t i = 0; i < th_sum_len(case_type); i++)
        if (ir_type_eq(module, th_sum_arm(case_type, i), wanted)) return true;
    return false;
}

static bool
n00bb_typecase_exhaustive(ir_module_t *module, th_type *subject,
                          core_list_t(n00bb_typecase_arm_t) arms)
{
    th_comb comb;
    if (th_sum_comb(subject, &comb) && comb == TH_UNION) {
        for (size_t i = 0; i < th_sum_len(subject); i++) {
            bool covered = false;
            for (size_t c = 0; c < arms.len; c++)
                covered |= n00bb_typecase_case_contains(
                    module, arms.data[c].type, th_sum_arm(subject, i));
            if (!covered) return false;
        }
        return true;
    }
    for (size_t c = 0; c < arms.len; c++)
        if (n00bb_typecase_case_contains(module, arms.data[c].type, subject))
            return true;
    return false;
}

static bool
n00bb_typecase_emit_dispatch(ir_lower_context_t *context,
                             n00bb_typecase_frame_t *frame)
{
    ir_value_t *subject = ir_lower_node_value(context, frame->subject_node);
    th_type *bool_type = th_name(tu_state(
        ((n00bb_lower_context_t *)context->user)->unit)->types, "bool");
    if (!subject || !bool_type) return false;
    frame->subject_value = subject;
    ir_block_t *test_block = context->block;
    ir_span_t span = lower_span_for_tree(context, frame->node);
    for (size_t i = 0; i < frame->arms.len; i++) {
        n00bb_typecase_arm_t *arm = &frame->arms.data[i];
        ir_block_t *next = i + 1 < frame->arms.len
            ? n00bb_branch_block(context, context->user, "typecase_test")
            : frame->else_body ? frame->else_block : frame->unmatched_block;
        char test_name[48];
        snprintf(test_name, sizeof(test_name), "__n00bb_type_test_%llu",
                 (unsigned long long)context->next_value_id++);
        ir_instr_t *test = ir_instr_add(test_block, IR_OP_TYPE_TEST);
        ir_instr_t *branch = test
            ? ir_instr_add(test_block, IR_OP_BRANCH) : nullptr;
        if (!next || !test || !ir_instr_set_dest(test, test_name, bool_type)
            || !ir_instr_operand_value(test, subject)
            || !ir_instr_operand_type(test, arm->type)
            || !branch || !ir_instr_operand_value(branch, test->dest)
            || !ir_instr_operand_block(branch, arm->block)
            || !ir_instr_operand_block(branch, next)) return false;
        test->span = span;
        branch->span = span;
        test_block = next;
    }
    if (!frame->else_body) {
        ir_instr_t *unreachable = ir_instr_add(test_block, IR_OP_UNREACHABLE);
        if (!unreachable) return false;
        unreachable->span = span;
    }
    frame->emitted = true;
    return true;
}

static bool
n00bb_lower_typecase_scope(ir_lower_context_t *context, parse_visit_t *visit,
                           bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !visit->annot
        || !core_string_eq(visit->annot->scope_tag,
                           CORE_STRING_STATIC("block"))) return true;
    for (size_t i = lower->typecases.len; i > 0; i--) {
        n00bb_typecase_frame_t *frame = &lower->typecases.data[i - 1];
        size_t arm_index = frame->arms.len;
        for (size_t a = 0; a < frame->arms.len; a++)
            if (frame->arms.data[a].body == visit->node) {
                arm_index = a;
                break;
            }
        bool is_else = frame->else_body == visit->node;
        if (arm_index == frame->arms.len && !is_else) continue;
        if (!frame->active) return true;
        if (entering) {
            if (!frame->emitted && !n00bb_typecase_emit_dispatch(context,
                                                                   frame)) {
                lower_error(context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(context, frame->node),
                    "could not emit typecase dispatch");
                return false;
            }
            if (is_else) {
                context->block = frame->else_block;
                context->reachable = true;
                return true;
            }
            n00bb_typecase_arm_t *arm = &frame->arms.data[arm_index];
            context->block = arm->block;
            char name[48];
            snprintf(name, sizeof(name), "__n00bb_narrow_%llu",
                     (unsigned long long)context->next_value_id++);
            ir_instr_t *narrow = ir_instr_add(context->block,
                                               IR_OP_TYPE_NARROW);
            if (!narrow || !ir_instr_set_dest(narrow, name, arm->type)
                || !ir_instr_operand_value(narrow, frame->subject_value)
                || !ir_instr_operand_type(narrow, arm->type)) {
                lower_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, frame->node),
                    "could not narrow the typecase subject");
                return false;
            }
            narrow->span = lower_span_for_tree(context, visit->node);
            frame->binding_checkpoint =
                ir_lower_bindings_checkpoint(context);
            ir_local_t *local = ir_lower_bound_local(context,
                                                     frame->subject_symbol);
            if (!ir_lower_bind_scoped_value(context, frame->subject_symbol,
                                            narrow->dest, local)) {
                lower_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, frame->node),
                    "could not bind the narrowed typecase subject");
                return false;
            }
            context->reachable = true;
            return true;
        }
        if (!is_else
            && !ir_lower_bindings_restore(context,
                                            frame->binding_checkpoint)) {
            lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, frame->node),
                "could not restore the typecase subject binding");
            return false;
        }
        if (context->reachable) {
            ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
            if (!jump || !ir_instr_operand_block(jump, frame->join)) {
                lower_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, frame->node),
                    "could not emit typecase join");
                return false;
            }
            jump->span = lower_span_for_tree(context, frame->node);
            frame->join_reachable = true;
        }
        return true;
    }
    return true;
}

static bool
n00bb_lower_typecase_annotation(ir_lower_context_t *context,
                                parse_visit_t *visit, bool entering)
{
    n00bb_lower_context_t *lower = context->user;
    if (entering) {
        parse_cf_label_t *label = parse_cf_label_lookup(
            tu_state(lower->unit)->annotations, visit->node);
        parse_tree_t *subject_node = label
            ? n00bb_simple_lvalue(label->cond) : nullptr;
        parse_sym_entry_t *symbol = subject_node
            ? core_tree_node_value(subject_node).sym : nullptr;
        if (!label || !subject_node || !symbol
            || (symbol->kind != PARSE_SYM_VARIABLE
                && symbol->kind != PARSE_SYM_PARAM)) {
            lower_error(context, IR_DIAG_VERIFY_PARSED,
                lower_span_for_tree(context, visit->node),
                "typecase subject must be a variable");
            return false;
        }
        n00bb_typecase_frame_t frame = {
            .node = visit->node,
            .label = n00bb_control_label(visit->grammar, visit->node,
                                         "labeled-typecase"),
            .subject_node = subject_node,
            .subject_symbol = symbol,
            .arms = core_list_new(n00bb_typecase_arm_t),
            .order = ++lower->next_control_order,
            .active = context->reachable,
        };
        if (!n00bb_typecase_collect(visit->grammar, label->then_body,
                                    &frame, context)) {
            core_list_free(frame.arms);
            return false;
        }
        if (!frame.arms.len) {
            core_list_free(frame.arms);
            lower_error(context, IR_DIAG_VERIFY_PARSED,
                lower_span_for_tree(context, visit->node),
                "typecase requires at least one case");
            return false;
        }
        th_type *subject_type = th_resolve(symbol->type);
        if (!frame.else_body
            && !n00bb_typecase_exhaustive(context->module, subject_type,
                                           frame.arms)) {
            core_list_free(frame.arms);
            lower_error(context, IR_DIAG_TYPE_MISMATCH,
                lower_span_for_tree(context, visit->node),
                "typecase without else must cover every subject type");
            return false;
        }
        if (frame.active) {
            frame.join = n00bb_branch_block(context, lower, "typecase_join");
            frame.else_block = frame.else_body
                ? n00bb_branch_block(context, lower, "typecase_else") : nullptr;
            frame.unmatched_block = frame.else_body ? frame.else_block
                : n00bb_branch_block(context, lower, "typecase_unmatched");
            if (!frame.join || (frame.else_body && !frame.else_block)
                || !frame.unmatched_block) {
                core_list_free(frame.arms);
                lower_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, visit->node),
                    "could not allocate typecase control-flow blocks");
                return false;
            }
        }
        core_list_push(lower->typecases, frame);
        return true;
    }
    if (!lower->typecases.len
        || lower->typecases.data[lower->typecases.len - 1].node != visit->node)
        return true;
    n00bb_typecase_frame_t frame =
        lower->typecases.data[--lower->typecases.len];
    core_list_free(frame.arms);
    if (!frame.active) return true;
    if (!frame.join_reachable) {
        ir_instr_t *unreachable = ir_instr_add(frame.join, IR_OP_UNREACHABLE);
        if (!unreachable) return false;
        unreachable->span = lower_span_for_tree(context, frame.node);
    }
    context->block = frame.join;
    context->reachable = frame.join_reachable;
    return true;
}

static bool
n00bb_lower_switch_annotation(ir_lower_context_t *context,
                              parse_visit_t *visit, bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (!lower) return false;
    if (nt_is(visit->grammar, visit->node, "typecase-stmt"))
        return n00bb_lower_typecase_annotation(context, visit, entering);
    if (!entering) {
        if (!lower->switches.len
            || lower->switches.data[lower->switches.len - 1].node
                != visit->node) return true;
        n00bb_switch_frame_t frame =
            lower->switches.data[--lower->switches.len];
        for (size_t i = 0; i < frame.cases.len; i++)
            core_list_free(frame.cases.data[i].patterns);
        core_list_free(frame.cases);
        if (frame.active) {
            if (!frame.join_reachable) {
                ir_instr_t *unreachable =
                    ir_instr_add(frame.join, IR_OP_UNREACHABLE);
                if (!unreachable) {
                    lower_error(context, IR_DIAG_INTERNAL,
                        lower_span_for_tree(context, frame.node),
                        "could not terminate unreachable switch join");
                    return false;
                }
                unreachable->span = lower_span_for_tree(context, frame.node);
            }
            if (frame.expression && frame.join_reachable) {
                ir_instr_t *read = ir_instr_add(frame.join, IR_OP_COPY);
                char value_name[48];
                snprintf(value_name, sizeof(value_name),
                         "__n00bb_switch_value_%llu",
                         (unsigned long long)context->next_value_id++);
                if (!read || !ir_instr_set_dest(read, value_name,
                                                frame.result_local->type)
                    || !ir_instr_operand_local(read, frame.result_local)
                    || !ir_lower_set_node_value(context, frame.node,
                                                read->dest)) {
                    lower_error(context, IR_DIAG_INTERNAL,
                        lower_span_for_tree(context, frame.node),
                        "could not read the value from the switch-expression join");
                    return false;
                }
                read->span = lower_span_for_tree(context, frame.node);
            }
            context->block = frame.join;
            context->reachable = frame.join_reachable;
        }
        return true;
    }
    bool expression = nt_is(visit->grammar, visit->node,
                            "switch-value-expr");
    if (!expression && !nt_is(visit->grammar, visit->node, "switch-stmt")) {
        lower_error(context, IR_DIAG_VERIFY_PARSED,
            lower_span_for_tree(context, visit->node),
            "typecase is not currently lowered");
        return false;
    }
    parse_cf_label_t *label = parse_cf_label_lookup(
        tu_state(lower->unit)->annotations, visit->node);
    if (!label || label->kind != PARSE_CF_SWITCH || !label->cond
        || !label->then_body) return false;
    n00bb_switch_frame_t frame = {
        .node = visit->node,
        .label = n00bb_control_label(visit->grammar, visit->node,
                                    "labeled-switch"),
        .scrutinee = label->cond,
        .cases = core_list_new(n00bb_switch_case_t),
        .order = ++lower->next_control_order,
        .expression = expression,
        .active = context->reachable,
    };
    parse_tree_t *subject_lvalue = n00bb_simple_lvalue(label->cond);
    frame.variant_subject = subject_lvalue
        ? attached_symbol(visit->grammar, lower->unit, subject_lvalue)
        : nullptr;
    th_type *subject_type = core_tree_node_value(label->cond).type;
    if (!subject_type && frame.variant_subject)
        subject_type = frame.variant_subject->type;
    frame.variant_scope = n00bb_variant_scope_for_type(
        lower->unit, tu_state(lower->unit), subject_type);
    frame.variant_switch = frame.variant_scope != nullptr;
    if (!frame.active) {
        core_list_push(lower->switches, frame);
        return true;
    }
    if (frame.variant_switch && !frame.variant_subject) {
        lower_error(context, IR_DIAG_VERIFY_TYPED,
            lower_span_for_tree(context, frame.node),
            "variant switch subject must be a variable");
        core_list_free(frame.cases);
        return false;
    }
    frame.join = n00bb_branch_block(context, lower, "switch_join");
    if (expression) {
        th_type *result_type = core_tree_node_value(visit->node).type;
        char local_name[48];
        snprintf(local_name, sizeof(local_name),
                 "__n00bb_switch_result_%llu",
                 (unsigned long long)context->next_value_id++);
        frame.result_local = result_type
            ? ir_local_add(context->function, local_name, result_type) : nullptr;
        if (!frame.result_local) {
            lower_error(context, IR_DIAG_VERIFY_TYPED,
                lower_span_for_tree(context, visit->node),
                "switch expression has no inferred result type");
            return false;
        }
    }
    if (!frame.join || !n00bb_switch_collect(visit->grammar, label->then_body,
                                             &frame, context)) {
        for (size_t i = 0; i < frame.cases.len; i++)
            core_list_free(frame.cases.data[i].patterns);
        core_list_free(frame.cases);
        return false;
    }
    if (frame.variant_switch) {
        for (size_t c = 0; c < frame.cases.len; c++) {
            n00bb_switch_case_t *item = &frame.cases.data[c];
            parse_sym_entry_t *arm = item->patterns.len == 1
                ? item->patterns.data[0].variant_arm : nullptr;
            if (!arm) {
                lower_error(context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(context, item->body),
                    "variant switch cases must name variant arms");
                for (size_t i = 0; i < frame.cases.len; i++)
                    core_list_free(frame.cases.data[i].patterns);
                core_list_free(frame.cases);
                return false;
            }
            for (size_t previous = 0; previous < c; previous++)
                if (frame.cases.data[previous].patterns.len == 1
                    && frame.cases.data[previous].patterns.data[0].variant_arm
                        == arm) {
                    lower_error(context, IR_DIAG_VERIFY_PARSED,
                        lower_span_for_tree(context, frame.node),
                        "variant switch arm appears more than once");
                    for (size_t i = 0; i < frame.cases.len; i++)
                        core_list_free(frame.cases.data[i].patterns);
                    core_list_free(frame.cases);
                    return false;
                }
        }
        bool exhaustive = n00bb_variant_switch_exhaustive(
            visit->grammar, frame.variant_scope, frame.cases);
        if (!frame.else_body && !exhaustive) {
            lower_error(context, IR_DIAG_TYPE_MISMATCH,
                lower_span_for_tree(context, frame.node),
                "variant switch without else must cover every arm exactly once");
            for (size_t i = 0; i < frame.cases.len; i++)
                core_list_free(frame.cases.data[i].patterns);
            core_list_free(frame.cases);
            return false;
        }
        th_type *tag_type = ir_type_parse(context->module, "uint");
        for (size_t c = 0; tag_type && c < frame.cases.len; c++) {
            for (size_t p = 0; p < frame.cases.data[c].patterns.len; p++) {
                n00bb_switch_pattern_t *pattern =
                    &frame.cases.data[c].patterns.data[p];
                if (!pattern->is_variant_tag) {
                    lower_error(context, IR_DIAG_VERIFY_PARSED,
                        lower_span_for_tree(context, frame.node),
                        "variant switch cases must name variant arms");
                    return false;
                }
                parse_tree_t *primary = descendant_nt(visit->grammar,
                    pattern->expression, "primary");
                char tag_value_name[48];
                snprintf(tag_value_name, sizeof(tag_value_name),
                    "__n00bb_arm_tag_%llu",
                    (unsigned long long)context->next_value_id++);
                ir_instr_t *tag_value = primary
                    ? ir_instr_add(context->block, IR_OP_ARMTAG) : nullptr;
                if (!tag_value || !ir_instr_set_dest(tag_value,
                        tag_value_name, tag_type)
                    || !ir_instr_operand_name(tag_value,
                        pattern->variant_arm_name.data)
                    || !ir_lower_set_node_value(context, primary,
                                                tag_value->dest)) {
                    lower_error(context, IR_DIAG_INTERNAL,
                        lower_span_for_tree(context, pattern->expression),
                        "could not bind variant case arm tag");
                    return false;
                }
                tag_value->span = lower_span_for_tree(
                    context, pattern->expression);
            }
        }
    }
    bool exhaustive_variant = frame.variant_switch
        && n00bb_variant_switch_exhaustive(visit->grammar,
                                           frame.variant_scope, frame.cases);
    frame.default_block = frame.else_body
        ? n00bb_branch_block(context, lower, "switch_else")
        : exhaustive_variant
            ? n00bb_branch_block(context, lower, "switch_unreachable")
            : frame.join;
    frame.join_reachable = frame.else_body == nullptr
        && !exhaustive_variant;
    if (!frame.default_block) {
        for (size_t i = 0; i < frame.cases.len; i++)
            core_list_free(frame.cases.data[i].patterns);
        core_list_free(frame.cases);
        return false;
    }
    if (exhaustive_variant && !frame.else_body) {
        ir_instr_t *unreachable = ir_instr_add(frame.default_block,
                                               IR_OP_UNREACHABLE);
        if (!unreachable) {
            lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, frame.node),
                "could not terminate exhaustive variant switch default");
            for (size_t i = 0; i < frame.cases.len; i++)
                core_list_free(frame.cases.data[i].patterns);
            core_list_free(frame.cases);
            return false;
        }
        unreachable->span = lower_span_for_tree(context, frame.node);
    }
    core_list_push(lower->switches, frame);
    return true;
}

static bool
n00bb_lower_variant_payload(ir_lower_context_t *context, parse_visit_t *visit,
                            bool entering, void *state)
{
    (void)state;
    if (entering || !nt_is(visit->grammar, visit->node, "postfix-expr")
        || core_tree_num_children(visit->node) < 3
        || !parse_tree_leaf_is(core_tree_child(visit->node, 1),
                               CORE_STRING_STATIC("."))) return true;
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    if (!lower) return false;
    parse_tree_t *receiver_node = core_tree_child(visit->node, 0);
    parse_sym_entry_t *receiver = attached_symbol(visit->grammar,
        lower->unit, receiver_node);
    if (!receiver) return true;
    core_string_t member = first_token_text(core_tree_child(visit->node, 2));
    parse_tree_t *parent = core_tree_node_value(visit->node).parent;
    while (parent) {
        for (size_t i = lower->switches.len; i > 0; i--) {
            n00bb_switch_frame_t *frame = &lower->switches.data[i - 1];
            if (!frame->active || !frame->variant_switch
                || frame->variant_subject != receiver) continue;
            for (size_t c = 0; c < frame->cases.len; c++) {
                n00bb_switch_case_t *item = &frame->cases.data[c];
                if (item->body != parent) continue;
                for (size_t p = 0; p < item->patterns.len; p++) {
                    n00bb_switch_pattern_t *pattern =
                        &item->patterns.data[p];
                    if (!pattern->is_variant_tag
                        || !core_string_eq(pattern->variant_arm_name, member))
                        continue;
                    if (pattern->variant_arm && pattern->variant_arm->type
                        && n00bb_nil_type(tu_state(lower->unit)->types,
                                          pattern->variant_arm->type)) {
                        lower_error(context, IR_DIAG_TYPE_MISMATCH,
                            lower_span_for_tree(context, visit->node),
                            "nil variant arm has no readable payload");
                        return false;
                    }
                    ir_value_t *value = ir_lower_node_value(context,
                                                             receiver_node);
                    if (!value) return true;
                    ir_instr_t *payload = ir_instr_add(context->block,
                                                       IR_OP_VARIANT_PAYLOAD);
                    char name[48];
                    snprintf(name, sizeof(name), "__n00bb_payload_%llu",
                        (unsigned long long)context->next_value_id++);
                    if (!payload || !pattern->variant_arm
                        || !pattern->variant_arm->type
                        || !ir_instr_set_dest(payload, name,
                            pattern->variant_arm->type)
                        || !ir_instr_operand_value(payload, value)
                        || !ir_instr_operand_name(payload,
                            pattern->variant_arm_name.data)
                        || !ir_lower_set_node_value(context, visit->node,
                                                    payload->dest)) {
                        lower_error(context, IR_DIAG_INTERNAL,
                            lower_span_for_tree(context, visit->node),
                            "could not lower variant payload access");
                        return false;
                    }
                    payload->span = lower_span_for_tree(context, visit->node);
                    return true;
                }
            }
        }
        parent = core_tree_node_value(parent).parent;
    }
    return true;
}

static bool
n00bb_lower_loop_annotation(ir_lower_context_t *context, parse_visit_t *visit,
                            bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (!lower) return false;
    if (!entering) {
        if (!lower->loops.len
            || lower->loops.data[lower->loops.len - 1].node != visit->node)
            return true;
        n00bb_loop_frame_t frame =
            lower->loops.data[--lower->loops.len];
        if (frame.active) {
            context->block = frame.exit;
            context->reachable = true;
        }
        return true;
    }

    n00bb_tu_state_t *tu = tu_state(lower->unit);
    parse_cf_label_t *label = tu
        ? parse_cf_label_lookup(tu->annotations, visit->node) : nullptr;
    if (!label || label->kind != PARSE_CF_LOOP
        || !label->cond || !label->then_body)
        return false;
    if (!context->reachable) {
        core_list_push(lower->loops, ((n00bb_loop_frame_t){
            .node = visit->node,
            .active = false,
        }));
        return true;
    }

    bool while_loop = nt_is(visit->grammar, visit->node, "while-stmt");
    bool range_loop = nt_is(visit->grammar, visit->node, "for-stmt")
        && nt_is(visit->grammar, label->cond, "range");
    if (!while_loop && !range_loop) {
        lower_error(context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(context, visit->node),
                    "non-range for loops are not lowered yet");
        return false;
    }

    parse_tree_t *range_start = nullptr;
    parse_tree_t *range_end = nullptr;
    parse_sym_entry_t *iterator_symbol = nullptr;
    ir_local_t *iterator = nullptr;
    bool inclusive = false;
    if (range_loop) {
        parse_tree_t *vars = child_nt(visit->grammar, visit->node,
                                      "for-var-list");
        parse_tree_t *parts = child_nt(visit->grammar, label->cond,
                                       "range-parts");
        if (!vars || terminal_count(vars) != 1 || !parts
            || core_tree_num_children(parts) < 3) {
            lower_error(context, IR_DIAG_VERIFY_PARSED,
                lower_span_for_tree(context, visit->node),
                "range loops currently require one loop variable and two endpoints");
            return false;
        }
        for (size_t i = 0; i < core_tree_num_children(parts); i++) {
            parse_tree_t *child = core_tree_child(parts, i);
            if (nt_is(visit->grammar, child, "expression")) {
                if (!range_start) range_start = child;
                else range_end = child;
            }
            if (nt_is(visit->grammar, child, "range-sep"))
                inclusive = core_string_eq(first_token_text(child),
                                             CORE_STRING_STATIC("to"));
        }
        iterator_symbol = core_tree_node_value(visit->node).sym;
        th_type *iterator_type = iterator_symbol
            ? iterator_symbol->type : nullptr;
        if (!range_start || !range_end || !iterator_symbol || !iterator_type) {
            lower_error(context, IR_DIAG_VERIFY_TYPED,
                lower_span_for_tree(context, visit->node),
                "range loop variable or endpoint types are unresolved");
            return false;
        }
        char local_name[48];
        snprintf(local_name, sizeof(local_name), "__n00bb_for_%llu",
                 (unsigned long long)context->next_value_id++);
        iterator = ir_local_add(context->function, local_name, iterator_type);
        if (!iterator || !ir_lower_bind_local(context, iterator_symbol,
                                               iterator)) {
            lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, visit->node),
                "could not bind range loop variable");
            return false;
        }
    }

    ir_block_t *header = n00bb_branch_block(context, lower, "while_header");
    ir_block_t *body = n00bb_branch_block(context, lower, "while_body");
    ir_block_t *latch = range_loop
        ? n00bb_branch_block(context, lower, "for_latch") : header;
    ir_block_t *increment = range_loop && inclusive
        ? n00bb_branch_block(context, lower, "for_increment") : nullptr;
    ir_block_t *exit = n00bb_branch_block(context, lower, "while_exit");
    ir_instr_t *jump = header && !range_loop
        ? ir_instr_add(context->block, IR_OP_JUMP) : nullptr;
    if (!header || !body || !latch || (range_loop && inclusive && !increment)
        || !exit || (!range_loop && (!jump
            || !ir_instr_operand_block(jump, header)))) {
        lower_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, visit->node),
                    "could not create while control-flow blocks");
        return false;
    }
    if (jump) jump->span = lower_span_for_tree(context, visit->node);
    core_list_push(lower->loops, ((n00bb_loop_frame_t){
        .node = visit->node,
        .label = n00bb_control_label(visit->grammar, visit->node,
                                    "labeled-while"),
        .condition = label->cond,
        .body = label->then_body,
        .range_start = range_start,
        .range_end = range_end,
        .header = header,
        .body_block = body,
        .latch = latch,
        .increment = increment,
        .exit = exit,
        .continue_block = range_loop ? latch : header,
        .iterator = iterator,
        .iterator_symbol = iterator_symbol,
        .range_inclusive = inclusive,
        .is_range = range_loop,
        .order = ++lower->next_control_order,
        .active = true,
    }));
    if (!range_loop) context->block = header;
    context->reachable = true;
    return true;
}

static bool
n00bb_lower_jump_annotation(ir_lower_context_t *context, parse_visit_t *visit,
                            bool entering, void *state)
{
    (void)state;
    core_string_t name = parse_tree_nt_name(visit->grammar, visit->node);
    if (core_string_eq(name, CORE_STRING_STATIC("return-stmt"))
        && !entering && context->reachable) {
        return n00bb_emit_with_success(context, visit->node)
            && n00bb_emit_copyouts(context,
                &((n00bb_lower_context_t *)context->user)
                    ->parameters_to_copy_out,
                lower_span_for_tree(context, visit->node));
    }
    if (core_string_eq(name, CORE_STRING_STATIC("value-stmt"))) {
        if (entering || !context->reachable) return true;
        n00bb_lower_context_t *lower = context->user;
        for (size_t i = lower->value_blocks.len; i > 0; i--) {
            n00bb_value_block_frame_t *frame =
                &lower->value_blocks.data[i - 1];
            if (!frame->active
                || !n00bb_tree_contains(frame->node, visit->node)) continue;
            parse_tree_t *expression = child_nt(visit->grammar, visit->node,
                                                "expression");
            ir_value_t *value = expression
                ? ir_lower_node_value(context, expression) : nullptr;
            ir_instr_t *store = value
                ? ir_instr_add(context->block, IR_OP_COPY) : nullptr;
            ir_instr_t *jump = store
                ? ir_instr_add(context->block, IR_OP_JUMP) : nullptr;
            if (!store || !ir_instr_set_dest_local(store, frame->result)
                || !ir_instr_operand_value(store, value) || !jump
                || !ir_instr_operand_block(jump, frame->continuation)) {
                lower_error(context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(context, visit->node),
                    "could not store a block value and continue");
                return false;
            }
            store->span = lower_span_for_tree(context, visit->node);
            jump->span = lower_span_for_tree(context, visit->node);
            frame->value_written = true;
            context->block = frame->continuation;
            context->reachable = true;
            return true;
        }
        bool handled_by_value_expression = false;
        for (size_t i = 0; i < lower->tries.len; i++) {
            n00bb_try_frame_t *frame = &lower->tries.data[i];
            if (frame->active && frame->expression
                && n00bb_tree_contains(frame->node, visit->node)) {
                handled_by_value_expression = true;
                break;
            }
        }
        for (parse_tree_t *ancestor = visit->node;
             ancestor && !handled_by_value_expression;
             ancestor = core_tree_node_value(ancestor).parent) {
            for (size_t i = 0; i < lower->branches.len; i++) {
                n00bb_branch_frame_t *frame = &lower->branches.data[i];
                if (!frame->active || !frame->expression) continue;
                for (size_t arm = 0; arm < frame->arms.len; arm++)
                    if (frame->arms.data[arm].body == ancestor)
                        handled_by_value_expression = true;
                if (frame->else_body == ancestor)
                    handled_by_value_expression = true;
            }
            for (size_t i = 0; i < lower->switches.len; i++) {
                n00bb_switch_frame_t *frame = &lower->switches.data[i];
                if (!frame->active || !frame->expression) continue;
                for (size_t arm = 0; arm < frame->cases.len; arm++)
                    if (frame->cases.data[arm].body == ancestor)
                        handled_by_value_expression = true;
                if (frame->else_body == ancestor)
                    handled_by_value_expression = true;
            }
        }
        if (handled_by_value_expression) return true;

        bool function_body_value = false;
        bool nested_value_body = false;
        for (parse_tree_t *ancestor = core_tree_node_value(visit->node).parent;
             ancestor; ancestor = core_tree_node_value(ancestor).parent) {
            if (nt_is(visit->grammar, ancestor, "value-body"))
                nested_value_body = true;
            if (nt_is(visit->grammar, ancestor, "body")) {
                function_body_value = ancestor == lower->function_body;
                break;
            }
        }
        if (!function_body_value || nested_value_body) {
            lower_error(context, IR_DIAG_VERIFY_PARSED,
                lower_span_for_tree(context, visit->node),
                "value statement is not in a supported value-producing block");
            return false;
        }
        parse_tree_t *expression = child_nt(visit->grammar, visit->node,
                                            "expression");
        ir_value_t *value = expression
            ? ir_lower_node_value(context, expression) : nullptr;
        th_type *result_type = lower->function_symbol
            ? th_func_ret(lower->function_symbol->type) : nullptr;
        if (!value || !result_type
            || !ir_type_eq(context->module, value->type, result_type)) {
            lower_error(context, IR_DIAG_TYPE_MISMATCH,
                lower_span_for_tree(context, visit->node),
                "function value does not match its success result type");
            return false;
        }
        if (!n00bb_emit_with_success(context, visit->node)
            || !n00bb_emit_copyouts(context,
                &lower->parameters_to_copy_out,
                lower_span_for_tree(context, visit->node))) return false;
        ir_instr_t *ret = ir_instr_add(context->block, IR_OP_RETURN);
        if (!ret || !ir_instr_operand_value(ret, value)) {
            lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, visit->node),
                "could not emit function value return");
            return false;
        }
        ret->span = lower_span_for_tree(context, visit->node);
        context->reachable = false;
        return true;
    }
    if (core_string_eq(name, CORE_STRING_STATIC("fail-stmt"))) {
        if (entering || !context->reachable) return true;
        n00bb_lower_context_t *lower = context->user;
        parse_tree_t *expression = child_nt(visit->grammar, visit->node,
                                            "expression");
        ir_value_t *value = expression
            ? ir_lower_node_value(context, expression) : nullptr;
        ir_span_t span = lower_span_for_tree(context, visit->node);
        if (!value) {
            lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                        "fail statement has no lowered failure value");
            return false;
        }
        if (!n00bb_emit_with_failure(context, visit->node))
            return false;
        int routed = n00bb_route_try_error(context, visit->node, value, span);
        if (routed) return routed > 0;
        th_type *failure_type = lower->function_symbol
            ? th_func_error(lower->function_symbol->type) : nullptr;
        if (!failure_type || n00bb_nil_type(
                tu_state(lower->unit)->types, failure_type)) {
            lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                "fail statement requires a non-nil function failure type");
            return false;
        }
        if (!value || !ir_type_eq(context->module, value->type,
                                  failure_type)) {
            lower_error(context, IR_DIAG_TYPE_MISMATCH,
                span,
                "failure value does not match this function's failure type");
            return false;
        }
        ir_instr_t *fail = ir_instr_add(context->block, IR_OP_FAIL);
        if (!fail || !ir_instr_operand_value(fail, value)) {
            lower_error(context, IR_DIAG_INTERNAL,
                span,
                "could not emit function failure exit");
            return false;
        }
        fail->span = span;
        context->reachable = false;
        return true;
    }
    if (!entering) return true;
    if (core_string_eq(name, CORE_STRING_STATIC("return-stmt"))) return true;
    if (!core_string_eq(name, CORE_STRING_STATIC("break-stmt"))
        && !core_string_eq(name, CORE_STRING_STATIC("continue-stmt"))) {
        lower_error(context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(context, visit->node),
                    "this jump form is not lowered yet");
        return false;
    }
    parse_cf_label_t *cf_label = parse_cf_label_lookup(
        tu_state(((n00bb_lower_context_t *)context->user)->unit)->annotations,
        visit->node);
    if (!cf_label || cf_label->kind != PARSE_CF_JUMP) return false;
    n00bb_lower_context_t *lower = context->user;
    core_string_t target_name = core_tree_num_children(visit->node) > 1
        ? first_token_text(core_tree_child(visit->node, 1))
        : (core_string_t){0};
    n00bb_loop_frame_t *target = nullptr;
    for (size_t i = lower->loops.len; i > 0; i--) {
        if (lower->loops.data[i - 1].active
            && (!target_name.data || core_string_eq(
                    lower->loops.data[i - 1].label, target_name))
            && (!target || lower->loops.data[i - 1].order > target->order)) {
            target = &lower->loops.data[i - 1];
        }
    }
    n00bb_switch_frame_t *switch_target = nullptr;
    n00bb_typecase_frame_t *typecase_target = nullptr;
    bool is_break = core_string_eq(name, CORE_STRING_STATIC("break-stmt"));
    if (is_break) {
        uint64_t best_order = target ? target->order : 0;
        for (size_t i = lower->switches.len; i > 0; i--) {
            n00bb_switch_frame_t *candidate = &lower->switches.data[i - 1];
            if (candidate->active && (!target_name.data || core_string_eq(
                    candidate->label, target_name)) && (!switch_target
                || candidate->order > best_order)) {
                switch_target = candidate;
                typecase_target = nullptr;
                target = nullptr;
                best_order = candidate->order;
            }
        }
        for (size_t i = lower->typecases.len; i > 0; i--) {
            n00bb_typecase_frame_t *candidate =
                &lower->typecases.data[i - 1];
            if (candidate->active && (!target_name.data || core_string_eq(
                    candidate->label, target_name))
                && candidate->order > best_order) {
                typecase_target = candidate;
                switch_target = nullptr;
                target = nullptr;
                best_order = candidate->order;
            }
        }
    }
    if (!target && !switch_target && !typecase_target) {
        lower_error(context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(context, visit->node),
                    target_name.data
                        ? "jump target does not name an active lowered construct"
                        : "loop jump appears outside a lowered loop, switch, or typecase");
        return false;
    }
    ir_block_t *destination = typecase_target ? typecase_target->join
        : switch_target ? switch_target->join
        : is_break ? target->exit : target->continue_block;
    ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
    if (!jump || !ir_instr_operand_block(jump, destination)) {
        lower_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, visit->node),
                    "could not emit loop jump");
        return false;
    }
    jump->span = lower_span_for_tree(context, visit->node);
    if (switch_target) switch_target->join_reachable = true;
    if (typecase_target) typecase_target->join_reachable = true;
    context->reachable = false;
    return true;
}

static ir_lower_result_t
n00bb_lower_binary(ir_lower_context_t *context, parse_visit_t *visit,
                   ir_value_t *const *operands, size_t operand_count,
                   th_type *result_type, ir_span_t span, void *state)
{
    (void)state;
    if (operand_count != 2 || !result_type)
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                           "binary operation has incomplete operands or type");
    core_string_t spelling = core_tree_num_children(visit->node) > 1
        ? first_token_text(core_tree_child(visit->node, 1))
        : (core_string_t){0};
    ir_op_t op;
    if (core_string_eq(spelling, CORE_STRING_STATIC("+"))) op = IR_OP_ADD;
    else if (core_string_eq(spelling, CORE_STRING_STATIC("-"))) op = IR_OP_SUB;
    else if (core_string_eq(spelling, CORE_STRING_STATIC("*"))) op = IR_OP_MUL;
    else if (core_string_eq(spelling, CORE_STRING_STATIC("/"))) op = IR_OP_DIV;
    else if (core_string_eq(spelling, CORE_STRING_STATIC("%"))) op = IR_OP_REM;
    else return lower_error(context, IR_DIAG_TYPE_CLASS, span,
                            "this binary operator is not lowered yet");

    ir_instr_t *instruction = ir_instr_add(context->block, op);
    char name[48];
    snprintf(name, sizeof(name), "__n00bb_%llu",
             (unsigned long long)context->next_value_id++);
    if (!instruction || !ir_instr_set_dest(instruction, name, result_type)
        || !ir_instr_operand_value(instruction, operands[0])
        || !ir_instr_operand_value(instruction, operands[1]))
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit a binary IR operation");
    if (ir_op_accepts_mod(op, "trap") && !ir_instr_add_mod(instruction, "trap"))
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not set checked arithmetic semantics");
    instruction->span = span;
    return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
}

static bool
collect_sequence_literal_values(ir_lower_context_t *context,
                                parse_visit_t *visit, parse_tree_t *node,
                                core_list_t(n00bb_ir_value_ptr_t) *values)
{
    if (!node || core_tree_is_leaf(node)) return true;
    if (nt_is(visit->grammar, node, "expression")) {
        ir_value_t *value = ir_lower_node_value(context, node);
        if (!value) return false;
        core_list_push(*values, value);
        return true;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!collect_sequence_literal_values(context, visit,
                core_tree_child(node, i), values)) return false;
    return true;
}

static ir_lower_result_t
n00bb_lower_sequence_literal(ir_lower_context_t *context,
                             parse_visit_t *visit,
                             ir_value_t *const *operands,
                             size_t operand_count, th_type *result_type,
                             ir_span_t span, void *state)
{
    (void)operands;
    (void)operand_count;
    (void)state;
    if (!result_type)
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                           "sequence literal has no inferred type");
    if (th_kind_of(th_resolve(result_type)) != TH_SEQUENCE)
        return lower_error(context, IR_DIAG_TYPE_CLASS, span,
            "bracket literal aggregate lowering requires a resolved array type");
    core_list_t(n00bb_ir_value_ptr_t) values =
        core_list_new(n00bb_ir_value_ptr_t);
    bool collected = collect_sequence_literal_values(context, visit,
                                                      visit->node, &values);
    if (!collected) {
        core_list_free(values);
        return lower_error(context, IR_DIAG_VERIFY_PARSED, span,
                           "sequence literal element has no lowered value");
    }
    ir_instr_t *instruction = ir_instr_add(context->block, IR_OP_AGGREGATE);
    char name[48];
    snprintf(name, sizeof(name), "__n00bb_%llu",
             (unsigned long long)context->next_value_id++);
    bool ok = instruction
        && ir_instr_set_dest(instruction, name, result_type);
    for (size_t i = 0; ok && i < values.len; i++)
        ok = ir_instr_operand_value(instruction, values.data[i]);
    core_list_free(values);
    if (!ok)
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit a sequence aggregate");
    instruction->span = span;
    return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
}

static void
collect_positional_nodes(parse_grammar_t *grammar, parse_tree_t *node,
                         core_list_t(n00bb_tree_ptr_t) *arguments)
{
    if (!node || core_tree_is_leaf(node)) return;
    if (nt_is(grammar, node, "expression")) {
        core_list_push(*arguments, node);
        return;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        collect_positional_nodes(grammar, core_tree_child(node, i),
                                 arguments);
}

static ir_value_t *
n00bb_emit_temporary_address(ir_lower_context_t *context, th_type *type,
                             ir_span_t span)
{
    th_type *pointer_type = n00bb_indirect_type(
        ir_module_types(context->module), type);
    char name[48];
    snprintf(name, sizeof(name), "__n00bb_arg_%llu",
             (unsigned long long)context->next_value_id++);
    ir_instr_t *alloca = pointer_type
        ? ir_instr_add(context->block, IR_OP_ALLOCA) : nullptr;
    if (!alloca || !ir_instr_set_dest(alloca, name, pointer_type)
        || !ir_instr_operand_type(alloca, type)) return nullptr;
    alloca->span = span;
    return alloca->dest;
}

static bool
n00bb_emit_parameter_copyout(ir_lower_context_t *context,
                             ir_value_t *address, ir_local_t *target,
                             ir_span_t span)
{
    ir_value_t *value = n00bb_emit_indirect_read(context, address,
                                                  target->type, span);
    ir_instr_t *copy = value ? ir_instr_add(context->block, IR_OP_COPY)
                             : nullptr;
    if (!copy || !ir_instr_set_dest_local(copy, target)
        || !ir_instr_operand_value(copy, value)) return false;
    copy->span = span;
    return true;
}

static bool
n00bb_call_is_error_propagated(parse_grammar_t *grammar, parse_tree_t *node)
{
    parse_tree_t *parent = core_tree_node_value(node).parent;
    if (!parent || core_tree_is_leaf(parent)
        || core_tree_num_children(parent) == 0
        || core_tree_child(parent, 0) != node)
        return false;
    const parse_rule_t *rule = parse_tree_rule(grammar, parent);
    parse_annotation_t *op = rule
        ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_OPERATOR)
        : nullptr;
    return op && core_string_eq(op->op_kind,
                                 CORE_STRING_STATIC("unwrap_result"));
}

static parse_tree_t *
n00bb_context_hook(parse_grammar_t *grammar, parse_sym_entry_t *symbol,
                  const char *name)
{
    return context_symbol(grammar, symbol)
        ? descendant_nt(grammar, symbol->decl_node, name) : nullptr;
}

static bool
n00bb_emit_context_hook_call(ir_lower_context_t *context,
                             n00bb_with_item_t *item,
                             const char *hook_kind, ir_value_t *hook_value,
                             ir_span_t span, ir_value_t **outcome,
                             th_type **failure)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *state = item->context_unit
        ? tu_state(item->context_unit) : nullptr;
    parse_tree_t *hook = lower
        ? n00bb_context_hook(lower->grammar, item->context_symbol,
                             hook_kind) : nullptr;
    if (!hook || !state || !state->ir_module) return false;
    char name[256];
    if (!context_hook_ir_name(lower->grammar, item->context_symbol, hook,
                              name, sizeof(name))) return false;
    ir_func_t *function = ir_func_find(state->ir_module, name);
    ir_value_t *address = function
        ? ir_func_address(state->ir_module, function) : nullptr;
    th_type *signature = function ? th_resolve(function->type) : nullptr;
    if (!address || !signature || th_kind_of(signature) != TH_FUNCTION) {
        lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
                    "context hook has no lowered function");
        return false;
    }
    th_type *error_type = th_func_error(signature);
    bool can_fail = error_type
        && !n00bb_nil_type(state->types, error_type);
    if (can_fail && !failure) {
        lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                    "this context hook is not allowed to fail");
        return false;
    }
    size_t expected = th_func_nparams(signature);
    if (expected != item->captures.len + (hook_value ? 1 : 0)) {
        lower_error(context, IR_DIAG_TYPE_ARGS, span,
                    "context hook capture count does not match its signature");
        return false;
    }
    th_type *result_type = can_fail
        ? ir_call_outcome_type(context->module, signature) : nullptr;
    if (can_fail && !result_type) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not construct the context hook outcome type");
        return false;
    }
    ir_instr_t *call = ir_instr_add(context->block, IR_OP_CALL);
    char result_name[48];
    if (can_fail)
        snprintf(result_name, sizeof(result_name), "__n00bb_hook_%llu",
                 (unsigned long long)context->next_value_id++);
    th_type *context_type = th_resolve(item->context_symbol->type);
    size_t fixed_captures = context_type
        ? th_func_nparams(context_type) : 0;
    bool has_rest_capture = context_type
        && th_func_rest(context_type);
    bool emitted = context_type
        && item->captures.len == fixed_captures + has_rest_capture
        && call && (!can_fail
        || ir_instr_set_dest(call, result_name, result_type))
        && ir_instr_operand_value(call, address);
    for (size_t i = 0; emitted && i < fixed_captures; i++)
        emitted = ir_instr_operand_value(call, item->captures.data[i]);
    if (emitted && hook_value)
        emitted = ir_instr_operand_value(call, hook_value);
    if (emitted && has_rest_capture)
        emitted = ir_instr_operand_value(call,
            item->captures.data[fixed_captures]);
    if (!emitted) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not invoke context hook");
        return false;
    }
    call->span = span;
    if (outcome) *outcome = can_fail ? call->dest : nullptr;
    if (failure) *failure = can_fail ? error_type : nullptr;
    return true;
}

static bool
n00bb_emit_context_hook(ir_lower_context_t *context,
                        n00bb_with_item_t *item, const char *hook_kind,
                        ir_value_t *hook_value, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    parse_tree_t *hook = lower
        ? n00bb_context_hook(lower->grammar, item->context_symbol,
                             hook_kind) : nullptr;
    if (!hook) return true;
    return n00bb_emit_context_hook_call(context, item, hook_kind,
        hook_value, span, nullptr, nullptr);
}

static bool
n00bb_emit_context_hook_outcome_branch(ir_lower_context_t *context,
                                      ir_value_t *outcome, ir_span_t span,
                                      ir_block_t **ok_block,
                                      ir_block_t **error_block)
{
    uint64_t id = context->next_value_id++;
    char ok_name[48], error_name[48], tag_name[48], literal_name[48];
    char test_name[48];
    snprintf(ok_name, sizeof(ok_name), "__n00bb_hook_ok_%llu",
             (unsigned long long)id);
    snprintf(error_name, sizeof(error_name), "__n00bb_hook_error_%llu",
             (unsigned long long)id);
    snprintf(tag_name, sizeof(tag_name), "__n00bb_hook_tag_%llu",
             (unsigned long long)id);
    snprintf(literal_name, sizeof(literal_name), "__n00bb_hook_tag_lit_%llu",
             (unsigned long long)id);
    snprintf(test_name, sizeof(test_name), "__n00bb_hook_is_ok_%llu",
             (unsigned long long)id);
    *ok_block = ir_block_add(context->function, ok_name);
    *error_block = ir_block_add(context->function, error_name);
    th_type *uint_type = ir_type_parse(context->module, "uint");
    th_type *bool_type = ir_type_parse(context->module, "bool");
    ir_instr_t *tag = ir_instr_add(context->block, IR_OP_VARIANT_TAG);
    ir_instr_t *literal = ir_instr_add(context->block, IR_OP_CONST);
    ir_instr_t *test = ir_instr_add(context->block, IR_OP_EQ);
    bool emitted = *ok_block && *error_block && uint_type && bool_type
        && tag && ir_instr_set_dest(tag, tag_name, uint_type)
        && ir_instr_operand_value(tag, outcome)
        && literal && ir_instr_set_dest(literal, literal_name, uint_type)
        && ir_instr_operand_uint(literal, th_arm_tag("ok"))
        && test && ir_instr_set_dest(test, test_name, bool_type)
        && ir_instr_operand_value(test, tag->dest)
        && ir_instr_operand_value(test, literal->dest);
    ir_instr_t *branch = emitted
        ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
    emitted = emitted && branch
        && ir_instr_operand_value(branch, test->dest)
        && ir_instr_operand_block(branch, *ok_block)
        && ir_instr_operand_block(branch, *error_block);
    if (!emitted) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not branch on a context hook outcome");
        return false;
    }
    tag->span = literal->span = test->span = branch->span = span;
    return true;
}

static bool
n00bb_propagate_context_hook_failure(ir_lower_context_t *context,
                                     parse_tree_t *source,
                                     ir_value_t *error, ir_span_t span)
{
    if (!n00bb_emit_with_failure(context, source)) return false;
    int routed = n00bb_route_try_error(context, source, error, span);
    if (routed < 0) return false;
    if (routed) return true;
    th_type *caller_error = th_func_error(context->function->type);
    if (!caller_error
        || n00bb_nil_type(ir_module_types(context->module), caller_error)
        || !ir_type_eq(context->module, caller_error, error->type)) {
        lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
            "an uncaught context hook error must match the enclosing function's failure channel");
        return false;
    }
    ir_instr_t *fail = ir_instr_add(context->block, IR_OP_FAIL);
    if (!fail || !ir_instr_operand_value(fail, error)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not propagate the context hook error");
        return false;
    }
    fail->span = span;
    context->reachable = false;
    return true;
}

static bool
n00bb_emit_context_error_handler(ir_lower_context_t *context,
                                 n00bb_with_item_t *item,
                                 parse_tree_t *source, ir_value_t *error,
                                 ir_span_t span, ir_block_t *continuation)
{
    n00bb_lower_context_t *lower = context->user;
    parse_tree_t *hook = lower
        ? n00bb_context_hook(lower->grammar, item->context_symbol,
                             "context-error") : nullptr;
    if (!hook)
        return n00bb_propagate_context_hook_failure(context, source, error,
                                                    span);

    ir_value_t *outcome = nullptr;
    th_type *failure = nullptr;
    if (!n00bb_emit_context_hook_call(context, item, "context-error", error,
                                      span, &outcome, &failure)
        || !outcome || !failure)
        return false;
    ir_block_t *handled = nullptr, *failed = nullptr;
    if (!n00bb_emit_context_hook_outcome_branch(context, outcome, span,
                                                 &handled, &failed))
        return false;

    context->block = failed;
    ir_value_t *handler_error = n00bb_emit_outcome_payload(context, outcome,
        "error", failure, span);
    if (!handler_error
        || !n00bb_propagate_context_hook_failure(context, source,
                                                  handler_error, span))
        return false;

    context->block = handled;
    context->reachable = true;
    if (!n00bb_emit_context_hook(context, item, "context-ensure",
                                 item->provided_value, span)
        || !n00bb_emit_jump(context, continuation, span))
        return false;
    context->block = continuation;
    context->reachable = true;
    return true;
}

static bool
n00bb_emit_context_exit(ir_lower_context_t *context,
                        n00bb_with_item_t *item, parse_tree_t *source,
                        ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    parse_tree_t *hook = lower
        ? n00bb_context_hook(lower->grammar, item->context_symbol,
                             "context-exit") : nullptr;
    if (!hook)
        return n00bb_emit_context_hook(context, item, "context-ensure",
                                       item->provided_value, span);

    ir_value_t *outcome = nullptr;
    th_type *failure = nullptr;
    if (!n00bb_emit_context_hook_call(context, item, "context-exit",
            item->provided_value, span, &outcome, &failure)) return false;
    if (!outcome)
        return n00bb_emit_context_hook(context, item, "context-ensure",
                                       item->provided_value, span);

    ir_block_t *normal = nullptr, *failed = nullptr;
    if (!failure || !n00bb_emit_context_hook_outcome_branch(context, outcome,
            span, &normal, &failed)) return false;
    uint64_t id = context->next_value_id++;
    char join_name[48];
    snprintf(join_name, sizeof(join_name), "__n00bb_hook_join_%llu",
             (unsigned long long)id);
    ir_block_t *join = ir_block_add(context->function, join_name);
    if (!join) return false;

    context->block = normal;
    context->reachable = true;
    if (!n00bb_emit_context_hook(context, item, "context-ensure",
                                 item->provided_value, span)
        || !n00bb_emit_jump(context, join, span)) return false;

    context->block = failed;
    context->reachable = true;
    ir_value_t *error = n00bb_emit_outcome_payload(context, outcome, "error",
                                                    failure, span);
    if (!error) return false;
    parse_tree_t *error_hook = n00bb_context_hook(lower->grammar,
        item->context_symbol, "context-error");
    if (!error_hook) {
        if (!n00bb_propagate_context_hook_failure(context, source, error,
                                                   span)) return false;
    }
    else if (!n00bb_emit_context_error_handler(context, item, source, error,
                                                span, join)) return false;

    context->block = join;
    context->reachable = true;
    return true;
}

static bool
n00bb_emit_with_success(ir_lower_context_t *context, parse_tree_t *exit_node)
{
    n00bb_lower_context_t *lower = context->user;
    ir_span_t span = lower_span_for_tree(context, exit_node);
    for (size_t w = lower->withs.len; w > 0; w--) {
        n00bb_with_frame_t *frame = &lower->withs.data[w - 1];
        if (!frame->active || !n00bb_tree_contains(frame->node, exit_node))
            continue;
        for (size_t i = frame->items.len; i > 0; i--) {
            n00bb_with_item_t *item = &frame->items.data[i - 1];
            if (!n00bb_emit_context_exit(context, item, exit_node, span))
                return false;
        }
    }
    return true;
}

static bool
n00bb_emit_with_failure(ir_lower_context_t *context, parse_tree_t *exit_node)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_try_frame_t *handler = nullptr;
    for (size_t i = lower->tries.len; i > 0; i--) {
        n00bb_try_frame_t *candidate = &lower->tries.data[i - 1];
        if (candidate->active && candidate->body
            && n00bb_tree_contains(candidate->body, exit_node)) {
            handler = candidate;
            break;
        }
    }
    ir_span_t span = lower_span_for_tree(context, exit_node);
    for (size_t w = lower->withs.len; w > 0; w--) {
        n00bb_with_frame_t *frame = &lower->withs.data[w - 1];
        if (!frame->active || !n00bb_tree_contains(frame->node, exit_node)
            || (handler && n00bb_tree_contains(frame->node, handler->node)))
            continue;
        for (size_t i = frame->items.len; i > 0; i--) {
            n00bb_with_item_t *item = &frame->items.data[i - 1];
            if (!n00bb_emit_context_hook(context, item, "context-ensure",
                                             item->provided_value, span))
                return false;
        }
    }
    return true;
}

static int
n00bb_context_call(ir_lower_context_t *context, parse_visit_t *visit,
                   th_type *result_type, ir_span_t span,
                   ir_lower_result_t *result)
{
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !lower->withs.len)
        return 0;
    n00bb_with_frame_t *frame = &lower->withs.data[lower->withs.len - 1];
    if (!frame->active || !n00bb_tree_contains(frame->node, visit->node))
        return 0;

    parse_tree_t *callee = core_tree_num_children(visit->node)
        ? core_tree_child(visit->node, 0) : nullptr;
    resolve_module_member_nodes(visit->grammar, lower->unit, callee);
    parse_sym_entry_t *symbol = attached_symbol(visit->grammar, lower->unit,
                                                 callee);
    n00bb_tu_state_t *caller_state = tu_state(lower->unit);
    if (!symbol && caller_state && caller_state->annotations
        && caller_state->annotations->symtab)
        symbol = parse_symtab_resolve_node(
            caller_state->annotations->symtab, callee,
            CORE_STRING_STATIC(""));
    if (!context_symbol(visit->grammar, symbol)) return 0;
    core_translation_unit_t *owner = unit_for_symbol(lower->units, symbol);
    n00bb_tu_state_t *owner_state = owner ? tu_state(owner) : nullptr;
    parse_tree_t *enter = n00bb_context_hook(visit->grammar, symbol,
                                             "context-enter");
    char function_name[256];
    if (!owner_state || !enter
        || !context_hook_ir_name(visit->grammar, symbol, enter,
                                 function_name, sizeof(function_name)))
        return -1;
    ir_func_t *function = ir_func_find(owner_state->ir_module,
                                        function_name);
    ir_value_t *address = function
        ? ir_func_address(owner_state->ir_module, function) : nullptr;
    th_type *signature = symbol && symbol->type
        ? th_resolve(symbol->type) : nullptr;
    if (!address || !signature || th_kind_of(signature) != TH_FUNCTION)
        return -1;
    th_type *failure = th_func_error(signature);
    bool can_fail = failure
        && !n00bb_nil_type(owner_state->types, failure);

    parse_annotation_t *call_annot = visit->rule
        ? parse_rule_find_annot(visit->grammar, visit->rule,
                                PARSE_ANNOT_CALL) : nullptr;
    parse_tree_t *args = call_annot
        && parse_child_ref_is_set(call_annot->value_ref)
            ? parse_tree_resolve_child_ref(visit->grammar, visit->node,
                                           call_annot->value_ref)
            : descendant_nt(visit->grammar, visit->node, "call-args");
    core_list_t(n00bb_ir_value_ptr_t) captures =
        core_list_new(n00bb_ir_value_ptr_t);
    core_list_t(n00bb_call_actual_t) actuals =
        core_list_new(n00bb_call_actual_t);
    size_t parameter_count = th_func_nparams(signature);
    parse_tree_t *formal_params = child_nt(visit->grammar,
        symbol->decl_node, "param-decl");
    size_t rest_index = 0;
    parse_sym_entry_t *rest_symbol = formal_symbol_at(visit->grammar,
        formal_params, parameter_count, &rest_index);
    const th_param *rest_parameter = th_func_rest(signature);
    bool has_rest = rest_symbol && rest_parameter;
    if ((rest_symbol != nullptr) != (rest_parameter != nullptr)) {
        core_list_free(captures);
        core_list_free(actuals);
        lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                    "context rest parameter does not match its constructor signature");
        return -1;
    }
    core_list_t(n00bb_ir_value_ptr_t) rest_values =
        core_list_new(n00bb_ir_value_ptr_t);
    ir_value_t **value_by_parameter = core_alloc_array(
        ir_value_t *, parameter_count);
    bool *provided_by_parameter = core_alloc_array(bool, parameter_count);
    parse_tree_t *positional = args
        ? descendant_nt(visit->grammar, args, "positional-args") : nullptr;
    core_list_t(n00bb_tree_ptr_t) positional_nodes =
        core_list_new(n00bb_tree_ptr_t);
    if (positional)
        collect_positional_nodes(visit->grammar, positional,
                                 &positional_nodes);
    bool mapped = has_rest || positional_nodes.len <= parameter_count;
    for (size_t i = 0; mapped && i < positional_nodes.len; i++) {
        if (i < parameter_count) {
            provided_by_parameter[i] = true;
            core_list_push(actuals, ((n00bb_call_actual_t){
                positional_nodes.data[i], i}));
        }
        else if (has_rest)
            core_list_push(actuals, ((n00bb_call_actual_t){
                positional_nodes.data[i], SIZE_MAX}));
        else mapped = false;
    }
    core_list_free(positional_nodes);
    parse_tree_t *keyword_args = args
        ? descendant_nt(visit->grammar, args, "keyword-args") : nullptr;
    core_list_t(n00bb_tree_ptr_t) keyword_nodes =
        core_list_new(n00bb_tree_ptr_t);
    if (keyword_args)
        collect_keyword_nodes(visit->grammar, keyword_args, &keyword_nodes);
    for (size_t i = 0; mapped && i < keyword_nodes.len; i++) {
        parse_tree_t *identifier = first_identifier_leaf(visit->grammar,
                                                          keyword_nodes.data[i]);
        core_string_t name = identifier ? parse_tree_leaf_text(identifier)
                                        : (core_string_t){0};
        size_t index = 0;
        parse_sym_entry_t *formal = formal_symbol_named(visit->grammar,
            formal_params, name, &index);
        parse_tree_t *argument = child_nt(visit->grammar,
            keyword_nodes.data[i], "expression");
        if (!formal || !argument || index >= parameter_count) {
            lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
                        "named context argument does not match a parameter");
            mapped = false;
            break;
        }
        if (provided_by_parameter[index]) {
            lower_error(context, IR_DIAG_TYPE_ARGS,
                lower_span_for_tree(context, keyword_nodes.data[i]),
                "a context parameter is supplied more than once");
            mapped = false;
            break;
        }
        provided_by_parameter[index] = true;
        core_list_push(actuals, ((n00bb_call_actual_t){argument, index}));
    }
    core_list_free(keyword_nodes);
    size_t missing_required = 0;
    for (size_t i = 0; i < parameter_count; i++) {
        size_t formal_index = 0;
        parse_sym_entry_t *formal = formal_symbol_at(visit->grammar,
            formal_params, i, &formal_index);
        const th_param *param = th_func_param(signature, i);
        if (!formal || !param || param->mode != TH_IN) {
            lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                        "context constructor parameters must be in parameters");
            mapped = false;
            break;
        }
        if (!formal->is_optional && !provided_by_parameter[i])
            missing_required++;
    }
    if (mapped && missing_required) {
        lower_error(context, IR_DIAG_TYPE_ARGS, span,
                    "context call omits a required parameter");
        mapped = false;
    }
    void *binding_checkpoint = ir_lower_bindings_checkpoint(context);
    bool collected = mapped;
    for (size_t i = 0; collected && i < actuals.len; i++) {
        size_t index = actuals.data[i].parameter_index;
        ir_value_t *value = ir_lower_node_value(context,
                                                 actuals.data[i].node);
        if (index == SIZE_MAX) {
            if (!rest_parameter || !value || !th_unify_in_place(
                    ir_module_types(context->module), rest_parameter->type,
                    value->type)) {
                if (rest_parameter && value)
                    lower_error(context, IR_DIAG_TYPE_ARGS,
                        lower_span_for_tree(context, actuals.data[i].node),
                        "context variadic argument does not match its element type");
                collected = false;
                break;
            }
            core_list_push(rest_values, value);
            continue;
        }
        parse_sym_entry_t *formal = formal_symbol_at(visit->grammar,
            formal_params, index, &(size_t){0});
        if (!formal || !value
            || !ir_lower_bind_scoped_value(context, formal, value, nullptr)) {
            collected = false;
            break;
        }
        value_by_parameter[index] = value;
    }
    size_t remaining_defaults = 0;
    for (size_t i = 0; i < parameter_count; i++)
        if (!provided_by_parameter[i]) remaining_defaults++;
    while (collected && remaining_defaults) {
        bool progress = false;
        for (size_t i = 0; collected && i < parameter_count; i++) {
            if (provided_by_parameter[i]) continue;
            parse_sym_entry_t *formal = formal_symbol_at(visit->grammar,
                formal_params, i, &(size_t){0});
            if (!formal || !formal->is_optional) {
                collected = false;
                break;
            }
            parse_tree_t *default_expr = child_nt(visit->grammar,
                formal->decl_node, "expression");
            bool ready = true;
            for (size_t dependency = 0; dependency < parameter_count;
                 dependency++) {
                if (provided_by_parameter[dependency]) continue;
                parse_sym_entry_t *required = formal_symbol_at(
                    visit->grammar, formal_params, dependency,
                    &(size_t){0});
                if (required && required != formal
                    && required->is_optional
                    && n00bb_default_references(visit->grammar, default_expr,
                                                required)) {
                    ready = false;
                    break;
                }
            }
            if (!ready) continue;
            ir_lower_result_t result = default_expr && lower->lowerer
                ? ir_lower_tree(lower->lowerer, context, visit->grammar,
                                default_expr)
                : (ir_lower_result_t){IR_LOWER_FAILED, nullptr};
            if (result.status != IR_LOWER_VALUE || !result.value
                || !ir_lower_bind_scoped_value(context, formal,
                                                result.value, nullptr)) {
                collected = false;
                break;
            }
            value_by_parameter[i] = result.value;
            provided_by_parameter[i] = true;
            remaining_defaults--;
            progress = true;
        }
        if (collected && !progress) {
            lower_error(context, IR_DIAG_TYPE_ARGS, span,
                        "context default parameter dependencies are cyclic");
            collected = false;
        }
    }
    if (collected) {
        for (size_t i = 0; i < parameter_count; i++) {
            if (!value_by_parameter[i]) {
                collected = false;
                break;
            }
            core_list_push(captures, value_by_parameter[i]);
        }
    }
    if (collected && has_rest) {
        th_type *array_type = th_array(ir_module_types(context->module),
                                       rest_parameter->type);
        char array_name[48];
        snprintf(array_name, sizeof(array_name), "__n00bb_context_rest_%llu",
                 (unsigned long long)context->next_value_id++);
        ir_instr_t *array = array_type
            ? ir_instr_add(context->block, IR_OP_AGGREGATE) : nullptr;
        bool packed = array
            && ir_instr_set_dest(array, array_name, array_type);
        for (size_t i = 0; packed && i < rest_values.len; i++)
            packed = ir_instr_operand_value(array, rest_values.data[i]);
        if (!packed) {
            lower_error(context, IR_DIAG_INTERNAL, span,
                        "could not pack context variadic arguments into an array");
            collected = false;
        }
        else {
            array->span = span;
            core_list_push(captures, array->dest);
        }
    }
    core_list_free(rest_values);
    if (!ir_lower_bindings_restore(context, binding_checkpoint))
        collected = false;
    core_list_free(actuals);
    core_free(value_by_parameter);
    core_free(provided_by_parameter);
    if (!collected) {
        core_list_free(captures);
        if (!ir_diags_failed(context->diagnostics))
            lower_error(context, IR_DIAG_TYPE_ARGS, span,
                        "context arguments do not match the constructor signature");
        return -1;
    }
    for (size_t i = 0; i < parameter_count; i++) {
        const th_param *param = th_func_param(signature, i);
        if (!param || param->mode != TH_IN) {
            core_list_free(captures);
            lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                        "context constructor parameters must be in parameters");
            return -1;
        }
    }
    if (rest_parameter && rest_parameter->mode != TH_IN) {
        core_list_free(captures);
        lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                    "context variadic parameters must be in parameters");
        return -1;
    }
    th_type *provided_type = th_func_ret(signature);
    if (!ir_type_eq(context->module, provided_type, result_type)) {
        core_list_free(captures);
    {
        lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                    "context enter result does not match the inferred context value");
        core_list_free(captures);
        return -1;
    }
    }
    bool has_value = !n00bb_nil_type(ir_module_types(context->module),
                                      provided_type);
    th_type *outcome_type = can_fail
        ? ir_call_outcome_type(context->module, signature) : provided_type;
    if (!outcome_type) {
        core_list_free(captures);
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not construct the context enter outcome type");
        return -1;
    }
    ir_instr_t *call = ir_instr_add(context->block, IR_OP_CALL);
    ir_value_t *provided = nullptr;
    char value_name[48];
    snprintf(value_name, sizeof(value_name), "__n00bb_context_%llu",
             (unsigned long long)context->next_value_id++);
    bool emitted = call && ir_instr_set_dest(call, value_name, outcome_type)
        && ir_instr_operand_value(call, address);
    for (size_t i = 0; emitted && i < captures.len; i++)
        emitted = ir_instr_operand_value(call, captures.data[i]);
    if (!emitted) {
        core_list_free(captures);
    {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not invoke context enter hook");
        return -1;
    }
    }
    call->span = span;
    if (can_fail) {
        th_type *definition = ir_type_definition(context->module,
                                                  outcome_type);
        bool variant = false;
        th_type *ok_type = th_record_lookup(definition, "ok");
        th_type *error_type = th_record_lookup(definition, "error");
        if (!definition || th_kind_of(definition) != TH_RECORD
            || !th_named_info(outcome_type, nullptr, nullptr, &variant,
                              nullptr)
            || !variant || !ok_type || !error_type) {
            lower_error(context, IR_DIAG_TYPE_CLASS, span,
                        "context enter outcome is not a complete variant");
            return -1;
        }
        uint64_t id = context->next_value_id++;
        char ok_name[48], error_name[48];
        snprintf(ok_name, sizeof(ok_name), "__n00bb_enter_ok_%llu",
                 (unsigned long long)id);
        snprintf(error_name, sizeof(error_name), "__n00bb_enter_error_%llu",
                 (unsigned long long)id);
        ir_block_t *ok_block = ir_block_add(context->function, ok_name);
        ir_block_t *error_block = ir_block_add(context->function, error_name);
        th_type *uint_type = ir_type_parse(context->module, "uint");
        th_type *bool_type = ir_type_parse(context->module, "bool");
        char tag_name[48], literal_name[48], test_name[48];
        snprintf(tag_name, sizeof(tag_name), "__n00bb_enter_tag_%llu",
                 (unsigned long long)id);
        snprintf(literal_name, sizeof(literal_name),
                 "__n00bb_enter_tag_lit_%llu", (unsigned long long)id);
        snprintf(test_name, sizeof(test_name), "__n00bb_enter_is_ok_%llu",
                 (unsigned long long)id);
        ir_instr_t *tag = ir_instr_add(context->block, IR_OP_VARIANT_TAG);
        ir_instr_t *literal = ir_instr_add(context->block, IR_OP_CONST);
        ir_instr_t *test = ir_instr_add(context->block, IR_OP_EQ);
        bool branched = ok_block && error_block && uint_type && bool_type
            && tag && ir_instr_set_dest(tag, tag_name, uint_type)
            && ir_instr_operand_value(tag, call->dest)
            && literal && ir_instr_set_dest(literal, literal_name, uint_type)
            && ir_instr_operand_uint(literal, th_arm_tag("ok"))
            && test && ir_instr_set_dest(test, test_name, bool_type)
            && ir_instr_operand_value(test, tag->dest)
            && ir_instr_operand_value(test, literal->dest);
        ir_instr_t *branch = branched
            ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
        branched = branched && branch
            && ir_instr_operand_value(branch, test->dest)
            && ir_instr_operand_block(branch, ok_block)
            && ir_instr_operand_block(branch, error_block);
        if (!branched) {
            lower_error(context, IR_DIAG_INTERNAL, span,
                        "could not branch on context enter outcome");
            return -1;
        }
        tag->span = literal->span = test->span = branch->span = span;
        context->block = error_block;
        ir_value_t *enter_error = n00bb_emit_outcome_payload(
            context, call->dest, "error", error_type, span);
        if (!enter_error || !n00bb_emit_with_failure(context,
                                                     visit->node)) {
            lower_error(context, IR_DIAG_INTERNAL, span,
                        "could not unwind active contexts after failed enter");
            return -1;
        }
        int routed = n00bb_route_try_error(context, visit->node,
                                            enter_error, span);
        if (routed < 0) return -1;
        if (!routed) {
            th_type *caller_error = th_func_error(context->function->type);
            if (!caller_error
                || n00bb_nil_type(ir_module_types(context->module),
                                  caller_error)
                || !ir_type_eq(context->module, caller_error, error_type)) {
                lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                    "an uncaught context enter error must match the enclosing function's failure channel");
                return -1;
            }
            ir_instr_t *fail = ir_instr_add(context->block, IR_OP_FAIL);
            if (!fail || !ir_instr_operand_value(fail, enter_error)) {
                lower_error(context, IR_DIAG_INTERNAL, span,
                            "could not propagate context enter failure");
                return -1;
            }
            fail->span = span;
            context->reachable = false;
        }
        context->block = ok_block;
        context->reachable = true;
        if (!n00bb_nil_type(ir_module_types(context->module), ok_type))
            provided = n00bb_emit_outcome_payload(context, call->dest,
                                                   "ok", ok_type, span);
        if (!n00bb_nil_type(ir_module_types(context->module), ok_type)
            && !provided) {
            lower_error(context, IR_DIAG_INTERNAL, span,
                        "could not extract successful context enter value");
            return -1;
        }
    }
    else provided = has_value ? call->dest : nullptr;

    n00bb_with_item_t item = {
        .context_symbol = symbol,
        .context_unit = owner,
        .call_node = visit->node,
        .captures = captures,
        .provided_value = provided,
        .active = true,
    };
    core_list_push(frame->items, item);

    *result = provided
        ? (ir_lower_result_t){IR_LOWER_VALUE, provided}
        : (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr};
    return 1;
}

static bool
n00bb_bind_with_manager(ir_lower_context_t *context, parse_tree_t *node)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *tu = tu_state(lower->unit);
    if (!tu || !tu->annotations || !tu->annotations->symtab
        || core_tree_num_children(node) < 3)
        return true;
    core_string_t separator = first_token_text(core_tree_child(node, 1));
    if (!core_string_eq(separator, CORE_STRING_STATIC("="))
        && !core_string_eq(separator, CORE_STRING_STATIC(":")))
        return true;
    parse_sym_entry_t *binding = parse_symtab_resolve_node(
        tu->annotations->symtab, node, CORE_STRING_STATIC(""));
    if (!binding || !binding->type) {
        lower_error(context, IR_DIAG_VERIFY_TYPED,
            lower_span_for_tree(context, node),
            "named context binding has no inferred type");
        return false;
    }
    char local_name[48];
    snprintf(local_name, sizeof(local_name), "__n00bb_context_value_%llu",
             (unsigned long long)context->next_value_id++);
    ir_local_t *local = ir_local_add(context->function, local_name,
                                     binding->type);
    if (!local || !ir_lower_bind_local(context, binding, local)) {
        lower_error(context, IR_DIAG_INTERNAL,
            lower_span_for_tree(context, node),
            "could not bind the context-provided local");
        return false;
    }
    return true;
}

static bool
n00bb_bind_with_managers(ir_lower_context_t *context, parse_tree_t *node,
                         parse_tree_t *body)
{
    if (!node || node == body || core_tree_is_leaf(node)) return true;
    n00bb_lower_context_t *lower = context->user;
    if (nt_is(lower->grammar, node, "context-manager"))
        return n00bb_bind_with_manager(context, node);
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!n00bb_bind_with_managers(context, core_tree_child(node, i),
                                      body))
            return false;
    return true;
}

static bool
n00bb_lower_with_scope(ir_lower_context_t *context, parse_visit_t *visit,
                       bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !nt_is(visit->grammar, visit->node, "with-stmt"))
        return true;
    if (entering) {
        n00bb_with_frame_t frame = {
            .node = visit->node,
            .body = child_nt(visit->grammar, visit->node, "body"),
            .items = core_list_new(n00bb_with_item_t),
            .active = context->reachable,
        };
        if (frame.active && !n00bb_bind_with_managers(
                context, visit->node, frame.body))
            return false;
        core_list_push(lower->withs, frame);
        return true;
    }
    if (!lower->withs.len
        || lower->withs.data[lower->withs.len - 1].node != visit->node)
        return false;
    n00bb_with_frame_t *frame = &lower->withs.data[lower->withs.len - 1];
    if (frame->active && context->reachable) {
        ir_span_t span = lower_span_for_tree(context, visit->node);
        for (size_t i = frame->items.len; i > 0; i--) {
            n00bb_with_item_t *item = &frame->items.data[i - 1];
            if (!n00bb_emit_context_exit(context, item, visit->node, span))
                return false;
        }
    }
    for (size_t i = 0; i < frame->items.len; i++)
        core_list_free(frame->items.data[i].captures);
    core_list_free(frame->items);
    lower->withs.len--;
    return true;
}

static void
collect_keyword_nodes(parse_grammar_t *grammar, parse_tree_t *node,
                      core_list_t(n00bb_tree_ptr_t) *arguments)
{
    if (!node || core_tree_is_leaf(node)) return;
    if (nt_is(grammar, node, "call-kw-arg")) {
        core_list_push(*arguments, node);
        return;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        collect_keyword_nodes(grammar, core_tree_child(node, i), arguments);
}

static parse_sym_entry_t *
formal_symbol_named(parse_grammar_t *grammar, parse_tree_t *formals,
                   core_string_t name, size_t *index)
{
    if (!formals || !name.data) return nullptr;
    for (size_t i = 0;; i++) {
        size_t cursor = 0;
        parse_sym_entry_t *formal = formal_symbol_at(grammar, formals, i,
                                                      &cursor);
        if (!formal) break;
        if (core_string_eq(formal->name, name)) {
            if (index) *index = i;
            return formal;
        }
    }
    return nullptr;
}

static bool
n00bb_mark_call_lvalues(ir_lower_context_t *context, parse_visit_t *visit,
                        bool entering, void *state)
{
    (void)state;
    if (!entering || !visit->annot || !visit->node
        || nt_is(visit->grammar, visit->node, "context-call")) return true;
    n00bb_lower_context_t *lower = context->user;
    parse_tree_t *callee = core_tree_num_children(visit->node)
        ? core_tree_child(visit->node, 0) : nullptr;
    parse_sym_entry_t *symbol = core_tree_node_value(visit->node).sym;
    if (!symbol || symbol->kind != PARSE_SYM_FUNCTION)
        symbol = callee && lower
            ? call_function_symbol(visit->grammar, lower->unit, callee, false)
            : nullptr;
    if (!symbol || symbol->kind != PARSE_SYM_FUNCTION) return true;
    parse_tree_t *member = member_access_node(visit->grammar, callee);
    parse_tree_t *receiver = member && core_tree_num_children(member)
        ? core_tree_child(member, 0) : nullptr;
    parse_sym_entry_t *receiver_symbol = receiver && lower
        ? attached_symbol(visit->grammar, lower->unit, receiver) : nullptr;
    bool trait_call = member
        && (!receiver_symbol || receiver_symbol->kind != PARSE_SYM_MODULE);
    parse_tree_t *unused_callee = trait_call
        ? core_tree_num_children(member) > 2
            ? core_tree_child(member, 2) : nullptr
        : callee;
    if (unused_callee
        && !ir_lower_skip_value(context, visit->node, unused_callee))
        return false;

    parse_tree_t *args = parse_child_ref_is_set(visit->annot->value_ref)
        ? parse_tree_resolve_child_ref(visit->grammar, visit->node,
                                       visit->annot->value_ref)
        : descendant_nt(visit->grammar, visit->node, "call-args");
    parse_tree_t *positional = args
        ? descendant_nt(visit->grammar, args, "positional-args") : nullptr;
    core_list_t(n00bb_tree_ptr_t) actuals = core_list_new(n00bb_tree_ptr_t);
    if (positional)
        collect_positional_nodes(visit->grammar, positional, &actuals);
    size_t formal_offset = trait_call && receiver ? 1 : 0;
    parse_tree_t *formals = child_nt(visit->grammar, symbol->decl_node,
                                     "func-type-signature");
    formals = child_nt(visit->grammar, formals, "param-decl");
    bool ok = true;
    for (size_t i = 0; ok && i < actuals.len; i++) {
        size_t index = 0;
        parse_sym_entry_t *formal = formal_symbol_at(visit->grammar,
            formals, i + formal_offset, &index);
        if (!formal || !formal->has_mode || formal->mode == TH_IN) continue;
        parse_tree_t *lvalue = n00bb_simple_lvalue(actuals.data[i]);
        if (lvalue)
            ok = ir_lower_mark_lvalue(context, visit->node,
                                      actuals.data[i])
                && ir_lower_mark_lvalue(context, visit->node, lvalue);
    }
    core_list_free(actuals);
    parse_tree_t *keywords = args
        ? descendant_nt(visit->grammar, args, "keyword-args") : nullptr;
    core_list_t(n00bb_tree_ptr_t) named = core_list_new(n00bb_tree_ptr_t);
    if (keywords) collect_keyword_nodes(visit->grammar, keywords, &named);
    for (size_t i = 0; ok && i < named.len; i++) {
        parse_tree_t *identifier = first_identifier_leaf(visit->grammar,
                                                          named.data[i]);
        core_string_t name = identifier ? parse_tree_leaf_text(identifier)
                                        : (core_string_t){0};
        parse_sym_entry_t *formal = formal_symbol_named(visit->grammar,
            formals, name, nullptr);
        if (!formal || !formal->has_mode || formal->mode == TH_IN) continue;
        parse_tree_t *argument = child_nt(visit->grammar, named.data[i],
                                           "expression");
        parse_tree_t *lvalue = n00bb_simple_lvalue(argument);
        if (lvalue)
            ok = ir_lower_mark_lvalue(context, visit->node, argument)
                && ir_lower_mark_lvalue(context, visit->node, lvalue);
    }
    core_list_free(named);
    if (!ok)
        lower_error(context, IR_DIAG_INTERNAL,
            lower_span_for_tree(context, visit->node),
            "could not mark a call argument as an lvalue");
    return ok;
}

static ir_lower_result_t
n00bb_lower_call(ir_lower_context_t *context, parse_visit_t *visit,
                 ir_value_t *const *operands, size_t operand_count,
                 th_type *result_type, ir_span_t span, void *state)
{
    (void)operands;
    (void)state;
    if (operand_count || !result_type)
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                           "call lowering needs a result type");
    ir_lower_result_t context_result = {IR_LOWER_FAILED, nullptr};
    int context_call = n00bb_context_call(context, visit, result_type, span,
                                          &context_result);
    if (context_call < 0)
        return (ir_lower_result_t){IR_LOWER_FAILED, nullptr};
    if (context_call > 0)
        return context_result;
    if (nt_is(visit->grammar, visit->node, "context-call"))
        return lower_error(context, IR_DIAG_VERIFY_PARSED, span,
            "context call is not inside an active with scope or did not resolve to a context declaration");
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *caller_state = lower
        ? tu_state(lower->unit) : nullptr;
    parse_tree_t *call_node = visit->node;
    parse_annotation_t *call_annot = visit->rule
        ? parse_rule_find_annot(visit->grammar, visit->rule,
                                PARSE_ANNOT_CALL) : nullptr;
    if (!call_annot && visit->nt)
        call_annot = parse_nt_find_direct_annot(visit->nt,
                                                PARSE_ANNOT_CALL);
    parse_cf_label_t *call_label = lower && caller_state
        ? parse_cf_label_lookup(caller_state->annotations, call_node) : nullptr;
    parse_tree_t *callee = core_tree_num_children(call_node)
        ? core_tree_child(call_node, 0) : nullptr;
    if (callee && lower)
        resolve_module_member_nodes(visit->grammar, lower->unit, callee);
    parse_tree_t *member = member_access_node(visit->grammar, callee);
    parse_tree_t *receiver_node = member && core_tree_num_children(member)
        ? core_tree_child(member, 0) : nullptr;
    parse_sym_entry_t *receiver_symbol = receiver_node && lower
        ? attached_symbol(visit->grammar, lower->unit, receiver_node)
        : nullptr;
    bool trait_call = member
        && (!receiver_symbol || receiver_symbol->kind != PARSE_SYM_MODULE);
    parse_sym_entry_t *symbol = core_tree_node_value(call_node).sym;
    if (!symbol || symbol->kind != PARSE_SYM_FUNCTION) {
        symbol = trait_call && member
            ? core_tree_node_value(member).sym : nullptr;
        if ((!symbol || symbol->kind != PARSE_SYM_FUNCTION) && !trait_call)
            symbol = callee && lower
                ? call_function_symbol(visit->grammar, lower->unit, callee,
                                       false)
                : nullptr;
    }
    if (!symbol && trait_call)
        return lower_error(context, IR_DIAG_VERIFY_PARSED, span,
                           "trait-dispatched calls are not lowered yet");
    if (!symbol) {
        ir_diag_t *diagnostic = ir_diag_begin(context->diagnostics,
            IR_DIAG_UNDEFINED_SYMBOL, span, nullptr);
        ir_diag_message(diagnostic, context->diagnostics,
                        "call target did not resolve to a declared function");
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    }
    core_translation_unit_t *callee_unit = lower
        ? unit_for_symbol(lower->units, symbol) : nullptr;
    if (!callee_unit)
        return lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
                           "call target is outside the lowered import closure");
    n00bb_tu_state_t *callee_state = tu_state(callee_unit);
    ir_func_t *callee_function = callee_state && symbol
        ? ir_func_find(callee_state->ir_module, symbol->name.data) : nullptr;
    if (!callee_function)
        return lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
                           "call target has no IR declaration in its module");
    ir_value_t *callee_value =
        ir_func_address(callee_state->ir_module, callee_function);
    if (!callee_value)
        return lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
                           "call target has no lowered function declaration");
    th_type *signature = is_scheme(symbol->type)
        ? th_instantiate(ir_module_types(context->module), symbol->type)
        : th_resolve(symbol->type);
    if (!signature || th_kind_of(signature) != TH_FUNCTION)
        return lower_error(context, IR_DIAG_TYPE_CLASS, span,
                           "call target does not have a function type");
    th_type *error_type = th_func_error(signature);
    bool has_error = error_type && !n00bb_nil_type(
        ir_module_types(context->module), error_type);
    th_type *call_result_type = has_error
        ? ir_call_outcome_type(context->module, signature) : result_type;
    if (has_error && (!n00bb_call_is_error_propagated(visit->grammar,
                                                       call_node)
        || !ir_type_eq(context->module, result_type,
                       th_func_ret(signature))))
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                           "a call with an error channel must be used with postfix ! and its return type must match");
    parse_tree_t *args = call_label
        && call_label->kind == PARSE_CF_CALL ? call_label->then_body
        : call_annot
        && parse_child_ref_is_set(call_annot->value_ref)
            ? parse_tree_resolve_child_ref(visit->grammar, call_node,
                                           call_annot->value_ref)
            : descendant_nt(visit->grammar, call_node, "call-args");
    core_list_t(n00bb_ir_value_ptr_t) values =
        core_list_new(n00bb_ir_value_ptr_t);
    core_list_t(n00bb_ir_value_ptr_t) rest_values =
        core_list_new(n00bb_ir_value_ptr_t);
    core_list_t(n00bb_call_actual_t) actuals =
        core_list_new(n00bb_call_actual_t);
    core_list_t(n00bb_copyout_t) copyouts =
        core_list_new(n00bb_copyout_t);
    size_t parameter_count = th_func_nparams(signature);
    parse_tree_t *formal_params = child_nt(visit->grammar,
        child_nt(visit->grammar, symbol->decl_node,
                 "func-type-signature"), "param-decl");
    parse_tree_t **argument_by_parameter = core_alloc_array(
        parse_tree_t *, parameter_count);
    ir_value_t **value_by_parameter = core_alloc_array(
        ir_value_t *, parameter_count);
    bool *provided = core_alloc_array(bool, parameter_count);
    bool mapped = true;
    if (trait_call && receiver_node) {
        if (!parameter_count) mapped = false;
        else {
            argument_by_parameter[0] = receiver_node;
            provided[0] = true;
            core_list_push(actuals, ((n00bb_call_actual_t){
                receiver_node, 0}));
        }
    }
    parse_tree_t *positional = args
        ? descendant_nt(visit->grammar, args, "positional-args") : nullptr;
    core_list_t(n00bb_tree_ptr_t) positional_nodes =
        core_list_new(n00bb_tree_ptr_t);
    if (positional)
        collect_positional_nodes(visit->grammar, positional,
                                 &positional_nodes);
    size_t positional_offset = trait_call && receiver_node ? 1 : 0;
    for (size_t i = 0; mapped && i < positional_nodes.len; i++) {
        size_t index = positional_offset + i;
        if (index >= parameter_count) {
            if (!th_func_rest(signature) || trait_call) {
                mapped = false;
                break;
            }
            core_list_push(actuals, ((n00bb_call_actual_t){
                positional_nodes.data[i], SIZE_MAX}));
            continue;
        }
        if (provided[index]) {
            mapped = false;
            break;
        }
        argument_by_parameter[index] = positional_nodes.data[i];
        provided[index] = true;
        core_list_push(actuals, ((n00bb_call_actual_t){
            positional_nodes.data[i], index}));
    }
    core_list_free(positional_nodes);
    parse_tree_t *keyword_args = args
        ? descendant_nt(visit->grammar, args, "keyword-args") : nullptr;
    core_list_t(n00bb_tree_ptr_t) keyword_nodes =
        core_list_new(n00bb_tree_ptr_t);
    if (keyword_args)
        collect_keyword_nodes(visit->grammar, keyword_args, &keyword_nodes);
    for (size_t i = 0; mapped && i < keyword_nodes.len; i++) {
        parse_tree_t *identifier = first_identifier_leaf(visit->grammar,
                                                          keyword_nodes.data[i]);
        core_string_t name = identifier ? parse_tree_leaf_text(identifier)
                                        : (core_string_t){0};
        size_t index = 0;
        parse_sym_entry_t *formal = formal_symbol_named(visit->grammar,
            formal_params, name, &index);
        parse_tree_t *argument = child_nt(visit->grammar,
            keyword_nodes.data[i], "expression");
        if (!formal || !argument || index >= parameter_count) {
            lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
                        "named argument does not match a parameter");
            mapped = false;
            break;
        }
        if (provided[index]) {
            lower_error(context, IR_DIAG_TYPE_ARGS,
                lower_span_for_tree(context, keyword_nodes.data[i]),
                "a parameter is supplied more than once");
            mapped = false;
            break;
        }
        argument_by_parameter[index] = argument;
        provided[index] = true;
        core_list_push(actuals, ((n00bb_call_actual_t){argument, index}));
    }
    core_list_free(keyword_nodes);
    size_t missing_required = 0;
    for (size_t i = 0; i < parameter_count; i++) {
        size_t formal_index = 0;
        parse_sym_entry_t *formal = formal_symbol_at(visit->grammar,
            formal_params, i, &formal_index);
        if (!formal || !formal->is_optional) {
            if (!provided[i]) missing_required++;
        }
    }
    if (!mapped || missing_required) {
        if (!mapped && !ir_diags_failed(context->diagnostics))
            lower_error(context, IR_DIAG_TYPE_ARGS, span,
                        "call argument does not match a parameter");
        if (mapped)
            lower_error(context, IR_DIAG_TYPE_ARGS, span,
                "call omits a required parameter");
        core_list_free(actuals);
        core_list_free(values);
        core_list_free(rest_values);
        core_list_free(copyouts);
        core_free(argument_by_parameter);
        core_free(value_by_parameter);
        core_free(provided);
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    }
    bool collected = true;
    for (size_t i = 0; collected && i < actuals.len; i++) {
        size_t index = actuals.data[i].parameter_index;
        if (index == SIZE_MAX) {
            const th_param *rest = th_func_rest(signature);
            ir_value_t *value = ir_lower_node_value(context,
                                                     actuals.data[i].node);
            if (!rest || !value || !th_unify_in_place(
                    ir_module_types(context->module), rest->type,
                    value->type)) {
                if (rest && value)
                    lower_error(context, IR_DIAG_TYPE_ARGS,
                        lower_span_for_tree(context, actuals.data[i].node),
                        "variadic argument does not match its element type");
                collected = false;
                break;
            }
            core_list_push(rest_values, value);
            continue;
        }
        const th_param *parameter = th_func_param(signature, index);
        parse_tree_t *argument = actuals.data[i].node;
        parse_sym_entry_t *formal = formal_symbol_at(visit->grammar,
            formal_params, index, &(size_t){0});
        th_mode mode = formal && formal->has_mode
            ? formal->mode : TH_IN;
        if (!parameter || !argument) {
            collected = false;
            break;
        }
        if (mode == TH_IN) {
            ir_value_t *value = ir_lower_node_value(context, argument);
            if (!value || !th_unify_in_place(
                    ir_module_types(context->module), parameter->type,
                    value->type)) {
                if (value)
                    lower_error(context, IR_DIAG_TYPE_ARGS,
                        lower_span_for_tree(context, argument),
                        "argument type does not match its parameter");
                collected = false;
                break;
            }
            value_by_parameter[index] = value;
            continue;
        }
        if (mode != TH_OUT && mode != TH_INOUT) {
            collected = false;
            break;
        }
        parse_tree_t *lvalue = n00bb_simple_lvalue(argument);
        parse_sym_entry_t *arg_symbol = lvalue
            ? attached_symbol(visit->grammar, lower->unit, lvalue) : nullptr;
        ir_local_t *target = ir_lower_bound_local(context, arg_symbol);
        if (!target || (arg_symbol->mutability != PARSE_SYM_MUTABLE
                        && arg_symbol->mutability != PARSE_SYM_GLOBAL)
            || !ir_type_eq(context->module, target->type,
                           parameter->type)) {
            lower_error(context, IR_DIAG_TYPE_ARGS,
                lower_span_for_tree(context, argument),
                "out and modify arguments must be mutable local variables of the parameter type");
            collected = false;
            break;
        }
        bool aliases = false;
        for (size_t previous = 0; previous < copyouts.len; previous++)
            aliases |= copyouts.data[previous].symbol == arg_symbol;
        if (aliases) {
            lower_error(context, IR_DIAG_TYPE_ARGS,
                lower_span_for_tree(context, argument),
                "the same variable cannot be passed to multiple out or modify parameters");
            collected = false;
            break;
        }
        ir_value_t *address = n00bb_emit_temporary_address(context,
            parameter->type, lower_span_for_tree(context, argument));
        if (!address) {
            collected = false;
            break;
        }
        if (mode == TH_INOUT) {
            ir_value_t *initial = n00bb_emit_local_read(context, target, span);
            if (!initial || !n00bb_emit_indirect_write(context, address,
                                                        initial, span)) {
                collected = false;
                break;
            }
        }
        value_by_parameter[index] = address;
        core_list_push(copyouts,
            ((n00bb_copyout_t){arg_symbol, target, address}));
    }
    for (size_t i = 0; collected && i < parameter_count; i++) {
        size_t formal_index = 0;
        parse_sym_entry_t *formal = formal_symbol_at(visit->grammar,
            formal_params, i, &formal_index);
        const th_param *parameter = th_func_param(signature, i);
        if (!formal || !parameter) {
            collected = false;
            break;
        }
        if (formal->is_optional) {
            if (parameter->mode != TH_IN) {
                collected = false;
                break;
            }
            ir_value_t *wrapped = n00bb_make_default_argument(context,
                parameter->type, value_by_parameter[i], provided[i],
                provided[i]
                    ? lower_span_for_tree(context,
                                          argument_by_parameter[i])
                    : lower_span_for_tree(context, formal->decl_node));
            if (!wrapped) {
                collected = false;
                break;
            }
            core_list_push(values, wrapped);
        }
        else if (value_by_parameter[i])
            core_list_push(values, value_by_parameter[i]);
        else {
            collected = false;
        }
    }
    if (collected && th_func_rest(signature)) {
        const th_param *rest = th_func_rest(signature);
        th_type *array_type = th_array(ir_module_types(context->module),
                                       rest->type);
        char array_name[48];
        snprintf(array_name, sizeof(array_name), "__n00bb_rest_%llu",
                 (unsigned long long)context->next_value_id++);
        ir_instr_t *array = array_type
            ? ir_instr_add(context->block, IR_OP_AGGREGATE) : nullptr;
        bool packed = array
            && ir_instr_set_dest(array, array_name, array_type);
        for (size_t i = 0; packed && i < rest_values.len; i++)
            packed = ir_instr_operand_value(array, rest_values.data[i]);
        if (!packed) {
            lower_error(context, IR_DIAG_INTERNAL, span,
                        "could not pack variadic arguments into an array");
            collected = false;
        }
        else {
            array->span = span;
            core_list_push(values, array->dest);
        }
    }
    core_list_free(actuals);
    core_list_free(rest_values);
    core_free(argument_by_parameter);
    core_free(value_by_parameter);
    core_free(provided);
    if (!collected) {
        core_list_free(values);
        core_list_free(copyouts);
        if (!ir_diags_failed(context->diagnostics))
            lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                "call arguments could not be lowered for their parameter modes");
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    }
    ir_instr_t *instruction = ir_instr_add(context->block, IR_OP_CALL);
    char name[48];
    do {
        snprintf(name, sizeof(name), "__n00bb_%llu",
                 (unsigned long long)context->next_value_id++);
    } while (ir_value_find(context->function, name));
    bool emitted = instruction
        && ir_instr_set_dest(instruction, name, call_result_type)
        && ir_instr_operand_value(instruction, callee_value);
    for (size_t i = 0; emitted && i < values.len; i++)
        emitted = ir_instr_operand_value(instruction, values.data[i]);
    core_list_free(values);
    if (!emitted) {
        core_list_free(copyouts);
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit the function call");
    }
    instruction->span = span;
    if (copyouts.len) {
        if (has_error) {
            core_list_push(lower->pending_call_copyouts,
                ((n00bb_pending_copyouts_t){instruction->dest, copyouts}));
        }
        else {
            bool copied = true;
            for (size_t i = 0; copied && i < copyouts.len; i++)
                copied = n00bb_emit_parameter_copyout(context,
                    copyouts.data[i].address, copyouts.data[i].local, span);
            core_list_free(copyouts);
            if (!copied)
                return lower_error(context, IR_DIAG_INTERNAL, span,
                                   "could not copy out call parameters");
        }
    }
    else core_list_free(copyouts);
    return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
}

static ir_value_t *
n00bb_emit_outcome_payload(ir_lower_context_t *context, ir_value_t *outcome,
                          const char *arm, th_type *type, ir_span_t span)
{
    ir_instr_t *payload = ir_instr_add(context->block, IR_OP_VARIANT_PAYLOAD);
    char name[48];
    do {
        snprintf(name, sizeof(name), "__n00bb_outcome_%llu",
                 (unsigned long long)context->next_value_id++);
    } while (ir_value_find(context->function, name));
    if (!payload || !ir_instr_set_dest(payload, name, type)
        || !ir_instr_operand_value(payload, outcome)
        || !ir_instr_operand_name(payload, arm))
        return nullptr;
    payload->span = span;
    return payload->dest;
}

static bool
n00bb_operator_error(ir_lower_context_t *context, ir_diagcode_t code,
                     ir_span_t span, const char *message)
{
    lower_error(context, code, span, message);
    return false;
}

static bool
n00bb_lower_operator_annotation(ir_lower_context_t *context,
                                parse_visit_t *visit, bool entering,
                                void *state)
{
    (void)state;
    if (!core_string_eq(visit->annot->op_kind,
                        CORE_STRING_STATIC("unwrap_result"))) {
        if (!entering) return true;
        lower_error(context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(context, visit->node),
                    "this operator is not lowered yet");
        return false;
    }
    if (entering) return true;

    ir_span_t span = lower_span_for_tree(context, visit->node);
    parse_tree_t *operand_node = core_tree_num_children(visit->node)
        ? core_tree_child(visit->node, 0) : nullptr;
    ir_value_t *outcome = operand_node
        ? ir_lower_node_value(context, operand_node) : nullptr;
    bool variant = false;
    th_type *definition = outcome
        ? ir_type_definition(context->module, outcome->type) : nullptr;
    if (!outcome || !th_named_info(outcome->type, nullptr, nullptr, &variant,
                                   nullptr) || !variant
        || !definition || th_kind_of(definition) != TH_RECORD)
        return n00bb_operator_error(context, IR_DIAG_TYPE_CLASS, span,
                                    "postfix ! requires a call with a failure channel");

    th_type *ok_type = th_record_lookup(definition, "ok");
    th_type *error_type = th_record_lookup(definition, "error");
    if (!ok_type || !error_type)
        return n00bb_operator_error(context, IR_DIAG_TYPE_CLASS, span,
                                    "postfix ! requires a complete call outcome type");
    if (n00bb_nil_type(ir_module_types(context->module), ok_type))
        return n00bb_operator_error(context, IR_DIAG_TYPE_CLASS, span,
                                    "postfix ! cannot be used as a value when the call returns no value");

    char ok_block_name[48], error_block_name[48];
    uint64_t block_id = context->next_value_id++;
    snprintf(ok_block_name, sizeof(ok_block_name),
             "__n00bb_unwrap_ok_%llu", (unsigned long long)block_id);
    snprintf(error_block_name, sizeof(error_block_name),
             "__n00bb_unwrap_error_%llu", (unsigned long long)block_id);
    ir_block_t *ok_block = ir_block_add(context->function, ok_block_name);
    ir_block_t *error_block = ir_block_add(context->function,
                                           error_block_name);
    th_type *uint_type = ir_type_parse(context->module, "uint");
    th_type *bool_type = ir_type_parse(context->module, "bool");
    char tag_name[48], literal_name[48], test_name[48];
    ir_instr_t *tag = ir_instr_add(context->block, IR_OP_VARIANT_TAG);
    do {
        snprintf(tag_name, sizeof(tag_name), "__n00bb_tag_%llu",
                 (unsigned long long)context->next_value_id++);
    } while (ir_value_find(context->function, tag_name));
    ir_instr_t *literal = ir_instr_add(context->block, IR_OP_CONST);
    do {
        snprintf(literal_name, sizeof(literal_name), "__n00bb_tag_lit_%llu",
                 (unsigned long long)context->next_value_id++);
    } while (ir_value_find(context->function, literal_name));
    ir_instr_t *test = ir_instr_add(context->block, IR_OP_EQ);
    do {
        snprintf(test_name, sizeof(test_name), "__n00bb_is_ok_%llu",
                 (unsigned long long)context->next_value_id++);
    } while (ir_value_find(context->function, test_name));
    bool emitted = ok_block && error_block && uint_type && bool_type
        && tag && ir_instr_set_dest(tag, tag_name, uint_type)
        && ir_instr_operand_value(tag, outcome)
        && literal && ir_instr_set_dest(literal, literal_name, uint_type)
        && ir_instr_operand_uint(literal, th_arm_tag("ok"))
        && test && ir_instr_set_dest(test, test_name, bool_type)
        && ir_instr_operand_value(test, tag->dest)
        && ir_instr_operand_value(test, literal->dest);
    ir_instr_t *branch = emitted
        ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
    emitted = emitted && branch
        && ir_instr_operand_value(branch, test->dest)
        && ir_instr_operand_block(branch, ok_block)
        && ir_instr_operand_block(branch, error_block);
    if (!emitted)
        return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
                                    "could not branch on the call outcome");
    tag->span = literal->span = test->span = branch->span = span;

    context->block = error_block;
    ir_value_t *error = n00bb_emit_outcome_payload(
        context, outcome, "error", error_type, span);
    if (!error || !n00bb_emit_with_failure(context, visit->node))
        return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
                                    "could not unwind active context managers");
    int routed = error
        ? n00bb_route_try_error(context, visit->node, error, span) : -1;
    if (routed < 0)
        return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
                                    "could not route the call error");
    if (!routed) {
        th_type *caller_error = th_func_error(context->function->type);
        if (!caller_error
            || n00bb_nil_type(ir_module_types(context->module), caller_error)
            || !ir_type_eq(context->module, caller_error, error_type))
            return n00bb_operator_error(context, IR_DIAG_TYPE_MISMATCH, span,
                "an uncaught call error must match the enclosing function's failure channel");
        ir_instr_t *fail = error
            ? ir_instr_add(context->block, IR_OP_FAIL) : nullptr;
        if (!fail || !ir_instr_operand_value(fail, error))
            return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
                                        "could not propagate the call error");
        fail->span = span;
        context->reachable = false;
    }

    context->block = ok_block;
    context->reachable = true;
    ir_value_t *value = n00bb_emit_outcome_payload(
        context, outcome, "ok", ok_type, span);
    n00bb_lower_context_t *lower = context->user;
    for (size_t i = 0; value && lower
            && i < lower->pending_call_copyouts.len; i++) {
        n00bb_pending_copyouts_t *pending =
            &lower->pending_call_copyouts.data[i];
        if (pending->outcome != outcome) continue;
        for (size_t c = 0; value && c < pending->items.len; c++)
            value = n00bb_emit_parameter_copyout(context,
                pending->items.data[c].address,
                pending->items.data[c].local, span)
                ? value : nullptr;
        core_list_free(pending->items);
        pending->items = core_list_new(n00bb_copyout_t);
        pending->outcome = nullptr;
        break;
    }
    if (!value || !ir_lower_set_node_value(context, visit->node, value))
        return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
                                    "could not extract the successful call value");
    return true;
}

static bool
n00bb_lower_declare_local(ir_lower_context_t *context, parse_visit_t *visit,
                          bool entering, void *state)
{
    (void)state;
    if (!entering || !nt_is(visit->grammar, visit->node, "sym-decl"))
        return true;
    parse_sym_entry_t *symbol = visit->pn ? visit->pn->sym : nullptr;
    if (symbol && symbol->scope
        && core_string_eq(symbol->scope->scope_tag,
                          CORE_STRING_STATIC("module")))
        return true;
    if (!symbol || !symbol->type) {
        lower_error(context, IR_DIAG_VERIFY_TYPED,
            lower_span_for_tree(context, visit->node),
            "local declaration has no resolved type");
        return false;
    }
    char name[48];
    snprintf(name, sizeof(name), "__n00bb_local_%llu",
             (unsigned long long)context->next_value_id++);
    ir_local_t *local = ir_local_add(context->function, name, symbol->type);
    if (!local || !ir_lower_bind_local(context, symbol, local)) {
        lower_error(context, IR_DIAG_INTERNAL,
            lower_span_for_tree(context, visit->node),
            "could not bind local declaration");
        return false;
    }
    return true;
}

static parse_tree_t *
n00bb_simple_lvalue(parse_tree_t *node)
{
    while (node && !core_tree_is_leaf(node)) {
        if (core_tree_node_value(node).sym) return node;
        if (core_tree_num_children(node) != 1) return nullptr;
        node = core_tree_child(node, 0);
    }
    return nullptr;
}

static bool
n00bb_check_assignment(ir_lower_context_t *context, parse_visit_t *visit,
                       bool entering, void *state)
{
    (void)state;
    if (!entering) return true;
    bool compound = nt_is(visit->grammar, visit->node,
                          "binop-assign-stmt");
    if (!compound && !nt_is(visit->grammar, visit->node, "assign-stmt"))
        return true;
    parse_tree_t *target = parse_tree_resolve_child_ref(
        visit->grammar, visit->node, visit->annot->name_ref);
    parse_tree_t *lvalue = n00bb_simple_lvalue(target);
    parse_sym_entry_t *symbol = lvalue
        ? core_tree_node_value(lvalue).sym : nullptr;
    if (symbol && (symbol->mutability == PARSE_SYM_CONST
                   || symbol->mutability == PARSE_SYM_IMMUTABLE)) {
        lower_error(context, IR_DIAG_TYPE_ARGS,
            lower_span_for_tree(context, visit->node),
            "cannot assign to an immutable local");
        return false;
    }
    return true;
}

static ir_lower_result_t
n00bb_lower_compound_assignment(ir_lower_context_t *context,
                                parse_visit_t *visit,
                                ir_value_t *const *operands,
                                size_t operand_count,
                                th_type *result_type, ir_span_t span,
                                void *state)
{
    (void)result_type;
    (void)state;
    if (operand_count != 1 || !operands || !operands[0])
        return lower_error(context, IR_DIAG_VERIFY_PARSED, span,
                           "compound assignment needs one lowered value");
    parse_annotation_t *assigns = visit->rule
        ? parse_rule_find_annot(visit->grammar, visit->rule,
                                PARSE_ANNOT_ASSIGNS) : nullptr;
    if (!assigns && visit->nt)
        assigns = parse_nt_find_direct_annot(visit->nt,
                                             PARSE_ANNOT_ASSIGNS);
    parse_tree_t *target = assigns
        ? parse_tree_resolve_child_ref(visit->grammar, visit->node,
                                       assigns->name_ref) : nullptr;
    parse_tree_t *lvalue = n00bb_simple_lvalue(target);
    parse_sym_entry_t *symbol = lvalue
        ? core_tree_node_value(lvalue).sym : nullptr;
    ir_local_t *local = ir_lower_bound_local(context, symbol);
    if (!local)
        return lower_error(context, IR_DIAG_VERIFY_PARSED, span,
                           "compound assignment currently requires a local variable");
    if (!ir_type_eq(context->module, local->type, operands[0]->type))
        return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                           "compound assignment value does not match the local type");

    parse_tree_t *operator = core_tree_num_children(visit->node) > 1
        ? core_tree_child(visit->node, 1) : nullptr;
    core_string_t spelling = first_token_text(operator);
    ir_op_t op;
    if (core_string_eq(spelling, CORE_STRING_STATIC("+="))) op = IR_OP_ADD;
    else if (core_string_eq(spelling, CORE_STRING_STATIC("-="))) op = IR_OP_SUB;
    else if (core_string_eq(spelling, CORE_STRING_STATIC("*="))) op = IR_OP_MUL;
    else if (core_string_eq(spelling, CORE_STRING_STATIC("/="))) op = IR_OP_DIV;
    else if (core_string_eq(spelling, CORE_STRING_STATIC("%="))) op = IR_OP_REM;
    else if (core_string_eq(spelling, CORE_STRING_STATIC("&="))) op = IR_OP_AND;
    else if (core_string_eq(spelling, CORE_STRING_STATIC("|="))) op = IR_OP_OR;
    else if (core_string_eq(spelling, CORE_STRING_STATIC("^="))) op = IR_OP_XOR;
    else if (core_string_eq(spelling, CORE_STRING_STATIC("<<="))) op = IR_OP_SHL;
    else if (core_string_eq(spelling, CORE_STRING_STATIC(">>="))) op = IR_OP_SHR;
    else return lower_error(context, IR_DIAG_TYPE_CLASS, span,
                            "this compound assignment operator is not lowered");

    ir_value_t *old_value = n00bb_emit_local_read(context, local, span);
    ir_value_t *updated = old_value
        ? n00bb_emit_binary_ir(context, op, local->type, old_value,
                               operands[0], span) : nullptr;
    ir_instr_t *store = updated
        ? ir_instr_add(context->block, IR_OP_COPY) : nullptr;
    if (!store || !ir_instr_set_dest_local(store, local)
        || !ir_instr_operand_value(store, updated))
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit compound assignment store");
    store->span = span;
    return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
}

static const parse_sym_entry_t *
function_symbol_at(parse_grammar_t *grammar, parse_tree_t *node)
{
    return node && nt_is(grammar, node, "func-def")
        ? core_tree_node_value(node).sym : nullptr;
}

static const char *
context_hook_suffix(parse_grammar_t *grammar, parse_tree_t *hook)
{
    if (nt_is(grammar, hook, "context-enter")) return "enter";
    if (nt_is(grammar, hook, "context-exit")) return "exit";
    if (nt_is(grammar, hook, "context-error")) return "error";
    if (nt_is(grammar, hook, "context-ensure")) return "ensure";
    return nullptr;
}

static bool
context_hook_ir_name(parse_grammar_t *grammar, parse_sym_entry_t *symbol,
                     parse_tree_t *hook, char *buffer, size_t size)
{
    const char *suffix = context_hook_suffix(grammar, hook);
    if (!symbol || !symbol->name.data || !suffix || !buffer || !size)
        return false;
    int written = snprintf(buffer, size, "__n00bb_context_%.*s_%s",
        (int)symbol->name.u8_bytes, symbol->name.data, suffix);
    return written > 0 && (size_t)written < size;
}

static bool
context_symbol(parse_grammar_t *grammar, parse_sym_entry_t *symbol)
{
    return symbol && symbol->decl_node
        && nt_is(grammar, symbol->decl_node, "context-def");
}

static th_type *
context_hook_ir_type(th_ctx *types, th_type *source_type)
{
    if (!types || !source_type || th_kind_of(source_type) != TH_FUNCTION)
        return nullptr;
    core_list_t(th_param) parameters = core_list_new(th_param);
    for (size_t i = 0; i < th_func_nparams(source_type); i++) {
        const th_param *parameter = th_func_param(source_type, i);
        if (!parameter) {
            core_list_free(parameters);
            return nullptr;
        }
        core_list_push(parameters, *parameter);
    }
    const th_param *rest = th_func_rest(source_type);
    if (rest) {
        th_type *array = th_array(types, rest->type);
        if (!array) {
            core_list_free(parameters);
            return nullptr;
        }
        core_list_push(parameters, ((th_param){.mode = TH_IN, .type = array}));
    }
    core_list_t(th_kwparam) keywords = core_list_new(th_kwparam);
    for (size_t i = 0; i < th_func_nkw(source_type); i++) {
        const th_kwparam *keyword = th_func_kw(source_type, i);
        if (!keyword) {
            core_list_free(parameters);
            core_list_free(keywords);
            return nullptr;
        }
        core_list_push(keywords, *keyword);
    }
    th_type *type = th_func_with_error(types, th_func_ret(source_type),
        th_func_error(source_type), parameters.data, parameters.len, nullptr,
        keywords.data, keywords.len);
    core_list_free(parameters);
    core_list_free(keywords);
    return type;
}

static bool
add_context_hook_functions(parse_grammar_t *grammar,
                           core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->ir_module) return false;
    core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
    bool ok = true;
    for (size_t s = 0; ok && s < scopes.len; s++) {
        for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
             ok && symbol; symbol = symbol->next_in_scope) {
            if (!context_symbol(grammar, symbol)) continue;
            static const char *hooks[] = {
                "context-enter", "context-exit", "context-error",
                "context-ensure",
            };
            for (size_t i = 0; ok && i < sizeof(hooks) / sizeof(hooks[0]);
                 i++) {
                parse_tree_t *hook = descendant_nt(grammar,
                    symbol->decl_node, hooks[i]);
                if (!hook) continue;
                char name[256];
                th_type *type = context_hook_ir_type(state->types,
                    core_tree_node_value(hook).type);
                ok = context_hook_ir_name(grammar, symbol, hook,
                                          name, sizeof(name))
                    && type && !ir_func_find(state->ir_module, name)
                    && ir_func_add(state->ir_module, name, type,
                                   IR_LINK_INTERNAL);
            }
        }
    }
    core_list_free(scopes);
    return ok;
}

static void
collect_ir_parameters(parse_grammar_t *grammar, parse_tree_t *node,
                      core_list_t(parse_sym_entry_ptr_t) *parameters,
                      parse_sym_entry_t **rest)
{
    if (!node || core_tree_is_leaf(node)) return;
    if (nt_is(grammar, node, "pos-param")
        || nt_is(grammar, node, "default-param")) {
        core_list_push(*parameters, core_tree_node_value(node).sym);
        return;
    }
    if (nt_is(grammar, node, "vargs-param")) {
        *rest = core_tree_node_value(node).sym;
        return;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        collect_ir_parameters(grammar, core_tree_child(node, i), parameters,
                               rest);
}

static parse_sym_entry_t *
formal_symbol_at(parse_grammar_t *grammar, parse_tree_t *node,
                size_t target, size_t *index)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    if (nt_is(grammar, node, "pos-param")
        || nt_is(grammar, node, "default-param")
        || nt_is(grammar, node, "vargs-param")) {
        parse_sym_entry_t *symbol = core_tree_node_value(node).sym;
        if ((*index)++ == target) return symbol;
        return nullptr;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_sym_entry_t *found = formal_symbol_at(grammar,
            core_tree_child(node, i), target, index);
        if (found) return found;
    }
    return nullptr;
}

static bool
n00bb_nil_type(th_ctx *types, th_type *type)
{
    return type && th_hash(types, th_resolve(type))
        == th_hash(types, th_name(types, "nil"));
}

static bool
register_variant_types(parse_grammar_t *grammar,
                       core_translation_unit_t *unit,
                       ir_module_t *destination)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !destination || !state->annotations
        || !state->annotations->symtab) return false;
    parse_symtab_t *symtab = state->annotations->symtab;
    bool ok = true;
    for (parse_scope_t *scope = symtab->all_scopes; ok && scope;
         scope = scope->all_next) {
        if (!core_string_eq(scope->scope_tag,
                            CORE_STRING_STATIC("variant"))) continue;
        core_list_t(th_member) members = core_list_new(th_member);
        core_list_t(th_arg) args = core_list_new(th_arg);
        for (parse_sym_entry_t *arm = scope->first_in_scope; arm;
             arm = arm->next_in_scope) {
            if (!arm->decl_node
                || !nt_is(grammar, arm->decl_node, "variant-decl-one"))
                continue;
            th_type *payload = declared_type_from_node(grammar, unit,
                                                        arm->decl_node);
            if (!payload) { ok = false; break; }
            core_list_push(members, ((th_member){
                .name = arm->name.data,
                .type = payload,
            }));
            core_list_push(args, ((th_arg){
                .kind = TH_ARG_TYPE,
                .type = payload,
            }));
        }
        core_buffer_t qualified = {0};
        append_module_type_name(unit, scope->name, &qualified);
        th_type *variant = ok
            ? th_variant(state->types, qualified.data, args.data, args.len)
            : nullptr;
        th_type *definition = ok
            ? th_record(state->types, members.data, members.len) : nullptr;
        ok = variant && definition
            && ir_type_define(destination, variant, definition);
        core_buffer_release(&qualified);
        core_list_free(args);
        core_list_free(members);
    }
    return ok;
}

static bool
register_record_types(parse_grammar_t *grammar,
                      core_translation_unit_t *unit,
                      ir_module_t *destination, ir_ctx_t *ir_context)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !destination || !state->types || !state->annotations
        || !state->annotations->symtab) return false;
    for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
         scope; scope = scope->all_next) {
        if (!core_string_eq(scope->scope_tag, CORE_STRING_STATIC("type"))
            || !scope->node || !nt_is(grammar, scope->node, "type-decl"))
            continue;
        core_list_t(th_member) members = core_list_new(th_member);
        bool valid = true;
        for (parse_sym_entry_t *field = scope->first_in_scope; field;
             field = field->next_in_scope) {
            if (!field->is_field) continue;
            th_type *field_type = declared_type_from_node(grammar, unit,
                                                           field->decl_node);
            if (!field_type) { valid = false; break; }
            core_list_push(members, ((th_member){
                .name = field->name.data,
                .type = field_type,
            }));
        }
        core_buffer_t qualified = {0};
        append_module_type_name(unit, scope->name, &qualified);
        th_type *named = valid
            ? th_name(state->types, qualified.data) : nullptr;
        th_type *record = valid
            ? th_record(state->types, members.data, members.len) : nullptr;
        valid = named && record
            && ir_type_define(destination, named, record);
        for (parse_sym_entry_t *field = scope->first_in_scope;
             valid && field; field = field->next_in_scope) {
            if (!field->is_field
                || !(field->has_bit_offset || field->has_bit_width)) continue;
            valid = ir_layout_pin_member(destination, named,
                field->name.data, field->bit_offset, field->bit_width);
        }
        core_list_free(members);
        core_buffer_release(&qualified);
        if (!valid) {
            ir_diags_t *diagnostics = ir_diags_new_attached(
                ir_context, nullptr,
                core_translation_unit_diagnostics(unit), unit);
            ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
                IR_DIAG_TYPE_MISMATCH, ir_module_span(state->ir_module),
                "this record declaration");
            ir_diag_message(diagnostic, diagnostics,
                "could not register the record representation in Lantern IR");
            return false;
        }
    }
    return true;
}

static bool
global_integer_fits(ir_module_t *module, th_type *type, ir_lit_t value)
{
    ir_machine_t machine = ir_machine_of(module, type);
    uint32_t width = machine.width;
    if (machine.class == IR_CLASS_BOOL)
        return !value.neg && value.mag <= 1;
    if (!width || width > 128) return false;
    if (machine.class == IR_CLASS_UINT)
        return !value.neg
            && (width == 128 || value.mag < ((ir_uint_t)1 << width));
    if (machine.class != IR_CLASS_SINT) return false;
    ir_uint_t sign = (ir_uint_t)1 << (width - 1);
    return value.neg ? value.mag <= sign : value.mag < sign;
}

static void
count_simple_literals(parse_grammar_t *grammar, parse_tree_t *node,
                      size_t *count)
{
    if (!node || core_tree_is_leaf(node)) return;
    if (nt_is(grammar, node, "simple-lit")) (*count)++;
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        count_simple_literals(grammar, core_tree_child(node, i), count);
}

static bool
static_literal_tokens_only(parse_grammar_t *grammar, parse_tree_t *literal,
                           parse_tree_t *node, bool inside_literal)
{
    if (!node) return true;
    inside_literal = inside_literal || node == literal;
    if (core_tree_is_leaf(node)) {
        if (inside_literal) return true;
        core_string_t text = parse_tree_leaf_text(node);
        return core_string_eq(text, CORE_STRING_STATIC("+"))
            || core_string_eq(text, CORE_STRING_STATIC("-"));
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!static_literal_tokens_only(grammar, literal,
                                        core_tree_child(node, i),
                                        inside_literal))
            return false;
    (void)grammar;
    return true;
}

static bool
initialize_static_global(parse_grammar_t *grammar,
                         core_translation_unit_t *unit,
                         parse_sym_entry_t *symbol, ir_global_t *global,
                         ir_diags_t *diagnostics, int32_t file_id)
{
    parse_tree_t *declaration = symbol->decl_node;
    parse_tree_t *initializer = nt_is(grammar, declaration, "sym-decl")
        ? child_nt(grammar, declaration, "expression") : nullptr;
    if (!initializer) {
        if (symbol->mutability == PARSE_SYM_CONST || symbol->is_thread_local) {
            ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
                IR_DIAG_VERIFY_TYPED,
                lower_span_for_tree(&(ir_lower_context_t){.file_id = file_id},
                                    declaration),
                "this module global");
            ir_diag_message(diagnostic, diagnostics,
                "const and thread-local globals require a compile-time initializer");
            return false;
        }
        return true;
    }

    size_t literal_count = 0;
    count_simple_literals(grammar, initializer, &literal_count);
    parse_tree_t *literal = literal_count == 1
        ? descendant_nt(grammar, initializer, "simple-lit") : nullptr;
    if (literal && !static_literal_tokens_only(grammar, literal, initializer,
                                               false))
        literal = nullptr;
    parse_token_info_t *token = literal
        ? parse_tree_find_first_terminal(literal) : nullptr;
    core_string_t spelling = token && core_option_is_set(token->value)
        ? core_option_get(token->value) : (core_string_t){0};
    ir_span_t span = lower_span_for_tree(
        &(ir_lower_context_t){.file_id = file_id}, initializer);
    bool initialized = false;

    core_string_t token_name = token
        ? parse_terminal_name(grammar, token->tid) : (core_string_t){0};
    if (token && (core_string_eq(token_name,
                                 CORE_STRING_STATIC("INT_LIT"))
                  || core_string_eq(token_name,
                                    CORE_STRING_STATIC("HEX_LIT")))) {
        ir_lit_t value = {0};
        initialized = n00bb_switch_case_integer(grammar, initializer, &value)
            && global_integer_fits(global->type ? tu_state(unit)->ir_module
                                                : nullptr,
                                   global->type, value);
        if (initialized) global->init = value;
    }
    else if (token && core_string_eq(token_name,
                                     CORE_STRING_STATIC("FLOAT_LIT"))) {
        char *end = nullptr;
        double value = strtod(spelling.data, &end);
        initialized = end && end != spelling.data && !*end
            && ir_machine_of(tu_state(unit)->ir_module,
                             global->type).class == IR_CLASS_FLOAT;
        if (initialized) {
            global->init_f = value;
            global->init_is_float = true;
        }
    }
    else if (core_string_eq(spelling, CORE_STRING_STATIC("true"))
             || core_string_eq(spelling, CORE_STRING_STATIC("false"))) {
        ir_machine_t machine = ir_machine_of(tu_state(unit)->ir_module,
                                             global->type);
        initialized = machine.class == IR_CLASS_BOOL;
        if (initialized)
            global->init.mag = core_string_eq(
                spelling, CORE_STRING_STATIC("true"));
    }

    if (!initialized) {
        if (symbol->mutability != PARSE_SYM_CONST
            && !symbol->is_thread_local)
            return true;
        ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
            IR_DIAG_TYPE_LITERAL, span, "this global initializer");
        ir_diag_message(diagnostic, diagnostics,
            "module globals currently require a supported scalar static literal initializer");
        return false;
    }
    global->has_init = true;
    return true;
}

static bool
register_module_globals(parse_grammar_t *grammar,
                        core_translation_unit_t *source,
                        ir_module_t *destination, ir_ctx_t *ir_context)
{
    n00bb_tu_state_t *source_state = tu_state(source);
    if (!source_state || !source_state->annotations || !destination)
        return false;
    ir_diags_t *diagnostics = ir_diags_new_attached(
        ir_context, nullptr, core_translation_unit_diagnostics(source), source);
    if (!diagnostics) return false;
    int32_t source_file = source_state->ir_module
        ? ir_file_count(source_state->ir_module) - 1 : -1;
    core_list_t(parse_scope_ptr_t) scopes = module_scopes(source_state);
    bool ok = true;
    for (size_t s = 0; ok && s < scopes.len; s++) {
        for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
             ok && symbol; symbol = symbol->next_in_scope) {
            if (symbol->kind != PARSE_SYM_VARIABLE || !symbol->decl_node
                || !symbol->type) continue;
            core_buffer_t name = {0};
            append_module_symbol_name(source, symbol, &name);
            ir_global_t *global = ir_global_add(destination, name.data,
                                                symbol->type);
            bool local_definition = destination == source_state->ir_module;
            if (global) {
                global->is_extern = !local_definition;
                global->is_static = local_definition
                    && !core_string_eq(symbol->visibility,
                                       CORE_STRING_STATIC("public"))
                    && !core_string_eq(symbol->visibility,
                                       CORE_STRING_STATIC("package"));
                global->mutability = symbol->mutability == PARSE_SYM_CONST
                    ? IR_MUT_CONST
                    : symbol->mutability == PARSE_SYM_IMMUTABLE
                        ? IR_MUT_IMMUTABLE : IR_MUT_MUTABLE;
                if (symbol->is_thread_local)
                    ir_annot_set(ir_context, &global->annots,
                        (ir_annot_t){.key = "n00b.thread_local",
                                     .kind = IR_ANNOT_BOOL, .b = true});
                if (local_definition) {
                    global->span = lower_span_for_tree(
                        &(ir_lower_context_t){.file_id = source_file},
                        symbol->decl_node);
                    ok = initialize_static_global(grammar, source, symbol,
                        global, diagnostics, source_file);
                }
            }
            else {
                ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
                    IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(
                        &(ir_lower_context_t){.file_id = source_file},
                        symbol->decl_node),
                    "this module global");
                ir_diag_message(diagnostic, diagnostics,
                    "could not create its IR global declaration");
                ok = false;
            }
            core_buffer_release(&name);
        }
    }
    core_list_free(scopes);
    return ok && !ir_diags_failed(diagnostics);
}

static th_type *
function_abi_type(parse_grammar_t *grammar, core_translation_unit_t *unit,
                  parse_sym_entry_t *symbol)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !symbol || !symbol->type) return nullptr;
    th_type *source_type = is_scheme(symbol->type)
        ? th_binder_body(symbol->type) : symbol->type;
    if (!source_type || th_kind_of(source_type) != TH_FUNCTION)
        return nullptr;
    parse_tree_t *signature = child_nt(grammar, symbol->decl_node,
                                       "func-type-signature");
    core_list_t(parse_sym_entry_ptr_t) formals =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *rest_symbol = nullptr;
    collect_ir_parameters(grammar, signature, &formals, &rest_symbol);
    if (formals.len != th_func_nparams(source_type)) {
        core_list_free(formals);
        return nullptr;
    }

    core_list_t(th_param) parameters = core_list_new(th_param);
    bool ok = true;
    for (size_t i = 0; ok && i < formals.len; i++) {
        const th_param *source = th_func_param(source_type, i);
        parse_sym_entry_t *formal = formals.data[i];
        th_type *type = source ? source->type : nullptr;
        if (!source || !formal) {
            ok = false;
            break;
        }
        if (formal->is_optional)
            type = n00bb_default_abi_type(state->types, type);
        if (formal->has_mode && formal->mode != TH_IN)
            type = n00bb_indirect_type(state->types, type);
        ok = type != nullptr;
        if (ok) core_list_push(parameters,
            ((th_param){.mode = TH_IN, .type = type}));
    }
    const th_param *source_rest = th_func_rest(source_type);
    if (ok && ((rest_symbol != nullptr) != (source_rest != nullptr)))
        ok = false;
    if (ok && source_rest) {
        th_type *array_type = th_array(state->types, source_rest->type);
        ok = array_type != nullptr;
        if (ok) core_list_push(parameters,
            ((th_param){.mode = TH_IN, .type = array_type}));
    }
    core_list_free(formals);
    if (!ok) {
        core_list_free(parameters);
        return nullptr;
    }

    core_list_t(th_kwparam) keywords = core_list_new(th_kwparam);
    for (size_t i = 0; i < th_func_nkw(source_type); i++) {
        const th_kwparam *keyword = th_func_kw(source_type, i);
        if (!keyword) {
            core_list_free(parameters);
            core_list_free(keywords);
            return nullptr;
        }
        core_list_push(keywords, *keyword);
    }
    th_type *abi = th_func_with_error(state->types,
        th_func_ret(source_type), th_func_error(source_type),
        parameters.data, parameters.len, nullptr,
        keywords.data, keywords.len);
    core_list_free(parameters);
    core_list_free(keywords);
    return abi;
}

static bool
initialize_ir_modules(ir_ctx_t *ir_context, parse_grammar_t *grammar,
                      core_list_t(core_translation_unit_ptr_t) *units)
{
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->annotation_root) return false;
        state->ir_module = ir_module_new(ir_context,
            unit->module.identifier.data, &N00BB_PROFILE, "host");
        if (!state->ir_module) return false;
        core_string_t path = core_source_file_path(&unit->source);
        int32_t file_id = ir_file_add(state->ir_module, path.data);
        ir_module_set_span(state->ir_module,
            ir_span_make(file_id, 0,
                (uint32_t)core_source_file_size(&unit->source), 1, 1, 1, 1));
        ir_module_set_tree(state->ir_module, grammar, state->annotation_root);
    }

    for (size_t i = 0; i < units->len; i++)
        for (size_t j = 0; j < units->len; j++)
            if (!register_variant_types(grammar, units->data[j],
                                         tu_state(units->data[i])->ir_module)
                || !register_record_types(grammar, units->data[j],
                    tu_state(units->data[i])->ir_module, ir_context))
                return false;

    for (size_t i = 0; i < units->len; i++)
        for (size_t j = 0; j < units->len; j++)
            if (!register_module_globals(grammar, units->data[j],
                    tu_state(units->data[i])->ir_module, ir_context))
                return false;

    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
        for (size_t s = 0; s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_FUNCTION || !symbol->type)
                    continue;
                bool definition = function_symbol_at(grammar,
                                                       symbol->decl_node);
                ir_linkage_t linkage = !definition ? IR_LINK_EXTERN
                    : symbol_visibility(symbol) == TH_ENV_PRIVATE
                        ? IR_LINK_INTERNAL : IR_LINK_EXTERNAL;
                th_type *abi_type = function_abi_type(grammar, unit, symbol);
                if (!abi_type || !ir_func_add(state->ir_module,
                                 symbol->name.data, abi_type, linkage)) {
                    core_list_free(scopes);
                    return false;
                }
            }
        }
        core_list_free(scopes);
    }
    for (size_t i = 0; i < units->len; i++)
        if (!add_context_hook_functions(grammar, units->data[i]))
            return false;
    return true;
}

static bool
validate_module_declarations(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             ir_ctx_t *ir_context)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->ir_module) return false;
    ir_diags_t *diagnostics = ir_diags_new_attached(
        ir_context, nullptr, core_translation_unit_diagnostics(unit), unit);
    if (!diagnostics) return false;
    int32_t file_id = ir_file_count(state->ir_module) - 1;
    ir_lower_context_t span_context = {.file_id = file_id};
    bool valid = true;
    core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
    for (size_t s = 0; valid && s < scopes.len; s++) {
        for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
             symbol; symbol = symbol->next_in_scope) {
            if (symbol->kind == PARSE_SYM_FUNCTION
                || symbol->kind == PARSE_SYM_MODULE
                || context_symbol(grammar, symbol)
                || (symbol->decl_node
                    && nt_is(grammar, symbol->decl_node, "trait-decl"))
                || (symbol->decl_node
                    && (nt_is(grammar, symbol->decl_node, "type-decl")
                        || nt_is(grammar, symbol->decl_node, "variant-decl")))
                || !symbol->decl_node)
                continue;
            if (symbol->kind == PARSE_SYM_VARIABLE
                && symbol->type
                && nt_is(grammar, symbol->decl_node, "sym-decl")) {
                core_buffer_t name = {0};
                append_module_symbol_name(unit, symbol, &name);
                bool registered = ir_global_find(state->ir_module,
                                                  name.data) != nullptr;
                core_buffer_release(&name);
                if (registered) continue;
            }
            ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
                IR_DIAG_VERIFY_PARSED,
                lower_span_for_tree(&span_context, symbol->decl_node),
                "this module-level declaration");
            ir_diag_message(diagnostic, diagnostics,
                "module-level variable declarations are not lowered yet");
            valid = false;
            break;
        }
    }
    core_list_free(scopes);
    return valid && !ir_diags_failed(diagnostics);
}

static bool
n00bb_default_references(parse_grammar_t *grammar, parse_tree_t *node,
                         parse_sym_entry_t *symbol)
{
    if (!node) return false;
    if (!core_tree_is_leaf(node)
        && core_tree_node_value(node).sym == symbol) return true;
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (n00bb_default_references(grammar, core_tree_child(node, i),
                                     symbol)) return true;
    (void)grammar;
    return false;
}

static ir_value_t *
n00bb_emit_default_field(ir_lower_context_t *context, ir_value_t *record,
                        const char *field, th_type *type, ir_span_t span)
{
    char name[48];
    snprintf(name, sizeof(name), "__n00bb_default_field_%llu",
             (unsigned long long)context->next_value_id++);
    ir_instr_t *extract = ir_instr_add(context->block, IR_OP_EXTRACT);
    if (!extract || !ir_instr_set_dest(extract, name, type)
        || !ir_instr_operand_value(extract, record)
        || !ir_instr_operand_name(extract, field)) return nullptr;
    extract->span = span;
    return extract->dest;
}

static bool
n00bb_emit_default_param(ir_lowerer_t *lowerer,
                        ir_lower_context_t *context,
                        n00bb_default_param_t *parameter)
{
    ir_span_t span = lower_span_for_tree(context, parameter->symbol->decl_node);
    th_type *bool_type = ir_type_parse(context->module, "bool");
    th_type *value_type = parameter->local ? parameter->local->type : nullptr;
    ir_value_t *present = bool_type
        ? n00bb_emit_default_field(context, parameter->argument,
                                    "present", bool_type, span) : nullptr;
    uint64_t id = context->next_value_id++;
    char provided_name[48], missing_name[48], join_name[48];
    snprintf(provided_name, sizeof(provided_name),
             "__n00bb_default_provided_%llu", (unsigned long long)id);
    snprintf(missing_name, sizeof(missing_name),
             "__n00bb_default_missing_%llu", (unsigned long long)id);
    snprintf(join_name, sizeof(join_name), "__n00bb_default_join_%llu",
             (unsigned long long)id);
    ir_block_t *provided = ir_block_add(context->function, provided_name);
    ir_block_t *missing = ir_block_add(context->function, missing_name);
    ir_block_t *join = ir_block_add(context->function, join_name);
    ir_instr_t *branch = provided && missing && join
        ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
    if (!present || !branch || !ir_instr_operand_value(branch, present)
        || !ir_instr_operand_block(branch, provided)
        || !ir_instr_operand_block(branch, missing)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not branch on default argument presence");
        return false;
    }
    branch->span = span;

    context->block = provided;
    context->reachable = true;
    ir_value_t *given = n00bb_emit_default_field(context, parameter->argument,
        "value", value_type, span);
    ir_instr_t *given_copy = given
        ? ir_instr_add(context->block, IR_OP_COPY) : nullptr;
    if (!given_copy || !ir_instr_set_dest_local(given_copy,
                                                 parameter->local)
        || !ir_instr_operand_value(given_copy, given)
        || !n00bb_emit_jump(context, join, span)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not bind a supplied default argument");
        return false;
    }
    given_copy->span = span;

    context->block = missing;
    context->reachable = true;
    ir_lower_result_t value = ir_lower_tree(lowerer, context,
        ((n00bb_lower_context_t *)context->user)->grammar,
        parameter->expression);
    if (value.status == IR_LOWER_FAILED) return false;
    if (value.status == IR_LOWER_VALUE && value.value) {
        ir_instr_t *copy = ir_instr_add(context->block, IR_OP_COPY);
        if (!copy || !ir_instr_set_dest_local(copy, parameter->local)
            || !ir_instr_operand_value(copy, value.value)) {
            lower_error(context, IR_DIAG_INTERNAL, span,
                        "could not store a default parameter value");
            return false;
        }
        copy->span = span;
        if (!n00bb_emit_jump(context, join, span)) return false;
    }
    else if (value.status != IR_LOWER_TERMINATED) {
        lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                    "a default parameter expression did not produce a value");
        return false;
    }
    context->block = join;
    context->reachable = true;
    parameter->initialized = true;
    return true;
}

static bool
n00bb_initialize_default_params(ir_lowerer_t *lowerer,
                                ir_lower_context_t *context,
                                core_list_t(n00bb_default_param_t) *params)
{
    size_t remaining = params->len;
    while (remaining) {
        bool progress = false;
        for (size_t i = 0; i < params->len; i++) {
            n00bb_default_param_t *candidate = &params->data[i];
            if (candidate->initialized) continue;
            bool ready = true;
            for (size_t dependency = 0; dependency < params->len;
                 dependency++) {
                n00bb_default_param_t *required = &params->data[dependency];
                if (!required->initialized
                    && n00bb_default_references(context->user
                            ? ((n00bb_lower_context_t *)context->user)->grammar
                            : nullptr,
                        candidate->expression, required->symbol)) {
                    ready = false;
                    break;
                }
            }
            if (!ready) continue;
            if (!candidate->expression
                || !n00bb_emit_default_param(lowerer, context, candidate))
                return false;
            remaining--;
            progress = true;
        }
        if (!progress) {
            n00bb_default_param_t *cyclic = nullptr;
            for (size_t i = 0; i < params->len; i++)
                if (!params->data[i].initialized) {
                    cyclic = &params->data[i];
                    break;
                }
            lower_error(context, IR_DIAG_TYPE_ARGS,
                lower_span_for_tree(context, cyclic
                    ? cyclic->symbol->decl_node : nullptr),
                "default parameter dependencies are cyclic");
            return false;
        }
    }
    return true;
}

static bool
lower_ir_body(ir_lowerer_t *lowerer, parse_grammar_t *grammar,
              core_translation_unit_t *unit, parse_sym_entry_t *symbol,
              parse_tree_t *body, ir_func_t *function,
              core_list_t(parse_sym_entry_ptr_t) *parameters,
              core_list_t(n00bb_tree_ptr_t) *roots,
              parse_sym_entry_t *rest_parameter,
              bool defaulted_params_in_abi,
              ir_ctx_t *ir_context,
              core_list_t(core_translation_unit_ptr_t) *units)
{
    n00bb_tu_state_t *state = tu_state(unit);
    th_type *function_type = symbol && symbol->type
        ? (is_scheme(symbol->type) ? th_binder_body(symbol->type)
                                   : symbol->type)
        : nullptr;
    if (!state || !body || !function || !function_type) return false;

    ir_diags_t *diagnostics = ir_diags_new_attached(
        ir_context, nullptr, core_translation_unit_diagnostics(unit), unit);
    if (!diagnostics) return false;
    if (parameters->len != th_func_nparams(function_type)) {
        ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
            IR_DIAG_VERIFY_TYPED, IR_SPAN_UNKNOWN, "these parameters");
        ir_diag_message(diagnostic, diagnostics,
                        "parameter declarations do not match the callable type");
        return false;
    }

    ir_block_t *entry = ir_block_add(function, "entry");
    if (!entry) {
        return false;
    }
    int32_t file_id = ir_file_count(state->ir_module) - 1;
    n00bb_lower_context_t lower_data = {
        .grammar = grammar,
        .lowerer = lowerer,
        .units = units,
        .unit = unit,
        .function_body = body,
        .function_symbol = symbol,
        .loops = core_list_new(n00bb_loop_frame_t),
        .value_blocks = core_list_new(n00bb_value_block_frame_t),
        .tries = core_list_new(n00bb_try_frame_t),
        .withs = core_list_new(n00bb_with_frame_t),
        .parameters_to_copy_out = core_list_new(n00bb_copyout_t),
        .pending_call_copyouts = core_list_new(n00bb_pending_copyouts_t),
        .default_params = core_list_new(n00bb_default_param_t),
    };
    ir_lower_context_t context = {
        .module = state->ir_module,
        .function = function,
        .block = entry,
        .diagnostics = diagnostics,
        .file_id = file_id,
        .user = &lower_data,
        .reachable = true,
    };
    for (size_t i = 0; i < parameters->len; i++) {
        parse_sym_entry_t *parameter = parameters->data[i];
        const th_param *type_param = th_func_param(function_type, i);
        th_mode mode = parameter && parameter->has_mode
            ? parameter->mode : TH_IN;
        if (!parameter || !type_param || !parameter->name.data
            || (parameter->is_optional && mode != TH_IN)
            || (mode != TH_IN && mode != TH_OUT && mode != TH_INOUT)) {
            ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
                IR_DIAG_VERIFY_TYPED, IR_SPAN_UNKNOWN, "this parameter");
            ir_diag_message(diagnostic, diagnostics,
                "only in, out, and modify parameters are lowered; defaults "
                "are allowed only for in parameters");
            core_list_free(lower_data.default_params);
            return false;
        }
        th_type *parameter_type = parameter->type
            ? th_unify_in_place(state->types, parameter->type,
                                type_param->type)
            : type_param->type;
        if (!parameter_type) {
            lower_error(&context, IR_DIAG_TYPE_MISMATCH, IR_SPAN_UNKNOWN,
                "parameter symbol type does not match the callable signature");
            return false;
        }
        parameter->type = parameter_type;
        if (parameter->decl_node)
            core_tree_node_value(parameter->decl_node).type = parameter_type;
        if (mode == TH_IN) {
            if (defaulted_params_in_abi && parameter->is_optional) {
                const th_param *abi_param = th_func_param(function->type, i);
                th_type *expected_abi = n00bb_default_abi_type(
                    state->types, parameter_type);
                char argument_name[48], local_name[48];
                snprintf(argument_name, sizeof(argument_name),
                         "__n00bb_default_param_%llu",
                         (unsigned long long)context.next_value_id++);
                snprintf(local_name, sizeof(local_name),
                         "__n00bb_default_local_%llu",
                         (unsigned long long)context.next_value_id++);
                ir_value_t *argument = abi_param && expected_abi
                    && ir_type_eq(state->ir_module, abi_param->type,
                                  expected_abi)
                    ? ir_param_add(function, argument_name,
                                   abi_param->type) : nullptr;
                ir_local_t *local = ir_local_add(function, local_name,
                                                 parameter_type);
                if (!argument || !local
                    || !ir_lower_bind_local(&context, parameter, local)) {
                    lower_error(&context, IR_DIAG_VERIFY_TYPED,
                        lower_span_for_tree(&context, parameter->decl_node),
                        "defaulted parameter has an invalid lowered ABI type");
                    core_list_free(lower_data.default_params);
                    return false;
                }
                core_list_push(lower_data.default_params,
                    ((n00bb_default_param_t){
                        .symbol = parameter,
                        .expression = child_nt(grammar, parameter->decl_node,
                                               "expression"),
                        .local = local,
                        .argument = argument,
                    }));
                continue;
            }
            ir_value_t *value = ir_param_add(function, parameter->name.data,
                                             parameter_type);
            if (!value || !ir_lower_bind_value(&context, parameter, value)) {
                core_list_free(lower_data.default_params);
                return false;
            }
            continue;
        }

        bool mode_matches = parameter->has_mode
            && (mode == TH_OUT || mode == TH_INOUT);
        th_type *address_type = n00bb_indirect_type(state->types,
                                                    parameter_type);
        ir_value_t *address = mode_matches && address_type
            ? ir_param_add(function, parameter->name.data, address_type)
            : nullptr;
        char local_name[48];
        snprintf(local_name, sizeof(local_name), "__n00bb_param_%llu",
                 (unsigned long long)context.next_value_id++);
        ir_local_t *local = mode_matches
            ? ir_local_add(function, local_name, parameter_type) : nullptr;
        if (!mode_matches || !address || !local
            || !ir_lower_bind_local(&context, parameter, local)) {
            lower_error(&context, IR_DIAG_VERIFY_TYPED, IR_SPAN_UNKNOWN,
                "out and modify parameters need matching passing metadata and a typed local copy");
            core_list_free(lower_data.default_params);
            return false;
        }
        if (mode == TH_INOUT) {
            ir_value_t *initial = n00bb_emit_indirect_read(&context, address,
                parameter_type, lower_span_for_tree(&context,
                                                      parameter->decl_node));
            ir_instr_t *copy = initial
                ? ir_instr_add(context.block, IR_OP_COPY) : nullptr;
            if (!copy || !ir_instr_set_dest_local(copy, local)
                || !ir_instr_operand_value(copy, initial)) {
                lower_error(&context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(&context, parameter->decl_node),
                    "could not copy a modify parameter into its local storage");
                core_list_free(lower_data.default_params);
                return false;
            }
            copy->span = lower_span_for_tree(&context,
                                              parameter->decl_node);
        }
        core_list_push(lower_data.parameters_to_copy_out,
            ((n00bb_copyout_t){parameter, local, address}));
    }

    if (rest_parameter) {
        ir_value_t *rest = ir_param_add(function, rest_parameter->name.data,
                                        rest_parameter->type);
        if (!rest || th_kind_of(th_resolve(rest_parameter->type))
                         != TH_SEQUENCE
            || !ir_lower_bind_value(&context, rest_parameter, rest)) {
            lower_error(&context, IR_DIAG_VERIFY_TYPED,
                lower_span_for_tree(&context, rest_parameter->decl_node),
                "variadic parameter does not have its collected array type");
            core_list_free(lower_data.default_params);
            return false;
        }
    }

    core_list_t(parse_scope_ptr_t) function_scopes = module_scopes(state);
    for (size_t s = 0; s < function_scopes.len; s++) {
        for (parse_sym_entry_t *candidate = function_scopes.data[s]->first_in_scope;
             candidate; candidate = candidate->next_in_scope) {
            if (candidate->kind != PARSE_SYM_FUNCTION || !candidate->type)
                continue;
            ir_func_t *target = ir_func_find(state->ir_module,
                                             candidate->name.data);
            ir_value_t *address = target
                ? ir_func_address(state->ir_module, target) : nullptr;
            if (!address || !ir_lower_bind_value(&context, candidate, address)) {
                core_list_free(function_scopes);
                core_list_free(lower_data.default_params);
                return false;
            }
        }
    }
    core_list_free(function_scopes);

    for (size_t u = 0; u < units->len; u++) {
        n00bb_tu_state_t *global_state = tu_state(units->data[u]);
        core_list_t(parse_scope_ptr_t) global_scopes =
            module_scopes(global_state);
        for (size_t s = 0; s < global_scopes.len; s++) {
            for (parse_sym_entry_t *global_symbol =
                    global_scopes.data[s]->first_in_scope;
                 global_symbol; global_symbol = global_symbol->next_in_scope) {
                if (global_symbol->kind != PARSE_SYM_VARIABLE
                    || !global_symbol->type || !global_symbol->decl_node
                    || !nt_is(grammar, global_symbol->decl_node, "sym-decl"))
                    continue;
                core_buffer_t name = {0};
                append_module_symbol_name(units->data[u], global_symbol,
                                          &name);
                ir_global_t *global = ir_global_find(state->ir_module,
                                                       name.data);
                ir_value_t *value = global
                    ? ir_global_address(state->ir_module, global) : nullptr;
                core_buffer_release(&name);
                if (!value || !ir_lower_bind_value(&context, global_symbol,
                                                    value)) {
                    core_list_free(global_scopes);
                    core_list_free(lower_data.default_params);
                    return false;
                }
            }
        }
        core_list_free(global_scopes);
    }

    if (!n00bb_initialize_default_params(lowerer, &context,
                                          &lower_data.default_params)) {
        core_list_free(lower_data.default_params);
        return false;
    }

    lower_data.branches = core_list_new(n00bb_branch_frame_t);
    lower_data.switches = core_list_new(n00bb_switch_frame_t);
    lower_data.typecases = core_list_new(n00bb_typecase_frame_t);
    ir_lower_result_t lowered = {IR_LOWER_NO_VALUE, nullptr};
    if (roots) {
        for (size_t i = 0; i < roots->len; i++) {
            if (!context.reachable) {
                lower_error(&context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(&context, roots->data[i]),
                    "module initializer control flow cannot terminate before later top-level statements");
                lowered.status = IR_LOWER_FAILED;
                break;
            }
            lower_data.function_body = nt_is(grammar, roots->data[i], "body")
                ? roots->data[i] : body;
            lowered = ir_lower_tree(lowerer, &context, grammar,
                                    roots->data[i]);
            if (lowered.status == IR_LOWER_FAILED) break;
        }
    }
    else lowered = ir_lower_tree(lowerer, &context, grammar, body);
    for (size_t i = 0; i < lower_data.branches.len; i++)
        core_list_free(lower_data.branches.data[i].arms);
    core_list_free(lower_data.branches);
    core_list_free(lower_data.loops);
    for (size_t i = 0; i < lower_data.switches.len; i++) {
        for (size_t c = 0; c < lower_data.switches.data[i].cases.len; c++)
            core_list_free(lower_data.switches.data[i].cases.data[c].patterns);
        core_list_free(lower_data.switches.data[i].cases);
    }
    core_list_free(lower_data.switches);
    for (size_t i = 0; i < lower_data.typecases.len; i++)
        core_list_free(lower_data.typecases.data[i].arms);
    core_list_free(lower_data.typecases);
    core_list_free(lower_data.value_blocks);
    core_list_free(lower_data.tries);
    for (size_t w = 0; w < lower_data.withs.len; w++) {
        for (size_t i = 0; i < lower_data.withs.data[w].items.len; i++)
            core_list_free(lower_data.withs.data[w].items.data[i].captures);
        core_list_free(lower_data.withs.data[w].items);
    }
    core_list_free(lower_data.withs);
    for (size_t i = 0; i < lower_data.pending_call_copyouts.len; i++)
        core_list_free(lower_data.pending_call_copyouts.data[i].items);
    core_list_free(lower_data.pending_call_copyouts);
    core_list_free(lower_data.default_params);
    if (lowered.status == IR_LOWER_FAILED || ir_diags_failed(diagnostics)) {
        return false;
    }
    if (context.reachable) {
        if (!n00bb_nil_type(state->types, th_func_ret(function_type))) {
            ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
                IR_DIAG_VERIFY_TYPED, IR_SPAN_UNKNOWN, "this function body");
            ir_diag_message(diagnostic, diagnostics,
                            "this non-void function can reach its end without returning a value");
            return false;
        }
        if (!n00bb_emit_with_success(&context, body)
            || !n00bb_emit_copyouts(&context,
                &lower_data.parameters_to_copy_out,
                lower_span_for_tree(&context, body))) return false;
        ir_instr_t *ret = ir_instr_add(context.block, IR_OP_RETURN);
        if (!ret) {
            return false;
        }
        ret->span = ir_module_span(state->ir_module);
    }
    core_list_free(lower_data.parameters_to_copy_out);
    return !ir_diags_failed(diagnostics);
}

static parse_tree_t *
find_comptime_construct(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    if (nt_is(grammar, node, "comptime-stmt")
        || nt_is(grammar, node, "comptime-value-expr"))
        return node;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *found = find_comptime_construct(
            grammar, core_tree_child(node, i));
        if (found) return found;
    }
    return nullptr;
}

static bool
lower_function_body(ir_lowerer_t *lowerer, parse_grammar_t *grammar,
                    core_translation_unit_t *unit,
                    parse_sym_entry_t *symbol, ir_ctx_t *ir_context,
                    core_list_t(core_translation_unit_ptr_t) *units)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_tree_t *declaration = symbol ? symbol->decl_node : nullptr;
    parse_tree_t *body = child_nt(grammar, declaration, "body");
    ir_func_t *function = state && state->ir_module && symbol
        ? ir_func_find(state->ir_module, symbol->name.data) : nullptr;
    core_list_t(parse_sym_entry_ptr_t) parameters =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *rest = nullptr;
    parse_tree_t *signature = child_nt(grammar, declaration,
                                       "func-type-signature");
    collect_ir_parameters(grammar, signature, &parameters, &rest);
    parse_tree_t *comptime_node = find_comptime_construct(grammar, body);
    if (comptime_node) {
        n00bb_tu_state_t *state = tu_state(unit);
        ir_diags_t *diagnostics = ir_diags_new_attached(
            ir_context, nullptr, core_translation_unit_diagnostics(unit), unit);
        ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
            IR_DIAG_VERIFY_PARSED,
            lower_span_for_tree(&(ir_lower_context_t){
                .file_id = state && state->ir_module
                    ? ir_file_count(state->ir_module) - 1 : -1,
            }, comptime_node), "this compile-time expression");
        ir_diag_message(diagnostic, diagnostics,
            "compile-time work inside a function must be evaluated before runtime lowering");
        core_list_free(parameters);
        return false;
    }
    bool ok = lower_ir_body(lowerer, grammar, unit, symbol, body,
        function, &parameters, nullptr, rest, true, ir_context, units);
    core_list_free(parameters);
    return ok;
}

static bool
lower_context_hook(ir_lowerer_t *lowerer, parse_grammar_t *grammar,
                   core_translation_unit_t *unit,
                   parse_sym_entry_t *context_symbol,
                   parse_tree_t *hook, ir_ctx_t *ir_context,
                   core_list_t(core_translation_unit_ptr_t) *units)
{
    n00bb_tu_state_t *state = tu_state(unit);
    char name[256];
    if (!state || !state->ir_module
        || !context_hook_ir_name(grammar, context_symbol, hook,
                                 name, sizeof(name))) return false;
    ir_func_t *function = ir_func_find(state->ir_module, name);
    parse_tree_t *body = child_nt(grammar, hook, "body");
    th_type *type = core_tree_node_value(hook).type;
    if (!function || !body || !type) return false;

    core_list_t(parse_sym_entry_ptr_t) parameters =
        core_list_new(parse_sym_entry_ptr_t);
    parse_tree_t *context_params = child_nt(grammar, context_symbol->decl_node,
                                             "param-decl");
    parse_tree_t *hook_params = child_nt(grammar, hook,
                                          "context-hook-params");
    parse_sym_entry_t *rest = nullptr;
    collect_ir_parameters(grammar, context_params, &parameters, &rest);
    collect_ir_parameters(grammar, hook_params, &parameters, &rest);
    parse_sym_entry_t function_symbol = {
        .name = context_symbol->name,
        .kind = PARSE_SYM_FUNCTION,
        .type = type,
        .decl_node = hook,
    };
    bool ok = lower_ir_body(lowerer, grammar, unit,
        &function_symbol, body, function, &parameters, nullptr, rest,
        false,
        ir_context, units);
    core_list_free(parameters);
    return ok;
}

static bool
module_initializer_declaration(parse_grammar_t *grammar, parse_tree_t *node)
{
    static const char *const declarations[] = {
        "func-def", "context-def", "enum-stmt", "import-stmt",
        "alias-stmt", "type-decl", "variant-decl", "trait-decl",
        "when-stmt",
    };
    for (size_t i = 0; i < sizeof(declarations) / sizeof(declarations[0]);
         i++)
        if (nt_is(grammar, node, declarations[i])) return true;
    return false;
}

static void
collect_dynamic_global_initializers(parse_grammar_t *grammar,
                                    core_translation_unit_t *unit,
                                    parse_tree_t *node,
                                    core_list_t(n00bb_tree_ptr_t) *roots)
{
    if (!node || core_tree_is_leaf(node)) return;
    if (nt_is(grammar, node, "sym-decl")) {
        if (!child_nt(grammar, node, "expression")) return;
        parse_sym_entry_t *symbol = core_tree_node_value(node).sym;
        n00bb_tu_state_t *state = tu_state(unit);
        if (!symbol || !state || !state->ir_module) return;
        core_buffer_t name = {0};
        append_module_symbol_name(unit, symbol, &name);
        ir_global_t *global = ir_global_find(state->ir_module, name.data);
        core_buffer_release(&name);
        if (global && !global->has_init) core_list_push(*roots, node);
        return;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        collect_dynamic_global_initializers(grammar, unit,
            core_tree_child(node, i), roots);
}

static void
collect_one_module_initializer_root(parse_grammar_t *grammar,
                                    core_translation_unit_t *unit,
                                    parse_tree_t *top,
                                    core_list_t(n00bb_tree_ptr_t) *runtime,
                                    core_list_t(n00bb_tree_ptr_t) *comptime)
{
    parse_tree_t *statement = nullptr;
    core_list_t(n00bb_tree_ptr_t) pending = core_list_new(n00bb_tree_ptr_t);
    size_t pending_at = 0;
    for (size_t i = core_tree_num_children(top); i > 0; i--)
        core_list_push(pending, core_tree_child(top, i - 1));
    while (!statement && pending_at < pending.len) {
        parse_tree_t *node = pending.data[pending_at++];
        if (core_tree_is_leaf(node)) continue;
        core_string_t name = parse_tree_nt_name(grammar, node);
        if (name.data) {
            statement = node;
            break;
        }
        for (size_t i = core_tree_num_children(node); i > 0; i--)
            core_list_push(pending, core_tree_child(node, i - 1));
    }
    core_list_free(pending);
    if (!statement || module_initializer_declaration(grammar, statement))
        return;
    if (nt_is(grammar, statement, "comptime-stmt")) {
        parse_tree_t *body = child_nt(grammar, statement, "body");
        if (body) core_list_push(*comptime, body);
        return;
    }
    if (nt_is(grammar, statement, "variable-decl")) {
        parse_tree_t *prefix = child_nt(grammar, statement,
                                        "declaration-prefix");
        if (tree_has_leaf_text(prefix, CORE_STRING_STATIC("const"))
            || tree_has_leaf_text(prefix, CORE_STRING_STATIC("thread")))
            return;
        collect_dynamic_global_initializers(grammar, unit, statement,
                                             runtime);
        return;
    }
    core_list_push(*runtime, statement);
}

static void
collect_module_initializer_roots(parse_grammar_t *grammar,
                                 core_translation_unit_t *unit,
                                 parse_tree_t *node,
                                 core_list_t(n00bb_tree_ptr_t) *runtime,
                                 core_list_t(n00bb_tree_ptr_t) *comptime)
{
    if (!node || core_tree_is_leaf(node)) return;
    if (nt_is(grammar, node, "top-level-stmt")) {
        collect_one_module_initializer_root(grammar, unit, node, runtime,
                                            comptime);
        return;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        collect_module_initializer_roots(grammar, unit,
            core_tree_child(node, i), runtime, comptime);
}

static bool
lower_module_initializer(ir_lowerer_t *lowerer,
                         parse_grammar_t *grammar,
                         core_translation_unit_t *unit,
                         core_list_t(core_translation_unit_ptr_t) *units,
                         ir_ctx_t *ir_context, bool comptime,
                         uint64_t order,
                         core_list_t(n00bb_tree_ptr_t) *roots)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->ir_module || !state->annotation_root) return false;
    if (comptime && !roots->len) return true;
    const char *name = comptime ? "__n00bb_comptime_init"
                                : "__n00bb_runtime_init";
    if (ir_func_find(state->ir_module, name)) {
        ir_diags_t *diagnostics = ir_diags_new_attached(
            ir_context, nullptr, core_translation_unit_diagnostics(unit), unit);
        ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
            IR_DIAG_VERIFY_PARSED, ir_module_span(state->ir_module),
            "this generated initializer");
        ir_diag_message(diagnostic, diagnostics,
            "a module declaration uses a reserved initializer name");
        return false;
    }
    th_type *type = ir_type_parse(state->ir_module, "() -> nil");
    ir_func_t *function = type
        ? ir_func_add(state->ir_module, name, type, IR_LINK_INTERNAL) : nullptr;
    if (!function) return false;
    bool annotated = ir_annot_set(ir_context, &function->annots,
        (ir_annot_t){.key = "n00bb.initializer", .kind = IR_ANNOT_STR,
                     .s = comptime ? "comptime" : "runtime"})
        && ir_annot_set(ir_context, &function->annots,
            (ir_annot_t){.key = "n00bb.initializer_order",
                         .kind = IR_ANNOT_INT,
                         .i = {.mag = order}});
    if (!annotated) return false;
    for (size_t i = 0; i < roots->len; i++) {
        parse_tree_t *nested = find_comptime_construct(grammar,
                                                       roots->data[i]);
        if (!nested) continue;
        ir_diags_t *diagnostics = ir_diags_new_attached(
            ir_context, nullptr, core_translation_unit_diagnostics(unit), unit);
        ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
            IR_DIAG_VERIFY_PARSED,
            lower_span_for_tree(&(ir_lower_context_t){
                .file_id = ir_file_count(state->ir_module) - 1,
            }, nested), "this compile-time expression");
        ir_diag_message(diagnostic, diagnostics,
            "nested compile-time work must be evaluated before initializer lowering");
        return false;
    }
    parse_sym_entry_t symbol = {
        .name = core_string_from_cstr(name),
        .kind = PARSE_SYM_FUNCTION,
        .type = type,
    };
    core_list_t(parse_sym_entry_ptr_t) parameters =
        core_list_new(parse_sym_entry_ptr_t);
    bool ok = lower_ir_body(lowerer, grammar, unit, &symbol,
        state->annotation_root, function, &parameters, roots, nullptr,
        false, ir_context, units);
    core_list_free(parameters);
    return ok;
}

static void
collect_runtime_init_order(core_translation_unit_t *unit,
                           core_list_t(core_translation_unit_ptr_t) *seen,
                           core_list_t(core_translation_unit_ptr_t) *order)
{
    if (!unit || tu_list_contains(seen, unit)) return;
    /* Mark on entry for cycles; append on exit for dependency-first order. */
    core_list_push(*seen, unit);
    for (size_t i = 0; i < core_translation_unit_import_count(unit); i++)
        collect_runtime_init_order(core_translation_unit_import(unit, i),
                                   seen, order);
    core_list_push(*order, unit);
}

static void
dump_ir_instr(FILE *out, const ir_instr_t *instr)
{
    fprintf(out, "@%s:%s#%u (%s)", instr->block->owner->name,
            instr->block->name, instr->index,
            ir_op_spelling(instr->op));
}

static void
dump_ir_analysis(ir_module_t *module, bool control_flow, bool full_pdg)
{
    for (size_t f = 0; f < ir_func_count(module); f++) {
        ir_func_t *function = ir_func_at(module, f);
        if (control_flow) {
            ir_pdg_t *pdg = ir_pdg_of(function);
            fprintf(stdout, "control-flow @%s\n", function->name);
            for (uint32_t b = 0; b < function->n_blocks; b++) {
                ir_block_t *block = function->blocks[b];
                ir_instr_t *term = ir_block_terminator(block);
                bool has_successor = false;
                for (uint32_t s = 0; term && s < term->n_operands; s++) {
                    ir_block_t *target = ir_succ_of(&term->operands[s]);
                    if (!target) continue;
                    fprintf(stdout, "  %s -> %s [%s]\n", block->name,
                            target->name,
                            ir_edge_kind_name(ir_edge_kind(pdg, block, s)));
                    has_successor = true;
                }
                if (!has_successor)
                    fprintf(stdout, "  %s -> exit\n", block->name);
            }
        }
        if (full_pdg) {
            ir_pdg_t *pdg = ir_pdg_of(function);
            fprintf(stdout, "pdg @%s (%s)\n", function->name,
                    ir_pdg_is_exact(pdg) ? "exact" : "may");
            for (uint32_t e = 0; e < ir_pdg_edge_count(pdg); e++) {
                const ir_dep_edge_t *edge = ir_pdg_edge_at(pdg, e);
                fputs(edge->kind == IR_DEP_CONTROL ? "  control " : "  data ",
                      stdout);
                dump_ir_instr(stdout, edge->from);
                fputs(edge->certainty == IR_DEP_EXACT ? " => " : " ~> ",
                      stdout);
                dump_ir_instr(stdout, edge->to);
                if (edge->via) fprintf(stdout, " via $%s", edge->via->name);
                fputc('\n', stdout);
            }
        }
    }
}

static bool
lower_requests_to_ir(parse_grammar_t *grammar, core_work_queue_t *queue,
                     core_list_t(locate_request_t) *requests,
                     ir_ctx_t *ir_context, bool dump_ir,
                     bool dump_control_flow, bool dump_pdg)
{
    core_list_t(core_translation_unit_ptr_t) units =
        core_list_new(core_translation_unit_ptr_t);
    for (size_t i = 0; i < requests->len; i++) {
        if (!requests->data[i].root_input) continue;
        core_translation_unit_t *root = core_work_job_result(
            queue, requests->data[i].job);
        collect_import_closure(root, &units);
    }
    bool ok = units.len && initialize_ir_modules(ir_context, grammar, &units);
    for (size_t i = 0; ok && i < units.len; i++)
        ok = validate_module_declarations(grammar, units.data[i],
                                           ir_context);
    ir_lowerer_t *lowerer = ir_lowerer_new(ir_context);
    ok = ok && lowerer
        && ir_lowerer_register(lowerer, "binary", n00bb_lower_binary,
                               nullptr)
        && ir_lowerer_register(lowerer, "sequence_literal",
                               n00bb_lower_sequence_literal, nullptr)
        && ir_lowerer_register(lowerer, "compound_assign",
                               n00bb_lower_compound_assignment, nullptr)
        && ir_lowerer_register(lowerer, "call", n00bb_lower_call, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_BRANCH,
                                     n00bb_lower_branch_annotation, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_LOOP,
                                     n00bb_lower_loop_annotation, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_SWITCH,
                                     n00bb_lower_switch_annotation, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_JUMP,
                                     n00bb_lower_jump_annotation, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_INFER,
                                     n00bb_lower_variant_payload, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_CALL,
                                     n00bb_mark_call_lvalues, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_DECLARES,
                                     n00bb_lower_declare_local, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_ASSIGNS,
                                     n00bb_check_assignment, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_SCOPE_OPEN,
                                     n00bb_lower_branch_body, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_SCOPE_OPEN,
                                     n00bb_lower_typecase_scope, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_SCOPE_OPEN,
                                     n00bb_lower_try_scope, nullptr);
    ok = ok && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_SCOPE_OPEN,
                                         n00bb_lower_with_scope, nullptr);
    ok = ok && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_OPERATOR,
                                         n00bb_lower_operator_annotation,
                                         nullptr);
    for (size_t i = 0; ok && i < units.len; i++) {
        core_translation_unit_t *unit = units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
        for (size_t s = 0; ok && s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 ok && symbol; symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_FUNCTION || !symbol->type
                    || !function_symbol_at(grammar, symbol->decl_node))
                    continue;
                ok = lower_function_body(lowerer, grammar, unit, symbol,
                                         ir_context, &units);
            }
        }
        core_list_free(scopes);
    }
    for (size_t i = 0; ok && i < units.len; i++) {
        core_translation_unit_t *unit = units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
        for (size_t s = 0; ok && s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 ok && symbol; symbol = symbol->next_in_scope) {
                if (!context_symbol(grammar, symbol)) continue;
                static const char *hooks[] = {
                    "context-enter", "context-exit", "context-error",
                    "context-ensure",
                };
                for (size_t h = 0; ok
                    && h < sizeof(hooks) / sizeof(hooks[0]); h++) {
                    parse_tree_t *hook = descendant_nt(grammar,
                        symbol->decl_node, hooks[h]);
                    if (hook)
                        ok = lower_context_hook(lowerer, grammar, unit,
                            symbol, hook, ir_context, &units);
                }
            }
        }
        core_list_free(scopes);
    }
    core_list_t(core_translation_unit_ptr_t) init_seen =
        core_list_new(core_translation_unit_ptr_t);
    core_list_t(core_translation_unit_ptr_t) init_order =
        core_list_new(core_translation_unit_ptr_t);
    for (size_t i = 0; i < requests->len; i++) {
        if (!requests->data[i].root_input) continue;
        core_translation_unit_t *root = core_work_job_result(
            queue, requests->data[i].job);
        collect_runtime_init_order(root, &init_seen, &init_order);
    }
    for (size_t i = 0; ok && i < init_order.len; i++) {
        core_translation_unit_t *unit = init_order.data[i];
        core_list_t(n00bb_tree_ptr_t) runtime_roots =
            core_list_new(n00bb_tree_ptr_t);
        core_list_t(n00bb_tree_ptr_t) comptime_roots =
            core_list_new(n00bb_tree_ptr_t);
        collect_module_initializer_roots(grammar, unit,
            tu_state(unit)->annotation_root,
            &runtime_roots, &comptime_roots);
        ok = lower_module_initializer(lowerer, grammar, unit,
            &units, ir_context, true, i, &comptime_roots)
            && lower_module_initializer(lowerer, grammar, unit,
                &units, ir_context, false, i, &runtime_roots);
        core_list_free(runtime_roots);
        core_list_free(comptime_roots);
    }
    core_list_free(init_seen);
    core_list_free(init_order);

    for (size_t i = 0; ok && i < units.len; i++) {
        core_translation_unit_t *unit = units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        ir_module_establish(state->ir_module, IR_PROP_PARSED | IR_PROP_TYPED);
        ir_diags_t *diagnostics = ir_diags_new_attached(
            ir_context, nullptr, core_translation_unit_diagnostics(unit), unit);
        ir_props_t properties = ir_verify(state->ir_module, diagnostics);
        ir_props_t verified = IR_PROP_PARSED | IR_PROP_TYPED | IR_PROP_VERIFIED;
        ok = !ir_diags_failed(diagnostics)
            && (properties & verified) == verified;
    }
    if (ok && dump_ir) {
        for (size_t i = 0; i < units.len; i++) {
            char *text = ir_module_print(tu_state(units.data[i])->ir_module);
            if (!text) { ok = false; break; }
            fputs(text, stdout);
            core_free(text);
        }
    }
    static const ir_pass_t *const lowering_passes[] = {
        &ir_pass_promote,
        &ir_pass_structure,
        &ir_pass_layout,
    };
    for (size_t i = 0; ok && i < units.len; i++) {
        core_translation_unit_t *unit = units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        ir_diags_t *diagnostics = ir_diags_new_attached(
            ir_context, nullptr, core_translation_unit_diagnostics(unit), unit);
        for (size_t p = 0; ok && p < sizeof(lowering_passes)
                                      / sizeof(lowering_passes[0]); p++) {
            ir_props_t unmet = 0;
            ir_pass_result_t result = ir_pass_run(lowering_passes[p],
                state->ir_module, nullptr, &unmet, diagnostics);
            if (result != IR_PASS_OK) {
                if (result == IR_PASS_UNMET) {
                    ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
                        IR_DIAG_VERIFY_TYPED, ir_module_span(state->ir_module),
                        "this lowered module");
                    ir_diag_message(diagnostic, diagnostics,
                        "Lantern pass prerequisites are not satisfied");
                }
                ok = false;
            }
        }
    }
    for (size_t i = 0; ok && i < units.len; i++) {
        core_translation_unit_t *unit = units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        ir_diags_t *diagnostics = ir_diags_new_attached(
            ir_context, nullptr, core_translation_unit_diagnostics(unit), unit);
        for (size_t f = 0; ok && f < ir_func_count(state->ir_module); f++) {
            ir_func_t *function = ir_func_at(state->ir_module, f);
            if (function->n_blocks && !ir_pdg_of(function)) {
                ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
                    IR_DIAG_INTERNAL, ir_module_span(state->ir_module),
                    "this lowered function");
                ir_diag_message(diagnostic, diagnostics,
                    "Lantern could not construct its dependence graph");
                ok = false;
            }
        }
    }

    core_diagnostics_t *build_diagnostics = core_diagnostics_new();
    for (size_t i = 0; i < units.len; i++) {
        core_diagnostics_merge(build_diagnostics,
            core_translation_unit_diagnostics(units.data[i]));
        if (core_diagnostics_failed(
                core_translation_unit_diagnostics(units.data[i])))
            ok = false;
    }
    if (core_diagnostics_count(build_diagnostics)) {
        char *text = core_diagnostics_render_text(build_diagnostics);
        if (text) { fputs(text, stderr); core_free(text); }
    }
    core_diagnostics_free(build_diagnostics);

    if (ok) {
        for (size_t i = 0; i < units.len; i++) {
            ir_module_t *module = tu_state(units.data[i])->ir_module;
            if (dump_control_flow || dump_pdg)
                dump_ir_analysis(module, dump_control_flow, dump_pdg);
        }
    }
    core_list_free(units);
    return ok;
}

static bool
register_unit_environment(parse_grammar_t *grammar, th_env *environment,
                          core_translation_unit_t *unit,
                          th_type *const *value_environment,
                          size_t value_environment_count)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->types || !state->annotations
        || !state->annotations->symtab) return true;
    core_list_t(parse_scope_ptr_t) module_level_scopes = module_scopes(state);
    const char *module_name = unit->module.identifier.data;
    char *module_text = core_strndup(module_name,
                                     unit->module.identifier.u8_bytes);
    bool ok = true;

    for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
         ok && scope; scope = scope->all_next) {
        if (!nt_is(grammar, scope->node, "trait-decl")) continue;
        core_list_t(th_member) methods = core_list_new(th_member);
        for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
             symbol = symbol->next_in_scope) {
            if (symbol->kind != PARSE_SYM_FUNCTION || !symbol->type) continue;
            th_type *contract = symbol->callable_contract
                ? symbol->callable_contract
                : th_env_signature_contract(state->types, symbol->type,
                    symbol->error_type ? symbol->error_type
                                       : th_name(state->types, "nil"));
            if (!symbol->callable_contract)
                contract = th_generalize_excluding(state->types, contract,
                    value_environment, value_environment_count);
            core_list_push(methods, ((th_member){
                .name = symbol->name.data,
                .type = contract,
            }));
        }
        if (methods.len) {
            th_type *method_record = th_record(state->types, methods.data,
                                               methods.len);
            core_buffer_t qualified = {0};
            core_buffer_printf(&qualified, "%s.%.*s", module_text,
                (int)scope->name.u8_bytes, scope->name.data);
            parse_sym_entry_t *trait_symbol =
                core_tree_node_value(scope->node).sym;
            ok = th_env_register_trait_contracts(environment, qualified.data,
                module_text, "default", symbol_visibility(trait_symbol),
                method_record);
            core_buffer_release(&qualified);
        }
        core_list_free(methods);
    }

    for (size_t s = 0; ok && s < module_level_scopes.len; s++) {
        for (parse_sym_entry_t *symbol = module_level_scopes.data[s]->first_in_scope;
             ok && symbol; symbol = symbol->next_in_scope) {
            if (symbol->kind != PARSE_SYM_FUNCTION || !symbol->type) continue;
            core_buffer_t qualified = {0};
            core_buffer_printf(&qualified, "%s.%.*s", module_text,
                (int)symbol->name.u8_bytes, symbol->name.data);
            th_type *contract = symbol->callable_contract
                ? symbol->callable_contract
                : th_env_signature_contract(state->types, symbol->type,
                    symbol->error_type ? symbol->error_type
                                       : th_name(state->types, "nil"));
            if (!symbol->callable_contract)
                contract = th_generalize_excluding(state->types, contract,
                    value_environment, value_environment_count);
            ok = th_env_register_function_contract(environment, qualified.data,
                symbol->name.data, module_text, "default",
                symbol_visibility(symbol), contract);
            core_buffer_release(&qualified);
        }
    }
    core_list_free(module_level_scopes);
    core_free(module_text);
    return ok;
}

static bool
register_builtin_traits(th_env *environment, th_ctx *types)
{
    th_type *methods = th_record(types, nullptr, 0);
    if (!methods ||
        !th_env_register_trait_contracts(environment, "Bracket_Literal",
            "builtins", "default", TH_ENV_PUBLIC, methods) ||
        !th_env_register_trait_contracts(environment, "Brace_Literal",
            "builtins", "default", TH_ENV_PUBLIC, methods))
        return false;

    th_type *source = th_var(types);
    th_type *cursor = th_var(types);
    th_type *iter_error = th_var(types);
    th_param iter_param = {.mode = TH_IN, .type = source};
    th_type *iter_signature = th_func_with_error(types, cursor, iter_error,
        &iter_param, 1, nullptr, nullptr, 0);
    th_member iter_method = {
        .name = "iter",
        .type = iter_signature ? th_generalize(types, iter_signature) : nullptr,
    };
    th_type *iter_contracts = iter_method.type
        ? th_record(types, &iter_method, 1) : nullptr;

    th_type *iterator = th_var(types);
    th_type *item = th_var(types);
    th_type *next_error = th_var(types);
    th_arg item_arg = {.kind = TH_ARG_TYPE, .type = item};
    th_type *optional_item = th_apply(types, "option", &item_arg, 1);
    th_param next_param = {.mode = TH_INOUT, .type = iterator};
    th_type *next_signature = optional_item
        ? th_func_with_error(types, optional_item, next_error, &next_param,
            1, nullptr, nullptr, 0) : nullptr;
    th_member next_method = {
        .name = "next",
        .type = next_signature ? th_generalize(types, next_signature) : nullptr,
    };
    th_type *next_contracts = next_method.type
        ? th_record(types, &next_method, 1) : nullptr;
    if (!iter_contracts || !next_contracts
        || !th_env_register_trait_contracts(environment, "Iterable",
            "builtins", "default", TH_ENV_PUBLIC, iter_contracts)
        || !th_env_register_trait_contracts(environment, "Iterator",
            "builtins", "default", TH_ENV_PUBLIC, next_contracts))
        return false;

    th_type *list_element = th_var(types);
    th_type *array_element = th_var(types);
    th_type *set_element = th_var(types);
    th_type *dict_key = th_var(types);
    th_type *dict_value = th_var(types);
    th_arg list_arg = {.kind = TH_ARG_TYPE, .type = list_element};
    th_arg set_arg = {.kind = TH_ARG_TYPE, .type = set_element};
    th_arg dict_args[] = {
        {.kind = TH_ARG_TYPE, .type = dict_key},
        {.kind = TH_ARG_TYPE, .type = dict_value},
    };
    return th_env_register_intrinsic_type_pattern(environment,
               "Bracket_Literal", th_apply(types, "list", &list_arg, 1)) &&
           th_env_register_intrinsic_type_pattern(environment,
               "Bracket_Literal", th_array(types, array_element)) &&
           th_env_register_intrinsic_type_pattern(environment,
               "Brace_Literal", th_apply(types, "set", &set_arg, 1)) &&
           th_env_register_intrinsic_type_pattern(environment,
               "Brace_Literal", th_apply(types, "dict", dict_args, 2));
}

static th_env *
build_import_environment(parse_grammar_t *grammar,
                         core_list_t(core_translation_unit_ptr_t) *units)
{
    core_list_t(th_type_ptr_t) value_environment =
        value_type_environment(units->data, units->len);
    th_env *environment = th_env_new();
    n00bb_tu_state_t *root_state = units->len
        ? tu_state(units->data[0]) : nullptr;
    bool ok = environment && root_state && root_state->types &&
        register_builtin_traits(environment, root_state->types);
    for (size_t i = 0; ok && i < units->len; i++)
        ok = register_unit_environment(grammar, environment, units->data[i],
            value_environment.data, value_environment.len);
    if (ok) ok = th_env_freeze(environment);
    if (!ok) {
        core_list_free(value_environment);
        th_env_free(environment);
        return nullptr;
    }
    bool environment_attached = false;
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->types) continue;
        bool attached = environment_attached
            ? th_ctx_set_environment_scope(state->types,
                unit->module.identifier.data, "default")
            : th_ctx_set_environment(state->types, environment,
                unit->module.identifier.data, "default");
        if (!attached) {
            core_buffer_t message = {0};
            size_t error_count = th_error_count(state->types);
            th_error_t detail = {0};
            bool has_detail = error_count &&
                th_error_at(state->types, error_count - 1, &detail);
            if (has_detail && detail.constraint) {
                core_buffer_printf(&message,
                    "trait constraint '%s' is not satisfied for %s: %s",
                    detail.constraint,
                    detail.got ? th_print(state->types, detail.got)
                               : "this type",
                    detail.message ? detail.message : "constraint failure");
                if (detail.expected)
                    core_buffer_printf(&message, " (expected %s)",
                        th_print(state->types, detail.expected));
                th_env_method_requirement_t requirements[16] = {0};
                size_t requirement_count = th_env_method_requirements(
                    state->types, detail.constraint, requirements, 16);
                for (size_t r = 0; r < requirement_count && r < 16; r++) {
                    size_t candidates = th_env_method_candidates(
                        state->types, requirements[r].trait_name,
                        detail.constraint, nullptr, 0);
                    core_buffer_printf(&message,
                        "; %s has %zu matching candidate%s",
                        requirements[r].trait_name, candidates,
                        candidates == 1 ? "" : "s");
                }
            }
            else {
                core_buffer_puts(&message,
                    "trait constraint is not satisfied in the loaded module graph");
            }
            core_diagnostics_report(core_translation_unit_diagnostics(unit),
                unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0003"),
                core_string_borrow(message.data, message.byte_len));
            core_buffer_release(&message);
            ok = false;
        }
        else environment_attached = true;
    }
    core_list_free(value_environment);
    return environment;
}

static bool
prepare_initial_symbols(parse_grammar_t *grammar,
                        core_translation_unit_t *unit, th_ctx *types)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !types) return false;
    if (state->annotations) return true;

    state->types = types;
    state->annotations = parse_annot_result_new(nullptr);
    parse_annot_result_set_diagnostics(state->annotations,
        core_translation_unit_diagnostics(unit), unit);
    parse_annot_result_set_node_callback(state->annotations,
                                         initial_symbol_node,
                                         state->annotations);
    parse_tree_walk_t *walk = parse_tree_walk_new(grammar, state->annotations);
    parse_annot_register_pass(walk, state->annotations, state->types, nullptr,
                              PARSE_ANNOT_PASS_INITIAL_SYMBOLS);
    state->annotation_walk = walk;
    state->reference_walk = parse_tree_walk_new(grammar, state->annotations);
    parse_annot_register_pass(state->reference_walk, state->annotations,
                              state->types, nullptr,
                              PARSE_ANNOT_PASS_CFLOW);
    return true;
}

static bool
append_typecase_specs(parse_grammar_t *grammar, parse_tree_t *node,
                      core_buffer_t *text, size_t *count)
{
    if (!node) return true;
    if (nt_is(grammar, node, "type-spec")) {
        if (*count) core_buffer_puts(text, " | ");
        if (!append_node_tokens(node, text)) return false;
        (*count)++;
        return true;
    }
    if (core_tree_is_leaf(node)) return true;
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!append_typecase_specs(grammar, core_tree_child(node, i),
                                   text, count)) return false;
    return true;
}

static parse_sym_entry_t *
typecase_subject_symbol(parse_grammar_t *grammar,
                        parse_annot_result_t *annotations,
                        parse_tree_t *case_block)
{
    parse_tree_t *typecase = core_tree_node_value(case_block).parent;
    while (typecase && !nt_is(grammar, typecase, "typecase-stmt"))
        typecase = core_tree_node_value(typecase).parent;
    parse_cf_label_t *label = typecase
        ? parse_cf_label_lookup(annotations, typecase) : nullptr;
    parse_tree_t *subject = label ? n00bb_simple_lvalue(label->cond) : nullptr;
    parse_sym_entry_t *symbol = subject
        ? core_tree_node_value(subject).sym : nullptr;
    return symbol && (symbol->kind == PARSE_SYM_VARIABLE
                      || symbol->kind == PARSE_SYM_PARAM)
        ? symbol : nullptr;
}

static void
prepare_typecase_arm(parse_grammar_t *grammar, core_translation_unit_t *unit,
                     parse_tree_t *case_block)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_sym_entry_t *subject = state
        ? typecase_subject_symbol(grammar, state->annotations, case_block)
        : nullptr;
    parse_tree_t *types = child_nt(grammar, case_block, "type-spec-list");
    core_buffer_t text = {0};
    size_t count = 0;
    bool parsed_text = types
        && append_typecase_specs(grammar, types, &text, &count) && count;
    th_type *narrowed = parsed_text
        ? ir_type_from_text(state->types,
              core_string_borrow(text.data, text.byte_len)) : nullptr;
    core_buffer_release(&text);
    if (!subject || !narrowed) {
        core_diagnostics_report(core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-TYPECASE"),
            CORE_STRING_STATIC("typecase requires a variable subject and valid case types"));
        return;
    }
    core_tree_node_value(case_block).sym = subject;
    core_tree_node_value(case_block).type = narrowed;
}

static void
apply_typecase_narrowing(parse_grammar_t *grammar, parse_tree_t *node)
{
    parse_sym_entry_t *symbol = core_tree_node_value(node).sym;
    if (!symbol) return;
    for (parse_tree_t *parent = core_tree_node_value(node).parent; parent;
         parent = core_tree_node_value(parent).parent) {
        if (!nt_is(grammar, parent, "typecase-case-block")) continue;
        parse_nt_node_t *arm = &core_tree_node_value(parent);
        if (arm->sym == symbol && arm->type) {
            core_tree_node_value(node).type = arm->type;
            return;
        }
    }
}

static parse_sym_entry_t *
variant_payload_arm(parse_grammar_t *grammar, core_translation_unit_t *unit,
                   parse_tree_t *node)
{
    if (!nt_is(grammar, node, "postfix-expr")
        || core_tree_num_children(node) < 3
        || !parse_tree_leaf_is(core_tree_child(node, 1),
                               CORE_STRING_STATIC("."))) return nullptr;
    parse_tree_t *receiver_node = core_tree_child(node, 0);
    parse_sym_entry_t *receiver = attached_symbol(grammar, unit,
                                                   receiver_node);
    if (!receiver || !receiver->type) return nullptr;
    core_string_t member = first_token_text(core_tree_child(node, 2));
    parse_tree_t *case_block = core_tree_node_value(node).parent;
    while (case_block && !nt_is(grammar, case_block, "switch-case-block"))
        case_block = core_tree_node_value(case_block).parent;
    if (!case_block) return nullptr;
    parse_tree_t *switch_node = core_tree_node_value(case_block).parent;
    while (switch_node && !nt_is(grammar, switch_node, "switch-stmt"))
        switch_node = core_tree_node_value(switch_node).parent;
    n00bb_tu_state_t *state = tu_state(unit);
    parse_cf_label_t *label = state && switch_node
        ? parse_cf_label_lookup(state->annotations, switch_node) : nullptr;
    parse_tree_t *subject = label
        ? n00bb_simple_lvalue(label->cond) : nullptr;
    if (!subject || attached_symbol(grammar, unit, subject) != receiver)
        return nullptr;
    parse_scope_t *variant_scope = n00bb_variant_scope_for_type(
        unit, state, receiver->type);
    parse_tree_t *item = descendant_nt(grammar, case_block,
                                         "case-expr-item");
    parse_tree_t *pattern = item
        ? child_nt(grammar, item, "expression") : nullptr;
    core_string_t pattern_name = {0};
    if (!variant_scope
        || !simple_identifier_expression(grammar, pattern, &pattern_name)
        || !core_string_eq(pattern_name, member)) return nullptr;
    parse_sym_entry_t *arm = parse_symtab_lookup_in(variant_scope, member);
    return arm && arm->decl_node
        && nt_is(grammar, arm->decl_node, "variant-decl-one") ? arm : nullptr;
}

static bool
prepare_variant_payload_type(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             parse_tree_t *node)
{
    parse_sym_entry_t *arm = variant_payload_arm(grammar, unit, node);
    if (!arm || !arm->type) return false;
    core_tree_node_value(node).sym = arm;
    core_tree_node_value(node).type = arm->type;
    return true;
}

static void
preprocessor_annotation_node(void *context, core_translation_unit_t *unit,
                             parse_tree_t *node, parse_tree_t *parent,
                             int32_t depth, int32_t child_index,
                             bool entering)
{
    annotation_context_t *annotation_context = context;
    parse_grammar_t *grammar = annotation_context->grammar;
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !node || !prepare_initial_symbols(
            grammar, unit, annotation_context->types)) return;
    if (!state->annotation_root) state->annotation_root = node;

    static const parse_annot_kind_t kinds[] = {
        PARSE_ANNOT_SCOPE_OPEN,
        PARSE_ANNOT_ADT,
        PARSE_ANNOT_NAMESPACE,
        PARSE_ANNOT_DECLARES,
        PARSE_ANNOT_TYPE_DECL,
        PARSE_ANNOT_FIELD,
        PARSE_ANNOT_METHOD,
        PARSE_ANNOT_TYPE,
        PARSE_ANNOT_EXPOSES,
        PARSE_ANNOT_INHERITS,
        PARSE_ANNOT_VISIBILITY,
        PARSE_ANNOT_STATIC,
        PARSE_ANNOT_ABSTRACT,
        PARSE_ANNOT_LITERAL,
        PARSE_ANNOT_RECORD,
        PARSE_ANNOT_MUTABILITY,
        PARSE_ANNOT_DIRECTION,
        PARSE_ANNOT_PASS_BY,
        PARSE_ANNOT_CONSTRAIN,
        PARSE_ANNOT_INFER,
        PARSE_ANNOT_SIZE,
        PARSE_ANNOT_ALIGNMENT,
        PARSE_ANNOT_OFFSET,
        PARSE_ANNOT_WIDTH,
    };
    static const parse_annot_kind_t reference_kinds[] = {
        PARSE_ANNOT_BRANCH,
        PARSE_ANNOT_LOOP,
        PARSE_ANNOT_SWITCH,
        PARSE_ANNOT_JUMP,
        PARSE_ANNOT_CAPTURE,
        PARSE_ANNOT_ASSIGNS,
        PARSE_ANNOT_VARREF,
        PARSE_ANNOT_OPERATOR,
        PARSE_ANNOT_NOTRIVIA,
        PARSE_ANNOT_CALL,
    };
    bool variant_payload = !entering
        && prepare_variant_payload_type(grammar, unit, node);
    parse_annot_kind_t filtered_kinds[
        sizeof(kinds) / sizeof(kinds[0])];
    size_t filtered_count = 0;
    for (size_t i = 0; variant_payload
            && i < sizeof(kinds) / sizeof(kinds[0]); i++)
        if (kinds[i] != PARSE_ANNOT_INFER)
            filtered_kinds[filtered_count++] = kinds[i];
    const parse_annot_kind_t *node_kinds = variant_payload
        ? filtered_kinds : kinds;
    size_t node_kind_count = variant_payload
        ? filtered_count : sizeof(kinds) / sizeof(kinds[0]);
    if (entering) {
        parse_tree_walk_apply_node(state->annotation_walk, node, parent,
            depth, child_index, node_kinds, node_kind_count, true);
        parse_tree_walk_apply_node(state->reference_walk, node, parent,
            depth, child_index, reference_kinds,
            sizeof(reference_kinds) / sizeof(reference_kinds[0]), true);
    }
    else {
        parse_tree_walk_apply_node(state->reference_walk, node, parent,
            depth, child_index, reference_kinds,
            sizeof(reference_kinds) / sizeof(reference_kinds[0]), false);
        parse_tree_walk_apply_node(state->annotation_walk, node, parent,
            depth, child_index, node_kinds, node_kind_count, false);
    }
    if (entering && nt_is(grammar, node, "typecase-case-block"))
        prepare_typecase_arm(grammar, unit, node);
    if (!entering) apply_typecase_narrowing(grammar, node);
    if (!entering && (nt_is(grammar, node, "pos-param")
        || nt_is(grammar, node, "default-param")
        || nt_is(grammar, node, "vargs-param")))
        bind_declared_parameter(grammar, unit, node, state);

    if (entering && nt_is(grammar, node, "func-def")) {
        parse_sym_entry_t *symbol = core_tree_node_value(node).sym;
        if (symbol) {
            th_type *declared_failure = nullptr;
            th_type *declared = explicit_function_signature(
                grammar, unit, node, state->types, &declared_failure);
            if (!declared)
                declared = provisional_function_signature(grammar, unit, node,
                                                          state->types);
            if (declared) {
                symbol->type = declared;
                symbol->error_type = declared_failure
                    ? declared_failure : th_name(state->types, "nil");
            }
        }
    }

    if (!entering && nt_is(grammar, node, "trait-spec")) {
        parse_sym_entry_t *symbol = core_tree_node_value(node).sym;
        if (symbol) {
            th_type *failure = nullptr;
            th_type *signature = explicit_trait_method_signature(
                grammar, unit, node, state->types, &failure);
            if (signature) {
                symbol->type = signature;
                symbol->error_type = failure
                    ? failure
                    : th_name(state->types, "nil");
                th_type *contract = th_env_signature_contract(
                    state->types, symbol->type, symbol->error_type);
                symbol->callable_contract = th_generalize(state->types,
                                                          contract);
            }
        }
    }

    if (!entering && nt_is(grammar, node, "func-def"))
        record_function_signature(grammar, unit, node, state, true);
    if (entering && nt_is(grammar, node, "context-def"))
        record_context_signature(grammar, unit, node, state);

    if (!entering && node == state->annotation_root) {
        for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
             scope; scope = scope->all_next) {
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                if (!symbol->type && symbol->kind != PARSE_SYM_MODULE) {
                    parse_annot_result_mark_inferred(state->annotations,
                                                     symbol);
                }
            }
        }
    }
}

static const parse_cmdr_flag_t FLAGS[] = {
    {CORE_STRING_STATIC("--help"), CORE_STRING_STATIC("-h"),
     PARSE_CMDR_TYPE_BOOL, false, CORE_STRING_STATIC("show this help")},
    {CORE_STRING_STATIC("--output"), CORE_STRING_STATIC("-o"),
     PARSE_CMDR_TYPE_WORD, true, CORE_STRING_STATIC("output file")},
    {CORE_STRING_STATIC("--jobs"), {0}, PARSE_CMDR_TYPE_INT, true,
     CORE_STRING_STATIC("maximum worker count")},
    {CORE_STRING_STATIC("--dump-tokens"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("dump tokens after lexing and continue")},
    {CORE_STRING_STATIC("--dump-tree"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("dump the parse tree and continue")},
    {CORE_STRING_STATIC("--dump-preprocessed"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("dump the preprocessed tree and continue")},
    {CORE_STRING_STATIC("--dump-symbols"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("dump collected symbols after preprocessing")},
    {CORE_STRING_STATIC("--dump-ir"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("dump lowered intermediate representation")},
    {CORE_STRING_STATIC("--dump-control-flow"), {0}, PARSE_CMDR_TYPE_BOOL,
     false, CORE_STRING_STATIC("dump control-flow edges")},
    {CORE_STRING_STATIC("--dump-pdg"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("dump the full program-dependence graph")},
    {CORE_STRING_STATIC("--define"), CORE_STRING_STATIC("-D"),
     PARSE_CMDR_TYPE_WORD, true, CORE_STRING_STATIC("preprocessor definition")},
    {{0}, {0}, 0, false, {0}},
};

static const parse_cmdr_cmd_t COMMANDS[] = {
    {CORE_STRING_STATIC("build"),
     CORE_STRING_STATIC("build an executable from one module or file"),
     nullptr, CORE_STRING_STATIC("input"), 1, 1},
    {CORE_STRING_STATIC("library"),
     CORE_STRING_STATIC("build a library from one or more modules or files"),
     nullptr, CORE_STRING_STATIC("input"), 1, -1},
    {CORE_STRING_STATIC("compile"),
     CORE_STRING_STATIC("compile one translation unit"),
     nullptr, CORE_STRING_STATIC("input"), 1, 1},
    {CORE_STRING_STATIC("preprocess"),
     CORE_STRING_STATIC("emit preprocessor output for one input"),
     nullptr, CORE_STRING_STATIC("input"), 1, 1},
    {CORE_STRING_STATIC("lower"),
     CORE_STRING_STATIC("lower a module and its imports to Lantern IR"),
     nullptr, CORE_STRING_STATIC("input"), 1, 1},
    {CORE_STRING_STATIC("locate-tu"),
     CORE_STRING_STATIC("locate and load translation units"),
     nullptr, CORE_STRING_STATIC("input"), 1, -1},
    {CORE_STRING_STATIC("lex"),
     CORE_STRING_STATIC("lex inputs and dump their tokens"),
     nullptr, CORE_STRING_STATIC("input"), 1, -1},
    {CORE_STRING_STATIC("parse"),
     CORE_STRING_STATIC("parse inputs and dump their syntax trees"),
     nullptr, CORE_STRING_STATIC("input"), 1, -1},
    {{0}, {0}, nullptr, {0}, 0, 0},
};

static parse_cmdr_t *
make_cli(void)
{
    const parse_cmdr_spec_t spec = {
        .tool = CORE_STRING_STATIC("n00bb"),
        .usage = CORE_STRING_STATIC(
            "usage: n00bb [options] <module|file>\n"
            "       n00bb <command> [options] <module|file>..."),
        .flags = FLAGS,
        .commands = COMMANDS,
        .arg = CORE_STRING_STATIC("input"),
        .min_args = 1,
        .max_args = 1,
    };
    parse_cmdr_t *cli = parse_cmdr_new(&spec);
    parse_cmdr_flag_set_repeatable(cli, (core_string_t){0},
                                   CORE_STRING_STATIC("--define"), true);
    return cli;
}

static bool
is_help_request(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
            return true;
    return false;
}

static locate_request_t *
request_for_job(core_list_t(locate_request_t) *requests, core_work_job_t *job)
{
    for (size_t i = 0; i < requests->len; i++)
        if (requests->data[i].job == job) return &requests->data[i];
    return nullptr;
}

static bool
request_snapshot(core_list_t(locate_request_t) *requests,
                 pthread_mutex_t *requests_lock, core_work_job_t *job,
                 char **operand, bool *root_input)
{
    if (requests_lock) pthread_mutex_lock(requests_lock);
    locate_request_t *request = request_for_job(requests, job);
    if (request) {
        *operand = core_strdup(request->operand);
        *root_input = request->root_input;
    }
    if (requests_lock) pthread_mutex_unlock(requests_lock);
    return request != nullptr;
}

static bool
add_locate_request_for_module(core_work_queue_t *queue,
                   core_list_t(locate_request_t) *requests,
                   pthread_mutex_t *requests_lock,
                   const core_module_spec_t *module, const char *operand,
                   bool root_input, bool allow_existing,
                   core_work_job_t **job_out)
{
    if (requests_lock) pthread_mutex_lock(requests_lock);
    core_work_job_t *job = nullptr;
    core_work_submit_result_t submitted = core_work_submit(queue,
        N00BB_JOB_LOCATE_TU, module, nullptr, 0, &job);
    if (submitted == CORE_WORK_INVALID
        || submitted == CORE_WORK_DEPENDENCY_FAILED) {
        fprintf(stderr, "n00bb: cannot schedule '%s'\n", operand);
        if (requests_lock) pthread_mutex_unlock(requests_lock);
        return false;
    }

    locate_request_t *prior = request_for_job(requests, job);
    if (prior) {
        if (!allow_existing && strcmp(prior->operand, operand)) {
            fprintf(stderr,
                    "n00bb: multiple inputs identify the same module '%.*s'\n",
                    (int)core_work_job_module(job)->identifier.u8_bytes,
                    core_work_job_module(job)->identifier.data);
            if (requests_lock) pthread_mutex_unlock(requests_lock);
            return false;
        }
        prior->root_input |= root_input;
        if (job_out) *job_out = job;
        if (requests_lock) pthread_mutex_unlock(requests_lock);
        return true;
    }
    core_list_push(*requests, ((locate_request_t){
        .job = job,
        .operand = core_strdup(operand),
        .root_input = root_input,
    }));
    if (job_out) *job_out = job;
    if (requests_lock) pthread_mutex_unlock(requests_lock);
    return true;
}

static bool
add_locate_request(core_work_queue_t *queue,
                   core_list_t(locate_request_t) *requests,
                   const char *operand)
{
    core_module_spec_t module = {0};
    if (!n00bb_operand_module_spec(operand, &module)) {
        fprintf(stderr, "n00bb: invalid module or file name '%s'\n", operand);
        return false;
    }
    bool ok = add_locate_request_for_module(queue, requests, nullptr, &module,
        operand, true, false, nullptr);
    core_module_spec_release(&module);
    return ok;
}

static void
dump_token(parse_grammar_t *grammar, core_translation_unit_t *unit,
           parse_token_info_t *token)
{
    core_string_t path = core_source_file_path(&unit->source);
    const char *source = core_source_file_contents(&unit->source);
    size_t source_size = core_source_file_size(&unit->source);
    core_string_t name = parse_terminal_name(grammar, token->tid);
    size_t end = token->end_byte > source_size ? source_size : token->end_byte;
    size_t start = token->start_byte <= end ? token->start_byte : end;
    printf("%.*s:%u:%u %.*s ", (int)path.u8_bytes, path.data,
           token->line, token->column, (int)name.u8_bytes, name.data);
    fputc('"', stdout);
    for (size_t j = start; j < end; j++) {
        unsigned char ch = (unsigned char)source[j];
        if (ch == '\\' || ch == '"') { fputc('\\', stdout); fputc(ch, stdout); }
        else if (ch == '\n') fputs("\\n", stdout);
        else if (ch == '\r') fputs("\\r", stdout);
        else if (ch == '\t') fputs("\\t", stdout);
        else if (ch < 0x20 || ch == 0x7f) printf("\\x%02x", ch);
        else fputc(ch, stdout);
    }
    puts("\"");
}

static void
dump_tree(parse_grammar_t *grammar, parse_tree_t *tree)
{
    char *text = parse_tree_dump_str(grammar, tree, false);
    if (text) {
        fputs(text, stdout);
        core_free(text);
    }
}

static const char *
symbol_kind_name(parse_sym_kind_t kind)
{
    static const char *names[] = {
        "variable", "function", "type", "tag", "enum-constant", "label",
        "parameter", "module",
    };
    return (size_t)kind < sizeof(names) / sizeof(names[0]) ? names[kind]
                                                           : "unknown";
}

static const char *
symbol_mutability_name(parse_sym_mutability_t mutability)
{
    static const char *names[] = {"mutable", "immutable", "const", "global"};
    return (size_t)mutability < sizeof(names) / sizeof(names[0])
        ? names[mutability] : "unknown";
}

static bool
symbol_is_inferred(parse_annot_result_t *annotations,
                   parse_sym_entry_t *symbol)
{
    for (size_t i = 0; i < annotations->inferred.len; i++)
        if (annotations->inferred.data[i] == symbol) return true;
    return false;
}

static void
dump_symbols(core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_symtab_t *symtab = state && state->annotations
        ? state->annotations->symtab : nullptr;
    if (!symtab) return;

    core_string_t module = unit->module.identifier;
    printf("symbols %.*s:\n", (int)module.u8_bytes, module.data);
    printf("  imports:");
    for (size_t i = 0; i < unit->import_count; i++) {
        core_string_t imported = unit->imports[i]->module.identifier;
        printf(" %.*s", (int)imported.u8_bytes, imported.data);
    }
    putchar('\n');
    for (parse_scope_t *scope = symtab->all_scopes; scope;
         scope = scope->all_next) {
        printf("  scope #%d depth=%d tag=%.*s\n", scope->id, scope->depth,
               (int)scope->scope_tag.u8_bytes,
               scope->scope_tag.data ? scope->scope_tag.data : "");
        core_list_t(parse_sym_entry_ptr_t) symbols =
            core_list_new(parse_sym_entry_ptr_t);
        for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
             symbol = symbol->next_in_scope)
            core_list_push(symbols, symbol);
        for (size_t i = symbols.len; i > 0; i--) {
            parse_sym_entry_t *symbol = symbols.data[i - 1];
            printf("    %.*s: %s, %s", (int)symbol->name.u8_bytes,
                   symbol->name.data, symbol_kind_name(symbol->kind),
                   symbol_mutability_name(symbol->mutability));
            if (symbol->type && state->types)
                printf(", type=%s", th_print(state->types, symbol->type));
            else if (symbol->type)
                printf(", type=<known>");
            else if (symbol_is_inferred(state->annotations, symbol))
                printf(", type=<inferred>");
            else
                printf(", type=<unset>");
            if (symbol->visibility.data)
                printf(", visibility=%.*s", (int)symbol->visibility.u8_bytes,
                       symbol->visibility.data);
            if (symbol->error_type && state->types)
                printf(", error=%s", th_print(state->types,
                                               symbol->error_type));
            if (symbol->direction.data)
                printf(", direction=%.*s", (int)symbol->direction.u8_bytes,
                       symbol->direction.data);
            if (symbol->pass_by.data)
                printf(", pass_by=%.*s", (int)symbol->pass_by.u8_bytes,
                       symbol->pass_by.data);
            if (symbol->is_optional) printf(", optional");
            if (symbol->is_variadic) printf(", variadic");
            if (symbol->error_type && state->types)
                printf(", error=%s", th_print(state->types,
                                               symbol->error_type));
            if (symbol->direction.data)
                printf(", direction=%.*s", (int)symbol->direction.u8_bytes,
                       symbol->direction.data);
            if (symbol->pass_by.data)
                printf(", pass_by=%.*s", (int)symbol->pass_by.u8_bytes,
                       symbol->pass_by.data);
            if (symbol->is_thread_local) printf(", thread-local");
            printf(", uses=%u", symbol->uses);
            parse_token_info_t *token = symbol->decl_node
                ? parse_tree_find_first_terminal(symbol->decl_node) : nullptr;
            if (token) printf(", at=%u:%u", token->line, token->column);
            putchar('\n');
        }
        core_list_free(symbols);
    }
}

static bool
discard_ignored_layout(parse_grammar_t *grammar, parse_token_stream_t *tokens)
{
    int64_t indent = parse_terminal_id(
        grammar, CORE_STRING_STATIC("INDENT"));
    int64_t dedent = parse_terminal_id(
        grammar, CORE_STRING_STATIC("DEDENT"));
    for (int32_t i = 0; i < scan_stream_token_count(tokens);) {
        parse_token_info_t *token = scan_stream_get(tokens, i);
        if (token && token->ignore_if_no_match
            && (token->tid == indent || token->tid == dedent)) {
            if (!scan_stream_token_to_trivia(tokens, i)) return false;
            continue;
        }
        i++;
    }
    return true;
}

static void
record_resolve_status(locate_worker_context_t *context,
                      core_work_job_t *job,
                      core_module_resolve_status_t status)
{
    pthread_mutex_lock(context->requests_lock);
    locate_request_t *request = request_for_job(context->requests, job);
    if (request) {
        request->resolve_status = status;
        request->resolve_attempted = true;
    }
    pthread_mutex_unlock(context->requests_lock);
}

static bool
process_locate_job(locate_worker_context_t *context, core_work_job_t *job)
{
    char *operand = nullptr;
    bool root_input = false;
    if (!request_snapshot(context->requests, context->requests_lock, job,
                          &operand, &root_input)) {
        record_resolve_status(context, job, CORE_MODULE_RESOLVE_ERROR);
        core_work_complete(context->queue, job, false, nullptr);
        return false;
    }
    core_module_spec_t resolved = {0};
    core_source_file_t source = {0};
    core_module_resolve_status_t status = n00bb_locate_operand(
        context->resolver, operand, &resolved, &source);
    core_free(operand);
    record_resolve_status(context, job, status);
    if (status != CORE_MODULE_RESOLVED) {
        core_module_spec_release(&resolved);
        core_work_complete(context->queue, job, false, nullptr);
        return false;
    }

    core_module_spec_release(&resolved);
    core_module_spec_init(&resolved, core_work_job_module(job)->identifier);
    core_translation_unit_t *unit = core_alloc(core_translation_unit_t);
    bool initialized = core_translation_unit_init(unit, &resolved, &source);
    core_module_spec_release(&resolved);
    if (!initialized) {
        core_source_file_release(&source);
        core_free(unit);
        core_work_complete(context->queue, job, false, nullptr);
        return false;
    }

    bool ok = true;
    if (context->lex) {
        const char *contents = core_source_file_contents(&unit->source);
        size_t length = core_source_file_size(&unit->source);
        n00bb_lex_result_t *result = core_alloc(n00bb_lex_result_t);
        memset(result, 0, sizeof(*result));
        result->tokens = scan_tokenize_owned(context->grammar, contents, length);
        if (!result->tokens) {
            core_free(result);
            ok = false;
        }
        else {
            result->tokens->scanner->file = core_option_set(
                core_string_t, core_source_file_path(&unit->source));
            n00bb_tu_state_t *state = core_alloc(n00bb_tu_state_t);
            memset(state, 0, sizeof(*state));
            state->lex = result;
            state->types = context->types;
            core_translation_unit_set_user_data(unit, state, free_tu_state);
            while (scan_stream_next(result->tokens)) {}
            if (!discard_ignored_layout(context->grammar, result->tokens)) {
                core_diagnostics_report(core_translation_unit_diagnostics(unit),
                    unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-P0002"),
                    CORE_STRING_STATIC("cannot normalize layout tokens"));
                ok = false;
            }
            scan_diagnostic_t diagnostic = {0};
            if (scan_diagnostic(result->tokens->scanner, &diagnostic) &&
                diagnostic.status != SCAN_STATUS_COMPLETE) {
                core_diagnostics_report(core_translation_unit_diagnostics(unit),
                    unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0001"),
                    diagnostic.message);
                ok = false;
            }
            if (ok && context->parse) {
                scan_stream_rewind(result->tokens,
                    scan_stream_token_count(result->tokens));
                parse_nt_id_t start = parse_nonterm_id(parse_find_nonterm(
                    context->grammar, CORE_STRING_STATIC("module")));
                result->parser = parse_pwz_new_for_start(context->grammar,
                                                         start);
                if (!parse_pwz_parse(result->parser, result->tokens)) {
                    int32_t error_pos = parse_pwz_error_pos(result->parser);
                    parse_token_info_t *error_token = error_pos >= 0
                        ? scan_stream_get(result->tokens, error_pos) : nullptr;
                    char message[192];
                    if (error_token) {
                        core_string_t token_name = parse_terminal_name(
                            context->grammar, error_token->tid);
                        char token_text[64];
                        if (token_name.data)
                            snprintf(token_text, sizeof(token_text), "%.*s",
                                (int)token_name.u8_bytes, token_name.data);
                        else if (error_token->tid > 0 && error_token->tid < 128)
                            snprintf(token_text, sizeof(token_text), "'%c'",
                                     error_token->tid);
                        else snprintf(token_text, sizeof(token_text), "token");
                        snprintf(message, sizeof(message),
                            "syntax error at line %u, column %u near %s",
                            error_token->line, error_token->column, token_text);
                    }
                    else
                        snprintf(message, sizeof(message),
                            "syntax error at end of input (line %u, column %u)",
                            result->tokens->scanner->line,
                            result->tokens->scanner->column);
                    core_diagnostics_report(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-P0001"),
                        core_string_from_cstr(message));
                    ok = false;
                }
            }
        }
    }
    (void)root_input;
    core_work_complete(context->queue, job, ok, unit);
    return ok;
}

static void *
locate_worker(void *opaque)
{
    locate_worker_context_t *context = opaque;
    core_work_job_t *job = nullptr;
    while (core_work_take_kind(context->queue, N00BB_JOB_LOCATE_TU, &job))
        process_locate_job(context, job);
    return nullptr;
}

static void
append_import_names(parse_grammar_t *grammar, parse_tree_t *node,
                    core_buffer_t *name, bool *has_name)
{
    if (!node) return;
    if (core_tree_is_leaf(node)) {
        parse_token_info_t *token = parse_node_token(node);
        if (token && core_string_eq(parse_terminal_name(grammar, token->tid),
                                    CORE_STRING_STATIC("IDENTIFIER"))) {
            if (*has_name) core_buffer_putc(name, '.');
            core_string_t text = parse_tree_leaf_text(node);
            core_buffer_append(name, text.data, text.u8_bytes);
            *has_name = true;
        }
        return;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        append_import_names(grammar, core_tree_child(node, i), name, has_name);
}

static parse_tree_t *
find_string_token(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (!node) return nullptr;
    if (core_tree_is_leaf(node)) {
        parse_token_info_t *token = parse_node_token(node);
        return token && core_string_eq(parse_terminal_name(grammar, token->tid),
            CORE_STRING_STATIC("STRING_LIT")) ? node : nullptr;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *found = find_string_token(grammar, core_tree_child(node, i));
        if (found) return found;
    }
    return nullptr;
}

static void
import_error(core_translation_unit_t *unit, parse_tree_t *statement,
             const char *message)
{
    parse_token_info_t *token = parse_tree_find_first_terminal(statement);
    core_buffer_t *text = core_buffer_empty();
    if (token)
        core_buffer_printf(text, "%s at line %u, column %u", message,
                           token->line, token->column);
    else core_buffer_append(text, message, strlen(message));
    core_diagnostics_report(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-PP0004"),
        core_string_borrow(text->data, text->byte_len));
    core_buffer_free(text);
}

static bool
import_request_details(parse_grammar_t *grammar, parse_tree_t *statement,
                      core_translation_unit_t *importer,
                      core_module_spec_t *module, char **operand_out)
{
    parse_tree_t *member = parse_tree_find_child_by_nt_name(
        grammar, statement, CORE_STRING_STATIC("member-chain"));
    core_buffer_t *name = core_buffer_empty();
    bool has_name = false;
    append_import_names(grammar, member, name, &has_name);
    if (!has_name) {
        core_buffer_free(name);
        import_error(importer, statement, "invalid imported module name");
        return false;
    }
    parse_tree_t *path_token = find_string_token(grammar, statement);
    core_string_t operand = core_string_borrow(name->data, name->byte_len);
    if (path_token) {
        core_string_t raw = parse_tree_leaf_text(path_token);
        if (raw.u8_bytes < 2 || raw.data[0] != '"'
            || raw.data[raw.u8_bytes - 1] != '"') {
            core_buffer_free(name);
            import_error(importer, statement, "invalid imported module path");
            return false;
        }
        operand = core_string_borrow(raw.data + 1, raw.u8_bytes - 2);
    }
    bool initialized = core_module_spec_init(module,
        core_string_borrow(name->data, name->byte_len));
    if (initialized)
        *operand_out = core_strndup(operand.data, operand.u8_bytes);
    core_buffer_free(name);
    if (!initialized) {
        import_error(importer, statement, "cannot form imported module identity");
        return false;
    }
    return true;
}

static bool
prepare_import(void *opaque, parse_grammar_t *grammar, parse_tree_t *statement,
               core_translation_unit_t *importer)
{
    import_driver_t *driver = opaque;
    core_module_spec_t module = {0};
    char *operand = nullptr;
    if (!import_request_details(grammar, statement, importer, &module,
                                &operand)) return false;
    core_work_job_t *job = nullptr;
    bool scheduled = add_locate_request_for_module(driver->queue,
        driver->requests, driver->requests_lock, &module, operand, false,
        true, &job);
    core_free(operand);
    core_module_spec_release(&module);
    if (!scheduled)
        import_error(importer, statement, "cannot schedule imported module");
    return scheduled;
}

static bool
load_import(void *opaque, parse_grammar_t *grammar, parse_tree_t *statement,
            core_translation_unit_t *importer,
            core_translation_unit_t **imported_out,
            parse_tree_t **tree_out)
{
    import_driver_t *driver = opaque;
    core_module_spec_t module = {0};
    char *operand_copy = nullptr;
    if (!import_request_details(grammar, statement, importer, &module,
                                &operand_copy)) return false;
    core_work_job_t *locate_job = nullptr;
    bool scheduled = add_locate_request_for_module(driver->queue,
        driver->requests, driver->requests_lock, &module, operand_copy,
        false, true, &locate_job);
    core_free(operand_copy);
    core_module_spec_release(&module);
    if (!scheduled) {
        import_error(importer, statement, "cannot schedule imported module");
        return false;
    }

    while (!driver->worker_count &&
           core_work_job_state(driver->queue, locate_job) != CORE_WORK_SUCCEEDED &&
           core_work_job_state(driver->queue, locate_job) != CORE_WORK_FAILED) {
        core_work_job_t *ready = nullptr;
        if (!core_work_take_kind(driver->queue, N00BB_JOB_LOCATE_TU, &ready))
            break;
        process_locate_job(driver->loader, ready);
    }
    if (!core_work_wait(driver->queue, locate_job)) {
        import_error(importer, statement, "imported module failed to load");
        return false;
    }
    core_translation_unit_t *imported = core_work_job_result(
        driver->queue, locate_job);
    n00bb_tu_state_t *state = tu_state(imported);
    n00bb_lex_result_t *lex = state ? state->lex : nullptr;
    parse_tree_t *tree = lex && lex->parser
        ? parse_pwz_get_tree(lex->parser) : nullptr;
    if (!imported || !tree
        || !core_translation_unit_add_import(importer, imported)) {
        import_error(importer, statement, "cannot retain imported module");
        return false;
    }

    core_work_job_t *dependencies[] = {locate_job};
    core_work_job_t *preprocess_job = nullptr;
    core_work_submit_result_t submitted = core_work_submit(driver->queue,
        N00BB_JOB_PREPROCESS, core_work_job_module(locate_job), dependencies,
        1, &preprocess_job);
    if (submitted == CORE_WORK_INVALID
        || submitted == CORE_WORK_DEPENDENCY_FAILED) {
        import_error(importer, statement, "cannot schedule imported preprocessing");
        return false;
    }
    if (submitted == CORE_WORK_SUBMITTED) {
        core_work_job_t *ready = nullptr;
        if (!core_work_take_kind(driver->queue, N00BB_JOB_PREPROCESS, &ready) ||
            ready != preprocess_job) {
            import_error(importer, statement, "imported preprocessing is not ready");
            return false;
        }
        bool ok = n00bb_preprocessor_run(driver->preprocessor, grammar, tree,
                                         imported);
        if (core_translation_unit_has_fatal_diagnostics(imported)) ok = false;
        core_work_complete_tu(driver->queue, preprocess_job, imported, ok);
        if (!ok) return false;
    }
    else if (core_work_job_state(driver->queue, preprocess_job)
             == CORE_WORK_FAILED) return false;

    *imported_out = imported;
    *tree_out = tree;
    return true;
}

static bool
run_locate_jobs(core_work_queue_t *queue, n00bb_resolver_t *resolver,
                core_list_t(locate_request_t) *requests,
                parse_grammar_t *grammar, bool lex, bool show_tokens,
                bool parse, bool show_tree, bool preprocess,
                bool show_preprocessed, bool show_symbols,
                const n00bb_preprocessor_options_t *preprocessor_options,
                th_ctx *types, size_t worker_limit,
                th_env **environment_out)
{
    if (environment_out) *environment_out = nullptr;
    bool success = true;
    size_t root_request_count = requests->len;
    core_diagnostics_t *diags = core_diagnostics_new();
    pthread_mutex_t requests_lock;
    pthread_mutex_init(&requests_lock, nullptr);
    locate_worker_context_t loader = {
        .queue = queue,
        .resolver = resolver,
        .requests = requests,
        .requests_lock = &requests_lock,
        .grammar = grammar,
        .types = types,
        .lex = lex,
        .parse = parse,
    };
    pthread_t *worker_threads = worker_limit
        ? core_alloc_array(pthread_t, worker_limit) : nullptr;
    size_t worker_count = 0;
    for (; worker_count < worker_limit; worker_count++)
        if (pthread_create(&worker_threads[worker_count], nullptr,
                           locate_worker, &loader) != 0)
            break;
    import_driver_t import_driver = {
        .queue = queue,
        .resolver = resolver,
        .requests = requests,
        .requests_lock = &requests_lock,
        .loader = &loader,
        .worker_count = worker_count,
        .grammar = grammar,
        .types = types,
    };
    annotation_context_t annotation_context = {grammar, types};
    n00bb_preprocessor_options_t active_options = *preprocessor_options;
    active_options.load_import = load_import;
    active_options.import_context = &import_driver;
    active_options.prepare_import = prepare_import;
    active_options.visit_node = preprocessor_annotation_node;
    active_options.visit_context = &annotation_context;
    n00bb_preprocessor_t *preprocessor =
        n00bb_preprocessor_new(&active_options);
    import_driver.preprocessor = preprocessor;
    for (size_t request_ix = 0; request_ix < root_request_count; request_ix++) {
        locate_request_t *request = &requests->data[request_ix];
        core_work_job_t *job = request->job;
        if (!worker_count) {
            while (core_work_job_state(queue, job) != CORE_WORK_SUCCEEDED &&
                   core_work_job_state(queue, job) != CORE_WORK_FAILED) {
                core_work_job_t *ready = nullptr;
                if (!core_work_take_kind(queue, N00BB_JOB_LOCATE_TU, &ready))
                    break;
                process_locate_job(&loader, ready);
            }
        }
        bool job_ok = core_work_wait(queue, job);
        core_translation_unit_t *unit = core_work_job_result(queue, job);
        if (!job_ok) success = false;
        if (!unit) {
            const char *operand = request->operand;
            core_module_resolve_status_t status = request->resolve_attempted
                ? request->resolve_status : CORE_MODULE_RESOLVE_ERROR;
            core_string_t searched = n00bb_resolver_search_description(
                resolver, operand);
            core_buffer_t message = {0};
            core_buffer_printf(&message, "%s '%s'",
                status == CORE_MODULE_NOT_FOUND ? "cannot locate" : "cannot read",
                operand);
            if (searched.data) core_buffer_printf(&message, "\n%s", searched.data);
            core_diagnostics_report(diags, nullptr, CORE_DIAG_ERROR,
                status == CORE_MODULE_NOT_FOUND
                    ? CORE_STRING_STATIC("N00BB-B0001")
                    : CORE_STRING_STATIC("N00BB-B0002"),
                core_string_from_cstr(message.data));
            core_buffer_release(&message);
            core_free(searched.data);
            continue;
        }
        n00bb_tu_state_t *state = tu_state(unit);
        n00bb_lex_result_t *lex_result = state ? state->lex : nullptr;
        if (show_tokens && request->root_input && lex_result) {
            int32_t count = scan_stream_token_count(lex_result->tokens);
            for (int32_t i = 0; i < count; i++) {
                parse_token_info_t *token = scan_stream_get(lex_result->tokens, i);
                if (token) dump_token(grammar, unit, token);
            }
        }
        if (show_tree && request->root_input && lex_result &&
            lex_result->parser && job_ok)
            dump_tree(grammar, parse_pwz_get_tree(lex_result->parser));
        if (!lex) {
            core_string_t path = core_source_file_path(&unit->source);
            const core_module_spec_t *module = core_work_job_module(job);
            printf("located %.*s from %.*s (%zu bytes)\n",
                   (int)module->identifier.u8_bytes, module->identifier.data,
                   (int)path.u8_bytes, path.data,
                   core_source_file_size(&unit->source));
        }
    }

    if (preprocess && parse) {
        for (size_t i = 0; i < root_request_count; i++) {
            if (!requests->data[i].root_input) continue;
            core_work_job_t *dependencies[] = {requests->data[i].job};
            core_work_job_t *preprocess_job = nullptr;
            core_work_submit_result_t submitted = core_work_submit(queue,
                N00BB_JOB_PREPROCESS,
                core_work_job_module(requests->data[i].job), dependencies,
                1, &preprocess_job);
            if (submitted == CORE_WORK_INVALID
                || submitted == CORE_WORK_DEPENDENCY_FAILED) {
                success = false;
                continue;
            }
            if (submitted == CORE_WORK_SUBMITTED) {
                core_work_job_t *ready = nullptr;
                if (!core_work_take_kind(queue, N00BB_JOB_PREPROCESS, &ready) ||
                    ready != preprocess_job) {
                    success = false;
                    continue;
                }
                core_translation_unit_t *unit = core_work_job_result(
                    queue, requests->data[i].job);
                n00bb_tu_state_t *state = tu_state(unit);
                n00bb_lex_result_t *lex = state ? state->lex : nullptr;
                parse_tree_t *tree = lex && lex->parser
                    ? parse_pwz_get_tree(lex->parser) : nullptr;
                bool job_ok = tree && n00bb_preprocessor_run(
                    preprocessor, grammar, tree, unit);
                if (core_translation_unit_has_fatal_diagnostics(unit))
                    job_ok = false;
                if (job_ok) job_ok = !core_diagnostics_failed(
                    core_translation_unit_diagnostics(unit));
                if (job_ok && show_preprocessed)
                    dump_tree(grammar, tree);
                core_work_complete_tu(queue, preprocess_job, unit, job_ok);
                if (!job_ok) success = false;
            }
            else if (core_work_job_state(queue, preprocess_job)
                     == CORE_WORK_FAILED) success = false;
        }
        if (success) {
            core_list_t(core_translation_unit_ptr_t) units =
                core_list_new(core_translation_unit_ptr_t);
            for (size_t i = 0; i < root_request_count; i++) {
                if (!requests->data[i].root_input) continue;
                core_translation_unit_t *root = core_work_job_result(
                    queue, requests->data[i].job);
                if (!root) continue;
                collect_import_closure(root, &units);
            }
            bool shared_universe = module_type_universe_is_shared(&units);
            if (!shared_universe) success = false;
            if (shared_universe)
                for (size_t i = 0; i < units.len; i++)
                    finalize_module_inference(units.data[i]);
            th_env *environment = shared_universe
                ? build_import_environment(grammar, &units) : nullptr;
            if (environment_out) *environment_out = environment;
            if (!environment) success = false;
            if (environment) {
                for (size_t i = 0; i < units.len; i++) {
                    core_translation_unit_t *unit = units.data[i];
                    n00bb_tu_state_t *state = tu_state(unit);
                    if (state && state->annotation_root && state->types)
                        validate_refined_function_bodies(grammar, unit,
                            state->annotation_root, state);
                }
                for (size_t i = 0; i < root_request_count; i++) {
                    if (!requests->data[i].root_input) continue;
                    core_translation_unit_t *root = core_work_job_result(
                        queue, requests->data[i].job);
                    if (root && !generalize_import_closure(grammar, root))
                        success = false;
                }
                for (size_t i = 0; i < units.len; i++) {
                    core_translation_unit_t *unit = units.data[i];
                    n00bb_tu_state_t *state = tu_state(unit);
                    if (!state || !state->types) continue;
                    if (!th_ctx_set_environment_scope(state->types,
                            unit->module.identifier.data, "default")) {
                        success = false;
                        continue;
                    }
                    unify_call_results(grammar, unit, units.data, units.len,
                                       nullptr, 0, true);
                }
            }
            for (size_t i = 0; i < units.len; i++) {
                core_translation_unit_t *unit = units.data[i];
                if (core_diagnostics_failed(
                        core_translation_unit_diagnostics(unit)))
                    success = false;
            }
            core_list_free(units);
        }
        core_work_queue_close(queue);
    }
    else core_work_queue_close(queue);

    for (size_t i = 0; i < worker_count; i++)
        pthread_join(worker_threads[i], nullptr);
    core_free(worker_threads);
    pthread_mutex_destroy(&requests_lock);

    for (size_t i = 0; i < requests->len; i++) {
        core_translation_unit_t *unit = core_work_job_result(
            queue, requests->data[i].job);
        if (unit && show_symbols) dump_symbols(unit);
        if (unit)
            core_diagnostics_merge(diags,
                core_translation_unit_diagnostics(unit));
    }

    n00bb_preprocessor_free(preprocessor);
    if (core_diagnostics_count(diags)) {
        char *text = core_diagnostics_render_text(diags);
        if (text) { fputs(text, stderr); core_free(text); }
    }
    core_diagnostics_free(diags);
    return success;
}

static void
release_requests(core_work_queue_t *queue,
                 core_list_t(locate_request_t) *requests)
{
    for (size_t i = 0; i < requests->len; i++) {
        core_translation_unit_t *unit =
            core_work_job_result(queue, requests->data[i].job);
        if (unit) {
            core_translation_unit_release(unit);
            core_free(unit);
        }
        core_free(requests->data[i].operand);
    }
    core_list_free(*requests);
}

int
main(int argc, char **argv)
{
    parse_cmdr_t *cli = make_cli();
    if (argc == 1 || is_help_request(argc, argv)) {
        parse_cmdr_print_usage(cli, argc == 1 ? stderr : stdout);
        parse_cmdr_free(cli);
        return argc == 1 ? 2 : 0;
    }

    parse_cmdr_result_t *args =
        parse_cmdr_parse(cli, argc - 1, (const char **)argv + 1);
    if (!parse_cmdr_result_ok(args)) {
        for (int32_t i = 0; i < parse_cmdr_result_error_count(args); i++) {
            core_string_t error = parse_cmdr_result_error(args, i);
            fprintf(stderr, "n00bb: %.*s\n", (int)error.u8_bytes, error.data);
        }
        parse_cmdr_print_usage(cli, stderr);
        parse_cmdr_result_free(args);
        parse_cmdr_free(cli);
        return 2;
    }

    core_string_t command = parse_cmdr_result_command(args);
    bool is_default = !command.data;
    if (is_default) command = CORE_STRING_STATIC("build");

    int32_t input_count = parse_cmdr_arg_count(args);
    core_work_queue_t *queue = core_work_queue_new();
    core_list_t(locate_request_t) requests = core_list_new(locate_request_t);
    bool scheduled = true;
    for (int32_t i = 0; i < input_count; i++) {
        core_string_t input = parse_cmdr_arg_str(args, i);
        char *operand = core_strndup(input.data, input.u8_bytes);
        scheduled = add_locate_request(queue, &requests, operand) && scheduled;
        core_free(operand);
    }

    n00bb_resolver_t *resolver = n00bb_resolver_new(argv[0]);
    if (!resolver) {
        fprintf(stderr, "n00bb: cannot determine the current directory\n");
        scheduled = false;
    }
    bool lex_command = core_string_eq(command, CORE_STRING_STATIC("lex"));
    bool parse_command = core_string_eq(command, CORE_STRING_STATIC("parse"));
    bool locate_only = core_string_eq(command, CORE_STRING_STATIC("locate-tu"));
    bool preprocess_command = core_string_eq(command, CORE_STRING_STATIC("preprocess"));
    bool lower_command = core_string_eq(command, CORE_STRING_STATIC("lower"));
    bool build_command = is_default
        || core_string_eq(command, CORE_STRING_STATIC("build"));
    bool library_command = core_string_eq(command, CORE_STRING_STATIC("library"));
    bool compile_command = core_string_eq(command, CORE_STRING_STATIC("compile"));
    bool show_tokens = lex_command || parse_cmdr_flag_present(
        args, CORE_STRING_STATIC("--dump-tokens"));
    bool show_tree = parse_command || parse_cmdr_flag_present(
        args, CORE_STRING_STATIC("--dump-tree"));
    bool show_preprocessed = preprocess_command || parse_cmdr_flag_present(
        args, CORE_STRING_STATIC("--dump-preprocessed"));
    bool show_symbols = parse_cmdr_flag_present(
        args, CORE_STRING_STATIC("--dump-symbols"));
    int64_t requested_jobs = parse_cmdr_flag_int(
        args, CORE_STRING_STATIC("--jobs"), 4);
    if (requested_jobs < 0 || requested_jobs > 128) {
        fprintf(stderr, "n00bb: --jobs must be between 0 and 128\n");
        scheduled = false;
    }
    int32_t define_count = parse_cmdr_flag_value_count(
        args, CORE_STRING_STATIC("--define"));
    const char **definitions = define_count
        ? core_alloc_array(const char *, (size_t)define_count) : nullptr;
    for (int32_t i = 0; i < define_count; i++)
        definitions[i] = parse_cmdr_flag_str_at(
            args, CORE_STRING_STATIC("--define"), i).data;
    n00bb_preprocessor_options_t preprocessor_options = {
        .definitions = definitions,
        .definition_count = (size_t)define_count,
    };
    parse_grammar_t *grammar = nullptr;
    th_env *environment = nullptr;
    ir_ctx_t *ir_context = ir_ctx_new();
    th_ctx *types = ir_ctx_types(ir_context);
    if (!locate_only) {
        grammar = parse_grammar_new();
        parse_grammar_set_error_recovery(grammar, false);
        char *grammar_path = n00bb_grammar_path(argv[0]);
        if (!grammar_path || !parse_bnf_load_file(
                grammar_path, CORE_STRING_STATIC("module"), grammar)) {
            const char *error = parse_bnf_error();
            fprintf(stderr, "n00bb: cannot load n00b grammar%s%s\n",
                    error ? ": " : "", error ? error : "");
            scheduled = false;
        }
        else if (!scan_grammar_lex_compiled(grammar)) {
            const char *error = scan_lex_compile_error();
            fprintf(stderr, "n00bb: cannot compile lexer%s%s\n",
                    error ? ": " : "", error ? error : "");
            scheduled = false;
        }
        core_free(grammar_path);
    }
    bool located = scheduled && run_locate_jobs(queue, resolver, &requests,
        grammar, !locate_only, show_tokens,
        !locate_only && !lex_command, show_tree,
        preprocess_command || show_symbols
            || (!locate_only && !lex_command && !parse_command),
        show_preprocessed, show_symbols, &preprocessor_options,
        types, (size_t)requested_jobs, &environment);
    if (located && (lower_command || build_command || library_command
                    || compile_command))
        located = lower_requests_to_ir(grammar, queue, &requests, ir_context,
            parse_cmdr_flag_present(args, CORE_STRING_STATIC("--dump-ir"))
                || (lower_command && !parse_cmdr_flag_present(args,
                        CORE_STRING_STATIC("--dump-control-flow"))
                    && !parse_cmdr_flag_present(args,
                        CORE_STRING_STATIC("--dump-pdg"))),
            parse_cmdr_flag_present(args,
                CORE_STRING_STATIC("--dump-control-flow")),
            parse_cmdr_flag_present(args, CORE_STRING_STATIC("--dump-pdg")));
    if (located && !locate_only && !lex_command && !parse_command
        && !preprocess_command && !lower_command) {
        fprintf(stderr,
                "n00bb: translation units are located; '%.*s' processing is not implemented yet\n",
                (int)command.u8_bytes, command.data);
        located = false;
    }

    release_requests(queue, &requests);
    th_env_free(environment);
    ir_ctx_free(ir_context);
    parse_grammar_free(grammar);
    n00bb_resolver_free(resolver);
    core_work_queue_free(queue);
    core_free(definitions);
    parse_cmdr_result_free(args);
    parse_cmdr_free(cli);
    return located ? 0 : 1;
}
