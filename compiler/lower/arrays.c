#include "lower_internal.h"

ir_instr_t *
n00bb_emit_checked_product(ir_lower_context_t *context, ir_value_t *left,
                           ir_value_t *right, th_type *uint_type,
                           ir_span_t span)
{
    ir_instr_t *product = ir_lower_emit_instruction(context, IR_OP_MUL, span);
    if (!product || !ir_lower_set_temp_dest(context, product,
                                            CORE_STRING_STATIC("__n00bb_array_product_"),
                                            uint_type)
        || !ir_instr_operand_value(product, left)
        || !ir_instr_operand_value(product, right)
        || !ir_instr_add_mod(product, CORE_STRING_STATIC("trap"))) return nullptr;
    return product;
}

ir_value_t *
n00bb_coerce_array_dimension_value(ir_lower_context_t *context,
                                   parse_tree_t *expression,
                                   ir_value_t *value, th_type *uint_type,
                                   ir_value_t *one, ir_span_t span,
                                   bool require_positive)
{
    (void)expression;
    th_type *source_type = value ? th_resolve(value->type) : nullptr;
    bool open = source_type
        && !th_is_closed(ir_module_types(context->module), source_type);
    if (!value || (!open && !is_integer_type(source_type))) return nullptr;
    ir_instr_t *cast = ir_lower_emit_instruction(context, IR_OP_CAST, span);
    if (open) n00bb_rewrite_later(context, cast);
    ir_machine_t source_machine = open ? (ir_machine_t){0}
        : ir_machine_of(context->module, source_type);
    ir_machine_t target_machine = ir_machine_of(context->module, uint_type);
    bool needs_fit = !open
        && (source_machine.width > target_machine.width
            || source_machine.class != target_machine.class);
    if (!cast || !ir_lower_set_temp_dest(context, cast,
                                         CORE_STRING_STATIC("__n00bb_array_extent_"), uint_type)
        || !ir_instr_operand_value(cast, value)
        || (needs_fit && !ir_instr_add_mod(cast, CORE_STRING_STATIC("trap")))) return nullptr;
    if (!require_positive) return cast->dest;
    ir_instr_t *check = ir_lower_emit_instruction(context, IR_OP_SUB, span);
    if (!check || !ir_lower_set_temp_dest(context, check,
                                          CORE_STRING_STATIC("__n00bb_array_positive_"),
                                          uint_type)
        || !ir_instr_operand_value(check, cast->dest)
        || !ir_instr_operand_value(check, one)
        || !ir_instr_add_mod(check, CORE_STRING_STATIC("trap"))) return nullptr;
    return cast->dest;
}

ir_value_t *
n00bb_lower_array_dimension_value(ir_lower_context_t *context,
                                  parse_tree_t *expression,
                                  th_type *uint_type, ir_value_t *one,
                                  ir_span_t span, bool require_positive)
{
    return n00bb_coerce_array_dimension_value(context, expression,
        ir_lower_node_value(context, expression), uint_type, one, span,
        require_positive);
}

#include "sequence_transfer.h"

static bool
static_extent(const ir_shape_t *shape, uint32_t dimension, uint64_t *length)
{
    ir_shape_extent_t extent;
    if (!shape || !ir_shape_extent(shape, dimension, &extent)
        || extent.kind != IR_SHAPE_EXTENT_STATIC) return false;
    *length = extent.static_value;
    return true;
}

static bool
static_array_extent(const ir_func_t *function, const ir_value_t *value,
                    uint32_t dimension, uint64_t *length, int depth);

static bool
declaration_default_write(const ir_instr_t *in)
{
    ir_annot_t *mark = ir_annot_get((ir_annots_t *)&in->annots,
        CORE_STRING_STATIC("ir.declaration_default"));
    return mark && mark->kind == IR_ANNOT_BOOL && mark->b;
}

