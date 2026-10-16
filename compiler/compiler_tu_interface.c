#include "compiler_tu_interface.h"
#include "compiler_link.h"
#include "abi_layout.h"
#include "enum_value.h"
#include "flagset_value.h"
#include "record_layout_request.h"
#include "global_metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool restore_function_semantics_data(parse_sym_entry_t *symbol,
                                            const uint8_t *data,
                                            size_t size);

bool
interface_context_hook_symbol(parse_sym_entry_t *symbol)
{
    static const char prefix[] = "__n00bb_context_";
    return symbol && symbol->name.data
        && symbol->name.u8_bytes >= sizeof(prefix) - 1
        && !memcmp(symbol->name.data, prefix, sizeof(prefix) - 1);
}


static core_string_t
n00bb_visibility_from_code(uint32_t visibility)
{
    switch (visibility) {
    case 0: return (core_string_t){0};
    case 1: return CORE_STRING_STATIC("private");
    case 2: return CORE_STRING_STATIC("public");
    }
    return (core_string_t){0};
}

static uint64_t
read_u64_le(const uint8_t *data)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; i++) value |= (uint64_t)data[i] << (i * 8);
    return value;
}

static __uint128_t
read_u128_le(const uint8_t *data)
{
    __uint128_t value = 0;
    for (size_t i = 0; i < 16; i++)
        value |= (__uint128_t)data[i] << (i * 8);
    return value;
}

const uint8_t *
interface_owned_metadata(const n00bb_interface_context_t *loader,
                         th_env_metadata_owner_t owner_kind,
                         const char *owner_name, const char *key,
                         uint32_t version, size_t *size)
{
    if (size) *size = 0;
    for (size_t e = 0; loader && e < loader->linked_interface_count; e++) {
        const th_env *environment = loader->linked_interfaces[e];
        for (size_t i = 0; i < th_env_metadata_count(environment); i++) {
            th_env_metadata_view_t metadata = {0};
            if (!th_env_metadata_at(environment, i, &metadata)
                || metadata.owner_kind != owner_kind
                || strcmp(metadata.owner_name, owner_name)
                || strcmp(metadata.key, key)
                || metadata.version != version) continue;
            if (size) *size = metadata.size;
            return metadata.data;
        }
    }
    return nullptr;
}

static bool
materialize_interface_export_origin(n00bb_interface_context_t *loader,
                                    n00bb_tu_state_t *state,
                                    parse_sym_entry_t *symbol,
                                    th_env_metadata_owner_t owner_kind,
                                    const char *external_name,
                                    const char *declaration_module,
                                    const char **failure)
{
    size_t size = 0;
    const uint8_t *data = interface_owned_metadata(loader, owner_kind,
        external_name, "n00b.export-origin", 1, &size);
    size_t declaration_module_size = declaration_module
        ? strlen(declaration_module) : 0;
    bool own_declaration = declaration_module_size
        && !strncmp(external_name, declaration_module,
                    declaration_module_size)
        && external_name[declaration_module_size] == '.';
    if (!data && !own_declaration) {
        if (failure) *failure = "interface export has no valid canonical origin";
        return false;
    }
    if (data && (size < 12 || memcmp(data, "N0EO", 4))) {
        if (failure) *failure = "interface export has no valid canonical origin";
        return false;
    }
    size_t module_size = data ? read_u32_le(data + 4)
                              : strlen(declaration_module);
    size_t declaration_size = data ? read_u32_le(data + 8)
                                   : strlen(external_name);
    if (!module_size || !declaration_size
        || (data && module_size + declaration_size != size - 12)) {
        if (failure) *failure = "interface export has malformed canonical origin metadata";
        return false;
    }
    n00bb_export_origin_t *origin = core_arena_alloc(
        &state->semantic_values, sizeof(*origin));
    char *module = core_arena_alloc(&state->semantic_values,
                                    (size_t)module_size + 1);
    char *declaration = core_arena_alloc(&state->semantic_values,
                                         (size_t)declaration_size + 1);
    if (!origin || !module || !declaration) {
        if (failure) *failure = "could not retain interface export origin";
        return false;
    }
    memcpy(module, data ? (const char *)data + 12 : declaration_module,
           module_size);
    module[module_size] = '\0';
    memcpy(declaration, data ? (const char *)data + 12 + module_size
                             : external_name, declaration_size);
    declaration[declaration_size] = '\0';
    *origin = (n00bb_export_origin_t){
        .module = core_string_borrow(module, module_size),
        .declaration = core_string_borrow(declaration, declaration_size),
    };
    if (!parse_sym_set_binding(symbol,
            CORE_STRING_STATIC("n00bb.export-origin"), origin)) {
        if (failure) *failure = "could not attach interface export origin";
        return false;
    }
    return true;
}
static const char *
interface_semantic_owner(parse_sym_entry_t *symbol, const char *fallback)
{
    n00bb_export_origin_t *origin = symbol ? parse_sym_binding(symbol,
        CORE_STRING_STATIC("n00bb.export-origin")) : nullptr;
    return origin && origin->declaration.data
        ? origin->declaration.data : fallback;
}


static th_type *
interface_callback_contract(n00bb_interface_context_t *loader,
                            const char *qualified_name)
{
    for (size_t e = 0; loader && e < loader->linked_interface_count; e++) {
        const th_env *environment = loader->linked_interfaces[e];
        for (size_t i = 0; i < th_env_declaration_count(environment); i++) {
            th_env_declaration_view_t declaration = {0};
            if (th_env_declaration_at(environment, i, &declaration)
                && declaration.kind == TH_ENV_DECLARATION_VALUE
                && declaration.qualified_name
                && !strcmp(declaration.qualified_name, qualified_name))
                return declaration.value_type;
        }
    }
    return nullptr;
}

static bool
materialize_interface_callback(n00bb_interface_context_t *loader,
                               n00bb_tu_state_t *state,
                               parse_symtab_t *symtab,
                               parse_sym_entry_t *symbol,
                               const char *qualified_name,
                               const char **failure)
{
    size_t marker_size = 0;
    const uint8_t *marker = interface_owned_metadata(loader,
        TH_ENV_METADATA_TYPE, qualified_name, "n00b.callback", 1,
        &marker_size);
    if (!marker) return true;
    if (!state || !symtab || !symbol || !marker_size
        || memchr(marker, '\0', marker_size)) {
        if (failure) *failure = "interface callback has invalid metadata";
        return false;
    }
    char *canonical_type = core_strndup((const char *)marker, marker_size);
    core_buffer_t protocol = {0};
    bool ok = canonical_type && callback_protocol_name(canonical_type,
        "__callback_contract__", &protocol);
    core_free(canonical_type);
    th_type *roles = ok
        ? interface_callback_contract(loader, protocol.data) : nullptr;
    core_buffer_release(&protocol);
    roles = roles ? th_resolve(roles) : nullptr;
    th_type *call_contract = roles && th_kind_of(roles) == TH_RECORD
        ? th_record_lookup(roles, "call") : nullptr;
    th_type *handler_contract = roles && th_kind_of(roles) == TH_RECORD
        ? th_record_lookup(roles, "handler") : nullptr;
    th_type *setup_contract = roles && th_kind_of(roles) == TH_RECORD
        ? th_record_lookup(roles, "setup") : nullptr;
    if (!call_contract || !handler_contract) {
        if (failure) *failure = "interface callback role contract is incomplete";
        return false;
    }

    parse_scope_t *parent = symbol->scope;
    if (!parent) return false;
    parse_symtab_enter_scope_all(symtab, parent);
    parse_symtab_push_scope_all(symtab, symbol->name);
    parse_scope_t *members = parse_symtab_current_scope(
        symtab, CORE_STRING_STATIC(""));
    if (!members) {
        parse_symtab_pop_scope_all(symtab);
        return false;
    }
    members->scope_tag = CORE_STRING_STATIC("callback");
    members->adt_kind = CORE_STRING_STATIC("callback");
    symbol->member_scope = members;
    symbol->adt_kind = CORE_STRING_STATIC("callback");

    const char *const role_names[] = {"call", "handler", "setup"};
    th_type *role_contracts[] = {
        call_contract, handler_contract, setup_contract,
    };
    parse_sym_entry_t *role_symbols[3] = {0};
    for (size_t i = 0; ok && i < 3; i++) {
        th_type *role_contract = role_contracts[i];
        if (!role_contract) continue;
        th_type *signature = nullptr, *error_type = nullptr;
        ok = th_env_signature_channels(state->types, role_contract,
                                        &signature, &error_type);
        parse_sym_entry_t *entry = ok ? parse_symtab_add(symtab,
            CORE_STRING_STATIC(""), core_string_from_cstr(role_names[i]),
            PARSE_SYM_FUNCTION, nullptr) : nullptr;
        ok = entry != nullptr;
        if (ok) {
            entry->type = signature;
            entry->error_type = error_type;
            entry->callable_contract = role_contract;
            entry->visibility = symbol->visibility;
            core_buffer_t key = {0};
            core_buffer_printf(&key, "n00b.callback-params.%s",
                               role_names[i]);
            size_t parameter_size = 0;
            const uint8_t *parameter_data = key.data
                ? interface_owned_metadata(loader, TH_ENV_METADATA_TYPE,
                    qualified_name, key.data, 1, &parameter_size) : nullptr;
            ok = parameter_data && restore_function_semantics_data(entry,
                parameter_data, parameter_size);
            core_buffer_release(&key);
            role_symbols[i] = entry;
        }
    }
    parse_symtab_pop_scope_all(symtab);
    if (!ok) {
        if (failure) *failure = "could not materialize interface callback roles";
        return false;
    }

    th_type *handler = th_resolve(role_symbols[1]->type);
    const th_param *state_parameter = setup_contract && handler
        && th_kind_of(handler) == TH_FUNCTION
        ? th_func_param(handler, 0) : nullptr;
    n00bb_callback_contract_t *contract = core_arena_alloc(
        &state->semantic_values, sizeof(*contract));
    if (!contract) return false;
    *contract = (n00bb_callback_contract_t){
        .call = role_symbols[0],
        .handler = role_symbols[1],
        .setup = role_symbols[2],
        .state_type = state_parameter ? state_parameter->type : nullptr,
        .stateful = setup_contract != nullptr,
    };
    if (!parse_sym_set_binding(symbol,
            CORE_STRING_STATIC("n00bb.callback-contract"), contract)) {
        if (failure) *failure = "could not retain interface callback contract";
        return false;
    }
    return true;
}



