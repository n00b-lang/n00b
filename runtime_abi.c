#include "n00b-abi-v1.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "abi_directory.h"
#include "abi_member_facts.h"
#include "abi_ref_schema.h"
#include "abi_string.h"
#include "abi_type_variable.h"

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
        if (!n00bb_abi_type_variable_final_valid(type))
            return invalid_record(error, N00B_ABI_TABLE_TYPE, i, 0);
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
            if (!member)
                return invalid_record(error, N00B_ABI_TABLE_TYPE, i, 0);
            n00bb_abi_member_facts_t facts = n00bb_abi_member_facts(
                member_kind, member);
            if (facts.parent != i)
                return invalid_record(error, N00B_ABI_TABLE_TYPE, i, 0);
            if (facts.symbol) {
                const n00b_abi_symbol_v1 *symbol = n00b_abi_v1_symbol(
                    snapshot, facts.symbol);
                if (!symbol || symbol->kind != facts.symbol_kind
                    || symbol->type_index != facts.type)
                    return invalid_record(error, N00B_ABI_TABLE_TYPE,
                                          i, 0);
            }
            for (uint32_t k = 0; k < j; k++) {
                uint32_t previous_index;
                if (n00b_abi_v1_index_at(snapshot, type->fields_or_arms,
                        k, &previous_index) != 1)
                    return invalid_record(error, N00B_ABI_TABLE_TYPE,
                                          i, 0);
                const void *previous = n00b_abi_v1_record(snapshot,
                    member_kind, previous_index, nullptr);
                if (!previous || n00bb_abi_member_facts(member_kind,
                        previous).ordinal == facts.ordinal)
                    return invalid_record(error, N00B_ABI_TABLE_TYPE,
                                          i, 0);
            }
        }
    }
    return true;
}

static bool
physical_run_bounds(const n00b_abi_snapshot_v1 *snapshot,
                    n00b_abi_range_v1 range, uint32_t ordinal,
                    uint64_t *start, uint64_t *end)
{
    uint32_t index = 0;
    if (n00b_abi_v1_index_at(snapshot, range, ordinal, &index) != 1
        || !index) return false;
    const n00b_abi_physical_run_v1 *run = n00b_abi_v1_record(snapshot,
        N00B_ABI_TABLE_PHYSICAL_RUN, index, nullptr);
    if (!run || run->bit_offset >= 8 || !run->width_bits
        || run->byte_offset > (UINT64_MAX - run->bit_offset) / 8)
        return false;
    *start = run->byte_offset * 8 + run->bit_offset;
    if (run->width_bits > UINT64_MAX - *start) return false;
    *end = *start + run->width_bits;
    return true;
}

static bool
physical_runs_cover(const n00b_abi_snapshot_v1 *snapshot,
                    n00b_abi_range_v1 range, uint64_t start, uint64_t end,
                    uint64_t parent_bits)
{
    uint64_t cursor = start;
    for (uint32_t i = 0; i < range.count && cursor < end; i++) {
        uint64_t run_start, run_end;
        if (!physical_run_bounds(snapshot, range, i, &run_start, &run_end)
            || run_end > parent_bits) return false;
        if (run_end <= cursor) continue;
        if (run_start > cursor) return false;
        cursor = run_end;
    }
    return cursor >= end;
}

static bool
type_needs_trace(const n00b_abi_type_v1 *type)
{
    if (type->flags & N00B_ABI_TYPE_FLAG_NON_CONCRETE) return false;
    switch (type->kind) {
    case N00B_ABI_TYPE_REFERENCE:
    case N00B_ABI_TYPE_ARRAY:
    case N00B_ABI_TYPE_VIEW:
    case N00B_ABI_TYPE_STRING:
    case N00B_ABI_TYPE_BOX:
    case N00B_ABI_TYPE_CALLBACK:
    case N00B_ABI_TYPE_TRAIT:
    case N00B_ABI_TYPE_UNION:
        return true;
    default:
        return type->tracing_index != 0;
    }
}