// A local's length, when every whole assignment to it in `function` gives
// it the same one that's known: a literal's, or another known array's.
static bool
assigned_local_extent(const ir_func_t *function, const ir_local_t *local,
                      uint32_t dimension, uint64_t *length, int depth)
{
    if (static_extent(ir_local_shape(local), dimension, length)) return true;
    if (!function || dimension || depth > 16) return false;
    // If its address goes anywhere but to index, read or store it (to a
    // call, say, which could assign it), its assignments aren't all here.
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            const ir_instr_t *taken = function->blocks[b]->instrs[i];
            if (taken->op != IR_OP_ADDRESS_OF || !taken->n_operands
                || !taken->dest
                || ir_operand_local(&taken->operands[0]) != local) continue;
            for (uint32_t ub = 0; ub < function->n_blocks; ub++)
                for (uint32_t u = 0; u < function->blocks[ub]->n_instrs; u++) {
                    const ir_instr_t *user = function->blocks[ub]->instrs[u];
                    for (uint32_t o = 0; o < user->n_operands; o++) {
                        if (ir_operand_value(&user->operands[o])
                            != taken->dest) continue;
                        bool reaches = o == 0
                            && (user->op == IR_OP_INDEX
                                || user->op == IR_OP_LOAD
                                || user->op == IR_OP_STORE);
                        if (!reaches) return false;
                    }
                }
        }
    bool any = false;
    uint64_t found = 0;
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = function->blocks[b]->instrs[i];
            const ir_value_t *written = nullptr;
            if (in->op == IR_OP_COPY && in->dest_kind == IR_DEST_LOCAL
                && in->dest_local == local && in->n_operands)
                written = ir_operand_value(&in->operands[0]);
            else if (in->op == IR_OP_STORE && in->n_operands == 2) {
                const ir_value_t *address = ir_operand_value(&in->operands[0]);
                const ir_instr_t *taken = address
                        && address->kind == IR_VAL_INSTR ? address->def
                                                         : nullptr;
                if (taken && taken->op == IR_OP_ADDRESS_OF
                    && taken->n_operands
                    && ir_operand_local(&taken->operands[0]) == local)
                    written = ir_operand_value(&in->operands[1]);
            }
            else continue;
            if (!written || declaration_default_write(in)) continue;
            uint64_t got = 0;
            if (!static_array_extent(function, written, 0, &got, depth + 1)
                || (any && got != found)) return false;
            found = got;
            any = true;
        }
    if (any) *length = found;
    return any;
}

// A module array's length: its declared size, or the length of the static
// value an immutable one starts with.
static bool
global_extent(const ir_global_t *global, uint32_t dimension, uint64_t *length)
{
    if (static_extent(global->shape, dimension, length)) return true;
    const ir_global_init_value_t *value = global->has_static_initializer
        ? global->static_initializer : nullptr;
    if (!value || global->mutability == IR_MUT_MUTABLE) return false;
    for (uint32_t d = 0; d < dimension; d++) {
        if (value->kind != IR_GLOBAL_INIT_SEQUENCE || !value->aggregate.count)
            return false;
        // Rows of one length are a rectangle; otherwise, there's no one
        // length for the dimension.
        uint64_t row = value->aggregate.items[0].aggregate.count;
        for (size_t i = 0; i < value->aggregate.count; i++)
            if (value->aggregate.items[i].kind != IR_GLOBAL_INIT_SEQUENCE
                || value->aggregate.items[i].aggregate.count != row)
                return false;
        value = &value->aggregate.items[0];
    }
    if (value->kind != IR_GLOBAL_INIT_SEQUENCE) return false;
    *length = value->aggregate.count;
    return true;
}