static bool
decode_interface_enum_value(const uint8_t *data, size_t size,
                            const char *member_name, n00bb_enum_value_t *value)
{
    if (!data || size < 8 || memcmp(data, "N0EV", 4)) return false;
    size_t offset = 4;
    uint32_t count = read_u32_le(data + offset);
    offset += 4;
    bool found = false;
    for (uint32_t i = 0; i < count; i++) {
        if (offset > size || size - offset < 4) return false;
        uint32_t name_size = read_u32_le(data + offset);
        offset += 4;
        if (name_size > size - offset) return false;
        const uint8_t *name = data + offset;
        offset += name_size;
        if (offset > size || size - offset < 17) return false;
        ir_uint_t decoded = read_u64_le(data + offset);
        offset += 8;
        decoded |= (ir_uint_t)read_u64_le(data + offset) << 64;
        offset += 8;
        uint8_t negative = data[offset++];
        if (negative > 1 || (negative && !decoded)) return false;
        if (strlen(member_name) == name_size &&
            !memcmp(name, member_name, name_size)) {
            if (found) return false;
            found = true;
            if (value) *value = (n00bb_enum_value_t){decoded, negative != 0};
        }
    }
    return offset == size && found;
}

static bool
decode_interface_flagset_value(const uint8_t *data, size_t size,
                               const char *member_name,
                               n00bb_flagset_value_t *value,
                               n00bb_flagset_info_t *info)
{
    if (!data || size < 28 || memcmp(data, "N0FV", 4)) return false;
    size_t offset = 4;
    n00bb_flagset_info_t decoded_info = {
        .all_mask = read_u128_le(data + offset),
    };
    offset += 16;
    decoded_info.atomic_count = read_u32_le(data + offset);
    offset += 4;
    uint32_t count = read_u32_le(data + offset);
    offset += 4;
    if (decoded_info.atomic_count > 128) return false;
    bool found = false;
    for (uint32_t i = 0; i < count; i++) {
        if (offset > size || size - offset < 4) return false;
        uint32_t name_size = read_u32_le(data + offset);
        offset += 4;
        if (name_size > size - offset) return false;
        const uint8_t *name = data + offset;
        offset += name_size;
        if (offset > size || size - offset < 21) return false;
        n00bb_flagset_value_t decoded = {
            .mask = read_u128_le(data + offset),
        };
        offset += 16;
        decoded.declaration_order = read_u32_le(data + offset);
        offset += 4;
        decoded.atomic = data[offset++] != 0;
        if (strlen(member_name) == name_size
            && !memcmp(name, member_name, name_size)) {
            if (found) return false;
            found = true;
            if (value) *value = decoded;
        }
    }
    if (info) *info = decoded_info;
    return offset == size && found;
}

static bool
decode_interface_member_metadata(const uint8_t *data, size_t size,
                                 const char *member_name,
                                 uint32_t *visibility, uint32_t *flags,
                                 uint64_t *bit_offset, uint64_t *bit_width)
{
    if (!data || size < 8) return false;
    size_t offset = 0;
    uint32_t type_visibility = read_u32_le(data);
    offset += 4;
    uint32_t count = read_u32_le(data + offset);
    offset += 4;
    if (type_visibility > 2) return false;
    for (uint32_t i = 0; i < count; i++) {
        if (offset > size || size - offset < 4) return false;
        uint32_t name_size = read_u32_le(data + offset);
        offset += 4;
        if (name_size > size - offset) return false;
        const uint8_t *name = data + offset;
        offset += name_size;
        if (offset > size || size - offset < 24) return false;
        uint32_t member_visibility = read_u32_le(data + offset);
        offset += 4;
        uint32_t member_flags = read_u32_le(data + offset);
        offset += 4;
        uint64_t member_offset = read_u64_le(data + offset);
        offset += 8;
        uint64_t member_width = read_u64_le(data + offset);
        offset += 8;
        if (member_visibility > 2 || member_flags > 3) return false;
        if (strlen(member_name) == name_size &&
            !memcmp(name, member_name, name_size)) {
            if (visibility) *visibility = member_visibility;
            if (flags) *flags = member_flags;
            if (bit_offset) *bit_offset = member_offset;
            if (bit_width) *bit_width = member_width;
        }
    }
    return offset == size;
}




static const uint8_t *
interface_type_member_array_shape_metadata(n00bb_interface_context_t *loader,
    const char *qualified_name, const char *member_name, size_t *size)
{
    core_buffer_t key = {0};
    core_buffer_printf(&key, "n00b.member-array-shape.%s", member_name);
    if (size) *size = 0;
    const uint8_t *result = key.data
        ? interface_owned_metadata(loader, TH_ENV_METADATA_TYPE,
            qualified_name, key.data, 1, size) : nullptr;
    core_buffer_release(&key);
    return result;
}

static th_type *
interface_array_shape_type(n00bb_interface_context_t *loader, th_type *type)
{
    core_list_t(th_type_ptr_t) seen = core_list_new(th_type_ptr_t);
    while (type) {
        type = th_resolve(type);
        if (!type) break;
        if (th_kind_of(type) != TH_NAMED || th_named_nargs(type)) break;
        bool cycle = false;
        for (size_t i = 0; i < seen.len; i++)
            if (seen.data[i] == type) { cycle = true; break; }
        if (cycle) { type = nullptr; break; }
        core_list_push(seen, type);
        const char *name = nullptr;
        size_t name_size = 0;
        if (!th_named_info(type, &name, &name_size, nullptr, nullptr)) break;
        th_type *next = nullptr;
        for (size_t e = 0;
             loader && e < loader->linked_interface_count && !next; e++) {
            const th_env *environment = loader->linked_interfaces[e];
            for (size_t i = 0; i < th_env_declaration_count(environment); i++) {
                th_env_declaration_view_t declaration = {0};
                if (!th_env_declaration_at(environment, i, &declaration)
                    || declaration.kind != TH_ENV_DECLARATION_TYPE
                    || declaration.type_shape_kind != TH_ENV_TYPE_SHAPE_ALIAS
                    || strlen(declaration.qualified_name) != name_size
                    || memcmp(declaration.qualified_name, name, name_size))
                    continue;
                next = declaration.type_shape;
                break;
            }
        }
        if (!next) break;
        type = next;
    }
    core_list_free(seen);
    return type;
}

