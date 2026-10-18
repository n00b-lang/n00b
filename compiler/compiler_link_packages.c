#include <stdio.h>
#include <string.h>

#include "compiler_link.h"
#include "signature_typevars.h"
#include "compiler_tu.h"
#include "record_layout_request.h"
#include "enum_value.h"
#include "flagset_value.h"
#include "global_metadata.h"
#include "lib/buffer.h"
#include "parse/tree_util.h"

core_list_decl(th_trait_requirement);
core_list_decl(th_member);

static bool
append_u64_le(core_buffer_t *buffer, uint64_t value)
{
    uint8_t bytes[8];
    for (size_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t)(value >> (i * 8));
    core_buffer_append(buffer, (const char *)bytes, sizeof(bytes));
    return true;
}

static bool
append_array_shape_metadata(core_buffer_t *buffer, const ir_shape_t *shape)
{
    ir_shape_storage_t storage;
    uint32_t rank = ir_shape_rank(shape);
    if (!buffer || !shape || !rank || !ir_shape_storage(shape, &storage)
        || !storage.classification_known) return false;
    core_buffer_append(buffer, "N0AS", 4);
    append_u32_le(buffer, rank);
    append_u32_le(buffer, storage.flat_prefix_rank);
    append_u32_le(buffer, 1);
    for (uint32_t i = 0; i < rank; i++) {
        ir_shape_extent_t extent = {0};
        if (!ir_shape_extent(shape, i, &extent)
            || extent.kind == IR_SHAPE_EXTENT_COMPTIME) return false;
        append_u32_le(buffer, (uint32_t)extent.kind);
        append_u64_le(buffer, extent.static_value);
    }
    return true;
}

static bool
register_function_array_shapes(parse_grammar_t *grammar,
                               core_translation_unit_t *unit,
                               th_env *environment,
                               parse_sym_entry_t *symbol,
                               const char *qualified_name)
{
    parse_tree_t *signature = child_nt(grammar, symbol->decl_node,
                                       "func-type-signature");
    core_list_t(parse_sym_entry_ptr_t) parameters =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *rest = nullptr;
    collect_ir_parameters(grammar, child_nt(grammar, signature, "param-decl"),
                          &parameters, &rest);
    core_buffer_t encoded = {0};
    core_buffer_append(&encoded, "N0FS", 4);
    bool ok = parameters.len <= UINT32_MAX
        && append_u32_le(&encoded, (uint32_t)parameters.len);
    bool has_shape = false;
    for (size_t i = 0; ok && i < parameters.len; i++) {
        ir_shape_t *shape = parse_sym_binding(parameters.data[i],
            CORE_STRING_STATIC("ir.array-shape"));
        core_buffer_t item = {0};
        if (shape) {
            ok = append_array_shape_metadata(&item, shape);
            has_shape = true;
        }
        ok = ok && item.byte_len <= UINT32_MAX
            && append_u32_le(&encoded, (uint32_t)item.byte_len);
        if (ok && item.byte_len)
            core_buffer_append(&encoded, item.data, item.byte_len);
        core_buffer_release(&item);
    }
    ir_shape_t *return_shape = parse_sym_binding(symbol,
        CORE_STRING_STATIC("ir.return-shape"));
    core_buffer_t result = {0};
    if (ok && return_shape) {
        ok = append_array_shape_metadata(&result, return_shape);
        has_shape = true;
    }
    ok = ok && result.byte_len <= UINT32_MAX
        && append_u32_le(&encoded, (uint32_t)result.byte_len);
    if (ok && result.byte_len)
        core_buffer_append(&encoded, result.data, result.byte_len);
    if (ok && has_shape)
        ok = th_env_register_metadata(environment, TH_ENV_METADATA_FUNCTION,
            qualified_name, "n00b.function-array-shapes", 2,
            encoded.data, encoded.byte_len);
    core_buffer_release(&result);
    core_buffer_release(&encoded);
    core_list_free(parameters);
    (void)unit;
    (void)rest;
    return ok;
}

static bool
register_array_shape_for_symbol(parse_grammar_t *grammar,
                                core_translation_unit_t *unit,
                                th_env *environment,
                                parse_sym_entry_t *symbol,
                                bool is_type)
{
    if (!is_type && symbol->kind == PARSE_SYM_FUNCTION) {
        core_buffer_t qualified = {0};
        bool ok = append_environment_symbol_name(grammar, unit, symbol,
                                                    &qualified)
            && register_function_array_shapes(grammar, unit, environment,
                                              symbol, qualified.data);
        core_buffer_release(&qualified);
        return ok;
    }
    ir_shape_t *shape = parse_sym_binding(symbol,
        CORE_STRING_STATIC("ir.array-shape"));
    if ((!shape && !is_type)
        || (!is_type && symbol->kind != PARSE_SYM_VARIABLE))
        return true;
    core_buffer_t qualified = {0};
    if (is_type) append_module_type_name(unit, symbol->name, &qualified);
    else core_buffer_printf(&qualified, "%.*s.%.*s",
        (int)unit->module.identifier.u8_bytes,
        unit->module.identifier.data, (int)symbol->name.u8_bytes,
        symbol->name.data);
    bool ok = true;
    if (shape) {
        core_buffer_t metadata = {0};
        ok = append_array_shape_metadata(&metadata, shape)
            && th_env_register_metadata(environment,
                is_type ? TH_ENV_METADATA_TYPE : TH_ENV_METADATA_VALUE,
                qualified.data, "n00b.array-shape", 2, metadata.data,
                metadata.byte_len);
        core_buffer_release(&metadata);
    }
    for (parse_sym_entry_t *member = is_type && symbol->member_scope
             ? symbol->member_scope->first_in_scope : nullptr;
         ok && member; member = member->next_in_scope) {
        ir_shape_t *member_shape = parse_sym_binding(member,
            CORE_STRING_STATIC("ir.array-shape"));
        if (!member_shape) continue;
        core_buffer_t key = {0}, metadata = {0};
        core_buffer_printf(&key, "n00b.member-array-shape.%.*s",
            (int)member->name.u8_bytes, member->name.data);
        ok = append_array_shape_metadata(&metadata, member_shape)
            && th_env_register_metadata(environment, TH_ENV_METADATA_TYPE,
                qualified.data, key.data, 2, metadata.data,
                metadata.byte_len);
        core_buffer_release(&key);
        core_buffer_release(&metadata);
    }
    core_buffer_release(&qualified);
    (void)grammar;
    return ok;
}

static bool
register_unit_array_shape_metadata(parse_grammar_t *grammar,
                                   core_translation_unit_t *unit,
                                   th_env *environment)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || state->interface_module) return true;
    core_list_t(parse_scope_ptr_t) values = module_scopes(state);
    core_list_t(parse_scope_ptr_t) types = module_scopes_in_namespace(
        state, CORE_STRING_STATIC("type"));
    bool ok = true;
    for (size_t i = 0; ok && i < values.len; i++)
        for (parse_sym_entry_t *symbol = values.data[i]->first_in_scope;
             ok && symbol; symbol = symbol->next_in_scope) {
            ok = register_array_shape_for_symbol(grammar, unit, environment,
                                                  symbol, false);
            if (!ok) {
                core_buffer_t message = {0};
                core_buffer_printf(&message,
                    "could not export array-shape metadata for '%.*s'",
                    (int)symbol->name.u8_bytes, symbol->name.data);
                core_diagnostics_report(core_translation_unit_diagnostics(
                    unit), unit, CORE_DIAG_FATAL,
                    CORE_STRING_STATIC("N00BB-L0062"),
                    core_string_borrow(message.data, message.byte_len));
                core_buffer_release(&message);
            }
        }
    for (size_t i = 0; ok && i < types.len; i++)
        for (parse_sym_entry_t *symbol = types.data[i]->first_in_scope;
             ok && symbol; symbol = symbol->next_in_scope)
            if (is_named_type_declaration(grammar, symbol)) {
                ok = register_array_shape_for_symbol(grammar, unit,
                    environment, symbol, true);
                if (!ok) {
                    core_buffer_t message = {0};
                    core_buffer_printf(&message,
                        "could not export array-shape metadata for type '%.*s'",
                        (int)symbol->name.u8_bytes, symbol->name.data);
                    core_diagnostics_report(core_translation_unit_diagnostics(
                        unit), unit, CORE_DIAG_FATAL,
                        CORE_STRING_STATIC("N00BB-L0062"),
                        core_string_borrow(message.data, message.byte_len));
                    core_buffer_release(&message);
                }
            }
    core_list_free(values);
    core_list_free(types);
    return ok;
}

static bool
append_u128_le(core_buffer_t *buffer, __uint128_t value)
{
    uint8_t bytes[16];
    for (size_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t)(value >> (i * 8));
    core_buffer_append(buffer, (const char *)bytes, sizeof(bytes));
    return true;
}

static bool
append_static_value(core_buffer_t *buffer,
                    const n00bb_static_value_t *value, size_t depth)
{
    if (!buffer || !value) return false;
    core_buffer_putc(buffer, (char)value->kind);
    switch (value->kind) {
    case N00BB_STATIC_VALUE_BOOL:
        core_buffer_putc(buffer, value->as.boolean ? 1 : 0);
        return true;
    case N00BB_STATIC_VALUE_INT:
        return append_u128_le(buffer, (__uint128_t)value->as.integer);
    case N00BB_STATIC_VALUE_FLOAT: {
        uint64_t bits = 0;
        memcpy(&bits, &value->as.real, sizeof(bits));
        return append_u64_le(buffer, bits);
    }
    case N00BB_STATIC_VALUE_STRING:
        return value->as.string.u8_bytes <= UINT32_MAX
            && append_u32_le(buffer, (uint32_t)value->as.string.u8_bytes)
            && (value->as.string.u8_bytes == 0
                || (core_buffer_append(buffer, value->as.string.data,
                                       value->as.string.u8_bytes), true));
    case N00BB_STATIC_VALUE_NIL:
        return true;
    case N00BB_STATIC_VALUE_VARIANT:
        if (!value->as.variant.arm.data || !value->as.variant.arm.u8_bytes
            || value->as.variant.arm.u8_bytes > UINT32_MAX
            || !value->as.variant.payload
            || !append_u32_le(buffer,
                (uint32_t)value->as.variant.arm.u8_bytes)) return false;
        core_buffer_append(buffer, value->as.variant.arm.data,
                           value->as.variant.arm.u8_bytes);
        return append_static_value(buffer, value->as.variant.payload,
                                   depth + 1);
    case N00BB_STATIC_VALUE_SEQUENCE:
    case N00BB_STATIC_VALUE_TUPLE:
    case N00BB_STATIC_VALUE_RECORD:
        if (value->as.aggregate.count > UINT32_MAX
            || (value->as.aggregate.count && !value->as.aggregate.items)
            || !append_u32_le(buffer,
                (uint32_t)value->as.aggregate.count)) return false;
        for (size_t i = 0; i < value->as.aggregate.count; i++)
            if (!append_static_value(buffer, &value->as.aggregate.items[i],
                                     depth + 1)) return false;
        return true;
    }
    return false;
}

