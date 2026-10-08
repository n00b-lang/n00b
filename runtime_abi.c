#include "n00b-abi-v1.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "abi_directory.h"
#include "abi_string.h"

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
    if (valid) valid = specialization_graph_valid(candidate);
    if (!valid) {
        snapshot_free(candidate, sizeof(*candidate));
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
