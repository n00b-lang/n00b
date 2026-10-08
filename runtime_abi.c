#include "n00b-abi-v1.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "abi_directory.h"
#include "abi_ref_schema.h"
#include "abi_string.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#define XXH_INLINE_ALL
#include <xxhash.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

_Static_assert(sizeof(size_t) >= 8, "runtime ABI targets need 64-bit sizes");

#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#include <sys/syscall.h>

static void *snapshot_alloc(size_t bytes)
{
    register long r10 __asm__("r10") = MAP_PRIVATE | MAP_ANONYMOUS;
    register long r8 __asm__("r8") = -1;
    register long r9 __asm__("r9") = 0;
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"((long)SYS_mmap), "D"(0L), "S"(bytes),
                       "d"((long)(PROT_READ | PROT_WRITE)),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return result < 0 ? nullptr : (void *)result;
}

static void snapshot_free(void *address, size_t bytes)
{
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"((long)SYS_munmap), "D"(address), "S"(bytes)
                     : "rcx", "r11", "memory");
    (void)result;
}
#define N00B_ABI_HOST_TARGET N00B_ABI_TARGET_X86_64_LINUX
#elif defined(__APPLE__) && defined(__aarch64__)
#include <sys/mman.h>

static void *snapshot_alloc(size_t bytes)
{
    void *value = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
    return value == MAP_FAILED ? nullptr : value;
}