static bool
register_callable_parameter_metadata(parse_grammar_t *grammar,
                                     th_env *environment,
                                     parse_tree_t *declaration,
                                     th_type *function_type,
                                     th_env_metadata_owner_t owner_kind,
                                     const char *owner_name,
                                     const char *key)
{
    th_type *signature = th_resolve(function_type);
    if (!signature || th_kind_of(signature) != TH_FUNCTION) return false;
    parse_tree_t *signature_node = child_nt(grammar, declaration,
                                             "func-type-signature");
    parse_tree_t *trait_signature = signature_node ? nullptr
        : child_nt(grammar, declaration, "trait-func-type-signature");
    parse_tree_t *param_decl = trait_signature
        ? child_nt(grammar, trait_signature, "trait-param-decl")
        : child_nt(grammar, signature_node ? signature_node : declaration,
                   "param-decl");
    if (!param_decl)
        param_decl = child_nt(grammar, declaration, "context-hook-params");
    core_list_t(parse_sym_entry_ptr_t) parameters =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *rest = nullptr;
    collect_ir_parameters(grammar, param_decl, &parameters, &rest);
    size_t count = parameters.len + (rest != nullptr);
    bool ok = count == th_func_nparams(signature)
        + (th_func_rest(signature) != nullptr) && count <= UINT32_MAX;
    core_buffer_t metadata = {0};
    if (ok) {
        core_buffer_append(&metadata, "N0FP", 4);
        core_buffer_putc(&metadata, 1);
        ok = append_u32_le(&metadata, (uint32_t)count);
    }
    for (size_t i = 0; ok && i < count; i++) {
        parse_sym_entry_t *parameter = i < parameters.len
            ? parameters.data[i] : rest;
        size_t name_size = parameter && parameter->name.data
            ? parameter->name.u8_bytes : 0;
        uint8_t flags = parameter && parameter->is_optional ? 1u : 0u;
        if (parameter == rest) flags |= 2u;
        th_mode mode = parameter && parameter->has_mode
            ? parameter->mode : TH_IN;
        ok = parameter && name_size > 0 && name_size <= UINT16_MAX
            && th_mode_is_valid(mode)
            && (!parameter->is_optional || th_mode_is_input_only(mode));
        if (!ok) break;
        core_buffer_putc(&metadata, (char)(name_size & 0xff));
        core_buffer_putc(&metadata, (char)(name_size >> 8));
        core_buffer_putc(&metadata, (char)flags);
        core_buffer_putc(&metadata, (char)mode);
        core_buffer_append(&metadata, parameter->name.data, name_size);
    }
    if (ok)
        ok = th_env_register_metadata(environment,
            owner_kind, owner_name, key, 1,
            metadata.data, metadata.byte_len);
    core_buffer_release(&metadata);
    core_list_free(parameters);
    return ok;
}

static bool
register_typevar_order(th_ctx *types, th_env *environment,
                       parse_sym_entry_t *symbol, th_type *contract,
                       th_env_metadata_owner_t owner_kind,
                       const char *qualified_name, const char *key)
{
    n00bb_signature_typevars_t *variables = parse_sym_binding(symbol,
        N00BB_SIGNATURE_TYPEVARS_KEY);
    if (!variables || !variables->len) return true;
    th_type **free_vars = nullptr;
    size_t free_count = th_free_vars(types, contract, &free_vars);
    core_buffer_t metadata = {0};
    uint32_t count = 0;
    core_buffer_t entries = {0};
    for (size_t i = 0; i < variables->len; i++) {
        th_type *variable = variables->data[i].type
            ? th_resolve(variables->data[i].type) : nullptr;
        size_t position = 0;
        while (variable && position < free_count
               && th_resolve(free_vars[position]) != variable) position++;
        core_string_t name = variables->data[i].name;
        if (!variable || position == free_count || position > UINT32_MAX
            || name.u8_bytes > UINT32_MAX) continue;
        append_u32_le(&entries, (uint32_t)position);
        append_u32_le(&entries, (uint32_t)name.u8_bytes);
        core_buffer_append(&entries, name.data, name.u8_bytes);
        count++;
    }
    append_u32_le(&metadata, count);
    if (entries.byte_len)
        core_buffer_append(&metadata, entries.data, entries.byte_len);
    bool ok = !count || th_env_register_metadata(environment, owner_kind,
        qualified_name, key, 1, metadata.data, metadata.byte_len);
    core_buffer_release(&entries);
    core_buffer_release(&metadata);
    return ok;
}

static bool
register_function_parameter_metadata(parse_grammar_t *grammar,
                                     th_env *environment,
                                     parse_tree_t *declaration,
                                     th_type *function_type,
                                     const char *qualified_name)
{
    return register_callable_parameter_metadata(grammar, environment,
        declaration, function_type, TH_ENV_METADATA_FUNCTION,
        qualified_name, "n00b.function-params");
}

static bool
register_context_callable_parameter_metadata(
    th_env *environment, const char *qualified_name,
    core_list_t(parse_sym_entry_ptr_t) *parameters,
    parse_sym_entry_t *rest)
{
    size_t count = parameters->len + (rest != nullptr);
    if (count > UINT32_MAX) return false;
    core_buffer_t metadata = {0};
    core_buffer_append(&metadata, "N0FP", 4);
    core_buffer_putc(&metadata, 1);
    append_u32_le(&metadata, (uint32_t)count);
    bool ok = true;
    for (size_t i = 0; ok && i < count; i++) {
        parse_sym_entry_t *parameter = i < parameters->len
            ? parameters->data[i] : rest;
        size_t name_size = parameter && parameter->name.data
            ? parameter->name.u8_bytes : 0;
        uint8_t flags = parameter == rest && rest ? 2u : 0u;
        th_mode mode = parameter && parameter->has_mode
            ? parameter->mode : TH_IN;
        ok = parameter && name_size && name_size <= UINT16_MAX
            && mode == TH_IN;
        if (!ok) break;
        core_buffer_putc(&metadata, (char)(name_size & 0xff));
        core_buffer_putc(&metadata, (char)(name_size >> 8));
        core_buffer_putc(&metadata, (char)flags);
        core_buffer_putc(&metadata, (char)mode);
        core_buffer_append(&metadata, parameter->name.data, name_size);
    }
    if (ok)
        ok = th_env_register_metadata(environment, TH_ENV_METADATA_FUNCTION,
            qualified_name, "n00b.function-params", 1, metadata.data,
            metadata.byte_len);
    core_buffer_release(&metadata);
    return ok;
}

static uint32_t
n00bb_visibility_code(core_string_t visibility)
{
    if (core_string_eq(visibility, CORE_STRING_STATIC("private"))) return 1;
    if (core_string_eq(visibility, CORE_STRING_STATIC("public"))) return 2;
    return 0;
}

static bool
register_type_member_metadata(parse_grammar_t *grammar,
                              core_translation_unit_t *unit,
                              th_env *environment,
                              parse_sym_entry_t *type_symbol,
                              const char *qualified_name)
{
    parse_scope_t *scope = type_symbol ? type_symbol->member_scope : nullptr;
    if (!scope) return true;
    uint32_t type_visibility = n00bb_visibility_code(type_symbol->visibility);
    if (!type_visibility) type_visibility = 2;
    size_t count = 0;
    for (parse_sym_entry_t *member = scope->first_in_scope; member;
         member = member->next_in_scope) {
        bool relevant = member->is_field ||
            (member->decl_node && nt_is(grammar, member->decl_node,
                                       "variant-decl-one")) ||
            (member->decl_node && nt_is(grammar, member->decl_node,
                                       "enum-item")) ||
            (member->decl_node && nt_is(grammar, member->decl_node,
                                       "flagset-item"));
        if (relevant) count++;
    }
    if (count > UINT32_MAX) return false;
    core_buffer_t data = {0};
    bool ok = append_u32_le(&data, type_visibility) &&
              append_u32_le(&data, (uint32_t)count);
    for (parse_sym_entry_t *member = scope->first_in_scope;
         ok && member; member = member->next_in_scope) {
        bool relevant = member->is_field ||
            (member->decl_node && nt_is(grammar, member->decl_node,
                                       "variant-decl-one")) ||
            (member->decl_node && nt_is(grammar, member->decl_node,
                                       "enum-item")) ||
            (member->decl_node && nt_is(grammar, member->decl_node,
                                       "flagset-item"));
        if (!relevant) continue;
        uint32_t name_size = (uint32_t)member->name.u8_bytes;
        uint32_t flags = (member->has_bit_offset ? 1u : 0u) |
                         (member->has_bit_width ? 2u : 0u);
        ok = member->name.u8_bytes <= UINT32_MAX;
        if (!ok) break;
        append_u32_le(&data, name_size);
        core_buffer_append(&data, member->name.data,
                           member->name.u8_bytes);
        ok = append_u32_le(&data,
                           n00bb_visibility_code(member->visibility)) &&
             append_u32_le(&data, flags) &&
             append_u64_le(&data, member->bit_offset) &&
             append_u64_le(&data, member->bit_width);
    }
    if (ok)
        ok = th_env_register_metadata(environment, TH_ENV_METADATA_TYPE,
            qualified_name, "n00b.type-members", 1, data.data,
            data.byte_len);
    core_buffer_release(&data);
    (void)unit;
    return ok;
}

static bool
register_enum_values_metadata(parse_grammar_t *grammar,
                              core_translation_unit_t *unit,
                              th_env *environment,
                              parse_sym_entry_t *type_symbol,
                              const char *qualified_name)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_scope_t *members = type_symbol ? type_symbol->member_scope : nullptr;
    if (!state || !members || !type_symbol->decl_node ||
        !nt_is(grammar, type_symbol->decl_node, "enum-stmt")) return true;

    core_list_t(parse_sym_entry_ptr_t) items =
        core_list_new(parse_sym_entry_ptr_t);
    for (parse_sym_entry_t *item = members->first_in_scope; item;
         item = item->next_in_scope)
        if (item->decl_node &&
            nt_is(grammar, item->decl_node, "enum-item"))
            core_list_push(items, item);
    size_t count = items.len;
    if (count > UINT32_MAX) {
        core_list_free(items);
        return false;
    }

    core_buffer_t data = {0};
    core_buffer_append(&data, "N0EV", 4);
    bool ok = append_u32_le(&data, (uint32_t)count);
    n00bb_enum_value_t previous = {0};
    n00bb_enum_range_t range = {0};
    bool have_previous = false;
    for (size_t index = items.len; ok && index > 0; index--) {
        parse_sym_entry_t *item = items.data[index - 1];
        n00bb_enum_value_t value = {0};
        if (item->has_comptime_value && item->comptime_value)
            value = *(n00bb_enum_value_t *)item->comptime_value;
        else if (have_previous) {
            if (previous.negative) {
                value.magnitude = previous.magnitude - 1;
                value.negative = value.magnitude != 0;
            }
            else if (previous.magnitude == ~(ir_uint_t)0) {
                ok = false;
                break;
            }
            else value.magnitude = previous.magnitude + 1;
        }
        n00bb_enum_value_t *stored = core_arena_alloc(
            &state->semantic_values, sizeof(*stored));
        if (!stored) {
            ok = false;
            break;
        }
        *stored = value;
        item->comptime_value = stored;
        item->has_comptime_value = true;
        size_t name_size = item->name.u8_bytes;
        ok = name_size <= UINT32_MAX && append_u32_le(&data,
            (uint32_t)name_size);
        if (ok) core_buffer_append(&data, item->name.data, name_size);
        ok = ok && append_u64_le(&data, (uint64_t)value.magnitude)
                && append_u64_le(&data, (uint64_t)(value.magnitude >> 64));
        if (ok) {
            char negative = value.negative;
            core_buffer_append(&data, &negative, 1);
        }
        previous = value;
        have_previous = true;
        n00bb_enum_range_include(&range, value);
    }
    if (ok) ok = n00bb_bind_enum_range(state->types, &state->semantic_values,
                                 type_symbol, range);
    if (ok)
        ok = th_env_register_metadata(environment, TH_ENV_METADATA_TYPE,
            qualified_name, "n00b.enum-values", 1, data.data,
            data.byte_len);
    core_buffer_release(&data);
    core_list_free(items);
    return ok;
}