static bool
decode_interface_array_shape(n00bb_interface_context_t *loader,
                             n00bb_tu_state_t *state, th_type *type,
                             const uint8_t *data, size_t size,
                             ir_shape_t **shape_out)
{
    if (shape_out) *shape_out = nullptr;
    if (!data) return true;
    type = interface_array_shape_type(loader, type);
    if (!loader->ir_context || !state || !type || !shape_out || size < 20
        || memcmp(data, "N0AS", 4) != 0) return false;
    size_t offset = 4;
    uint32_t rank = read_u32_le(data + offset);
    offset += 4;
    uint32_t flat_prefix = read_u32_le(data + offset);
    offset += 4;
    uint32_t classified = read_u32_le(data + offset);
    offset += 4;
    if (!rank || flat_prefix > rank || classified != 1) return false;
    th_type *element = th_resolve(type);
    for (uint32_t i = 0; i < rank; i++) {
        if (!element || th_kind_of(element) != TH_SEQUENCE) return false;
        element = th_resolve(th_seq_elem(element));
    }
    uint32_t archived_rank = read_u32_le(data + 4);
    th_type *sequence = th_resolve(type);
    uint32_t type_rank = 0;
    while (sequence && th_kind_of(sequence) == TH_SEQUENCE) {
        type_rank++;
        sequence = th_resolve(th_seq_elem(sequence));
    }
    if (type_rank != archived_rank) return false;
    rank = archived_rank;
    ir_shape_t *shape = ir_shape_new(loader->ir_context, element, rank);
    if (!shape || !ir_shape_set_storage(shape, flat_prefix, true))
        return false;
    uint32_t known_prefix = 0;
    bool saw_unresolved = false;
    for (uint32_t i = 0; i < rank; i++) {
        if (size - offset < 16) return false;
        ir_shape_extent_kind_t kind =
            (ir_shape_extent_kind_t)read_u32_le(data + offset);
        offset += 4;
        uint64_t static_value = read_u64_le(data + offset);
        offset += 8;
        uint32_t name_size = read_u32_le(data + offset);
        offset += 4;
        if (name_size > size - offset || kind > IR_SHAPE_EXTENT_RUNTIME)
            return false;
        const char *producer = nullptr;
        if (name_size) {
            char *copy = core_arena_alloc(&state->semantic_values,
                                           (size_t)name_size + 1);
            if (!copy) return false;
            memcpy(copy, data + offset, name_size);
            copy[name_size] = 0;
            producer = copy;
        }
        offset += name_size;
        ir_shape_extent_t extent = {.kind = kind};
        if (kind == IR_SHAPE_EXTENT_STATIC) {
            if (!static_value || name_size) return false;
            extent.static_value = static_value;
        }
        else if (kind == IR_SHAPE_EXTENT_COMPTIME) {
            if (static_value || !producer) return false;
            extent.producer_identifier = producer;
        }
        else if (kind == IR_SHAPE_EXTENT_UNRESOLVED) {
            if (static_value || name_size) return false;
            saw_unresolved = true;
        }
        else if (static_value || name_size)
            return false;
        if (kind != IR_SHAPE_EXTENT_UNRESOLVED) {
            if (saw_unresolved) return false;
            known_prefix++;
        }
        if (!ir_shape_set_extent(shape, i, extent)) return false;
    }
    if (offset != size || known_prefix != flat_prefix) return false;
    *shape_out = shape;
    return true;
}

static bool
restore_interface_array_shape_data(n00bb_interface_context_t *loader,
                                   n00bb_tu_state_t *state,
                                   parse_sym_entry_t *symbol,
                                   const uint8_t *data, size_t size)
{
    if (!data) return true;
    ir_shape_t *shape = nullptr;
    return symbol && decode_interface_array_shape(loader, state, symbol->type,
                data, size, &shape)
        && shape && parse_sym_set_binding(symbol,
            CORE_STRING_STATIC("ir.array-shape"), shape);
}

static bool
interface_comptime_producer_available(n00bb_interface_context_t *loader,
                                      const char *owner_name,
                                      const char *producer)
{
    if (!loader || !owner_name || !producer || !*producer) return false;
    core_buffer_t key = {0};
    core_buffer_printf(&key, "n00b.comptime-producer.%s", producer);
    bool found = false;
    for (size_t e = 0; !found && e < loader->linked_interface_count; e++) {
        const th_env *environment = loader->linked_interfaces[e];
        for (size_t i = 0; i < th_env_metadata_count(environment); i++) {
            th_env_metadata_view_t metadata = {0};
            if (th_env_metadata_at(environment, i, &metadata)
                && !strcmp(metadata.owner_name, owner_name)
                && !strcmp(metadata.key, key.data)
                && metadata.version == 1
                && metadata.size == strlen(producer)
                && !memcmp(metadata.data, producer, metadata.size)) {
                found = true;
                break;
            }
        }
    }
    core_buffer_release(&key);
    return found;
}

static bool
interface_comptime_dependency_available(n00bb_interface_context_t *loader,
                                        const char *module_name,
                                        const char *function_name)
{
    for (size_t e = 0; loader && e < loader->linked_interface_count; e++) {
        const th_env *environment = loader->linked_interfaces[e];
        for (size_t i = 0; i < th_env_declaration_count(environment); i++) {
            th_env_declaration_view_t declaration = {0};
            if (th_env_declaration_at(environment, i, &declaration)
                && declaration.kind == TH_ENV_DECLARATION_FUNCTION
                && declaration.module && declaration.root_name
                && !strcmp(declaration.module, module_name)
                && !strcmp(declaration.root_name, function_name))
                return true;
        }
    }
    return false;
}

static bool
interface_comptime_dependencies_available(n00bb_interface_context_t *loader,
                                          const char *owner_name,
                                          const char *producer)
{
    if (!loader || !owner_name || !producer) return false;
    core_buffer_t key = {0};
    core_buffer_printf(&key, "n00b.comptime-dependencies.%s", producer);
    const uint8_t *data = nullptr;
    size_t size = 0;
    for (size_t e = 0; !data && e < loader->linked_interface_count; e++) {
        const th_env *environment = loader->linked_interfaces[e];
        for (size_t i = 0; i < th_env_metadata_count(environment); i++) {
            th_env_metadata_view_t metadata = {0};
            if (th_env_metadata_at(environment, i, &metadata)
                && !strcmp(metadata.owner_name, owner_name)
                && !strcmp(metadata.key, key.data)
                && metadata.version == 1) {
                data = metadata.data;
                size = metadata.size;
                break;
            }
        }
    }
    core_buffer_release(&key);
    if (!data || size < 8 || memcmp(data, "N0CD", 4) != 0) return false;
    uint32_t count = read_u32_le(data + 4);
    size_t offset = 8;
    if (count > (size - offset) / 12) return false;
    for (uint32_t i = 0; i < count; i++) {
        if (size - offset < 12) return false;
        uint32_t module_size = read_u32_le(data + offset);
        uint32_t declaration_size = read_u32_le(data + offset + 4);
        uint32_t name_size = read_u32_le(data + offset + 8);
        offset += 12;
        if (!module_size || !declaration_size || !name_size
            || module_size > size - offset) return false;
        const char *module = (const char *)data + offset;
        offset += module_size;
        if (declaration_size > size - offset) return false;
        offset += declaration_size;
        if (name_size > size - offset) return false;
        char *module_copy = core_strndup(module, module_size);
        char *function_name = core_strndup(
            (const char *)data + offset, name_size);
        if (!function_name || !module_copy) {
            core_free(function_name);
            core_free(module_copy);
            return false;
        }
        offset += name_size;
        bool available = interface_comptime_dependency_available(
            loader, module_copy, function_name);
        core_free(function_name);
        core_free(module_copy);
        if (!available)
            return false;
    }
    return offset == size;
}

static bool
interface_shape_producers_available(n00bb_interface_context_t *loader,
                                    const char *owner_name,
                                    ir_shape_t *shape)
{
    for (uint32_t i = 0; shape && i < ir_shape_rank(shape); i++) {
        ir_shape_extent_t extent = {0};
        if (!ir_shape_extent(shape, i, &extent)) return false;
        if (extent.kind != IR_SHAPE_EXTENT_COMPTIME) continue;
        const char *producer = extent.producer
            ? extent.producer->name : extent.producer_identifier;
        if (!interface_comptime_producer_available(loader, owner_name,
                                                       producer)
            || !interface_comptime_dependencies_available(loader, owner_name,
                                                            producer))
            return false;
    }
    return true;
}

static bool
restore_interface_array_shape(n00bb_interface_context_t *loader,
                              n00bb_tu_state_t *state,
                              parse_sym_entry_t *symbol,
                              const char *qualified_name)
{
    size_t size = 0;
    const uint8_t *data = interface_owned_metadata(loader,
        TH_ENV_METADATA_VALUE, qualified_name, "n00b.array-shape", 1, &size);
    if (!restore_interface_array_shape_data(loader, state, symbol,
                                             data, size)) return false;
    ir_shape_t *shape = parse_sym_binding(symbol,
        CORE_STRING_STATIC("ir.array-shape"));
    return interface_shape_producers_available(loader,
        interface_semantic_owner(symbol, qualified_name), shape);
}



static bool
restore_interface_function_shapes(n00bb_interface_context_t *loader,
                                  n00bb_tu_state_t *state,
                                  parse_sym_entry_t *symbol,
                                  const char *qualified_name)
{
    size_t size = 0;
    const uint8_t *data = interface_owned_metadata(loader,
        TH_ENV_METADATA_FUNCTION, qualified_name,
        "n00b.function-array-shapes", 1, &size);
    if (!data) return true;
    if (!state || !symbol || size < 12 || memcmp(data, "N0FS", 4))
        return false;
    th_type *signature = symbol->type ? th_resolve(symbol->type) : nullptr;
    uint32_t count = read_u32_le(data + 4);
    if (!signature || th_kind_of(signature) != TH_FUNCTION
        || count != th_func_nparams(signature)) return false;
    n00bb_callable_shapes_t *shapes = core_arena_alloc(
        &state->semantic_values, sizeof(*shapes));
    ir_shape_t **parameters = count ? core_arena_alloc(
        &state->semantic_values, count * sizeof(*parameters)) : nullptr;
    if (!shapes || (count && !parameters)) return false;
    memset(shapes, 0, sizeof(*shapes));
    if (count) memset(parameters, 0, count * sizeof(*parameters));
    size_t offset = 8;
    for (uint32_t i = 0; i < count; i++) {
        if (size - offset < 4) return false;
        uint32_t item_size = read_u32_le(data + offset);
        offset += 4;
        if (item_size > size - offset) return false;
        const th_param *parameter = th_func_param(signature, i);
        if (item_size && (!parameter
            || !decode_interface_array_shape(loader, state,
                parameter->type, data + offset, item_size,
                &parameters[i]))) return false;
        offset += item_size;
    }
    if (size - offset < 4) return false;
    uint32_t result_size = read_u32_le(data + offset);
    offset += 4;
    if (result_size > size - offset || offset + result_size != size)
        return false;
    if (result_size) {
        ir_shape_t *result = nullptr;
        if (!decode_interface_array_shape(loader, state,
                th_func_ret(signature), data + offset, result_size, &result)
            || !result || !parse_sym_set_binding(symbol,
                CORE_STRING_STATIC("ir.return-shape"), result)) return false;
    }
    shapes->parameter_count = count;
    shapes->parameters = parameters;
    const char *semantic_owner = interface_semantic_owner(symbol,
                                                           qualified_name);
    for (uint32_t i = 0; i < count; i++)
        if (!interface_shape_producers_available(loader, semantic_owner,
                                                  parameters[i])) return false;
    if (!interface_shape_producers_available(loader, semantic_owner,
            parse_sym_binding(symbol, CORE_STRING_STATIC("ir.return-shape"))))
        return false;
    return parse_sym_set_binding(symbol,
        CORE_STRING_STATIC("ir.parameter-shapes"), shapes);
}