// The static extent of dimension `dimension` of the array `value` reaches,
// following copies, loads and addresses back to what declares its shape or
// assigns it, and indexes out to the dimension they select from.
static bool
static_array_extent(const ir_func_t *function, const ir_value_t *value,
                    uint32_t dimension, uint64_t *length, int depth)
{
    if (!value || depth > 16) return false;
    if (static_extent(ir_value_shape((ir_value_t *)value), dimension, length))
        return true;
    if (value->local)
        return assigned_local_extent(function, value->local, dimension,
                                     length, depth);
    if (value->kind == IR_VAL_GLOBAL && value->global)
        return global_extent(value->global, dimension, length);
    const ir_instr_t *def = value->kind == IR_VAL_INSTR ? value->def : nullptr;
    if (!def) return false;
    if (!function && def->block) function = def->block->owner;
    // An array literal not laid out by its shape has one item per operand.
    if (def->op == IR_OP_AGGREGATE && !dimension
        && th_kind_of(th_resolve(value->type)) == TH_SEQUENCE) {
        *length = def->n_operands;
        return true;
    }
    if (!def->n_operands) return false;
    const ir_value_t *base = ir_operand_value(&def->operands[0]);
    switch (def->op) {
    case IR_OP_ADDRESS_OF:
    case IR_OP_COPY:
    case IR_OP_NOMINAL_CAST:
    case IR_OP_LOAD: {
        const ir_local_t *local = ir_operand_local(&def->operands[0]);
        if (local)
            return assigned_local_extent(function, local, dimension, length,
                                         depth);
        if (base && base->kind == IR_VAL_GLOBAL && base->global)
            return global_extent(base->global, dimension, length);
        return static_array_extent(function, base, dimension, length,
                                   depth + 1);
    }
    case IR_OP_INDEX:
    case IR_OP_SEQUENCE_GET:
        // A row of a multi-dimensional array: the next dimension.
        return static_array_extent(function, base, dimension + 1, length,
                                   depth + 1);
    case IR_OP_FIELD:
    case IR_OP_EXTRACT: {
        // A one-dimensional array field held inline has as many elements as
        // its layout holds.
        th_type *field_type = th_resolve(value->type);
        if (th_kind_of(field_type) == TH_INDIRECTION)
            field_type = th_resolve(th_ind_referent(field_type));
        th_type *record = base ? th_resolve(base->type) : nullptr;
        if (record && def->op == IR_OP_FIELD
            && th_kind_of(record) == TH_INDIRECTION)
            record = th_resolve(th_ind_referent(record));
        ir_module_t *module = def->block && def->block->owner
            ? def->block->owner->owner : nullptr;
        bool known = false;
        const ir_final_layout_t *layout = module && record
            ? ir_layout_final(module, record, ir_module_target(module))
            : nullptr;
        if (dimension || def->n_operands != 2
            || def->operands[1].kind != IR_OPD_NAME || !layout
            || th_kind_of(field_type) != TH_SEQUENCE
            || th_kind_of(th_resolve(th_seq_elem(field_type))) == TH_SEQUENCE
            || !ir_layout_member_inline(module, record,
                                        def->operands[1].name, &known))
            return false;
        for (size_t i = 0; i < layout->n_members; i++)
            if (layout->members[i].name.data
                && core_string_eq(layout->members[i].name,
                                  def->operands[1].name)) {
                *length = layout->members[i].inline_element_count;
                return true;
            }
        return false;
    }
    default:
        return false;
    }
}

bool
n00bb_static_array_length(const ir_func_t *function, const ir_value_t *value,
                          uint64_t *length)
{
    return static_array_extent(function, value, 0, length, 0);
}

// Whether a closed enum `symbol` has a member whose value is 0.
static bool
enum_has_zero(parse_sym_entry_t *symbol)
{
    while (symbol && parse_sym_binding_origin(symbol))
        symbol = parse_sym_binding_origin(symbol);
    for (parse_sym_entry_t *item = symbol && symbol->member_scope
             ? symbol->member_scope->first_in_scope : nullptr;
         item; item = item->next_in_scope) {
        const n00bb_enum_value_t *value = item->kind == PARSE_SYM_ENUM_CONST
            && item->has_comptime_value ? item->comptime_value : nullptr;
        if (value && !value->magnitude) return true;
    }
    return false;
}