static bool
register_flagset_values_metadata(parse_grammar_t *grammar,
                                 core_translation_unit_t *unit,
                                 th_env *environment,
                                 parse_sym_entry_t *type_symbol,
                                 const char *qualified_name)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_scope_t *members = type_symbol ? type_symbol->member_scope : nullptr;
    if (!state || !members || !type_symbol->decl_node
        || !nt_is(grammar, type_symbol->decl_node, "flagset-stmt"))
        return true;
    n00bb_flagset_info_t *info = parse_sym_binding(type_symbol,
        CORE_STRING_STATIC("n00bb.flagset-info"));
    size_t count = 0;
    for (parse_sym_entry_t *item = members->first_in_scope; item;
         item = item->next_in_scope)
        if (item->decl_node
            && nt_is(grammar, item->decl_node, "flagset-item")) count++;
    if (!info || count > UINT32_MAX) return false;
    core_buffer_t data = {0};
    core_buffer_append(&data, "N0FV", 4);
    bool ok = append_u128_le(&data, info->all_mask)
        && append_u32_le(&data, info->atomic_count)
        && append_u32_le(&data, (uint32_t)count);
    for (parse_sym_entry_t *item = members->first_in_scope;
         ok && item; item = item->next_in_scope) {
        if (!item->decl_node
            || !nt_is(grammar, item->decl_node, "flagset-item")) continue;
        n00bb_flagset_value_t *value = item->has_comptime_value
            ? item->comptime_value : nullptr;
        ok = value && item->name.u8_bytes <= UINT32_MAX
            && append_u32_le(&data, (uint32_t)item->name.u8_bytes);
        if (!ok) break;
        core_buffer_append(&data, item->name.data, item->name.u8_bytes);
        ok = append_u128_le(&data, value->mask)
            && append_u32_le(&data, value->declaration_order);
        if (ok) core_buffer_putc(&data, value->atomic ? 1 : 0);
    }
    if (ok)
        ok = th_env_register_metadata(environment, TH_ENV_METADATA_TYPE,
            qualified_name, "n00b.flagset-values", 1,
            data.data, data.byte_len);
    core_buffer_release(&data);
    return ok;
}

