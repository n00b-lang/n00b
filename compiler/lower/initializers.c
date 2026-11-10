#include "lower_internal.h"
#include "comptime.h"

static void
collect_dynamic_sym_declarations(parse_grammar_t *grammar,
                                 n00bb_tu_state_t *state, parse_tree_t *node,
                                 core_list_t(n00bb_tree_ptr_t) *roots);
static void
collect_dynamic_global_initializers(parse_grammar_t *grammar,
                                    core_translation_unit_t *unit,
                                    parse_tree_t *node,
                                    core_list_t(n00bb_tree_ptr_t) *roots);
static bool
collect_one_module_initializer_root(parse_grammar_t *grammar,
                                    core_translation_unit_t *unit,
                                    parse_tree_t *statement,
                                    core_list_t(n00bb_tree_ptr_t) *roots);
static bool
collect_module_initializer_roots(parse_grammar_t *grammar,
                                 core_translation_unit_t *unit,
                                 core_list_t(n00bb_tree_ptr_t) *roots);
static ir_func_t *
add_module_initializer(ir_ctx_t *ir_context, core_translation_unit_t *unit,
                       core_string_t name);
static bool
lower_initializer_body(ir_lowerer_t *lowerer, parse_grammar_t *grammar,
                       core_translation_unit_t *unit,
                       core_list_t(core_translation_unit_ptr_t) *units,
                       ir_ctx_t *ir_context, ir_func_t *function,
                       core_list_t(n00bb_tree_ptr_t) *roots);
static bool
lower_module_initializer(ir_lowerer_t *lowerer,
                         parse_grammar_t *grammar,
                         core_translation_unit_t *unit,
                         core_list_t(core_translation_unit_ptr_t) *units,
                         ir_ctx_t *ir_context);
static ir_func_t *
comptime_start_function(core_translation_unit_t *unit, bool *ok);
static ir_func_t *
comptime_native(ir_ctx_t *ir_context, ir_module_t *module, core_string_t name,
                core_string_t result);
static bool
add_comptime_transition(ir_ctx_t *ir_context, core_translation_unit_t *unit,
                        ir_func_t *function);
static bool
collect_phase_reachable_function(ir_func_t *function, void *raw);
static ir_func_t *
reachable_function_body(ir_func_t *function, void *raw);
static bool
walk_reachable_functions(const n00bb_link_analysis_t *analysis,
                         core_list_t(n00bb_ir_func_ptr_t) *roots,
                         core_list_t(n00bb_ir_func_ptr_t) *reachable);
static bool
function_has_body(ir_func_t *function);
static void
suggest_program_function(core_diagnostics_t *diagnostics, core_string_t name,
    core_list_t(core_translation_unit_ptr_t) *initializer_order);
static bool
polymorphic_backup_lowerable(ir_module_t *module, ir_func_t *function);
static bool
attach_cached_initializers(n00bb_link_analysis_t *analysis,
                           core_translation_unit_t *unit,
                           ir_ctx_t *ir_context);
static bool
build_emitted_functions(n00bb_link_analysis_t *analysis);

static ir_global_t *
declared_global(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                parse_tree_t *node, parse_sym_entry_t **symbol)
{
    *symbol = nt_is(grammar, node, CORE_STRING_STATIC("sym-decl"))
            && child_nt(grammar, node, CORE_STRING_STATIC("expression"))
            && state && state->ir_module
        ? parse_tree_sym(node) : nullptr;
    return *symbol ? link_ir_global_for_symbol(state->link_analysis, *symbol)
                   : nullptr;
}

bool
n00bb_static_declaration(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                         parse_tree_t *node)
{
    parse_sym_entry_t *symbol = nullptr;
    ir_global_t *global = declared_global(grammar, state, node, &symbol);
    return global && global->has_static_initializer
        && symbol->mutability == PARSE_SYM_MUTABLE
        && !symbol->is_thread_local;
}

static void
collect_dynamic_sym_declarations(parse_grammar_t *grammar,
                                 n00bb_tu_state_t *state, parse_tree_t *node,
                                 core_list_t(n00bb_tree_ptr_t) *roots)
{
    if (nt_is(grammar, node, CORE_STRING_STATIC("sym-decl"))) {
        parse_sym_entry_t *symbol = nullptr;
        ir_global_t *global = declared_global(grammar, state, node, &symbol);
        if (global && (!global->has_static_initializer
                       || n00bb_static_declaration(grammar, state, node)))
            core_list_push(*roots, node);
        return;
    }
    if (!nt_is(grammar, node, CORE_STRING_STATIC("sym-decl-list"))) return;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (nt_is(grammar, child, CORE_STRING_STATIC("sym-decl"))
            || nt_is(grammar, child, CORE_STRING_STATIC("sym-decl-list")))
            collect_dynamic_sym_declarations(grammar, state, child, roots);
    }
}

static void
collect_dynamic_global_initializers(parse_grammar_t *grammar,
                                    core_translation_unit_t *unit,
                                    parse_tree_t *node,
                                    core_list_t(n00bb_tree_ptr_t) *roots)
{
    if (!nt_is(grammar, node, CORE_STRING_STATIC("variable-decl"))) return;
    if (child_nt(grammar, node, CORE_STRING_STATIC("binding-pattern"))) {
        core_list_push(*roots, node);
        return;
    }
    parse_tree_t *list = child_nt(grammar, node, CORE_STRING_STATIC("sym-decl-list"));
    collect_dynamic_sym_declarations(grammar, tu_state(unit), list, roots);
}

