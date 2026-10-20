#include "lib/native_archive.h"

#include <string.h>

enum { AR_MAGIC_BYTES = 8, AR_HEADER_BYTES = 60 };

static bool
decimal(const uint8_t *bytes, size_t width, size_t *out)
{
    size_t value = 0;
    size_t i = 0;
    while (i < width && bytes[i] == ' ') i++;
    size_t first = i;
    for (; i < width && bytes[i] >= '0' && bytes[i] <= '9'; i++) {
        size_t digit = (size_t)(bytes[i] - '0');
        if (value > (SIZE_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    if (i == first) return false;
    for (; i < width; i++) if (bytes[i] != ' ') return false;
    *out = value;
    return true;
}

static size_t
short_name_length(const uint8_t *name)
{
    size_t length = 16;
    while (length && name[length - 1] == ' ') length--;
    return length;
}

static bool
long_name(const uint8_t *table, size_t table_size, size_t offset,
          const uint8_t **out_name, size_t *out_length)
{
    if (!table || offset >= table_size) return false;
    size_t end = offset;
    while (end < table_size && table[end] != '\n' && table[end] != '\0') end++;
    if (end == table_size) return false;
    size_t length = end - offset;
    if (length && table[offset + length - 1] == '/') length--;
    if (!length) return false;
    *out_name = table + offset;
    *out_length = length;
    return true;
}

bool
core_native_archive_visit(const void *archive, size_t archive_bytes,
                          core_native_archive_visit_fn visitor, void *user)
{
    if (!archive || !visitor || archive_bytes < AR_MAGIC_BYTES
        || memcmp(archive, "!<arch>\n", AR_MAGIC_BYTES)) return false;
    const uint8_t *data = archive;
    const uint8_t *name_table = nullptr;
    size_t name_table_size = 0;
    size_t offset = AR_MAGIC_BYTES;
    while (offset < archive_bytes) {
        if (archive_bytes - offset < AR_HEADER_BYTES) return false;
        const uint8_t *header = data + offset;
        if (header[58] != '`' || header[59] != '\n') return false;
        size_t size = 0;
        if (!decimal(header + 48, 10, &size)
            || size > archive_bytes - offset - AR_HEADER_BYTES) return false;
        size_t name_length = short_name_length(header);
        if (!name_length) return false;
        const uint8_t *name = header;
        const uint8_t *body = header + AR_HEADER_BYTES;
        size_t body_size = size;
        if (name_length >= 3 && !memcmp(name, "#1/", 3)) {
            size_t embedded_length = 0;
            if (!decimal(name + 3, name_length - 3, &embedded_length)
                || !embedded_length || embedded_length > size) return false;
            name = body;
            name_length = embedded_length;
            while (name_length && name[name_length - 1] == '\0') name_length--;
            if (!name_length) return false;
            body += embedded_length;
            body_size -= embedded_length;
        } else if (name_length == 2 && !memcmp(name, "//", 2)) {
            name_table = body;
            name_table_size = size;
        } else if (name_length > 1 && name[0] == '/'
                   && name[1] >= '0' && name[1] <= '9') {
            size_t name_offset = 0;
            if (!decimal(name + 1, name_length - 1, &name_offset)
                || !long_name(name_table, name_table_size, name_offset,
                              &name, &name_length)) return false;
        } else if (name_length > 1 && name[name_length - 1] == '/'
                   && !(name_length == 7 && !memcmp(name, "/SYM64/", 7))) {
            name_length--;
        }
        core_native_archive_member_t member = {
            .name = name, .name_length = name_length,
            .bytes = body, .size = body_size,
            .archive_offset = offset,
        };
        if (!visitor(&member, user)) return false;
        size_t next = offset + AR_HEADER_BYTES + size;
        if (size & 1) {
            if (next == archive_bytes || data[next] != '\n') return false;
            next++;
        }
        offset = next;
    }
    return true;
}
