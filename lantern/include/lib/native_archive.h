#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *name;
    size_t name_length;
    const uint8_t *bytes;
    size_t size;
    size_t archive_offset;
} core_native_archive_member_t;

typedef bool (*core_native_archive_visit_fn)(
    const core_native_archive_member_t *member, void *user);

/* Visits the members of a regular native ar archive. Views borrow the archive
 * bytes; thin archives and malformed inputs are rejected. The visitor also
 * receives archive index members, which a caller may ignore. */
bool core_native_archive_visit(const void *archive, size_t archive_bytes,
                               core_native_archive_visit_fn visitor,
                               void *user);