ir_shape_t *
function_parameter_shape(parse_sym_entry_t *symbol, size_t index)
{
    n00bb_callable_shapes_t *shapes = parse_sym_binding(symbol,
        CORE_STRING_STATIC("ir.parameter-shapes"));
    return shapes && index < shapes->parameter_count
        ? shapes->parameters[index] : nullptr;
}


static bool
restore_function_semantics_data(parse_sym_entry_t *symbol,
                                const uint8_t *data, size_t size)
{
    if (!data || size < 9 || memcmp(data, "N0FP", 4) || data[4] != 1)
        return false;
    uint32_t count = read_u32_le(data + 5);
    th_type *signature = th_resolve(symbol ? symbol->type : nullptr);
    size_t expected = signature && th_kind_of(signature) == TH_FUNCTION
        ? th_func_nparams(signature) + (th_func_rest(signature) != nullptr)
        : 0;
    if (!signature || th_kind_of(signature) != TH_FUNCTION
        || count != expected || count > (size - 9) / 4) return false;
    size_t offset = 9;
    for (uint32_t i = 0; i < count; i++) {
        if (size - offset < 4) return false;
        uint16_t name_size = (uint16_t)data[offset]
            | (uint16_t)data[offset + 1] << 8;
        uint8_t flags = data[offset + 2];
        uint8_t mode = data[offset + 3];
        offset += 4;
        if (!name_size || (flags & ~3u)
            || !th_mode_is_valid((th_mode)mode)
            || name_size > size - offset) return false;
        bool is_rest = (flags & 2u) != 0;
        if (is_rest != (i + 1 == count
                && th_func_rest(signature) != nullptr)
            || ((flags & 1u)
                && (is_rest || !th_mode_is_input_only((th_mode)mode))))
            return false;
        size_t previous_offset = 9;
        for (uint32_t previous = 0; previous < i; previous++) {
            uint16_t previous_size = (uint16_t)data[previous_offset]
                | (uint16_t)data[previous_offset + 1] << 8;
            previous_offset += 4;
            if (previous_size == name_size
                && !memcmp(data + previous_offset, data + offset, name_size))
                return false;
            previous_offset += previous_size;
        }
        offset += name_size;
    }
    if (offset != size) return false;
    symbol->user_data = (void *)data;
    return true;
}

static bool
restore_interface_function_semantics(n00bb_interface_context_t *loader,
                                     parse_sym_entry_t *symbol,
                                     const char *qualified_name)
{
    size_t size = 0;
    const uint8_t *data = interface_owned_metadata(loader,
        TH_ENV_METADATA_FUNCTION, qualified_name, "n00b.function-params", 1,
        &size);
    return restore_function_semantics_data(symbol, data, size);
}

static th_type *
interface_static_record_shape(n00bb_interface_context_t *loader,
                              n00bb_tu_state_t *state, th_type *type,
                              const char *preferred_module)
{
    th_type *shape = static_record_shape(state, type);
    if (shape || !loader || !state || !state->types) return shape;
    type = type ? th_resolve(type) : nullptr;
    const char *name = nullptr;
    size_t name_size = 0;
    if (!type || !th_named_info(type, &name, &name_size, nullptr, nullptr))
        return nullptr;
    const char *short_name = name;
    size_t short_name_size = name_size;
    for (size_t i = 0; i < name_size; i++)
        if (name[i] == '.') {
            short_name = name + i + 1;
            short_name_size = name_size - i - 1;
        }
    th_type *preferred = nullptr, *fallback = nullptr;
    bool ambiguous_fallback = false;
    for (size_t e = 0; e < loader->linked_interface_count; e++) {
        const th_env *environment = loader->linked_interfaces[e];
        for (size_t i = 0; i < th_env_declaration_count(environment); i++) {
            th_env_declaration_view_t declaration = {0};
            if (!th_env_declaration_at(environment, i, &declaration)
                || declaration.kind != TH_ENV_DECLARATION_TYPE
                || declaration.type_shape_kind != TH_ENV_TYPE_SHAPE_RECORD
                || !declaration.type_shape || !declaration.qualified_name)
                continue;
            size_t qualified_size = strlen(declaration.qualified_name);
            if (qualified_size == name_size
                && !memcmp(declaration.qualified_name, name, name_size)) {
                preferred = declaration.type_shape;
                break;
            }
            const char *root = strrchr(declaration.qualified_name, '.');
            root = root ? root + 1 : declaration.qualified_name;
            if (strlen(root) != short_name_size
                || memcmp(root, short_name, short_name_size)) continue;
            if (preferred_module && declaration.module
                && !strcmp(declaration.module, preferred_module)) {
                preferred = declaration.type_shape;
                break;
            }
            if (!fallback) fallback = declaration.type_shape;
            else if (th_hash(state->types, fallback)
                     != th_hash(state->types, declaration.type_shape))
                ambiguous_fallback = true;
        }
        if (preferred) break;
    }
    shape = preferred ? preferred : ambiguous_fallback ? nullptr : fallback;
    return shape && th_type_define(state->types, type, shape) ? shape : nullptr;
}

static bool
decode_static_value(const uint8_t *data, size_t size, size_t *offset,
                    th_type *type, n00bb_tu_state_t *state,
                    n00bb_interface_context_t *loader,
                    const char *preferred_module,
                    n00bb_static_value_t *out, size_t depth)
{
    if (!data || !offset || !type || !state || !out
        || *offset >= size) return false;
    type = th_resolve(type);
    n00bb_static_value_kind_t kind =
        (n00bb_static_value_kind_t)data[(*offset)++];
    *out = (n00bb_static_value_t){.kind = kind};
    switch (kind) {
    case N00BB_STATIC_VALUE_BOOL:
        if (*offset >= size || data[*offset] > 1) return false;
        out->as.boolean = data[(*offset)++] != 0;
        return true;
    case N00BB_STATIC_VALUE_INT:
        if (size - *offset < 16) return false;
        out->as.integer = (__int128)read_u128_le(data + *offset);
        *offset += 16;
        return true;
    case N00BB_STATIC_VALUE_FLOAT: {
        if (size - *offset < 8) return false;
        uint64_t bits = read_u64_le(data + *offset);
        memcpy(&out->as.real, &bits, sizeof(bits));
        *offset += 8;
        return true;
    }
    case N00BB_STATIC_VALUE_STRING: {
        if (size - *offset < 4) return false;
        uint32_t length = read_u32_le(data + *offset);
        *offset += 4;
        if (length > size - *offset) return false;
        char *copy = length
            ? core_arena_alloc(&state->semantic_values, length) : nullptr;
        if (length && !copy) return false;
        if (length) memcpy(copy, data + *offset, length);
        out->as.string = core_string_borrow(copy, length);
        *offset += length;
        return true;
    }
    case N00BB_STATIC_VALUE_NIL:
    {
        th_type *element = nullptr;
        return th_is_nil(type)
            || n00bb_option_type_element(type, &element);
    }
    case N00BB_STATIC_VALUE_VARIANT: {
        bool variant = false;
        th_type *shape = interface_static_record_shape(loader, state, type,
                                                       preferred_module);
        if (!th_named_info(type, nullptr, nullptr, &variant, nullptr)
            || !variant || !shape || th_kind_of(shape) != TH_RECORD
            || size - *offset < 4) return false;
        uint32_t length = read_u32_le(data + *offset);
        *offset += 4;
        if (!length || length > size - *offset) return false;
        const th_member *arm = nullptr;
        for (size_t i = 0; i < th_record_len(shape); i++) {
            const th_member *candidate = th_record_member(shape, i);
            if (candidate && strlen(candidate->name) == length
                && !memcmp(candidate->name, data + *offset, length)) {
                arm = candidate;
                break;
            }
        }
        if (!arm) return false;
        *offset += length;
        n00bb_static_value_t *payload = core_arena_alloc(
            &state->semantic_values, sizeof(*payload));
        if (!payload || !decode_static_value(data, size, offset, arm->type,
                state, loader, preferred_module, payload, depth + 1))
            return false;
        out->as.variant.arm = core_string_from_cstr(arm->name);
        out->as.variant.payload = payload;
        return true;
    }
    case N00BB_STATIC_VALUE_SEQUENCE:
    case N00BB_STATIC_VALUE_TUPLE:
    case N00BB_STATIC_VALUE_RECORD: {
        if (size - *offset < 4) return false;
        uint32_t count = read_u32_le(data + *offset);
        *offset += 4;
        if (count > (size - *offset)) return false;
        th_type *shape = nullptr;
        if (kind == N00BB_STATIC_VALUE_SEQUENCE) {
            uint64_t extent = 0;
            if (th_kind_of(type) == TH_SEQUENCE
                && th_seq_extent(type, &extent) && extent != count)
                return false;
        }
        else if (kind == N00BB_STATIC_VALUE_TUPLE) {
            if (!th_builtin_applied(type, TH_BUILTIN_TUPLE, count)) return false;
        }
        else if (kind == N00BB_STATIC_VALUE_RECORD) {
            shape = interface_static_record_shape(loader, state, type,
                                                  preferred_module);
            if (!shape || th_kind_of(shape) != TH_RECORD
                || th_record_len(shape) != count) return false;
        }
        n00bb_static_value_t *items = count
            ? core_arena_alloc(&state->semantic_values,
                               count * sizeof(*items)) : nullptr;
        if (count && !items) return false;
        for (uint32_t i = 0; i < count; i++) {
            th_type *child_type = nullptr;
            if (kind == N00BB_STATIC_VALUE_SEQUENCE)
                child_type = static_sequence_element_type(type);
            else if (kind == N00BB_STATIC_VALUE_TUPLE)
                child_type = static_tuple_element_type(type, i, nullptr);
            else {
                const th_member *member = th_record_member(shape, i);
                child_type = member ? member->type : nullptr;
            }
            if (!child_type || !decode_static_value(data, size, offset,
                    child_type, state, loader, preferred_module,
                    &items[i], depth + 1)) return false;
        }
        out->as.aggregate.items = items;
        out->as.aggregate.count = count;
        return true;
    }
    }
    return false;
}