static bool
collect_one_module_initializer_root(parse_grammar_t *grammar,
                                    core_translation_unit_t *unit,
                                    parse_tree_t *statement,
                                    core_list_t(n00bb_tree_ptr_t) *roots)
{
    if (!statement || module_initializer_declaration(grammar, statement))
        return true;
    if (nt_is(grammar, statement, CORE_STRING_STATIC("variable-decl"))) {
        collect_dynamic_global_initializers(grammar, unit, statement, roots);
        return true;
    }
    core_list_push(*roots, statement);
    return true;
}

static bool
collect_module_initializer_roots(parse_grammar_t *grammar,
                                 core_translation_unit_t *unit,
                                 core_list_t(n00bb_tree_ptr_t) *roots)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state) return false;
    for (size_t i = 0; i < state->module_top_level_statements.len; i++)
        if (!collect_one_module_initializer_root(grammar, unit,
                state->module_top_level_statements.data[i], roots))
            return false;
    return true;
}

static ir_func_t *
add_module_initializer(ir_ctx_t *ir_context, core_translation_unit_t *unit,
                       core_string_t name)
{
    n00bb_tu_state_t *state = tu_state(unit);
    ir_module_t *module = unit_module(state);
    if (!module) return nullptr;
    if (ir_func_find(module, name)) {
        n00bb_internal_error(core_translation_unit_diagnostics(unit), unit,
            (core_diag_location_t){0}, "creating module initializers",
            "a declaration already uses the initializer name `%.*s`",
            (int)name.u8_bytes, name.data);
        return nullptr;
    }
    th_type *nil_type = n00bb_ir_builtin_type(module, CORE_STRING_STATIC("nil"));
    th_type *type = nil_type
        ? th_func_with_error(state->types, nil_type, nil_type,
                             nullptr, 0, nullptr, nullptr, 0)
        : nullptr;
    ir_func_t *function = type
        ? ir_func_add(module, name, type, state->interface_module
            ? IR_LINK_EXTERN : IR_LINK_INTERNAL) : nullptr;
    bool annotated = function
        && ir_func_set_initializer(ir_context, function, true);
    return annotated ? function : nullptr;
}

static bool
lower_initializer_body(ir_lowerer_t *lowerer, parse_grammar_t *grammar,
                       core_translation_unit_t *unit,
                       core_list_t(core_translation_unit_ptr_t) *units,
                       ir_ctx_t *ir_context, ir_func_t *function,
                       core_list_t(n00bb_tree_ptr_t) *roots)
{
    parse_tree_t *tree = n00bb_tu_parsed_tree(tu_state(unit));
    parse_sym_entry_t symbol = {
        .name = function->name,
        .kind = PARSE_SYM_FUNCTION,
        .type = function->type,
    };
    core_list_t(parse_sym_entry_ptr_t) parameters =
        core_list_new(parse_sym_entry_ptr_t);
    bool ok = tree && lower_ir_body(lowerer, grammar, unit, &symbol,
        tree, function, &parameters, roots, nullptr,
        false, ir_context, units, nullptr, nullptr);
    core_list_free(parameters);
    return ok;
}

bool
n00bb_const_initializer(const ir_func_t *function)
{
    return function && core_string_starts_with(function->name,
        CORE_STRING_STATIC(N00BB_CONST_INIT_NAME));
}

static bool
declares_const(parse_tree_t *node)
{
    parse_sym_entry_t *symbol = node ? parse_tree_sym(node) : nullptr;
    if (symbol && symbol->decl_node == node)
        return symbol->mutability == PARSE_SYM_CONST;
    for (size_t i = 0; node && !core_tree_is_leaf(node)
         && i < core_tree_num_children(node); i++)
        if (declares_const(core_tree_child(node, i))) return true;
    return false;
}

static bool
const_root(parse_grammar_t *grammar, parse_tree_t *root)
{
    parse_sym_entry_t *symbol = nt_is(grammar, root,
                                      CORE_STRING_STATIC("sym-decl"))
        ? parse_tree_sym(root) : nullptr;
    if (symbol) return symbol->mutability == PARSE_SYM_CONST;
    return nt_is(grammar, root, CORE_STRING_STATIC("variable-decl"))
        && declares_const(child_nt(grammar, root,
                                   CORE_STRING_STATIC("binding-pattern")));
}

