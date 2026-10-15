#include <string.h>
#include "compiler_tu.h"
#include "compiler_link.h"
#include "compiler_layout.h"
#include "compiler_lower.h"
#include "compiler_tu_interface.h"
#include "abi_layout.h"
#include "abi_semantics.h"
#include "abi_record_layout.h"
#include "record_layout_request.h"
#include "enum_value.h"
#include "flagset_value.h"
#include "storage_selection.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/list.h"
#include "lib/link_unit.h"
#include "lib/diagnostic.h"
#include "ir/ir.h"
#include "ir/layout.h"
#include "th/environment.h"

bool
publish_known_scalar_layouts(n00bb_link_analysis_t *analysis)
{
    for (size_t i = 0; i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        ir_module_t *modules[] = {
            state ? state->comptime_ir_module : nullptr,
            state ? state->ir_module : nullptr,
        };
        if (!symtab || !modules[0] || !modules[1]) return false;
        for (parse_scope_t *scope = symtab->all_scopes; scope;
             scope = scope->all_next) {
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                if (!symbol->type) continue;
                const n00bb_enum_range_t *enum_range = parse_sym_binding(
                    symbol, CORE_STRING_STATIC("n00bb.enum-range"));
                uint64_t enum_bits = 0;
                if (enum_range)
                    enum_bits = enum_range->magnitude_bits > UINT32_MAX
                            - (uint32_t)enum_range->is_signed
                        ? UINT64_MAX
                        : enum_range->magnitude_bits + enum_range->is_signed;
                if (enum_range && !enum_bits) enum_bits = 1;
                const n00bb_flagset_info_t *flagset_info = parse_sym_binding(
                    symbol, CORE_STRING_STATIC("n00bb.flagset-info"));
                for (size_t m = 0; m < sizeof(modules) / sizeof(modules[0]);
                     m++) {
                    th_type *type = th_apply_subst(
                        ir_module_types(modules[m]), symbol->type);
                    bool handled = false;
                    bool layout_ok = type && n00bb_abi_publish_direct_layout(
                        modules[m], type, &analysis->target, &handled);
                    if (layout_ok && enum_bits)
                        layout_ok = enum_bits <= UINT32_MAX
                            && n00bb_abi_publish_integer_like_layout(
                                modules[m], type, &analysis->target,
                                (uint32_t)enum_bits);
                    if (layout_ok && flagset_info && flagset_info->atomic_count)
                        layout_ok = n00bb_abi_publish_integer_like_layout(
                            modules[m], type, &analysis->target,
                            flagset_info->atomic_count);
                    if (layout_ok) continue;
                    core_buffer_t message = {0};
                    core_buffer_printf(&message,
                        "could not finalize ABI layout for type '%s'",
                        n00bb_print_type(ir_module_types(modules[m]),
                                         type ? type : symbol->type));
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0067"),
                        core_string_from_cstr(message.data),
                        symbol->decl_node
                            ? link_source_location(symbol->decl_node)
                            : (core_diag_location_t){0});
                    core_buffer_release(&message);
                    return false;
                }
            }
        }
        for (size_t m = 0; m < sizeof(modules) / sizeof(modules[0]); m++) {
            th_type *failed_type = nullptr;
            if (n00bb_abi_publish_interned_direct_layouts(
                    modules[m], &analysis->target, &failed_type)) continue;
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "could not finalize ABI layout for IR type '%s'",
                failed_type ? n00bb_print_type(ir_module_types(modules[m]),
                                                failed_type) : "unknown");
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0067"),
                core_string_from_cstr(message.data),
                (core_diag_location_t){0});
            core_buffer_release(&message);
            return false;
        }
    }
    return true;
}