static bool
restore_interface_value_semantics(n00bb_interface_context_t *loader,
                                  core_translation_unit_t *unit,
                                  parse_sym_entry_t *symbol,
                                  const char *qualified_name)
{
    size_t size = 0;
    const uint8_t *data = interface_owned_metadata(loader,
        TH_ENV_METADATA_VALUE, qualified_name, "n00b.global", 1, &size);
    core_string_t requested = {0};
    bool storage_required = false;
    size_t offset = 0;
    if (!n00bb_global_metadata_read_storage(data, size, &requested,
            &storage_required, &offset)) return false;
    symbol->mutability = (parse_sym_mutability_t)data[0];
    if (symbol->mutability > PARSE_SYM_GLOBAL || data[1] > PARSE_SYM_PHASE_BOTH
        || data[2] > PARSE_SYM_PHASE_BOTH
        || data[3] > PARSE_SYM_PHASE_BOTH
        || data[4] > PARSE_SYM_STORAGE_DYNAMIC || data[5] > 1 || data[6] > 1)
        return false;
    parse_sym_set_phases(symbol, (parse_sym_phase_t)data[1],
        (parse_sym_phase_t)data[2], (parse_sym_phase_t)data[3]);
    parse_sym_set_storage_duration(symbol,
        (parse_sym_storage_duration_t)data[4]);
    symbol->is_thread_local = data[5] != 0;
    parse_sym_set_storage_class(symbol, requested, storage_required,
                                (core_string_t){0});
    if (data[7] && !parse_sym_set_binding(symbol,
            CORE_STRING_STATIC("n00bb.global-explicit-initializer"),
            symbol)) return false;
    if (data[10] > 1 || data[11] > N00BB_STATIC_VALUE_VARIANT)
        return false;
    bool has_value = data[10] != 0;
    if (!has_value && (data[11] != 0 || offset != size)) return false;
    if (has_value) {
        n00bb_tu_state_t *state = tu_state(unit);
        n00bb_export_origin_t *origin = parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.export-origin"));
        if (!state) return false;
        n00bb_static_value_t *value = core_arena_alloc(
            &state->semantic_values, sizeof(*value));
        if (!value) return false;
        size_t value_offset = offset;
        if (!decode_static_value(data, size, &value_offset,
                symbol->type, state, loader,
                origin ? origin->module.data : nullptr, value, 0)
            || value->kind != (n00bb_static_value_kind_t)data[11]
            || value_offset != size
            || !static_value_matches_type(value, symbol->type, state, 0))
            return false;
        symbol->comptime_value = value;
        symbol->has_comptime_value = true;
    }
    return true;
}

static bool
restore_interface_context_semantics(n00bb_interface_context_t *loader,
                                    parse_sym_entry_t *symbol,
                                    const char *qualified_name)
{
    size_t size = 0;
    const uint8_t *data = interface_owned_metadata(loader,
        TH_ENV_METADATA_VALUE, qualified_name, "n00b.context", 1, &size);
    if (!data || size < 5 || (data[0] & 0x70u)) return false;
    bool extended = (data[0] & 0x80u) != 0;
    uint32_t parameter_count = read_u32_le(data + 1);
    if (parameter_count > (size - 5) / 5) return false;
    size_t offset = 5;
    for (uint32_t i = 0; i < parameter_count; i++) {
        if (size - offset < 4) return false;
        uint16_t name_size = (uint16_t)data[offset]
            | (uint16_t)data[offset + 1] << 8;
        uint8_t flags = data[offset + 2];
        uint8_t mode = data[offset + 3];
        offset += 4;
        if (!name_size || (flags & ~(extended ? 7u : 3u))
            || ((flags & 4u) && (!(flags & 1u) || !extended))
            || !th_mode_is_valid((th_mode)mode)
            || name_size > size - offset) return false;
        offset += name_size;
        if (extended && (flags & 4u)) {
            if (size - offset < 2) return false;
            uint16_t dependency_count = (uint16_t)data[offset]
                | (uint16_t)data[offset + 1] << 8;
            offset += 2;
            if (dependency_count > (size - offset) / 2) return false;
            bool seen[UINT16_MAX + 1] = {0};
            for (uint16_t d = 0; d < dependency_count; d++) {
                uint16_t dependency = (uint16_t)data[offset]
                    | (uint16_t)data[offset + 1] << 8;
                offset += 2;
                if (dependency >= parameter_count || seen[dependency])
                    return false;
                seen[dependency] = true;
            }
        }
    }
    if (offset != size) return false;
    symbol->adt_kind = CORE_STRING_STATIC("context");
    symbol->user_data = (void *)data;
    size_t effects_size = 0;
    const uint8_t *effects = interface_owned_metadata(loader,
        TH_ENV_METADATA_VALUE, qualified_name, "n00b.context-effects", 1,
        &effects_size);
    if (!effects || effects_size < 8 || memcmp(effects, "N0CE", 4))
        return false;
    uint32_t effect_count = read_u32_le(effects + 4);
    size_t effects_offset = 8;
    for (uint32_t i = 0; i < effect_count; i++) {
        if (effects_size - effects_offset < 9) return false;
        uint32_t kind = effects[effects_offset++];
        uint32_t module_size = read_u32_le(effects + effects_offset);
        effects_offset += 4;
        uint32_t key_size = read_u32_le(effects + effects_offset);
        effects_offset += 4;
        if (((kind < N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_SUMMARY
                || kind > N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_ALIAS_SUMMARY)
            && kind != N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_UNKNOWN)
            || module_size > effects_size - effects_offset
            || key_size > effects_size - effects_offset - module_size)
            return false;
        if ((kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_UNKNOWN)
            != (module_size == 0 && key_size == 0)) return false;
        effects_offset += module_size + key_size;
    }
    if (effects_offset != effects_size
        || !parse_sym_set_binding(symbol,
            CORE_STRING_STATIC("n00bb.context-effects"), (void *)effects))
        return false;
    th_type *signature = th_resolve(symbol->type);
    size_t expected = signature && th_kind_of(signature) == TH_FUNCTION
        ? th_func_nparams(signature) + (th_func_rest(signature) != nullptr)
        : 0;
    if (!signature || th_kind_of(signature) != TH_FUNCTION
        || expected != parameter_count) return false;
    parse_sym_set_phases(symbol, PARSE_SYM_PHASE_BOTH,
                         PARSE_SYM_PHASE_NONE, PARSE_SYM_PHASE_NONE);
    return true;
}