static bool
add_comptime_prologue(ir_ctx_t *ir_context, ir_func_t *function,
                      ir_func_t *constants)
{
    ir_module_t *module = function->owner;
    th_type *bool_type = n00bb_ir_builtin_type(module,
                                               CORE_STRING_STATIC("bool"));
    th_type *nil_type = n00bb_ir_builtin_type(module,
                                              CORE_STRING_STATIC("nil"));
    th_type *key_type = n00bb_ir_builtin_type(module,
                                              CORE_STRING_STATIC("u64"));
    ir_value_t *flag = bool_type ? n00bb_comptime_flag(module, bool_type)
                                 : nullptr;
    ir_func_t *frozen = comptime_native(ir_context, module,
        CORE_STRING_STATIC(IR_COMPTIME_FROZEN),
        CORE_STRING_STATIC("bool"));
    ir_block_t *entry = ir_block_entry(function);
    ir_instr_t *first = entry && entry->n_instrs ? entry->instrs[0] : nullptr;
    if (!flag || !nil_type || !key_type || !frozen || !first) return false;
    ir_span_t span = function->span;
    core_string_t pass = CORE_STRING_STATIC("n00bb.comptime");
    ir_block_t *body = ir_block_split(entry, 0,
                                      CORE_STRING_STATIC("comptime.body"));
    ir_block_t *check = body ? ir_block_add(function,
        CORE_STRING_STATIC("comptime.check")) : nullptr;
    ir_block_t *skip = check ? ir_block_add(function,
        CORE_STRING_STATIC("comptime.frozen")) : nullptr;
    ir_block_t *run = skip ? ir_block_add(function,
        CORE_STRING_STATIC("comptime.constants")) : nullptr;
    ir_instr_t *load = run ? ir_instr_generated(entry, IR_OP_LOAD, span, pass,
                                                entry->n_instrs) : nullptr;
    if (!load || !ir_instr_set_fresh_dest(load, "comptime", bool_type)
        || !ir_instr_operand_value(load, flag)
        || !ir_annot_set(ir_context, &load->annots, (ir_annot_t){
               .key = CORE_STRING_STATIC(N00BB_COMPTIME_TRANSITION_KEY),
               .kind = IR_ANNOT_BOOL, .b = true})) return false;
    ir_instr_t *enter = ir_instr_generated(entry, IR_OP_BRANCH, span, pass,
                                           entry->n_instrs);
    if (!enter || !ir_instr_operand_value(enter, load->dest)
        || !ir_instr_operand_block(enter, check)
        || !ir_instr_operand_block(enter, body)) return false;
    ir_instr_t *key = ir_instr_generated(check, IR_OP_CONST, span, pass, 0);
    ir_instr_t *ask = key ? ir_instr_generated(check, IR_OP_CALL, span, pass,
                                               1) : nullptr;
    ir_instr_t *decide = ask ? ir_instr_generated(check, IR_OP_BRANCH, span,
                                                  pass, 2) : nullptr;
    if (!decide || !ir_instr_set_fresh_dest(key, "comptime", key_type)
        || !ir_instr_operand_uint(key,
               n00bb_comptime_module_key(ir_module_name(module)))
        || !ir_instr_set_fresh_dest(ask, "comptime", bool_type)
        || !ir_instr_operand_value(ask, ir_func_address(module, frozen))
        || !ir_instr_operand_value(ask, key->dest)
        || !ir_instr_operand_value(decide, ask->dest)
        || !ir_instr_operand_block(decide, skip)
        || !ir_instr_operand_block(decide, run)) return false;
    ir_func_t *freeze = comptime_native(ir_context, module,
        CORE_STRING_STATIC(N00BB_COMPTIME_FREEZE_NATIVE),
        CORE_STRING_STATIC("nil"));
    ir_instr_t *settle = freeze ? ir_instr_generated(skip, IR_OP_CALL, span,
                                                     pass, 0) : nullptr;
    if (!settle || !ir_instr_set_fresh_dest(settle, "comptime", nil_type)
        || !ir_instr_operand_value(settle, ir_func_address(module, freeze))
        || !ir_instr_operand_value(settle, key->dest)
        || !ir_instr_generated(skip, IR_OP_RETURN, span, pass, 1))
        return false;
    if (constants) {
        ir_instr_t *call = ir_instr_generated(run, IR_OP_CALL, span, pass,
                                              run->n_instrs);
        if (!call || !ir_instr_set_fresh_dest(call, "constants", nil_type)
            || !ir_instr_operand_value(call,
                   ir_func_address(module, constants))) return false;
    }
    ir_instr_t *jump = ir_instr_generated(run, IR_OP_JUMP, span, pass,
                                          run->n_instrs);
    return jump && ir_instr_operand_block(jump, body);
}

