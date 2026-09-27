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

static const ir_scalar_t N00BB_SCALARS[] = {
    {"int", IR_CLASS_SINT, 64, IR_ROLE_NONE},
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
core_list_decl(th_member);
typedef th_type *th_type_ptr_t;
core_list_decl(th_type_ptr_t);
typedef core_translation_unit_t *core_translation_unit_ptr_t;
core_list_decl(core_translation_unit_ptr_t);
typedef parse_scope_t *parse_scope_ptr_t;
core_list_decl(parse_scope_ptr_t);

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
    core_list_t(core_translation_unit_ptr_t) *units;
    core_translation_unit_t *unit;
} n00bb_lower_context_t;

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
    if (!node || core_tree_is_leaf(node)) return nullptr;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (nt_is(grammar, child, name)) return child;
    }
    return nullptr;
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
        if (parameter->type && parameter->type != type) {
            th_type *unified = th_unify_in_place(types, type,
                                                 parameter->type);
            if (unified) type = unified;
        }
        parameter->type = type;
        if (symbol) symbol->type = type;
        if (variadic) {
            *rest = (th_param){.mode = TH_IN, .type = type};
            *has_rest = true;
        }
        else core_list_push(*params, ((th_param){.mode = TH_IN, .type = type}));
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

static bool
append_type_span(parse_grammar_t *grammar, core_translation_unit_t *unit,
                 parse_tree_t *node, const char *type_nonterminal,
                 core_buffer_t *buffer)
{
    (void)unit;
    parse_tree_t *spec = descendant_nt(grammar, node, type_nonterminal);
    return append_node_tokens(spec, buffer);
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
        if (!append_type_span(grammar, unit, node, "one-tspec", buffer))
            return false;
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
    th_type *parsed = ir_typelang_parse_function(types, text.data, failure,
                                                  &parse_error);
    core_free(parse_error);
    core_buffer_release(&text);
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
    th_type *provisional = th_func(types, th_var(types), parameters.data,
        parameters.len, rest_ptr, nullptr, 0);
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
    th_type *parsed = ir_typelang_parse_function(types, text.data, failure,
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

static void
bind_declared_parameter(parse_grammar_t *grammar,
                        core_translation_unit_t *unit, parse_tree_t *node,
                        n00bb_tu_state_t *state)
{
    if (!state || !state->types || !node || core_tree_is_leaf(node)) return;
    parse_tree_t *function = core_tree_node_value(node).parent;
    while (function && !nt_is(grammar, function, "func-def"))
        function = core_tree_node_value(function).parent;
    if (!function) return;
    parse_sym_entry_t *function_symbol = core_tree_node_value(function).sym;
    if (!function_symbol || !function_symbol->type) return;
    size_t index = 0;
    parse_tree_t *params = descendant_nt(grammar, function, "param-decl");
    if (!parameter_index_in(grammar, params, node, &index)) return;
    const th_param *declared = th_func_param(function_symbol->type, index);
    parse_sym_entry_t *parameter = core_tree_node_value(node).sym;
    if (!declared || !parameter) return;
    if (parameter->type) {
        th_type *unified = th_unify_in_place(state->types, parameter->type,
                                             declared->type);
        if (!unified) {
            core_diagnostics_report(core_translation_unit_diagnostics(unit),
                unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
                CORE_STRING_STATIC("parameter declaration type does not match function signature"));
            return;
        }
        parameter->type = unified;
    }
    else parameter->type = declared->type;
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
    th_type *declared = explicit_function_signature(
        grammar, unit, func, state->types, &declared_failure);
    th_type *inferred = th_func(state->types, success, params.data,
        params.len, has_rest ? &rest : nullptr, nullptr, 0);
    if (declared) {
        th_type *check_type = th_instantiate(state->types,
            th_generalize(state->types, declared));
        bool matches = !check_body || !has_success_value
            || th_unify(state->types, check_type, inferred) != nullptr;
        th_type *declared_error = declared_failure
            ? declared_failure : th_name(state->types, "nil");
        th_type *check_error = th_instantiate(state->types,
            th_generalize(state->types, declared_error));
        matches = matches && (!check_body || !has_failure_value
            || th_unify(state->types, check_error, failure) != nullptr);
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
        symbol->type = declared;
        symbol->error_type = declared_error;
    }
    else {
        if (symbol->type) {
            th_type *unified = th_unify_in_place(state->types,
                                                  symbol->type, inferred);
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

static bool
append_callee_path(parse_grammar_t *grammar, parse_tree_t *node,
                   core_buffer_t *path)
{
    if (!node) return false;
    if (core_tree_is_leaf(node)) {
        if (parse_tree_leaf_is(node, CORE_STRING_STATIC("."))) {
            core_buffer_putc(path, '.');
            return true;
        }
        parse_token_info_t *token = parse_node_token(node);
        if (!token || !core_string_eq(parse_terminal_name(grammar, token->tid),
                                      CORE_STRING_STATIC("IDENTIFIER")))
            return false;
        core_string_t name = first_token_text(node);
        if (!name.data) return false;
        core_buffer_append(path, name.data, name.u8_bytes);
        return true;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!append_callee_path(grammar, core_tree_child(node, i), path))
            return false;
    return true;
}

static bool
module_path_matches(core_string_t path, core_string_t module)
{
    if (core_string_eq(path, module)) return true;
    if (path.u8_bytes > module.u8_bytes) {
        size_t offset = path.u8_bytes - module.u8_bytes;
        return path.data[offset - 1] == '.'
            && !memcmp(path.data + offset, module.data, module.u8_bytes);
    }
    if (module.u8_bytes > path.u8_bytes) {
        size_t offset = module.u8_bytes - path.u8_bytes;
        return module.data[offset - 1] == '.'
            && !memcmp(module.data + offset, path.data, path.u8_bytes);
    }
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

static parse_sym_entry_t *
qualified_call_symbol(parse_grammar_t *grammar,
                      core_translation_unit_t *unit, parse_tree_t *callee)
{
    core_buffer_t path = {0};
    if (!append_callee_path(grammar, callee, &path)) {
        core_buffer_release(&path);
        return nullptr;
    }
    char *last_dot = strrchr(path.data, '.');
    if (!last_dot) {
        core_buffer_release(&path);
        return nullptr;
    }
    core_string_t module = core_string_borrow(path.data,
        (size_t)(last_dot - path.data));
    core_string_t member = core_string_borrow(last_dot + 1,
        path.byte_len - (size_t)(last_dot + 1 - path.data));
    core_translation_unit_t *target = nullptr;
    if (module_path_matches(module, unit->module.identifier)) target = unit;
    for (size_t i = 0; !target
         && i < core_translation_unit_import_count(unit); i++) {
        core_translation_unit_t *imported = core_translation_unit_import(unit,
                                                                          i);
        if (module_path_matches(module, imported->module.identifier))
            target = imported;
    }
    parse_sym_entry_t *symbol = nullptr;
    core_list_t(parse_scope_ptr_t) scopes =
        module_scopes(target ? tu_state(target) : nullptr);
    for (size_t i = 0; !symbol && i < scopes.len; i++)
        symbol = parse_symtab_lookup_in(scopes.data[i], member);
    core_list_free(scopes);
    core_buffer_release(&path);
    return symbol && symbol->kind == PARSE_SYM_FUNCTION ? symbol : nullptr;
}

static parse_sym_entry_t *
bound_function_symbol(parse_grammar_t *grammar, parse_symtab_t *symtab,
                      parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    parse_sym_entry_t *symbol = core_tree_node_value(node).sym;
    if (symbol && symbol->kind == PARSE_SYM_FUNCTION) return symbol;
    if (nt_is(grammar, node, "primary")) {
        parse_nt_node_t *primary = &core_tree_node_value(node);
        core_string_t name = parse_tree_extract_first_identifier(node);
        if (primary->scope && name.data) {
            symbol = parse_symtab_lookup_scoped(symtab, primary->scope,
                                                CORE_STRING_STATIC(""), name);
            if (symbol && symbol->kind == PARSE_SYM_FUNCTION) return symbol;
        }
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
    parse_sym_entry_t *symbol = qualified_call_symbol(grammar, unit, callee);
    if (symbol || member_call) return symbol;
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->annotations || !state->annotations->symtab)
        return nullptr;
    symbol = bound_function_symbol(grammar, state->annotations->symtab,
                                   callee);
    if (symbol) return symbol;
    core_string_t name = parse_tree_extract_first_identifier(callee);
    core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
    for (size_t i = 0; !symbol && name.data && i < scopes.len; i++)
        symbol = parse_symtab_lookup_in(scopes.data[i], name);
    core_list_free(scopes);
    return symbol && symbol->kind == PARSE_SYM_FUNCTION ? symbol : nullptr;
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
                if (method_status == TH_ENV_METHOD_FOUND)
                    signature = method.signature;
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
collect_positional_values(parse_grammar_t *grammar, parse_tree_t *node,
                          ir_lower_context_t *context,
                          core_list_t(n00bb_ir_value_ptr_t) *values)
{
    if (!node || core_tree_is_leaf(node)) return true;
    if (nt_is(grammar, node, "expression")) {
        ir_value_t *value = ir_lower_node_value(context, node);
        if (!value) return false;
        core_list_push(*values, value);
        return true;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!collect_positional_values(grammar, core_tree_child(node, i),
                                       context, values))
            return false;
    return true;
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
    parse_tree_t *callee = core_tree_num_children(visit->node)
        ? core_tree_child(visit->node, 0) : nullptr;
    n00bb_lower_context_t *lower = context->user;
    parse_sym_entry_t *symbol = callee && lower
        ? call_function_symbol(visit->grammar, lower->unit, callee,
            member_access_node(visit->grammar, callee) != nullptr)
        : nullptr;
    if (!symbol) {
        core_buffer_t path = {0};
        append_node_tokens(callee, &path);
        core_buffer_t message = {0};
        core_buffer_printf(&message,
                           "call target %s did not resolve to a declared function",
                           path.data ? path.data : "<unknown>");
        ir_diag_t *diagnostic = ir_diag_begin(context->diagnostics,
            IR_DIAG_UNDEFINED_SYMBOL, span, nullptr);
        ir_diag_message(diagnostic, context->diagnostics, message.data);
        core_buffer_release(&message);
        core_buffer_release(&path);
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
    th_type *signature = th_resolve(callee_value->type);
    if (!signature || th_kind_of(signature) != TH_FUNCTION
        || th_func_rest(signature))
        return lower_error(context, IR_DIAG_TYPE_CLASS, span,
                           "only non-variadic function calls are lowered here");
    parse_tree_t *args = descendant_nt(visit->grammar, visit->node,
                                       "call-args");
    if (args && descendant_nt(visit->grammar, args, "keyword-args"))
        return lower_error(context, IR_DIAG_VERIFY_PARSED, span,
                           "keyword arguments are not lowered yet");
    core_list_t(n00bb_ir_value_ptr_t) values =
        core_list_new(n00bb_ir_value_ptr_t);
    parse_tree_t *positional = args
        ? descendant_nt(visit->grammar, args, "positional-args") : nullptr;
    bool collected = !positional || collect_positional_values(
        visit->grammar, positional, context, &values);
    if (!collected || values.len != th_func_nparams(signature)) {
        core_list_free(values);
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                           "call arguments do not match the lowered function signature");
    }
    for (size_t i = 0; i < values.len; i++) {
        const th_param *parameter = th_func_param(signature, i);
        if (!parameter || parameter->mode != TH_IN) {
            core_list_free(values);
            return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                               "only in parameters are supported by this call lowering");
        }
    }
    ir_instr_t *instruction = ir_instr_add(context->block, IR_OP_CALL);
    char name[48];
    do {
        snprintf(name, sizeof(name), "__n00bb_%llu",
                 (unsigned long long)context->next_value_id++);
    } while (ir_value_find(context->function, name));
    bool emitted = instruction
        && ir_instr_set_dest(instruction, name, result_type)
        && ir_instr_operand_value(instruction, callee_value);
    for (size_t i = 0; emitted && i < values.len; i++)
        emitted = ir_instr_operand_value(instruction, values.data[i]);
    core_list_free(values);
    if (!emitted)
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit the function call");
    instruction->span = span;
    return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
}

static bool
n00bb_reject_unlowered(ir_lower_context_t *context, parse_visit_t *visit,
                       bool entering, void *state)
{
    (void)state;
    if (!entering) return true;
    bool unsupported = visit->annot->kind == PARSE_ANNOT_BRANCH
        || visit->annot->kind == PARSE_ANNOT_LOOP
        || visit->annot->kind == PARSE_ANNOT_SWITCH
        || visit->annot->kind == PARSE_ANNOT_ASSIGNS
        || visit->annot->kind == PARSE_ANNOT_OPERATOR;
    if (visit->annot->kind == PARSE_ANNOT_CALL) {
        parse_annotation_t *emit = visit->rule
            ? parse_rule_find_annot(visit->grammar, visit->rule,
                                    PARSE_ANNOT_EMIT) : nullptr;
        unsupported = !emit || !core_string_eq(
            emit->emit_action, CORE_STRING_STATIC("call"));
    }
    if (visit->annot->kind == PARSE_ANNOT_JUMP) {
        core_string_t name = parse_tree_nt_name(visit->grammar, visit->node);
        unsupported = !core_string_eq(name, CORE_STRING_STATIC("return-stmt"));
    }
    if (!unsupported) return true;
    ir_span_t span = IR_SPAN_UNKNOWN;
    parse_token_info_t *token = parse_tree_find_first_terminal(visit->node);
    if (token) span = ir_span_make(context->file_id,
        (uint32_t)token->start_byte, (uint32_t)token->end_byte,
        token->line, token->column, token->endline, token->endcol);
    ir_diag_t *diagnostic = ir_diag_begin(context->diagnostics,
        IR_DIAG_VERIFY_PARSED, span, "this source construct");
    ir_diag_message(diagnostic, context->diagnostics,
                    "construct is outside the current lowering subset");
    return false;
}

static const parse_sym_entry_t *
function_symbol_at(parse_grammar_t *grammar, parse_tree_t *node)
{
    return node && nt_is(grammar, node, "func-def")
        ? core_tree_node_value(node).sym : nullptr;
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

static bool
n00bb_nil_type(th_ctx *types, th_type *type)
{
    return type && th_hash(types, th_resolve(type))
        == th_hash(types, th_name(types, "nil"));
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
                if (!ir_func_add(state->ir_module, symbol->name.data,
                                 symbol->type, linkage)) {
                    core_list_free(scopes);
                    return false;
                }
            }
        }
        core_list_free(scopes);
    }
    return true;
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
    if (!state || !body || !function || !symbol->type) return false;

    ir_diags_t *diagnostics = ir_diags_new_attached(
        ir_context, nullptr, core_translation_unit_diagnostics(unit), unit);
    if (!diagnostics) return false;
    if (!n00bb_nil_type(state->types, symbol->error_type)) {
        ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
            IR_DIAG_VERIFY_TYPED, IR_SPAN_UNKNOWN, "this function");
        ir_diag_message(diagnostic, diagnostics,
                        "the bootstrap IR slice does not lower failure return channels yet");
        return false;
    }

    core_list_t(parse_sym_entry_ptr_t) parameters =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *rest = nullptr;
    parse_tree_t *signature = child_nt(grammar, declaration,
                                       "func-type-signature");
    collect_ir_parameters(grammar, signature, &parameters, &rest);
    if (rest || parameters.len != th_func_nparams(symbol->type)) {
        ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
            IR_DIAG_VERIFY_TYPED, IR_SPAN_UNKNOWN, "these parameters");
        ir_diag_message(diagnostic, diagnostics,
                        "variadic and unmatched parameter forms are not lowered yet");
        core_list_free(parameters);
        return false;
    }

    ir_block_t *entry = ir_block_add(function, "entry");
    if (!entry) {
        core_list_free(parameters);
        return false;
    }
    int32_t file_id = ir_file_count(state->ir_module) - 1;
    n00bb_lower_context_t lower_data = {
        .units = units,
        .unit = unit,
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
    for (size_t i = 0; i < parameters.len; i++) {
        parse_sym_entry_t *parameter = parameters.data[i];
        const th_param *type_param = th_func_param(symbol->type, i);
        if (!parameter || !type_param || !parameter->name.data
            || type_param->mode != TH_IN || parameter->is_optional) {
            ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
                IR_DIAG_VERIFY_TYPED, IR_SPAN_UNKNOWN, "this parameter");
            ir_diag_message(diagnostic, diagnostics,
                            "only required in parameters are lowered by the bootstrap slice");
            core_list_free(parameters);
            return false;
        }
        ir_value_t *value = ir_param_add(function, parameter->name.data,
                                         type_param->type);
        if (!value) {
            core_list_free(parameters);
            return false;
        }
        if (!ir_lower_bind_value(&context, parameter, value)) {
            core_list_free(parameters);
            return false;
        }
    }
    core_list_free(parameters);

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
                return false;
            }
        }
    }
    core_list_free(function_scopes);

    ir_lower_result_t lowered = ir_lower_tree(lowerer, &context, grammar, body);
    if (lowered.status == IR_LOWER_FAILED || ir_diags_failed(diagnostics)) {
        return false;
    }
    if (context.reachable) {
        if (!n00bb_nil_type(state->types, th_func_ret(symbol->type))) {
            ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
                IR_DIAG_VERIFY_TYPED, IR_SPAN_UNKNOWN, "this function body");
            ir_diag_message(diagnostic, diagnostics,
                            "this non-void function can reach its end without returning a value");
            return false;
        }
        ir_instr_t *ret = ir_instr_add(entry, IR_OP_RETURN);
        if (!ret) {
            return false;
        }
        ret->span = ir_module_span(state->ir_module);
    }
    return !ir_diags_failed(diagnostics);
}

