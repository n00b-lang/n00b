#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/buffer.h"
#include "lib/string.h"

bool n00bb_global_metadata_append_storage(
    core_buffer_t *payload, uint8_t metadata[12],
    core_string_t requested_storage_class, bool required);
bool n00bb_global_metadata_read_storage(
    const uint8_t *data, size_t size,
    core_string_t *requested_storage_class, bool *required,
    size_t *payload_offset);
