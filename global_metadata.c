#include "global_metadata.h"

bool
n00bb_global_metadata_append_storage(core_buffer_t *payload,
                                    uint8_t metadata[12],
                                    core_string_t requested_storage_class,
                                    bool required)
{
    if (!payload || !metadata
        || requested_storage_class.u8_bytes > UINT16_MAX
        || (requested_storage_class.u8_bytes
            && !requested_storage_class.data)) return false;

    uint16_t size = (uint16_t)requested_storage_class.u8_bytes;
    metadata[6] = required ? 1 : 0;
    metadata[7] = 0;
    metadata[8] = (uint8_t)(size & 0xff);
    metadata[9] = (uint8_t)(size >> 8);
    core_buffer_append(payload, (const char *)metadata, 12);
    if (size)
        core_buffer_append(payload, requested_storage_class.data, size);
    return true;
}

bool
n00bb_global_metadata_read_storage(const uint8_t *data, size_t size,
                                   core_string_t *requested_storage_class,
                                   bool *required, size_t *payload_offset)
{
    if (!data || size < 12 || data[6] > 1 || data[7] != 0)
        return false;
    uint16_t name_size = (uint16_t)data[8] | (uint16_t)data[9] << 8;
    size_t offset = 12u + name_size;
    if (offset > size) return false;

    if (requested_storage_class)
        *requested_storage_class = name_size
            ? core_string_borrow((const char *)data + 12, name_size)
            : (core_string_t){0};
    if (required) *required = data[6] != 0;
    if (payload_offset) *payload_offset = offset;
    return true;
}