// Whether a record or object `type`, zeroed, is a value that code in `site`
// could build from a field tuple: no `__init` it must run (one outside its
// package), no field private to another module, and every field zeroable,
// none of them an `object` or a reference.
static bool
record_zero_tolerant(core_translation_unit_t *site, th_type *type,
                     th_type *record, th_type **blame, uint32_t depth)
{
    n00bb_tu_state_t *state = tu_state(site);
    core_translation_unit_t *owner = nullptr;
    parse_scope_t *scope = record_scope_for_type(state->link_analysis, type,
                                                 &owner);
    for (parse_sym_entry_t *field = scope && owner != site
             ? scope->first_in_scope : nullptr; field;
         field = field->next_in_scope)
        if (field->is_field && member_is_private(field, field->owner_type)) {
            *blame = type;
            return false;
        }
    if (n00bb_type_has_constructor(site, type)
        && !n00bb_same_package(owner, site)) {
        *blame = type;
        return false;
    }
    for (size_t i = 0; i < th_record_len(record); i++) {
        th_type *field = th_record_member(record, i)->type;
        th_type *resolved = field ? th_resolve(field) : nullptr;
        if (resolved && !th_option_item(resolved)
            && n00bb_object_type(state->types, resolved)) {
            *blame = resolved;
            return false;
        }
        if (!n00bb_zero_tolerant(site, field, blame, depth + 1))
            return false;
    }
    return true;
}

bool
n00bb_zero_tolerant(core_translation_unit_t *site, th_type *type,
                    th_type **blame, uint32_t depth)
{
    n00bb_tu_state_t *state = site ? tu_state(site) : nullptr;
    type = type ? th_resolve(type) : nullptr;
    // A type not yet known (a generic's parameter) is checked where it is.
    if (!state || !type || depth > 16 || th_kind_of(type) == TH_VAR
        || th_option_item(type) || th_is_nil(type)) return true;
    *blame = type;
    switch (th_kind_of(type)) {
    case TH_SEQUENCE:
        return n00bb_zero_tolerant(site, th_seq_elem(type), blame, depth + 1);
    case TH_INDIRECTION: {
        uint8_t attrs = 0;
        return th_ind_attrs(type, &attrs) && (attrs & TH_NULLABLE);
    }
    default:
        break;
    }
    if (n00bb_flagset_type(site, state, type, nullptr)) return true;
    parse_sym_entry_t *enumeration = n00bb_enum_type_symbol(
        state->link_analysis, type);
    if (enumeration) {
        const n00bb_enum_range_t *range = parse_sym_binding(enumeration,
            CORE_STRING_STATIC("n00bb.enum-range"));
        return (range && range->is_open) || enum_has_zero(enumeration);
    }
    bool variant = false;
    if (th_named_info(type, nullptr, &variant, nullptr) && variant)
        return false;
    th_type *record = n00bb_record_shape(state, type);
    if (record)
        return record_zero_tolerant(site, type, record, blame, depth);
    if (th_builtin_kind(state->types, type) == TH_BUILTIN_STRING) return true;
    ir_class_t category = state->ir_module
        ? ir_machine_of(state->ir_module, type).class : IR_CLASS_VOID;
    return category == IR_CLASS_SINT || category == IR_CLASS_UINT
        || category == IR_CLASS_BOOL || category == IR_CLASS_FLOAT;
}

// An array whose elements start zeroed must hold elements with a zero
// value. Reports, and returns false, when `element` has none.
bool
n00bb_check_zeroed_elements(ir_lower_context_t *context, th_type *element,
                            ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    th_type *blame = nullptr;
    if (!lower || n00bb_zero_tolerant(lower->unit, element, &blame, 0))
        return true;
    core_string_t element_text = lower_type_text(context, element);
    core_string_t blame_text = lower_type_text(context, blame);
    bool inner = !ir_type_eq(context->module, th_resolve(element),
                             th_resolve(blame));
    if (inner)
        lower_report(context, N00B_DIAG_ARRAY_ELEMENT_NO_ZERO, span,
            N00B_TEXT_ARRAY_ELEMENT_NO_ZERO_IN,
            N00BB_ARGS(ARG_TEXT(element_text), ARG_TEXT(blame_text)));
    else
        lower_report(context, N00B_DIAG_ARRAY_ELEMENT_NO_ZERO, span,
            N00B_TEXT_ARRAY_ELEMENT_NO_ZERO, N00BB_ARGS(ARG_TEXT(blame_text)));
    lower_help(context, N00B_TEXT_ARRAY_ELEMENT_NO_ZERO_HELP,
               N00BB_ARGS(ARG_TEXT(element_text)));
    core_free(element_text.data);
    core_free(blame_text.data);
    return false;
}