static bool
inline_layout_cycle(ir_module_t *module, th_type *type, const char *via,
                    core_list_t(th_type_ptr_t) *active,
                    core_list_t(core_string_t) *edges,
                    core_buffer_t *message)
{
    type = th_resolve(type);
    if (!type) return false;
    if (th_kind_of(type) == TH_NAMED) {
        th_type *definition = ir_type_definition(module, type);
        size_t argument_count = th_named_nargs(type);
        bool inline_wrapper = th_builtin_kind_of(type) == TH_BUILTIN_ATOMIC;
        if (!definition && !inline_wrapper) return false;
        for (size_t i = 0; i < active->len; i++) {
            if (active->data[i] != type) continue;
            core_buffer_puts(message, "inline layout cycle: ");
            core_buffer_puts(message, n00bb_print_type(
                ir_module_types(module), type));
            for (size_t j = i + 1; j < active->len; j++) {
                core_buffer_printf(message, ".%.*s -> %s",
                    (int)edges->data[j].u8_bytes, edges->data[j].data,
                    n00bb_print_type(ir_module_types(module),
                                     active->data[j]));
            }
            core_buffer_printf(message, ".%s -> %s", via ? via : "value",
                n00bb_print_type(ir_module_types(module), type));
            return true;
        }
        core_list_push(*active, type);
        core_list_push(*edges, core_string_from_cstr(via ? via : "value"));
        bool cycle = definition
            ? inline_layout_cycle(module, definition, nullptr, active,
                                  edges, message) : false;
        for (size_t i = 0; inline_wrapper && !cycle
             && i < argument_count; i++) {
            const th_arg *argument = th_named_arg(type, i);
            if (argument && argument->kind == TH_ARG_TYPE)
                cycle = inline_layout_cycle(module, argument->type,
                    "value", active, edges, message);
        }
        active->len--;
        edges->len--;
        return cycle;
    }
    if (th_kind_of(type) == TH_RECORD) {
        for (size_t i = 0; i < th_record_len(type); i++) {
            const th_member *member = th_record_member(type, i);
            if (member && inline_layout_cycle(module, member->type,
                    member->name, active, edges, message)) return true;
        }
    }
    else if (th_kind_of(type) == TH_SUM) {
        for (size_t i = 0; i < th_sum_len(type); i++)
            if (inline_layout_cycle(module, th_sum_arm(type, i), "arm",
                                    active, edges, message)) return true;
    }
    return false;
}

static bool
validate_inline_layout_cycles(n00bb_link_analysis_t *analysis)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        ir_module_t *module = state ? state->ir_module : nullptr;
        if (!symtab || !module) return false;
        for (parse_scope_t *scope = symtab->all_scopes; scope;
             scope = scope->all_next) {
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_TYPEDEF || !symbol->type
                    || !ir_type_definition(module, symbol->type)) continue;
                core_list_t(th_type_ptr_t) active =
                    core_list_new(th_type_ptr_t);
                core_list_t(core_string_t) edges =
                    core_list_new(core_string_t);
                core_buffer_t message = {0};
                bool cycle = inline_layout_cycle(module, symbol->type,
                    nullptr, &active, &edges, &message);
                core_list_free(active);
                core_list_free(edges);
                if (cycle)
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0068"),
                        core_string_borrow(message.data, message.byte_len),
                        symbol->decl_node
                            ? link_source_location(symbol->decl_node)
                            : (core_diag_location_t){0});
                core_buffer_release(&message);
                if (cycle) return false;
            }
        }
    }
    return true;
}

static bool
record_field_has_c_scalar_representation(ir_module_t *module, th_type *type)
{
    type = th_resolve(type);
    th_type *atomic_element = n00bb_atomic_element_type(type);
    if (atomic_element) {
        const ir_final_layout_t *atomic_layout = ir_layout_final(module,
            type, ir_module_target(module));
        const ir_final_layout_t *element_layout = ir_layout_final(module,
            atomic_element, ir_module_target(module));
        return atomic_layout && element_layout
            && atomic_layout->intrinsic_bits
                == element_layout->intrinsic_bits
            && atomic_layout->alignment_bytes
                == element_layout->alignment_bytes
            && record_field_has_c_scalar_representation(module,
                                                         atomic_element);
    }
    if (th_kind_of(type) == TH_INDIRECTION) {
        const ir_final_layout_t *layout = ir_layout_final(module,
            type, ir_module_target(module));
        return layout && layout->intrinsic_bits == sizeof(void *) * 8
            && layout->alignment_bytes == _Alignof(void *);
    }
    th_type *option_element = nullptr;
    if (n00bb_option_type_element(type, &option_element)
        && th_kind_of(th_resolve(option_element)) == TH_INDIRECTION) {
        const ir_final_layout_t *layout = ir_layout_final(module, type,
            ir_module_target(module));
        return layout && layout->intrinsic_bits == sizeof(void *) * 8
            && layout->alignment_bytes == _Alignof(void *)
            && layout->has_discriminant
            && layout->discriminant_is_niche;
    }
    th_type *field_type = type;
    core_list_t(th_type_ptr_t) seen = core_list_new(th_type_ptr_t);
    bool compatible = false;
    while (type && th_kind_of(type) == TH_NAMED) {
        const char *name = nullptr;
        size_t argument_count = 0;
        th_named_info(type, &name, nullptr, nullptr, &argument_count);
        if (argument_count) break;
        th_type *definition = ir_type_definition(module, type);
        if (definition) {
            bool repeated = false;
            for (size_t i = 0; i < seen.len; i++)
                if (seen.data[i] == type) repeated = true;
            if (repeated) break;
            core_list_push(seen, type);
            type = th_resolve(definition);
            continue;
        }
        if (!builtin_type_name(name)) break;
        ir_machine_t machine = ir_machine_of(module, type);
        compatible = (machine.class == IR_CLASS_BOOL && machine.width == 1)
            || (machine.class == IR_CLASS_FLOAT
                && (machine.width == 64 || machine.width == 32))
            || ((machine.class == IR_CLASS_SINT
                 || machine.class == IR_CLASS_UINT)
                && machine.width && machine.width <= 64);
        break;
    }
    if (!compatible && type && th_kind_of(type) == TH_BITINT) {
        const ir_final_layout_t *layout = ir_layout_final(module, field_type,
            ir_module_target(module));
        uint64_t bits = 0;
        compatible = layout && th_bitint_info(type, &bits, nullptr)
            && bits && bits <= 64
            && layout->intrinsic_bits >= bits
            && (layout->intrinsic_bits == 8
                || layout->intrinsic_bits == 16
                || layout->intrinsic_bits == 32
                || layout->intrinsic_bits == 64)
            && layout->alignment_bytes == layout->intrinsic_bits / 8;
    }
    core_list_free(seen);
    return compatible;
}