static bool
lower_module_initializer(ir_lowerer_t *lowerer,
                         parse_grammar_t *grammar,
                         core_translation_unit_t *unit,
                         core_list_t(core_translation_unit_ptr_t) *units,
                         ir_ctx_t *ir_context)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!n00bb_tu_parsed_tree(state)) return false;
    ir_func_t *function = add_module_initializer(ir_context, unit,
        CORE_STRING_STATIC(N00BB_MODULE_INIT_NAME));
    if (!function) return false;
    if (state->interface_module) return true;
    core_list_t(n00bb_tree_ptr_t) roots = core_list_new(n00bb_tree_ptr_t);
    core_dict_t deferred;
    core_dict_init(&deferred, nullptr, nullptr);
    bool ok = collect_module_initializer_roots(grammar, unit, &roots);
    for (size_t i = 0; ok && i < roots.len; i++) {
        if (!const_root(grammar, roots.data[i])) continue;
        char buffer[64];
        core_string_t name = n00bb_format_name(buffer, sizeof(buffer),
            "%s.%zu", N00BB_CONST_INIT_NAME, i);
        ir_func_t *constant = add_module_initializer(ir_context, unit, name);
        core_list_t(n00bb_tree_ptr_t) single = core_list_new(n00bb_tree_ptr_t);
        core_list_push(single, roots.data[i]);
        ok = constant && ir_func_set_initializer(ir_context, constant, false)
            && lower_initializer_body(lowerer, grammar, unit, units,
                                      ir_context, constant, &single);
        core_list_free(single);
        if (ok) core_dict_put(&deferred, roots.data[i], constant);
    }
    state->deferred_consts = &deferred;
    ok = ok && lower_initializer_body(lowerer, grammar, unit, units,
                                      ir_context, function, &roots);
    state->deferred_consts = nullptr;
    core_dict_free(&deferred);
    core_list_t(n00bb_tree_ptr_t) constants = core_list_new(n00bb_tree_ptr_t);
    for (size_t i = 0; ok && i < state->hoisted_literals.len; i++)
        core_list_push(constants, state->hoisted_literals.data[i].node);
    ir_func_t *initializer = ok && constants.len
        ? add_module_initializer(ir_context, unit,
              CORE_STRING_STATIC(N00BB_CONST_INIT_NAME))
        : nullptr;
    if (initializer && !ir_func_set_initializer(ir_context, initializer,
                                                false))
        initializer = nullptr;
    ok = ok && (!constants.len || (initializer
            && lower_initializer_body(lowerer, grammar, unit, units,
                                      ir_context, initializer, &constants)))
        && add_comptime_transition(ir_context, unit, function)
        && add_comptime_prologue(ir_context, function, initializer);
    core_list_free(roots);
    core_list_free(constants);
    return ok;
}

static ir_func_t *
comptime_start_function(core_translation_unit_t *unit, bool *ok)
{
    n00bb_tu_state_t *state = tu_state(unit);
    *ok = true;
    if (!state->link || unit != core_link_unit_root(state->link, 0))
        return nullptr;
    ir_module_t *module = unit_module(state);
    ir_func_t *function = ir_func_find(module,
        CORE_STRING_STATIC(N00BB_COMPTIME_START_NAME));
    if (!function) return nullptr;
    th_type *type = th_resolve(function->type);
    th_type *result = th_resolve(th_func_ret(type));
    if (!th_func_nparams(type)
        && (th_is_nil(result) || th_kind_of(result) == TH_VAR))
        return function;
    parse_scope_t *scope = module_scope(state);
    parse_sym_entry_t *symbol = scope ? parse_symtab_lookup_in(scope,
        CORE_STRING_STATIC(N00BB_COMPTIME_START_NAME)) : nullptr;
    core_diag_location_t location = symbol && symbol->decl_node
        ? link_source_location(symbol->decl_node)
        : (core_diag_location_t){0};
    core_diagnostics_t *sink = core_translation_unit_diagnostics(unit);
    core_string_t found = n00bb_print_type(ir_module_types(module), type);
    n00bb_report(sink, unit, N00B_DIAG_COMPTIME_START_SIGNATURE, location,
        N00B_TEXT_COMPTIME_START_MUST_TAKE_NO, N00BB_ARGS(ARG_TEXT(found)));
    n00bb_report_help(sink, N00B_TEXT_DECLARE_IT_AS_FUNC_COMPTIME_START,
        N00BB_NO_ARGS);
    *ok = false;
    return nullptr;
}

static ir_func_t *
comptime_native(ir_ctx_t *ir_context, ir_module_t *module, core_string_t name,
                core_string_t result)
{
    ir_func_t *function = ir_func_find(module, name);
    if (function) return function;
    th_ctx *types = ir_module_types(module);
    th_type *nil_type = n00bb_ir_builtin_type(module,
                                              CORE_STRING_STATIC("nil"));
    th_type *result_type = n00bb_ir_builtin_type(module, result);
    th_param key = {.mode = TH_IN, .type = n00bb_ir_builtin_type(module,
                                               CORE_STRING_STATIC("u64"))};
    th_type *type = nil_type && result_type && key.type
        ? th_func_with_error(types, result_type, nil_type, &key, 1, nullptr,
                             nullptr, 0)
        : nullptr;
    function = type ? ir_func_add(module, name, type, IR_LINK_EXTERN)
                    : nullptr;
    bool annotated = function && ir_annot_set(ir_context, &function->annots,
        (ir_annot_t){.key = CORE_STRING_STATIC("n00bb.abi.native"),
                     .kind = IR_ANNOT_STR, .s = name});
    return annotated ? function : nullptr;
}