static const n00b_abi_field_v1 *
trace_member_at(const n00b_abi_snapshot_v1 *snapshot,
                const n00b_abi_type_v1 *parent, uint32_t ordinal)
{
    uint32_t index = 0;
    if (n00b_abi_v1_index_at(snapshot, parent->fields_or_arms,
            ordinal, &index) != 1 || !index) return nullptr;
    if (parent->kind == N00B_ABI_TYPE_VARIANT) {
        const n00b_abi_arm_v1 *arm = n00b_abi_v1_record(snapshot,
            N00B_ABI_TABLE_ARM, index, nullptr);
        return arm ? &arm->field : nullptr;
    }
    return n00b_abi_v1_record(snapshot, N00B_ABI_TABLE_FIELD,
                              index, nullptr);
}

static bool
trace_placement_valid(const n00b_abi_snapshot_v1 *snapshot,
                      const n00b_abi_type_v1 *parent,
                      n00b_abi_range_v1 parent_occupied,
                      const n00b_abi_type_v1 *type,
                      uint64_t byte_offset)
{
    if (!type || (type->flags & N00B_ABI_TYPE_FLAG_NON_CONCRETE)
        || !type->size_bits || !type->alignment_bytes
        || byte_offset > UINT64_MAX / 8
        || byte_offset % type->alignment_bytes
        || byte_offset % 8) return false;
    uint64_t start = byte_offset * 8;
    if (start > parent->size_bits
        || type->size_bits > parent->size_bits - start)
        return false;
    const n00b_abi_layout_v1 *layout = type->layout_index
        ? n00b_abi_v1_layout(snapshot, type->layout_index) : nullptr;
    if (!type_needs_trace(type) || !layout
        || !layout->occupied_runs.count)
        return false;
    for (uint32_t i = 0; i < layout->occupied_runs.count; i++) {
        uint64_t child_start, child_end;
        if (!physical_run_bounds(snapshot, layout->occupied_runs, i,
                &child_start, &child_end)
            || child_end > type->size_bits
            || child_end > UINT64_MAX - start
            || !physical_runs_cover(snapshot, parent_occupied,
                start + child_start, start + child_end,
                parent->size_bits))
            return false;
    }
    return true;
}

static bool
trace_child_valid(const n00b_abi_snapshot_v1 *snapshot,
                  const n00b_abi_type_v1 *parent,
                  n00b_abi_range_v1 parent_occupied,
                  uint32_t child_index, n00b_abi_error_v1 *error)
{
    const n00b_abi_trace_child_v1 *child = n00b_abi_v1_record(snapshot,
        N00B_ABI_TABLE_TRACE_CHILD, child_index, nullptr);
    const n00b_abi_type_v1 *type = child
        ? n00b_abi_v1_type(snapshot, child->child_type_index) : nullptr;
    if (!child || child->reserved_zero
        || !trace_placement_valid(snapshot, parent, parent_occupied,
            type, child->byte_offset))
        return invalid_record(error, N00B_ABI_TABLE_TRACE_CHILD,
                              child_index, 0);
    return true;
}