static bool
inline_array_member_storage(parse_sym_entry_t *field, th_type *type,
                            th_type **element, uint64_t *count)
{
    if (element) *element = nullptr;
    if (count) *count = 0;
    ir_shape_t *shape = field ? parse_sym_binding(field,
        CORE_STRING_STATIC("ir.array-shape")) : nullptr;
    ir_shape_storage_t storage;
    ir_shape_measure_t measure;
    if (!shape || !type || !ir_shape_storage(shape, &storage)
        || !storage.flat_prefix_rank
        || !ir_shape_element_count(shape, &measure)
        || measure.kind != IR_SHAPE_MEASURE_STATIC
        || !measure.static_value) return false;
    th_type *item = th_resolve(type);
    for (uint32_t i = 0; i < storage.flat_prefix_rank; i++) {
        if (!item || th_kind_of(item) != TH_SEQUENCE) return false;
        item = th_resolve(th_seq_elem(item));
    }
    if (!item) return false;
    if (element) *element = item;
    if (count) *count = measure.static_value;
    return true;
}

static bool
publish_c_record_symbol(n00bb_link_analysis_t *analysis,
                        core_translation_unit_t *owner,
                        parse_sym_entry_t *symbol, ir_module_t *module);

static parse_sym_entry_t *
requested_record_symbol_for_type(n00bb_link_analysis_t *analysis,
                                 th_type *type,
                                 n00bb_record_policy_t policy,
                                 core_translation_unit_t **owner_out)
{
    if (!analysis || !analysis->units.len || !type) return nullptr;
    th_ctx *types = tu_state(analysis->units.data[0])->types;
    uint64_t wanted = th_hash(types, th_resolve(type));
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        for (parse_scope_t *scope = symtab ? symtab->all_scopes : nullptr;
             scope; scope = scope->all_next) {
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                n00bb_record_layout_request_t *request = parse_sym_binding(
                    symbol, CORE_STRING_STATIC(
                        "n00bb.record-layout-request"));
                if (!request || request->policy != policy
                    || request->alignment_expression || !symbol->type
                    || th_hash(types, th_resolve(symbol->type)) != wanted)
                    continue;
                if (owner_out) *owner_out = unit;
                return symbol;
            }
        }
    }
    return nullptr;
}

static bool
publish_c_record_representation(n00bb_link_analysis_t *analysis,
                                ir_module_t *module, th_type *type)
{
    type = th_resolve(type);
    if (!type || th_kind_of(type) != TH_NAMED) return false;
    core_translation_unit_t *owner = nullptr;
    parse_sym_entry_t *record = requested_record_symbol_for_type(analysis,
        type, N00BB_RECORD_C, &owner);
    if (record) return publish_c_record_symbol(analysis, owner, record, module);
    th_type *base = ir_type_definition(module, type);
    base = base ? th_resolve(base) : nullptr;
    if (!base || base == type || th_kind_of(base) != TH_NAMED
        || !publish_c_record_representation(analysis, module, base))
        return false;
    return ir_layout_final(module, type, analysis->target.triple)
        || ir_layout_copy_final(module, type, base, analysis->target.triple);
}