static bool
add_comptime_transition(ir_ctx_t *ir_context, core_translation_unit_t *unit,
                        ir_func_t *function)
{
    bool ok = true;
    ir_func_t *start = comptime_start_function(unit, &ok);
    if (!ok) return false;
    ir_module_t *module = function->owner;
    th_type *bool_type = n00bb_ir_builtin_type(module,
                                               CORE_STRING_STATIC("bool"));
    th_type *nil_type = n00bb_ir_builtin_type(module,
                                              CORE_STRING_STATIC("nil"));
    ir_value_t *flag = bool_type ? n00bb_comptime_flag(module, bool_type)
                                 : nullptr;
    th_type *key_type = n00bb_ir_builtin_type(module,
                                              CORE_STRING_STATIC("u64"));
    ir_func_t *freeze = comptime_native(ir_context, module,
        CORE_STRING_STATIC(N00BB_COMPTIME_FREEZE_NATIVE),
        CORE_STRING_STATIC("nil"));
    if (!flag || !freeze || !nil_type || !key_type) return false;
    uint64_t key = n00bb_comptime_module_key(ir_module_name(module));
    core_string_t pass = CORE_STRING_STATIC("n00bb.comptime");
    uint32_t blocks = function->n_blocks;
    for (uint32_t i = 0; i < blocks; i++) {
        ir_block_t *block = function->blocks[i];
        ir_instr_t *exit = ir_block_terminator(block);
        if (!exit || exit->op != IR_OP_RETURN) continue;
        ir_span_t span = function->span;
        ir_block_t *done = ir_block_split(block, block->n_instrs - 1,
            CORE_STRING_STATIC("comptime.done"));
        ir_block_t *transition = done ? ir_block_add(function,
            CORE_STRING_STATIC("comptime.transition")) : nullptr;
        ir_instr_t *load = transition ? ir_instr_generated(block, IR_OP_LOAD,
            span, pass, block->n_instrs) : nullptr;
        if (!load || !ir_instr_set_fresh_dest(load, "comptime", bool_type)
            || !ir_instr_operand_value(load, flag)
            || !ir_annot_set(ir_context, &load->annots, (ir_annot_t){
                   .key = CORE_STRING_STATIC(N00BB_COMPTIME_TRANSITION_KEY),
                   .kind = IR_ANNOT_BOOL, .b = true})) return false;
        ir_instr_t *branch = ir_instr_generated(block, IR_OP_BRANCH, span,
            pass, block->n_instrs);
        if (!branch || !ir_instr_operand_value(branch, load->dest)
            || !ir_instr_operand_block(branch, transition)
            || !ir_instr_operand_block(branch, done)) return false;
        ir_instr_t *constant = ir_instr_generated(transition, IR_OP_CONST,
            span, pass, transition->n_instrs);
        if (!constant || !ir_instr_set_fresh_dest(constant, "comptime",
                                                  key_type)
            || !ir_instr_operand_uint(constant, key)) return false;
        ir_func_t *callees[] = {start, freeze};
        for (size_t c = 0; c < 2; c++) {
            if (!callees[c]) continue;
            ir_instr_t *call = ir_instr_generated(transition, IR_OP_CALL,
                span, pass, transition->n_instrs);
            if (!call || !ir_instr_set_fresh_dest(call, "comptime", nil_type)
                || !ir_instr_operand_value(call,
                       ir_func_address(module, callees[c]))
                || (callees[c] == freeze
                    && !ir_instr_operand_value(call, constant->dest)))
                return false;
        }
        ir_instr_t *jump = ir_instr_generated(transition, IR_OP_JUMP, span,
            pass, transition->n_instrs);
        if (!jump || !ir_instr_operand_block(jump, done)) return false;
    }
    return true;
}

static bool
collect_phase_reachable_function(ir_func_t *function, void *raw)
{
    reachable_collection_t *collection = raw;
    if (!n00bb_cache_owns(collection->cache, function->owner))
        core_list_push(*collection->functions, function);
    return true;
}

static ir_func_t *
reachable_function_body(ir_func_t *function, void *raw)
{
    (void)raw;
    return n00bb_cached_body(function);
}

static bool
walk_reachable_functions(const n00bb_link_analysis_t *analysis,
                         core_list_t(n00bb_ir_func_ptr_t) *roots,
                         core_list_t(n00bb_ir_func_ptr_t) *reachable)
{
    reachable->len = 0;
    reachable_collection_t collection = {reachable, analysis->cache};
    return ir_walk_reachable_functions(roots->data, roots->len,
        collect_phase_reachable_function, reachable_function_body,
        &collection);
}

ir_func_t *
n00bb_root_entry(const n00bb_link_analysis_t *analysis, core_string_t name)
{
    n00bb_tu_state_t *any = analysis && analysis->units.len
        ? tu_state(analysis->units.data[0]) : nullptr;
    core_translation_unit_t *root = any && any->link
        ? core_link_unit_root(any->link, 0) : nullptr;
    ir_module_t *module = root ? unit_module(tu_state(root)) : nullptr;
    ir_func_t *function = module ? ir_func_find(module, name) : nullptr;
    return function && function->n_blocks ? function : nullptr;
}

static bool
function_has_body(ir_func_t *function)
{
    return function->n_blocks
        || ir_annot_get(&function->annots, CORE_STRING_STATIC("n00bb.cached_body"));
}