static void snapshot_free(void *address, size_t bytes)
{
    munmap(address, bytes);
}
#define N00B_ABI_HOST_TARGET N00B_ABI_TARGET_ARM64_MACOS
#elif defined(_WIN32) && defined(_M_X64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static void *snapshot_alloc(size_t bytes)
{
    return VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static void snapshot_free(void *address, size_t bytes)
{
    (void)bytes;
    VirtualFree(address, 0, MEM_RELEASE);
}
#define N00B_ABI_HOST_TARGET N00B_ABI_TARGET_X86_64_WINDOWS
#else
#error "runtime ABI snapshot allocation needs a supported target"
#endif

struct n00b_abi_snapshot_v1 {
    uint64_t generation;
    const uint8_t *metadata;
    uint64_t metadata_bytes;
    n00bb_abi_directory_entry_t tables[N00B_ABI_TABLE_SPECIALIZATION_LINK + 1];
};

static _Atomic(const n00b_abi_snapshot_v1 *) current_snapshot;
static atomic_flag registration_lock = ATOMIC_FLAG_INIT;

typedef struct {
    const uint8_t *module;
    uint64_t module_bytes;
    const uint8_t *name;
    uint64_t name_bytes;
} symbol_path_t;

static bool
same_bytes(const uint8_t *left, const uint8_t *right, uint64_t count)
{
    for (uint64_t i = 0; i < count; i++)
        if (left[i] != right[i]) return false;
    return true;
}

static bool
symbol_path(const n00b_abi_snapshot_v1 *snapshot, uint32_t symbol_index,
            symbol_path_t *out)
{
    const n00b_abi_symbol_v1 *symbol = n00b_abi_v1_symbol(snapshot,
                                                            symbol_index);
    if (!symbol || (symbol->kind != N00B_ABI_SYMBOL_TYPE
                    && symbol->kind != N00B_ABI_SYMBOL_FUNCTION
                    && symbol->kind != N00B_ABI_SYMBOL_VARIABLE))
        return false;
    const n00b_abi_scope_v1 *scope = n00b_abi_v1_scope(snapshot,
                                                       symbol->scope_index);
    const n00b_abi_module_v1 *module = n00b_abi_v1_module(snapshot,
                                                           symbol->module_index);
    if (!scope || !module || scope->parent_scope_index
        || scope->module_index != symbol->module_index) return false;
    symbol_path_t path = {0};
    if (n00b_abi_v1_string_bytes(snapshot, &module->full_path,
            &path.module, &path.module_bytes) != 1
        || n00b_abi_v1_string_bytes(snapshot, &symbol->name,
            &path.name, &path.name_bytes) != 1
        || !path.name_bytes || path.module_bytes > SIZE_MAX - 1
        || path.name_bytes > SIZE_MAX - path.module_bytes - 1)
        return false;
    *out = path;
    return true;
}

static bool
path_matches(symbol_path_t path, const uint8_t *bytes, uint64_t count)
{
    return count == path.module_bytes + 1 + path.name_bytes
        && same_bytes(bytes, path.module, path.module_bytes)
        && bytes[path.module_bytes] == '.'
        && same_bytes(bytes + path.module_bytes + 1, path.name,
                      path.name_bytes);
}

static uint32_t
registration_error(n00b_abi_error_v1 *error, uint32_t status,
                   uint32_t kind, uint32_t index, uint64_t offset)
{
    if (error) *error = (n00b_abi_error_v1){
        .status = status, .table_kind = kind, .record_index = index,
        .byte_offset = offset,
    };
    return status;
}

static bool
header_valid(const n00b_abi_header_v1 *header, uint64_t bytes)
{
    if (!header) return false;
    for (size_t i = 0; i < 8; i++)
        if (header->magic[i] != (uint8_t)N00B_ABI_V1_MAGIC[i]) return false;
    return header->container_version == N00B_ABI_V1_CONTAINER_VERSION
        && header->header_bytes == sizeof(*header)
        && header->target_id == N00B_ABI_HOST_TARGET
        && header->abi_profile_id == N00B_ABI_V1_PROFILE_ID
        && !header->feature_bits && !header->cpu_profile
        && header->data_layout_profile == N00B_ABI_DATA_LAYOUT_PROFILE_V1
        && !(header->artifact_capabilities
             & ~(N00B_ABI_CAP_WRAPPING | N00B_ABI_CAP_DEBUG
                 | N00B_ABI_CAP_OPTIMIZED | N00B_ABI_CAP_SANITIZED))
        && header->required_alignment
        && !(header->required_alignment & (header->required_alignment - 1))
        && header->total_extent >= sizeof(*header)
        && header->total_extent <= bytes
        && header->directory_offset >= sizeof(*header)
        && header->directory_offset <= header->total_extent
        && header->directory_count <= N00B_ABI_TABLE_SPECIALIZATION_LINK;
}

static const n00bb_abi_directory_entry_t *
table_for(const n00b_abi_snapshot_v1 *snapshot, uint32_t kind)
{
    if (!snapshot || kind < N00B_ABI_TABLE_PACKAGE
        || kind > N00B_ABI_TABLE_SPECIALIZATION_LINK) return nullptr;
    const n00bb_abi_directory_entry_t *table = &snapshot->tables[kind];
    return table->kind ? table : nullptr;
}

static bool
invalid_record(n00b_abi_error_v1 *error, uint32_t kind,
               uint32_t index, uint64_t offset)
{
    registration_error(error, N00B_ABI_BAD_METADATA, kind, index, offset);
    return false;
}

static uint32_t
member_table_kind(uint32_t type_kind)
{
    switch (type_kind) {
    case N00B_ABI_TYPE_RECORD:
    case N00B_ABI_TYPE_TUPLE: return N00B_ABI_TABLE_FIELD;
    case N00B_ABI_TYPE_VARIANT: return N00B_ABI_TABLE_ARM;
    case N00B_ABI_TYPE_ENUM:
    case N00B_ABI_TYPE_FLAGSET: return N00B_ABI_TABLE_ENUM_ITEM;
    case N00B_ABI_TYPE_TRAIT: return N00B_ABI_TABLE_TRAIT_METHOD;
    default: return 0;
    }
}

static bool
metadata_references_valid(const n00b_abi_snapshot_v1 *snapshot,
                          n00b_abi_error_v1 *error)
{
    for (uint32_t kind = N00B_ABI_TABLE_PACKAGE;
         kind <= N00B_ABI_TABLE_SPECIALIZATION_LINK; kind++) {
        const n00bb_abi_directory_entry_t *table = table_for(snapshot, kind);
        if (!table) continue;
        if (kind == N00B_ABI_TABLE_REQUIREMENT
            || kind == N00B_ABI_TABLE_INDEX_PATCH)
            return invalid_record(error, kind, 0, table->file_offset);
        if (kind == N00B_ABI_TABLE_U32_STREAM
            || kind == N00B_ABI_TABLE_U64_STREAM
            || kind == N00B_ABI_TABLE_BYTE_STREAM) continue;
        if (!table->record_count)
            return invalid_record(error, kind, 0, table->file_offset);
        if (kind != N00B_ABI_TABLE_TYPE) {
            const uint8_t *reserved = n00b_abi_v1_record(snapshot, kind,
                                                          0, nullptr);
            for (uint32_t byte = 0; byte < table->record_bytes; byte++)
                if (reserved[byte])
                    return invalid_record(error, kind, 0,
                                          table->file_offset + byte);
        }
        uint32_t first = kind == N00B_ABI_TABLE_TYPE ? 0 : 1;
        for (uint32_t row = first; row < table->record_count; row++) {
            const uint8_t *bytes = n00b_abi_v1_record(snapshot, kind,
                                                       row, nullptr);
            for (size_t i = 0; i < sizeof(abi_index_columns)
                 / sizeof(abi_index_columns[0]); i++) {
                const abi_ref_column_t *field = &abi_index_columns[i];
                if (field->row_kind != kind) continue;
                if (field->offset > table->record_bytes
                    || sizeof(uint32_t) > table->record_bytes - field->offset)
                    return invalid_record(error, kind, row,
                        table->file_offset + (uint64_t)row
                            * table->record_bytes + field->offset);
                uint32_t value = *(const uint32_t *)(bytes + field->offset);
                if (field->optional_type
                    && value == N00B_ABI_V1_NO_TYPE_INDEX) continue;
                if (!value && field->required)
                    return invalid_record(error, kind, row,
                        table->file_offset + (uint64_t)row
                            * table->record_bytes + field->offset);
                if (!value && field->target_kind != N00B_ABI_TABLE_TYPE)
                    continue;
                const n00bb_abi_directory_entry_t *target =
                    table_for(snapshot, field->target_kind);
                if (!target || value >= target->record_count)
                    return invalid_record(error, kind, row,
                        table->file_offset + (uint64_t)row
                            * table->record_bytes + field->offset);
            }
            for (size_t i = 0; i < sizeof(abi_range_columns)
                 / sizeof(abi_range_columns[0]); i++) {
                const abi_range_column_t *field = &abi_range_columns[i];
                if (field->row_kind != kind) continue;
                if (field->offset > table->record_bytes
                    || sizeof(n00b_abi_range_v1)
                        > table->record_bytes - field->offset)
                    return invalid_record(error, kind, row,
                        table->file_offset + (uint64_t)row
                            * table->record_bytes + field->offset);
                const n00b_abi_range_v1 *range =
                    (const void *)(bytes + field->offset);
                uint64_t offset = table->file_offset + (uint64_t)row
                    * table->record_bytes + field->offset;
                if (!range->count) {
                    if (range->first)
                        return invalid_record(error, kind, row, offset);
                    continue;
                }
                const n00bb_abi_directory_entry_t *stream =
                    table_for(snapshot, field->stream_kind);
                if (!stream || range->first >= stream->record_count
                    || range->count > stream->record_count - range->first)
                    return invalid_record(error, kind, row, offset);
                if (field->stream_kind != N00B_ABI_TABLE_U32_STREAM)
                    continue;
                uint32_t target_kind = field->target_kind;
                if (kind == N00B_ABI_TABLE_TYPE
                    && field->offset == offsetof(n00b_abi_type_v1,
                                                 fields_or_arms))
                    target_kind = member_table_kind(
                        ((const n00b_abi_type_v1 *)bytes)->kind);
                if (kind == N00B_ABI_TABLE_SCC
                    && field->offset == offsetof(n00b_abi_scc_v1,
                                                 lexical_order))
                    target_kind = UINT32_MAX;
                if (!target_kind) return invalid_record(error, kind, row,
                                                        offset);
                if (target_kind == UINT32_MAX) continue;
                const n00bb_abi_directory_entry_t *target =
                    table_for(snapshot, target_kind);
                if (!target) return invalid_record(error, kind, row, offset);
                for (uint32_t j = 0; j < range->count; j++) {
                    const uint32_t *value = n00b_abi_v1_record(snapshot,
                        N00B_ABI_TABLE_U32_STREAM, range->first + j,
                        nullptr);
                    if (!value || *value >= target->record_count
                        || (!*value && target_kind != N00B_ABI_TABLE_TYPE))
                        return invalid_record(error, kind, row, offset);
                }
            }
        }
    }
    uint32_t type_count = n00b_abi_v1_table_count(snapshot, N00B_ABI_TABLE_TYPE);
    for (uint32_t i = 0; i < type_count; i++) {
        const n00b_abi_type_v1 *type = n00b_abi_v1_type(snapshot, i);
        if (type->layout_index) {
            const n00b_abi_layout_v1 *layout = n00b_abi_v1_layout(snapshot,
                type->layout_index);
            if (!layout || layout->type_index != i)
                return invalid_record(error, N00B_ABI_TABLE_TYPE, i, 0);
        }
        if (type->tracing_index) {
            const n00b_abi_trace_v1 *trace = n00b_abi_v1_trace(snapshot,
                type->tracing_index);
            if (!trace || trace->type_index != i)
                return invalid_record(error, N00B_ABI_TABLE_TYPE, i, 0);
        }
        uint32_t member_kind = member_table_kind(type->kind);
        for (uint32_t j = 0; j < type->fields_or_arms.count; j++) {
            uint32_t member_index;
            if (!member_kind || n00b_abi_v1_index_at(snapshot,
                    type->fields_or_arms, j, &member_index) != 1)
                return invalid_record(error, N00B_ABI_TABLE_TYPE, i, 0);
            const void *member = n00b_abi_v1_record(snapshot, member_kind,
                member_index, nullptr);
            uint32_t parent = member_kind == N00B_ABI_TABLE_FIELD
                ? ((const n00b_abi_field_v1 *)member)->parent_type_index
                : member_kind == N00B_ABI_TABLE_ARM
                ? ((const n00b_abi_arm_v1 *)member)->field.parent_type_index
                : member_kind == N00B_ABI_TABLE_ENUM_ITEM
                ? ((const n00b_abi_enum_item_v1 *)member)->parent_type_index
                : ((const n00b_abi_trait_method_v1 *)member)->trait_type_index;
            if (parent != i)
                return invalid_record(error, N00B_ABI_TABLE_TYPE, i, 0);
        }
    }
    return true;
}

static bool
metadata_storage_valid(const n00b_abi_snapshot_v1 *snapshot,
                       n00b_abi_error_v1 *error)
{
    uint32_t module_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_MODULE);
    uint32_t storage_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_STORAGE);
    uint32_t relocation_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_TLS_RELOCATION);
    if (!storage_count && !relocation_count) return true;
    size_t scratch_bytes = (size_t)storage_count + relocation_count;
    uint8_t *seen = snapshot_alloc(scratch_bytes);
    if (!seen) return false;
    uint8_t *seen_relocations = seen + storage_count;
    bool valid = true;

    for (uint32_t module_index = 1; valid && module_index < module_count;
         module_index++) {
        const n00b_abi_module_v1 *module = n00b_abi_v1_module(snapshot,
            module_index);
        n00b_abi_range_v1 ranges[] = {module->static_storage,
                                       module->tls_storage};
        for (size_t kind = 0; valid && kind < 2; kind++)
            for (uint32_t i = 0; valid && i < ranges[kind].count; i++) {
                uint32_t storage_index = 0;
                const n00b_abi_storage_v1 *storage = nullptr;
                valid = n00b_abi_v1_index_at(snapshot, ranges[kind], i,
                    &storage_index) == 1 && storage_index
                    && storage_index < storage_count && !seen[storage_index];
                if (valid) {
                    storage = n00b_abi_v1_storage(snapshot, storage_index);
                    valid = storage && storage->module_index == module_index
                        && (kind ? storage->segment_kind == N00B_ABI_STORAGE_TLS
                                 : storage->segment_kind
                                     == N00B_ABI_STORAGE_READ_ONLY
                                   || storage->segment_kind
                                     == N00B_ABI_STORAGE_WRITABLE);
                }
                if (!valid)
                    invalid_record(error, N00B_ABI_TABLE_MODULE,
                                   module_index, 0);
                else seen[storage_index] = 1;
            }
    }
    for (uint32_t index = 1; valid && index < storage_count; index++) {
        const n00b_abi_storage_v1 *storage = n00b_abi_v1_storage(snapshot,
                                                                  index);
        valid = storage && seen[index] && storage->segment_index
            && storage->segment_kind >= N00B_ABI_STORAGE_READ_ONLY
            && storage->segment_kind <= N00B_ABI_STORAGE_TLS
            && storage->byte_offset <= UINT64_MAX - storage->byte_length;
        if (valid && storage->symbol_index) {
            const n00b_abi_symbol_v1 *symbol = n00b_abi_v1_symbol(snapshot,
                storage->symbol_index);
            valid = symbol && symbol->module_index == storage->module_index
                && symbol->type_index == storage->type_index;
        }
        if (valid && storage->tls_relocations.count)
            valid = storage->segment_kind == N00B_ABI_STORAGE_TLS;
        if (!valid) {
            invalid_record(error, N00B_ABI_TABLE_STORAGE, index, 0);
            break;
        }
        for (uint32_t i = 0; valid && i < storage->tls_relocations.count;
             i++) {
            uint32_t relocation_index = 0;
            valid = n00b_abi_v1_index_at(snapshot, storage->tls_relocations,
                i, &relocation_index) == 1 && relocation_index
                && relocation_index < relocation_count
                && !seen_relocations[relocation_index];
            const n00b_abi_tls_relocation_v1 *relocation = valid
                ? n00b_abi_v1_record(snapshot, N00B_ABI_TABLE_TLS_RELOCATION,
                                     relocation_index, nullptr) : nullptr;
            const n00b_abi_storage_v1 *target = relocation
                ? n00b_abi_v1_storage(snapshot,
                    relocation->referenced_storage_index) : nullptr;
            valid = valid && relocation && target
                && relocation->template_storage_index == index
                && relocation->target_kind == target->segment_kind
                && relocation->write_byte_offset <= storage->byte_length
                && sizeof(void *) <= storage->byte_length
                    - relocation->write_byte_offset;
            if (!valid)
                invalid_record(error, N00B_ABI_TABLE_TLS_RELOCATION,
                               relocation_index, 0);
            else seen_relocations[relocation_index] = 1;
        }
    }
    for (uint32_t index = 1; valid && index < relocation_count; index++)
        if (!seen_relocations[index])
            valid = invalid_record(error, N00B_ABI_TABLE_TLS_RELOCATION,
                                   index, 0);
    snapshot_free(seen, scratch_bytes);
    return valid;
}