static ir_value_t *
emit_array_allocation(ir_lower_context_t *context, th_type *leaf,
                      uint32_t rank, ir_shape_extent_t *extents,
                      th_type *result_type, ir_span_t span,
                      bool check_elements);

ir_value_t *
n00bb_emit_array_allocation(ir_lower_context_t *context, th_type *leaf,
                            uint32_t rank, ir_shape_extent_t *extents,
                            th_type *result_type, ir_span_t span)
{
    return emit_array_allocation(context, leaf, rank, extents, result_type,
                                 span, true);
}

// The zeroed default of a `let` array declared without a size or a value,
// whose length its assignment gives (n00bb_size_let_defaults): `length` is
// a placeholder constant that it sets, and it checks the elements only if
// the default can be read.
ir_value_t *
n00bb_emit_let_default_array(ir_lower_context_t *context, th_type *type,
                             ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    th_type *uint_type = n00bb_ir_builtin_type(context->module,
                                               CORE_STRING_STATIC("uint"));
    uint32_t rank = 0;
    th_type *leaf = type ? th_resolve(type) : nullptr;
    while (leaf && th_kind_of(leaf) == TH_SEQUENCE && rank < 64) {
        leaf = th_resolve(th_seq_elem(leaf));
        rank++;
    }
    ir_instr_t *length = uint_type && rank ? n00bb_emit_uint_constant(context,
        uint_type, 0, span, CORE_STRING_STATIC("__n00bb_let_length")) : nullptr;
    if (!length || !lower) return nullptr;
    ir_shape_extent_t extents[64] = {0};
    extents[0] = (ir_shape_extent_t){.kind = IR_SHAPE_EXTENT_RUNTIME,
                                     .value = length->dest};
    for (uint32_t i = 1; i < rank; i++)
        extents[i] = (ir_shape_extent_t){.kind = IR_SHAPE_EXTENT_UNRESOLVED};
    ir_value_t *array = emit_array_allocation(context, leaf, rank, extents,
                                              type, span, false);
    return array && array->def
        && ir_annot_set(lower->ir_context, &array->def->annots, (ir_annot_t){
               .key = N00BB_LET_DEFAULT_KEY, .kind = IR_ANNOT_BOOL,
               .b = true})
        ? array : nullptr;
}