static void
suggest_program_function(core_diagnostics_t *diagnostics, core_string_t name,
    core_list_t(core_translation_unit_ptr_t) *initializer_order)
{
    size_t dot = name.u8_bytes;
    while (dot > 0 && name.data[dot - 1] != '.') dot--;
    core_string_t wanted_module = dot ? core_string_slice(name, 0, dot - 1)
                                      : (core_string_t){};
    bool module_known = false;
    for (size_t i = 0; i < initializer_order->len && wanted_module.u8_bytes;
         i++) {
        ir_module_t *module = unit_module(tu_state(initializer_order->data[i]));
        module_known |= module
            && core_string_eq(ir_module_name(module), wanted_module);
    }
    core_list_t(core_string_t) candidates = core_list_new(core_string_t);
    for (size_t i = 0; i < initializer_order->len; i++) {
        ir_module_t *module = unit_module(tu_state(initializer_order->data[i]));
        core_string_t module_name = module ? ir_module_name(module)
                                           : (core_string_t){};
        if (!module_name.u8_bytes
            || (module_known && !core_string_eq(module_name, wanted_module)))
            continue;
        for (size_t j = 0; j < ir_func_count(module); j++) {
            ir_func_t *function = ir_func_at(module, j);
            if (!function || !function_has_body(function)
                || core_string_find_byte(function->name, '$') >= 0
                || core_string_starts_with(function->name,
                                           CORE_STRING_STATIC("__")))
                continue;
            core_buffer_t *qualified = core_buffer_empty();
            if (!module_known) {
                core_buffer_append_string(qualified, module_name);
                core_buffer_puts(qualified, ".");
            }
            core_buffer_append_string(qualified, function->name);
            core_list_push(candidates, core_buffer_take_string(qualified));
        }
    }
    core_string_t compared = module_known
        ? core_string_slice(name, dot, name.u8_bytes - dot) : name;
    n00bb_diag_did_you_mean(diagnostics, compared, candidates.data,
                            candidates.len);
    for (size_t i = 0; i < candidates.len; i++)
        core_free(candidates.data[i].data);
    core_list_free(candidates);
}

static bool
collect_program_roots(
    n00bb_link_analysis_t *analysis,
    core_list_t(core_translation_unit_ptr_t) *initializer_order,
    core_diagnostics_t *diagnostics,
    core_list_t(n00bb_ir_func_ptr_t) *out)
{
    core_list_t(n00bb_ir_func_ptr_t) roots =
        core_list_new(n00bb_ir_func_ptr_t);
    for (size_t i = 0; i < initializer_order->len; i++) {
        ir_module_t *module = unit_module(
            tu_state(initializer_order->data[i]));
        ir_func_t *initializer = module ? ir_func_find(module,
            CORE_STRING_STATIC(N00BB_MODULE_INIT_NAME)) : nullptr;
        if (initializer) core_list_push(roots, initializer);
    }
    ir_func_t *entry = n00bb_root_entry(analysis,
                                        CORE_STRING_STATIC(N00BB_START_NAME));
    if (entry) core_list_push(roots, entry);
    for (size_t i = 0; i < initializer_order->len; i++) {
        n00bb_tu_state_t *state = tu_state(initializer_order->data[i]);
        ir_module_t *module = unit_module(state);
        core_string_t module_name = module ? ir_module_name(module)
                                           : (core_string_t){};
        if (!module_name.data) continue;
        for (size_t j = 0; j < ir_func_count(module); j++) {
            ir_func_t *function = ir_func_at(module, j);
            if (!function || !function_has_body(function)
                || !th_is_closed(ir_module_types(module), function->type))
                continue;
            if (core_string_eq(function->name,
                               CORE_STRING_STATIC("__finalize"))
                || core_string_starts_with(function->name,
                                           CORE_STRING_STATIC("__finalize#")))
                core_list_push(roots, function);
        }
        size_t prefix = module_name.u8_bytes;
        size_t cursor = 0;
        const n00bb_runtime_native_root_t *native;
        while ((native = n00bb_native_root_in_module(module_name,
                                                     &cursor))) {
            ir_func_t *function = ir_func_find(module,
                n00bb_native_root_function(native, prefix));
            if (function && function_has_body(function))
                core_list_push(roots, function);
        }
    }
    for (size_t r = 0; r < analysis->native_root_count; r++) {
        bool found = false;
        for (size_t i = 0; i < initializer_order->len && !found; i++) {
            n00bb_tu_state_t *state = tu_state(initializer_order->data[i]);
            ir_module_t *module = unit_module(state);
            core_string_t owner = module ? ir_module_name(module)
                                         : (core_string_t){};
            if (!owner.data) continue;
            for (size_t j = 0; j < ir_func_count(module); j++) {
                ir_func_t *function = ir_func_at(module, j);
                if (!function || !function_has_body(function)
                    || !native_root_matches(analysis->native_roots[r],
                        owner, function->name))
                    continue;
                core_list_push(roots, function);
                found = true;
                break;
            }
        }
        if (!found) {
            n00bb_report(diagnostics, nullptr, N00B_DIAG_NATIVE_ROOT_MISSING,
                (core_diag_location_t){0}, N00B_TEXT_NATIVE_ROOT_NAMES_BUT_THE,
                N00BB_ARGS(ARG_TEXT(analysis->native_roots[r])));
            suggest_program_function(diagnostics, analysis->native_roots[r],
                                     initializer_order);
            core_list_free(roots);
            return false;
        }
    }
    for (size_t r = 0; r < n00bb_native_root_registered_count(); r++) {
        core_string_t name = core_string_from_cstr(
            n00bb_native_root_registered_at(r)->qualified_name);
        bool found = false;
        for (size_t i = 0; i < initializer_order->len && !found; i++) {
            ir_module_t *module = unit_module(
                tu_state(initializer_order->data[i]));
            core_string_t module_name = module ? ir_module_name(module)
                                               : (core_string_t){};
            size_t prefix = module_name.u8_bytes;
            ir_func_t *function = prefix
                && core_string_starts_with(name, module_name)
                && name.u8_bytes > prefix && name.data[prefix] == '.'
                ? ir_func_find(module, core_string_slice(name, prefix + 1,
                    name.u8_bytes - prefix - 1)) : nullptr;
            found = function && function_has_body(function);
        }
        if (!found) {
            n00bb_report(diagnostics, nullptr,
                N00B_DIAG_PROVIDED_FUNCTION_MISSING, (core_diag_location_t){0},
                N00B_TEXT_AN_FFI_BINDING_PROVIDES_BUT,
                N00BB_ARGS(ARG_TEXT(name)));
            suggest_program_function(diagnostics, name, initializer_order);
            core_free(name.data);
            core_list_free(roots);
            return false;
        }
        core_free(name.data);
    }
    *out = roots;
    return true;
}