static bool
metadata_dispatch_valid(const n00b_abi_snapshot_v1 *snapshot,
                        n00b_abi_error_v1 *error)
{
    uint32_t type_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_TYPE);
    uint32_t dispatch_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_DISPATCH);
    uint32_t entry_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_DISPATCH_ENTRY);
    size_t receiver_bytes = (size_t)type_count * sizeof(uint32_t);
    size_t scratch_bytes = receiver_bytes + entry_count;
    uint8_t *scratch = snapshot_alloc(scratch_bytes);
    if (!scratch) return false;
    uint32_t *seen_receivers = (void *)scratch;
    uint8_t *seen_entries = scratch + receiver_bytes;
    bool valid = true;
    for (uint32_t index = 1; valid && index < dispatch_count; index++) {
        const n00b_abi_dispatch_v1 *dispatch = n00b_abi_v1_record(snapshot,
            N00B_ABI_TABLE_DISPATCH, index, nullptr);
        const n00b_abi_type_v1 *trait = n00b_abi_v1_type(snapshot,
            dispatch->trait_type_index);
        uint32_t method_index = 0;
        valid = trait && trait->kind == N00B_ABI_TYPE_TRAIT
            && dispatch->method_ordinal < trait->fields_or_arms.count
            && n00b_abi_v1_index_at(snapshot, trait->fields_or_arms,
                dispatch->method_ordinal, &method_index) == 1;
        const n00b_abi_trait_method_v1 *method = valid
            ? n00b_abi_v1_record(snapshot, N00B_ABI_TABLE_TRAIT_METHOD,
                                 method_index, nullptr) : nullptr;
        valid = valid && method
            && method->trait_type_index == dispatch->trait_type_index
            && method->declaration_ordinal == dispatch->method_ordinal;
        for (uint32_t prior_index = 1; valid && prior_index < index;
             prior_index++) {
            const n00b_abi_dispatch_v1 *prior = n00b_abi_v1_record(snapshot,
                N00B_ABI_TABLE_DISPATCH, prior_index, nullptr);
            if (prior->trait_type_index == dispatch->trait_type_index
                && prior->method_ordinal == dispatch->method_ordinal
                && prior->generation == dispatch->generation)
                valid = false;
        }
        if (!valid) {
            invalid_record(error, N00B_ABI_TABLE_DISPATCH, index, 0);
            break;
        }
        for (uint32_t i = 0; valid && i < dispatch->entries.count; i++) {
            uint32_t entry_index = 0;
            valid = n00b_abi_v1_index_at(snapshot, dispatch->entries, i,
                &entry_index) == 1 && entry_index
                && entry_index < entry_count && !seen_entries[entry_index];
            const n00b_abi_dispatch_entry_v1 *entry = valid
                ? n00b_abi_v1_record(snapshot, N00B_ABI_TABLE_DISPATCH_ENTRY,
                                     entry_index, nullptr) : nullptr;
            const n00b_abi_type_v1 *receiver = entry
                ? n00b_abi_v1_type(snapshot, entry->receiver_type_index)
                : nullptr;
            valid = valid && receiver
                && !(receiver->flags & N00B_ABI_TYPE_FLAG_NON_CONCRETE)
                && seen_receivers[entry->receiver_type_index] != index;
            if (!valid)
                invalid_record(error, N00B_ABI_TABLE_DISPATCH_ENTRY,
                               entry_index, 0);
            else {
                seen_entries[entry_index] = 1;
                seen_receivers[entry->receiver_type_index] = index;
            }
        }
    }
    for (uint32_t index = 1; valid && index < entry_count; index++)
        if (!seen_entries[index])
            valid = invalid_record(error, N00B_ABI_TABLE_DISPATCH_ENTRY,
                                   index, 0);
    snapshot_free(scratch, scratch_bytes);
    return valid;
}