static bool
trace_fields_complete(const n00b_abi_snapshot_v1 *snapshot,
                      const n00b_abi_type_v1 *parent,
                      n00b_abi_range_v1 children,
                      n00b_abi_error_v1 *error)
{
    uint32_t member_count = parent->fields_or_arms.count;
    if (!member_count) return children.count == 0;
    uint8_t *seen = snapshot_alloc(member_count);
    if (!seen) return false;
    bool valid = true;
    for (uint32_t i = 0; valid && i < children.count; i++) {
        uint32_t child_index = 0;
        valid = n00b_abi_v1_index_at(snapshot, children, i,
            &child_index) == 1 && child_index;
        const n00b_abi_trace_child_v1 *child = valid
            ? n00b_abi_v1_record(snapshot, N00B_ABI_TABLE_TRACE_CHILD,
                child_index, nullptr) : nullptr;
        valid = child != nullptr;
        uint32_t match = member_count;
        for (uint32_t j = 0; valid && j < member_count; j++) {
            const n00b_abi_field_v1 *field = trace_member_at(snapshot,
                parent, j);
            valid = field != nullptr;
            if (valid && field->child_type_index == child->child_type_index
                && field->logical_bit_offset / 8 == child->byte_offset
                && field->logical_bit_offset % 8 == 0) {
                if (match != member_count) valid = false;
                else match = j;
            }
        }
        if (valid) valid = match != member_count && !seen[match];
        if (valid) seen[match] = 1;
        else invalid_record(error, N00B_ABI_TABLE_TRACE_CHILD,
                            child_index, 0);
    }
    for (uint32_t j = 0; valid && j < member_count; j++) {
        const n00b_abi_field_v1 *field = trace_member_at(snapshot,
            parent, j);
        const n00b_abi_type_v1 *child = field
            ? n00b_abi_v1_type(snapshot, field->child_type_index) : nullptr;
        valid = child && seen[j] == type_needs_trace(child);
    }
    snapshot_free(seen, member_count);
    return valid;
}

static bool
trace_children_valid(const n00b_abi_snapshot_v1 *snapshot,
                     const n00b_abi_type_v1 *parent,
                     n00b_abi_range_v1 parent_occupied,
                     n00b_abi_range_v1 children,
                     n00b_abi_error_v1 *error)
{
    for (uint32_t i = 0; i < children.count; i++) {
        uint32_t child_index = 0;
        if (n00b_abi_v1_index_at(snapshot, children, i, &child_index) != 1
            || !child_index
            || !trace_child_valid(snapshot, parent, parent_occupied,
                child_index, error)) return false;
    }
    return true;
}

static bool
variant_tag_valid(const n00b_abi_type_v1 *type,
                  const n00b_abi_tag_extractor_v1 *tag,
                  uint32_t arm_count, uint64_t *tag_start,
                  uint64_t *tag_end)
{
    if (!tag || tag->kind != N00B_ABI_TAG_DIRECT_BITS
        || (tag->unit_width_bits != 8 && tag->unit_width_bits != 16
            && tag->unit_width_bits != 32 && tag->unit_width_bits != 64)
        || !tag->tag_width_bits || tag->tag_width_bits > 32
        || tag->lsb_position >= tag->unit_width_bits
        || tag->tag_width_bits
            > tag->unit_width_bits - tag->lsb_position
        || tag->unit_byte_offset > UINT64_MAX / 8)
        return false;
    uint64_t mask = (UINT64_C(1) << tag->tag_width_bits) - 1;
    if (tag->mask != mask || arm_count > mask) return false;
    if (type->size_bits > UINT64_MAX - 7) return false;
    uint64_t byte_extent = (type->size_bits + 7) / 8;
    if (tag->unit_byte_offset > byte_extent
        || tag->unit_width_bits / 8
            > byte_extent - tag->unit_byte_offset) return false;
    uint64_t unit_start = tag->unit_byte_offset * 8;
    if (unit_start > type->size_bits
        || tag->lsb_position > type->size_bits - unit_start
        || tag->tag_width_bits > type->size_bits - unit_start
            - tag->lsb_position) return false;
    *tag_start = unit_start + tag->lsb_position;
    *tag_end = *tag_start + tag->tag_width_bits;
    return true;
}