bool
build_reachable_functions(
    n00bb_link_analysis_t *analysis,
    core_list_t(core_translation_unit_ptr_t) *initializer_order,
    core_diagnostics_t *diagnostics)
{
    core_list_t(n00bb_ir_func_ptr_t) roots = {0};
    if (!collect_program_roots(analysis, initializer_order, diagnostics,
                               &roots)) return false;
    bool ok = walk_reachable_functions(analysis, &roots,
                                       &analysis->reachable_functions);
    core_list_free(roots);
    return ok && build_emitted_functions(analysis);
}

static void collect_emission_roots(n00bb_link_analysis_t *analysis,
                                   core_list_t(n00bb_ir_func_ptr_t) *roots);

static bool
collect_any_function(ir_func_t *function, void *raw)
{
    core_list_t(n00bb_ir_func_ptr_t) *functions = raw;
    core_list_push(*functions, function);
    ir_func_t *body = n00bb_cached_body(function);
    if (body && body != function) core_list_push(*functions, body);
    return true;
}

bool
n00bb_runtime_functions(n00bb_link_analysis_t *analysis,
                        core_list_t(n00bb_ir_func_ptr_t) *out)
{
    core_list_t(n00bb_ir_func_ptr_t) roots = {0};
    if (!collect_program_roots(analysis, &analysis->initializer_order,
                               nullptr, &roots)) return false;
    collect_emission_roots(analysis, &roots);
    n00bb_comptime_function_roots(analysis, &roots);
    *out = core_list_new(n00bb_ir_func_ptr_t);
    bool ok = ir_walk_reachable_functions(roots.data, roots.len,
        collect_any_function, reachable_function_body, out);
    core_list_free(roots);
    return ok;
}

bool
n00bb_boxed_signature_position(ir_module_t *module, th_type *type)
{
    th_ctx *types = ir_module_types(module);
    th_type *resolved = th_resolve(type);
    th_kind kind = th_kind_of(resolved);
    if (kind == TH_VAR || kind == TH_TRAIT || kind == TH_SUM) return true;
    if (th_is_closed(types, resolved)) return false;
    if (kind == TH_SEQUENCE || kind == TH_INDIRECTION || kind == TH_RECORD)
        return true;
    bool variant = false;
    if (kind == TH_NAMED && th_named_info(resolved, nullptr, &variant,
                                          nullptr) && variant)
        return true;
    if (th_builtin_kind_of(resolved) == TH_BUILTIN_TUPLE) return true;
    th_type *definition = kind == TH_NAMED
        ? ir_type_definition(module, resolved) : nullptr;
    return definition && (th_kind_of(definition) == TH_RECORD
                          || th_kind_of(definition) == TH_INDIRECTION);
}

static bool
polymorphic_backup_lowerable(ir_module_t *module, ir_func_t *function)
{
    th_ctx *types = ir_module_types(module);
    th_type **variables = nullptr;
    size_t count = th_free_vars(types, function->type, &variables);
    th_type *result = th_func_ret(function->type);
    for (size_t v = 0; v < count; v++) {
        bool recovered = result
            && n00bb_boxed_signature_position(module, result)
            && th_occurs(types, variables[v], result);
        for (size_t p = 0; !recovered && p < th_func_nparams(function->type);
             p++) {
            const th_param *parameter = th_func_param(function->type, p);
            recovered = parameter && parameter->type
                && n00bb_boxed_signature_position(module, parameter->type)
                && th_occurs(types, variables[v], parameter->type);
        }
        if (!recovered) return false;
    }
    return true;
}