static bool
metadata_name_slots_valid(const n00b_abi_snapshot_v1 *snapshot,
                          n00b_abi_error_v1 *error)
{
    uint32_t symbol_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_SYMBOL);
    uint32_t slot_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_NAME_SLOT);
    if (slot_count == 1)
        return invalid_record(error, N00B_ABI_TABLE_NAME_SLOT, 0, 0);
    size_t longest = 0;
    bool has_empty = false;
    for (uint32_t i = 1; i < slot_count; i++) {
        const n00b_abi_name_slot_v1 *slot = n00b_abi_v1_record(snapshot,
            N00B_ABI_TABLE_NAME_SLOT, i, nullptr);
        if (!slot || slot->reserved_zero
            || (!slot->symbol_index && slot->name_hash))
            return invalid_record(error, N00B_ABI_TABLE_NAME_SLOT, i, 0);
        if (!slot->symbol_index) { has_empty = true; continue; }
        symbol_path_t path;
        if (!symbol_path(snapshot, slot->symbol_index, &path))
            return invalid_record(error, N00B_ABI_TABLE_NAME_SLOT, i, 0);
        size_t length = (size_t)(path.module_bytes + 1 + path.name_bytes);
        if (length > longest) longest = length;
    }
    if (slot_count && !has_empty)
        return invalid_record(error, N00B_ABI_TABLE_NAME_SLOT, 0, 0);
    if (longest > SIZE_MAX - symbol_count)
        return invalid_record(error, N00B_ABI_TABLE_NAME_SLOT, 0, 0);
    size_t scratch_bytes = (size_t)symbol_count + longest;
    if (!scratch_bytes) scratch_bytes = 1;
    uint8_t *scratch = snapshot_alloc(scratch_bytes);
    if (!scratch) return false;
    uint8_t *seen = scratch;
    uint8_t *full_path = scratch + symbol_count;
    bool valid = true;
    uint32_t capacity = slot_count ? slot_count - 1 : 0;
    for (uint32_t i = 1; valid && i < slot_count; i++) {
        const n00b_abi_name_slot_v1 *slot = n00b_abi_v1_record(snapshot,
            N00B_ABI_TABLE_NAME_SLOT, i, nullptr);
        if (!slot->symbol_index) continue;
        symbol_path_t path;
        valid = !seen[slot->symbol_index]
            && symbol_path(snapshot, slot->symbol_index, &path);
        if (!valid) {
            invalid_record(error, N00B_ABI_TABLE_NAME_SLOT, i, 0);
            break;
        }
        seen[slot->symbol_index] = 1;
        size_t length = (size_t)(path.module_bytes + 1 + path.name_bytes);
        for (size_t byte = 0; byte < path.module_bytes; byte++)
            full_path[byte] = path.module[byte];
        full_path[path.module_bytes] = '.';
        for (size_t byte = 0; byte < path.name_bytes; byte++)
            full_path[path.module_bytes + 1 + byte] = path.name[byte];
        uint64_t hash = XXH3_64bits(full_path, length);
        valid = hash == slot->name_hash;
        uint32_t probe = 1 + (uint32_t)(hash % capacity);
        while (valid && probe != i) {
            const n00b_abi_name_slot_v1 *prior = n00b_abi_v1_record(
                snapshot, N00B_ABI_TABLE_NAME_SLOT, probe, nullptr);
            valid = prior && prior->symbol_index;
            if (valid && prior->name_hash == hash) {
                symbol_path_t prior_path;
                valid = symbol_path(snapshot, prior->symbol_index,
                                    &prior_path)
                    && !path_matches(prior_path, full_path, length);
            }
            probe = probe == capacity ? 1 : probe + 1;
        }
        if (!valid)
            invalid_record(error, N00B_ABI_TABLE_NAME_SLOT, i, 0);
    }
    for (uint32_t i = 1; valid && i < symbol_count; i++) {
        const n00b_abi_symbol_v1 *symbol = n00b_abi_v1_symbol(snapshot, i);
        if (!symbol || (symbol->kind != N00B_ABI_SYMBOL_TYPE
                        && symbol->kind != N00B_ABI_SYMBOL_FUNCTION
                        && symbol->kind != N00B_ABI_SYMBOL_VARIABLE))
            continue;
        const n00b_abi_scope_v1 *scope = n00b_abi_v1_scope(snapshot,
                                                            symbol->scope_index);
        if (scope && scope->parent_scope_index) continue;
        symbol_path_t path;
        if (!symbol_path(snapshot, i, &path) || !seen[i])
            valid = invalid_record(error, N00B_ABI_TABLE_SYMBOL, i, 0);
    }
    snapshot_free(scratch, scratch_bytes);
    return valid;
}