static bool
publish_c_record_symbol(n00bb_link_analysis_t *analysis,
                        core_translation_unit_t *owner,
                        parse_sym_entry_t *symbol, ir_module_t *module)
{
    th_type *record = symbol->type ? th_apply_subst(
        ir_module_types(module), symbol->type) : nullptr;
    if (!record) return false;
    if (ir_layout_final(module, record, analysis->target.triple)) return true;

    core_list_t(parse_sym_entry_ptr_t) fields =
        core_list_new(parse_sym_entry_ptr_t);
    bool valid = record_fields_in_source_order(symbol->member_scope, &fields)
        && fields.len && fields.len <= UINT32_MAX;
    n00bb_abi_record_member_t *members = valid
        ? core_alloc_array(n00bb_abi_record_member_t, fields.len) : nullptr;
    valid = valid && members;
    for (size_t i = 0; valid && i < fields.len; i++) {
        parse_sym_entry_t *field = fields.data[i];
        th_type *declared = declared_type_from_node(
            analysis->grammar, owner, field->decl_node);
        th_type *type = declared ? th_apply_subst(
            ir_module_types(module), declared) : nullptr;
        th_type *inline_element = nullptr;
        uint64_t inline_count = 0;
        if (type)
            (void)inline_array_member_storage(field, type,
                &inline_element, &inline_count);
        th_type *physical = inline_element ? inline_element : type;
        bool handled = false;
        bool direct = physical && n00bb_abi_publish_direct_layout(
            module, physical, &analysis->target, &handled);
        bool compatible = direct && handled
            && record_field_has_c_scalar_representation(module, physical);
        if (direct && !compatible && physical
            && th_kind_of(th_resolve(physical))
                == TH_NAMED) {
            compatible = publish_c_record_representation(analysis, module,
                                                          physical);
            if (!compatible && call_units_failed(analysis->units.data,
                                                  analysis->units.len)) {
                valid = false;
                break;
            }
        }
        if (!compatible) {
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "field '%.*s' has no C-compatible finalized ABI representation",
                (int)field->name.u8_bytes, field->name.data);
            core_diagnostics_report_at(core_translation_unit_diagnostics(
                owner), owner, CORE_DIAG_FATAL,
                CORE_STRING_STATIC("N00BB-L0069"),
                core_string_from_cstr(message.data),
                link_source_location(field->decl_node));
            core_buffer_release(&message);
            valid = false;
            break;
        }
        members[i] = (n00bb_abi_record_member_t){
            .name = field->name.data,
            .type = type,
            .ordinal = (uint32_t)i,
            .c_compatible = true,
            .inline_element_type = inline_element,
            .inline_element_count = inline_count,
        };
    }
    if (valid) valid = n00bb_abi_publish_c_record_layout(module,
        record, &analysis->target, members, fields.len);
    core_free(members);
    core_list_free(fields);
    if (!valid && !call_units_failed(analysis->units.data,
                                     analysis->units.len))
        core_diagnostics_report_at(core_translation_unit_diagnostics(owner),
            owner, CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0069"),
            CORE_STRING_STATIC("could not finalize C record layout"),
            link_source_location(symbol->decl_node));
    return valid;
}

static bool
symbol_has_comptime_shape(parse_sym_entry_t *symbol)
{
    const char *keys[] = {"ir.array-shape", "ir.return-shape"};
    for (size_t k = 0; k < sizeof(keys) / sizeof(*keys); k++) {
        ir_shape_t *shape = parse_sym_binding(symbol,
            core_string_borrow(keys[k], strlen(keys[k])));
        ir_shape_storage_t storage;
        if (!shape || !ir_shape_storage(shape, &storage)) continue;
        for (uint32_t d = 0; d < storage.flat_prefix_rank; d++) {
            ir_shape_extent_t extent;
            if (ir_shape_extent(shape, d, &extent)
                && extent.kind == IR_SHAPE_EXTENT_COMPTIME) return true;
        }
    }
    return false;
}

static bool
c_record_has_comptime_shape(n00bb_link_analysis_t *analysis,
                            parse_sym_entry_t *record,
                            core_list_t(parse_sym_entry_ptr_t) *seen)
{
    for (size_t i = 0; i < seen->len; i++)
        if (seen->data[i] == record) return false;
    core_list_push(*seen, record);
    for (parse_sym_entry_t *field = record->member_scope
            ? record->member_scope->first_in_scope : nullptr;
         field; field = field->next_in_scope) {
        if (symbol_has_comptime_shape(field)) return true;
        parse_sym_entry_t *child = field->type
            ? requested_record_symbol_for_type(analysis, field->type,
                N00BB_RECORD_C, nullptr) : nullptr;
        if (child && c_record_has_comptime_shape(analysis, child, seen))
            return true;
    }
    return false;
}