const char *
interface_package_for_module(const n00bb_interface_context_t *context,
                            core_string_t module)
{
    const char *package = nullptr;
    for (size_t i = 0; i < context->linked_interface_count; i++) {
        const th_env *environment = context->linked_interfaces[i];
        for (size_t m = 0; m < th_env_metadata_count(environment); m++) {
            th_env_metadata_view_t metadata = {0};
            if (!th_env_metadata_at(environment, m, &metadata)
                || metadata.owner_kind != TH_ENV_METADATA_ENVIRONMENT
                || (strcmp(metadata.key, "n00b.package-interface")
                    && strcmp(metadata.key, "n00b.module-interface"))
                || metadata.version != 1
                || strlen(metadata.owner_name) != module.u8_bytes
                || memcmp(metadata.owner_name, module.data,
                          module.u8_bytes)
                || !metadata.size
                || ((const char *)metadata.data)[metadata.size - 1] != '\0')
                continue;
            const char *candidate = (const char *)metadata.data;
            if (package && strcmp(package, candidate))
                return nullptr;
            package = candidate;
        }
    }
    return package;
}


static core_string_t
interface_visibility_name(th_env_visibility_t visibility)
{
    switch (visibility) {
    case TH_ENV_PRIVATE: return CORE_STRING_STATIC("private");
    case TH_ENV_PACKAGE: return CORE_STRING_STATIC("public");
    case TH_ENV_PUBLIC: return CORE_STRING_STATIC("public");
    }
    return (core_string_t){0};
}

static const char *
interface_metadata_failure(n00bb_tu_state_t *state, const char *owner,
                           const char *member, const char *reason)
{
    if (!state || !owner || !reason) return reason;
    size_t owner_size = strlen(owner);
    size_t member_size = member ? strlen(member) : 0;
    size_t reason_size = strlen(reason);
    size_t size = reason_size + owner_size + member_size + 5;
    char *message = core_arena_alloc(&state->semantic_values, size);
    if (!message) return reason;
    if (member)
        snprintf(message, size, "%s: %s.%s", reason, owner, member);
    else
        snprintf(message, size, "%s: %s", reason, owner);
    return message;
}

static parse_scope_t *
ensure_interface_package_scope(n00bb_interface_context_t *loader,
                               n00bb_tu_state_t *state,
                               parse_symtab_t *symtab,
                               core_translation_unit_t *unit,
                               core_string_t package)
{
    core_string_t root = unit->module.identifier;
    if (core_string_eq(package, root)) return module_scope(state);
    if (package.u8_bytes <= root.u8_bytes
        || memcmp(package.data, root.data, root.u8_bytes)
        || package.data[root.u8_bytes] != '.'
        || !interface_package_for_module(loader, package))
        return nullptr;
    size_t split = package.u8_bytes;
    while (split && package.data[split - 1] != '.') split--;
    if (!split) return nullptr;
    core_string_t parent_name = core_string_borrow(package.data, split - 1);
    core_string_t member_name = core_string_borrow(
        package.data + split, package.u8_bytes - split);
    parse_scope_t *parent = ensure_interface_package_scope(
        loader, state, symtab, unit, parent_name);
    if (!parent) return nullptr;
    parse_sym_entry_t *existing = parse_symtab_lookup_in(parent, member_name);
    if (existing)
        return existing->kind == PARSE_SYM_MODULE ? existing->exposed_scope
                                                  : nullptr;
    parse_symtab_enter_scope_all(symtab, parent);
    parse_symtab_push_scope_all(symtab, package);
    parse_scope_t *scope = parse_symtab_current_scope(
        symtab, CORE_STRING_STATIC(""));
    if (scope) {
        scope->scope_tag = CORE_STRING_STATIC("package-interface");
        parse_symtab_pop_scope_all(symtab);
    }
    parse_symtab_leave_scope_all(symtab);
    if (!scope) return nullptr;
    parse_symtab_enter_scope_all(symtab, parent);
    parse_sym_entry_t *binding = parse_symtab_add(symtab,
        CORE_STRING_STATIC(""), member_name, PARSE_SYM_MODULE, nullptr);
    parse_symtab_leave_scope_all(symtab);
    if (!binding || !parse_sym_set_binding(binding,
            CORE_STRING_STATIC("n00bb.package-unit"), unit)) return nullptr;
    binding->exposed_scope = scope;
    binding->visibility = CORE_STRING_STATIC("public");
    return scope;
}

static bool
materialize_interface_type_members(n00bb_interface_context_t *loader,
                                   n00bb_tu_state_t *state,
                                   parse_symtab_t *symtab,
                                   parse_scope_t *module_type_scope,
                                   parse_sym_entry_t *type_symbol,
                                   const th_env_declaration_view_t *declaration,
                                   const char **failure)
{
    if (!state || !symtab || !module_type_scope || !type_symbol ||
        !declaration || !declaration->type_shape ||
        declaration->type_shape_kind == TH_ENV_TYPE_SHAPE_NONE ||
        declaration->type_shape_kind == TH_ENV_TYPE_SHAPE_ALIAS ||
        declaration->type_shape_kind == TH_ENV_TYPE_SHAPE_NOMINAL) return true;
    parse_scope_t *type_namespace_scope = parse_symtab_scope_in_namespace(
        symtab, module_type_scope, CORE_STRING_STATIC("type"));
    if (!type_namespace_scope) return false;
    parse_symtab_enter_scope_all(symtab, type_namespace_scope);
    const char *scope_tag = declaration->type_shape_kind ==
            TH_ENV_TYPE_SHAPE_VARIANT ? "variant"
        : declaration->type_shape_kind == TH_ENV_TYPE_SHAPE_ENUM ? "enum"
        : declaration->type_shape_kind == TH_ENV_TYPE_SHAPE_BITSET ? "flagset"
        : "type";
    parse_symtab_push_scope_all(symtab,
                                core_string_from_cstr(type_symbol->name.data));
    parse_scope_t *member_scope = parse_symtab_current_scope(
        symtab, CORE_STRING_STATIC(""));
    if (!member_scope) {
        parse_symtab_pop_scope_all(symtab);
        return false;
    }
    member_scope->scope_tag = core_string_dup(
        core_string_from_cstr(scope_tag));
    member_scope->adt_kind = core_string_dup(
        core_string_from_cstr(scope_tag));
    type_symbol->member_scope = member_scope;
    type_symbol->adt_kind = core_string_dup(
        core_string_from_cstr(scope_tag));

    size_t metadata_size = 0;
    const uint8_t *metadata = interface_owned_metadata(loader,
        TH_ENV_METADATA_TYPE, declaration->qualified_name,
        "n00b.type-members", 1, &metadata_size);
    size_t enum_values_size = 0;
    const uint8_t *enum_values = declaration->type_shape_kind ==
            TH_ENV_TYPE_SHAPE_ENUM
        ? interface_owned_metadata(loader, TH_ENV_METADATA_TYPE,
            declaration->qualified_name, "n00b.enum-values", 1,
            &enum_values_size)
        : nullptr;
    size_t flagset_values_size = 0;
    const uint8_t *flagset_values = declaration->type_shape_kind ==
            TH_ENV_TYPE_SHAPE_BITSET
        ? interface_owned_metadata(loader, TH_ENV_METADATA_TYPE,
            declaration->qualified_name, "n00b.flagset-values", 1,
            &flagset_values_size)
        : nullptr;
    if (declaration->type_shape_kind == TH_ENV_TYPE_SHAPE_ENUM &&
        !enum_values) {
        parse_symtab_pop_scope_all(symtab);
        return false;
    }
    if (declaration->type_shape_kind == TH_ENV_TYPE_SHAPE_BITSET
        && !flagset_values) {
        parse_symtab_pop_scope_all(symtab);
        return false;
    }
    if (metadata) {
        if (metadata_size < 8 || read_u32_le(metadata) > 5) {
            parse_symtab_pop_scope_all(symtab);
            return false;
        }
        type_symbol->visibility = n00bb_visibility_from_code(
            read_u32_le(metadata));
    }

    const char *semantic_owner = interface_semantic_owner(
        type_symbol, declaration->qualified_name);
    bool ok = true;
    n00bb_enum_range_t range = {0};
    for (size_t i = 0; i < th_record_len(declaration->type_shape); i++) {
        const th_member *member = th_record_member(declaration->type_shape, i);
        if (!member || !member->name || !member->type) {
            ok = false;
            break;
        }
        parse_sym_kind_t kind = declaration->type_shape_kind ==
                TH_ENV_TYPE_SHAPE_VARIANT ? PARSE_SYM_TAG
            : declaration->type_shape_kind == TH_ENV_TYPE_SHAPE_ENUM
                || declaration->type_shape_kind == TH_ENV_TYPE_SHAPE_BITSET
                ? PARSE_SYM_ENUM_CONST : PARSE_SYM_VARIABLE;
        parse_sym_entry_t *entry = parse_symtab_add(symtab,
            CORE_STRING_STATIC(""), core_string_from_cstr(member->name),
            kind, nullptr);
        if (!entry) {
            ok = false;
            break;
        }
        entry->type = member->type;
        entry->owner_type = type_symbol;
        size_t shape_size = 0;
        const uint8_t *shape_data = interface_type_member_array_shape_metadata(
            loader, declaration->qualified_name, member->name, &shape_size);
        if (shape_data && !restore_interface_array_shape_data(loader, state,
                entry, shape_data, shape_size)) {
            if (failure) *failure = interface_metadata_failure(state,
                declaration->qualified_name, member->name,
                "interface field has invalid array-shape metadata");
            ok = false;
            break;
        }
        if (shape_data
            && !interface_shape_producers_available(loader,
                semantic_owner, parse_sym_binding(entry,
                    CORE_STRING_STATIC("ir.array-shape")))) {
            if (failure) *failure = interface_metadata_failure(state,
                declaration->qualified_name, member->name,
                "interface field is missing its compile-time IR closure");
            ok = false;
            break;
        }
        if (kind == PARSE_SYM_ENUM_CONST) {
            entry->mutability = PARSE_SYM_CONST;
            parse_sym_set_phases(entry, PARSE_SYM_PHASE_BOTH,
                PARSE_SYM_PHASE_NONE, PARSE_SYM_PHASE_COMPTIME);
            parse_sym_set_storage_duration(entry,
                PARSE_SYM_STORAGE_COMPTIME);
            if (declaration->type_shape_kind == TH_ENV_TYPE_SHAPE_BITSET) {
                n00bb_flagset_value_t value = {0};
                n00bb_flagset_info_t info = {0};
                if (!decode_interface_flagset_value(flagset_values,
                        flagset_values_size, member->name, &value, &info)) {
                    ok = false;
                    break;
                }
                n00bb_flagset_value_t *stored = core_arena_alloc(
                    &state->semantic_values, sizeof(*stored));
                n00bb_flagset_info_t *stored_info = parse_sym_binding(
                    type_symbol, CORE_STRING_STATIC("n00bb.flagset-info"));
                if (!stored) { ok = false; break; }
                *stored = value;
                if (!stored_info) {
                    stored_info = core_arena_alloc(&state->semantic_values,
                                                   sizeof(*stored_info));
                    if (!stored_info || !parse_sym_set_binding(type_symbol,
                            CORE_STRING_STATIC("n00bb.flagset-info"),
                            stored_info)) { ok = false; break; }
                    *stored_info = info;
                }
                entry->comptime_value = stored;
            }
            else {
                n00bb_enum_value_t value = {0};
                if (!decode_interface_enum_value(enum_values,
                        enum_values_size, member->name, &value)) {
                    ok = false;
                    break;
                }
                n00bb_enum_value_t *stored = core_arena_alloc(
                    &state->semantic_values, sizeof(*stored));
                if (!stored) { ok = false; break; }
                *stored = value;
                n00bb_enum_range_include(&range, value);
                entry->comptime_value = stored;
            }
            entry->has_comptime_value = true;
        }
        if (declaration->type_shape_kind == TH_ENV_TYPE_SHAPE_RECORD)
            entry->is_field = true;
        if (metadata) {
            uint32_t visibility = 0, flags = 0;
            uint64_t bit_offset = 0, bit_width = 0;
            if (!decode_interface_member_metadata(metadata, metadata_size,
                    member->name, &visibility, &flags, &bit_offset,
                    &bit_width)) {
                ok = false;
                break;
            }
            entry->visibility = n00bb_visibility_from_code(visibility);
            entry->has_bit_offset = (flags & 1u) != 0;
            entry->has_bit_width = (flags & 2u) != 0;
            entry->bit_offset = bit_offset;
            entry->bit_width = bit_width;
        }
    }
    if (ok && declaration->type_shape_kind == TH_ENV_TYPE_SHAPE_ENUM)
        ok = n00bb_bind_enum_range(state->types, &state->semantic_values,
                             type_symbol, range);
    const n00bb_flagset_info_t *flagset_info = ok
        && declaration->type_shape_kind == TH_ENV_TYPE_SHAPE_BITSET
        ? parse_sym_binding(type_symbol,
              CORE_STRING_STATIC("n00bb.flagset-info")) : nullptr;
    if (flagset_info && flagset_info->atomic_count
        && flagset_info->atomic_count <= 128) {
        th_type *representation = th_bitint(state->types,
            flagset_info->atomic_count, false);
        ok = representation && th_type_define(state->types,
            type_symbol->type, representation);
    }
    parse_symtab_pop_scope_all(symtab);
    return ok;
}