typedef struct {
    uint32_t type_index;
    uint32_t next_link;
} specialization_frame_t;

static bool
specialization_graph_valid(const n00b_abi_snapshot_v1 *snapshot)
{
    uint32_t type_count = n00b_abi_v1_table_count(snapshot, N00B_ABI_TABLE_TYPE);
    uint32_t link_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_SPECIALIZATION_LINK);
    if (!type_count || (table_for(snapshot, N00B_ABI_TABLE_SPECIALIZATION_LINK)
                        && !link_count)) return false;
    size_t frame_bytes = (size_t)type_count * sizeof(specialization_frame_t);
    size_t scratch_bytes = frame_bytes + type_count + link_count;
    uint8_t *scratch = snapshot_alloc(scratch_bytes);
    if (!scratch) return false;
    specialization_frame_t *frames = (void *)scratch;
    uint8_t *colors = scratch + frame_bytes;
    uint8_t *owned_links = colors + type_count;
    bool valid = true;
    if (link_count) {
        const uint32_t *reserved = n00b_abi_v1_record(snapshot,
            N00B_ABI_TABLE_SPECIALIZATION_LINK, 0, nullptr);
        valid = reserved && !*reserved;
    }
    for (uint32_t child = 0; valid && child < type_count; child++) {
        const n00b_abi_type_v1 *type = n00b_abi_v1_type(snapshot, child);
        if (!type) { valid = false; break; }
        uint32_t start = type->specialization_start_ix;
        uint32_t end = type->specialization_end_ix;
        if ((!start && end) || (start && (end <= start || end > link_count))) {
            valid = false;
            break;
        }
        for (uint32_t i = start; valid && i < end; i++) {
            const uint32_t *parent = n00b_abi_v1_record(snapshot,
                N00B_ABI_TABLE_SPECIALIZATION_LINK, i, nullptr);
            if (!parent || *parent >= type_count || *parent == child
                || owned_links[i]) { valid = false; break; }
            owned_links[i] = 1;
            for (uint32_t j = start; j < i; j++) {
                const uint32_t *previous = n00b_abi_v1_record(snapshot,
                    N00B_ABI_TABLE_SPECIALIZATION_LINK, j, nullptr);
                if (!previous || *previous == *parent) {
                    valid = false;
                    break;
                }
            }
        }
    }
    for (uint32_t i = 1; valid && i < link_count; i++)
        if (!owned_links[i]) valid = false;
    for (uint32_t root = 0; valid && root < type_count; root++) {
        if (colors[root]) continue;
        size_t depth = 0;
        const n00b_abi_type_v1 *type = n00b_abi_v1_type(snapshot, root);
        frames[depth++] = (specialization_frame_t){
            .type_index = root,
            .next_link = type->specialization_start_ix,
        };
        colors[root] = 1;
        while (depth && valid) {
            specialization_frame_t *frame = &frames[depth - 1];
            type = n00b_abi_v1_type(snapshot, frame->type_index);
            if (frame->next_link == type->specialization_end_ix) {
                colors[frame->type_index] = 2;
                depth--;
                continue;
            }
            const uint32_t *parent = n00b_abi_v1_record(snapshot,
                N00B_ABI_TABLE_SPECIALIZATION_LINK, frame->next_link++,
                nullptr);
            if (!parent || colors[*parent] == 1) { valid = false; break; }
            if (colors[*parent] == 2) continue;
            const n00b_abi_type_v1 *parent_type =
                n00b_abi_v1_type(snapshot, *parent);
            frames[depth++] = (specialization_frame_t){
                .type_index = *parent,
                .next_link = parent_type->specialization_start_ix,
            };
            colors[*parent] = 1;
        }
    }
    snapshot_free(scratch, scratch_bytes);
    return valid;
}