bool
publish_requested_c_record_layouts(n00bb_link_analysis_t *analysis,
                                   bool known_only)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *owner = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(owner);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        if (!symtab) return false;
        for (parse_scope_t *scope = symtab->all_scopes; scope;
             scope = scope->all_next) {
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                n00bb_record_layout_request_t *request = parse_sym_binding(
                    symbol, CORE_STRING_STATIC(
                        "n00bb.record-layout-request"));
                if (!request || request->policy != N00BB_RECORD_C
                    || request->alignment_expression) continue;
                if (known_only) {
                    core_list_t(parse_sym_entry_ptr_t) seen =
                        core_list_new(parse_sym_entry_ptr_t);
                    bool pending = c_record_has_comptime_shape(analysis,
                        symbol, &seen);
                    core_list_free(seen);
                    if (pending) continue;
                }
                for (size_t d = 0; d < analysis->units.len; d++) {
                    core_translation_unit_t *importer =
                        analysis->units.data[d];
                    if (importer != owner
                        && !n00bb_unit_reaches_import(importer, owner)) continue;
                    n00bb_tu_state_t *destination = tu_state(
                        importer);
                    ir_module_t *modules[] = {
                        destination->comptime_ir_module,
                        destination->ir_module,
                    };
                    for (size_t m = 0;
                         m < sizeof(modules) / sizeof(modules[0]); m++) {
                        if (!modules[m] || !publish_c_record_symbol(analysis,
                                owner, symbol, modules[m])) return false;
                    }
                }
            }
        }
    }
    return true;
}

static bool
reject_unlowered_record_layout_requests(n00bb_link_analysis_t *analysis)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        if (!symtab) return false;
        for (parse_scope_t *scope = symtab->all_scopes; scope;
             scope = scope->all_next) {
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                n00bb_record_layout_request_t *request = parse_sym_binding(
                    symbol, CORE_STRING_STATIC(
                        "n00bb.record-layout-request"));
                if (!request) continue;
                if (!request->alignment_expression) {
                    if (request->policy == N00BB_RECORD_C) continue;
                    if (request->policy == N00BB_RECORD_ORDERED
                        && symbol->type) {
                        ir_module_t *modules[] = {
                            state->comptime_ir_module, state->ir_module,
                        };
                        bool finalized = true;
                        for (size_t m = 0; m < sizeof(modules)
                                / sizeof(*modules); m++) {
                            ir_module_t *module = modules[m];
                            th_type *type = module ? th_apply_subst(
                                ir_module_types(module), symbol->type)
                                : nullptr;
                            if (!type || !ir_layout_final(module, type,
                                    analysis->target.triple)) {
                                finalized = false;
                                break;
                            }
                        }
                        if (finalized) continue;
                    }
                }
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0069"),
                    CORE_STRING_STATIC(
                        "record layout modifiers are not finalized for this target"),
                    link_source_location(request->policy_node
                        ? request->policy_node : request->alignment_node));
                return false;
            }
        }
    }
    return true;
}