bool
register_unit_environment(parse_grammar_t *grammar, th_env *environment,
                          core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->types || !state->annotations
        || !state->annotations->symtab) return true;
    if (state->interface_module) return true;
    core_list_t(parse_scope_ptr_t) module_level_scopes = module_scopes(state);
    const char *module_name = unit->module.identifier.data;
    char *module_text = core_strndup(module_name,
                                     unit->module.identifier.u8_bytes);
    core_string_t package_id = n00bb_package_id(unit);
    char *package_text = core_strndup(package_id.data, package_id.u8_bytes);
    bool ok = true;

    core_list_t(parse_scope_ptr_t) type_module_scopes =
        module_scopes_in_namespace(state, CORE_STRING_STATIC("type"));
    for (size_t s = 0; ok && s < type_module_scopes.len; s++) {
        for (parse_sym_entry_t *symbol = type_module_scopes.data[s]->first_in_scope;
             ok && symbol; symbol = symbol->next_in_scope) {
            if (!is_named_type_declaration(grammar, symbol)
                || nt_is(grammar, symbol->decl_node, "trait-decl")) continue;
            core_buffer_t qualified = {0};
            append_module_type_name(unit, symbol->name, &qualified);
            ok = th_env_register_type(environment, qualified.data,
                module_text, package_text, symbol_visibility(symbol));
            th_env_type_shape_kind_t shape_kind = TH_ENV_TYPE_SHAPE_NONE;
            th_type *shape = nullptr;
            if (ok && !exported_type_shape(grammar, unit, symbol,
                                            &shape_kind, &shape)) ok = false;
            if (ok && shape_kind != TH_ENV_TYPE_SHAPE_NONE)
                ok = th_env_register_type_shape(environment, qualified.data,
                                                 shape_kind, shape);
            if (ok && shape_kind != TH_ENV_TYPE_SHAPE_NONE)
                ok = register_type_member_metadata(grammar, unit,
                    environment, symbol, qualified.data);
            if (ok && shape)
                ok = register_typevar_order(state->types, environment,
                    symbol, shape, TH_ENV_METADATA_TYPE, qualified.data,
                    "n00b.type-typevars");
            n00bb_record_layout_request_t *layout = parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.record-layout-request"));
            if (ok && layout && !layout->alignment_expression) {
                uint8_t policy = (uint8_t)layout->policy;
                ok = th_env_register_metadata(environment,
                    TH_ENV_METADATA_TYPE, qualified.data,
                    "n00b.record-layout", 1, &policy, sizeof(policy));
            }
            if (ok && shape_kind == TH_ENV_TYPE_SHAPE_ENUM)
                ok = register_enum_values_metadata(grammar, unit,
                    environment, symbol, qualified.data);
            if (ok && shape_kind == TH_ENV_TYPE_SHAPE_BITSET)
                ok = register_flagset_values_metadata(grammar, unit,
                    environment, symbol, qualified.data);
            n00bb_callback_contract_t *callback = parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.callback-contract"));
            if (ok && callback)
                ok = th_env_register_metadata(environment,
                    TH_ENV_METADATA_TYPE, qualified.data,
                    "n00b.callback", 1, qualified.data,
                    qualified.byte_len);
            core_buffer_release(&qualified);
        }
    }
    core_list_free(type_module_scopes);
    if (!ok)
        core_diagnostics_report(core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0063"),
            CORE_STRING_STATIC("failed while exporting named type declarations"));

    for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
         ok && scope; scope = scope->all_next) {
        bool trait_contract = nt_is(grammar, scope->node, "trait-decl");
        bool callback_contract = nt_is(grammar, scope->node,
                                       "callback-decl");
        if (!trait_contract && !callback_contract) continue;
        if (trait_contract) {
            parse_sym_entry_t *trait_symbol =
                core_tree_node_value(scope->node).sym;
            if (trait_symbol && scope != trait_symbol->member_scope)
                continue;
            core_buffer_t qualified = {0};
            core_buffer_printf(&qualified, "%s.%.*s", module_text,
                (int)scope->name.u8_bytes, scope->name.data);
            ok = trait_symbol && trait_symbol->type && qualified.data
                && th_kind_of(th_resolve(trait_symbol->type)) == TH_TRAIT
                && th_env_register_trait_contracts(environment,
                    qualified.data, module_text, package_text,
                    symbol_visibility(trait_symbol), trait_symbol->type);
            for (parse_sym_entry_t *method = scope->first_in_scope;
                 ok && method; method = method->next_in_scope) {
                if (method->kind != PARSE_SYM_FUNCTION || !method->type
                    || !method->decl_node) continue;
                core_buffer_t key = {0};
                core_buffer_printf(&key, "n00b.trait-method-params.%.*s",
                    (int)method->name.u8_bytes, method->name.data);
                ok = register_callable_parameter_metadata(grammar,
                    environment, method->decl_node,
                    method->callable_contract ? method->callable_contract
                                              : method->type,
                    TH_ENV_METADATA_TRAIT, qualified.data, key.data);
                core_buffer_release(&key);
            }
            core_buffer_release(&qualified);
            continue;
        }
        core_list_t(th_member) methods = core_list_new(th_member);
        core_list_t(th_member) callback_roles = core_list_new(th_member);
        for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
             symbol = symbol->next_in_scope) {
            if (symbol->kind != PARSE_SYM_FUNCTION || !symbol->type) continue;
            th_type *role_contract = symbol->callable_contract
                ? symbol->callable_contract
                : th_env_signature_contract(state->types, symbol->type,
                    symbol->error_type ? symbol->error_type
                                       : th_name(state->types, "nil"));
            if (callback_contract)
                core_list_push(callback_roles, ((th_member){
                    .name = symbol->name.data,
                    .type = role_contract,
                }));
            if (callback_contract && ok) {
                core_buffer_t owner = {0}, key = {0};
                core_buffer_printf(&owner, "%s.%.*s", module_text,
                    (int)scope->name.u8_bytes, scope->name.data);
                core_buffer_printf(&key, "n00b.callback-params.%.*s",
                    (int)symbol->name.u8_bytes, symbol->name.data);
                ok = owner.data && key.data
                    && register_callable_parameter_metadata(grammar,
                        environment, symbol->decl_node, symbol->type,
                        TH_ENV_METADATA_TYPE, owner.data, key.data);
                core_buffer_release(&key);
                core_buffer_release(&owner);
            }
            if (!callback_contract || core_string_eq(symbol->name,
                    CORE_STRING_STATIC("setup"))) {
                th_type *contract = callback_contract
                    ? callback_setup_dispatch_contract(state->types,
                                                        symbol->type)
                    : role_contract;
                core_list_push(methods, ((th_member){
                    .name = symbol->name.data,
                    .type = contract,
                }));
            }
        }
        if (callback_roles.len) {
            th_type *role_record = th_record(state->types,
                callback_roles.data, callback_roles.len);
            core_buffer_t qualified = {0};
            core_buffer_printf(&qualified, "%s.__callback_contract__%.*s",
                module_text, (int)scope->name.u8_bytes, scope->name.data);
            ok = role_record && th_env_register_value(environment,
                qualified.data, qualified.data, module_text, package_text,
                TH_ENV_PRIVATE, role_record);
            core_buffer_release(&qualified);
            parse_sym_entry_t *callback_symbol =
                core_tree_node_value(scope->node).sym;
            core_buffer_t type_name = {0};
            core_buffer_printf(&type_name, "%s.%.*s", module_text,
                (int)scope->name.u8_bytes, scope->name.data);
            ok = ok && callback_symbol && type_name.data
                && register_typevar_order(state->types, environment,
                    callback_symbol, role_record, TH_ENV_METADATA_TYPE,
                    type_name.data, "n00b.type-typevars");
            core_buffer_release(&type_name);
        }
        if (ok && methods.len) {
            th_type *method_record = th_record(state->types, methods.data,
                                               methods.len);
            core_buffer_t qualified = {0};
            core_buffer_printf(&qualified,
                callback_contract ? "%s.__callback_setup__%.*s"
                                  : "%s.%.*s",
                module_text, (int)scope->name.u8_bytes, scope->name.data);
            parse_sym_entry_t *trait_symbol =
                core_tree_node_value(scope->node).sym;
            ok = th_env_register_trait_contracts(environment, qualified.data,
                module_text, package_text, symbol_visibility(trait_symbol),
                method_record);
            core_buffer_release(&qualified);
        }
        core_list_free(callback_roles);
        core_list_free(methods);
    }
    if (!ok && !core_diagnostics_failed(
            core_translation_unit_diagnostics(unit)))
        core_diagnostics_report(core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0064"),
            CORE_STRING_STATIC("failed while exporting trait contracts"));

    for (size_t s = 0; ok && s < module_level_scopes.len; s++) {
        for (parse_sym_entry_t *symbol = module_level_scopes.data[s]->first_in_scope;
             ok && symbol; symbol = symbol->next_in_scope) {
            if (context_symbol(grammar, symbol) && symbol->type
                && symbol->decl_node) {
                core_buffer_t qualified = {0};
                core_buffer_printf(&qualified, "%s.%.*s", module_text,
                    (int)symbol->name.u8_bytes, symbol->name.data);
                ok = th_env_register_value(environment, qualified.data,
                    symbol->name.data, module_text, package_text,
                    symbol_visibility(symbol), symbol->type);
                uint8_t hook_mask = 0;
                static const struct {
                    const char *nonterminal;
                    const char *suffix;
                    uint8_t bit;
                } context_hooks[] = {
                    {"context-enter", "enter", 1u << 0},
                    {"context-exit", "exit", 1u << 1},
                    {"context-error", "error", 1u << 2},
                    {"context-ensure", "ensure", 1u << 3},
                };
                for (size_t h = 0; ok && h <
                     sizeof(context_hooks) / sizeof(context_hooks[0]); h++) {
                    const char *role = n00bb_context_hook_role(
                        context_hooks[h].nonterminal);
                    parse_tree_t *hook = role ? parse_sym_binding(symbol,
                        core_string_borrow(role, strlen(role))) : nullptr;
                    if (!hook) continue;
                    th_type *hook_type = core_tree_node_value(hook).type;
                    th_type *contract = hook_type
                        ? th_env_signature_contract(state->types, hook_type,
                            th_func_error(hook_type)
                                ? th_func_error(hook_type)
                                : th_name(state->types, "nil")) : nullptr;
                    core_buffer_t hook_name = {0}, hook_qualified = {0};
                    core_buffer_printf(&hook_name,
                        "__n00bb_context_%.*s_%s",
                        (int)symbol->name.u8_bytes, symbol->name.data,
                        context_hooks[h].suffix);
                    core_buffer_printf(&hook_qualified, "%s.%s",
                        module_text, hook_name.data);
                    ok = contract && th_env_register_function_contract(
                        environment, hook_qualified.data, hook_name.data,
                        module_text, package_text, TH_ENV_PRIVATE, contract);
                    core_list_t(parse_sym_entry_ptr_t) hook_parameters =
                        core_list_new(parse_sym_entry_ptr_t);
                    parse_sym_entry_t *hook_rest = nullptr;
                    collect_ir_parameters(grammar,
                        child_nt(grammar, symbol->decl_node, "param-decl"),
                        &hook_parameters, &hook_rest);
                    collect_ir_parameters(grammar,
                        child_nt(grammar, hook, "context-hook-params"),
                        &hook_parameters, &hook_rest);
                    if (ok)
                        ok = register_context_callable_parameter_metadata(
                            environment, hook_qualified.data,
                            &hook_parameters, hook_rest);
                    core_list_free(hook_parameters);
                    core_buffer_release(&hook_qualified);
                    core_buffer_release(&hook_name);
                    if (ok) hook_mask |= context_hooks[h].bit;
                }
                core_buffer_t context_metadata = {0};
                core_list_t(parse_sym_entry_ptr_t) context_params =
                    core_list_new(parse_sym_entry_ptr_t);
                parse_sym_entry_t *context_rest = nullptr;
                collect_ir_parameters(grammar,
                    child_nt(grammar, symbol->decl_node, "param-decl"),
                    &context_params, &context_rest);
                if (context_rest)
                    core_list_push(context_params, context_rest);
                size_t serialized_param_count = context_params.len;
                ok = ok && serialized_param_count <= UINT32_MAX
                    && serialized_param_count <= UINT16_MAX;
                for (size_t p = 0; ok && p < serialized_param_count; p++) {
                    parse_sym_entry_t *parameter = context_params.data[p];
                    if (!parameter || !parameter->is_optional) continue;
                    parse_tree_t *default_expr = child_nt(grammar,
                        parameter->decl_node, "expression");
                    core_list_t(parse_sym_entry_ptr_t) dependencies =
                        core_list_new(parse_sym_entry_ptr_t);
                    context_default_dependencies(default_expr,
                        &context_params, &dependencies);
                    th_type *helper_type = context_default_ir_type(
                        state->types, symbol->type, &dependencies,
                        parameter->type);
                    th_type *helper_contract = helper_type
                        ? th_env_signature_contract(state->types, helper_type,
                            th_func_error(helper_type)
                                ? th_func_error(helper_type)
                                : th_name(state->types, "nil")) : nullptr;
                    char helper_name[256];
                    core_buffer_t helper_qualified = {0};
                    bool named = context_default_name(symbol, p, helper_name,
                                                       sizeof(helper_name));
                    if (named)
                        core_buffer_printf(&helper_qualified, "%s.%s",
                            module_text, helper_name);
                    ok = named && helper_contract
                        && th_env_register_function_contract(environment,
                            helper_qualified.data, helper_name, module_text,
                            package_text, TH_ENV_PRIVATE, helper_contract);
                    if (ok)
                        ok = register_context_callable_parameter_metadata(
                            environment, helper_qualified.data,
                            &dependencies, nullptr);
                    core_buffer_release(&helper_qualified);
                    core_list_free(dependencies);
                }
                if (ok) {
                    core_buffer_putc(&context_metadata,
                                     (char)(hook_mask | 0x80u));
                    append_u32_le(&context_metadata,
                        (uint32_t)serialized_param_count);
                    for (size_t p = 0; ok && p < serialized_param_count; p++) {
                        parse_sym_entry_t *parameter = context_params.data[p];
                        size_t name_len = parameter && parameter->name.data
                            ? parameter->name.u8_bytes : 0;
                        uint8_t flags = parameter && parameter->is_optional
                            ? 1u : 0u;
                        if (parameter == context_rest) flags |= 2u;
                        if (parameter && parameter->is_optional) flags |= 4u;
                        th_mode mode = parameter && parameter->has_mode
                            ? parameter->mode : TH_IN;
                        ok = parameter && name_len > 0
                            && name_len <= UINT16_MAX;
                        if (!ok) break;
                        core_buffer_putc(&context_metadata,
                            (char)(name_len & 0xff));
                        core_buffer_putc(&context_metadata,
                            (char)(name_len >> 8));
                        core_buffer_putc(&context_metadata, (char)flags);
                        core_buffer_putc(&context_metadata, (char)mode);
                        core_buffer_append(&context_metadata,
                            parameter->name.data, name_len);
                        if (flags & 4u) {
                            parse_tree_t *default_expr = child_nt(grammar,
                                parameter->decl_node, "expression");
                            core_list_t(parse_sym_entry_ptr_t) dependencies =
                                core_list_new(parse_sym_entry_ptr_t);
                            context_default_dependencies(default_expr,
                                &context_params, &dependencies);
                            ok = dependencies.len <= UINT16_MAX;
                            if (ok) {
                                core_buffer_putc(&context_metadata,
                                    (char)(dependencies.len & 0xff));
                                core_buffer_putc(&context_metadata,
                                    (char)(dependencies.len >> 8));
                            }
                            for (size_t d = 0; ok && d < dependencies.len;
                                 d++) {
                                size_t index = 0;
                                while (index < context_params.len
                                    && context_params.data[index]
                                        != dependencies.data[d]) index++;
                                ok = index < context_params.len
                                    && index <= UINT16_MAX;
                                if (ok) {
                                    core_buffer_putc(&context_metadata,
                                        (char)(index & 0xff));
                                    core_buffer_putc(&context_metadata,
                                        (char)(index >> 8));
                                }
                            }
                            core_list_free(dependencies);
                        }
                    }
                }
                core_list_free(context_params);
                if (ok)
                    ok = th_env_register_metadata(environment,
                        TH_ENV_METADATA_VALUE, qualified.data,
                        "n00b.context", 1, context_metadata.data,
                        context_metadata.byte_len);
                core_buffer_release(&context_metadata);
                core_buffer_release(&qualified);
                continue;
            }
            if (symbol->kind == PARSE_SYM_VARIABLE && symbol->type
                && symbol_binding_node(symbol)) {
                core_buffer_t qualified = {0};
                core_buffer_printf(&qualified, "%s.%.*s", module_text,
                    (int)symbol->name.u8_bytes, symbol->name.data);
                ok = th_env_register_value(environment, qualified.data,
                    symbol->name.data, module_text, package_text,
                    symbol_visibility(symbol), symbol->type);
                if (ok) {
                    uint8_t metadata[12] = {0};
                    metadata[0] = (uint8_t)symbol->mutability;
                    metadata[1] = (uint8_t)symbol->phases;
                    metadata[2] = (uint8_t)symbol->mutable_phases;
                    metadata[3] = (uint8_t)symbol->initializer_phases;
                    metadata[4] = (uint8_t)symbol->storage_duration;
                    metadata[5] = symbol->is_thread_local ? 1 : 0;
                    metadata[7] = global_has_explicit_initializer(grammar,
                        symbol) ? 1 : 0;
                    const n00bb_static_value_t *static_value =
                        symbol->has_comptime_value && symbol->comptime_value
                        ? symbol->comptime_value : nullptr;
                    metadata[10] = static_value ? 1 : 0;
                    metadata[11] = static_value
                        ? (uint8_t)static_value->kind : 0;
                    core_buffer_t payload = {0};
                    ok = n00bb_global_metadata_append_storage(&payload,
                        metadata, symbol->requested_storage_class,
                        symbol->storage_class_required);
                    if (ok && static_value)
                        ok = append_static_value(&payload, static_value, 0);
                    ok = ok && th_env_register_metadata(environment,
                        TH_ENV_METADATA_VALUE, qualified.data,
                        "n00b.global", 1, payload.data, payload.byte_len);
                    core_buffer_release(&payload);
                }
                core_buffer_release(&qualified);
                continue;
            }
            if (symbol->kind != PARSE_SYM_FUNCTION || !symbol->type) continue;
            core_buffer_t qualified = {0};
            ok = append_environment_symbol_name(grammar, unit, symbol,
                                                   &qualified);
            th_type *contract = symbol->callable_contract
                ? symbol->callable_contract
                : th_env_signature_contract(state->types, symbol->type,
                    symbol->error_type ? symbol->error_type
                                       : th_name(state->types, "nil"));
            const char *export_failure = "function contract";
            ok = ok && th_env_register_function_contract(environment,
                qualified.data,
                symbol->name.data, module_text, package_text,
                symbol_is_public_declaration(symbol)
                    ? TH_ENV_PUBLIC : TH_ENV_PRIVATE, contract);
            if (ok) {
                export_failure = "function parameter metadata";
                ok = register_function_parameter_metadata(grammar,
                    environment, symbol->decl_node, symbol->type,
                    qualified.data);
            }
            if (ok) {
                export_failure = "function type-variable metadata";
                ok = register_typevar_order(state->types, environment,
                    symbol, contract, TH_ENV_METADATA_FUNCTION,
                    qualified.data, "n00b.function-typevars");
            }
            core_buffer_release(&qualified);
            if (!ok && !core_diagnostics_failed(
                    core_translation_unit_diagnostics(unit))) {
                core_buffer_t message = {0};
                core_buffer_printf(&message, "failed to register %s for '%.*s'",
                    export_failure,
                    (int)symbol->name.u8_bytes, symbol->name.data);
                core_diagnostics_report(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0066"),
                    core_string_borrow(message.data, message.byte_len));
                core_buffer_release(&message);
            }
        }
    }
    core_list_free(module_level_scopes);
    if (ok) {
        ok = register_unit_array_shape_metadata(grammar, unit, environment);
        if (!ok && !core_diagnostics_failed(
                core_translation_unit_diagnostics(unit)))
            core_diagnostics_report(core_translation_unit_diagnostics(unit),
                unit, CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0065"),
                CORE_STRING_STATIC("failed while exporting array-shape metadata"));
    }
    core_free(package_text);
    core_free(module_text);
    return ok;
}