uint32_t
n00b_abi_v1_register_image(const n00b_abi_image_descriptor_v1 *image,
                           uint64_t *published_generation,
                           n00b_abi_error_v1 *error)
{
    if (error) *error = (n00b_abi_error_v1){0};
    if (!image || image->descriptor_version != 1 || image->reserved_zero
        || !image->metadata
        || (uintptr_t)image->metadata % _Alignof(n00b_abi_header_v1)
        || image->metadata_bytes < sizeof(*image->metadata)
        || image->metadata_bytes > SIZE_MAX)
        return registration_error(error, N00B_ABI_BAD_ARGUMENT, 0, 0, 0);
    const n00b_abi_header_v1 *header = image->metadata;
    if (!header_valid(header, image->metadata_bytes)
        || (uintptr_t)header % header->required_alignment)
        return registration_error(error, N00B_ABI_BAD_METADATA, 0, 0, 0);
    n00b_abi_snapshot_v1 *candidate = snapshot_alloc(sizeof(*candidate));
    if (!candidate)
        return registration_error(error, N00B_ABI_BAD_METADATA, 0, 0, 0);
    candidate->metadata = (const uint8_t *)header;
    candidate->metadata_bytes = header->total_extent;
    size_t schema_count = 0;
    const n00bb_abi_table_schema_t *schemas =
        n00bb_abi_v1_table_schemas(&schema_count);
    n00bb_abi_directory_entry_t directory[N00B_ABI_TABLE_SPECIALIZATION_LINK];
    bool valid = n00bb_abi_directory_validate(candidate->metadata,
        (size_t)candidate->metadata_bytes, sizeof(*header),
        header->directory_offset, header->directory_count,
        header->total_extent, schemas, schema_count, false, directory);
    for (uint32_t i = 0; valid && i < header->directory_count; i++) {
        const n00bb_abi_directory_entry_t *entry = &directory[i];
        if (entry->kind > N00B_ABI_TABLE_SPECIALIZATION_LINK
            || candidate->tables[entry->kind].kind) valid = false;
        else {
            n00bb_abi_directory_entry_t *target =
                &candidate->tables[entry->kind];
            target->kind = entry->kind;
            target->schema_version = entry->schema_version;
            target->record_bytes = entry->record_bytes;
            target->record_count = entry->record_count;
            target->file_offset = entry->file_offset;
            target->byte_length = entry->byte_length;
        }
    }
    const n00bb_abi_directory_entry_t *types = table_for(candidate,
        N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *requirements = table_for(candidate,
        N00B_ABI_TABLE_REQUIREMENT);
    const n00bb_abi_directory_entry_t *patches = table_for(candidate,
        N00B_ABI_TABLE_INDEX_PATCH);
    if (valid && (!types || !types->record_count
        || (requirements && requirements->record_count > 1)
        || (patches && patches->record_count > 1))) valid = false;
    if (valid) {
        const n00b_abi_type_v1 *nil = (const void *)(candidate->metadata
            + types->file_offset);
        valid = nil->kind == N00B_ABI_TYPE_NIL
            && nil->typehash == UINT64_C(0x0ea862bddb8ce94c);
    }
    if (valid) valid = metadata_references_valid(candidate, error);
    if (valid) valid = metadata_storage_valid(candidate, error);
    if (valid) valid = metadata_dispatch_valid(candidate, error);
    if (valid) valid = metadata_name_slots_valid(candidate, error);
    if (valid) valid = specialization_graph_valid(candidate);
    if (!valid) {
        snapshot_free(candidate, sizeof(*candidate));
        if (error && error->status == N00B_ABI_BAD_METADATA)
            return N00B_ABI_BAD_METADATA;
        return registration_error(error, N00B_ABI_BAD_METADATA, 0, 0, 0);
    }
    while (atomic_flag_test_and_set_explicit(&registration_lock,
                                            memory_order_acquire)) {}
    if (atomic_load_explicit(&current_snapshot, memory_order_relaxed)) {
        atomic_flag_clear_explicit(&registration_lock, memory_order_release);
        snapshot_free(candidate, sizeof(*candidate));
        return registration_error(error, N00B_ABI_UNRESOLVED, 0, 0, 0);
    }
    candidate->generation = 1;
    atomic_store_explicit(&current_snapshot, candidate, memory_order_release);
    atomic_flag_clear_explicit(&registration_lock, memory_order_release);
    if (published_generation) *published_generation = 1;
    if (error) *error = (n00b_abi_error_v1){0};
    return N00B_ABI_OK;
}

const n00b_abi_snapshot_v1 *
n00b_abi_v1_current_snapshot(void)
{
    return atomic_load_explicit(&current_snapshot, memory_order_acquire);
}

uint64_t
n00b_abi_v1_generation(const n00b_abi_snapshot_v1 *snapshot)
{
    return snapshot ? snapshot->generation : 0;
}

uint32_t
n00b_abi_v1_table_count(const n00b_abi_snapshot_v1 *snapshot, uint32_t kind)
{
    const n00bb_abi_directory_entry_t *table = table_for(snapshot, kind);
    return table ? table->record_count : 0;
}

const void *
n00b_abi_v1_record(const n00b_abi_snapshot_v1 *snapshot, uint32_t kind,
                   uint32_t index, uint32_t *record_bytes)
{
    const n00bb_abi_directory_entry_t *table = table_for(snapshot, kind);
    if (!table || index >= table->record_count) {
        if (record_bytes) *record_bytes = 0;
        return nullptr;
    }
    if (record_bytes) *record_bytes = table->record_bytes;
    return snapshot->metadata + table->file_offset
        + (uint64_t)index * table->record_bytes;
}

static int32_t
range_data(const n00b_abi_snapshot_v1 *snapshot, uint32_t kind,
           n00b_abi_range_v1 range, const uint8_t **out)
{
    if (!snapshot || !out || (!range.count && range.first)) return -1;
    if (!range.count) {
        *out = nullptr;
        return 0;
    }
    const n00bb_abi_directory_entry_t *table = table_for(snapshot, kind);
    if (!table || range.first >= table->record_count
        || range.count > table->record_count - range.first) return -1;
    *out = snapshot->metadata + table->file_offset
        + (uint64_t)range.first * table->record_bytes;
    return 1;
}

int32_t
n00b_abi_v1_index_at(const n00b_abi_snapshot_v1 *snapshot,
                     n00b_abi_range_v1 range, uint32_t ordinal,
                     uint32_t *index_out)
{
    const uint8_t *data;
    if (!index_out) return -1;
    int32_t status = range_data(snapshot, N00B_ABI_TABLE_U32_STREAM,
                                range, &data);
    if (status <= 0) return status;
    if (ordinal >= range.count) return 0;
    *index_out = ((const uint32_t *)data)[ordinal];
    return 1;
}

int32_t
n00b_abi_v1_u64_at(const n00b_abi_snapshot_v1 *snapshot,
                   n00b_abi_range_v1 range, uint32_t ordinal,
                   uint64_t *value_out)
{
    const uint8_t *data;
    if (!value_out) return -1;
    int32_t status = range_data(snapshot, N00B_ABI_TABLE_U64_STREAM,
                                range, &data);
    if (status <= 0) return status;
    if (ordinal >= range.count) return 0;
    *value_out = ((const uint64_t *)data)[ordinal];
    return 1;
}

int32_t
n00b_abi_v1_bytes(const n00b_abi_snapshot_v1 *snapshot,
                  n00b_abi_range_v1 range, const uint8_t **data_out)
{
    return range_data(snapshot, N00B_ABI_TABLE_BYTE_STREAM, range, data_out);
}

int32_t
n00b_abi_v1_string_bytes(const n00b_abi_snapshot_v1 *snapshot,
                         const n00b_abi_string_v1 *value,
                         const uint8_t **data_out, uint64_t *byte_count_out)
{
    if (!snapshot || !value || !data_out || !byte_count_out) return -1;
    n00bb_abi_string_header_t header;
    if (!n00bb_abi_string_decode((const uint8_t *)value,
                                  sizeof(*value), &header)
        || header.tag == N00BB_ABI_STRING_RICH) return -1;
    const uint8_t *bytes = header.tag == N00BB_ABI_STRING_SHORT
        ? value->short_value.bytes : value->long_value.data;
    if (header.length && !bytes) return -1;
    *data_out = bytes;
    *byte_count_out = header.length;
    return 1;
}

#define TYPED_GETTER(name, kind, type) \
    const n00b_abi_##type##_v1 *n00b_abi_v1_##name( \
        const n00b_abi_snapshot_v1 *snapshot, uint32_t index) \
    { \
        return n00b_abi_v1_record(snapshot, N00B_ABI_TABLE_##kind, \
                                  index, nullptr); \
    }