bool
publish_declared_aggregate_layouts(n00bb_link_analysis_t *analysis)
{
    bool valid = true;
    for (size_t u = 0; valid && u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        ir_module_t *modules[] = {state->comptime_ir_module,
                                  state->ir_module};
        for (size_t m = 0; valid && m < sizeof(modules) / sizeof(*modules);
             m++) {
            ir_module_t *module = modules[m];
            if (!module) { valid = false; break; }
            for (size_t f = 0; valid && f < ir_func_count(module); f++) {
                ir_func_t *function = ir_func_at(module, f);
                th_type *signature = function ? function->type : nullptr;
                if (!signature || th_kind_of(signature) != TH_FUNCTION)
                    continue;
                th_type *result = th_func_ret(signature);
                if (result && th_kind_of(th_resolve(result)) == TH_NAMED
                    && ir_type_definition(module, result))
                    valid = ir_layout_ensure(module, result,
                        analysis->target.triple) != nullptr;
                for (size_t p = 0; valid && p < th_func_nparams(signature);
                     p++) {
                    const th_param *parameter = th_func_param(signature, p);
                    th_type *type = parameter ? parameter->type : nullptr;
                    if (type && th_kind_of(th_resolve(type)) == TH_NAMED
                        && ir_type_definition(module, type))
                        valid = ir_layout_ensure(module, type,
                            analysis->target.triple) != nullptr;
                }
            }
            bool progress;
            do {
                progress = false;
                for (size_t i = 0; valid && i < ir_type_count(module); i++) {
                    th_type *registered = ir_type_at(module, i);
                    if (!registered
                        || th_kind_of(th_resolve(registered)) != TH_NAMED)
                        continue;
                    th_type *type = th_apply_subst(ir_module_types(module),
                                                   registered);
                    if (!type) { valid = false; break; }
                    if (ir_layout_final(module, type, analysis->target.triple))
                        continue;
                    th_type *definition = ir_type_definition(module, type);
                    if (!definition || th_kind_of(definition) != TH_RECORD)
                        continue;
                    if (requested_record_symbol_for_type(analysis, type,
                            N00BB_RECORD_C, nullptr)) continue;
                    size_t count = th_record_len(definition);
                    if (count > UINT32_MAX) { valid = false; break; }
                    bool variant = false;
                    th_named_info(type, nullptr, nullptr, &variant, nullptr);
                    n00bb_abi_record_member_t *members = count
                        ? core_alloc_array(n00bb_abi_record_member_t, count)
                        : nullptr;
                    n00bb_abi_variant_arm_t *arms = variant && count
                        ? core_alloc_array(n00bb_abi_variant_arm_t, count)
                        : nullptr;
                    bool ready = (!count || members != nullptr)
                        && (!variant || !count || arms != nullptr);
                    parse_scope_t *member_scope = record_scope_for_type(
                        analysis->grammar, &analysis->units, type, nullptr);
                    for (size_t j = 0; ready && j < count; j++) {
                        const th_member *member = th_record_member(definition,
                                                                    j);
                        th_type *child_type = member && member->type
                            ? th_apply_subst(ir_module_types(module),
                                             member->type) : nullptr;
                        parse_sym_entry_t *field = member && member_scope
                            ? parse_symtab_lookup_in(member_scope,
                                core_string_from_cstr(member->name))
                            : nullptr;
                        th_type *inline_element = nullptr;
                        uint64_t inline_count = 0;
                        if (child_type && field && !variant)
                            (void)inline_array_member_storage(field,
                                child_type, &inline_element, &inline_count);
                        th_type *physical = inline_element
                            ? inline_element : child_type;
                        bool handled = false;
                        if (physical && !n00bb_abi_publish_direct_layout(
                                module, physical, &analysis->target,
                                &handled)) {
                            valid = false;
                            break;
                        }
                        const ir_final_layout_t *child = physical
                            ? ir_layout_final(module, physical,
                                              analysis->target.triple) : nullptr;
                        if (!member || !member->name || !child) {
                            ready = false;
                            break;
                        }
                        bool packable = child->n_occupied == 1
                            && child->occupied[0].offset == 0
                            && child->occupied[0].width < child->intrinsic_bits
                            && !child->n_reserved;
                        members[j] = (n00bb_abi_record_member_t){
                            .name = member->name,
                            .type = child_type,
                            .ordinal = (uint32_t)j,
                            .packed = packable,
                            .inline_element_type = inline_element,
                            .inline_element_count = inline_count,
                        };
                        if (variant) arms[j] = (n00bb_abi_variant_arm_t){
                            .name = member->name,
                            .type = child_type,
                            .ordinal = (uint32_t)j,
                        };
                    }
                    if (ready) {
                        n00bb_record_policy_t policy =
                            requested_record_symbol_for_type(analysis, type,
                                N00BB_RECORD_ORDERED, nullptr)
                            ? N00BB_RECORD_ORDERED : N00BB_RECORD_DEFAULT;
                        valid = variant
                            ? n00bb_abi_publish_variant_layout(module, type,
                                &analysis->target, arms, count)
                            : n00bb_abi_publish_record_layout(module,
                                type, &analysis->target, policy,
                                false, members, count);
                        progress = valid;
                    }
                    core_free(arms);
                    core_free(members);
                }
            } while (valid && progress);
        }
    }
    return valid;
}