static bool
lower_requests_to_ir(parse_grammar_t *grammar, core_work_queue_t *queue,
                     core_list_t(locate_request_t) *requests,
                     ir_ctx_t *ir_context)
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
    ir_lowerer_t *lowerer = ir_lowerer_new(ir_context);
    ok = ok && lowerer
        && ir_lowerer_register(lowerer, "binary", n00bb_lower_binary,
                               nullptr)
        && ir_lowerer_register(lowerer, "call", n00bb_lower_call, nullptr);
    static const parse_annot_kind_t rejected_kinds[] = {
        PARSE_ANNOT_BRANCH, PARSE_ANNOT_LOOP, PARSE_ANNOT_SWITCH,
        PARSE_ANNOT_CALL, PARSE_ANNOT_ASSIGNS, PARSE_ANNOT_OPERATOR,
        PARSE_ANNOT_JUMP,
    };
    for (size_t i = 0; ok && i < sizeof(rejected_kinds)
                                      / sizeof(rejected_kinds[0]); i++)
        ok = ir_lowerer_register_visit(lowerer, rejected_kinds[i],
                                       n00bb_reject_unlowered, nullptr);

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
        ir_module_establish(state->ir_module, IR_PROP_PARSED | IR_PROP_TYPED);
        ir_diags_t *diagnostics = ir_diags_new_attached(
            ir_context, nullptr, core_translation_unit_diagnostics(unit), unit);
        ir_props_t properties = ir_verify(state->ir_module, diagnostics);
        ok = !ir_diags_failed(diagnostics)
            && (properties & IR_PROP_PARSED)
            && ir_structure(state->ir_module, diagnostics)
            && !ir_diags_failed(diagnostics);
        for (size_t f = 0; ok && f < ir_func_count(state->ir_module); f++) {
            ir_func_t *function = ir_func_at(state->ir_module, f);
            if (function->n_blocks && !ir_pdg_of(function)) ok = false;
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
            char *text = ir_module_print(tu_state(units.data[i])->ir_module);
            if (!text) { ok = false; break; }
            fputs(text, stdout);
            core_free(text);
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
register_builtin_literal_traits(th_env *environment, th_ctx *types)
{
    th_type *methods = th_record(types, nullptr, 0);
    if (!methods ||
        !th_env_register_trait_contracts(environment, "Bracket_Literal",
            "builtins", "default", TH_ENV_PUBLIC, methods) ||
        !th_env_register_trait_contracts(environment, "Brace_Literal",
            "builtins", "default", TH_ENV_PUBLIC, methods))
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
        register_builtin_literal_traits(environment, root_state->types);
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
                    "trait constraint '%s' is not satisfied for %s",
                    detail.constraint,
                    detail.got ? th_print(state->types, detail.got)
                               : "this type");
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
    if (entering) {
        parse_tree_walk_apply_node(state->annotation_walk, node, parent,
            depth, child_index, kinds,
            sizeof(kinds) / sizeof(kinds[0]), true);
        parse_tree_walk_apply_node(state->reference_walk, node, parent,
            depth, child_index, reference_kinds,
            sizeof(reference_kinds) / sizeof(reference_kinds[0]), true);
    }
    else {
        parse_tree_walk_apply_node(state->reference_walk, node, parent,
            depth, child_index, reference_kinds,
            sizeof(reference_kinds) / sizeof(reference_kinds[0]), false);
        parse_tree_walk_apply_node(state->annotation_walk, node, parent,
            depth, child_index, kinds,
            sizeof(kinds) / sizeof(kinds[0]), false);
    }
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
    if (located && lower_command)
        located = lower_requests_to_ir(grammar, queue, &requests, ir_context);
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