TYPED_GETTER(type, TYPE, type)
TYPED_GETTER(package, PACKAGE, package)
TYPED_GETTER(module, MODULE, module)
TYPED_GETTER(runtime_config, RUNTIME_CONFIG, runtime_config)
TYPED_GETTER(layout, LAYOUT, layout)
TYPED_GETTER(field, FIELD, field)
TYPED_GETTER(function, FUNCTION, function)
TYPED_GETTER(call_signature, CALL_SIGNATURE, call_signature)
TYPED_GETTER(symbol, SYMBOL, symbol)
TYPED_GETTER(scope, SCOPE, scope)
TYPED_GETTER(source_location, SOURCE_LOCATION, source_location)
TYPED_GETTER(code_range, CODE_RANGE, code_range)
TYPED_GETTER(frame_layout, FRAME_LAYOUT, frame_layout)
TYPED_GETTER(trace, TRACE, trace)
TYPED_GETTER(storage, STORAGE, storage)

#undef TYPED_GETTER

int32_t
n00b_abi_v1_find_typehash(const n00b_abi_snapshot_v1 *snapshot,
                          uint64_t typehash, uint32_t *type_index)
{
    if (!snapshot || !type_index) return -1;
    uint32_t found = UINT32_MAX;
    uint32_t count = n00b_abi_v1_table_count(snapshot, N00B_ABI_TABLE_TYPE);
    for (uint32_t i = 0; i < count; i++) {
        const n00b_abi_type_v1 *type = n00b_abi_v1_type(snapshot, i);
        if (!type || (type->flags & N00B_ABI_TYPE_FLAG_NON_CONCRETE)
            || type->typehash != typehash) continue;
        if (found != UINT32_MAX) return -1;
        found = i;
    }
    if (found == UINT32_MAX) return 0;
    *type_index = found;
    return 1;
}

int32_t
n00b_abi_v1_find_module(const n00b_abi_snapshot_v1 *snapshot,
                        n00b_abi_string_v1 full_path,
                        uint32_t *module_index)
{
    if (!snapshot || !module_index) return -1;
    const uint8_t *wanted;
    uint64_t wanted_bytes;
    if (n00b_abi_v1_string_bytes(snapshot, &full_path, &wanted,
                                  &wanted_bytes) != 1) return -1;
    uint32_t found = 0;
    uint32_t count = n00b_abi_v1_table_count(snapshot, N00B_ABI_TABLE_MODULE);
    for (uint32_t i = 1; i < count; i++) {
        const n00b_abi_module_v1 *module = n00b_abi_v1_module(snapshot, i);
        const uint8_t *path;
        uint64_t path_bytes;
        if (!module || n00b_abi_v1_string_bytes(snapshot,
                &module->full_path, &path, &path_bytes) != 1) return -1;
        if (path_bytes != wanted_bytes) continue;
        if (!same_bytes(path, wanted, path_bytes)) continue;
        if (found) return -1;
        found = i;
    }
    if (!found) return 0;
    *module_index = found;
    return 1;
}