static th_env_metadata_owner_t
package_export_metadata_kind(parse_sym_entry_t *symbol)
{
    return symbol && ((symbol->member_scope
            && core_string_eq(symbol->member_scope->scope_tag,
                              CORE_STRING_STATIC("trait")))
        || core_string_eq(symbol->adt_kind, CORE_STRING_STATIC("trait")))
        ? TH_ENV_METADATA_TRAIT
        : symbol && symbol->kind == PARSE_SYM_TYPEDEF
        ? TH_ENV_METADATA_TYPE
        : symbol && symbol->kind == PARSE_SYM_FUNCTION
        ? TH_ENV_METADATA_FUNCTION : TH_ENV_METADATA_VALUE;
}

static bool
append_package_export_origin(th_env *environment,
                             th_env_metadata_owner_t kind,
                             const char *external_name,
                             core_translation_unit_t *source_unit,
                             const char *source_name)
{
    if (!environment || !external_name || !source_unit || !source_name)
        return false;
    size_t module_size = source_unit->module.identifier.u8_bytes;
    size_t name_size = strlen(source_name);
    if (module_size > UINT32_MAX || name_size > UINT32_MAX) return false;
    core_buffer_t payload = {0};
    core_buffer_append(&payload, "N0EO", 4);
    bool ok = append_u32_le(&payload, (uint32_t)module_size)
        && append_u32_le(&payload, (uint32_t)name_size);
    if (ok) {
        core_buffer_append(&payload, source_unit->module.identifier.data,
                           module_size);
        core_buffer_append(&payload, source_name, name_size);
        ok = th_env_register_metadata(environment, kind, external_name,
            "n00b.export-origin", 1, payload.data, payload.byte_len);
    }
    core_buffer_release(&payload);
    return ok;
}

static th_type *
environment_function_contract(const th_env *environment,
                              const char *qualified_name)
{
    for (size_t i = 0; environment
            && i < th_env_declaration_count(environment); i++) {
        th_env_declaration_view_t declaration = {0};
        if (th_env_declaration_at(environment, i, &declaration)
            && declaration.kind == TH_ENV_DECLARATION_FUNCTION
            && declaration.qualified_name
            && !strcmp(declaration.qualified_name, qualified_name))
            return declaration.contract;
    }
    return nullptr;
}

static bool
copy_package_export_metadata(const th_env *source, th_env *destination,
                             th_env_metadata_owner_t kind,
                             const char *source_name,
                             const char *external_name,
                             bool include_members, const char *only_key)
{
    for (size_t i = 0; source && i < th_env_metadata_count(source); i++) {
        th_env_metadata_view_t metadata = {0};
        if (!th_env_metadata_at(source, i, &metadata)
            || metadata.owner_kind != kind
            || strcmp(metadata.owner_name, source_name)
            || (only_key && strcmp(metadata.key, only_key))
            || (!include_members
                && !strncmp(metadata.key, "n00b.member-", 12))) continue;
        if (!th_env_register_metadata(destination, kind, external_name,
                metadata.key, metadata.version, metadata.data,
                metadata.size)) return false;
    }
    return true;
}

static bool
register_package_context_callable(th_env *environment,
                                  core_translation_unit_t *interface_unit,
                                  n00bb_package_declaration_t *package,
                                  core_translation_unit_t *source_unit,
                                  const char *source_name,
                                  const char *external_name)
{
    core_buffer_t source_qualified = {0}, external_qualified = {0};
    core_buffer_printf(&source_qualified, "%.*s.%s",
        (int)source_unit->module.identifier.u8_bytes,
        source_unit->module.identifier.data, source_name);
    core_buffer_printf(&external_qualified, "%.*s.%s",
        (int)package->name.u8_bytes, package->name.data, external_name);
    th_type *contract = environment_function_contract(
        environment, source_qualified.data);
    bool ok = contract && th_env_register_function_contract(environment,
        external_qualified.data, external_name, package->name.data,
        n00bb_package_id(interface_unit).data, TH_ENV_PRIVATE, contract);
    if (ok)
        ok = copy_package_export_metadata(environment, environment,
            TH_ENV_METADATA_FUNCTION, source_qualified.data,
            external_qualified.data, true, nullptr);
    if (ok)
        ok = append_package_export_origin(environment,
            TH_ENV_METADATA_FUNCTION, external_qualified.data, source_unit,
            source_qualified.data);
    core_buffer_release(&external_qualified);
    core_buffer_release(&source_qualified);
    return ok;
}

static bool
register_package_context_callables(parse_grammar_t *grammar,
                                   th_env *environment,
                                   core_translation_unit_t *interface_unit,
                                   n00bb_package_declaration_t *package,
                                   parse_sym_entry_t *binding,
                                   parse_sym_entry_t *source,
                                   core_translation_unit_t *source_unit)
{
    static const struct {
        const char *nonterminal;
        const char *suffix;
    } hooks[] = {
        {"context-enter", "enter"},
        {"context-exit", "exit"},
        {"context-error", "error"},
        {"context-ensure", "ensure"},
    };
    for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
        const char *role = n00bb_context_hook_role(hooks[i].nonterminal);
        if (!role || !parse_sym_binding(source,
                core_string_borrow(role, strlen(role)))) continue;
        char source_name[256], external_name[256];
        if (!context_hook_name(source, hooks[i].suffix, source_name,
                               sizeof(source_name))
            || !context_hook_name(binding, hooks[i].suffix, external_name,
                                  sizeof(external_name))
            || !register_package_context_callable(environment, interface_unit,
                package, source_unit, source_name, external_name))
            return false;
    }
    core_list_t(parse_sym_entry_ptr_t) parameters =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *rest = nullptr;
    collect_ir_parameters(grammar,
        child_nt(grammar, source->decl_node, "param-decl"),
        &parameters, &rest);
    if (rest) core_list_push(parameters, rest);
    bool ok = true;
    for (size_t i = 0; ok && i < parameters.len; i++) {
        parse_sym_entry_t *parameter = parameters.data[i];
        if (!parameter || !parameter->is_optional) continue;
        char source_name[256], external_name[256];
        ok = context_default_name(source, i, source_name, sizeof(source_name))
            && context_default_name(binding, i, external_name,
                                    sizeof(external_name))
            && register_package_context_callable(environment, interface_unit,
                package, source_unit, source_name, external_name);
    }
    core_list_free(parameters);
    return ok;
}

static bool
register_package_export_declaration(n00bb_link_analysis_t *analysis,
                                    th_env *environment,
                                    core_translation_unit_t *interface_unit,
                                    n00bb_package_declaration_t *package,
                                    parse_sym_entry_t *binding)
{
    parse_sym_entry_t *source = canonical_symbol(binding);
    core_translation_unit_t *source_unit = source
        ? unit_for_symbol(analysis->grammar, &analysis->units, source)
        : nullptr;
    if (!source || !source_unit || !source->type) return false;
    n00bb_tu_state_t *source_state = tu_state(source_unit);
    core_buffer_t external = {0}, origin = {0};
    core_buffer_printf(&external, "%.*s.%.*s", (int)package->name.u8_bytes,
        package->name.data, (int)binding->name.u8_bytes, binding->name.data);
    th_env_metadata_owner_t metadata_kind =
        package_export_metadata_kind(source);
    bool include_members = true;
    bool ok = external.data != nullptr
        && append_function_contract_identity(source_unit, binding, &external);
    if (ok && source->kind == PARSE_SYM_TYPEDEF) {
        if ((source->decl_node && nt_is(analysis->grammar, source->decl_node,
                                       "trait-decl"))
            || (!source->decl_node && core_string_eq(source->adt_kind,
                                                     CORE_STRING_STATIC("trait")))) {
            size_t method_count = th_trait_len(source->type);
            const th_member *methods = method_count
                ? th_trait_method(source->type, 0) : nullptr;
            size_t requirement_count =
                th_trait_requirement_count(source->type);
            core_list_t(th_trait_requirement) requirements =
                core_list_new(th_trait_requirement);
            for (size_t i = 0; i < requirement_count; i++) {
                th_trait_requirement item;
                if (!th_trait_requirement_at(source->type, i, &item)) {
                    ok = false;
                    break;
                }
                core_list_push(requirements, item);
            }
            th_type *public_trait = th_kind_of(th_resolve(source->type))
                    == TH_TRAIT && ok
                ? th_trait(source_state->types, external.data, methods,
                           method_count, requirements.data,
                           requirements.len) : nullptr;
            core_list_free(requirements);
            ok = public_trait && th_env_register_trait_contracts(environment,
                external.data, package->name.data,
                n00bb_package_id(interface_unit).data, TH_ENV_PUBLIC,
                public_trait);
        }
        else {
            ok = th_env_register_type(environment, external.data,
                package->name.data, n00bb_package_id(interface_unit).data,
                TH_ENV_PUBLIC);
            th_env_type_shape_kind_t shape_kind = TH_ENV_TYPE_SHAPE_NONE;
            th_type *shape = nullptr;
            if (ok && !exported_type_shape(analysis->grammar, source_unit,
                                           source, &shape_kind, &shape))
                ok = false;
            bool opaque_record = source->decl_node
                && nt_is(analysis->grammar, source->decl_node, "type-decl");
            if (opaque_record) {
                shape_kind = TH_ENV_TYPE_SHAPE_NONE;
                shape = nullptr;
                include_members = false;
            }
            if (ok && shape_kind != TH_ENV_TYPE_SHAPE_NONE)
                ok = th_env_register_type_shape(environment, external.data,
                                                 shape_kind, shape);
            if (ok) {
                uint8_t exposure = opaque_record ? 0 : 1;
                ok = th_env_register_metadata(environment,
                    TH_ENV_METADATA_TYPE, external.data,
                    "n00b.type-exposure", 1, &exposure, sizeof(exposure));
            }
        }
    }
    else if (ok && source->kind == PARSE_SYM_FUNCTION) {
        th_type *contract = source->callable_contract
            ? source->callable_contract
            : th_env_signature_contract(source_state->types, source->type,
                source->error_type ? source->error_type
                                   : th_name(source_state->types, "nil"));
        ok = contract && th_env_register_function_contract(environment,
            external.data, binding->name.data, package->name.data,
            n00bb_package_id(interface_unit).data, TH_ENV_PUBLIC, contract);
    }
    else if (ok) {
        ok = th_env_register_value(environment, external.data,
            binding->name.data, package->name.data,
            n00bb_package_id(interface_unit).data, TH_ENV_PUBLIC,
            source->type);
    }
    if (ok)
        ok = append_environment_symbol_name(analysis->grammar, source_unit,
                                            source, &origin);
    if (ok)
        ok = copy_package_export_metadata(environment, environment,
            metadata_kind, origin.data, external.data, include_members,
            nullptr);
    if (ok)
        ok = append_package_export_origin(environment, metadata_kind,
            external.data, source_unit, origin.data);
    if (ok && context_symbol(analysis->grammar, source))
        ok = register_package_context_callables(analysis->grammar,
            environment, interface_unit, package, binding, source,
            source_unit);
    core_buffer_release(&origin);
    core_buffer_release(&external);
    return ok;
}