static ir_value_t *
emit_array_allocation(ir_lower_context_t *context, th_type *leaf,
                      uint32_t rank, ir_shape_extent_t *extents,
                      th_type *result_type, ir_span_t span,
                      bool check_elements)
{
    n00bb_lower_context_t *lower = context->user;
    th_type *uint_type = n00bb_ir_builtin_type(context->module, CORE_STRING_STATIC("uint"));
    ir_instr_t *one = uint_type
        ? n00bb_emit_uint_constant(context, uint_type, 1, span,
                                   CORE_STRING_STATIC("__n00bb_array_one")) : nullptr;
    if (!lower || !lower->ir_context || !one) return nullptr;
    for (uint32_t i = 0; i < rank; i++) {
        if (extents[i].value || extents[i].kind != IR_SHAPE_EXTENT_STATIC)
            continue;
        ir_instr_t *constant = n00bb_emit_uint_constant(context, uint_type,
            extents[i].static_value, span, CORE_STRING_STATIC("__n00bb_array_extent"));
        if (!constant) return nullptr;
        extents[i].value = constant->dest;
    }
    uint32_t known_prefix = 0;
    while (known_prefix < rank && extents[known_prefix].value)
        known_prefix++;
    for (uint32_t i = known_prefix; i < rank; i++)
        if (extents[i].value) return nullptr;
    // The elements start zeroed; ragged rows start as empty arrays instead.
    // A bad element is reported, and the allocation still made, so the
    // caller goes on as usual.
    if (check_elements && known_prefix == rank)
        (void)n00bb_check_zeroed_elements(context, leaf, span);
    ir_shape_t *shape = ir_shape_new(lower->ir_context, leaf, rank);
    bool ok = shape != nullptr;
    for (uint32_t i = 0; ok && i < rank; i++)
        ok = ir_shape_set_extent(shape, i, extents[i]);
    ok = ok && ir_shape_set_storage(shape, known_prefix, true);
    ir_value_t *count = one->dest;
    for (uint32_t i = 0; ok && i < known_prefix; i++) {
        ir_instr_t *product = n00bb_emit_checked_product(context, count,
            extents[i].value, uint_type, span);
        if (!product) ok = false;
        else count = product->dest;
    }
    ok = ok && ir_shape_set_element_count(shape, (ir_shape_measure_t){
        .kind = IR_SHAPE_MEASURE_RUNTIME,
        .value = count,
    });
    ir_value_t *stride = one->dest;
    for (uint32_t i = known_prefix; ok && i > 0; i--) {
        uint32_t dimension = i - 1;
        ok = ir_shape_set_stride(shape, dimension,
            (ir_shape_measure_t){.kind = IR_SHAPE_MEASURE_RUNTIME,
                                  .value = stride});
        if (ok && dimension > 0) {
            ir_instr_t *product = n00bb_emit_checked_product(context, stride,
                extents[dimension].value, uint_type, span);
            if (!product) ok = false;
            else stride = product->dest;
        }
    }
    ir_instr_t *instruction = ok
        ? ir_lower_emit_instruction(context, IR_OP_ARRAY_NEW, span) : nullptr;
    ok = ok && instruction
        && ir_lower_set_temp_dest(context, instruction,
                                  CORE_STRING_STATIC("__n00bb_array_new_"), result_type)
        && ir_instr_set_array_init(lower->ir_context, instruction,
                                   IR_ARRAY_INIT_LANGUAGE_ZERO);
    for (uint32_t i = 0; ok && i < known_prefix; i++)
        ok = ir_instr_operand_value(instruction, extents[i].value);
    ok = ok && ir_value_set_shape(instruction->dest, shape);
    // A zeroed object is a fresh object with zeroed fields, not a null
    // handle, so each element gets one: the annotation names the storage to
    // allocate for it.
    th_ctx *types = ir_module_types(context->module);
    th_type *element = leaf ? th_resolve(leaf) : nullptr;
    th_type *reference = ok && known_prefix == rank && element
            && !th_option_item(element) && n00bb_object_type(types, element)
        ? n00bb_reference_definition(types, element) : nullptr;
    th_type *storage = reference ? th_ind_referent(reference) : nullptr;
    if (storage)
        ok = ir_annot_set(lower->ir_context, &instruction->annots,
            (ir_annot_t){.key = N00BB_ARRAY_ZERO_OBJECTS_KEY,
                         .kind = IR_ANNOT_TYPE, .t = storage});
    return ok ? instruction->dest : nullptr;
}