int32_t
n00b_abi_v1_find_symbol(const n00b_abi_snapshot_v1 *snapshot,
                        n00b_abi_string_v1 full_path,
                        uint32_t *symbol_index)
{
    if (!snapshot || !symbol_index) return -1;
    const uint8_t *wanted;
    uint64_t wanted_bytes;
    if (n00b_abi_v1_string_bytes(snapshot, &full_path, &wanted,
                                  &wanted_bytes) != 1) return -1;
    uint32_t count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_NAME_SLOT);
    if (count < 2) return 0;
    uint32_t capacity = count - 1;
    uint64_t hash = XXH3_64bits(wanted, (size_t)wanted_bytes);
    uint32_t slot_index = 1 + (uint32_t)(hash % capacity);
    for (uint32_t probes = 0; probes < capacity; probes++) {
        const n00b_abi_name_slot_v1 *slot = n00b_abi_v1_record(snapshot,
            N00B_ABI_TABLE_NAME_SLOT, slot_index, nullptr);
        if (!slot) return -1;
        if (!slot->symbol_index) return 0;
        if (slot->name_hash == hash) {
            symbol_path_t path;
            if (!symbol_path(snapshot, slot->symbol_index, &path)) return -1;
            if (path_matches(path, wanted, wanted_bytes)) {
                *symbol_index = slot->symbol_index;
                return 1;
            }
        }
        slot_index = slot_index == capacity ? 1 : slot_index + 1;
    }
    return -1;
}

int32_t
n00b_abi_v1_type_specializes(const n00b_abi_snapshot_v1 *snapshot,
                             uint32_t type_index,
                             uint32_t general_type_index)
{
    uint32_t count = n00b_abi_v1_table_count(snapshot, N00B_ABI_TABLE_TYPE);
    if (!snapshot || type_index >= count || general_type_index >= count)
        return -1;
    if (type_index == general_type_index) return 0;
    size_t scratch_bytes = (size_t)count * sizeof(uint32_t) + count;
    uint8_t *scratch = snapshot_alloc(scratch_bytes);
    if (!scratch) return -1;
    uint32_t *stack = (void *)scratch;
    uint8_t *seen = scratch + (size_t)count * sizeof(uint32_t);
    size_t depth = 0;
    stack[depth++] = type_index;
    seen[type_index] = 1;
    int32_t result = 0;
    while (depth && !result) {
        uint32_t child = stack[--depth];
        const n00b_abi_type_v1 *type = n00b_abi_v1_type(snapshot, child);
        for (uint32_t i = type->specialization_start_ix;
             i < type->specialization_end_ix; i++) {
            const uint32_t *parent = n00b_abi_v1_record(snapshot,
                N00B_ABI_TABLE_SPECIALIZATION_LINK, i, nullptr);
            if (*parent == general_type_index) { result = 1; break; }
            if (!seen[*parent]) {
                seen[*parent] = 1;
                stack[depth++] = *parent;
            }
        }
    }
    snapshot_free(scratch, scratch_bytes);
    return result;
}

int32_t
n00b_abi_v1_resolve_trait_method(const n00b_abi_snapshot_v1 *snapshot,
                                 uint32_t trait_type_index,
                                 uint32_t method_ordinal,
                                 uint32_t receiver_type_index,
                                 uint32_t *function_index)
{
    if (!snapshot || !function_index) return -1;
    const n00b_abi_type_v1 *trait = n00b_abi_v1_type(snapshot,
        trait_type_index);
    const n00b_abi_type_v1 *receiver = n00b_abi_v1_type(snapshot,
        receiver_type_index);
    if (!trait || trait->kind != N00B_ABI_TYPE_TRAIT || !receiver
        || (receiver->flags & N00B_ABI_TYPE_FLAG_NON_CONCRETE)
        || method_ordinal >= trait->fields_or_arms.count) return -1;
    uint32_t method_index;
    if (n00b_abi_v1_index_at(snapshot, trait->fields_or_arms,
            method_ordinal, &method_index) != 1) return -1;
    const n00b_abi_trait_method_v1 *method = n00b_abi_v1_record(snapshot,
        N00B_ABI_TABLE_TRAIT_METHOD, method_index, nullptr);
    if (!method || method->trait_type_index != trait_type_index
        || method->declaration_ordinal != method_ordinal) return -1;
    const n00b_abi_dispatch_v1 *dispatch = nullptr;
    uint32_t dispatch_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_DISPATCH);
    for (uint32_t i = 1; i < dispatch_count; i++) {
        const n00b_abi_dispatch_v1 *candidate = n00b_abi_v1_record(snapshot,
            N00B_ABI_TABLE_DISPATCH, i, nullptr);
        if (candidate->trait_type_index != trait_type_index
            || candidate->method_ordinal != method_ordinal) continue;
        if (dispatch) return -1;
        dispatch = candidate;
    }
    if (!dispatch) return 0;
    uint32_t selected_type = UINT32_MAX, selected_function = 0;
    for (uint32_t i = 0; i < dispatch->entries.count; i++) {
        uint32_t entry_index;
        if (n00b_abi_v1_index_at(snapshot, dispatch->entries, i,
                                 &entry_index) != 1) return -1;
        const n00b_abi_dispatch_entry_v1 *entry = n00b_abi_v1_record(snapshot,
            N00B_ABI_TABLE_DISPATCH_ENTRY, entry_index, nullptr);
        if (!entry || !entry->function_index
            || !n00b_abi_v1_function(snapshot, entry->function_index)
            || !n00b_abi_v1_type(snapshot, entry->receiver_type_index))
            return -1;
        int32_t matches = receiver_type_index == entry->receiver_type_index
            ? 1 : n00b_abi_v1_type_specializes(snapshot, receiver_type_index,
                                                entry->receiver_type_index);
        if (matches < 0) return -1;
        if (!matches) continue;
        if (selected_type == UINT32_MAX) {
            selected_type = entry->receiver_type_index;
            selected_function = entry->function_index;
            continue;
        }
        if (selected_type == entry->receiver_type_index) return -1;
        int32_t more_specific = n00b_abi_v1_type_specializes(snapshot,
            entry->receiver_type_index, selected_type);
        if (more_specific < 0) return -1;
        if (more_specific) {
            selected_type = entry->receiver_type_index;
            selected_function = entry->function_index;
            continue;
        }
        int32_t less_specific = n00b_abi_v1_type_specializes(snapshot,
            selected_type, entry->receiver_type_index);
        if (less_specific <= 0) return -1;
    }
    if (selected_type == UINT32_MAX) return 0;
    *function_index = selected_function;
    return 1;
}