static bool
variant_trace_arms_valid(const n00b_abi_snapshot_v1 *snapshot,
                         const n00b_abi_type_v1 *type,
                         const n00b_abi_trace_v1 *trace,
                         uint64_t tag_start, uint64_t tag_end,
                         n00b_abi_error_v1 *error)
{
    uint32_t count = type->fields_or_arms.count;
    if (!count || trace->arms.count != count) return false;
    uint8_t *seen = snapshot_alloc(count);
    if (!seen) return false;
    bool valid = true;
    for (uint32_t i = 0; valid && i < count; i++) {
        uint32_t trace_arm_index = 0;
        valid = n00b_abi_v1_index_at(snapshot, trace->arms, i,
            &trace_arm_index) == 1 && trace_arm_index;
        const n00b_abi_trace_arm_v1 *arm_trace = valid
            ? n00b_abi_v1_record(snapshot, N00B_ABI_TABLE_TRACE_ARM,
                trace_arm_index, nullptr) : nullptr;
        valid = arm_trace && arm_trace->tag_value
            && arm_trace->tag_value <= count
            && !seen[arm_trace->tag_value - 1];
        uint32_t arm_index = 0;
        if (valid) valid = n00b_abi_v1_index_at(snapshot,
            type->fields_or_arms, arm_trace->tag_value - 1,
            &arm_index) == 1 && arm_index;
        const n00b_abi_arm_v1 *arm = valid
            ? n00b_abi_v1_record(snapshot, N00B_ABI_TABLE_ARM,
                arm_index, nullptr) : nullptr;
        valid = arm && arm->tag_value == arm_trace->tag_value
            && arm->field.declaration_ordinal == arm_trace->tag_value - 1;
        for (uint32_t j = 0; valid
             && j < arm->field.physical_runs.count; j++) {
            uint64_t start, end;
            valid = physical_run_bounds(snapshot,
                arm->field.physical_runs, j, &start, &end)
                && end <= type->size_bits
                && (end <= tag_start || start >= tag_end);
        }
        const n00b_abi_type_v1 *child_type = valid
            ? n00b_abi_v1_type(snapshot, arm->field.child_type_index)
            : nullptr;
        valid = child_type != nullptr;
        bool needs_trace = valid && type_needs_trace(child_type);
        if (valid && arm_trace->child_trace_index)
            valid = needs_trace && !arm_trace->children.count
                && arm_trace->child_trace_index
                    == child_type->tracing_index
                && arm->field.logical_bit_offset % 8 == 0
                && trace_placement_valid(snapshot, type,
                    arm->field.physical_runs, child_type,
                    arm->field.logical_bit_offset / 8);
        else if (valid) valid = arm_trace->children.count
            == (uint32_t)needs_trace;
        if (valid) valid = trace_children_valid(snapshot, type,
            arm->field.physical_runs, arm_trace->children, error);
        if (valid && arm_trace->children.count) {
            uint32_t child_index = 0;
            valid = n00b_abi_v1_index_at(snapshot, arm_trace->children,
                0, &child_index) == 1 && child_index;
            const n00b_abi_trace_child_v1 *child = valid
                ? n00b_abi_v1_record(snapshot, N00B_ABI_TABLE_TRACE_CHILD,
                    child_index, nullptr) : nullptr;
            valid = child && arm->field.logical_bit_offset % 8 == 0
                && child->byte_offset == arm->field.logical_bit_offset / 8
                && child->child_type_index
                    == arm->field.child_type_index;
        }
        if (valid) seen[arm_trace->tag_value - 1] = 1;
        else if (!error || error->table_kind != N00B_ABI_TABLE_TRACE_CHILD)
            invalid_record(error, N00B_ABI_TABLE_TRACE_ARM,
                           trace_arm_index, 0);
    }
    snapshot_free(seen, count);
    return valid;
}