bool
publish_interned_variant_layouts(n00bb_link_analysis_t *analysis)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        ir_module_t *modules[] = {state->comptime_ir_module,
                                  state->ir_module};
        for (size_t m = 0; m < sizeof(modules) / sizeof(*modules); m++) {
            ir_module_t *module = modules[m];
            for (size_t f = 0; f < ir_func_count(module); f++) {
                ir_func_t *function = ir_func_at(module, f);
                th_type *channels[] = {
                    th_func_ret(function->type),
                    th_func_error(function->type),
                };
                for (size_t c = 0; c < sizeof(channels) / sizeof(*channels);
                     c++) {
                    if (!channels[c] || !th_is_closed(ir_module_types(module),
                                                     channels[c])) continue;
                    bool handled = false;
                    if (!n00bb_abi_publish_direct_layout(module,
                            channels[c], &analysis->target, &handled))
                        return false;
                }
                for (size_t b = 0; b < function->n_blocks; b++) {
                    ir_block_t *block = function->blocks[b];
                    for (size_t x = 0; x < block->n_instrs; x++) {
                        ir_instr_t *instruction = block->instrs[x];
                        th_type *type = instruction->dest
                            ? instruction->dest->type : nullptr;
                        if (!type || !th_is_closed(ir_module_types(module),
                                                   type)) continue;
                        bool handled = false;
                        if (!n00bb_abi_publish_direct_layout(module, type,
                                &analysis->target, &handled)) return false;
                    }
                }
            }
            th_type *failed_type = nullptr;
            if (!n00bb_abi_publish_interned_direct_layouts(module,
                    &analysis->target, &failed_type)) return false;
            bool progress;
            do {
                progress = false;
                for (size_t i = 0; i < ir_type_count(module); i++) {
                    th_type *type = ir_type_at(module, i);
                    bool variant = false;
                    if (!th_named_info(type, nullptr, nullptr, &variant,
                                       nullptr) || !variant
                        || ir_layout_final(module, type,
                                           analysis->target.triple)) continue;
                    th_type *definition = ir_type_definition(module, type);
                    if (!definition || th_kind_of(definition) != TH_RECORD)
                        continue;
                    size_t count = th_record_len(definition);
                    if (!count || count >= UINT32_MAX) continue;
                    n00bb_abi_variant_arm_t *arms = core_alloc_array(
                        n00bb_abi_variant_arm_t, count);
                    if (!arms) return false;
                    bool ready = true;
                    for (size_t j = 0; ready && j < count; j++) {
                        const th_member *member = th_record_member(
                            definition, j);
                        th_type *child = member && member->type
                            ? th_apply_subst(ir_module_types(module),
                                             member->type) : nullptr;
                        bool handled = false;
                        ready = member && member->name && child
                            && n00bb_abi_publish_direct_layout(module,
                                child, &analysis->target, &handled)
                            && ir_layout_final(module, child,
                                               analysis->target.triple);
                        if (ready) arms[j] = (n00bb_abi_variant_arm_t){
                            .name = member->name,
                            .type = child,
                            .ordinal = (uint32_t)j,
                        };
                    }
                    if (ready) {
                        bool published = n00bb_abi_publish_variant_layout(
                            module, type, &analysis->target, arms, count);
                        core_free(arms);
                        if (!published) return false;
                        progress = true;
                    }
                    else core_free(arms);
                }
            } while (progress);
        }
    }
    return true;
}

static bool
finalize_atomic_aggregate_layouts(n00bb_link_analysis_t *analysis)
{
    for (size_t i = 0; i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        ir_module_t *modules[] = {
            state ? state->comptime_ir_module : nullptr,
            state ? state->ir_module : nullptr,
        };
        for (size_t m = 0; m < sizeof(modules) / sizeof(*modules); m++) {
            ir_module_t *module = modules[m];
            if (!module) return false;
            for (size_t j = 0; j < ir_type_count(module); j++) {
                th_type *type = ir_type_at(module, j);
                if (!n00bb_atomic_element_type(type)
                    || ir_layout_final(module, type,
                                       analysis->target.triple)) continue;
                if (n00bb_atomic_native_support(module, type,
                                                &analysis->target)) continue;
                core_diag_location_t location = {0};
                parse_symtab_t *symbols = state->annotations
                    ? state->annotations->symtab : nullptr;
                for (parse_scope_t *scope = symbols
                        ? symbols->all_scopes : nullptr;
                     scope && !location.line; scope = scope->all_next)
                    for (parse_sym_entry_t *symbol = scope->first_in_scope;
                         symbol; symbol = symbol->next_in_scope)
                        if (symbol->type && symbol->decl_node
                            && ir_type_eq(module, symbol->type, type)) {
                            location = link_source_location(symbol->decl_node);
                            break;
                        }
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0018"),
                    CORE_STRING_STATIC(
                        "atomic type is not supported by the selected target"),
                    location);
                return false;
            }
        }
    }
    return true;
}