static bool
append_text_le(core_buffer_t *out, core_string_t text)
{
    if (text.u8_bytes > UINT32_MAX
        || !append_u32_le(out, (uint32_t)text.u8_bytes)) return false;
    if (text.u8_bytes) core_buffer_append(out, text.data, text.u8_bytes);
    return true;
}

bool
register_package_context_effect_metadata(n00bb_link_analysis_t *analysis)
{
    th_env *environment = analysis ? analysis->interface_environment : nullptr;
    if (!environment) return false;
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        if (state && !state->interface_module) {
            core_string_t package_id = n00bb_package_id(unit);
            core_string_t module = unit->module.identifier;
            char *module_name = core_strndup(module.data, module.u8_bytes);
            bool registered = module_name && th_env_register_metadata(
                environment, TH_ENV_METADATA_ENVIRONMENT, module_name,
                "n00b.module-interface", 1, package_id.data,
                package_id.u8_bytes + 1);
            core_buffer_t imports = {0};
            size_t import_count = core_translation_unit_import_count(unit);
            if (registered && import_count <= UINT32_MAX)
                registered = append_u32_le(&imports, (uint32_t)import_count);
            else registered = false;
            for (size_t i = 0; registered && i < import_count; i++) {
                core_translation_unit_t *imported =
                    core_translation_unit_import(unit, i);
                core_string_t name = imported->module.identifier;
                registered = name.u8_bytes <= UINT32_MAX
                    && append_u32_le(&imports, (uint32_t)name.u8_bytes);
                if (registered)
                    core_buffer_append(&imports, name.data, name.u8_bytes);
            }
            if (registered)
                registered = th_env_register_metadata(environment,
                    TH_ENV_METADATA_ENVIRONMENT, module_name,
                    "n00b.module-imports", 1, imports.data, imports.byte_len);
            core_buffer_t aliases = {0};
            if (registered && state->import_aliases.len <= UINT32_MAX)
                registered = append_u32_le(&aliases,
                    (uint32_t)state->import_aliases.len);
            for (size_t i = 0; registered && i < state->import_aliases.len;
                 i++) {
                n00bb_import_alias_t *alias = &state->import_aliases.data[i];
                registered = append_text_le(&aliases, alias->name)
                    && append_text_le(&aliases,
                                      alias->unit->module.identifier);
            }
            if (registered && state->import_aliases.len)
                registered = th_env_register_metadata(environment,
                    TH_ENV_METADATA_ENVIRONMENT, module_name,
                    "n00b.module-import-aliases", 1, aliases.data,
                    aliases.byte_len);
            core_buffer_release(&aliases);
            core_buffer_t litmods = {0};
            uint32_t litmod_count = 0;
            for (size_t i = 0; registered
                 && i < state->literal_modifiers.len; i++) {
                n00bb_user_litmod_t *registration =
                    &state->literal_modifiers.data[i];
                if (!registration->function) continue;
                core_buffer_t qualified = {0};
                registered = append_environment_symbol_name(
                        analysis->grammar, unit, registration->function,
                        &qualified)
                    && append_text_le(&litmods, registration->modifier)
                    && append_text_le(&litmods, registration->kind)
                    && append_text_le(&litmods, core_string_borrow(
                        qualified.data, qualified.byte_len));
                core_buffer_release(&qualified);
                litmod_count++;
            }
            if (registered && litmod_count) {
                core_buffer_t payload = {0};
                registered = append_u32_le(&payload, litmod_count);
                if (registered)
                    core_buffer_append(&payload, litmods.data,
                                       litmods.byte_len);
                registered = registered && th_env_register_metadata(
                    environment, TH_ENV_METADATA_ENVIRONMENT, module_name,
                    "n00b.module-litmods", 1, payload.data,
                    payload.byte_len);
                core_buffer_release(&payload);
            }
            core_buffer_release(&litmods);
            for (size_t p = 0; registered
                 && p < state->package_declarations.len; p++) {
                core_string_t package =
                    state->package_declarations.data[p].name;
                char *package_name = core_strndup(package.data,
                                                  package.u8_bytes);
                registered = package_name && th_env_register_metadata(
                    environment, TH_ENV_METADATA_ENVIRONMENT, package_name,
                    "n00b.package-imports", 1, imports.data, imports.byte_len);
                core_free(package_name);
            }
            core_buffer_release(&imports);
            core_free(module_name);
            if (!registered) return false;
        }
        for (size_t p = 0; state && !state->interface_module
             && p < state->package_declarations.len; p++) {
            n00bb_package_declaration_t *package =
                &state->package_declarations.data[p];
            parse_scope_t *scopes[] = {
                package->scope,
                parse_symtab_scope_in_namespace(state->annotations->symtab,
                    package->scope, CORE_STRING_STATIC("type")),
            };
            for (size_t s = 0; s < sizeof(scopes) / sizeof(scopes[0]); s++) {
                for (parse_sym_entry_t *binding = scopes[s]
                         ? scopes[s]->first_in_scope : nullptr;
                     binding; binding = binding->next_in_scope) {
                    parse_sym_entry_t *source = canonical_symbol(binding);
                    if (!source || !context_symbol(analysis->grammar, source))
                        continue;
                    core_translation_unit_t *source_unit = unit_for_symbol(
                        analysis->grammar, &analysis->units, source);
                    core_buffer_t origin = {0}, external = {0};
                    bool ok = source_unit
                        && append_environment_symbol_name(analysis->grammar,
                            source_unit, source, &origin);
                    if (ok)
                        core_buffer_printf(&external, "%.*s.%.*s",
                            (int)package->name.u8_bytes, package->name.data,
                            (int)binding->name.u8_bytes, binding->name.data);
                    if (ok)
                        ok = copy_package_export_metadata(environment,
                            environment, TH_ENV_METADATA_VALUE, origin.data,
                            external.data, true, "n00b.context-effects");
                    core_buffer_release(&external);
                    core_buffer_release(&origin);
                    if (!ok) return false;
                }
            }
        }
    }
    return true;
}

static bool
append_grant_string(core_buffer_t *payload, core_string_t text)
{
    if (text.u8_bytes > UINT32_MAX
        || !append_u32_le(payload, (uint32_t)text.u8_bytes)) return false;
    core_buffer_append(payload, text.data, text.u8_bytes);
    return true;
}

static bool
register_package_grants(n00bb_link_analysis_t *analysis, th_env *environment,
                        n00bb_package_declaration_t *package)
{
    uint32_t count = 0;
    core_buffer_t entries = {0};
    bool ok = true;
    for (size_t e = 0; ok && e < package->exports.len; e++) {
        n00bb_package_export_t *export = &package->exports.data[e];
        if (!export->grantee.data || !export->symbol) continue;
        parse_sym_entry_t *source = canonical_symbol(export->symbol);
        core_translation_unit_t *source_unit = source
            ? unit_for_symbol(analysis->grammar, &analysis->units, source)
            : nullptr;
        ok = source_unit
            && append_grant_string(&entries, export->export_name)
            && append_grant_string(&entries, export->grantee)
            && append_grant_string(&entries, source_unit->module.identifier)
            && append_grant_string(&entries, source->name);
        count++;
    }
    core_buffer_t payload = {0};
    if (ok && count) {
        ok = append_u32_le(&payload, count);
        if (ok) core_buffer_append(&payload, entries.data, entries.byte_len);
        if (ok) ok = th_env_register_metadata(environment,
            TH_ENV_METADATA_ENVIRONMENT, package->name.data,
            "n00b.package-grants", 1, payload.data, payload.byte_len);
    }
    core_buffer_release(&payload);
    core_buffer_release(&entries);
    return ok;
}

static bool
register_package_manifest(th_env *environment, core_translation_unit_t *unit,
                          n00bb_package_declaration_t *package)
{
    core_buffer_t payload = {0};
    bool ok = append_text_le(&payload, unit->module.identifier)
        && package->exports.len <= UINT32_MAX
        && append_u32_le(&payload, (uint32_t)package->exports.len);
    for (size_t e = 0; ok && e < package->exports.len; e++) {
        n00bb_package_export_t *export = &package->exports.data[e];
        uint8_t flags = (uint8_t)((export->renamed ? 1u : 0u)
            | (export->subpackage ? 2u : 0u)
            | (export->grantee.data ? 4u : 0u));
        ok = append_text_le(&payload, export->source_name)
            && append_text_le(&payload, export->export_name)
            && append_text_le(&payload, export->grantee);
        if (ok) core_buffer_putc(&payload, (char)flags);
    }
    ok = ok && th_env_register_metadata(environment,
        TH_ENV_METADATA_ENVIRONMENT, package->name.data,
        "n00b.package-manifest", 1, payload.data, payload.byte_len);
    core_buffer_release(&payload);
    return ok;
}

bool
register_package_interfaces(n00bb_link_analysis_t *analysis,
                            th_env *environment)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        if (state && state->interface_module) continue;
        for (size_t p = 0; state && p < state->package_declarations.len; p++) {
            n00bb_package_declaration_t *package =
                &state->package_declarations.data[p];
            core_string_t package_id = n00bb_package_id(unit);
            if (!th_env_register_metadata(environment,
                    TH_ENV_METADATA_ENVIRONMENT,
                    package->name.data, "n00b.package-interface", 1,
                    package_id.data, package_id.u8_bytes + 1)) return false;
            if (!register_package_grants(analysis, environment, package)) return false;
            if (!register_package_manifest(environment, unit, package)) return false;
            parse_scope_t *scopes[] = {
                package->scope,
                parse_symtab_scope_in_namespace(state->annotations->symtab,
                    package->scope, CORE_STRING_STATIC("type")),
            };
            for (size_t s = 0; s < sizeof(scopes) / sizeof(scopes[0]); s++)
                for (parse_sym_entry_t *binding = scopes[s]
                         ? scopes[s]->first_in_scope : nullptr;
                     binding; binding = binding->next_in_scope)
                    if (binding->kind != PARSE_SYM_MODULE
                        && !register_package_export_declaration(analysis,
                            environment, unit, package, binding)) return false;
        }
    }
    return true;
}


void
package_diagnostic(core_translation_unit_t *unit, parse_tree_t *node,
                   const char *code, const char *message)
{
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_ERROR, core_string_from_cstr(code),
        core_string_from_cstr(message), link_source_location(node));
}

static parse_scope_t *
package_import_scope(core_translation_unit_t *importer, core_string_t name,
                     core_translation_unit_t *imported)
{
    n00bb_tu_state_t *state = tu_state(importer);
    for (size_t i = 0; state && i < state->import_aliases.len; i++) {
        n00bb_import_alias_t *alias = &state->import_aliases.data[i];
        if (alias->unit != imported || !core_string_eq(alias->name, name))
            continue;
        n00bb_package_declaration_t *package =
            find_package_declaration(imported, name);
        return package ? package->scope : nullptr;
    }
    return nullptr;
}

static bool
package_name_owned_by_unit(core_string_t package,
                           core_translation_unit_t *unit)
{
    (void)package;
    return !core_string_eq(n00bb_package_id(unit),
                           CORE_STRING_STATIC("default"));
}