static bool
metadata_traces_valid(const n00b_abi_snapshot_v1 *snapshot,
                      n00b_abi_error_v1 *error)
{
    uint32_t type_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_TYPE);
    for (uint32_t i = 1; i < type_count; i++) {
        const n00b_abi_type_v1 *type = n00b_abi_v1_type(snapshot, i);
        if (type->tracing_index || (type->kind != N00B_ABI_TYPE_RECORD
            && type->kind != N00B_ABI_TYPE_TUPLE
            && type->kind != N00B_ABI_TYPE_VARIANT)) continue;
        for (uint32_t j = 0; j < type->fields_or_arms.count; j++) {
            const n00b_abi_field_v1 *field = trace_member_at(snapshot,
                type, j);
            const n00b_abi_type_v1 *child = field
                ? n00b_abi_v1_type(snapshot, field->child_type_index)
                : nullptr;
            if (!child || type_needs_trace(child))
                return invalid_record(error, N00B_ABI_TABLE_TYPE, i, 0);
        }
    }
    uint32_t trace_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_TRACE);
    if (trace_count <= 1) return true;
    uint8_t *seen = snapshot_alloc(trace_count);
    if (!seen) return false;
    bool valid = true;
    for (uint32_t i = 0; valid && i < type_count; i++) {
        const n00b_abi_type_v1 *type = n00b_abi_v1_type(snapshot, i);
        if (!type->tracing_index) continue;
        uint32_t trace_index = type->tracing_index;
        const n00b_abi_trace_v1 *trace = n00b_abi_v1_trace(snapshot,
            trace_index);
        const n00b_abi_layout_v1 *layout = type->layout_index
            ? n00b_abi_v1_layout(snapshot, type->layout_index) : nullptr;
        valid = trace && trace->type_index == i && !seen[trace_index]
            && !trace->reserved_zero && layout;
        if (valid && (type->kind == N00B_ABI_TYPE_RECORD
                      || type->kind == N00B_ABI_TYPE_TUPLE))
            valid = !trace->child_trace_index
                && !trace->tag_extractor_index && !trace->arms.count
                && !trace->option_bit_offset
                && trace_children_valid(snapshot, type,
                    layout->occupied_runs, trace->fixed_children, error)
                && trace_fields_complete(snapshot, type,
                    trace->fixed_children, error);
        else if (valid && type->kind == N00B_ABI_TYPE_VARIANT) {
            const n00b_abi_tag_extractor_v1 *tag =
                n00b_abi_v1_record(snapshot, N00B_ABI_TABLE_TAG_EXTRACTOR,
                    trace->tag_extractor_index, nullptr);
            uint64_t tag_start = 0, tag_end = 0;
            valid = !trace->fixed_children.count
                && !trace->child_trace_index && !trace->option_bit_offset
                && trace->tag_extractor_index == layout->tag_extractor_index
                && variant_tag_valid(type, tag, type->fields_or_arms.count,
                    &tag_start, &tag_end)
                && variant_trace_arms_valid(snapshot, type, trace,
                    tag_start, tag_end, error);
        }
        if (valid) seen[trace_index] = 1;
        else if (!error || (error->table_kind != N00B_ABI_TABLE_TRACE_CHILD
                           && error->table_kind != N00B_ABI_TABLE_TRACE_ARM))
            invalid_record(error, N00B_ABI_TABLE_TRACE, trace_index, 0);
    }
    for (uint32_t i = 1; valid && i < trace_count; i++)
        if (!seen[i]) valid = invalid_record(error, N00B_ABI_TABLE_TRACE,
                                            i, 0);
    snapshot_free(seen, trace_count);
    return valid;
}

static bool
metadata_code_ranges_valid(const n00b_abi_snapshot_v1 *snapshot,
                           n00b_abi_error_v1 *error)
{
    uint32_t code_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_CODE_RANGE);
    if (code_count <= 1) return true;
    uint8_t *seen = snapshot_alloc(code_count);
    if (!seen) return false;
    bool valid = true;
    uint32_t function_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_FUNCTION);
    for (uint32_t function_index = 1; valid
         && function_index < function_count; function_index++) {
        const n00b_abi_function_v1 *function = n00b_abi_v1_function(snapshot,
            function_index);
        const n00b_abi_symbol_v1 *symbol = function->symbol_index
            ? n00b_abi_v1_symbol(snapshot, function->symbol_index) : nullptr;
        for (uint32_t i = 0; valid && i < function->code_ranges.count; i++) {
            uint32_t code_index = 0;
            valid = n00b_abi_v1_index_at(snapshot, function->code_ranges,
                i, &code_index) == 1 && code_index
                && code_index < code_count
                && !seen[code_index];
            const n00b_abi_code_range_v1 *code = valid
                ? n00b_abi_v1_code_range(snapshot, code_index) : nullptr;
            valid = valid && code && code->function_index == function_index
                && code->start && code->byte_length
                && code->byte_length <= UINTPTR_MAX
                && (uintptr_t)code->start <= UINTPTR_MAX - code->byte_length
                && code->phase_mask
                && !(code->phase_mask
                    & ~(N00B_ABI_PHASE_COMPTIME | N00B_ABI_PHASE_RUNTIME))
                && (!function->symbol_index
                    || (symbol && symbol->kind == N00B_ABI_SYMBOL_FUNCTION
                        && (code->phase_mask & symbol->phase_mask)
                            == code->phase_mask));
            if (valid) seen[code_index] = 1;
            else invalid_record(error, N00B_ABI_TABLE_CODE_RANGE,
                                code_index, 0);
        }
    }
    for (uint32_t i = 1; valid && i < code_count; i++)
        if (!seen[i]) valid = invalid_record(error,
            N00B_ABI_TABLE_CODE_RANGE, i, 0);
    snapshot_free(seen, code_count);
    return valid;
}