bool
select_storage_placement(n00bb_link_analysis_t *analysis)
{
    if (!analysis || !validate_inline_layout_cycles(analysis)
        || !publish_known_scalar_layouts(analysis)
        || !publish_requested_c_record_layouts(analysis, false)
        || !publish_declared_aggregate_layouts(analysis)
        || !finalize_atomic_aggregate_layouts(analysis)
        || !reject_unlowered_record_layout_requests(analysis)) return false;
    for (size_t i = 0; i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        ir_module_t *module = state ? state->ir_module : nullptr;
        if (!symtab || !module) return false;
        for (parse_scope_t *scope = symtab->all_scopes; scope;
             scope = scope->all_next) {
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                bool local_value = (symbol->kind == PARSE_SYM_VARIABLE
                                    || symbol->kind == PARSE_SYM_PARAM)
                    && symbol->type
                    && scope_has_lexical_lifetime(scope);
                if (!local_value || is_reference_symbol(symbol)
                    || symbol->storage_class_required
                    || (symbol->selected_storage_class.data
                        && symbol->selected_storage_class.u8_bytes)
                    || symbol->storage_duration
                        == PARSE_SYM_STORAGE_DYNAMIC)
                    continue;

                th_type *type = th_apply_subst(ir_module_types(module),
                                               symbol->type);
                ir_descriptor_t *layout = nullptr;
                bool size_known = type
                    && ir_layout_compute(module, type, nullptr);
                if (size_known) {
                    layout = ir_layout_of(module, type,
                                          ir_module_target(module));
                    size_known = layout
                        && ir_layout_is_settled(&layout->size);
                }
                bool dynamic_duration = false;
                n00bb_storage_decision_t decision =
                    n00bb_storage_choose_placement(
                        &(n00bb_storage_placement_facts_t){
                            .has_lexical_lifetime = true,
                            .is_reference = false,
                            .has_aliases = symbol->alias_count
                                || symbol->aliased_by_count,
                            .escapes = symbol->lifetime.escapes,
                            .alias_analysis_complete =
                                symbol->alias_analysis_complete,
                            .size_known = size_known,
                            .size_bytes = size_known
                                ? layout->size.value : 0,
                            .stack_limit_bytes =
                                N00BB_STACK_OBJECT_LIMIT_BYTES,
                        }, &dynamic_duration);
                if (decision >= N00BB_STORAGE_INVALID_TARGET)
                    return false;
                if (dynamic_duration)
                    parse_sym_set_storage_duration(
                        symbol, PARSE_SYM_STORAGE_DYNAMIC);
                else if (symbol->storage_duration
                         == PARSE_SYM_STORAGE_UNSPECIFIED)
                    parse_sym_set_storage_duration(
                        symbol, PARSE_SYM_STORAGE_AUTOMATIC);

                core_link_declaration_t *declaration =
                    link_declaration_for_symbol(analysis, symbol);
                n00bb_link_payload_t *payload = declaration
                    ? core_link_declaration_binding(declaration,
                        CORE_STRING_STATIC("n00bb.semantic.declaration-facts"))
                    : nullptr;
                if (payload) {
                    payload->storage_duration = symbol->storage_duration;
                    payload->selected_storage_class =
                        symbol->selected_storage_class;
                }
            }
        }
    }
    return true;
}

bool
n00bb_has_deferred_shape_layout(n00bb_link_analysis_t *analysis,
                                bool report_unresolved)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        parse_symtab_t *table = state && state->annotations
            ? state->annotations->symtab : nullptr;
        for (parse_scope_t *scope = table ? table->all_scopes : nullptr;
             scope; scope = scope->all_next)
            for (parse_sym_entry_t *symbol = scope->first_in_scope;
                 symbol; symbol = symbol->next_in_scope)
                if (symbol_has_comptime_shape(symbol)) {
                    if (report_unresolved) {
                        core_buffer_t message = {0};
                        core_buffer_printf(&message,
                            "compile-time array extent remains unresolved for '%.*s'",
                            (int)symbol->name.u8_bytes, symbol->name.data);
                        core_diagnostics_report_at(
                            core_translation_unit_diagnostics(
                                analysis->units.data[u]),
                            analysis->units.data[u], CORE_DIAG_FATAL,
                            CORE_STRING_STATIC("N00BB-B0013"),
                            core_string_from_cstr(message.data),
                            link_source_location(symbol->decl_node));
                        core_buffer_release(&message);
                    }
                    return true;
                }
    }
    return false;
}