static void
collect_emission_roots(n00bb_link_analysis_t *analysis,
                       core_list_t(n00bb_ir_func_ptr_t) *roots)
{
    for (size_t i = 0; i < analysis->specializations.len; i++)
        core_list_push(*roots, analysis->specializations.data[i].instance);
    for (size_t i = 0; i < analysis->boxed_hooks.finalizers.len; i++)
        core_list_push(*roots,
            analysis->boxed_hooks.finalizers.data[i].function);
    for (size_t i = 0; i < analysis->boxed_hooks.addresses.len; i++)
        core_list_push(*roots,
            analysis->boxed_hooks.addresses.data[i].function);
    for (size_t i = 0; i < analysis->boxed_hooks.mutable_addresses.len; i++)
        core_list_push(*roots,
            analysis->boxed_hooks.mutable_addresses.data[i].function);
    for (size_t i = 0; i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        bool generated = n00bb_generated_source(
            core_source_file_path(&unit->source));
        bool root = state && state->link && !state->initializer_excluded
            && unit == core_link_unit_root(state->link, 0);
        ir_module_t *module = unit_module(state);
        for (size_t j = 0; module && j < ir_func_count(module); j++) {
            ir_func_t *function = ir_func_at(module, j);
            ir_annot_t *value_method = function ? ir_annot_get(
                &function->annots,
                CORE_STRING_STATIC("n00bb.generated-value-method")) : nullptr;
            if (function && function->n_blocks && value_method
                && th_is_closed(ir_module_types(module), function->type))
                core_list_push(*roots, function);
            else if (function && function->n_blocks && value_method
                && !analysis->closed_world
                && core_string_starts_with(function->name,
                       CORE_STRING_STATIC("__n00bb_dispatch_")))
                core_list_push(*roots, function);
        }
        if (generated || root) continue;
        for (size_t j = 0; module && j < ir_func_count(module); j++) {
            ir_func_t *function = ir_func_at(module, j);
            ir_func_t *body = function ? n00bb_cached_body(function) : nullptr;
            if (body && body->n_blocks
                && (th_is_closed(ir_module_types(body->owner), body->type)
                    || (!analysis->closed_world
                        && polymorphic_backup_lowerable(body->owner, body))))
                core_list_push(*roots, body);
        }
    }
}

static bool
build_emitted_functions(n00bb_link_analysis_t *analysis)
{
    core_list_t(n00bb_ir_func_ptr_t) roots =
        core_list_new(n00bb_ir_func_ptr_t);
    for (size_t i = 0; i < analysis->reachable_functions.len; i++)
        core_list_push(roots, analysis->reachable_functions.data[i]);
    collect_emission_roots(analysis, &roots);
    bool ok = walk_reachable_functions(analysis, &roots,
                                       &analysis->emitted_functions);
    core_list_free(roots);
    return ok;
}

void
runtime_global_final_state(
    n00bb_runtime_init_analysis_t *runtime,
    ir_func_t *const *roots, size_t root_count, ir_global_t *global,
    bool *phase_succeeds, bool *definitely_initialized,
    bool *has_successful_write)
{
    ir_splace_t place = ir_splace_global(global);
    uint8_t state = global->has_static_initializer ? IR_PATH_PASSED
                                                   : IR_PATH_UNPASSED;
    uint8_t write_state = IR_PATH_UNPASSED;
    *phase_succeeds = true;
    for (size_t i = 0; i < root_count; i++) {
        uint8_t effects = ir_summary_effect(
            ir_summary_of(runtime->summaries, roots[i]), &place,
            IR_PATH_SUCCESS);
        if (!effects) {
            *phase_succeeds = false;
            state = 0;
            break;
        }
        state = ir_path_apply(state, effects);
        write_state = ir_path_apply(write_state, effects);
    }
    *definitely_initialized = state && !(state & IR_PATH_UNPASSED);
    *has_successful_write = (write_state & IR_PATH_PASSED) != 0;
}

static bool
attach_cached_initializers(n00bb_link_analysis_t *analysis,
                           core_translation_unit_t *unit,
                           ir_ctx_t *ir_context)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state->interface_module) return true;
    ir_module_t *cached = n00bb_cache_module(analysis->cache,
                                             unit->module.identifier);
    ir_module_t *module = unit_module(state);
    core_string_t name = CORE_STRING_STATIC(N00BB_MODULE_INIT_NAME);
    ir_func_t *function = cached && module ? ir_func_find(module, name)
                                           : nullptr;
    ir_func_t *body = function ? ir_func_find(cached, name) : nullptr;
    if (function && !function->n_blocks && body && body->n_blocks
        && !attach_cached_body(ir_context, function, body)) return false;
    for (size_t i = 0; cached && module && i < ir_global_count(module); i++) {
        ir_global_t *global = ir_global_at(module, i);
        ir_global_t *original = global && !global->has_static_initializer
            ? ir_global_find(cached, global->name) : nullptr;
        if (original && original->has_static_initializer
            && original->static_initializer
            && !ir_global_adopt_static_initializer(module, global,
                   original->static_initializer)) return false;
    }
    return true;
}

bool
lower_all_module_initializers(
    n00bb_link_analysis_t *analysis, ir_lowerer_t *lowerer,
    parse_grammar_t *grammar,
    core_list_t(core_translation_unit_ptr_t) *units,
    ir_ctx_t *ir_context)
{
    bool lowered = true;
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        if (lower_module_initializer(lowerer, grammar, unit, units,
                                     ir_context)
            && attach_cached_initializers(analysis, unit, ir_context))
            continue;
        lowered = false;
        tu_state(unit)->initializer_excluded = true;
        ir_module_t *module = unit_module(tu_state(unit));
        ir_func_t *function = module ? ir_func_find(module,
            CORE_STRING_STATIC(N00BB_MODULE_INIT_NAME)) : nullptr;
        if (function && function->n_blocks)
            (void)exclude_function(analysis, unit, function);
    }
    return lowered;
}