static bool
materialize_interface_module_symbols(n00bb_interface_context_t *loader,
                                     core_translation_unit_t *unit,
                                     const char **failure)
{
    if (failure) *failure = "unknown interface declaration error";
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->interface_module) return true;
    if (!state->annotations || !state->annotations->symtab) {
        if (failure) *failure = "interface TU has no symbol table";
        return false;
    }
    parse_symtab_t *symtab = state->annotations->symtab;
    parse_scope_t *values = module_scope(state);
    if (values) parse_symtab_ns(symtab, CORE_STRING_STATIC("type"));
    parse_scope_t *types = values ? parse_symtab_scope_in_namespace(
        symtab, values, CORE_STRING_STATIC("type")) : nullptr;
    if (!values || !types) {
        if (failure) *failure = "interface TU has no module namespace scopes";
        return false;
    }
    for (size_t e = 0; e < loader->linked_interface_count; e++) {
        const th_env *environment = loader->linked_interfaces[e];
        for (size_t m = 0; m < th_env_metadata_count(environment); m++) {
            th_env_metadata_view_t metadata = {0};
            if (!th_env_metadata_at(environment, m, &metadata)
                || metadata.owner_kind != TH_ENV_METADATA_ENVIRONMENT
                || strcmp(metadata.key, "n00b.package-interface")
                || metadata.version != 1)
                continue;
            core_string_t package = core_string_from_cstr(metadata.owner_name);
            if (package.u8_bytes <= unit->module.identifier.u8_bytes
                || memcmp(package.data, unit->module.identifier.data,
                          unit->module.identifier.u8_bytes)
                || package.data[unit->module.identifier.u8_bytes] != '.')
                continue;
            if (!ensure_interface_package_scope(loader, state, symtab, unit,
                                                package)) {
                if (failure) *failure =
                    "could not reconstruct an archived subpackage scope";
                return false;
            }
        }
        for (size_t i = 0; i < th_env_declaration_count(environment); i++) {
            th_env_declaration_view_t declaration = {0};
            if (!th_env_declaration_at(environment, i, &declaration)
                || !declaration.module
                || strlen(declaration.module)
                    != unit->module.identifier.u8_bytes
                || memcmp(declaration.module,
                    unit->module.identifier.data,
                    unit->module.identifier.u8_bytes))
                continue;
            const char *declaration_name = declaration.qualified_name
                ? strrchr(declaration.qualified_name, '.') : nullptr;
            if (declaration.kind == TH_ENV_DECLARATION_VALUE
                && declaration_name
                && !strncmp(declaration_name + 1,
                    "__callback_contract__",
                    sizeof("__callback_contract__") - 1))
                continue;
            core_string_t declaration_module = core_string_from_cstr(
                declaration.module);
            parse_scope_t *package_values = ensure_interface_package_scope(
                loader, state, symtab, unit, declaration_module);
            if (!package_values) continue;
            parse_scope_t *package_types = parse_symtab_scope_in_namespace(
                symtab, package_values, CORE_STRING_STATIC("type"));
            if (!package_types) {
                if (failure) *failure =
                    "interface package has no type namespace scope";
                return false;
            }
            bool is_value = declaration.kind == TH_ENV_DECLARATION_VALUE;
            bool is_function = declaration.kind == TH_ENV_DECLARATION_FUNCTION;
            parse_scope_t *scope = is_function || is_value
                ? package_values : package_types;
            core_string_t namespace_name = is_function || is_value
                ? CORE_STRING_STATIC("") : CORE_STRING_STATIC("type");
            const char *name = is_function || is_value
                ? declaration.root_name : strrchr(declaration.qualified_name, '.');
            if (!is_function && !is_value && name)
                name++;
            if (!name || !*name) {
                if (failure) *failure = "interface declaration has no local name";
                return false;
            }
            parse_sym_entry_t *existing = parse_symtab_lookup_in(
                scope, core_string_borrow(name, strlen(name)));
            if (existing && !is_function) continue;
            bool already_materialized = false;
            for (parse_sym_entry_t *prior = existing; is_function && prior;
                 prior = prior->shadowed) {
                if (prior->scope != scope) continue;
                const char *qualified = parse_sym_binding(prior,
                    CORE_STRING_STATIC("n00bb.interface-qualified-name"));
                if (qualified && !strcmp(qualified,
                                          declaration.qualified_name)) {
                    already_materialized = true;
                    break;
                }
            }
            if (already_materialized) continue;
            parse_scope_t *saved = parse_symtab_current_scope(symtab,
                                                               namespace_name);
            parse_symtab_enter_scope(symtab, namespace_name, scope);
            parse_sym_kind_t kind = is_function ? PARSE_SYM_FUNCTION
                : is_value ? PARSE_SYM_VARIABLE : PARSE_SYM_TYPEDEF;
            parse_sym_entry_t *symbol = parse_symtab_add(symtab,
                namespace_name, core_string_borrow(name, strlen(name)), kind,
                nullptr);
            if (saved)
                parse_symtab_enter_scope(symtab, namespace_name, saved);
            if (!symbol) {
                if (failure) *failure = "could not add interface symbol";
                return false;
            }
            if (is_function) {
                core_string_t qualified = store_semantic_string(state,
                    core_string_from_cstr(declaration.qualified_name));
                if (!qualified.data || !parse_sym_set_binding(symbol,
                        CORE_STRING_STATIC("n00bb.interface-qualified-name"),
                        qualified.data)) {
                    if (failure) *failure =
                        "could not retain interface function identity";
                    return false;
                }
            }
            th_env_metadata_owner_t metadata_kind = is_function
                ? TH_ENV_METADATA_FUNCTION
                : is_value ? TH_ENV_METADATA_VALUE
                : declaration.kind == TH_ENV_DECLARATION_TRAIT
                    ? TH_ENV_METADATA_TRAIT : TH_ENV_METADATA_TYPE;
            if (!materialize_interface_export_origin(loader, state, symbol,
                    metadata_kind, declaration.qualified_name,
                    declaration.module, failure))
                return false;
            n00bb_export_origin_t *origin = parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.export-origin"));
            symbol->visibility = interface_visibility_name(
                declaration.visibility);
            if (is_function) {
                th_type *signature = nullptr, *error_type = nullptr;
                if (!declaration.contract ||
                    !th_env_signature_channels(state->types,
                        declaration.contract, &signature, &error_type))
                {
                    if (failure) *failure = "interface function has invalid contract";
                    return false;
                }
                if (!th_func_error(signature))
                    signature = function_with_error_channel(state->types,
                        signature, error_type);
                if (!signature || !th_func_error(signature)) {
                    if (failure) *failure =
                        "interface function has no canonical failure channel";
                    return false;
                }
                symbol->type = signature;
                symbol->error_type = th_func_error(signature);
                symbol->callable_contract = signature;
                bool has_function_metadata = interface_owned_metadata(
                    loader, TH_ENV_METADATA_FUNCTION,
                    declaration.qualified_name, "n00b.function-params", 1,
                    nullptr) != nullptr;
                if ((has_function_metadata
                        || !interface_context_hook_symbol(symbol))
                    && !restore_interface_function_semantics(loader, symbol,
                        declaration.qualified_name)) {
                    if (failure) *failure =
                        "interface function has invalid or missing parameter metadata";
                    return false;
                }
                if (!restore_interface_function_shapes(loader, state,
                        symbol, declaration.qualified_name)) {
                    if (failure) *failure =
                        interface_metadata_failure(state,
                            declaration.qualified_name, nullptr,
                            "interface function has invalid array-shape metadata");
                    return false;
                }
                parse_sym_set_phases(symbol, PARSE_SYM_PHASE_BOTH,
                                     PARSE_SYM_PHASE_NONE,
                                     PARSE_SYM_PHASE_NONE);
            }
            else if (is_value) {
                symbol->type = declaration.value_type;
                size_t context_metadata_size = 0;
                bool is_context = interface_owned_metadata(loader,
                    TH_ENV_METADATA_VALUE, declaration.qualified_name,
                    "n00b.context", 1, &context_metadata_size) != nullptr;
                bool restored = is_context
                    ? restore_interface_context_semantics(loader, symbol,
                        declaration.qualified_name)
                    : restore_interface_value_semantics(loader, unit, symbol,
                        declaration.qualified_name)
                        && restore_interface_array_shape(loader, state,
                            symbol, declaration.qualified_name);
                if (!restored) {
                    if (failure) *failure =
                        interface_metadata_failure(state,
                            declaration.qualified_name, nullptr,
                            "interface value has invalid or missing n00b semantics");
                    return false;
                }
            }
            else {
                parse_sym_set_phases(symbol, PARSE_SYM_PHASE_BOTH,
                                     PARSE_SYM_PHASE_NONE,
                                     PARSE_SYM_PHASE_NONE);
                if (declaration.type_shape_kind ==
                    TH_ENV_TYPE_SHAPE_VARIANT) {
                    symbol->type = th_variant(state->types,
                        origin->declaration.data, nullptr, 0);
                }
                else if (declaration.type_shape_kind ==
                         TH_ENV_TYPE_SHAPE_ALIAS)
                    symbol->type = declaration.type_shape;
                else if (declaration.kind == TH_ENV_DECLARATION_TRAIT)
                    symbol->type = declaration.contract &&
                            th_kind_of(th_resolve(declaration.contract))
                                == TH_TRAIT
                        ? declaration.contract : nullptr;
                else
                    symbol->type = th_name(state->types,
                                           origin->declaration.data);
                symbol->adt_kind = declaration.kind ==
                        TH_ENV_DECLARATION_TRAIT
                    ? CORE_STRING_STATIC("trait")
                    : declaration.type_shape_kind ==
                            TH_ENV_TYPE_SHAPE_VARIANT
                        ? CORE_STRING_STATIC("variant")
                    : declaration.type_shape_kind == TH_ENV_TYPE_SHAPE_ENUM
                        ? CORE_STRING_STATIC("enum")
                    : declaration.type_shape_kind == TH_ENV_TYPE_SHAPE_BITSET
                        ? CORE_STRING_STATIC("flagset")
                    : declaration.type_shape_kind == TH_ENV_TYPE_SHAPE_ALIAS
                        ? CORE_STRING_STATIC("alias")
                    : declaration.type_shape_kind == TH_ENV_TYPE_SHAPE_NOMINAL
                        ? CORE_STRING_STATIC("nominal")
                        : CORE_STRING_STATIC("type");
                size_t exposure_size = 0;
                const uint8_t *exposure = interface_owned_metadata(loader,
                    TH_ENV_METADATA_TYPE, declaration.qualified_name,
                    "n00b.type-exposure", 1, &exposure_size);
                if (exposure) {
                    if (exposure_size != 1 || exposure[0] > 1) {
                        if (failure) *failure =
                            "interface type has invalid exposure metadata";
                        return false;
                    }
                    if (!exposure[0] && !parse_sym_set_binding(symbol,
                            CORE_STRING_STATIC("n00bb.opaque-type"),
                            symbol)) {
                        if (failure) *failure =
                            "could not retain interface type exposure";
                        return false;
                    }
                    if (exposure[0] && !parse_sym_set_binding(symbol,
                            CORE_STRING_STATIC("n00bb.exposed-record"),
                            symbol)) {
                        if (failure) *failure =
                            "could not retain interface type exposure";
                        return false;
                    }
                }
                if (declaration.type_shape &&
                    declaration.type_shape_kind !=
                        TH_ENV_TYPE_SHAPE_ALIAS &&
                    declaration.type_shape_kind !=
                        TH_ENV_TYPE_SHAPE_ENUM &&
                    declaration.type_shape_kind !=
                        TH_ENV_TYPE_SHAPE_BITSET &&
                    !th_type_define(state->types, symbol->type,
                                    declaration.type_shape)) {
                    if (failure) *failure =
                        "interface aliases disagree on a canonical type representation";
                    return false;
                }
                if (declaration.kind == TH_ENV_DECLARATION_TYPE
                    && !materialize_interface_callback(loader, state,
                        symtab, symbol, declaration.qualified_name,
                        failure))
                    return false;
                if (declaration.kind == TH_ENV_DECLARATION_TYPE) {
                    const char *member_failure = nullptr;
                    if (!materialize_interface_type_members(loader, state,
                            symtab, types, symbol, &declaration,
                            &member_failure)) {
                        if (failure) *failure = member_failure
                            ? member_failure : interface_metadata_failure(
                                state, declaration.qualified_name, nullptr,
                                "could not materialize interface type members");
                        return false;
                    }
                }
                if (declaration.kind == TH_ENV_DECLARATION_TYPE) {
                    size_t shape_size = 0;
                    const uint8_t *shape_data =
                        interface_owned_metadata(loader,
                            TH_ENV_METADATA_TYPE, declaration.qualified_name,
                            "n00b.array-shape", 1, &shape_size);
                    if (shape_data && !restore_interface_array_shape_data(
                            loader, state, symbol, shape_data, shape_size)) {
                        if (failure) *failure =
                            interface_metadata_failure(state,
                                declaration.qualified_name, nullptr,
                                "interface type has invalid array-shape metadata");
                        return false;
                    }
                    if (shape_data
                        && !interface_shape_producers_available(loader,
                            interface_semantic_owner(symbol,
                                declaration.qualified_name), parse_sym_binding(
                                symbol, CORE_STRING_STATIC("ir.array-shape")))) {
                        if (failure) *failure =
                            "interface type is missing its compile-time IR closure";
                        return false;
                    }
                }
            }
            if (!symbol->type) {
                if (failure) *failure = "could not instantiate interface type";
                return false;
            }
        }
    }
    return true;
}


bool
finish_semantic_unit(void *context, core_translation_unit_t *unit)
{
    n00bb_interface_context_t *loader = context;
    n00bb_tu_state_t *state = tu_state(unit);
    if (state->interface_module) {
        const char *failure = nullptr;
        if (!materialize_interface_module_symbols(loader, unit, &failure)) {
            core_diagnostics_report(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0046"),
                core_string_from_cstr(failure
                    ? failure : "could not restore interface symbols"));
            return false;
        }
    }
    if (!initialize_unit_package_interfaces(unit)
        || !populate_unit_package_interfaces(unit))
        return false;
    return true;
}