static bool
metadata_frame_layouts_valid(const n00b_abi_snapshot_v1 *snapshot,
                             n00b_abi_error_v1 *error)
{
    uint32_t frame_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_FRAME_LAYOUT);
    if (frame_count <= 1) return true;
    uint8_t *seen = snapshot_alloc(frame_count);
    if (!seen) return false;
    bool valid = true;
    uint32_t function_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_FUNCTION);
    for (uint32_t function_index = 1; valid
         && function_index < function_count; function_index++) {
        const n00b_abi_function_v1 *function = n00b_abi_v1_function(snapshot,
            function_index);
        for (uint32_t i = 0; valid && i < function->frame_layouts.count; i++) {
            uint32_t frame_index = 0;
            valid = n00b_abi_v1_index_at(snapshot, function->frame_layouts,
                i, &frame_index) == 1 && frame_index
                && frame_index < frame_count && !seen[frame_index];
            const n00b_abi_frame_layout_v1 *frame = valid
                ? n00b_abi_v1_frame_layout(snapshot, frame_index) : nullptr;
            valid = valid && frame && frame->function_index == function_index
                && frame->frame_byte_extent >= 8
                && !(frame->frame_byte_extent % 8);
            if (valid) {
                uint64_t slots = (frame->frame_byte_extent - 8) / 8;
                uint64_t bitmap_bytes = (slots + 7) / 8;
                const uint8_t *bitmap = nullptr;
                valid = bitmap_bytes == frame->pointer_bitmap_bytes.count
                    && n00b_abi_v1_bytes(snapshot, frame->pointer_bitmap_bytes,
                        &bitmap) >= 0;
                if (valid && slots % 8) {
                    uint8_t trailing = (uint8_t)~((1u << (slots % 8)) - 1);
                    valid = !(bitmap[bitmap_bytes - 1] & trailing);
                }
            }
            if (valid) seen[frame_index] = 1;
            else invalid_record(error, N00B_ABI_TABLE_FRAME_LAYOUT,
                                frame_index, 0);
        }
    }
    for (uint32_t i = 1; valid && i < frame_count; i++)
        if (!seen[i]) valid = invalid_record(error,
            N00B_ABI_TABLE_FRAME_LAYOUT, i, 0);
    snapshot_free(seen, frame_count);
    return valid;
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

static bool metadata_dispatch_ambiguity_valid(
    const n00b_abi_snapshot_v1 *snapshot, n00b_abi_error_v1 *error);

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
    if (valid) valid = metadata_traces_valid(candidate, error);
    if (valid) valid = metadata_code_ranges_valid(candidate, error);
    if (valid) valid = metadata_frame_layouts_valid(candidate, error);
    if (valid) valid = metadata_storage_valid(candidate, error);
    if (valid) valid = metadata_dispatch_valid(candidate, error);
    if (valid) valid = metadata_name_slots_valid(candidate, error);
    if (valid) valid = specialization_graph_valid(candidate);
    if (valid) valid = metadata_dispatch_ambiguity_valid(candidate, error);
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