static bool
connect_package_subpackage(core_translation_unit_t *parent_unit,
                           n00bb_package_declaration_t *parent,
                           core_translation_unit_t *child_unit,
                           n00bb_package_declaration_t *child,
                           core_string_t member_name)
{
    n00bb_tu_state_t *parent_state = tu_state(parent_unit);
    parse_symtab_t *symtab = parent_state && parent_state->annotations
        ? parent_state->annotations->symtab : nullptr;
    if (!parent || !parent->scope || !child || !child->scope || !symtab)
        return false;
    if (!n00bb_same_package(parent_unit, child_unit)) {
        package_diagnostic(parent_unit, parent->node, "N00BB-M0010",
            "a manifest can export only subpackages from its own source package");
        return false;
    }
    parse_scope_t *types = parse_symtab_scope_in_namespace(
        symtab, parent->scope, CORE_STRING_STATIC("type"));
    parse_sym_entry_t *existing = parse_symtab_lookup_in(parent->scope,
                                                         member_name);
    if (existing && existing->kind == PARSE_SYM_MODULE
        && existing->exposed_scope == child->scope
        && parse_sym_binding(existing,
            CORE_STRING_STATIC("n00bb.package-unit")) == child_unit)
        return true;
    if (existing || parse_symtab_lookup_in(types, member_name)) {
        package_diagnostic(child_unit, child->node, "N00BB-M0006",
            "subpackage name conflicts with another package binding");
        return false;
    }
    parse_symtab_enter_scope_all(symtab, parent->scope);
    parse_sym_entry_t *binding = parse_symtab_add(symtab,
        CORE_STRING_STATIC(""), member_name, PARSE_SYM_MODULE, child->node);
    parse_symtab_leave_scope_all(symtab);
    if (!binding || !parse_sym_set_binding(binding,
            CORE_STRING_STATIC("n00bb.package-unit"), child_unit))
        return false;
    binding->exposed_scope = child->scope;
    binding->visibility = CORE_STRING_STATIC("public");
    return true;
}

bool
initialize_unit_package_interfaces(core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_symtab_t *symtab = state && state->annotations
        ? state->annotations->symtab : nullptr;
    parse_scope_t *module = module_scope(state);
    if (!symtab || !module) return false;
    for (size_t p = 0; p < state->package_declarations.len; p++) {
        n00bb_package_declaration_t *package =
            &state->package_declarations.data[p];
        if (!package_name_owned_by_unit(package->name, unit)) {
            package_diagnostic(unit, package->node, "N00BB-M0004",
                "a package manifest must be declared inside a named source package");
            return false;
        }
        if (!package->scope) {
            parse_symtab_enter_scope_all(symtab, module);
            parse_symtab_push_scope_all(symtab, package->name);
            package->scope = parse_symtab_current_scope(
                symtab, CORE_STRING_STATIC(""));
            if (!package->scope) return false;
            package->scope->scope_tag = CORE_STRING_STATIC("package-interface");
            package->scope->node = package->node;
            parse_symtab_pop_scope_all(symtab);
            parse_symtab_leave_scope_all(symtab);
        }
    }
    return true;
}

bool
validate_package_interfaces(n00bb_link_analysis_t *analysis)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!initialize_unit_package_interfaces(unit)) return false;
        for (size_t p = 0; p < state->package_declarations.len; p++) {
            n00bb_package_declaration_t *package =
                &state->package_declarations.data[p];
            for (size_t ou = 0; ou <= u; ou++) {
                n00bb_tu_state_t *other_state = tu_state(
                    analysis->units.data[ou]);
                size_t limit = ou == u ? p
                    : other_state->package_declarations.len;
                for (size_t op = 0; op < limit; op++)
                    if (core_string_eq(
                            other_state->package_declarations.data[op].name,
                            package->name)) {
                        package_diagnostic(unit, package->node,
                            "N00BB-M0001",
                            "a package interface may have only one declaration");
                        return false;
                    }
            }
        }
    }
    return true;
}

static n00bb_package_declaration_t *
find_link_package_declaration(n00bb_link_analysis_t *analysis,
                              core_string_t name,
                              core_translation_unit_t **unit_out)
{
    for (size_t u = 0; analysis && u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_package_declaration_t *package =
            find_package_declaration(unit, name);
        if (!package) continue;
        if (unit_out) *unit_out = unit;
        return package;
    }
    return nullptr;
}

bool
connect_package_subpackages(n00bb_link_analysis_t *analysis)
{
    for (size_t u = 0; analysis && u < analysis->units.len; u++) {
        core_translation_unit_t *child_unit = analysis->units.data[u];
        n00bb_tu_state_t *child_state = tu_state(child_unit);
        for (size_t p = 0; child_state
                && p < child_state->package_declarations.len; p++) {
            n00bb_package_declaration_t *child =
                &child_state->package_declarations.data[p];
            size_t split = child->name.u8_bytes;
            while (split && child->name.data[split - 1] != '.') split--;
            if (!split) continue;
            core_string_t parent_name = core_string_borrow(child->name.data,
                                                           split - 1);
            core_string_t member_name = core_string_borrow(
                child->name.data + split, child->name.u8_bytes - split);
            core_translation_unit_t *parent_unit = nullptr;
            n00bb_package_declaration_t *parent =
                find_link_package_declaration(analysis, parent_name,
                                              &parent_unit);
            if (!parent) continue;
            bool exported = false;
            for (size_t e = 0; e < parent->exports.len; e++) {
                n00bb_package_export_t *item = &parent->exports.data[e];
                if (!item->subpackage
                    || !core_string_eq(item->export_name, member_name))
                    continue;
                exported = core_string_eq(item->source_name, member_name)
                    || core_string_eq(item->source_name, child->name);
                if (exported) break;
            }
            if (!exported) continue;
            if (!connect_package_subpackage(parent_unit, parent, child_unit,
                                            child, member_name))
                return false;
        }
    }
    return true;
}

static bool
export_name_conflicts(parse_scope_t *scope, parse_scope_t *types,
                      core_string_t name, parse_sym_entry_t *source)
{
    if (parse_symtab_lookup_in(types, name)) return true;
    for (parse_sym_entry_t *entry = scope->first_in_scope; entry;
         entry = entry->next_in_scope) {
        if (!core_string_eq(entry->name, name)) continue;
        if (source->kind != PARSE_SYM_FUNCTION
            || entry->kind != PARSE_SYM_FUNCTION
            || canonical_symbol(entry) == canonical_symbol(source))
            return true;
    }
    return false;
}

static bool
add_package_export_symbol(core_translation_unit_t *interface_unit,
                          n00bb_package_declaration_t *package,
                          n00bb_package_export_t *export,
                          parse_sym_entry_t *source)
{
    n00bb_tu_state_t *state = tu_state(interface_unit);
    parse_symtab_t *symtab = state && state->annotations
        ? state->annotations->symtab : nullptr;
    if (!symtab || !package->scope || !source) return false;
    parse_scope_t *type_scope = parse_symtab_scope_in_namespace(
        symtab, package->scope, CORE_STRING_STATIC("type"));
    if (export_name_conflicts(package->scope, type_scope, export->export_name,
                              source)) {
        package_diagnostic(interface_unit, export->node, "N00BB-M0006",
                           "package export name conflicts with another binding");
        return false;
    }
    core_string_t ns = source->scope && source->scope->ns
        ? source->scope->ns->ns_name : CORE_STRING_STATIC("");
    parse_symtab_enter_scope_all(symtab, package->scope);
    parse_sym_entry_t *binding = parse_symtab_add(symtab, ns,
        export->export_name, source->kind, export->node);
    parse_symtab_leave_scope_all(symtab);
    parse_sym_entry_t *origin = canonical_symbol(source);
    if (!binding || !origin || !parse_sym_set_binding_origin(binding, origin))
        return false;
    binding->type = origin->type;
    binding->error_type = origin->error_type;
    binding->callable_contract = origin->callable_contract;
    binding->mutability = origin->mutability;
    binding->adt_kind = origin->adt_kind;
    binding->member_scope = origin->member_scope;
    binding->visibility = CORE_STRING_STATIC("public");
    binding->phases = origin->phases;
    binding->mutable_phases = origin->mutable_phases;
    binding->initializer_phases = origin->initializer_phases;
    binding->storage_duration = origin->storage_duration;
    if (!parse_sym_set_binding(origin,
            CORE_STRING_STATIC("n00bb.external-export"), binding))
        return false;
    if (origin->kind == PARSE_SYM_TYPEDEF && origin->member_scope
        && core_string_eq(origin->member_scope->scope_tag,
                          CORE_STRING_STATIC("record"))
        && !parse_sym_set_binding(origin,
            CORE_STRING_STATIC("n00bb.exposed-record"), origin))
        return false;
    export->symbol = binding;
    return true;
}

static parse_sym_entry_t *
lookup_export_symbol_in_scope(parse_symtab_t *symtab, parse_scope_t *scope,
                              core_string_t name)
{
    parse_sym_entry_t *value = parse_symtab_lookup_in(scope, name);
    while (value && value->kind == PARSE_SYM_MODULE
           && parse_sym_binding(value,
                  CORE_STRING_STATIC("n00bb.implicit-package")))
        value = value->shadowed && value->shadowed->scope == scope
            ? value->shadowed : nullptr;
    parse_scope_t *types = parse_symtab_scope_in_namespace(
        symtab, scope, CORE_STRING_STATIC("type"));
    parse_sym_entry_t *type = parse_symtab_lookup_in(types, name);
    return value ? value : type;
}

static int resolve_one_package_export(core_translation_unit_t *unit,
                                      n00bb_package_declaration_t *package,
                                      n00bb_package_export_t *export);

parse_sym_entry_t *
granted_export_symbol(core_translation_unit_t *provider, core_string_t name,
                      core_string_t grantee)
{
    n00bb_tu_state_t *state = tu_state(provider);
    for (size_t p = 0; state && p < state->package_declarations.len; p++) {
        n00bb_package_declaration_t *package =
            &state->package_declarations.data[p];
        for (size_t e = 0; e < package->exports.len; e++) {
            n00bb_package_export_t *export = &package->exports.data[e];
            if (!export->grantee.data
                || !core_string_eq(export->grantee, grantee)
                || !core_string_eq(export->export_name, name)) continue;
            if (!export->symbol && export->source_name.data)
                (void)resolve_one_package_export(provider, package, export);
            if (export->symbol) return export->symbol;
        }
    }
    return nullptr;
}

static bool
granted_to(core_translation_unit_t *provider, parse_sym_entry_t *symbol,
           core_string_t grantee)
{
    n00bb_tu_state_t *state = tu_state(provider);
    for (size_t p = 0; state && p < state->package_declarations.len; p++) {
        n00bb_package_declaration_t *package =
            &state->package_declarations.data[p];
        for (size_t e = 0; e < package->exports.len; e++) {
            n00bb_package_export_t *export = &package->exports.data[e];
            if (export->symbol && export->grantee.data
                && core_string_eq(export->grantee, grantee)
                && canonical_symbol(export->symbol) == symbol)
                return true;
        }
    }
    return false;
}

