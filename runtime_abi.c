#include "n00b-abi-v1.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "abi_directory.h"
#include "abi_string.h"

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