static const n00b_abi_dispatch_entry_v1 *
dispatch_entry_at(const n00b_abi_snapshot_v1 *snapshot,
                  const n00b_abi_dispatch_v1 *dispatch, uint32_t ordinal)
{
    uint32_t index;
    if (n00b_abi_v1_index_at(snapshot, dispatch->entries,
            ordinal, &index) != 1) return nullptr;
    const n00b_abi_dispatch_entry_v1 *entry = n00b_abi_v1_record(snapshot,
        N00B_ABI_TABLE_DISPATCH_ENTRY, index, nullptr);
    return entry && entry->function_index
        && n00b_abi_v1_function(snapshot, entry->function_index)
        && n00b_abi_v1_type(snapshot, entry->receiver_type_index)
        ? entry : nullptr;
}

static int32_t
select_dispatch_entry(const n00b_abi_snapshot_v1 *snapshot,
                      const n00b_abi_dispatch_v1 *dispatch,
                      uint32_t receiver_type_index, uint32_t *function_index)
{
    uint32_t selected_type = UINT32_MAX;
    uint32_t selected_function = 0;
    uint32_t selected_ordinal = UINT32_MAX;
    for (uint32_t i = 0; i < dispatch->entries.count; i++) {
        const n00b_abi_dispatch_entry_v1 *entry = dispatch_entry_at(snapshot,
            dispatch, i);
        if (!entry) return -1;
        int32_t matches = receiver_type_index == entry->receiver_type_index
            ? 1 : n00b_abi_v1_type_specializes(snapshot, receiver_type_index,
                                                entry->receiver_type_index);
        if (matches < 0) return -1;
        if (!matches) continue;
        if (selected_type != UINT32_MAX) {
            int32_t more_specific = n00b_abi_v1_type_specializes(snapshot,
                entry->receiver_type_index, selected_type);
            if (more_specific < 0) return -1;
            if (!more_specific) continue;
        }
        selected_type = entry->receiver_type_index;
        selected_function = entry->function_index;
        selected_ordinal = i;
    }
    if (selected_type == UINT32_MAX) return 0;
    for (uint32_t i = 0; i < dispatch->entries.count; i++) {
        if (i == selected_ordinal) continue;
        const n00b_abi_dispatch_entry_v1 *entry = dispatch_entry_at(snapshot,
            dispatch, i);
        if (!entry) return -1;
        int32_t matches = receiver_type_index == entry->receiver_type_index
            ? 1 : n00b_abi_v1_type_specializes(snapshot, receiver_type_index,
                                                entry->receiver_type_index);
        if (matches < 0) return -1;
        if (!matches) continue;
        int32_t dominates = n00b_abi_v1_type_specializes(snapshot,
            selected_type, entry->receiver_type_index);
        if (dominates != 1) return -1;
    }
    *function_index = selected_function;
    return 1;
}

static bool
metadata_dispatch_ambiguity_valid(const n00b_abi_snapshot_v1 *snapshot,
                                  n00b_abi_error_v1 *error)
{
    uint32_t dispatch_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_DISPATCH);
    uint32_t type_count = n00b_abi_v1_table_count(snapshot,
        N00B_ABI_TABLE_TYPE);
    for (uint32_t i = 1; i < dispatch_count; i++) {
        const n00b_abi_dispatch_v1 *dispatch = n00b_abi_v1_record(snapshot,
            N00B_ABI_TABLE_DISPATCH, i, nullptr);
        if (dispatch->entries.count < 2) continue;
        for (uint32_t type_index = 0; type_index < type_count; type_index++) {
            const n00b_abi_type_v1 *type = n00b_abi_v1_type(snapshot,
                                                             type_index);
            if (type->flags & N00B_ABI_TYPE_FLAG_NON_CONCRETE) continue;
            uint32_t function_index;
            if (select_dispatch_entry(snapshot, dispatch, type_index,
                    &function_index) < 0)
                return invalid_record(error, N00B_ABI_TABLE_DISPATCH,
                                      i, 0);
        }
    }
    return true;
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
    return dispatch ? select_dispatch_entry(snapshot, dispatch,
        receiver_type_index, function_index) : 0;
}