ir_lower_result_t
n00bb_lower_array_new(ir_lower_context_t *context, parse_visit_t *visit,
                      ir_value_t *const *operands, size_t operand_count,
                      th_type *result_type, ir_span_t span, void *state)
{
    (void)operands;
    (void)operand_count;
    (void)state;
    th_type *declared = parse_tree_type(child_nt(visit->grammar, visit->node,
        CORE_STRING_STATIC("type-spec")));
    if (declared) result_type = declared;
    if (!result_type)
        return lower_internal(context, span,
                              "`new` has no resolved result type");
    th_type *resolved_type = th_resolve(result_type);
    n00bb_lower_context_t *lower = context->user;
    if (th_kind_of(resolved_type) != TH_SEQUENCE)
        return n00bb_lower_constructor_new(context, visit, result_type, span);

    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_analysis_t *analysis = tu ? tu->link_analysis : nullptr;
    if (!lower || !tu || !analysis)
        return lower_internal(context, span,
                           "array construction has no LinkUnit analysis");
    parse_tree_t *type_node = child_nt(visit->grammar, visit->node,
                                       CORE_STRING_STATIC("type-spec"));
    core_list_t(n00bb_tree_ptr_t) type_dimensions =
        core_list_new(n00bb_tree_ptr_t);
    bool type_shape_ok = !type_node
        || collect_array_dimension_nodes(visit->grammar, tu, type_node,
                                         &type_dimensions);
    if (!type_shape_ok) {
        core_list_free(type_dimensions);
        return lower_internal(context, span,
                              "could not determine the array type dimensions");
    }
    uint32_t rank = 0;
    th_type *leaf = th_resolve(result_type);
    while (th_kind_of(leaf) == TH_SEQUENCE) {
        leaf = th_resolve(th_seq_elem(leaf));
        if (!leaf || rank == UINT32_MAX) {
            core_list_free(type_dimensions);
            return lower_internal(context, span,
                                  "`new` has an invalid array type");
        }
        rank++;
    }
    parse_tree_t *call_args = child_nt(visit->grammar, visit->node,
                                       CORE_STRING_STATIC("call-args"));
    if (child_nt(visit->grammar, call_args, CORE_STRING_STATIC("keyword-args"))) {
        core_list_free(type_dimensions);
        return lower_report(context, N00B_DIAG_ARRAY_DIMENSIONS, span,
            N00B_TEXT_ARRAY_DIMENSIONS_GIVEN_TO_NEW,
            N00BB_NO_ARGS);
    }
    core_list_t(n00bb_tree_ptr_t) arguments =
        core_list_new(n00bb_tree_ptr_t);
    collect_positional_nodes(visit->grammar,
        child_nt(visit->grammar, call_args, CORE_STRING_STATIC("positional-args")),
        &arguments);
    size_t argument_offset = 0;
    size_t dimension_argument_count = arguments.len - argument_offset;
    if (type_dimensions.len > rank) {
        core_list_free(arguments);
        core_list_free(type_dimensions);
        return lower_internal(context, span,
                              "array type shape exceeds its sequence rank");
    }
    uint32_t unsized_count = rank;
    for (size_t i = 0; i < type_dimensions.len; i++)
        if (child_nt(visit->grammar, type_dimensions.data[i],
                     CORE_STRING_STATIC("expression"))) unsized_count--;
    if (dimension_argument_count > unsized_count
        || (!dimension_argument_count && unsized_count)) {
        core_list_free(arguments);
        core_list_free(type_dimensions);
    {
        core_string_t type_text = lower_type_text(context, result_type);
        ir_lower_result_t result = lower_report(context,
            N00B_DIAG_ARRAY_DIMENSIONS, span,
            N00B_TEXT_NEW_DIMENSIONS_WITHOUT_SIZE,
            N00BB_ARGS(ARG_NUMBER(unsized_count), ARG_TEXT(type_text),
                       ARG_NUMBER(dimension_argument_count)));
        lower_help(context, N00B_TEXT_PASS_ONE_SIZE_FOR_EACH, N00BB_NO_ARGS);
        core_free(type_text.data);
        return result;
    }
    }

    th_type *uint_type = n00bb_ir_builtin_type(context->module, CORE_STRING_STATIC("uint"));
    ir_instr_t *one = uint_type
        ? n00bb_emit_uint_constant(context, uint_type, 1, span,
                                   CORE_STRING_STATIC("__n00bb_array_one")) : nullptr;
    if (!one) {
        core_list_free(arguments);
        core_list_free(type_dimensions);
        return lower_internal(context, span,
                           "could not create the array extent constant");
    }
    core_list_t(n00bb_ir_value_ptr_t) dimension_values =
        core_list_new(n00bb_ir_value_ptr_t);
    core_list_t(n00bb_tree_ptr_t) dimension_expressions =
        core_list_new(n00bb_tree_ptr_t);
    for (uint32_t i = 0; i < rank; i++) {
        core_list_push(dimension_values, (ir_value_t *)nullptr);
        core_list_push(dimension_expressions, (parse_tree_t *)nullptr);
    }
    bool ok = true;
    for (size_t i = 0; ok && i < type_dimensions.len; i++) {
        parse_tree_t *expression = child_nt(visit->grammar,
            type_dimensions.data[i], CORE_STRING_STATIC("expression"));
        if (!expression) continue;
        dimension_expressions.data[i] = expression;
        ir_shape_extent_t declared_extent = array_dimension_extent(
            visit->grammar, expression);
        if (declared_extent.kind == IR_SHAPE_EXTENT_STATIC) {
            ir_instr_t *constant = n00bb_emit_uint_constant(context,
                uint_type, declared_extent.static_value,
                lower_span_for_tree(context, expression),
                CORE_STRING_STATIC("__n00bb_array_extent"));
            if (!constant) {
                ok = false;
                break;
            }
            dimension_values.data[i] = constant->dest;
            continue;
        }
        if (declared_extent.kind == IR_SHAPE_EXTENT_RUNTIME) {
            dimension_values.data[i] = n00bb_lower_array_dimension_value(
                context, expression, uint_type, one->dest,
                lower_span_for_tree(context, expression), true);
            if (!dimension_values.data[i]) ok = false;
            continue;
        }
        core_string_t name = {0};
        simple_identifier_expression(visit->grammar, expression, &name);
        if (name.u8_bytes)
            lower_report(context, N00B_DIAG_ARRAY_EXTENT_NOT_CONSTANT,
                lower_span_for_tree(context, expression),
                N00B_TEXT_ARRAY_SIZE_MUST_BE_A,
                N00BB_ARGS(ARG_TEXT(name)));
        else
            lower_report(context, N00B_DIAG_ARRAY_EXTENT_NOT_CONSTANT,
                lower_span_for_tree(context, expression),
                N00B_TEXT_THIS_ARRAY_SIZE_MUST_BE,
                N00BB_NO_ARGS);
        lower_help(context, N00B_TEXT_USE_A_CONST_OR_LEAVE, N00BB_NO_ARGS);
        ok = false;
    }
    if (!ok) {
        core_list_free(arguments);
        core_list_free(type_dimensions);
        core_list_free(dimension_values);
        core_list_free(dimension_expressions);
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    }

    for (size_t i = 0; ok && i < dimension_argument_count; i++) {
        parse_tree_t *argument = arguments.data[argument_offset + i];
        ir_value_t *extent = n00bb_lower_array_dimension_value(context,
            argument, uint_type, one->dest,
            lower_span_for_tree(context, argument), false);
        if (!extent) { ok = false; break; }
        uint32_t dimension = 0;
        while (dimension < rank && dimension_values.data[dimension])
            dimension++;
        if (dimension == rank) {
            ok = false;
            break;
        }
        dimension_values.data[dimension] = extent;
    }
    core_list_free(arguments);
    if (!ok) {
        core_list_free(type_dimensions);
        core_list_free(dimension_values);
        core_list_free(dimension_expressions);
        return lower_failed_after(context, span,
            CORE_STRING_STATIC("could not compute the array dimensions"));
    }

    ir_shape_extent_t *extents = core_alloc_array(ir_shape_extent_t, rank);
    for (uint32_t i = 0; i < rank; i++) {
        if (dimension_expressions.data[i]) {
            extents[i] = array_dimension_extent(visit->grammar,
                dimension_expressions.data[i]);
            extents[i].value = dimension_values.data[i];
        }
        else if (dimension_values.data[i])
            extents[i] = (ir_shape_extent_t){
                .kind = IR_SHAPE_EXTENT_RUNTIME,
                .value = dimension_values.data[i],
            };
        else extents[i] = (ir_shape_extent_t){
                .kind = IR_SHAPE_EXTENT_UNRESOLVED};
    }
    ir_value_t *array = n00bb_emit_array_allocation(context, leaf, rank,
        extents, result_type, span);
    core_free(extents);
    core_list_free(type_dimensions);
    core_list_free(dimension_values);
    core_list_free(dimension_expressions);
    if (!array)
        return lower_internal(context, span,
                           "could not emit semantic array construction");
    return (ir_lower_result_t){IR_LOWER_VALUE, array};
}