static bool
interface_module_declares(core_translation_unit_t *unit,
                          parse_sym_entry_t *symbol)
{
    n00bb_tu_state_t *state = tu_state(unit);
    n00bb_export_origin_t *origin = state && state->interface_module
        ? parse_sym_binding(symbol, CORE_STRING_STATIC("n00bb.export-origin"))
        : nullptr;
    return origin && core_string_eq(origin->module, unit->module.identifier);
}

static int
resolve_one_package_export(core_translation_unit_t *unit,
                           n00bb_package_declaration_t *package,
                           n00bb_package_export_t *export)
{
    n00bb_tu_state_t *state = tu_state(unit);
    core_string_t source = export->source_name;
    core_translation_unit_t *source_unit = nullptr;
    parse_scope_t *scope = nullptr;
    size_t prefix_end = source.u8_bytes;
    for (;;) {
        core_string_t prefix = core_string_borrow(source.data, prefix_end);
        core_translation_unit_t *candidate =
            core_string_eq(unit->module.identifier, prefix)
                ? unit : find_direct_import(unit, prefix);
        if (candidate) {
            source_unit = candidate;
            scope = package_import_scope(unit, prefix, candidate);
            if (!scope) scope = module_scope(tu_state(candidate));
            break;
        }
        if (!prefix_end) break;
        while (prefix_end && source.data[prefix_end - 1] != '.') prefix_end--;
        if (!prefix_end) break;
        prefix_end--;
    }
    if (source_unit && prefix_end == source.u8_bytes) {
        if (export->renamed) {
            package_diagnostic(unit, export->node, "N00BB-M0007",
                               "a filesystem module export cannot be renamed");
            return -1;
        }
        export->module_export = true;
        export->source_unit = source_unit;
        n00bb_tu_state_t *source_state = tu_state(source_unit);
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(source_state);
        core_list_t(parse_scope_ptr_t) type_scopes =
            module_scopes_in_namespace(source_state,
                                       CORE_STRING_STATIC("type"));
        bool ok = true;
        for (size_t s = 0; ok && s < scopes.len; s++)
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 ok && symbol; symbol = symbol->next_in_scope) {
                if ((!symbol->decl_node
                     && !interface_module_declares(source_unit, symbol))
                    || !symbol_is_public_declaration(symbol))
                    continue;
                n00bb_package_export_t item = *export;
                item.export_name = symbol->name;
                ok = add_package_export_symbol(unit, package, &item, symbol);
            }
        for (size_t s = 0; ok && s < type_scopes.len; s++)
            for (parse_sym_entry_t *symbol = type_scopes.data[s]->first_in_scope;
                 ok && symbol; symbol = symbol->next_in_scope) {
                if ((!symbol->decl_node
                     && !interface_module_declares(source_unit, symbol))
                    || !symbol_is_public_declaration(symbol))
                    continue;
                n00bb_package_export_t item = *export;
                item.export_name = symbol->name;
                ok = add_package_export_symbol(unit, package, &item, symbol);
            }
        core_list_free(scopes);
        core_list_free(type_scopes);
        return ok ? 1 : -1;
    }

    size_t component = source_unit ? prefix_end + 1 : 0;
    if (!scope) {
        source_unit = unit;
        scope = module_scope(state);
    }
    parse_sym_entry_t *symbol = nullptr;
    while (component < source.u8_bytes) {
        size_t end = component;
        while (end < source.u8_bytes && source.data[end] != '.') end++;
        core_string_t name = core_string_borrow(source.data + component,
                                                end - component);
        symbol = source_unit != unit
            ? granted_export_symbol(source_unit, name, package->name)
            : nullptr;
        if (!symbol)
            symbol = lookup_export_symbol_in_scope(
                tu_state(source_unit)->annotations->symtab, scope, name);
        if (!symbol) return 0;
        if (end < source.u8_bytes) {
            scope = symbol->member_scope;
            if (!scope) return 0;
        }
        component = end + 1;
    }
    symbol = canonical_symbol(symbol);
    if (!symbol || !symbol_is_public_declaration(symbol)) {
        package_diagnostic(unit, export->node, "N00BB-M0008",
                           "only public declarations may be exported");
        return -1;
    }
    export->source_unit = source_unit;
    if (export->grantee.data) {
        export->symbol = symbol;
        return 1;
    }
    return add_package_export_symbol(unit, package, export, symbol) ? 1 : -1;
}

bool
populate_unit_package_interfaces(core_translation_unit_t *unit)
{
    size_t unresolved = 0;
    bool progress;
    do {
        progress = false;
        unresolved = 0;
        n00bb_tu_state_t *state = tu_state(unit);
        for (size_t p = 0; p < state->package_declarations.len; p++) {
                n00bb_package_declaration_t *package =
                    &state->package_declarations.data[p];
                for (size_t e = 0; e < package->exports.len; e++) {
                    n00bb_package_export_t *export = &package->exports.data[e];
                    if (export->subpackage) continue;
                    if (export->symbol || export->module_export) continue;
                    int result = resolve_one_package_export(unit, package,
                                                            export);
                    if (result < 0) return false;
                    if (result > 0) progress = true;
                    else unresolved++;
                }
        }
    } while (progress && unresolved);
    if (!unresolved) return true;
    n00bb_tu_state_t *state = tu_state(unit);
        for (size_t p = 0; p < state->package_declarations.len; p++)
            for (size_t e = 0; e < state->package_declarations.data[p].exports.len;
                 e++) {
                n00bb_package_export_t *export =
                    &state->package_declarations.data[p].exports.data[e];
                if (!export->subpackage && !export->symbol
                    && !export->module_export)
                    package_diagnostic(unit, export->node, "N00BB-M0009",
                                       "package export source cannot be resolved");
            }
    return false;
}

bool
populate_package_interfaces(n00bb_link_analysis_t *analysis)
{
    for (size_t u = 0; u < analysis->units.len; u++)
        if (!populate_unit_package_interfaces(analysis->units.data[u]))
            return false;
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        if (state && state->interface_module) continue;
        for (size_t p = 0; state && p < state->package_declarations.len; p++) {
            n00bb_package_declaration_t *package =
                &state->package_declarations.data[p];
            for (size_t e = 0; e < package->exports.len; e++) {
                n00bb_package_export_t *export = &package->exports.data[e];
                core_translation_unit_t *owner = export->source_unit;
                if (export->symbol) {
                    parse_sym_entry_t *source =
                        canonical_symbol(export->symbol);
                    core_translation_unit_t *symbol_owner = source
                        ? unit_for_symbol(analysis->grammar,
                                          &analysis->units, source) : nullptr;
                    if (symbol_owner) owner = symbol_owner;
                }
                if (owner && !n00bb_same_package(unit, owner)
                    && !(export->symbol && export->source_unit
                         && granted_to(export->source_unit,
                                       canonical_symbol(export->symbol),
                                       package->name))) {
                    package_diagnostic(unit, export->node, "N00BB-M0010",
                        "a manifest can export only symbols from its own source package");
                    return false;
                }
            }
        }
    }
    return true;
}

static void
report_implicit_package_name(core_translation_unit_t *unit,
                             parse_sym_entry_t *existing, core_string_t name)
{
    core_buffer_t message = {0};
    core_buffer_printf(&message,
        "'%.*s' cannot be declared here because it names a package this module uses",
        (int)name.u8_bytes, name.data);
    core_diag_location_t location = existing->decl_node
        ? link_source_location(existing->decl_node)
        : (core_diag_location_t){0};
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-M0012"),
        core_string_borrow(message.data, message.byte_len), location);
    core_buffer_release(&message);
}

static bool
add_implicit_package_scope(parse_symtab_t *symtab,
                           core_translation_unit_t *unit,
                           parse_scope_t *parent, core_string_t name,
                           parse_scope_t **out)
{
    parse_sym_entry_t *existing = parse_symtab_lookup_in(parent, name);
    if (existing && existing->kind == PARSE_SYM_MODULE) {
        *out = parse_sym_binding(existing,
            CORE_STRING_STATIC("n00bb.implicit-package"));
        if (!*out) report_implicit_package_name(unit, existing, name);
        return *out != nullptr;
    }
    if (existing && !n00bb_declares_litmod(tu_state(unit), existing)) {
        report_implicit_package_name(unit, existing, name);
        *out = nullptr;
        return false;
    }
    parse_symtab_enter_scope_all(symtab, parent);
    parse_symtab_push_scope_all(symtab, name);
    parse_scope_t *scope = parse_symtab_current_scope(
        symtab, CORE_STRING_STATIC(""));
    if (scope) {
        scope->scope_tag = CORE_STRING_STATIC("implicit-package");
        parse_symtab_pop_scope_all(symtab);
    }
    parse_symtab_leave_scope_all(symtab);
    if (!scope) return false;
    parse_symtab_enter_scope_all(symtab, parent);
    parse_sym_entry_t *module = parse_symtab_add(symtab,
        CORE_STRING_STATIC(""), name, PARSE_SYM_MODULE, nullptr);
    parse_symtab_leave_scope_all(symtab);
    if (!module || !parse_sym_set_binding(module,
            CORE_STRING_STATIC("n00bb.implicit-package"), scope)
        || !parse_sym_set_binding(module,
            CORE_STRING_STATIC("n00bb.package-unit"), unit)) return false;
    module->exposed_scope = scope;
    module->visibility = CORE_STRING_STATIC("public");
    *out = scope;
    return true;
}

static bool
install_implicit_builtin_package(core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_symtab_t *symtab = state && state->annotations
        ? state->annotations->symtab : nullptr;
    parse_scope_t *parent = module_scope(state);
    parse_scope_t *builtin_scope = nullptr;
    parse_scope_t *io_scope = nullptr;
    parse_scope_t *types_scope = nullptr;
    return symtab && parent
        && add_implicit_package_scope(symtab, unit, parent,
            CORE_STRING_STATIC("builtin"), &builtin_scope)
        && add_implicit_package_scope(symtab, unit, builtin_scope,
            CORE_STRING_STATIC("io"), &io_scope)
        && add_implicit_package_scope(symtab, unit, builtin_scope,
            CORE_STRING_STATIC("types"), &types_scope);
}

bool
bind_imported_module_scopes(core_list_t(core_translation_unit_ptr_t) *units)
{
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        if (!install_implicit_builtin_package(unit)) return false;
        n00bb_tu_state_t *state = tu_state(unit);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        parse_scope_t *root = module_scope(state);
        for (size_t a = 0; state && a < state->import_aliases.len; a++) {
            core_string_t alias = state->import_aliases.data[a].name;
            const char *dot = memchr(alias.data, '.', alias.u8_bytes);
            if (!dot) continue;
            core_string_t prefix = core_string_borrow(alias.data,
                (size_t)(dot - alias.data));
            parse_sym_entry_t *existing = root
                ? parse_symtab_lookup_in(root, prefix) : nullptr;
            if (existing && existing->kind == PARSE_SYM_MODULE) continue;
            parse_scope_t *prefix_scope = nullptr;
            if (!symtab || !root || !add_implicit_package_scope(symtab,
                    unit, root, prefix, &prefix_scope)) return false;
        }
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
        for (size_t s = 0; s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_MODULE) continue;
                core_translation_unit_t *imported =
                    find_direct_import(unit, symbol->name);
                if (imported) {
                    parse_scope_t *package_scope = package_import_scope(
                        unit, symbol->name, imported);
                    symbol->exposed_scope = package_scope
                        ? package_scope : module_scope(tu_state(imported));
                }
            }
        }
        core_list_free(scopes);
    }
    return true;
}

